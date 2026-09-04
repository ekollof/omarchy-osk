/*
 * hypr-osk — Hyprland plugin: touchscreen → pointer/keyboard for on-screen
 * keyboard use.
 *
 * Runs inside the compositor:
 *   - Single-finger touch: cursor under the finger (warpTo, no
 *     acceleration). A landing second finger before the drag slop becomes
 *     a scroll (no drag-select). Movement past ~12 px presses left and
 *     drags; a quick tap clicks on lift; a still hold (~450 ms) is a
 *     right click. Touches are consumed before they reach applications:
 *     no double input, no browser touch gestures.
 *   - Two-finger drag: pixel scroll on both axes. SOURCE_FINGER so Chromium
 *     uses a precision ScrollEvent (no wheel animation lag). Legacy axis
 *     value is /12 so Chromium's OnAxis (÷10 × 120) does not 12×-stack on
 *     v120. Fling after lift. Two-finger pinch: ctrl+wheel zoom.
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
 *   PMOVE / PBTN                 always "err pointer disabled" (no remote
 *                                pointer injection)
 *   FLING <tau_ms> <cap_px_s>    scroll momentum: decay time constant and
 *                                entry-velocity cap for the post-lift fling
 *   POINTER <slop_px> <long_ms>  drag slop (px before left-down) and
 *                                long-press delay (ms; 0 = right-click off)
 *   SCROLL <gain_pct> [0|1]      two-finger scroll speed (50–200, 100 = 1×);
 *                                optional 1 = pixel axis value (terminals),
 *                                0 = Chromium-scaled (value = px/12)
 *   SWALLOW <0|1>                consume touchscreen input (virtual pointer
 *                                + gestures) or pass it to Hyprland's native
 *                                touchscreen support
 *
 * Access control: the socket can type into the focused session, so peers are
 * validated on accept with SO_PEERCRED — the uid must match, and
 * /proc/<pid>/exe must realpath to a root-owned, non-group/world-writable
 * regular file that is the packaged shell (/usr/bin/quickshell). Basename
 * matching is not used. Injection commands (TEXT/KEY/MOD) are refused with
 * "err hidden" while the keyboard panel is not on screen (no PANEL rect).
 *
 * Commands are queued from the socket thread and executed on the
 * compositor main thread via an EventLoop timer.
 */
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <set>
#include <hyprland/src/event/EventBus.hpp>

#include <cmath>
#include <cstdlib>
#include <climits>
#include <fstream>
#include <fcntl.h>
#include <cerrno>
#include <sys/eventfd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/limits.h>
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
    enum class EType : uint8_t { KEY, MOD, MODS, TEXT, LAYOUT, PMOVE, PBTN, FLING, POINTER, SCROLL, SWALLOW, PANEL } type;
    int   a = 0, b = 0;
    float panel[4]      = {0, 0, 0, 0}; /* PANEL nx ny nw nh */
    char  text[TEXT_CAP] = {0};
};
static constexpr size_t RING_SIZE = 64;
static SOskCommand      g_ring[RING_SIZE];
static std::mutex       g_ringMutex;
static size_t           g_ringHead = 0, g_ringCount = 0;
static bool             g_inDrain = false;
static std::atomic<bool> g_socketRunning{false};
static std::atomic<bool> g_panelVisible{false}; /* main thread writes; socket thread reads for the hidden gate */
static int               g_wakePipe[2] = {-1, -1}; /* self-pipe: wakes the socket thread's polls */
static int               g_drainEventFd = -1;      /* eventfd: socket thread → compositor loop */
static wl_event_source  *g_drainEventSource = nullptr;

static void wakeDrain();

static void queueCommand(SOskCommand cmd)
{
    bool queued = false;
    {
        std::lock_guard<std::mutex> lg(g_ringMutex);
        if (g_ringCount >= RING_SIZE) {
            DBG("ring full: command dropped");
        } else {
            g_ring[(g_ringHead + g_ringCount) % RING_SIZE] = cmd;
            g_ringCount++;
            queued = true;
        }
    }
    if (queued)
        wakeDrain(); /* main thread only: the eventfd callback arms the drain timer */
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

/* one letter-grid key: evdev code plus level-0/1 labels from the keymap */
struct SGridKey {
    unsigned    c = 0;
    std::string l0, l1;
    bool        raw = false;
};

static bool gridKeyFromMap(xkb_keymap *km, unsigned evdev, SGridKey &out)
{
    out = SGridKey{evdev, {}, {}, false};
    xkb_keycode_t         key    = evdev + 8; /* grid carries evdev; xkb is evdev+8 */
    xkb_layout_index_t    layout = 0;
    xkb_level_index_t     nlev   = xkb_keymap_num_levels_for_key(km, key, layout);
    const xkb_keysym_t   *syms   = nullptr;
    if (nlev > 0 && xkb_keymap_key_get_syms_by_level(km, key, layout, 0, &syms) > 0 && syms[0]) {
        out.l0 = symLabel(syms[0]);
        if (out.l0.empty())
            out.raw = true; /* dead key or modifier: type the raw keycode */
    }
    if (nlev > 1 && xkb_keymap_key_get_syms_by_level(km, key, layout, 1, &syms) > 0 && syms[0])
        out.l1 = symLabel(syms[0]);
    return !(out.l0.empty() && out.l1.empty() && !out.raw);
}

static void jsonAppendGridKey(std::string &out, const SGridKey &k, bool &first)
{
    if (!first)
        out += ",";
    first = false;
    out += "{\"c\":" + std::to_string(k.c);
    if (!k.l0.empty()) {
        out += ",\"l\":\"";
        jsonAppendEscaped(out, k.l0);
        out += "\"";
    }
    if (!k.l1.empty()) {
        out += ",\"s\":\"";
        jsonAppendEscaped(out, k.l1);
        out += "\"";
    }
    if (k.raw)
        out += ",\"raw\":1";
    out += "}";
}

/* the main layer's letter grid, from physical evdev rows: keycode order IS the
 * physical arrangement (Q is 16, A is 30, Z is 44 for every latin layout).
 * KEY_102ND (the ISO key between LShift and Z) is in every pc105 keymap —
 * even US, where it duplicates Shift+comma/period — so it is only prepended
 * when it adds a glyph the rest of the letter grid does not already have. */
static void rebuildGrid()
{
    static const unsigned rows[][15] = {
        {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 41, 0},         /* digits + - = ` */
        {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 43, 0}, /* q .. ] \ */
        {30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 0},         /* a .. ' */
        {44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 0},             /* z .. / */
    };
    xkb_keymap *km = g_oskKeyboard ? g_oskKeyboard->m_xkbKeymap : nullptr;
    std::vector<SGridKey> dumped[4];
    std::set<std::string> seen;
    if (km) {
        for (size_t r = 0; r < 4; r++) {
            for (size_t i = 0; rows[r][i]; i++) {
                SGridKey k;
                if (!gridKeyFromMap(km, rows[r][i], k))
                    continue;
                dumped[r].push_back(k);
                if (!k.l0.empty())
                    seen.insert(k.l0);
                if (!k.l1.empty())
                    seen.insert(k.l1);
            }
        }
        SGridKey iso;
        if (gridKeyFromMap(km, KEY_102ND, iso)) {
            bool unique = iso.raw;
            if (!iso.l0.empty() && !seen.count(iso.l0))
                unique = true;
            if (!iso.l1.empty() && !seen.count(iso.l1))
                unique = true;
            if (unique)
                dumped[3].insert(dumped[3].begin(), iso);
        }
    }
    std::string json = "{\"rows\":[";
    for (size_t r = 0; r < 4; r++) {
        if (r)
            json += ",";
        json += "[";
        bool first = true;
        for (const auto &k : dumped[r])
            jsonAppendGridKey(json, k, first);
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
            /* SKeyHit.key is evdev; xkb_keymap_* wants evdev+8 */
            nmasks = xkb_keymap_key_get_mods_for_level(km, it->second.key + 8, it->second.layout,
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
    held_mods = 0;
    sent_mods = 0;
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
static Vector2D scroll_anchor;                  /* normalized contact-center at last scroll sample */
static double   scroll_travel_px = 0;           /* px moved this gesture (tap-vs-scroll heuristic) */
static double   scroll_raw_px = 0;              /* unaccelerated travel (scroll->pinch handoff math) */
static uint32_t dual_start_ms = 0;
static Vector2D lastPos;                        /* normalized (0..1) */
static bool     pressed = false;                /* left button held */
static bool     panel_pressed = false;          /* synthetic click on the OSK panel */
static unsigned g_drain_fires = 0;               /* drain timer fire count */
static bool     down_flag = false, up_flag = false, motion_flag = false;
static std::string touchDeviceOutput = ""; /* resolved per gesture from the touch device's
                                            * bound output (touchDown); empty → focus monitor */
static bool     apply_pending = false;
static bool     press_pending = false;          /* single finger, no button yet: tap / slop-drag /
                                                 * long-press; a landing second finger cancels it */
static bool     long_press_due = false;         /* long-press timer fired (main thread) */
static Vector2D press_origin;                   /* normalized down point for slop */
static double   drag_slop_px  = 12.0;           /* POINTER cmd; px before left-down */
static int      long_press_ms = 450;            /* POINTER cmd; 0 disables right-click hold */
static std::unordered_map<int32_t, Vector2D> g_slotPos; /* live contact positions (touchID → pos) */
static bool     gesture_decided = false;        /* scroll-vs-pinch latch for the current gesture */
static bool     pinch_mode = false;             /* latched pinch (mutually exclusive with scroll) */
static double   pinch_delta = 0;                /* signed contact-distance change since gesture start */
static double   pinch_prev_d = 0;               /* previous frame's contact distance */
static bool     pinch_prev_valid = false;
static int      pinch_ctrl_held = 0;            /* ctrl held on the synthetic keyboard for zoom */
static Vector2D scroll_vel;                     /* smoothed finger velocity, px/s */
static uint32_t scroll_last_ms = 0;             /* last scroll-frame timestamp */
static bool     fling_active = false;           /* momentum scrolling after lift */
static uint32_t fling_last_ms = 0;
static SP<CEventLoopTimer> g_flingTimer;
static double   fling_tau = 0.32;               /* momentum decay constant (s, FLING cmd) */
static double   fling_cap = 5500.0;             /* fling entry velocity cap (px/s) */
static double   fling_min = 200.0;              /* minimum lift velocity to fling (px/s) */
static double   scroll_gain = 1.0;              /* SCROLL cmd; 1.0 = 100% */
static bool     scroll_axis_px = false;         /* SCROLL 2nd arg: full-pixel axis value */
static bool     touch_swallow = true;           /* consume all touch input (virtual pointer);
                                                 * off = native touchscreen support */

/* OSK panel exemption: touches inside this rect (normalized 0..1 on the
 * touch device's frame) pass through to the panel's own Qt touch handling;
 * everything else is consumed and emulated. Announced by the QML client
 * via PANELNORM. */
static double panel_nx = 0, panel_ny = 0, panel_nw = 0, panel_nh = 0;
static bool   panel_rect_valid = false;
static bool   contact_is_panel_native = false;

/* snapshot of the touch monitor — written on the main thread, read by MON/STATS */
struct SMonSnap {
    char name[64] = {0};
    int  x = 0, y = 0, w = 0, h = 0;
    bool valid = false;
};
static std::mutex g_monMutex;
static SMonSnap   g_monSnap;

struct SStatsSnap {
    int      fingers = 0, pressed = 0, ignore = 0, scroll = 0, down = 0, up = 0;
    int      contact = 0, panel_valid = 0, inject = 0, anypeer = 0, swallow = 0, indrain = 0;
    unsigned fires     = 0;
    size_t   ring      = 0;
    size_t   textmap   = 0;
    double   panel_ny = 0, panel_nh = 0, lastx = 0, lasty = 0, fling_tau_ms = 0, fling_cap = 0;
    char     layout[64] = {0};
};
static std::mutex  g_statsMutex;
static SStatsSnap  g_statsSnap;

static void publishStats()
{
    SStatsSnap s;
    s.fingers     = fingers;
    s.pressed     = (int)pressed;
    s.ignore      = (int)ignore_until_zero;
    s.scroll      = (int)scroll_mode;
    s.down        = (int)down_flag;
    s.up          = (int)up_flag;
    s.contact     = (int)contact_is_panel_native;
    s.panel_valid = (int)panel_rect_valid;
    s.inject      = (int)g_panelVisible.load(std::memory_order_relaxed);
    s.anypeer     = 0; /* remote pointer injection is not available */
    s.swallow     = (int)touch_swallow;
    s.indrain     = (int)g_inDrain;
    s.fires       = g_drain_fires;
    s.panel_ny    = panel_ny;
    s.panel_nh    = panel_nh;
    s.lastx       = lastPos.x;
    s.lasty       = lastPos.y;
    s.fling_tau_ms = fling_tau * 1000.0;
    s.fling_cap    = fling_cap;
    s.textmap      = g_textMap.size();
    {
        std::lock_guard<std::mutex> lg(g_ringMutex);
        s.ring = g_ringCount;
    }
    snprintf(s.layout, sizeof s.layout, "%s", g_layoutSpec.c_str());
    std::lock_guard<std::mutex> lg(g_statsMutex);
    g_statsSnap = s;
}

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

static SP<Monitor::CMonitor> resolveTouchMonitor()
{
    auto mon = State::monitorState()
                   ->query()
                   .name(!touchDeviceOutput.empty() ? touchDeviceOutput : "")
                   .run();
    if (!mon)
        mon = Desktop::focusState()->monitor();
    SMonSnap snap{};
    if (mon) {
        snprintf(snap.name, sizeof snap.name, "%s", mon->m_name.c_str());
        snap.x     = (int)mon->m_position.x;
        snap.y     = (int)mon->m_position.y;
        snap.w     = (int)mon->m_size.x;
        snap.h     = (int)mon->m_size.y;
        snap.valid = true;
    }
    {
        std::lock_guard<std::mutex> lg(g_monMutex);
        g_monSnap = snap;
    }
    return mon;
}

/* center + distance of the two live contacts (scroll/pinch geometry).
 * Some digitizers (NVTK0603) never vary touchID, so slots can collapse to
 * one entry — in that case pair the stored position against lastPos. */
static bool contactGeometry(Vector2D &center, double &dist)
{
    if (g_slotPos.size() >= 2) {
        auto     it = g_slotPos.begin();
        Vector2D a  = it->second;
        ++it;
        Vector2D b = it->second;
        center      = Vector2D{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
        dist        = std::hypot(a.x - b.x, a.y - b.y);
        return true;
    }
    if (g_slotPos.size() == 1) {
        Vector2D a = g_slotPos.begin()->second;
        if (std::abs(a.x - lastPos.x) > 0.001 || std::abs(a.y - lastPos.y) > 0.001) {
            center = Vector2D{(a.x + lastPos.x) / 2.0, (a.y + lastPos.y) / 2.0};
            dist   = std::hypot(a.x - lastPos.x, a.y - lastPos.y);
            return true;
        }
    }
    return false;
}

static void releasePinchCtrl()
{
    if (pinch_ctrl_held) {
        execKey(KEY_LEFTCTRL, 0);
        pinch_ctrl_held = 0;
    }
}

/* DBG is a silent no-op from inside a plugin (header-inline logger singleton),
 * so the gesture geometry traces go straight to a file — gated behind
 * HYPR_OSK_TRACE=1 in the compositor's environment (low power: no writes,
 * no open() calls unless explicitly asked for) */
static void traceGeom(const std::string &line)
{
    static const bool enabled = [] {
        const char *e = getenv("HYPR_OSK_TRACE");
        return e && *e && strcmp(e, "0") != 0;
    }();
    if (!enabled)
        return;
    std::ofstream f("/tmp/hypr-osk-geom.log", std::ios::app);
    f << nowMs() << " " << line << "\n";
}

/* Two-finger scroll: SOURCE_FINGER → Chromium ET_SCROLL (no wheel-smooth
 * lag). Chromium OnAxis is `value/10*120` (=×12); terminals treat the
 * continuous axis as HIGHRES pixels. Known terminal exes get value=px;
 * everyone else gets value=px/12 + v120=px. Widget "pixel axis" forces
 * the terminal encoding for all clients. */
static bool g_fingerAxisLive = false;

static void emitFingerAxisStop()
{
    if (!g_fingerAxisLive)
        return;
    g_fingerAxisLive = false;
    auto seatRes     = g_pSeatManager->m_state.pointerFocusResource.lock();
    if (!seatRes)
        return;
    const uint32_t t = nowMs();
    for (auto &wp : seatRes->m_pointers) {
        auto p = wp.lock();
        if (!p || !p->good() || p->version() < 5)
            continue;
        p->sendAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
        p->sendAxisStop(t, WL_POINTER_AXIS_VERTICAL_SCROLL);
        p->sendAxisStop(t, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        p->sendFrame();
    }
}

static bool pointerFocusIsTerminal()
{
    static const char *const terms[] = {
        "kitty",
        "alacritty",
        "foot",
        "footclient",
        "wezterm",
        "wezterm-gui",
        "ghostty",
        "rio",
        "contour",
        "kgx",
        "gnome-terminal",
        "gnome-terminal-server",
        "konsole",
        "qterminal",
        "terminator",
        "tilix",
        "urxvt",
        "rxvt",
        "xterm",
        "st",
        "xfce4-terminal",
        "lxterminal",
        "mate-terminal",
        "ptyxis",
        "blackbox",
        "cool-retro-term",
        nullptr,
    };
    auto surf = g_pSeatManager->m_state.pointerFocus.lock();
    if (!surf)
        return false;
    wl_client *cl = surf->client();
    if (!cl)
        return false;
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(cl, &pid, &uid, &gid);
    if (pid <= 0)
        return false;
    char link[64], path[256];
    snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0)
        return false;
    path[n] = 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (const char *const *t = terms; *t; t++) {
        if (strcmp(base, *t) == 0)
            return true;
    }
    return false;
}

static void emitScroll(double pxx, double pxy)
{
    pxx *= scroll_gain;
    pxy *= scroll_gain;
    if (pxx == 0.0 && pxy == 0.0)
        return;
    const bool   px  = scroll_axis_px || pointerFocusIsTerminal();
    const double div = px ? 1.0 : 12.0;
    uint32_t     t   = nowMs();
    if (pxx != 0.0)
        g_pSeatManager->sendPointerAxis(t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -pxx / div, 0,
                                        (int32_t)std::lround(-pxx), WL_POINTER_AXIS_SOURCE_FINGER,
                                        WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (pxy != 0.0)
        g_pSeatManager->sendPointerAxis(t, WL_POINTER_AXIS_VERTICAL_SCROLL, -pxy / div, 0,
                                        (int32_t)std::lround(-pxy), WL_POINTER_AXIS_SOURCE_FINGER,
                                        WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    g_pSeatManager->sendPointerFrame();
    g_fingerAxisLive = true;
    scroll_travel_px += std::hypot(pxx, pxy);
}

static void pointerWarp(SP<Monitor::CMonitor> mon, Vector2D npos)
{
    if (!mon)
        return;
    Vector2D global = mon->m_position + (npos * mon->m_size);
    Pointer::pointerController()->warpTo(global, true);
    g_pInputManager->simulateMouseMovement();
}

static void pointerClick(uint32_t btn)
{
    uint32_t t = nowMs();
    g_pSeatManager->sendPointerButton(t, btn, WL_POINTER_BUTTON_STATE_PRESSED);
    g_pSeatManager->sendPointerFrame();
    g_pSeatManager->sendPointerButton(t + 1, btn, WL_POINTER_BUTTON_STATE_RELEASED);
    g_pSeatManager->sendPointerFrame();
}

static double distPx(Vector2D a, Vector2D b, SP<Monitor::CMonitor> mon)
{
    if (!mon)
        return 0;
    return std::hypot((a.x - b.x) * mon->m_size.x, (a.y - b.y) * mon->m_size.y);
}

/* ---------------- fling momentum (android-style) ---------------- */
static void stopFling()
{
    fling_active = false;
    scroll_vel   = Vector2D{0, 0};
    emitFingerAxisStop();
}

static void flingTick()
{
    if (!fling_active)
        return;
    uint32_t now = nowMs();
    double dt = (now - fling_last_ms) / 1000.0;
    fling_last_ms = now;
    if (dt <= 0.0 || dt > 0.25) {
        stopFling();
        return;
    }
    double decay = std::exp(-dt / fling_tau); /* friction: ~28% velocity loss per 100 ms at 0.32 s */
    scroll_vel.x *= decay;
    scroll_vel.y *= decay;
    if (std::hypot(scroll_vel.x, scroll_vel.y) < 130.0) {
        stopFling();
        return;
    }
    emitScroll(scroll_vel.x * dt, scroll_vel.y * dt);
    if (g_flingTimer)
        g_flingTimer->updateTimeout(std::chrono::milliseconds(16));
}

/* still single-finger hold → right click. Cancelled by slop-drag, a second
 * finger, lift, or swallow-off. */
static SP<CEventLoopTimer> g_pressTimer; /* reused: long-press, not the old 130 ms left-down */

static void cancelLongPress()
{
    long_press_due = false;
    if (g_pressTimer)
        g_pressTimer->updateTimeout(std::nullopt);
}

static void armLongPressTimer()
{
    if (long_press_ms <= 0) {
        cancelLongPress();
        return;
    }
    auto delay = std::chrono::milliseconds(long_press_ms);
    if (!g_pressTimer) {
        g_pressTimer = makeShared<CEventLoopTimer>(
            delay,
            [](SP<CEventLoopTimer>, void *) {
                long_press_due = true;
                applyTouches();
            },
            nullptr);
        g_pEventLoopManager->addTimer(g_pressTimer);
    } else
        g_pressTimer->updateTimeout(delay);
}

/* handlers: record state + schedule only — no compositor calls (calling
 * input/monitor code from inside the touch callback deadlocks the pipeline) */
static void touchDown(ITouch::SDownEvent ev, Event::SCallbackInfo &info)
{
    if (!touch_swallow) {
        /* native touchscreen mode: hand the contact to Hyprland untouched */
        info.cancelled = false;
        return;
    }
    stopFling(); /* a new touch always kills momentum */
    fingers++;
    lastPos            = ev.pos;
    g_slotPos[ev.touchID] = ev.pos;
    /* the touch device's bound output decides which monitor frame ev.pos is
     * normalized against — no hardcoded display anywhere */
    if (ev.device && !ev.device->m_boundOutput.empty())
        touchDeviceOutput = ev.device->m_boundOutput;
    if (fingers == 1)
        contact_is_panel_native = posInPanel(ev.pos.x, ev.pos.y); /* primary contact decides the mode */
    down_flag = true; /* EVERY down must apply: the resolver needs to see finger #2 to enter scroll */
    if (fingers == 1 && !contact_is_panel_native) {
        /* no button yet: a second finger converts this to a scroll; movement
         * past drag_slop_px left-drags; a still hold becomes a right click */
        press_pending = true;
        press_origin  = ev.pos;
        armLongPressTimer();
    }
    info.cancelled = true; /* consumed: Hyprland's touch refocus would steal keyboard focus */
    scheduleApply();
}

static void touchUp(ITouch::SUpEvent ev, Event::SCallbackInfo &info)
{
    if (!touch_swallow) {
        info.cancelled = false;
        return;
    }
    if (fingers > 0)
        fingers--;
    g_slotPos.erase(ev.touchID);
    up_flag = true;
    info.cancelled = true;
    scheduleApply();
}

static void touchMotion(ITouch::SMotionEvent ev, Event::SCallbackInfo &info)
{
    if (!touch_swallow) {
        info.cancelled = false;
        return;
    }
    lastPos       = ev.pos;
    g_slotPos[ev.touchID] = ev.pos; /* keep slot positions live — stale slots freeze the
                                     * scroll/pinch geometry at the down points */
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
        press_pending = false;
        cancelLongPress();
        stopFling();
        releasePinchCtrl();
        g_slotPos.clear();
        pinch_mode      = false;
        gesture_decided = false;
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
            /* two-finger tap without scrolling or pinching = right click */
            if (std::abs(scroll_travel_px) < 10 && !pinch_mode && nowMs() - dual_start_ms <= 250) {
                pointerClick(BTN_RIGHT);
                DBG("two-finger tap: right click");
            }
            releasePinchCtrl();
            pinch_mode      = false;
            gesture_decided = false;
            /* android-style fling: keep going with the lift velocity, decay */
            double speed = std::hypot(scroll_vel.x, scroll_vel.y);
            if (speed > fling_min) {
                fling_active  = true;
                fling_last_ms = nowMs();
                if (speed > fling_cap) {
                    double s = fling_cap / speed;
                    scroll_vel.x *= s;
                    scroll_vel.y *= s;
                }
                if (!g_flingTimer) {
                    g_flingTimer = makeShared<CEventLoopTimer>(
                        std::chrono::milliseconds(16),
                        [](SP<CEventLoopTimer>, void*) { flingTick(); },
                        nullptr);
                    g_pEventLoopManager->addTimer(g_flingTimer);
                } else
                    g_flingTimer->updateTimeout(std::chrono::milliseconds(16));
            } else {
                scroll_vel = Vector2D{0, 0};
                emitFingerAxisStop();
            }
        }
        if (fingers == 0 && press_pending) {
            /* quick tap: no slop, no long-press — click on lift */
            press_pending = false;
            cancelLongPress();
            pointerWarp(resolveTouchMonitor(), lastPos);
            pointerClick(BTN_LEFT);
            DBG("tap: press+release at lift");
        } else if (fingers == 0 && pressed) {
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
        if (fingers == 0) {
            contact_is_panel_native = false;
            g_slotPos.clear();
        }
    }

    if (down_flag) {
        /* mode resolver: settles the gesture state from (fingers, pressed).
         * Runs for EVERY contact down — the second finger's down is what
         * enters scroll mode; batched landings (both fingers before the
         * apply timer) resolve here in one pass. */
        down_flag = false;
        auto mon = resolveTouchMonitor();

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
            press_pending = false; /* panel taps press immediately, nothing deferred */
            cancelLongPress();
        } else if (fingers >= 3) {
            /* 3+ fingers: hyprgrass territory */
            if (pressed) {
                g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                g_pSeatManager->sendPointerFrame();
                pressed = false;
            }
            scroll_mode = false;
            press_pending = false;
            cancelLongPress();
            releasePinchCtrl();
            pinch_mode      = false;
            gesture_decided = false;
        } else if (fingers == 2) {
            /* two fingers: end any drag, enter two-finger scroll — the
             * pending single-finger tap/long-press dies here, so scrolling
             * never starts with a held button */
            press_pending = false;
            cancelLongPress();
            if (pressed) {
                g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                g_pSeatManager->sendPointerFrame();
                pressed = false;
            }
            if (!scroll_mode) {
                scroll_mode       = true;
                scroll_travel_px  = 0;
                scroll_raw_px     = 0;
                dual_start_ms     = nowMs();
                scroll_vel        = Vector2D{0, 0};
                scroll_last_ms    = 0;
                gesture_decided   = false;
                pinch_mode        = false;
                pinch_delta       = 0;
                Vector2D c;
                double   d;
                if (contactGeometry(c, d)) {
                    scroll_anchor    = c; /* scroll (if it wins) follows the contact center */
                    pinch_prev_d     = d;
                    pinch_prev_valid = true;
                    traceGeom("entry slots=" + std::to_string(g_slotPos.size()) + " d=" + std::to_string(d));
                } else {
                    scroll_anchor    = lastPos;
                    pinch_prev_valid = false;
                    traceGeom("entry slots=" + std::to_string(g_slotPos.size()) + " no geometry");
                }
                DBG("two fingers: scroll mode");
            }
        } else if (press_pending) {
            /* primary contact: no button yet; cursor tracks the finger */
            pointerWarp(mon, lastPos);
        }
    }

    if (long_press_due) {
        long_press_due = false;
        if (press_pending && fingers == 1 && !pressed && !contact_is_panel_native) {
            pointerWarp(resolveTouchMonitor(), lastPos);
            pointerClick(BTN_RIGHT);
            press_pending     = false;
            ignore_until_zero = true;
            DBG("long-press: right click");
        }
    }

    if (fingers == 1 && (pressed || press_pending)) {
        /* motion: cursor follows the finger; past slop, left-down and drag */
        auto mon = resolveTouchMonitor();
        pointerWarp(mon, lastPos);
        if (press_pending && distPx(lastPos, press_origin, mon) > drag_slop_px) {
            cancelLongPress();
            g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
            g_pSeatManager->sendPointerFrame();
            pressed       = true;
            press_pending = false;
            DBG("slop: left press, drag");
        }
    } else if (fingers == 1 && panel_pressed) {
        /* drag on the panel: cursor follows, pointer focus stays on the layer.
         * Sliding off the keyboard must not become a click-drag into the client. */
        if (!posInPanel(lastPos.x, lastPos.y)) {
            g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
            g_pSeatManager->sendPointerFrame();
            panel_pressed           = false;
            contact_is_panel_native = false;
            ignore_until_zero       = true;
        } else {
            auto mon = resolveTouchMonitor();
            Vector2D global = mon->m_position + (lastPos * mon->m_size);
            Pointer::pointerController()->warpTo(global, true);
            g_pInputManager->simulateMouseMovement();
        }
    } else if (scroll_mode && fingers == 2) {
        /* two-finger gesture: the contact-center delta scrolls, the contact-
         * distance delta zooms; whichever dominates by the decision threshold
         * latches the gesture (pixel scroll / ctrl+wheel zoom) */
        auto mon  = resolveTouchMonitor();
        if (!mon)
            return;
        Vector2D center = lastPos;
        double   d      = pinch_prev_d;
        contactGeometry(center, d);
        if (!gesture_decided) {
            if (pinch_prev_valid)
                pinch_delta += d - pinch_prev_d;
            double centerPx = distPx(center, scroll_anchor, mon);
            double pinchPx  = std::abs(pinch_delta) * mon->m_size.y;
            static unsigned geom_dbg = 0;
            if (++geom_dbg % 5 == 0)
                traceGeom("undecided slots=" + std::to_string(g_slotPos.size()) + " d=" + std::to_string(d) +
                          " pinchPx=" + std::to_string(pinchPx) + " centerPx=" + std::to_string(centerPx));
            if (centerPx + pinchPx > 9) {
                gesture_decided = true;
                pinch_mode      = pinchPx > 1.4 * centerPx;
                scroll_anchor   = center; /* consume the dead-zone drift */
                traceGeom("decided pinch=" + std::string(pinch_mode ? "yes" : "no") +
                          " pinchPx=" + std::to_string(pinchPx) + " centerPx=" + std::to_string(centerPx));
            }
        }
        if (gesture_decided && pinch_mode) {
            /* pinch: ctrl+wheel — universal continuous v120. Native wl_touch
             * pinch is unreachable (the plugin consumes all touch input), so
             * smoothness is recreated with per-frame deltas: Chromium's ctrl
             * +high-res path zooms smoothly and snaps to presets on gesture
             * end; other v8 clients get proportional ctrl-scroll. */
            double dd = pinch_prev_valid ? (d - pinch_prev_d) * mon->m_size.y : 0.0; /* px */
            if (dd != 0.0) {
                if (!pinch_ctrl_held) {
                    execKey(KEY_LEFTCTRL, 1);
                    pinch_ctrl_held = 1;
                }
                /* legacy value keeps the /10 x 120 convention in parity */
                g_pSeatManager->sendPointerAxis(nowMs(), WL_POINTER_AXIS_VERTICAL_SCROLL,
                                                -dd / 12.0, 0, (int32_t)std::lround(-dd),
                                                WL_POINTER_AXIS_SOURCE_WHEEL,
                                                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
                g_pSeatManager->sendPointerFrame();
            }
        } else if (gesture_decided) {
            /* scroll: hand off to pinch while little scroll has committed and
             * spread starts to dominate (a pinch intent mislatched as scroll) */
            if (nowMs() - dual_start_ms <= 250 &&
                std::abs(pinch_delta) * mon->m_size.y > 2.0 * scroll_raw_px + 9.0) {
                emitFingerAxisStop();
                pinch_mode = true;
                traceGeom("handoff scroll->pinch");
            } else {
                /* touchpad-style scroll: pixel deltas following the fingers, with
                 * a velocity-scaled acceleration and a fling on lift */
                double pxx = (center.x - scroll_anchor.x) * mon->m_size.x;
                double pxy = (center.y - scroll_anchor.y) * mon->m_size.y;
                uint32_t now = nowMs();
                double dt = scroll_last_ms ? (now - scroll_last_ms) / 1000.0 : 0.0;
                scroll_last_ms = now;
                scroll_anchor  = center; /* consume the full delta: no drift */
                scroll_raw_px += std::hypot(pxx, pxy);
                if (pxx != 0.0 || pxy != 0.0) {
                    if (dt > 0.001 && dt < 0.2) {
                        scroll_vel.x = 0.5 * scroll_vel.x + 0.5 * (pxx / dt);
                        scroll_vel.y = 0.5 * scroll_vel.y + 0.5 * (pxy / dt);
                    }
                    /* 1:1 with the fingers (native wl_touch); speed is scrollGain */
                    emitScroll(pxx, pxy);
                }
            }
        }
        pinch_prev_d     = d;
        pinch_prev_valid = true;
    }
    publishStats();
}


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
 * The socket can type into the focused session, so only the user's own
 * packaged shell may connect. SO_PEERCRED yields pid/uid from the kernel.
 * /proc/<pid>/exe is realpath'd and must be a root-owned, not group/world-
 * writable regular file equal to the packaged quickshell binary — not a
 * spoofable basename. There is no environment switch that skips this. */
static bool peerExeAllowed(pid_t pid)
{
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
    char raw[PATH_MAX];
    ssize_t n = readlink(link, raw, sizeof raw);
    if (n <= 0 || n >= (ssize_t)sizeof raw)
        return false;
    raw[n] = 0;
    if (strstr(raw, " (deleted)"))
        return false;

    char real[PATH_MAX];
    if (!realpath(raw, real))
        return false;

    struct stat st = {};
    if (lstat(real, &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode) || st.st_uid != 0)
        return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH))
        return false;
    if (!(st.st_mode & S_IXUSR))
        return false;

    static const char *const kAllowed[] = {"/usr/bin/quickshell", "/bin/quickshell"};
    for (const char *cand : kAllowed) {
        char allowed[PATH_MAX];
        if (!realpath(cand, allowed))
            continue;
        if (strcmp(real, allowed) == 0)
            return true;
    }
    return false;
}

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
    if (!peerExeAllowed(cred.pid)) {
        DBG("peer check: pid " + std::to_string(cred.pid) + " exe not packaged quickshell, rejected");
        return false;
    }
    return true;
}

static void handle_line(int cfd, char *line)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = 0;
    DBG("cmd: " + std::string(line));

    std::string reply = "err unknown command";
    SOskCommand cmd;

    auto hidden = [] { return !g_panelVisible.load(std::memory_order_acquire); };

    if (strcmp(line, "PING") == 0) {
        reply = "PONG";
    } else if (!strncmp(line, "KEY ", 4)) {
        if (hidden()) {
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
        if (hidden()) {
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
                    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
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
        reply = "grid " + (g_gridJson.empty() ? std::string("{\"rows\":[[],[],[],[]]}") : g_gridJson);
    } else if (!strncmp(line, "TEXT ", 5)) {
        if (hidden()) {
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
    } else if (!strncmp(line, "PMOVE ", 6) || !strncmp(line, "PBTN ", 5)) {
        reply = "err pointer disabled";
    } else if (!strncmp(line, "FLING ", 6)) {
        /* FLING <tau_ms> <cap_px_s> — momentum decay + speed cap */
        int tau, cap;
        if (sscanf(line + 6, "%d %d", &tau, &cap) == 2 && tau >= 50 && tau <= 2000 && cap >= 500 &&
            cap <= 20000) {
            cmd.type = SOskCommand::EType::FLING;
            cmd.a    = tau;
            cmd.b    = cap;
            queueCommand(std::move(cmd));
            reply = "ok";
        } else
            reply = "err bad args";
    } else if (!strncmp(line, "POINTER ", 8)) {
        /* POINTER <slop_px> <long_ms> — drag slop and long-press delay (0 = off) */
        int slop, hold;
        if (sscanf(line + 8, "%d %d", &slop, &hold) == 2 && slop >= 4 && slop <= 40 && hold >= 0 &&
            hold <= 2000) {
            cmd.type = SOskCommand::EType::POINTER;
            cmd.a    = slop;
            cmd.b    = hold;
            queueCommand(std::move(cmd));
            reply = "ok";
        } else
            reply = "err bad args";
    } else if (!strncmp(line, "SCROLL ", 7)) {
        int gain = 0, axispx = -1;
        int n = sscanf(line + 7, "%d %d", &gain, &axispx);
        if (n >= 1 && gain >= 50 && gain <= 200 && (n == 1 || axispx == 0 || axispx == 1)) {
            cmd.type = SOskCommand::EType::SCROLL;
            cmd.a    = gain;
            cmd.b    = n == 2 ? axispx : -1;
            queueCommand(std::move(cmd));
            reply = "ok";
        } else
            reply = "err bad args";
    } else if (!strncmp(line, "SWALLOW ", 8)) {
        /* SWALLOW <0|1> — consume touchscreen input (virtual pointer) or
         * hand it to Hyprland's native touchscreen support */
        if (!strcmp(line + 8, "0") || !strcmp(line + 8, "1")) {
            cmd.type = SOskCommand::EType::SWALLOW;
            cmd.a    = line[8] - '0';
            queueCommand(std::move(cmd));
            reply = "ok";
        } else
            reply = "err bad args";
    } else if (!strcmp(line, "MON")) {
        /* MON — reply from the main-thread snapshot (never call into
         * monitor/input code from this thread) */
        SMonSnap snap;
        {
            std::lock_guard<std::mutex> lg(g_monMutex);
            snap = g_monSnap;
        }
        if (snap.valid) {
            char buf[160];
            snprintf(buf, sizeof buf, "mon %s %d %d %d %d", snap.name, snap.x, snap.y, snap.w, snap.h);
            reply = buf;
        } else
            reply = "err no monitor";
    } else if (!strcmp(line, "STATS")) {
        SStatsSnap s;
        {
            std::lock_guard<std::mutex> lg(g_statsMutex);
            s = g_statsSnap;
        }
        char buf[320];
        snprintf(buf, sizeof buf,
                 "state fingers=%d pressed=%d ignore=%d scroll=%d down=%d up=%d contact_osk=%d "
                 "panel_valid=%d inject=%d anypeer=%d panel_ny=%.3f panel_nh=%.3f last=%.3f,%.3f "
                 "fires=%u ring=%zu indrain=%d layout=%s textmap=%zu fling=%.0fms/%.0fpx swallow=%d",
                 s.fingers, s.pressed, s.ignore, s.scroll, s.down, s.up, s.contact, s.panel_valid,
                 s.inject, s.anypeer, s.panel_ny, s.panel_nh, s.lastx, s.lasty, s.fires,
                 s.ring, s.indrain, s.layout, s.textmap, s.fling_tau_ms, s.fling_cap, s.swallow);
        reply = buf;
    } else if (!strncmp(line, "CALIB", 5)) {
        reply = "ok"; /* accepted for protocol compatibility; the frame comes from the compositor */
    } else if (!strncmp(line, "PANEL ", 6)) {
        /* PANEL x y w h — queued to the main thread (applyTouches reads the rect) */
        char *a = line + 6;
        char *b = strchr(a, ' ');
        char *c = b ? strchr(b + 1, ' ') : NULL;
        char *d = c ? strchr(c + 1, ' ') : NULL;
        if (b && c && d) {
            *b = 0; *c = 0; *d = 0;
            char *endp;
            float px = strtof(a, &endp);     bool ok1 = endp != a;
            float py = strtof(b + 1, &endp); bool ok2 = endp != b + 1;
            float pw = strtof(c + 1, &endp); bool ok3 = endp != c + 1;
            float ph = strtof(d + 1, &endp); bool ok4 = endp != d + 1;
            if (ok1 && ok2 && ok3 && ok4) {
                cmd.type     = SOskCommand::EType::PANEL;
                cmd.panel[0] = px;
                cmd.panel[1] = py;
                cmd.panel[2] = pw;
                cmd.panel[3] = ph;
                queueCommand(std::move(cmd));
                reply = "ok";
            } else
                reply = "err bad args";
        } else {
            cmd.type     = SOskCommand::EType::PANEL;
            cmd.panel[0] = cmd.panel[1] = cmd.panel[2] = cmd.panel[3] = 0;
            queueCommand(std::move(cmd));
            reply = "ok";
        }
    }
    send_reply(cfd, reply.c_str());
}

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

    /* self-pipe: PLUGIN_EXIT writes a byte so the blocked polls wake instantly
     * (no periodic flag-polling — an idle socket thread sleeps forever) */
    if (pipe(g_wakePipe) == 0) {
        fcntl(g_wakePipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_wakePipe[1], F_SETFL, O_NONBLOCK);
    } else {
        g_wakePipe[0] = g_wakePipe[1] = -1; /* fallback: 200 ms flag-poll */
    }

    int  cfd  = -1;
    char rbuf[MAX_LINE];
    size_t rlen = 0;
    while (g_socketRunning) {
        struct pollfd pfd[2] = {{.fd = sock_fd, .events = POLLIN},
                                {.fd = g_wakePipe[0], .events = POLLIN}};
        int r = poll(pfd, 2, g_wakePipe[0] >= 0 ? -1 : 200);
        if (!g_socketRunning)
            break;
        if (r <= 0)
            continue;
        if (g_wakePipe[0] >= 0 && (pfd[1].revents & POLLIN)) {
            char b;
            while (read(g_wakePipe[0], &b, 1) > 0) {}
        }
        if (!(pfd[0].revents & POLLIN))
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

        struct pollfd cfds[3] = {{.fd = sock_fd, .events = POLLIN},
                                 {.fd = cfd, .events = POLLIN},
                                 {.fd = g_wakePipe[0], .events = POLLIN}};
        while (g_socketRunning) {
            int r2 = poll(cfds, 3, g_wakePipe[0] >= 0 ? -1 : 200);
            if (!g_socketRunning)
                break;
            if (r2 <= 0)
                continue;
            if (g_wakePipe[0] >= 0 && (cfds[2].revents & POLLIN)) {
                char b;
                while (read(g_wakePipe[0], &b, 1) > 0) {}
            }
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
    if (g_wakePipe[0] >= 0)
        close(g_wakePipe[0]);
    if (g_wakePipe[1] >= 0)
        close(g_wakePipe[1]);
    g_wakePipe[0] = g_wakePipe[1] = -1;
    close(sock_fd);
    unlink(path.c_str());
}

/* ---------------- queue drain (main thread) ---------------- */
static SP<CEventLoopTimer> g_drainTimer;
static bool                g_drainPollFallback = false;
static void drainQueue(SP<CEventLoopTimer> self, void *data);

/* socket thread: write the eventfd. The compositor loop's fd callback (main
 * thread) arms the drain timer. Never addTimer/updateTimeout from here. */
static void wakeDrain()
{
    if (g_drainEventFd < 0)
        return;
    uint64_t one = 1;
    ssize_t  r;
    do {
        r = write(g_drainEventFd, &one, sizeof one);
    } while (r < 0 && errno == EINTR);
}

static int onDrainReadable(int fd, uint32_t /*mask*/, void * /*data*/)
{
    uint64_t n;
    while (read(fd, &n, sizeof n) > 0) {}
    if (g_drainTimer)
        g_drainTimer->updateTimeout(std::chrono::milliseconds(0));
    return 0;
}

static void drainQueue(SP<CEventLoopTimer> self, void *data)
{
    g_drain_fires++;
    if (!g_socketRunning) {
        /* shutting down: never re-arm, leave nothing pending */
        if (self)
            self->updateTimeout(std::nullopt);
        return;
    }
    if (g_inDrain) {
        /* nested event dispatch: leave work for the next tick */
        if (self)
            self->updateTimeout(std::chrono::milliseconds(g_drainPollFallback ? 10 : 0));
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
            case SOskCommand::EType::KEY:
                if (g_panelVisible.load(std::memory_order_relaxed))
                    execKey((unsigned)c.a, c.b);
                break;
            case SOskCommand::EType::MOD: {
                if (!g_panelVisible.load(std::memory_order_relaxed))
                    break;
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
                if (g_panelVisible.load(std::memory_order_relaxed))
                    execText(c.text);
                break;
            case SOskCommand::EType::LAYOUT: execLayout(c.text); break;
            case SOskCommand::EType::PMOVE: execPmove(c.a, c.b); break;
            case SOskCommand::EType::PBTN: execPbtn((unsigned)c.a, c.b); break;
            case SOskCommand::EType::FLING:
                fling_tau = std::max(0.05, std::min(2.0, c.a / 1000.0));
                fling_cap = std::max(500.0, std::min(20000.0, (double)c.b));
                DBG("fling: tau=" + std::to_string(fling_tau) + " cap=" + std::to_string(fling_cap));
                break;
            case SOskCommand::EType::POINTER:
                drag_slop_px  = std::max(4.0, std::min(40.0, (double)c.a));
                long_press_ms = std::max(0, std::min(2000, c.b));
                DBG("pointer: slop=" + std::to_string(drag_slop_px) + " long=" + std::to_string(long_press_ms));
                break;
            case SOskCommand::EType::SCROLL:
                scroll_gain = std::max(0.5, std::min(2.0, c.a / 100.0));
                if (c.b >= 0)
                    scroll_axis_px = c.b != 0;
                DBG("scroll gain: " + std::to_string(scroll_gain) +
                    " axispx=" + std::to_string((int)scroll_axis_px));
                break;
            case SOskCommand::EType::SWALLOW:
                touch_swallow = c.a != 0;
                traceGeom(std::string("swallow set: ") + (touch_swallow ? "on" : "off"));
                if (!touch_swallow) {
                    /* mid-gesture toggle: unwind everything cleanly */
                    if (pressed || panel_pressed) {
                        g_pSeatManager->sendPointerButton(nowMs(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                        g_pSeatManager->sendPointerFrame();
                    }
                    pressed = panel_pressed = false;
                    stopFling();
                    releasePinchCtrl();
                    press_pending = false;
                    cancelLongPress();
                    down_flag = up_flag = motion_flag = false;
                    contact_is_panel_native = false;
                    fingers          = 0;
                    ignore_until_zero = false;
                    scroll_mode      = false;
                    pinch_mode       = false;
                    gesture_decided  = false;
                    g_slotPos.clear();
                }
                DBG(std::string("touch swallow: ") + (touch_swallow ? "on" : "off"));
                break;
            case SOskCommand::EType::PANEL:
                panel_nx = c.panel[0];
                panel_ny = c.panel[1];
                panel_nw = c.panel[2];
                panel_nh = c.panel[3];
                panel_rect_valid = (panel_nw > 0 && panel_nh > 0);
                g_panelVisible.store(panel_rect_valid, std::memory_order_release);
                DBG("panel rect (norm): " + std::to_string(panel_nx) + " " + std::to_string(panel_ny) +
                    " " + std::to_string(panel_nw) + " " + std::to_string(panel_nh));
                break;
            }
            g_ringMutex.lock();
        }
    }
    g_inDrain = false; /* CRITICAL: without this the drain stalls forever after the first tick */
    publishStats();
    /* low power: re-arm only while work remains. eventfd delivers the next
     * wakeup; the 10 ms path is only the poll fallback when eventfd failed */
    {
        std::lock_guard<std::mutex> lg(g_ringMutex);
        if (g_ringCount > 0 && g_drainTimer)
            g_drainTimer->updateTimeout(std::chrono::milliseconds(g_drainPollFallback ? 10 : 0));
        else if (g_drainPollFallback && g_drainTimer)
            g_drainTimer->updateTimeout(std::chrono::milliseconds(10));
    }
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

    /* drain timer lives on the main thread for the whole plugin lifetime,
     * disarmed until the eventfd callback (or the 10 ms fallback) arms it */
    g_drainTimer = makeShared<CEventLoopTimer>(
        std::nullopt, [](SP<CEventLoopTimer> self, void *) { drainQueue(self, nullptr); }, nullptr);
    g_pEventLoopManager->addTimer(g_drainTimer);

    g_drainEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_drainEventFd >= 0 && g_pCompositor && g_pCompositor->m_wlEventLoop) {
        g_drainEventSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_drainEventFd,
                                                  WL_EVENT_READABLE, onDrainReadable, nullptr);
    }
    if (!g_drainEventSource) {
        g_drainPollFallback = true;
        g_drainTimer->updateTimeout(std::chrono::milliseconds(10));
        Log::logger->log(Log::WARN, "[hypr-osk] drain eventfd unavailable, polling every 10 ms");
    }

    resolveTouchMonitor(); /* seed the MON snapshot so the first MON need not wait for a touch */
    publishStats();

    g_socketRunning = true;
    traceGeom(std::string("plugin init, swallow=") + (touch_swallow ? "on" : "off"));
    g_socketThread = std::thread(socket_thread_fn, socketPath());

    Log::logger->log(Log::INFO, "[hypr-osk] plugin initialized, socket at " + socketPath());
    return {"hypr-osk", "On-screen keyboard: touch->pointer + keyboard synthesis", "ekollof", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    /* stop the socket thread and join it BEFORE the .so is unmapped: the
     * thread's code lives in this library */
    g_socketRunning = false;
    /* wake its blocked polls instantly (they sleep without a timeout) */
    if (g_wakePipe[1] >= 0) {
        char b = 1;
        ssize_t r;
        do {
            r = write(g_wakePipe[1], &b, 1);
        } while (r < 0 && errno == EINTR);
    }
    if (g_socketThread.joinable())
        g_socketThread.join();
    if (g_drainEventSource) {
        wl_event_source_remove(g_drainEventSource);
        g_drainEventSource = nullptr;
    }
    if (g_drainEventFd >= 0) {
        close(g_drainEventFd);
        g_drainEventFd = -1;
    }
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
    if (g_pressTimer) {
        g_pressTimer->cancel();
        g_pEventLoopManager->removeTimer(g_pressTimer);
        g_pressTimer.reset();
    }
    if (g_flingTimer) {
        g_flingTimer->cancel();
        g_pEventLoopManager->removeTimer(g_flingTimer);
        g_flingTimer.reset();
    }
    fling_active = false;
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
