/*
 * hypr-osk — Hyprland plugin: touchscreen → pointer/keyboard for on-screen
 * keyboard use.
 *
 * Runs inside the compositor:
 *   - Single-finger touch: exact cursor placement under the finger
 *     (warpTo + client update — no acceleration, no dispatcher, no wiggle),
 *     press on contact, release on lift. Touches are consumed before they
 *     reach applications: no double input, no browser touch gestures.
 *   - Two-finger drag: scroll (natural direction, discrete wheel steps).
 *   - Quick two-finger tap: right click.
 *   - 3+ fingers: consumed for apps, left to hyprgrass (workspace swipes).
 *
 * IPC for the QML on-screen keyboard panel: unix socket
 * $XDG_RUNTIME_DIR/hypr-osk.sock, newline-terminated lines, replies
 * "ok" / "err <msg>" / "PONG":
 *   PING
 *   KEY <evdev-code> <1|0>       press/release a key (panel must be visible)
 *   MOD <shift|ctrl|alt|super> <on|off>   sticky modifier (panel must be
 *                                visible)
 *   MODS off                     release all sticky modifiers
 *   TEXT <string>                type UTF-8 text (chars resolved against the
 *                                ACTIVE xkb keymap of the synthetic device;
 *                                panel must be visible)
 *   LAYOUT <name>[(<variant>)]   switch the synthetic device's keymap (any
 *                                layout installed under /usr/share/X11/xkb),
 *                                replies "ok" after validation
 *   ROWS                         reply "grid <json>" — letter grid for the
 *                                main layer, dumped from the active keymap:
 *                                {"rows":[[{"l":"q","s":"Q","c":16},...],...]}
 *                                (raw:1 means send KEY <c> instead of TEXT)
 *   PMOVE <x> <y>                absolute cursor move (global logical px;
 *                                debug only, needs HYPR_OSK_ALLOW_ANY_PEER=1)
 *   PBTN <code> <1|0>            pointer button (0x110 = left; debug only,
 *                                needs HYPR_OSK_ALLOW_ANY_PEER=1)
 *
 * Access control: the socket can type into the focused session, so peers are
 * validated on accept with SO_PEERCRED — the uid must match and (unless
 * HYPR_OSK_ALLOW_ANY_PEER=1 in the compositor's environment) the peer exe
 * must be the shell (quickshell); everyone else gets "err unauthorized".
 * Injection commands (TEXT/KEY/MOD) are refused with "err hidden" while the
 * keyboard panel is not on screen (no PANEL rect published).
 *
 * Commands are queued from the socket thread and executed on the
 * compositor main thread via an EventLoop timer.
 */
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <set>
#include <hyprland/src/event/EventBus.hpp>

#include <cmath>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>
#include <poll.h>

using namespace Hyprutils::Math;

#define MAX_LINE 1024
#define TEXT_CAP 96 /* TEXT payload capacity in the ring (incl. NUL) */
static HANDLE PHANDLE = nullptr;
static int    debug   = 1;
#define DBG(...)                                                                                                                                                               \
    do {                                                                                                                                                                       \
        if (debug)                                                                                                                                                             \
            Log::logger->log(Log::DEBUG, std::string("[hypr-osk] ") + std::string(__VA_ARGS__));                                                                              \
    } while (0)

/* ---------------- socket <-> main thread queue ----------------
 * Fixed-size POD ring buffer: no heap allocations in transit, nothing to
 * corrupt under event-loop reentrancy (the crash mechanism). TEXT is
 * bounded to 95 bytes. */
struct SOskCommand {
    enum class EType : uint8_t { KEY, MOD, MODS, TEXT, LAYOUT, PMOVE, PBTN } type;
    int  a = 0, b = 0;
    char text[TEXT_CAP] = {0};
};
static constexpr size_t RING_SIZE = 64;
static SOskCommand      g_ring[RING_SIZE];
static std::mutex       g_ringMutex;
static size_t           g_ringHead = 0, g_ringCount = 0;
static bool             g_inDrain = false;
static std::atomic<bool> g_socketRunning{false};

static void queueCommand(SOskCommand cmd)
{
    std::lock_guard<std::mutex> lg(g_ringMutex);
    if (g_ringCount >= RING_SIZE) {
        DBG("ring full: command dropped");
        return;
    }
    g_ring[(g_ringHead + g_ringCount) % RING_SIZE] = cmd;
    g_ringCount++;
}

/* ---------------- keyboard state ---------------- */
static unsigned held_mods = 0; /* xkb depressed-mask of sticky modifiers */
static unsigned sent_mods = 0; /* what was last sent to the seat */
static const struct {
    const char *name;
    unsigned    evdev;
    unsigned    modbit;
} modnames[] = {
    {"shift", KEY_LEFTSHIFT, 1 /* WLR_MODIFIER_SHIFT */},
    {"ctrl",  KEY_LEFTCTRL,  4 /* WLR_MODIFIER_CTRL */},
    {"alt",   KEY_LEFTALT,   8 /* WLR_MODIFIER_ALT */},
    {"super", KEY_LEFTMETA,  64 /* WLR_MODIFIER_LOGO */},
};
#define NUM_MODNAMES (sizeof(modnames) / sizeof(modnames[0]))

/* ---------------- synthetic keyboard device ----------------
 * Keys injected through a registered IKeyboard device flow through the
 * compositor's REAL input pipeline: compositor keybinds (SUPER+SPACE etc.)
 * fire and modifier combos reach clients naturally. Seat-level
 * sendKeyboardKey/sendKeyboardMods bypass all of that. */
#include <hyprland/src/devices/IKeyboard.hpp>

class COskKeyboard : public IKeyboard {
  public:
    virtual bool                      isVirtual() override { return true; }
    virtual SP<Aquamarine::IKeyboard> aq() override { return nullptr; }
    virtual uint32_t                  getCapabilities() override { return HID_INPUT_CAPABILITY_KEYBOARD; }
    virtual eHIDType                  getType() override { return HID_TYPE_KEYBOARD; }
};

static SP<COskKeyboard>       g_oskKeyboard;
static std::set<unsigned int> g_pressedKeys; /* evdev codes currently held on the device */

static void execKey(unsigned evdev, int press);

/* ---------------- xkb-driven text mapping ----------------
 * TEXT chars are resolved against the device's ACTIVE keymap: utf32 → keysym
 * → (keycode, layout, level). Rebuilt whenever the keymap changes (init or
 * LAYOUT). AltGr chars (level ≥ 2) are typed via the Mod5 mask, which the
 * standard evdev maps tie to KEY_RIGHTALT. */
#include <unordered_map>

struct SKeyHit {
    xkb_keycode_t      key;
    xkb_layout_index_t layout;
    xkb_level_index_t  level;
};
static std::unordered_map<uint32_t, SKeyHit> g_textMap;

static std::string g_layoutSpec = "us"; /* what the QML last sent, for STATS */
static std::string g_gridJson;          /* cached ROWS payload, rebuilt with the map */
static std::mutex  g_gridMutex;

static size_t utf8Decode(const char *s, size_t max, uint32_t *out)
{
    if (max == 0)
        return 0;
    unsigned char c = s[0];
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    size_t len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else
        return 0;
    if (max < len)
        return 0;
    for (size_t i = 1; i < len; i++) {
        unsigned char cc = s[i];
        if ((cc & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    *out = cp;
    return len;
}

static size_t utf8Encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    out[0] = 0xF0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F);
    out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

static void rebuildTextMap()
{
    g_textMap.clear();
    if (!g_oskKeyboard || !g_oskKeyboard->m_xkbKeymap)
        return;
    xkb_keymap *km = g_oskKeyboard->m_xkbKeymap;
    xkb_layout_index_t layouts = xkb_keymap_num_layouts(km);
    for (xkb_keycode_t key = xkb_keymap_min_keycode(km); key <= xkb_keymap_max_keycode(km); key++) {
        for (xkb_layout_index_t layout = 0; layout < layouts; layout++) {
            xkb_level_index_t levels = xkb_keymap_num_levels_for_key(km, key, layout);
            if (levels > 8)
                levels = 8;
            for (xkb_level_index_t level = 0; level < levels; level++) {
                const xkb_keysym_t *syms = nullptr;
                int                 n    = xkb_keymap_key_get_syms_by_level(km, key, layout, level, &syms);
                if (n < 1 || !syms || !syms[0])
                    continue;
                /* execKey emits EVDEV keycodes; the keymap iterates XKB codes
                 * (evdev+8) — convert or every key lands 8 low (q types o) */
                if (key < 8)
                    continue;
                g_textMap.emplace(syms[0], SKeyHit{key - 8, layout, level});
            }
        }
    }
    DBG("text map rebuilt: " + std::to_string(g_textMap.size()) + " keysyms");
}

/* printable label for a keysym, for the ROWS grid; dead keys get their accent
 * glyph so the key is still identifiable */
static const struct {
    const char *suffix;
    const char *glyph;
} deadNames[] = {
    {"grave", "`"}, {"acute", "´"}, {"circumflex", "^"}, {"tilde", "~"},
    {"diaeresis", "¨"}, {"cedilla", "¸"}, {"caron", "ˇ"}, {"abovering", "˚"},
    {"macron", "¯"}, {"ogonek", "˛"}, {"breve", "˘"}, {"doubleacute", "˝"},
    {"horn", "̛"}, {"belowdot", "‧"}, {"small_high_dot", "·"}, {"i", "ı"},
};
#define DEAD_NAMES_LEN (sizeof(deadNames) / sizeof(deadNames[0]))

static std::string symLabel(xkb_keysym_t sym)
{
    std::string out;
    uint32_t    cp = xkb_keysym_to_utf32(sym);
    if (cp >= 0x20) {
        char  buf[5];
        size_t n = utf8Encode(cp, buf);
        out.append(buf, n);
        return out;
    }
    char name[64];
    if (xkb_keysym_get_name(sym, name, sizeof name) > 0 && !strncmp(name, "dead_", 5)) {
        for (size_t i = 0; i < DEAD_NAMES_LEN; i++) {
            if (!strcmp(name + 5, deadNames[i].suffix))
                return deadNames[i].glyph;
        }
    }
    return out; /* non-printable, no glyph */
}

static void jsonAppendEscaped(std::string &out, const std::string &s)
{
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
}

/* the main layer's letter grid, from physical evdev rows: keycode order IS the
 * physical arrangement (Q is 16, A is 30, Z is 44 for every latin layout) */
static void appendGridRow(std::string &out, const unsigned *codes, size_t len)
{
    xkb_keymap *km = g_oskKeyboard ? g_oskKeyboard->m_xkbKeymap : nullptr;
    if (!km)
        return;
    xkb_layout_index_t layout = 0;
    bool               first  = true;
    for (size_t i = 0; i < len && codes[i]; i++) {
        xkb_keycode_t key = codes[i] + 8; /* grid carries evdev codes; xkb is evdev+8 */
        xkb_level_index_t     nlev = xkb_keymap_num_levels_for_key(km, key, layout);
        const xkb_keysym_t   *syms = nullptr;
        std::string           l0, l1;
        bool                  raw  = false;
        if (nlev > 0 && xkb_keymap_key_get_syms_by_level(km, key, layout, 0, &syms) > 0 && syms[0]) {
            l0 = symLabel(syms[0]);
            if (l0.empty())
                raw = true; /* dead key or modifier: type the raw keycode */
        }
        if (nlev > 1 && xkb_keymap_key_get_syms_by_level(km, key, layout, 1, &syms) > 0 && syms[0])
            l1 = symLabel(syms[0]);
        if (l0.empty() && l1.empty() && !raw)
            continue;
        if (!first)
            out += ",";
        first = false;
        out += "{\"c\":" + std::to_string(key);
        if (!l0.empty()) {
            out += ",\"l\":\"";
            jsonAppendEscaped(out, l0);
            out += "\"";
        }
        if (!l1.empty()) {
            out += ",\"s\":\"";
            jsonAppendEscaped(out, l1);
            out += "\"";
        }
        if (raw)
            out += ",\"raw\":1";
        out += "}";
    }
}

static void rebuildGrid()
{
    static const unsigned rows[][14] = {
        {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 41, 0},          /* digits + - = ` */
        {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 43, 0},  /* q .. ] \ */
        {30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 0},          /* a .. ' */
        {44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 0},              /* z .. / */
    };
    std::string json = "{\"rows\":[";
    for (size_t r = 0; r < 4; r++) {
        if (r)
            json += ",";
        json += "[";
        appendGridRow(json, rows[r], 14);
        json += "]";
    }
    json += "]}";
    std::lock_guard<std::mutex> lg(g_gridMutex);
    g_gridJson = json;
}

/* current IPC client: the socket thread owns the connection lifecycle; the
 * main thread only sends unsolicited pushes (the fresh grid after a layout
 * switch, so the QML never has to guess when a queued LAYOUT has landed) */
static std::mutex g_clientMutex;
static int        g_clientFd = -1;

static void sendToClient(const std::string &line)
{
    std::lock_guard<std::mutex> lg(g_clientMutex);
    if (g_clientFd < 0)
        return;
    std::string out = line + "\n";
    ssize_t     r;
    do {
        r = send(g_clientFd, out.c_str(), out.size(), MSG_NOSIGNAL);
    } while (r < 0 && errno == EINTR);
}

static void pushGrid()
{
    std::string json;
    {
        std::lock_guard<std::mutex> lg(g_gridMutex);
        json = g_gridJson.empty() ? std::string("{\"rows\":[[],[],[],[]]}") : g_gridJson;
    }
    sendToClient("grid " + json);
}

/* ---------------- event bus hooks ---------------- */
static Hyprutils::Signal::CHyprSignalListener g_touchDownHook;
static Hyprutils::Signal::CHyprSignalListener g_touchUpHook;
static Hyprutils::Signal::CHyprSignalListener g_touchMoveHook;
static std::thread                            g_socketThread;

/* ---------------- main-thread executors ---------------- */
static uint32_t nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void sendMods(unsigned mask)
{
    /* modifier state via real modifier KEY presses on the device (delta
     * against what's currently held): xkb masks shift=1 ctrl=4 alt=8
     * super(M4)=64, altgr(M5)=128 (KEY_RIGHTALT is ISO_Level3_Shift on the
     * standard evdev maps) */
    static const struct {
        unsigned bit;
        unsigned evdev;
    } modkeys[] = {
        {1,   KEY_LEFTSHIFT },
        {4,   KEY_LEFTCTRL  },
        {8,   KEY_LEFTALT   },
        {64,  KEY_LEFTMETA  },
        {128, KEY_RIGHTALT  },
    };
    for (auto const &m : modkeys) {
        bool want = mask & m.bit;
        bool held = g_pressedKeys.count(m.evdev) > 0;
        if (want && !held)
            execKey(m.evdev, 1);
        else if (!want && held)
            execKey(m.evdev, 0);
    }
    sent_mods = mask;
}

static void execKey(unsigned evdev, int press)
{
    if (!g_oskKeyboard)
        return;
    IKeyboard::SKeyEvent e;
    e.timeMs     = nowMs();
    e.keycode    = evdev;
    e.updateMods = true; /* compositor updates its xkb state: modifiers derive from held keys */
    e.state      = press ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    g_oskKeyboard->m_keyboardEvents.key.emit(e);
    /* maintain the device's xkb state: this is how the compositor (and every
     * client) learns which modifiers are held — nothing else consumes it */
    g_oskKeyboard->updateXkbStateWithKey(evdev + 8, press);
    if (press)
        g_pressedKeys.insert(evdev);
    else
        g_pressedKeys.erase(evdev);
}

static void execText(const std::string &text)
{
    if (!g_oskKeyboard || g_textMap.empty())
        return;
    bool altgr_was_held = false;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp;
        size_t   len = utf8Decode(text.c_str() + i, text.size() - i, &cp);
        if (!len) {
            i++;
            continue;
        }
        i += len;
        if (cp == '\n') {
            execKey(KEY_ENTER, 1);
            execKey(KEY_ENTER, 0);
            continue;
        }
        if (cp == '\t') {
            execKey(KEY_TAB, 1);
            execKey(KEY_TAB, 0);
            continue;
        }
        uint32_t   sym = xkb_utf32_to_keysym(cp);
        const auto it  = sym ? g_textMap.find(sym) : g_textMap.end();
        if (sym == 0 || it == g_textMap.end()) {
            DBG("TEXT: no keysym/keycode for U+" + std::to_string(cp));
            continue;
        }
        /* per-char mods: shift for level 1, AltGr (Mod5) for level ≥ 2, taken
         * from the keymap so exotic levels stay honest */
        unsigned want = held_mods & ~(1u | 128u);
        xkb_keymap *km = g_oskKeyboard->m_xkbKeymap;
        xkb_mod_mask_t maskBuf[8] = {0};
        size_t nmasks = 0;
        if (km && it->second.level > 0) {
            nmasks = xkb_keymap_key_get_mods_for_level(km, it->second.key, it->second.layout,
                                                       it->second.level, maskBuf, 8);
        }
        if (nmasks > 0) {
            if (maskBuf[0] & 1u)
                want |= 1u;
            if (maskBuf[0] & 128u) {
                want |= 128u;
                altgr_was_held = true;
            }
        }
        sendMods(want);
        execKey(it->second.key, 1);
        execKey(it->second.key, 0);
        if (altgr_was_held) {
            /* AltGr is never sticky: release it immediately so the next char
             * doesn't inherit it */
            execKey(KEY_RIGHTALT, 0);
            altgr_was_held = false;
        }
    }
    sendMods(held_mods);
}

static void execLayout(const std::string &spec)
{
    if (!g_oskKeyboard)
        return;
    /* "name" or "name(variant)"; validated by the socket thread before
     * queueing, so a bad name never reaches the compositor's fallback path */
    std::string layout = spec, variant;
    size_t      paren  = spec.find('(');
    if (paren != std::string::npos && spec.back() == ')') {
        layout  = spec.substr(0, paren);
        variant = spec.substr(paren + 1, spec.size() - paren - 2);
    }
    /* release everything held: the fresh xkb state knows nothing about them */
    for (auto it = g_pressedKeys.begin(); it != g_pressedKeys.end();)
        execKey(*it++, 0);
    IKeyboard::SStringRuleNames rules;
    rules.layout  = layout;
    rules.variant = variant;
    rules.model   = "";
    rules.options = "";
    rules.rules   = "";
    g_oskKeyboard->setKeymap(rules);
    g_layoutSpec = spec;
    rebuildTextMap();
    rebuildGrid();
    pushGrid();
    DBG("layout applied: " + spec);
}

static void execPmove(int x, int y)
{
    Pointer::pointerController()->warpTo(Vector2D{(double)x, (double)y}, true);
    g_pInputManager->simulateMouseMovement();
}

static void execPbtn(unsigned code, int press)
{
    g_pSeatManager->sendPointerButton(nowMs(), code,
                                      press ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    g_pSeatManager->sendPointerFrame();
}

/* ---------------- touch state ----------------
 * Handlers only RECORD state and schedule the deferred apply timer; every compositor
 * mutation (warp, buttons, scroll, keyboard) happens in applyTouches() on
 * the idle phase of the main thread — calling input/monitor code from
 * inside the touch callback deadlocks the input pipeline. */
static int      fingers = 0;
static bool     ignore_until_zero = false;      /* gesture ended; wait for all fingers up */
static bool     scroll_mode = false;
static double   scroll_anchor_y = 0;            /* normalized y anchor */
static double   scroll_travel_px = 0;           /* px moved this gesture (tap-vs-scroll heuristic) */
static bool     scroll_wheel_shape = false;     /* per-gesture event shape, chosen by focused window class:
                                                 * Chromium rescales legacy axis values by 1/10 * 120 (wheel
                                                 * ticks) and only honors v120 when Hyprland sends it, which
                                                 * 0.56 gates to wheel source — so chrom*-class windows get
                                                 * wheel+v120; kitty & co read plain axis as continuous px
                                                 * (their v120 would be wheel notches) and get FINGER px. */
static uint32_t dual_start_ms = 0;
static Vector2D lastPos;                        /* normalized (0..1) */
static bool     pressed = false;                /* left button held */
static bool     panel_pressed = false;          /* synthetic click on the OSK panel */
static unsigned g_drain_fires = 0;               /* drain timer fire count */
static bool     down_flag = false, up_flag = false, motion_flag = false;
static std::string touchDeviceOutput = "eDP-1";
static bool     apply_pending = false;

/* OSK panel exemption: touches inside this rect (normalized 0..1 on the
 * touch device's frame) pass through to the panel's own Qt touch handling;
 * everything else is consumed and emulated. Announced by the QML client
 * via PANELNORM. */
static double panel_nx = 0, panel_ny = 0, panel_nw = 0, panel_nh = 0;
static bool   panel_rect_valid = false;
static bool   contact_is_panel_native = false;

static bool posInPanel(double nx, double ny)
{
    return panel_rect_valid && nx >= panel_nx && nx < panel_nx + panel_nw && ny >= panel_ny &&
           ny < panel_ny + panel_nh;
}

static void applyTouches(); /* deferred: all compositor mutations happen here */

/* plugin-owned one-shot timer: unlike doLater (whose queued lambdas fire even
 * after dlclose — that crashed the compositor on unload), this stays
 * cancellable and is torn down in PLUGIN_EXIT before the .so is unmapped */
static SP<CEventLoopTimer> g_applyTimer;

static void scheduleApply()
{
    if (apply_pending)
        return;
    apply_pending = true;
    if (!g_applyTimer) {
        g_applyTimer = makeShared<CEventLoopTimer>(
            std::chrono::milliseconds(0),
            [](SP<CEventLoopTimer> self, void*) {
                apply_pending = false;
                applyTouches();
            },
            nullptr);
        g_pEventLoopManager->addTimer(g_applyTimer);
    } else
        g_applyTimer->updateTimeout(std::chrono::milliseconds(0));
}

/* handlers: record state + schedule only — no compositor calls (calling
 * input/monitor code from inside the touch callback deadlocks the pipeline) */
static void touchDown(ITouch::SDownEvent ev, Event::SCallbackInfo &info)
{
    fingers++;
    lastPos            = ev.pos;
    if (fingers == 1)
        contact_is_panel_native = posInPanel(ev.pos.x, ev.pos.y); /* primary contact decides the mode */
    down_flag = true; /* EVERY down must apply: the resolver needs to see finger #2 to enter scroll */
    info.cancelled = true; /* consumed: Hyprland's touch refocus would steal keyboard focus */
    scheduleApply();
}

static void touchUp(ITouch::SUpEvent ev, Event::SCallbackInfo &info)
{
    if (fingers > 0)
        fingers--;
    up_flag = true;
    info.cancelled = true;
    scheduleApply();
}

static void touchMotion(ITouch::SMotionEvent ev, Event::SCallbackInfo &info)
{
    lastPos       = ev.pos;
    motion_flag   = true;
    info.cancelled = true;
    scheduleApply();
}

/* ---------------- deferred application (idle phase, main thread) ---------- */
static void applyTouches()
{
    if (ignore_until_zero) {
        up_flag = false;
        down_flag = false;
        if (fingers == 0)
            ignore_until_zero = false;
        return;
    }

    if (up_flag) {
        up_flag = false;
        if (contact_is_panel_native) {
            /* panel contact ended: release the synthetic click */
            if (panel_pressed) {
                g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                g_pSeatManager->sendPointerFrame();
                panel_pressed = false;
            }
            if (fingers == 0)
                contact_is_panel_native = false;
            return;
        }
        if (scroll_mode && fingers < 2) {
            /* finger lifted mid-scroll: gesture over */
            scroll_mode = false;
            /* two-finger tap without scrolling = right click */
            if (std::abs(scroll_travel_px) < 10 && nowMs() - dual_start_ms <= 250) {
                g_pSeatManager->sendPointerButton(nowMs(), BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
                g_pSeatManager->sendPointerFrame();
                g_pSeatManager->sendPointerButton(nowMs(), BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
                g_pSeatManager->sendPointerFrame();
                DBG("two-finger tap: right click");
            }
        }
        if (fingers == 0 && pressed) {
            g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
            g_pSeatManager->sendPointerFrame();
            pressed = false;
        }
        if (fingers == 1 && pressed) {
            /* can't tell which finger lifted: end the drag to be safe */
            g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
            g_pSeatManager->sendPointerFrame();
            pressed           = false;
            ignore_until_zero = true;
        }
        if (fingers == 0)
            contact_is_panel_native = false;
    }

    if (down_flag) {
        /* mode resolver: settles the gesture state from (fingers, pressed).
         * Runs for EVERY contact down — the second finger's down is what
         * enters scroll mode; batched landings (both fingers before the
         * apply timer) resolve here in one pass. */
        down_flag = false;
        auto mon = State::monitorState()
                       ->query()
                       .name(!touchDeviceOutput.empty() ? touchDeviceOutput : "")
                       .run();
        if (!mon)
            mon = Desktop::focusState()->monitor();

        if (contact_is_panel_native) {
            /* panel contact: synthetic click on the keyboard layer — pointer
             * focus delivers the tap, keyboard focus never moves (the layer
             * is keyboard-focus-none). Extra fingers on the panel: ignored. */
            if (!panel_pressed && fingers == 1) {
                Vector2D global = mon->m_position + (lastPos * mon->m_size);
                Pointer::pointerController()->warpTo(global, true);
                g_pInputManager->simulateMouseMovement();
                g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
                g_pSeatManager->sendPointerFrame();
                panel_pressed = true;
                DBG("panel contact: synthetic pointer press");
            }
            scroll_mode  = false;
            scroll_travel_px = 0;
        } else if (fingers >= 3) {
            /* 3+ fingers: hyprgrass territory */
            if (pressed) {
                g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                g_pSeatManager->sendPointerFrame();
                pressed = false;
            }
            scroll_mode = false;
        } else if (fingers == 2) {
            /* two fingers: end any drag, enter two-finger scroll */
            if (pressed) {
                g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                g_pSeatManager->sendPointerFrame();
                pressed = false;
            }
            if (!scroll_mode) {
                scroll_mode     = true;
                scroll_anchor_y = lastPos.y;
                scroll_travel_px = 0;
                dual_start_ms   = nowMs();
                scroll_wheel_shape = false;
                if (auto w = Desktop::focusState()->window()) {
                    std::string cls = w->m_class;
                    for (char &c : cls)
                        c = c >= 'A' && c <= 'Z' ? c + 32 : c;
                    if (cls.contains("chrom"))
                        scroll_wheel_shape = true;
                }
                DBG("two fingers: scroll mode");
            }
        } else if (!pressed) {
            /* primary contact: cursor under the finger, button pressed */
            Vector2D global = mon->m_position + (lastPos * mon->m_size);
            Pointer::pointerController()->warpTo(global, true);
            g_pInputManager->simulateMouseMovement();
            g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
            g_pSeatManager->sendPointerFrame();
            pressed      = true;
            scroll_mode  = false;
            scroll_travel_px = 0;
            DBG("contact: pointer under finger, pressed");
        }
    }

    if (fingers == 1 && pressed) {
        /* motion: cursor follows the finger exactly */
        auto mon = State::monitorState()
                       ->query()
                       .name(!touchDeviceOutput.empty() ? touchDeviceOutput : "")
                       .run();
        if (!mon)
            mon = Desktop::focusState()->monitor();
        Vector2D global = mon->m_position + (lastPos * mon->m_size);
        Pointer::pointerController()->warpTo(global, true);
        g_pInputManager->simulateMouseMovement();
    } else if (fingers == 1 && panel_pressed) {
        /* drag on the panel: cursor follows, pointer focus stays on the layer */
        auto mon = State::monitorState()
                       ->query()
                       .name(!touchDeviceOutput.empty() ? touchDeviceOutput : "")
                       .run();
        if (!mon)
            mon = Desktop::focusState()->monitor();
        Vector2D global = mon->m_position + (lastPos * mon->m_size);
        Pointer::pointerController()->warpTo(global, true);
        g_pInputManager->simulateMouseMovement();
    } else if (scroll_mode && fingers == 2) {
        /* touchpad-style scroll: pixel deltas 1:1 with the finger, so content
         * tracks the hand exactly. Wheel-source events (the old 0.025-units-
         * per-notch scheme) get client-side acceleration/smoothing and
         * outrun the fingers. */
        auto mon = State::monitorState()
                       ->query()
                       .name(!touchDeviceOutput.empty() ? touchDeviceOutput : "")
                       .run();
        if (!mon)
            mon = Desktop::focusState()->monitor();
        double px = (lastPos.y - scroll_anchor_y) * mon->m_size.y; /* logical px */
        scroll_anchor_y = lastPos.y; /* consume the full delta: no drift */
        if (px != 0.0) {
            scroll_travel_px += std::abs(px);
            /* natural scrolling: content follows the fingers */
            if (scroll_wheel_shape) {
                /* Chromium: v120 rides as exact pixels (its v8 handler wins
                 * over the x12 legacy rescale within the frame) */
                g_pSeatManager->sendPointerAxis(nowMs(), WL_POINTER_AXIS_VERTICAL_SCROLL, -px, 0,
                                                (int32_t)std::lround(-px), WL_POINTER_AXIS_SOURCE_WHEEL,
                                                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            } else {
                /* kitty & co: plain axis is the continuous pixel bucket */
                g_pSeatManager->sendPointerAxis(nowMs(), WL_POINTER_AXIS_VERTICAL_SCROLL, -px, 0, 0,
                                                WL_POINTER_AXIS_SOURCE_FINGER,
                                                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            }
            g_pSeatManager->sendPointerFrame();
        }
    }
}

#if 0
static void touchDown(ITouch::SDownEvent ev, Event::SCallbackInfo &info)
{
    fingers++;

    /* resolve the global position (the event carries normalized coords) */
    auto mon = State::monitorState()
                   ->query()
                   .name(!ev.device->m_boundOutput.empty() ? ev.device->m_boundOutput : "")
                   .run();
    if (!mon)
        mon = Desktop::focusState()->monitor();
    g_touchMonitor = mon;
    Vector2D global = mon->m_position + (ev.pos * mon->m_size);

    /* OSK surface passthrough: wvkbd bridge + QML panel rect. Touches there
     * belong to the keyboard's own Qt/client handling. */
    int wx, wy, ww, wh;
    bool onOskSurface = (wvkbd_rect(&wx, &wy, &ww, &wh) && global.x >= wx && global.x < wx + ww &&
                         global.y >= wy && global.y < wy + wh) ||
                        posInPanel((int)global.x, (int)global.y);
    contact_is_panel_native = onOskSurface;
    info.cancelled          = onOskSurface ? false : true;
    if (onOskSurface) {
        DBG("touch on OSK surface: passed through");
        return;
    }

    if (ignore_until_zero)
        return;

    if (fingers == 1) {
        /* absolute: cursor under the finger, button pressed */
        Pointer::pointerController()->warpTo(global, true);
        g_pInputManager->simulateMouseMovement();
        g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
        g_pSeatManager->sendPointerFrame();
        pressed     = true;
        scroll_mode = false;
        scroll_steps = 0;
        DBG("touch down: pointer under finger, pressed");
    } else if (fingers == 2) {
        /* second finger: end drag, enter two-finger scroll */
        if (pressed) {
            g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
            g_pSeatManager->sendPointerFrame();
            pressed = false;
        }
        scroll_mode       = true;
        scroll_anchor_y   = ev.pos.y;
        scroll_steps      = 0;
        dual_start_ms     = nowMs();
        last_motion_y     = ev.pos.y;
        last_motion_valid = true;
        DBG("two fingers: scroll mode");
    } else {
        /* 3+ fingers: hyprgrass territory */
        scroll_mode = false;
        DBG("3+ fingers: handed to hyprgrass");
    }
}

static void touchUp(ITouch::SUpEvent ev, Event::SCallbackInfo &info)
{
    info.cancelled = contact_is_panel_native ? false : true;
    if (fingers > 0)
        fingers--;
    DBG("touch up, fingers left: " + std::to_string(fingers));

    if (fingers == 0) {
        ignore_until_zero = false;
        contact_is_panel_native = false;
        g_touchMonitor    = nullptr;
        if (pressed) {
            g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
            g_pSeatManager->sendPointerFrame();
            pressed = false;
        }
        scroll_mode = false;
        return;
    }

    if (scroll_mode && fingers < 2) {
        /* finger lifted mid-scroll: gesture over */
        scroll_mode       = false;
        ignore_until_zero = true;
        /* two-finger tap without scrolling = right click */
        if (scroll_steps == 0 && nowMs() - dual_start_ms <= 250) {
            g_pSeatManager->sendPointerButton(nowMs(), BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
            g_pSeatManager->sendPointerFrame();
            g_pSeatManager->sendPointerButton(nowMs(), BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
            g_pSeatManager->sendPointerFrame();
            DBG("two-finger tap: right click");
        }
        return;
    }

    if (fingers == 1 && pressed) {
        /* can't tell which finger lifted: end the drag to be safe */
        g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
        g_pSeatManager->sendPointerFrame();
        pressed           = false;
        ignore_until_zero = true;
    }
}

static void touchMotion(ITouch::SMotionEvent ev, Event::SCallbackInfo &info)
{
    info.cancelled = contact_is_panel_native ? false : true;
    if (contact_is_panel_native)
        return;
    if (ignore_until_zero) {
        last_motion_y     = ev.pos.y;
        last_motion_valid = true;
        return;
    }

    if (fingers == 1 && pressed) {
        /* absolute: cursor follows the finger exactly. The monitor is
         * re-resolved per event: with no monitor config the layout is
         * dynamic (screens are placed wherever Hyprland puts them). */
        auto mon = g_touchMonitor;
        if (!mon)
            mon = Desktop::focusState()->monitor();
        g_touchMonitor = mon;
        Vector2D global = mon->m_position + (ev.pos * mon->m_size);
        Pointer::pointerController()->warpTo(global, true);
        g_pInputManager->simulateMouseMovement();
    } else if (scroll_mode && fingers == 2) {
        /* scroll from the vertical delta (whichever finger moved);
         * ev.pos is normalized — 0.025 ≈ 20 logical px on an 800-tall screen */
        double dy = last_motion_valid ? (ev.pos.y - last_motion_y) : 0.0;
        last_motion_y     = ev.pos.y;
        last_motion_valid = true;
        int steps         = (int)(dy / 0.025);
        if (steps == 0)
            return;
        scroll_anchor_y += steps * 0.025;
        scroll_steps += abs(steps);
        /* natural scrolling: content follows the fingers */
        g_pSeatManager->sendPointerAxis(nowMs(), WL_POINTER_AXIS_VERTICAL_SCROLL, steps * -10.0, -steps,
                                        steps * -120, WL_POINTER_AXIS_SOURCE_WHEEL,
                                        WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        g_pSeatManager->sendPointerFrame();
        DBG("scroll " + std::to_string(steps) + " steps");
    }
}

#endif

/* ---------------- socket protocol ---------------- */
static void send_reply(int cfd, const char *msg)
{
    std::string out = std::string(msg) + "\n";
    ssize_t     r;
    do {
        r = send(cfd, out.c_str(), out.size(), MSG_NOSIGNAL);
    } while (r < 0 && errno == EINTR);
}

/* ---------------- peer validation ----------------
 * The socket can type into the focused session, so only the user's own shell
 * (quickshell — omarchy-shell is a wrapper script that execs it) may connect.
 * SO_PEERCRED yields the peer's pid/uid from the kernel; /proc/<pid>/exe
 * identifies the binary. HYPR_OSK_ALLOW_ANY_PEER=1 in the compositor's
 * environment skips the exe check (never the uid check) — it is also the
 * gate for the PMOVE/PBTN debug commands. */
static bool g_allowAnyPeer = false;

static bool peerAllowed(int cfd)
{
    struct ucred cred = {};
    socklen_t clen = sizeof cred;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0) {
        DBG("peer check: getsockopt failed");
        return false;
    }
    if (cred.uid != geteuid()) {
        DBG("peer check: uid " + std::to_string(cred.uid) + " rejected");
        return false;
    }
    if (g_allowAnyPeer)
        return true;
    char buf[4096];
    std::string link = "/proc/" + std::to_string(cred.pid) + "/exe";
    ssize_t n = readlink(link.c_str(), buf, sizeof buf - 1);
    if (n <= 0) {
        DBG("peer check: pid " + std::to_string(cred.pid) + " exe unreadable, rejected");
        return false;
    }
    buf[n] = 0;
    const char *slash = strrchr(buf, '/');
    const char *base  = slash ? slash + 1 : buf;
    if (strcmp(base, "quickshell") != 0) {
        DBG("peer check: pid " + std::to_string(cred.pid) + " exe=" + std::string(base) +
            ", rejected");
        return false;
    }
    return true;
}

/* ---------------- safe parsing helpers ---------------- */
static int parse_uint(const char *s, unsigned long max, unsigned *out)
{
    char *end = NULL;
    if (!s || !*s)
        return 0;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    while (end && (*end == ' ' || *end == '\n' || *end == '\t' || *end == '\r' || *end == ',' || *end == '}' || *end == '"'))
        end++;
    if (errno || end == s || *end != '\0' || v > max)
        return 0;
    *out = (unsigned)v;
    return 1;
}

static int parse_int(const char *s, long min, long max, int *out)
{
    char *end = NULL;
    if (!s || !*s)
        return 0;
    errno = 0;
    long v = strtol(s, &end, 10);
    while (end && (*end == ' ' || *end == '\n' || *end == '\t' || *end == '\r' || *end == ',' || *end == '}' || *end == '"'))
        end++;
    if (errno || end == s || *end != '\0' || v < min || v > max)
        return 0;
    *out = (int)v;
    return 1;
}

static void handle_line(int cfd, char *line)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = 0;
    DBG("cmd: " + std::string(line));

    const char *reply = "err unknown command";
    SOskCommand cmd;

    if (strcmp(line, "PING") == 0) {
        reply = "PONG";
    } else if (!strncmp(line, "KEY ", 4)) {
        if (!panel_rect_valid) { /* input injection only while the panel is on screen */
            reply = "err hidden";
        } else {
            cmd.type = SOskCommand::EType::KEY;
            if (sscanf(line + 4, "%u %d", &cmd.a, &cmd.b) == 2) {
                queueCommand(std::move(cmd));
                reply = "ok";
            } else
                reply = "err bad args";
        }
    } else if (!strncmp(line, "MOD ", 4)) {
        char name[16], state[8];
        if (!panel_rect_valid) { /* input injection only while the panel is on screen */
            reply = "err hidden";
        } else if (sscanf(line + 4, "%15s %7s", name, state) != 2) {
            reply = "err bad args";
        } else {
            int found = -1;
            for (size_t i = 0; i < NUM_MODNAMES; i++) {
                if (strcmp(name, modnames[i].name) == 0)
                    found = (int)i;
            }
            if (found < 0 || (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
                reply = "err bad args";
            } else {
                cmd.type = SOskCommand::EType::MOD;
                cmd.a    = found;
                cmd.b    = (strcmp(state, "on") == 0) ? 1 : 0;
                queueCommand(std::move(cmd));
                reply = "ok";
            }
        }
    } else if (!strncmp(line, "MODS ", 5)) {
        if (strcmp(line + 5, "off") != 0) {
            reply = "err bad args";
        } else {
            cmd.type = SOskCommand::EType::MODS;
            queueCommand(std::move(cmd));
            reply = "ok";
        }
    } else if (!strncmp(line, "LAYOUT ", 7)) {
        /* "name" or "name(variant)" — validate synchronously: compile the
         * keymap with xkbcommon BEFORE queueing, so a bad name replies "err"
         * instead of tripping the compositor's fallback-to-us error overlay */
        const char *spec = line + 7;
        std::string layout(spec), variant;
        bool        ok = false;
        size_t      n  = strlen(spec);
        if (n > 0 && n < 64) {
            if (const char *paren = strchr(spec, '('); paren && spec[n - 1] == ')' && paren < spec + n - 1) {
                layout  = std::string(spec, paren - spec);
                variant = std::string(paren + 1, spec + n - 1 - paren - 1);
            }
            bool valid = !layout.empty() && variant.size() < 48 && layout.size() < 48;
            if (valid) {
                for (char c : layout + variant) {
                    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid) {
                xkb_rule_names names = {.rules   = "",
                                        .model   = "",
                                        .layout  = layout.c_str(),
                                        .variant = variant.c_str(),
                                        .options = ""};
                if (xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
                    if (xkb_keymap *km = xkb_keymap_new_from_names2(ctx, &names, XKB_KEYMAP_FORMAT_TEXT_V2,
                                                                    XKB_KEYMAP_COMPILE_NO_FLAGS)) {
                        xkb_keymap_unref(km);
                        ok = true;
                    }
                    xkb_context_unref(ctx);
                }
            }
        }
        if (!ok) {
            reply = "err bad layout";
        } else {
            cmd.type = SOskCommand::EType::LAYOUT;
            snprintf(cmd.text, TEXT_CAP, "%s", spec);
            queueCommand(std::move(cmd));
            reply = "ok";
        }
    } else if (!strcmp(line, "ROWS")) {
        /* cached grid, rebuilt on the main thread with every keymap change;
         * no keymap access from this thread */
        std::lock_guard<std::mutex> lg(g_gridMutex);
        static std::string          gridReply;
        gridReply = "grid " + (g_gridJson.empty() ? std::string("{\"rows\":[[],[],[],[]]}") : g_gridJson);
        reply     = gridReply.c_str();
    } else if (!strncmp(line, "TEXT ", 5)) {
        if (!panel_rect_valid) { /* input injection only while the panel is on screen */
            reply = "err hidden";
        } else {
            cmd.type = SOskCommand::EType::TEXT;
            size_t n = strlen(line + 5);
            if (n > TEXT_CAP - 1) /* keep room for the NUL terminator */
                n = TEXT_CAP - 1;
            memcpy(cmd.text, line + 5, n);
            cmd.text[n] = 0;
            queueCommand(std::move(cmd));
            reply = "ok";
        }
    } else if (!strncmp(line, "PMOVE ", 6)) {
        if (!g_allowAnyPeer) { /* debug-only remote pointer control */
            reply = "err pointer disabled";
        } else {
            cmd.type = SOskCommand::EType::PMOVE;
            if (sscanf(line + 6, "%d %d", &cmd.a, &cmd.b) == 2) {
                queueCommand(std::move(cmd));
                reply = "ok";
            } else
                reply = "err bad args";
        }
    } else if (!strncmp(line, "PBTN ", 5)) {
        if (!g_allowAnyPeer) { /* debug-only remote pointer control */
            reply = "err pointer disabled";
        } else {
            cmd.type = SOskCommand::EType::PBTN;
            if (sscanf(line + 5, "%i %d", &cmd.a, &cmd.b) == 2) { /* %i: accepts 0x hex */
                queueCommand(std::move(cmd));
                reply = "ok";
            } else
                reply = "err bad args";
        }
    } else if (!strncmp(line, "MON", 3)) {
        /* MON — reply with the touch monitor's logical frame (x y w h), the
         * exact frame touch ev.pos is normalized against (m_position/m_size) */
        auto mon = State::monitorState()
                       ->query()
                       .name(!touchDeviceOutput.empty() ? touchDeviceOutput : "")
                       .run();
        if (!mon)
            mon = Desktop::focusState()->monitor();
        if (mon) {
            char buf[128];
            snprintf(buf, sizeof buf, "mon %d %d %d %d", (int)mon->m_position.x, (int)mon->m_position.y,
                     (int)mon->m_size.x, (int)mon->m_size.y);
            reply = buf;
        } else
            reply = "err no monitor";
    } else if (!strncmp(line, "STATS", 5)) {
        /* live touch state for debugging */
        char buf[320];
        snprintf(buf, sizeof buf,
                 "state fingers=%d pressed=%d ignore=%d scroll=%d down=%d up=%d contact_osk=%d "
                 "panel_valid=%d inject=%d anypeer=%d panel_ny=%.3f panel_nh=%.3f last=%.3f,%.3f "
                 "fires=%u ring=%zu indrain=%d layout=%s textmap=%zu",
                 fingers, (int)pressed, (int)ignore_until_zero, (int)scroll_mode, (int)down_flag,
                 (int)up_flag, (int)contact_is_panel_native, (int)panel_rect_valid,
                 (int)panel_rect_valid, (int)g_allowAnyPeer, panel_ny,
                 panel_nh, lastPos.x, lastPos.y, g_drain_fires, g_ringCount, (int)g_inDrain,
                 g_layoutSpec.c_str(), g_textMap.size());
        reply = buf;
    } else if (!strncmp(line, "CALIB ", 6)) {
        reply = "ok"; /* accepted for protocol compatibility; the frame comes from the compositor */
    } else if (!strncmp(line, "PANEL ", 6)) {
        /* PANEL x y w h — QML panel rect in normalized (0..1) coords, matching
         * the touch event frame (panel_nx/ny/nw/nh); 0 0 0 0 clears it */
        char *a = line + 6;
        char *b = strchr(a, ' ');
        char *c = b ? strchr(b + 1, ' ') : NULL;
        char *d = c ? strchr(c + 1, ' ') : NULL;
        double px, py, pw, ph;
        if (b && c && d) {
            *b = 0; *c = 0; *d = 0;
            char *endp;
            px = strtod(a, &endp);   bool ok1 = endp != a;
            py = strtod(b + 1, &endp); bool ok2 = endp != b + 1;
            pw = strtod(c + 1, &endp); bool ok3 = endp != c + 1;
            ph = strtod(d + 1, &endp); bool ok4 = endp != d + 1;
            if (ok1 && ok2 && ok3 && ok4) {
                panel_nx = px; panel_ny = py; panel_nw = pw; panel_nh = ph;
                panel_rect_valid = (panel_nw > 0 && panel_nh > 0);
                DBG("panel rect (norm): " + std::string(line + 6));
                reply = "ok";
            } else
                reply = "err bad args";
        } else {
            panel_rect_valid = false; /* PANEL 0 0 0 0 = clear */
            reply = "ok";
        }
    }
    send_reply(cfd, reply);
}

/* ---------------- socket thread ---------------- */
static void socket_thread_fn(std::string path)
{
    unlink(path.c_str());
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        Log::logger->log(Log::ERR, "[hypr-osk] socket() failed");
        g_socketRunning = false;
        return;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family         = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(sock_fd, 4) < 0) {
        Log::logger->log(Log::ERR, "[hypr-osk] bind/listen failed");
        g_socketRunning = false;
        return;
    }
    chmod(path.c_str(), 0600);
    DBG("listening on " + path);

    int  cfd  = -1;
    char rbuf[MAX_LINE];
    size_t rlen = 0;
    while (g_socketRunning) {
        struct pollfd pfd = {.fd = sock_fd, .events = POLLIN};
        int r = poll(&pfd, 1, 200);
        if (r <= 0 || !g_socketRunning)
            continue;
        int nfd = accept(sock_fd, NULL, NULL);
        if (nfd < 0)
            continue;
        if (!peerAllowed(nfd)) {
            send_reply(nfd, "err unauthorized");
            close(nfd);
            continue;
        }
        if (cfd >= 0) {
            send_reply(cfd, "err replaced");
            close(cfd);
        }
        cfd  = nfd;
        rlen = 0;
        {
            std::lock_guard<std::mutex> lg(g_clientMutex);
            g_clientFd = nfd;
        }
        DBG("client connected");
        pushGrid(); /* proactively hand the fresh grid to the new client */

        struct pollfd cfds[2] = {{.fd = sock_fd, .events = POLLIN}, {.fd = cfd, .events = POLLIN}};
        while (g_socketRunning) {
            int r2 = poll(cfds, 2, 200);
            if (r2 <= 0 || !g_socketRunning)
                continue;
            if (cfds[0].revents & POLLIN) {
                int newer = accept(sock_fd, NULL, NULL);
                if (newer >= 0) {
                    if (!peerAllowed(newer)) {
                        send_reply(newer, "err unauthorized");
                        close(newer);
                    } else {
                        send_reply(cfd, "err replaced");
                        close(cfd);
                        cfd  = newer;
                        rlen = 0;
                        {
                            std::lock_guard<std::mutex> lg(g_clientMutex);
                            g_clientFd = newer;
                        }
                    }
                }
            }
            if (cfds[1].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(cfd, rbuf + rlen, MAX_LINE - 1 - rlen);
                if (n <= 0) {
                    DBG("client gone");
                    break;
                }                rlen += (size_t)n;
                rbuf[rlen] = 0;
                char *nl;
                while ((nl = strchr(rbuf, '\n'))) {
                    *nl = 0;
                    handle_line(cfd, rbuf);
                    size_t consumed = (size_t)(nl - rbuf) + 1;
                    memmove(rbuf, rbuf + consumed, rlen - consumed + 1);
                    rlen -= consumed;
                }
                if (rlen >= MAX_LINE - 1) {
                    send_reply(cfd, "err line too long");
                    DBG("client dropped (overlong line)");
                    break;
                }
            }
        }
        close(cfd);
        cfd  = -1;
        rlen = 0;
        {
            std::lock_guard<std::mutex> lg(g_clientMutex);
            g_clientFd = -1;
        }
    }
    close(sock_fd);
    unlink(path.c_str());
}

/* ---------------- queue drain (main thread) ---------------- */
static SP<CEventLoopTimer> g_drainTimer;

static void drainQueue(SP<CEventLoopTimer> self, void *data)
{
    g_drain_fires++;
    if (!g_socketRunning) {
        /* shutting down: never re-arm, leave nothing pending */
        self->updateTimeout(std::nullopt);
        return;
    }
    if (g_inDrain) {
        /* nested event dispatch: leave work for the next tick */
        self->updateTimeout(std::chrono::milliseconds(10));
        return;
    }
    g_inDrain = true;
    {
        std::lock_guard<std::mutex> lg(g_ringMutex);
        while (g_ringCount > 0) {
            SOskCommand c = g_ring[g_ringHead];
            g_ringHead = (g_ringHead + 1) % RING_SIZE;
            g_ringCount--;
            g_ringMutex.unlock();
            switch (c.type) {
            case SOskCommand::EType::KEY: execKey((unsigned)c.a, c.b); break;
            case SOskCommand::EType::MOD: {
                unsigned bit = modnames[c.a].modbit; /* xkb mask: shift=1 ctrl=4 alt=8 super=64 */
                if (c.b && !(held_mods & bit)) {
                    held_mods |= bit;
                    sendMods(held_mods);
                } else if (!c.b && (held_mods & bit)) {
                    held_mods &= ~bit;
                    sendMods(held_mods);
                }
                break;
            }
            case SOskCommand::EType::MODS: {
                held_mods = 0;
                sendMods(0);
                break;
            }
            case SOskCommand::EType::TEXT:
                Log::logger->log(Log::INFO, "[hypr-osk] execText: '{}'", std::string(c.text));
                execText(c.text);
                break;
            case SOskCommand::EType::LAYOUT: execLayout(c.text); break;
            case SOskCommand::EType::PMOVE: execPmove(c.a, c.b); break;
            case SOskCommand::EType::PBTN: execPbtn((unsigned)c.a, c.b); break;
            }
            g_ringMutex.lock();
        }
    }
    g_inDrain = false; /* CRITICAL: without this the drain stalls forever after the first tick */
    if (g_socketRunning)
        self->updateTimeout(std::chrono::milliseconds(10));
}

/* ---------------- plugin init ---------------- */
static std::string socketPath()
{
    const char *rtd = getenv("XDG_RUNTIME_DIR");
    return std::string(rtd ? rtd : "/tmp") + "/hypr-osk.sock";
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    g_touchDownHook = Event::bus()->m_events.input.touch.down.listen(
        [](ITouch::SDownEvent ev, Event::SCallbackInfo &info) { touchDown(ev, info); });
    g_touchUpHook = Event::bus()->m_events.input.touch.up.listen(
        [](ITouch::SUpEvent ev, Event::SCallbackInfo &info) { touchUp(ev, info); });
    g_touchMoveHook = Event::bus()->m_events.input.touch.motion.listen(
        [](ITouch::SMotionEvent ev, Event::SCallbackInfo &info) { touchMotion(ev, info); });

    /* register the synthetic keyboard device — with a keymap FIRST: seat
     * paths throw for a keyboard device without one */
    g_oskKeyboard = makeShared<COskKeyboard>();
    g_oskKeyboard->m_deviceName = "hypr-osk-vk";
    g_oskKeyboard->m_hlName     = "hypr-osk-vk";
    /* CRÍTICO: los listeners que setupKeyboard registra hacen
     * keeb->m_self.lock() en cada evento de tecla; sin esto PKEEB es null
     * y el primer KEY/MOD inyectado derefencia null (SIGSEGV). Es lo mismo
     * que hace CVirtualKeyboard::create() en Hyprland. */
    g_oskKeyboard->m_self = g_oskKeyboard;
    IKeyboard::SStringRuleNames rules;
    rules.layout  = "us";
    rules.model   = "";
    rules.variant = "";
    rules.options = "";
    rules.rules   = "";
    g_oskKeyboard->setKeymap(rules);
    try {
        g_pInputManager->newKeyboard(g_oskKeyboard);
    } catch (const std::exception &e) {
        Log::logger->log(Log::ERR, "[hypr-osk] newKeyboard threw: {}", std::string(e.what()));
        g_oskKeyboard.reset();
    } catch (...) {
        Log::logger->log(Log::ERR, "[hypr-osk] newKeyboard threw (unknown)");
        g_oskKeyboard.reset();
    }
    if (g_oskKeyboard) {
        /* default keymap comes from the QML client (LANG-derived) over the
         * socket; until then the text map and grid describe the init keymap */
        rebuildTextMap();
        rebuildGrid();
    }

    g_drainTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(10), drainQueue, nullptr);
    g_pEventLoopManager->addTimer(g_drainTimer);

    const char *anypeer = getenv("HYPR_OSK_ALLOW_ANY_PEER");
    g_allowAnyPeer = anypeer && *anypeer && strcmp(anypeer, "0") != 0;
    if (g_allowAnyPeer)
        Log::logger->log(Log::INFO,
                         "[hypr-osk] HYPR_OSK_ALLOW_ANY_PEER=1: peer exe check skipped, PMOVE/PBTN enabled");

    g_socketRunning = true;
    g_socketThread = std::thread(socket_thread_fn, socketPath());

    Log::logger->log(Log::INFO, "[hypr-osk] plugin initialized, socket at " + socketPath());
    return {"hypr-osk", "On-screen keyboard: touch->pointer + keyboard synthesis", "ekollof", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    /* stop the socket thread and join it BEFORE the .so is unmapped: the
     * thread's code lives in this library */
    g_socketRunning = false;
    if (g_socketThread.joinable())
        g_socketThread.join();
    /* disconnect the bus listeners: their lambdas also live in this library */
    g_touchDownHook.reset();
    g_touchUpHook.reset();
    g_touchMoveHook.reset();
    /* fully remove timers from the manager's list: a cancelled timer alone
     * still sits in that list, and the idle purge deletes it later — after
     * dlclose that ran into unmapped plugin code and crashed the compositor */
    if (g_applyTimer) {
        g_applyTimer->cancel();
        g_pEventLoopManager->removeTimer(g_applyTimer);
        g_applyTimer.reset();
    }
    if (g_drainTimer) {
        g_drainTimer->cancel();
        g_pEventLoopManager->removeTimer(g_drainTimer);
        g_drainTimer.reset();
    }
    /* release anything still held, then unregister the keyboard device: the
     * destroy signal removes it from the input manager before dlclose */
    for (auto it = g_pressedKeys.begin(); it != g_pressedKeys.end();)
        execKey(*it++, 0);
    if (g_oskKeyboard) {
        g_oskKeyboard->m_events.destroy.emit();
        g_oskKeyboard.reset();
    }
    Log::logger->log(Log::INFO, "[hypr-osk] unloaded");
}
