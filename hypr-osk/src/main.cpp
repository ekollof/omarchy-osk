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
 *   PMOVE / PBTN                 always "err pointer disabled" (rejected
 *                                at the socket; never queued)
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
 *   GAMEPAD <on|off|toggle|query> runtime enable for the pad reader
 *                                (pinned-shell only; query answers
 *                                "pad <e> <a>" inline, sets are "ok" + an
 *                                async push). Default is on: a plugged-in
 *                                matching controller works immediately;
 *                                GAMEPAD off is the kill switch.
 *   PADMAP pointer=… scroll=…     analog sources (allowlisted tokens)
 *   PADBTN <d|o> a=enter,…        desktop/osk button map (allowlisted)
 *   Unsolicited pushes (never replies): `grid <json>`, `mon <…>`,
 *   `nav <action> <1|0>`, `pad <enabled01> <active01>`, `toggle`.
 *
 * Access control: the socket can type into the focused session. A well-known
 * path plus "this pid is packaged quickshell and mapped ekollof-osk" is not
 * identity — any same-uid process can exec /usr/bin/quickshell and choose
 * that namespace. Instead the compositor pins one intended shell instance:
 * the Wayland client that already owns the session's omarchy-bar layer
 * (a property of the installed omarchy-shell, not of this plugin). That
 * pin is a pidfd (stable across pid reuse). The unix
 * connection from that process is the instance-bound capability: when a pin
 * is live, accept() refuses any other peer. TEXT/KEY/MOD additionally
 * require that same instance to own a mapped ekollof-osk layer (visibility,
 * not authority). Sends are non-blocking.
 *
 * Commands are queued from the socket thread and executed on the
 * compositor main thread via an EventLoop timer.
 *
 * Gamepad: on by default so a plugged-in controller works without a
 * settings round-trip. The reader starts at plugin load (HYPR_OSK_GAMEPAD=0
 * disables at load). Valve 2026 Steam Controller hidraw (VID 28de, PID
 * 1302/1304, report 0x42) is preferred; otherwise a standard evdev gamepad
 * (BTN_GAMEPAD, including Bluetooth DualSense/Xbox/8BitDo) is used. Mapping
 * is allowlisted PADMAP/PADBTN from the pinned shell (osk.json gamepadMap).
 * Socket GAMEPAD is never an injection path: it only arms or parks the
 * hidraw reader. GAMEPAD off from the pinned shell is the kill switch.
 * While a focused window is gamescope, or exclusive-fullscreen and not a
 * browser/media player, the reader closes hidraw/evdev so the game is
 * not sharing the node (gamescope lag). Fullscreen YouTube keeps the pad.
 * The OSK layer taking focus reopens hidraw. Lizard-mode mouse/keyboard
 * nodes stay EVIOCGRABbed while a game is focused (even if GAMEPAD is off)
 * so Hyprland does not see Puck Mouse events on top of the game.
 * A reader thread then opens the pad's hidraw node (VID 28de, PID 1302/1304
 * parsed from HID_ID=, not a uevent substring), parses report 0x42 (wire
 * layout from s3govesus/steam-controller-x crates/sc-protocol/src/report.rs)
 * and drives the same in-compositor primitives as a local USB keyboard /
 * touchpad: right pad → relative cursor motion, right-pad click / right
 * trigger → left button, left pad click / left trigger → right button,
 * left stick → scroll, allowlisted face buttons/D-pad → keys, GUIDE →
 * socket `toggle`, START → SUPER+SPACE then force-release mods. Pad
 * commands are stamped pid=0 so a socket client cannot forge them;
 * enable/disable still requires the pinned shell. The reader never calls
 * compositor APIs or writes the client socket (ring + coalesced
 * PADWAKE/PADSTATE, same discipline as the socket thread).
 * Steam-like typing: while the OSK panel is visible the reader routes
 * D-pad / A / X / Y / B and the left stick to unsolicited `nav <action>
 * <1|0>` socket lines (action = up|down|left|right|commit|back|space|close)
 * instead of desktop keys/scroll, and the QML owns the highlight, repeat
 * and commit path. Hidden panel = desktop behavior, unchanged.
 * Caveats: hidraw has no exclusive open — a running Steam client reads the
 * same reports and acts on its own (mis)parse in parallel, so quit Steam
 * (or silence its desktop config) while testing. The kernel's
 * hid-generic phantom mouse/keyboard nodes for the pad are EVIOCGRABbed
 * while gamepad mode holds the device so lizard-mode events don't double
 * with the synthetic ones. Sign conventions (pad Y, stick rest) are
 * calibrated live — see STATS padbtn/padraw. Tune motion with
 * HYPR_OSK_PAD_GAIN (default 1.0).
 */
/* Pull signal deps with normal access, then re-open CSignalBase so PLUGIN_EXIT
 * can unregister listenStatic handlers (they run after every regular plugin
 * listener). */
#include <functional>
#include <any>
#include <type_traits>
#include <utility>
#include <vector>
#include <memory>
#include <tuple>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/signal/Listener.hpp>
#define private public
#define protected public
#include <hyprutils/signal/Signal.hpp>
#undef private
#undef protected

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <set>
#include <hyprland/src/event/EventBus.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <cerrno>
#include <sys/eventfd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef PIDFD_NONBLOCK
#define PIDFD_NONBLOCK O_NONBLOCK
#endif

/* glibc's pidfd_open is not reliably extern "C" for a C++ plugin .so
 * (Hyprland died on `_Z10pidfd_openij`). Go through the syscalls. */
static int oskPidfdOpen(pid_t pid, unsigned flags)
{
    return (int)syscall(SYS_pidfd_open, pid, flags);
}
static int oskPidfdSendSignal(int fd, int sig)
{
    return (int)syscall(SYS_pidfd_send_signal, fd, sig, (void *)nullptr, 0u);
}
#include <linux/limits.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/ioctl.h>

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
    enum class EType : uint8_t { KEY, MOD, MODS, TEXT, LAYOUT, FLING, POINTER, SCROLL, SWALLOW, PANEL,
                                 PADKEY, PADPTR, PADCHORD, PADWAKE, PADNAV, PADSTATE, PADMAP, MONREFRESH, GAMEPAD } type;
    int   a = 0, b = 0;
    pid_t pid           = 0; /* SO_PEERCRED pid stamped at queue time */
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
static std::atomic<int>  g_listenFd{-1};
static pid_t             g_panelPid = 0; /* peer pid that last published a PANEL rect */
static std::mutex        g_shellIdentMutex;
static int               g_shellPidfd = -1; /* compositor-pinned omarchy-shell instance */
static std::atomic<pid_t> g_shellPid{0};
static int               g_wakePipe[2] = {-1, -1}; /* self-pipe: wakes the socket thread's polls */
static int               g_drainEventFd = -1;      /* eventfd: socket thread → compositor loop */
static wl_event_source  *g_drainEventSource = nullptr;
/* gamepad prototype state (defined in the gamepad section below; declared
 * here because publishStats/STATS read them) */
static std::atomic<bool> g_padActive{false};
static std::atomic<uint32_t> g_padButtons{0};
static std::atomic<bool> g_padEnabled{true}; /* auto: inject once a matching pad streams */
static std::atomic<bool> g_padYielded{false}; /* fullscreen / gamescope owns the pad */
struct PadMap {
    uint8_t pointer = 2 | 4; /* PAN_RS|PAN_RPAD; values match the gamepad section */
    uint8_t scroll  = 1;     /* PAN_LS */
    uint8_t desktop[19]{};
    uint8_t osk[19]{};
};
static std::mutex g_padMapMutex;
static PadMap     g_padMapLive;
static PadMap     g_padMapIncoming;
static std::atomic<uint32_t> g_padMapGen{1};
static bool padParseMapLine(const char *line, PadMap *m);
static bool padParseBtnLine(const char *line, PadMap *m);

static void wakeDrain();

static void ensurePadThread(); /* defined at the gamepad section; drain calls it */
static void padPushState();    /* enabled + active snapshot; atomics + sendToClient only */

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

static bool sendAllDontWait(int fd, const char *data, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = send(fd, data + off, n - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        off += (size_t)r;
    }
    return true;
}

static void dropClientFd(int fd)
{
    if (fd < 0)
        return;
    shutdown(fd, SHUT_RDWR);
}

static void sendToClient(const std::string &line)
{
    std::lock_guard<std::mutex> lg(g_clientMutex);
    if (g_clientFd < 0)
        return;
    std::string out = line + "\n";
    if (!sendAllDontWait(g_clientFd, out.c_str(), out.size()))
        dropClientFd(g_clientFd); /* wake the socket thread; it owns close() */
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
static Hyprutils::Signal::CHyprSignalListener g_padWinActive;
static Hyprutils::Signal::CHyprSignalListener g_padWinFs;
static Hyprutils::Signal::CHyprSignalListener g_padWinClass;
static Hyprutils::Signal::CHyprSignalListener g_padWinTitle;
static Hyprutils::Signal::CHyprSignalListener g_padWinClose;
static Hyprutils::Signal::CHyprSignalListener g_padWinOpen;
static Hyprutils::Signal::CHyprSignalListener g_padWinFloat;
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

static void releasePinchCtrl();

/* Drop leftover Super/Shift/Ctrl/Alt on the virtual keyboard. GUIDE used
 * to inject SUPER+SHIFT+K to toggle the OSK and those mods could stick,
 * so later typing looked like a held logo/ctrl key. */
static void releaseInjectedMods()
{
    static const unsigned mods[] = {KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTMETA, KEY_RIGHTALT};
    for (unsigned k : mods) {
        if (g_pressedKeys.count(k))
            execKey(k, 0);
    }
    held_mods = 0;
    sent_mods = 0;
    releasePinchCtrl();
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
    int      pad = 0;
    int      paden = 0;
    uint32_t padbtn = 0;
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
    s.pad         = (int)g_padActive.load(std::memory_order_relaxed);
    s.paden       = (int)g_padEnabled.load(std::memory_order_relaxed);
    s.padbtn      = g_padButtons.load(std::memory_order_relaxed);
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
static bool layerAllowsInject(pid_t pid);
static void refreshIntendedShell();
static bool intendedShellAllows(pid_t pid);
static void bindTouchHooks();

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
    /* Touch docking wins while a touch device is bound (tap coordinates are
     * normalized against that device's frame, so the panel rect must live in
     * the same frame). Otherwise the OSK follows the pointer — the screen
     * the controller/mouse user is actually looking at. */
    auto mon = State::monitorState()
                   ->query()
                   .name(!touchDeviceOutput.empty() ? touchDeviceOutput : "")
                   .run();
    if (!mon && g_pInputManager)
        mon = State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run();
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

/* main thread only: refresh the MON snapshot and push it to the client
 * (the QML re-docks + re-PANELs on a changed monitor name) */
static std::string g_monPushedFor;
static void pushMon()
{
    SP<Monitor::CMonitor> mon = resolveTouchMonitor();
    SMonSnap snap{};
    {
        std::lock_guard<std::mutex> lg(g_monMutex);
        snap = g_monSnap;
    }
    if (mon && snap.valid) {
        char buf[160];
        snprintf(buf, sizeof buf, "mon %s %d %d %d %d", snap.name, snap.x, snap.y, snap.w, snap.h);
        sendToClient(buf);
        g_monPushedFor = touchDeviceOutput;
    }
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
    const char *rtd = getenv("XDG_RUNTIME_DIR");
    if (!rtd || rtd[0] != '/')
        return; /* never fall back to shared /tmp */
    std::string path = std::string(rtd) + "/hypr-osk-geom.log";
    std::ofstream f(path, std::ios::app);
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

/* Drop a native wl_touch sequence that leaked past the bus (another plugin
 * can assign cancelled=false after us). Seat cancel + clear Hyprland's touch
 * focus so later motion/up have nothing to deliver. Pointer emulation is
 * the only stream while swallowing. */
static void dropNativeTouch()
{
    if (!g_pSeatManager || !g_pInputManager)
        return;
    g_pSeatManager->sendTouchCancel();
    g_pInputManager->m_touchData.touchFocusLockSurface.reset();
    g_pInputManager->m_touchData.touchFocusWindow.reset();
    g_pInputManager->m_touchData.touchFocusLS.reset();
    g_pInputManager->m_touchData.touchFocusSurface.reset();
}

/* ---------------- deferred application (idle phase, main thread) ---------- */
static void applyTouches()
{
    if (touch_swallow)
        dropNativeTouch();
    panel_rect_valid = (panel_nw > 0 && panel_nh > 0 && layerAllowsInject(g_panelPid));
    g_panelVisible.store(panel_rect_valid, std::memory_order_release);

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
    if (touchDeviceOutput != g_monPushedFor) {
        /* touch bound (or re-bound) a device: re-dock the panel to its
         * frame so taps land — the QML re-PANELs on a changed mon name */
        pushMon();
    }
    publishStats();
}


/* ---------------- socket protocol ---------------- */
static bool send_reply(int cfd, const char *msg)
{
    std::string out = std::string(msg) + "\n";
    return sendAllDontWait(cfd, out.c_str(), out.size());
}

/* Main thread only. Walk each monitor's layer lists (what hyprctl layers uses).
 * Desktop::layerState is not the compositor's mapped-layer set. */
static void forEachMappedLayer(auto &&fn)
{
    if (auto &ms = State::monitorState(); ms) {
        for (const auto &mon : ms->monitors()) {
            if (!mon)
                continue;
            for (auto &vec : mon->m_layerSurfaceLayers) {
                for (auto &ref : vec) {
                    if (auto ls = ref.lock()) {
                        if (ls->m_mapped)
                            fn(ls);
                    }
                }
            }
        }
    }
    if (auto &st = Desktop::layerState(); st) {
        for (const auto &ls : st->layers()) {
            if (ls && ls->m_mapped)
                fn(ls);
        }
    }
    if (auto &vs = Desktop::viewState(); vs) {
        for (const auto &ls : vs->layers()) {
            if (ls && ls->m_mapped)
                fn(ls);
        }
    }
}

static bool pidOwnsMappedNs(pid_t pid, const char *ns)
{
    if (pid <= 0 || !ns)
        return false;
    bool found = false;
    forEachMappedLayer([&](const PHLLS &ls) {
        if (!found && ls->m_namespace == ns && ls->getPID() == pid)
            found = true;
    });
    return found;
}

static bool pidfdAlive(int fd)
{
    if (fd < 0)
        return false;
    return oskPidfdSendSignal(fd, 0) == 0;
}

static bool pidfdsSameProcess(int a, int b)
{
    struct stat sa = {}, sb = {};
    if (a < 0 || b < 0 || fstat(a, &sa) != 0 || fstat(b, &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void clearIntendedShell()
{
    std::lock_guard<std::mutex> lg(g_shellIdentMutex);
    if (g_shellPidfd >= 0)
        close(g_shellPidfd);
    g_shellPidfd = -1;
    g_shellPid.store(0, std::memory_order_release);
}

static void setIntendedShell(pid_t pid)
{
    if (pid <= 0) {
        clearIntendedShell();
        return;
    }
    int fd = oskPidfdOpen(pid, PIDFD_NONBLOCK);
    if (fd < 0) {
        clearIntendedShell();
        return;
    }
    {
        std::lock_guard<std::mutex> lg(g_shellIdentMutex);
        if (g_shellPidfd >= 0)
            close(g_shellPidfd);
        g_shellPidfd = fd;
        g_shellPid.store(pid, std::memory_order_release);
    }
    DBG("intended shell instance pinned pid=" + std::to_string(pid));
}

/* Main thread only. Pin the installed omarchy-shell Wayland client — the
 * process that already owns omarchy-bar — and keep that pin until its
 * pidfd dies. A later process that maps the same namespace cannot steal
 * the capability while the session shell is alive. Prefer a pid that also
 * owns omarchy-background when several bars exist. */
static void refreshIntendedShell()
{
    pid_t cur = g_shellPid.load(std::memory_order_acquire);
    bool  keep = false;
    {
        std::lock_guard<std::mutex> lg(g_shellIdentMutex);
        keep = pidfdAlive(g_shellPidfd);
    }
    if (keep && cur > 0 && pidOwnsMappedNs(cur, "omarchy-bar"))
        return;
    clearIntendedShell();

    std::set<pid_t> bar, bg;
    forEachMappedLayer([&](const PHLLS &ls) {
        pid_t p = ls->getPID();
        if (p <= 0)
            return;
        if (ls->m_namespace == "omarchy-bar")
            bar.insert(p);
        else if (ls->m_namespace == "omarchy-background")
            bg.insert(p);
    });

    pid_t chosen = 0;
    for (pid_t p : bar) {
        if (bg.count(p)) {
            chosen = p;
            break;
        }
        if (!chosen)
            chosen = p;
    }
    if (chosen)
        setIntendedShell(chosen);
}

static bool intendedShellAllows(pid_t pid)
{
    refreshIntendedShell();
    pid_t pinned = g_shellPid.load(std::memory_order_acquire);
    if (pid <= 0 || pinned <= 0 || pid != pinned)
        return false;
    std::lock_guard<std::mutex> lg(g_shellIdentMutex);
    return pidfdAlive(g_shellPidfd);
}

/* Visibility of the OSK panel, not identity: the pinned shell instance must
 * own the mapped ekollof-osk layer. */
static bool layerAllowsInject(pid_t pid)
{
    return intendedShellAllows(pid) && pidOwnsMappedNs(pid, "ekollof-osk");
}

static pid_t credPid(int cfd)
{
    struct ucred cred = {};
    socklen_t clen = sizeof cred;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0)
        return 0;
    return cred.pid;
}

static void queueFromPeer(int cfd, SOskCommand cmd)
{
    cmd.pid = credPid(cfd);
    queueCommand(std::move(cmd));
}

/* ---------------- peer validation ----------------
 * First filter: SO_PEERCRED uid + packaged /usr/bin/quickshell (realpath,
 * root-owned, not group/world-writable). That is not identity.
 * Capability: when the compositor has pinned the session shell pidfd,
 * the peer must be that same process (pidfd inode, not a recycled pid).
 * While unpinned (shell restarting), packaged quickshell may connect so
 * the main thread can observe the new omarchy-bar and pin it. */
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

static int openPeerPidfd(int cfd, pid_t pid)
{
    int pfd = -1;
    socklen_t plen = sizeof pfd;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERPIDFD, &pfd, &plen) == 0 && pfd >= 0)
        return pfd;
    if (pid <= 0)
        return -1;
    return oskPidfdOpen(pid, PIDFD_NONBLOCK);
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

    int peerfd = openPeerPidfd(cfd, cred.pid);
    if (peerfd < 0) {
        DBG("peer check: no pidfd for pid " + std::to_string(cred.pid));
        return false;
    }
    bool ok = false;
    bool pinned = false;
    {
        std::lock_guard<std::mutex> lg(g_shellIdentMutex);
        pinned = pidfdAlive(g_shellPidfd);
        if (pinned)
            ok = pidfdsSameProcess(g_shellPidfd, peerfd);
        else
            ok = true; /* unpinned: main thread will pin the session shell */
    }
    close(peerfd);
    if (pinned && !ok)
        DBG("peer check: pid " + std::to_string(cred.pid) + " is not the pinned shell instance");
    return ok;
}

static bool handle_line(int cfd, char *line)
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
        cmd.type = SOskCommand::EType::KEY;
        if (sscanf(line + 4, "%u %d", &cmd.a, &cmd.b) == 2) {
            queueFromPeer(cfd, std::move(cmd));
            reply = hidden() ? "err hidden" : "ok";
        } else
            reply = "err bad args";
    } else if (!strncmp(line, "MOD ", 4)) {
        char name[16], state[8];
        if (sscanf(line + 4, "%15s %7s", name, state) != 2) {
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
                queueFromPeer(cfd, std::move(cmd));
                reply = "ok";
            }
        }
    } else if (!strncmp(line, "MODS ", 5)) {
        if (strcmp(line + 5, "off") != 0) {
            reply = "err bad args";
        } else {
            cmd.type = SOskCommand::EType::MODS;
            queueFromPeer(cfd, std::move(cmd));
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
            queueFromPeer(cfd, std::move(cmd));
            reply = "ok";
        }
    } else if (!strcmp(line, "ROWS")) {
        /* cached grid, rebuilt on the main thread with every keymap change;
         * no keymap access from this thread */
        std::lock_guard<std::mutex> lg(g_gridMutex);
        reply = "grid " + (g_gridJson.empty() ? std::string("{\"rows\":[[],[],[],[]]}") : g_gridJson);
    } else if (!strncmp(line, "TEXT ", 5)) {
        cmd.type = SOskCommand::EType::TEXT;
        size_t n = strlen(line + 5);
        if (n > TEXT_CAP - 1)
            n = TEXT_CAP - 1;
        memcpy(cmd.text, line + 5, n);
        cmd.text[n] = 0;
        queueFromPeer(cfd, std::move(cmd));
        reply = hidden() ? "err hidden" : "ok";
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
            queueFromPeer(cfd, std::move(cmd));
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
            queueFromPeer(cfd, std::move(cmd));
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
            queueFromPeer(cfd, std::move(cmd));
            reply = "ok";
        } else
            reply = "err bad args";
    } else if (!strncmp(line, "SWALLOW ", 8)) {
        /* SWALLOW <0|1> — consume touchscreen input (virtual pointer) or
         * hand it to Hyprland's native touchscreen support */
        if (!strcmp(line + 8, "0") || !strcmp(line + 8, "1")) {
            cmd.type = SOskCommand::EType::SWALLOW;
            cmd.a    = line[8] - '0';
            queueFromPeer(cfd, std::move(cmd));
            reply = "ok";
        } else
            reply = "err bad args";
    } else if (!strcmp(line, "MON")) {
        /* MON — answered deterministically: refresh on the main thread and
         * push `mon ...` (never serve the possibly-stale snapshot here).
         * The direct reply is just "ok"; the QML handles the push. */
        SOskCommand mc;
        mc.type = SOskCommand::EType::MONREFRESH;
        mc.pid  = 0;
        queueCommand(mc);
        reply = "ok";
    } else if (!strncmp(line, "GAMEPAD", 7)) {
        /* GAMEPAD on|off|toggle|query — runtime enable for the pad reader.
         * query answers synchronously from atomics; sets queue + reply ok,
         * state follows as an unsolicited `pad <enabled> <active>` push. */
        const char *arg = line + 7;
        while (*arg == ' ')
            arg++;
        if (!strcmp(arg, "query")) {
            char buf[32];
            snprintf(buf, sizeof buf, "pad %d %d", (int)g_padEnabled.load(std::memory_order_relaxed),
                     (int)g_padActive.load(std::memory_order_relaxed));
            reply = buf;
        } else if (!strcmp(arg, "on") || !strcmp(arg, "off") || !strcmp(arg, "toggle")) {
            cmd.type = SOskCommand::EType::GAMEPAD;
            cmd.a    = !strcmp(arg, "toggle") ? 2 : (!strcmp(arg, "on") ? 1 : 0);
            queueFromPeer(cfd, std::move(cmd));
            reply = "ok";
        } else
            reply = "err bad args";
    } else if (!strncmp(line, "PADMAP ", 7)) {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lg(g_padMapMutex);
            ok = padParseMapLine(line + 7, &g_padMapIncoming);
        }
        if (!ok)
            reply = "err bad map";
        else {
            cmd.type = SOskCommand::EType::PADMAP;
            queueFromPeer(cfd, std::move(cmd));
            reply = "ok";
        }
    } else if (!strncmp(line, "PADBTN ", 7)) {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lg(g_padMapMutex);
            ok = padParseBtnLine(line + 7, &g_padMapIncoming);
        }
        if (!ok)
            reply = "err bad map";
        else {
            cmd.type = SOskCommand::EType::PADMAP;
            queueFromPeer(cfd, std::move(cmd));
            reply = "ok";
        }
    } else if (!strcmp(line, "STATS")) {
        SStatsSnap s;
        {
            std::lock_guard<std::mutex> lg(g_statsMutex);
            s = g_statsSnap;
        }
        char buf[384];
        snprintf(buf, sizeof buf,
                 "state fingers=%d pressed=%d ignore=%d scroll=%d down=%d up=%d contact_osk=%d "
                 "panel_valid=%d inject=%d anypeer=%d panel_ny=%.3f panel_nh=%.3f last=%.3f,%.3f "
                 "fires=%u ring=%zu indrain=%d layout=%s textmap=%zu fling=%.0fms/%.0fpx swallow=%d "
                 "pad=%d padbtn=%06x paden=%d",
                  s.fingers, s.pressed, s.ignore, s.scroll, s.down, s.up, s.contact, s.panel_valid,
                  s.inject, s.anypeer, s.panel_ny, s.panel_nh, s.lastx, s.lasty, s.fires,
                  s.ring, s.indrain, s.layout, s.textmap, s.fling_tau_ms, s.fling_cap, s.swallow,
                  s.pad, s.padbtn, s.paden);
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
                queueFromPeer(cfd, std::move(cmd));
                reply = "ok";
            } else
                reply = "err bad args";
        } else {
            cmd.type     = SOskCommand::EType::PANEL;
            cmd.panel[0] = cmd.panel[1] = cmd.panel[2] = cmd.panel[3] = 0;
            queueFromPeer(cfd, std::move(cmd));
            reply = "ok";
        }
    }
    return send_reply(cfd, reply.c_str());
}

static void socket_thread_fn(std::string path)
{
    unlink(path.c_str());
    int sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
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
        close(sock_fd);
        g_socketRunning = false;
        return;
    }
    g_listenFd.store(sock_fd, std::memory_order_release);
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
        int nfd = accept4(sock_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
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
        padPushState(); /* and the pad state: the reader holds the device
                           across client reconnects with no new transition to push */

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
            bool replaced = false;
            if (cfds[0].revents & POLLIN) {
                int newer = accept4(sock_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (newer >= 0) {
                    if (!peerAllowed(newer)) {
                        send_reply(newer, "err unauthorized");
                        close(newer);
                    } else {
                        send_reply(cfd, "err replaced");
                        {
                            std::lock_guard<std::mutex> lg(g_clientMutex);
                            if (g_clientFd == cfd)
                                g_clientFd = -1;
                        }
                        close(cfd);
                        cfd            = newer;
                        cfds[1].fd     = cfd;
                        cfds[1].revents = 0;
                        rlen           = 0;
                        replaced       = true;
                        {
                            std::lock_guard<std::mutex> lg(g_clientMutex);
                            g_clientFd = newer;
                        }
                    }
                }
            }
            if (!replaced && (cfds[1].revents & (POLLIN | POLLHUP))) {
                ssize_t n = read(cfd, rbuf + rlen, MAX_LINE - 1 - rlen);
                if (n < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                        continue;
                    DBG("client gone");
                    break;
                }
                if (n == 0) {
                    DBG("client gone");
                    break;
                }
                rlen += (size_t)n;
                rbuf[rlen] = 0;
                char *nl;
                bool drop = false;
                while ((nl = strchr(rbuf, '\n'))) {
                    *nl = 0;
                    if (!handle_line(cfd, rbuf)) {
                        drop = true;
                        break;
                    }
                    size_t consumed = (size_t)(nl - rbuf) + 1;
                    memmove(rbuf, rbuf + consumed, rlen - consumed + 1);
                    rlen -= consumed;
                }
                if (drop) {
                    DBG("client dropped (send would block or peer gone)");
                    break;
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
    g_listenFd.store(-1, std::memory_order_release);
    close(sock_fd);
    unlink(path.c_str());
}

/* ---------------- gamepad (Steam Controller 2026 prototype) ----------------
 * Reader thread, main-thread application. The reader NEVER calls compositor
 * APIs: button edges go through the command ring (wakeDrain), motion/scroll
 * accumulate in atomics and are consumed by a single coalesced PADWAKE per
 * drain (270 Hz reports must not flood the 64-slot ring). Teardown mirrors
 * the socket thread: flag + wake pipe → join in PLUGIN_EXIT before unmap;
 * no event-loop timers of its own, so nothing extra to remove there. */
static std::thread       g_padThread;
static std::atomic<bool> g_padRunning{false};
/* g_padActive/g_padButtons are declared near the top (STATS reads them) */
static int               g_padPipe[2] = {-1, -1};
static std::atomic<double> g_padDX{0}, g_padDY{0}, g_padSX{0}, g_padSY{0};
static std::atomic<bool> g_padWakePending{false};
static double            g_padGain = 1.0;
/* stick rest centers persist across reopens; only still windows adopt */
static double padRestLX = 0, padRestLY = 0, padRestRX = 0, padRestRY = 0;

#ifndef EVIOCGNAME_256
#define EVIOCGNAME_256 0x80804506
#endif
#ifndef EVIOCGRAB
#define EVIOCGRAB 0x40044590
#endif

/* report 0x42 offsets (s3govesus/steam-controller-x sc-protocol report.rs) */
#define PAD_LEN_MIN 46
#define PAD_B0 2
#define PAD_STATUS 5
#define PAD_LTRIG 6
#define PAD_RTRIG 8
#define PAD_LSTICK_X 10
#define PAD_LSTICK_Y 12
#define PAD_RSTICK_X 14
#define PAD_RSTICK_Y 16
#define PAD_RPAD_X 24
#define PAD_RPAD_Y 26
/* button bits over bytes 2..4 */
#define PB_A (1u << 0)
#define PB_B (1u << 1)
#define PB_X (1u << 2)
#define PB_Y (1u << 3)
#define PB_START (1u << 6)
#define PB_RT_LOWER (1u << 8)
#define PB_DPAD_DOWN (1u << 10)
#define PB_DPAD_RIGHT (1u << 11)
#define PB_DPAD_LEFT (1u << 12)
#define PB_DPAD_UP (1u << 13)
#define PB_BACK (1u << 14)
#define PB_GUIDE (1u << 16)
#define PB_RPAD_TOUCH (1u << 21)
#define PB_RPAD_CLICK (1u << 22)
/* NB: raw byte4 bit 0x80 (mask position 1u<<23) is the trigger bottom-out
 * click, NOT a pad button — it must be masked out of the button word (a
 * full RT pull sets it, which used to alias to a right-click). The real
 * left-pad click arrives via the STATUS byte (offset 5) bit 0x04. */
#define PB_LPAD_CLICK_STATUS 0x04

static void padQueue(SOskCommand::EType t, int a, int b)
{
    SOskCommand c;
    c.type = t;
    c.a    = a;
    c.b    = b;
    c.pid  = 0; /* hardware path: never a socket peer; drain must not treat this as fromShell */
    queueCommand(c);
}

static void padWake()
{
    /* coalesce: at most one PADWAKE in flight; the drain consumes whatever
     * accumulated since the last one */
    if (!g_padWakePending.exchange(true))
        padQueue(SOskCommand::EType::PADWAKE, 0, 0);
}

static bool padHidIdMatch(const std::string &txt)
{
    /* HID_ID=0003:000028DE:00001304 — vendor/product fields only, not a
     * substring match anywhere in uevent (HID_NAME, PHYS, UNIQ, …). */
    const char *p = txt.c_str();
    while ((p = strstr(p, "HID_ID=")) != nullptr) {
        p += 7;
        unsigned bus = 0, vid = 0, pid = 0;
        if (sscanf(p, "%x:%x:%x", &bus, &vid, &pid) == 3 &&
            vid == 0x28de && (pid == 0x1304 || pid == 0x1302))
            return true;
    }
    return false;
}

static int padOpenStreamer()
{
    /* find hidraw nodes for the pad (VID 28de, wired 1302 / puck 1304) and
     * keep the first interface that actually streams report 0x42 — over the
     * puck only the paired/awake channel talks, the rest stay silent */
    for (int i = 0; i < 32; i++) {
        char node[64], uevent[128];
        snprintf(node, sizeof node, "/dev/hidraw%d", i);
        snprintf(uevent, sizeof uevent, "/sys/class/hidraw/hidraw%d/device/uevent", i);
        std::ifstream f(uevent);
        if (!f.good())
            continue;
        std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!padHidIdMatch(txt))
            continue;
        int fd = open(node, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        struct pollfd pfd{fd, POLLIN, 0};
        bool streams = false;
        if (poll(&pfd, 1, 300) > 0 && (pfd.revents & POLLIN)) {
            unsigned char probe[64];
            ssize_t n = read(fd, probe, sizeof probe);
            if (n >= 2 && probe[0] == 0x42)
                streams = true;
            /* drain any backlog from the probe window */
            while (n > 0) {
                n = read(fd, probe, sizeof probe);
            }
        }
        if (streams)
            return fd;
        close(fd);
    }
    return -1;
}

static bool padPhantomName(const char *name)
{
    /* Lizard-mode mouse/keyboard nodes only — not the hid-steam gamepad
     * node named "Steam Controller", which we may read as evdev. */
    return strstr(name, "Puck Mouse") || strstr(name, "Puck Keyboard") ||
           strstr(name, "Puck Consumer") || strstr(name, "Puck System") ||
           strstr(name, "Steam Controller Mouse") || strstr(name, "Steam Controller Keyboard");
}

static void padGrabPhantoms(int *held, int *nheld)
{
    /* EVIOCGRAB the kernel hid-generic phantom mouse/keyboard nodes so
     * lizard-mode events don't double with the synthetic ones. Best effort:
     * name-probe with O_RDONLY, grab only matching nodes; released on
     * thread exit. */
    *nheld = 0;
    for (int i = 0; i < 64 && *nheld < 16; i++) {
        char node[64];
        snprintf(node, sizeof node, "/dev/input/event%d", i);
        int fd = open(node, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME_256, name) < 0 || !padPhantomName(name)) {
            close(fd);
            continue;
        }
        int wr = open(node, O_RDWR | O_NONBLOCK);
        close(fd);
        if (wr < 0)
            continue;
        if (ioctl(wr, EVIOCGRAB, (void *)1) == 0)
            held[(*nheld)++] = wr;
        else
            close(wr);
    }
}

static void padUngrabPhantoms(int *held, int *nheld)
{
    for (int i = 0; i < *nheld; i++) {
        ioctl(held[i], EVIOCGRAB, (void *)0);
        close(held[i]);
    }
    *nheld = 0;
}

static int16_t padI16(const unsigned char *r, int off)
{
    return (int16_t)((uint16_t)r[off] | ((uint16_t)r[off + 1] << 8));
}

static uint16_t padU16(const unsigned char *r, int off)
{
    return (uint16_t)r[off] | ((uint16_t)r[off + 1] << 8);
}

/* ---- logical controls / allowlisted map (PADMAP + PADBTN) ---- */
enum {
    PC_A = 0, PC_B, PC_X, PC_Y,
    PC_DUP, PC_DDN, PC_DLT, PC_DRT,
    PC_LB, PC_RB, PC_LT, PC_RT,
    PC_SELECT, PC_START, PC_GUIDE,
    PC_LSCLICK, PC_RSCLICK,
    PC_LPCLICK, PC_RPCLICK,
    PC_COUNT
};
enum {
    PA_NONE = 0,
    PA_ENTER, PA_ESC, PA_BACK, PA_SPACE, PA_TAB,
    PA_UP, PA_DOWN, PA_LEFT, PA_RIGHT,
    PA_MENU, PA_TOGGLE,
    PA_COMMIT, PA_CLOSE,
    PA_NAVUP, PA_NAVDN, PA_NAVLT, PA_NAVRT,
    PA_LCLICK, PA_RCLICK
};
enum { PAN_LS = 1, PAN_RS = 2, PAN_RPAD = 4 };

struct PadView {
    uint32_t btn = 0;
    double   lx = 0, ly = 0, rx = 0, ry = 0;
    double   lt = -1, rt = -1; /* 0..1, <0 = absent */
    double   rpadDx = 0, rpadDy = 0;
};
struct PadApplyState {
    uint32_t lastBtn     = 0;
    bool     lastLT      = false, lastRT = false;
    bool     lastLclick  = false, lastRclick = false;
    bool     lastNavMode = false;
    int      lastNavDirs = 0;
    uint32_t lastMs      = 0;
    uint32_t mapGen      = 0;
    PadMap   map{};
    std::set<unsigned> padKeys;
};

static inline double padSq(double x, double y)
{
    return x * x + y * y;
}

static void padMapDefault(PadMap *m)
{
    *m = PadMap{};
    auto D = m->desktop, O = m->osk;
    D[PC_A] = PA_ENTER;     O[PC_A] = PA_COMMIT;
    D[PC_B] = PA_ESC;       O[PC_B] = PA_CLOSE;
    D[PC_X] = PA_BACK;      O[PC_X] = PA_BACK;
    D[PC_Y] = PA_SPACE;     O[PC_Y] = PA_SPACE;
    D[PC_DUP] = PA_UP;      O[PC_DUP] = PA_NAVUP;
    D[PC_DDN] = PA_DOWN;    O[PC_DDN] = PA_NAVDN;
    D[PC_DLT] = PA_LEFT;    O[PC_DLT] = PA_NAVLT;
    D[PC_DRT] = PA_RIGHT;   O[PC_DRT] = PA_NAVRT;
    D[PC_SELECT] = PA_TAB;  O[PC_SELECT] = PA_TAB;
    D[PC_START] = PA_MENU;  O[PC_START] = PA_MENU;
    D[PC_GUIDE] = PA_TOGGLE; O[PC_GUIDE] = PA_TOGGLE;
    D[PC_RT] = PA_LCLICK;   O[PC_RT] = PA_LCLICK;
    D[PC_LT] = PA_RCLICK;   O[PC_LT] = PA_RCLICK;
    D[PC_RPCLICK] = PA_LCLICK; O[PC_RPCLICK] = PA_LCLICK;
    D[PC_LPCLICK] = PA_RCLICK; O[PC_LPCLICK] = PA_RCLICK;
}

static int padParseCtrl(const char *s)
{
    static const char *n[] = {"a","b","x","y","dpadUp","dpadDown","dpadLeft","dpadRight",
                              "lb","rb","lt","rt","select","start","guide","lsClick","rsClick",
                              "leftPadClick","rightPadClick"};
    for (int i = 0; i < PC_COUNT; i++)
        if (!strcmp(s, n[i]))
            return i;
    return -1;
}
static int padParseAct(const char *s)
{
    static const struct { const char *n; int v; } t[] = {
        {"none", PA_NONE}, {"enter", PA_ENTER}, {"escape", PA_ESC}, {"backspace", PA_BACK},
        {"space", PA_SPACE}, {"tab", PA_TAB}, {"up", PA_UP}, {"down", PA_DOWN},
        {"left", PA_LEFT}, {"right", PA_RIGHT}, {"menu", PA_MENU}, {"toggleOsk", PA_TOGGLE},
        {"commit", PA_COMMIT}, {"close", PA_CLOSE}, {"navUp", PA_NAVUP}, {"navDown", PA_NAVDN},
        {"navLeft", PA_NAVLT}, {"navRight", PA_NAVRT}, {"leftClick", PA_LCLICK},
        {"rightClick", PA_RCLICK},
    };
    for (auto &e : t)
        if (!strcmp(s, e.n))
            return e.v;
    return -1;
}
static int padParseAnalogBits(const char *csv)
{
    int bits = 0;
    const char *p = csv ? csv : "";
    while (*p) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        const char *e = p;
        while (*e && *e != ',')
            e++;
        char tok[32];
        size_t n = (size_t)(e - p);
        if (n >= sizeof tok)
            return -1;
        memcpy(tok, p, n);
        tok[n] = 0;
        while (n && tok[n - 1] == ' ')
            tok[--n] = 0;
        if (strcmp(tok, "none") && *tok) {
            if (!strcmp(tok, "leftStick"))
                bits |= PAN_LS;
            else if (!strcmp(tok, "rightStick"))
                bits |= PAN_RS;
            else if (!strcmp(tok, "rightPad"))
                bits |= PAN_RPAD;
            else
                return -1;
        }
        p = e;
    }
    return bits;
}

static bool padParseMapLine(const char *line, PadMap *m)
{
    /* PADMAP pointer=rightStick,rightPad scroll=leftStick */
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", line);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
        char *eq = strchr(tok, '=');
        if (!eq)
            return false;
        *eq = 0;
        int bits = padParseAnalogBits(eq + 1);
        if (bits < 0)
            return false;
        if (!strcmp(tok, "pointer"))
            m->pointer = (uint8_t)bits;
        else if (!strcmp(tok, "scroll"))
            m->scroll = (uint8_t)bits;
        else
            return false;
    }
    return true;
}

static bool padParseBtnLine(const char *line, PadMap *m)
{
    /* PADBTN d a=enter,b=escape,...   or  PADBTN o ... */
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", line);
    char *sp = strchr(buf, ' ');
    if (!sp)
        return false;
    *sp = 0;
    uint8_t *dst = nullptr;
    if (!strcmp(buf, "d") || !strcmp(buf, "desktop"))
        dst = m->desktop;
    else if (!strcmp(buf, "o") || !strcmp(buf, "osk"))
        dst = m->osk;
    else
        return false;
    for (char *tok = strtok(sp + 1, ","); tok; tok = strtok(nullptr, ",")) {
        while (*tok == ' ')
            tok++;
        char *eq = strchr(tok, '=');
        if (!eq)
            return false;
        *eq = 0;
        int c = padParseCtrl(tok), a = padParseAct(eq + 1);
        if (c < 0 || a < 0)
            return false;
        dst[c] = (uint8_t)a;
    }
    return true;
}

static void padEmitKey(PadApplyState &st, unsigned evdev, int press)
{
    padQueue(SOskCommand::EType::PADKEY, (int)evdev, press);
    if (press)
        st.padKeys.insert(evdev);
    else
        st.padKeys.erase(evdev);
}

static bool padIcontains(const std::string &hay, const char *needle)
{
    if (!needle || !*needle || hay.empty())
        return false;
    auto pred = [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    };
    const char *end = needle + strlen(needle);
    return std::search(hay.begin(), hay.end(), needle, end, pred) != hay.end();
}

static bool padWindowClassHas(const PHLWINDOW &w, const char *needle)
{
    if (!w || !needle)
        return false;
    return padIcontains(w->m_class, needle) || padIcontains(w->m_initialClass, needle);
}

static bool padWindowLooksLikeGame(const PHLWINDOW &w)
{
    if (!w)
        return false;
    if (padWindowClassHas(w, "gamescope") || padIcontains(w->m_title, "gamescope") ||
        padIcontains(w->m_initialTitle, "gamescope"))
        return true;
    /* Steam/Proton windows: steam_app_<id>, often borderless-maximized not exclusive FS */
    if (padWindowClassHas(w, "steam_app"))
        return true;
    return false;
}

/* Fullscreen YouTube in a browser (or mpv/VLC) still wants the pad as a
 * mouse. Gamescope, Steam games, and other covering/maximized clients do not. */
static bool padWindowKeepsPad(const PHLWINDOW &w)
{
    if (!w)
        return false;
    static const char *keep[] = {
        "firefox", "librewolf", "floorp", "waterfox", "zen", "navigator",
        "chromium", "chrome", "brave", "vivaldi", "thorium", "edge", "opera",
        "qutebrowser", "epiphany", "freetube", "mpv", "vlc", "celluloid", "totem",
    };
    for (const char *k : keep) {
        if (padWindowClassHas(w, k))
            return true;
    }
    return false;
}

static bool padWindowTakesPad(const PHLWINDOW &w)
{
    if (!w)
        return false;
    if (padWindowLooksLikeGame(w))
        return true;
    if (padWindowKeepsPad(w))
        return false;
    auto &ctl = Fullscreen::controller();
    if (!ctl)
        return false;
    /* Exclusive FS *or* maximized/borderless — many games never set FSMODE_FULLSCREEN. */
    auto modes = ctl->getFullscreenModes(w);
    return modes.internal != Fullscreen::FSMODE_NONE || modes.client != Fullscreen::FSMODE_NONE;
}

/* True if a game/gamescope is in play on the focused monitor — not only the
 * focused window. gamescope's class is often the inner game, not "gamescope". */
static bool padMonitorHasGame()
{
    PHLWINDOW focused;
    PHLMONITOR mon;
    if (auto fs = Desktop::focusState()) {
        focused = fs->window();
        mon     = fs->monitor();
    }
    if (padWindowTakesPad(focused))
        return true;
    if (focused && !mon)
        mon = focused->m_monitor.lock();
    auto &ctl = Fullscreen::controller();
    if (ctl && mon) {
        auto fsw = ctl->getFullscreenWindow(mon, true);
        if (fsw && !padWindowKeepsPad(fsw))
            return true;
    }
    auto &st = Desktop::windowState();
    if (!st)
        return false;
    for (const auto &w : st->windows()) {
        if (!w || !w->m_isMapped)
            continue;
        if (mon && w->m_monitor.lock() != mon)
            continue;
        if (padWindowLooksLikeGame(w) || padWindowTakesPad(w))
            return true;
    }
    return false;
}

static void padWakeThread();

static bool padInjectAllowed()
{
    if (!g_padEnabled.load(std::memory_order_relaxed))
        return false;
    if (g_panelVisible.load(std::memory_order_relaxed))
        return true;
    return !g_padYielded.load(std::memory_order_relaxed);
}

/* 0 = park, 1 = grab lizard phantoms only (game), 2 = hidraw reader (desktop). */
static int padThreadMode()
{
    if (padInjectAllowed())
        return 2;
    if (g_padYielded.load(std::memory_order_relaxed))
        return 1;
    return 0;
}

static void padKickIfHoldChanged()
{
    static std::atomic<int> lastMode{-1};
    int mode = padThreadMode();
    int was  = lastMode.exchange(mode, std::memory_order_relaxed);
    if (was != mode) {
        traceGeom(std::string("gamepad: mode ") + std::to_string(was) + " -> " + std::to_string(mode));
        if (mode != 0)
            ensurePadThread();
        padWakeThread();
    }
}

static void padRefreshYield()
{
    bool yield = padMonitorHasGame();
    bool was   = g_padYielded.exchange(yield, std::memory_order_relaxed);
    if (was != yield)
        traceGeom(std::string("gamepad: ") + (yield ? "yield (game/gamescope)" : "resume (desktop)"));
    padKickIfHoldChanged();
}

static void padRelease(PadApplyState &st);

static void padApply(const PadView &v, PadApplyState &st)
{
    if (!padInjectAllowed()) {
        if (st.lastBtn || st.lastLclick || st.lastRclick || st.lastNavDirs || !st.padKeys.empty())
            padRelease(st);
        return;
    }
    if (v.btn == 0 && v.lx == 0.0 && v.ly == 0.0 && v.rx == 0.0 && v.ry == 0.0 &&
        v.rpadDx == 0.0 && v.rpadDy == 0.0 && v.lt <= 0.0 && v.rt <= 0.0 &&
        st.lastBtn == 0 && !st.lastLT && !st.lastRT && !st.lastLclick && !st.lastRclick &&
        st.lastNavDirs == 0 && st.padKeys.empty()) {
        st.lastMs = nowMs();
        return;
    }
    uint32_t gen = g_padMapGen.load(std::memory_order_relaxed);
    if (gen != st.mapGen) {
        std::lock_guard<std::mutex> lg(g_padMapMutex);
        st.map    = g_padMapLive;
        st.mapGen = g_padMapGen.load(std::memory_order_relaxed);
    }
    const PadMap &map = st.map;
    bool navMode = g_panelVisible.load(std::memory_order_relaxed);
    uint32_t btn = v.btn;
    auto trig = [](double x, bool last) -> bool {
        if (x < 0.0)
            return last;
        return last ? (x > 0.25) : (x > 0.37);
    };
    st.lastLT = trig(v.lt, st.lastLT);
    st.lastRT = trig(v.rt, st.lastRT);
    if (st.lastLT)
        btn |= (1u << PC_LT);
    if (st.lastRT)
        btn |= (1u << PC_RT);
    g_padButtons.store(btn, std::memory_order_relaxed);
    const uint8_t *acts = navMode ? map.osk : map.desktop;

    bool wantL = false, wantR = false;
    for (int c = 0; c < PC_COUNT; c++) {
        if (!(btn & (1u << c)))
            continue;
        if (acts[c] == PA_LCLICK)
            wantL = true;
        if (acts[c] == PA_RCLICK)
            wantR = true;
    }
    if (wantL != st.lastLclick)
        padQueue(SOskCommand::EType::PADPTR, BTN_LEFT, wantL ? 1 : 0);
    if (wantR != st.lastRclick)
        padQueue(SOskCommand::EType::PADPTR, BTN_RIGHT, wantR ? 1 : 0);
    st.lastLclick = wantL;
    st.lastRclick = wantR;

    uint32_t edges = btn ^ st.lastBtn;
    if (edges) {
        for (int c = 0; c < PC_COUNT; c++) {
            if (!(edges & (1u << c)))
                continue;
            int press = (btn & (1u << c)) ? 1 : 0;
            switch (acts[c]) {
            case PA_ENTER: padEmitKey(st, KEY_ENTER, press); break;
            case PA_ESC: padEmitKey(st, KEY_ESC, press); break;
            case PA_BACK: padEmitKey(st, KEY_BACKSPACE, press); break;
            case PA_SPACE: padEmitKey(st, KEY_SPACE, press); break;
            case PA_TAB: padEmitKey(st, KEY_TAB, press); break;
            case PA_UP: padEmitKey(st, KEY_UP, press); break;
            case PA_DOWN: padEmitKey(st, KEY_DOWN, press); break;
            case PA_LEFT: padEmitKey(st, KEY_LEFT, press); break;
            case PA_RIGHT: padEmitKey(st, KEY_RIGHT, press); break;
            case PA_MENU:
                if (press)
                    padQueue(SOskCommand::EType::PADCHORD, 1, 0);
                break;
            case PA_TOGGLE:
                if (press)
                    padQueue(SOskCommand::EType::PADCHORD, 0, 0);
                break;
            case PA_COMMIT: padQueue(SOskCommand::EType::PADNAV, 4, press); break;
            case PA_CLOSE: padQueue(SOskCommand::EType::PADNAV, 7, press); break;
            case PA_NAVUP: padQueue(SOskCommand::EType::PADNAV, 0, press); break;
            case PA_NAVDN: padQueue(SOskCommand::EType::PADNAV, 1, press); break;
            case PA_NAVLT: padQueue(SOskCommand::EType::PADNAV, 2, press); break;
            case PA_NAVRT: padQueue(SOskCommand::EType::PADNAV, 3, press); break;
            default: break;
            }
        }
        st.lastBtn = btn;
    }

    uint32_t now = nowMs();
    /* Cap the gap. Evdev is not a stream: hid-input drops samples inside
     * fuzz, so a quiet Bluetooth stick can sit silent for tens of ms and
     * the next report would otherwise jump by the whole gap. The evdev
     * reader keeps integrating the last deflection on a short tick, so a
     * late sample must not also dump the missed time. */
    double dt = st.lastMs ? std::min(0.012, (now - st.lastMs) / 1000.0) : (1.0 / 270.0);
    st.lastMs = now;
    double px = 0, py = 0;
    /* PadView stick Y is up-positive; compositor pointer Y is down-positive. */
    if (map.pointer & PAN_LS) {
        px += v.lx * 3240.0 * dt;
        py += -v.ly * 3240.0 * dt;
    }
    if (map.pointer & PAN_RS) {
        px += v.rx * 3240.0 * dt;
        py += -v.ry * 3240.0 * dt;
    }
    if (map.pointer & PAN_RPAD) {
        px += v.rpadDx;
        py += v.rpadDy;
    }
    if (px != 0.0 || py != 0.0) {
        g_padDX.fetch_add(px);
        g_padDY.fetch_add(py);
    }

    double sx = 0, sy = 0;
    if (map.scroll & PAN_LS) {
        sx = v.lx;
        sy = v.ly;
    } else if (map.scroll & PAN_RS) {
        sx = v.rx;
        sy = v.ry;
    }
    if (navMode) {
        int dirs = 0;
        if (sy > 0.5 || ((st.lastNavDirs & 1) && sy > 0.3))
            dirs |= 1;
        if (sy < -0.5 || ((st.lastNavDirs & 2) && sy < -0.3))
            dirs |= 2;
        if (sx < -0.5 || ((st.lastNavDirs & 4) && sx < -0.3))
            dirs |= 4;
        if (sx > 0.5 || ((st.lastNavDirs & 8) && sx > 0.3))
            dirs |= 8;
        int changed = dirs ^ st.lastNavDirs;
        if (changed) {
            for (int i = 0; i < 4; i++) {
                if (changed & (1 << i))
                    padQueue(SOskCommand::EType::PADNAV, i, (dirs & (1 << i)) ? 1 : 0);
            }
            st.lastNavDirs = dirs;
        }
    } else {
        if (st.lastNavMode) {
            for (int i = 0; i < 4; i++) {
                if (st.lastNavDirs & (1 << i))
                    padQueue(SOskCommand::EType::PADNAV, i, 0);
            }
            st.lastNavDirs = 0;
        }
        if (padSq(sx, sy) > 0.0025) {
            g_padSX.fetch_add(sx * 810.0 * dt);
            g_padSY.fetch_add(-sy * 810.0 * dt);
        }
    }
    st.lastNavMode = navMode;
    if (px != 0.0 || py != 0.0)
        padWake();
    else if (!navMode && padSq(sx, sy) > 0.0025)
        padWake();
}

static void padRelease(PadApplyState &st)
{
    for (unsigned k : st.padKeys)
        padQueue(SOskCommand::EType::PADKEY, (int)k, 0);
    st.padKeys.clear();
    if (st.lastLclick)
        padQueue(SOskCommand::EType::PADPTR, BTN_LEFT, 0);
    if (st.lastRclick)
        padQueue(SOskCommand::EType::PADPTR, BTN_RIGHT, 0);
    for (int i = 0; i < 4; i++) {
        if (st.lastNavDirs & (1 << i))
            padQueue(SOskCommand::EType::PADNAV, i, 0);
    }
    st.lastBtn = 0;
    st.lastLclick = st.lastRclick = false;
    st.lastNavDirs = 0;
}

#define PAD_NBITS(x) ((((x) - 1) / 8) + 1)
#define PAD_TEST(bit, arr) (((arr)[(bit) / 8] >> ((bit) % 8)) & 1)

static int padOpenEvdev()
{
    for (int i = 0; i < 64; i++) {
        char node[64];
        snprintf(node, sizeof node, "/dev/input/event%d", i);
        int fd = open(node, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME_256, name) < 0 || padPhantomName(name)) {
            close(fd);
            continue;
        }
        unsigned char evb[PAD_NBITS(EV_MAX)] = {0};
        unsigned char key[PAD_NBITS(KEY_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof evb), evb) < 0 || !PAD_TEST(EV_KEY, evb) ||
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0 || !PAD_TEST(BTN_SOUTH, key)) {
            close(fd);
            continue;
        }
        return fd;
    }
    return -1;
}

static double padNormAbs(const input_absinfo &inf, int v, bool trigger)
{
    if (trigger) {
        int span = inf.maximum - inf.minimum;
        if (span <= 0)
            return 0;
        double n = (v - inf.minimum) / (double)span;
        if (n < 0)
            n = 0;
        if (n > 1)
            n = 1;
        return n;
    }
    int mid = (inf.minimum + inf.maximum) / 2;
    int span = inf.maximum - mid;
    if (span <= 0)
        return 0;
    double n = (v - mid) / (double)span;
    if (inf.flat > 0 && std::abs(v - mid) <= inf.flat)
        n = 0;
    if (n < -1)
        n = -1;
    if (n > 1)
        n = 1;
    return n;
}

static bool padReadEvdev(int fd, PadApplyState &st)
{
    unsigned char absb[PAD_NBITS(ABS_MAX)] = {0};
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
    input_absinfo ax{}, ay{}, arx{}, ary{}, az{}, arz{}, hatx{}, haty{};
    bool hasX = PAD_TEST(ABS_X, absb) && ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0;
    bool hasY = PAD_TEST(ABS_Y, absb) && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0;
    bool hasRX = PAD_TEST(ABS_RX, absb) && ioctl(fd, EVIOCGABS(ABS_RX), &arx) == 0;
    bool hasRY = PAD_TEST(ABS_RY, absb) && ioctl(fd, EVIOCGABS(ABS_RY), &ary) == 0;
    bool hasZ = PAD_TEST(ABS_Z, absb) && ioctl(fd, EVIOCGABS(ABS_Z), &az) == 0;
    bool hasRZ = PAD_TEST(ABS_RZ, absb) && ioctl(fd, EVIOCGABS(ABS_RZ), &arz) == 0;
    bool hasHX = PAD_TEST(ABS_HAT0X, absb) && ioctl(fd, EVIOCGABS(ABS_HAT0X), &hatx) == 0;
    bool hasHY = PAD_TEST(ABS_HAT0Y, absb) && ioctl(fd, EVIOCGABS(ABS_HAT0Y), &haty) == 0;
    input_absinfo ag{}, ab{};
    bool hasGas = PAD_TEST(ABS_GAS, absb) && ioctl(fd, EVIOCGABS(ABS_GAS), &ag) == 0;
    bool hasBrake = PAD_TEST(ABS_BRAKE, absb) && ioctl(fd, EVIOCGABS(ABS_BRAKE), &ab) == 0;
    /* hid-microsoft's Xbox BLE descriptor puts the right stick on Z/RZ and
     * the triggers on Brake/Accelerator (ABS_BRAKE = LT, ABS_GAS = RT).
     * Select/Start/Guide already arrive as BTN_SELECT/BTN_START/BTN_MODE;
     * do not reshuffle them onto the stick clicks (those have no default
     * binding, so View and Menu go dead). */
    /* Z/RZ without RX/RY is ambiguous: analog triggers (rest at minimum)
     * or the second stick (rest mid-travel). Brake/Gas, when present, are
     * the triggers, so Z/RZ is the stick. Otherwise sample rest at open.
     * (Holding a trigger while the device opens misreads until reopen.) */
    bool restNearMin = false;
    if (hasZ && hasRZ && !hasGas && !hasBrake) {
        double spanZ = (double)az.maximum - (double)az.minimum;
        double spanRZ = (double)arz.maximum - (double)arz.minimum;
        restNearMin = spanZ > 0 && spanRZ > 0 && (double)az.value - (double)az.minimum < spanZ / 8.0 &&
                      (double)arz.value - (double)arz.minimum < spanRZ / 8.0;
    }
    bool zIsTrig = !hasGas && !hasBrake && (hasRX || restNearMin);
    /* Seed from the current axis position. evdev emits only axes that
     * changed, and these sticks are 0..65535 with rest at mid-scale: a
     * zero here is full deflection, so the first right-stick report used
     * to scroll (left stick still 0) and peg the unreported stick axis. */
    int vx = hasX ? ax.value : 0, vy = hasY ? ay.value : 0;
    int vrx = hasRX ? arx.value : 0, vry = hasRY ? ary.value : 0;
    int vz = hasZ ? az.value : 0, vrz = hasRZ ? arz.value : 0;
    int vg = hasGas ? ag.value : 0, vb = hasBrake ? ab.value : 0;
    int vhx = hasHX ? hatx.value : 0, vhy = hasHY ? haty.value : 0;
    traceGeom(std::string("gamepad: evdev map zTrig=") + (zIsTrig ? "1" : "0") +
              " gas=" + (hasGas ? "1" : "0") + " brake=" + (hasBrake ? "1" : "0") +
              " rx=" + (hasRX ? "1" : "0"));
    /* While a stick is deflected, integrate on a fixed tick. Bluetooth
     * Xbox reports are sparse after kernel defuzz (fuzz is ~255 on a
     * 0..65535 axis), so event-timed steps arrive as visible jumps. */
    PadView held{};
    bool coast = false;
    auto pullAbs = [&](int axis, input_absinfo &inf, int &slot, bool present) {
        if (present && ioctl(fd, EVIOCGABS(axis), &inf) == 0)
            slot = inf.value;
    };
    uint32_t btn = 0;
    auto setb = [&](int pc, bool on) {
        if (on)
            btn |= (1u << pc);
        else
            btn &= ~(1u << pc);
    };
    struct pollfd pfds[2]{{fd, POLLIN, 0}, {g_padPipe[0], POLLIN, 0}};
    while (g_padRunning && padInjectAllowed()) {
        int pr = poll(pfds, 2, coast ? 4 : 1000);
        if (!g_padRunning || (pr > 0 && (pfds[1].revents & POLLIN))) {
            /* drain the wake byte: poll stays readable while bytes remain,
             * and an undrained byte re-exits every reopen instantly — the
             * reader spins open/close forever and input dies silently */
            char b;
            while (read(g_padPipe[0], &b, 1) > 0) {}
            return true; /* wake: disable/exit, keep scanning */
        }
        if (pr == 0) {
            if (coast)
                padApply(held, st); /* same deflection, next slice of time */
            continue;
        }
        if (pr < 0 || !(pfds[0].revents & POLLIN)) {
            if (pr < 0 && errno != EINTR)
                return false;
            if (pr > 0 && (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
                return false;
            continue;
        }
        input_event ev[32];
        ssize_t n;
        while ((n = read(fd, ev, sizeof ev)) > 0) {
            int cnt = n / (int)sizeof(input_event);
            for (int i = 0; i < cnt; i++) {
                if (ev[i].type == EV_KEY) {
                    bool on = ev[i].value != 0;
                    switch (ev[i].code) {
                    case BTN_SOUTH: setb(PC_A, on); break;
                    case BTN_EAST: setb(PC_B, on); break;
                    case BTN_NORTH: setb(PC_X, on); break;
                    case BTN_WEST: setb(PC_Y, on); break;
                    case BTN_TL: setb(PC_LB, on); break;
                    case BTN_TR: setb(PC_RB, on); break;
                    case BTN_TL2: if (!zIsTrig || !hasZ) setb(PC_LT, on); break;
                    case BTN_TR2: if (!zIsTrig || !hasRZ) setb(PC_RT, on); break;
                    case BTN_SELECT: setb(PC_SELECT, on); break;
                    case BTN_START: setb(PC_START, on); break;
                    case BTN_MODE: setb(PC_GUIDE, on); break;
                    case BTN_THUMBL: setb(PC_LSCLICK, on); break;
                    case BTN_THUMBR: setb(PC_RSCLICK, on); break;
                    case BTN_DPAD_UP: setb(PC_DUP, on); break;
                    case BTN_DPAD_DOWN: setb(PC_DDN, on); break;
                    case BTN_DPAD_LEFT: setb(PC_DLT, on); break;
                    case BTN_DPAD_RIGHT: setb(PC_DRT, on); break;
                    default: break;
                    }
                } else if (ev[i].type == EV_ABS) {
                    switch (ev[i].code) {
                    case ABS_X: vx = ev[i].value; break;
                    case ABS_Y: vy = ev[i].value; break;
                    case ABS_RX: vrx = ev[i].value; break;
                    case ABS_RY: vry = ev[i].value; break;
                    case ABS_Z: vz = ev[i].value; break;
                    case ABS_RZ: vrz = ev[i].value; break;
                    case ABS_GAS: vg = ev[i].value; break;
                    case ABS_BRAKE: vb = ev[i].value; break;
                    case ABS_HAT0X: vhx = ev[i].value; break;
                    case ABS_HAT0Y: vhy = ev[i].value; break;
                    default: break;
                    }
                } else if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) {
                    /* state was lost; the next SYN would otherwise coast
                     * on a stale deflection until a later change */
                    pullAbs(ABS_X, ax, vx, hasX);
                    pullAbs(ABS_Y, ay, vy, hasY);
                    pullAbs(ABS_RX, arx, vrx, hasRX);
                    pullAbs(ABS_RY, ary, vry, hasRY);
                    pullAbs(ABS_Z, az, vz, hasZ);
                    pullAbs(ABS_RZ, arz, vrz, hasRZ);
                    pullAbs(ABS_GAS, ag, vg, hasGas);
                    pullAbs(ABS_BRAKE, ab, vb, hasBrake);
                    pullAbs(ABS_HAT0X, hatx, vhx, hasHX);
                    pullAbs(ABS_HAT0Y, haty, vhy, hasHY);
                } else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
                    PadView view;
                    view.btn = btn;
                    if (hasHX) {
                        setb(PC_DLT, vhx < 0);
                        setb(PC_DRT, vhx > 0);
                        view.btn = btn;
                    }
                    if (hasHY) {
                        setb(PC_DUP, vhy < 0);
                        setb(PC_DDN, vhy > 0);
                        view.btn = btn;
                    }
                    double nx = hasX ? padNormAbs(ax, vx, false) : 0;
                    double ny = hasY ? -padNormAbs(ay, vy, false) : 0; /* up-positive */
                    double nrx = 0, nry = 0;
                    if (hasRX) {
                        nrx = padNormAbs(arx, vrx, false);
                        nry = hasRY ? -padNormAbs(ary, vry, false) : 0;
                    } else if (!zIsTrig && hasZ) {
                        nrx = padNormAbs(az, vz, false);
                        nry = hasRZ ? -padNormAbs(arz, vrz, false) : 0;
                    }
                    /* Rescale past the deadzone so motion starts at rest
                     * instead of stepping to the 15% floor. */
                    auto soften = [](double &x, double &y) {
                        const double dz = 0.15;
                        double m2 = padSq(x, y);
                        if (m2 <= dz * dz) {
                            x = y = 0;
                            return;
                        }
                        double m = std::sqrt(m2);
                        double s = (m - dz) / ((1.0 - dz) * m);
                        x *= s;
                        y *= s;
                    };
                    soften(nx, ny);
                    soften(nrx, nry);
                    view.lx = nx;
                    view.ly = ny;
                    view.rx = nrx;
                    view.ry = nry;
                    if (hasBrake)
                        view.lt = padNormAbs(ab, vb, true);
                    else if (zIsTrig && hasZ)
                        view.lt = padNormAbs(az, vz, true);
                    if (hasGas)
                        view.rt = padNormAbs(ag, vg, true);
                    else if (zIsTrig && hasRZ)
                        view.rt = padNormAbs(arz, vrz, true);
                    held = view;
                    coast = padSq(view.lx, view.ly) > 0.0 || padSq(view.rx, view.ry) > 0.0;
                    padApply(view, st);
                }
            }
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return false;
        if (n == 0)
            return false;
    }
    return true;
}

static void padPushState()
{
    /* enabled + active snapshot to the client. Atomics + mutex-protected
     * non-blocking send only, so the socket thread (client connect) and the
     * drain (PADSTATE) may call it. The reader thread never calls compositor
     * APIs or the client socket — it queues PADSTATE instead. */
    char buf[32];
    snprintf(buf, sizeof buf, "pad %d %d", (int)g_padEnabled.load(std::memory_order_relaxed),
             (int)g_padActive.load(std::memory_order_relaxed));
    sendToClient(buf);
}

static void padQueueState()
{
    padQueue(SOskCommand::EType::PADSTATE, 0, 0);
}

static void padWakeThread()
{
    /* wake the reader's blocking polls (rescan sleep / exit) */
    if (g_padPipe[1] >= 0) {
        char b = 1;
        ssize_t r;
        do {
            r = write(g_padPipe[1], &b, 1);
        } while (r < 0 && errno == EINTR);
    }
}

static void padThreadFn()
{
    int grabbed[16], ngrabbed = 0;
    unsigned char report[128];
    while (g_padRunning) {
        int mode = padThreadMode();
        if (mode == 0) {
            g_padActive.store(false, std::memory_order_relaxed);
            g_padButtons.store(0, std::memory_order_relaxed);
            struct pollfd pfd{g_padPipe[0], POLLIN, 0};
            poll(&pfd, 1, 1000);
            char b;
            while (read(g_padPipe[0], &b, 1) > 0) {}
            continue;
        }
        if (mode == 1) {
            /* Game/gamescope owns hidraw. Do not open it (that lagged). Still
             * EVIOCGRAB lizard-mode mouse/keyboard so Hyprland does not see
             * them — that lag remained even with GAMEPAD off. */
            g_padActive.store(false, std::memory_order_relaxed);
            g_padButtons.store(0, std::memory_order_relaxed);
            padGrabPhantoms(grabbed, &ngrabbed);
            traceGeom("gamepad: phantom grab only, n=" + std::to_string(ngrabbed));
            struct pollfd pfd{g_padPipe[0], POLLIN, 0};
            while (g_padRunning && padThreadMode() == 1) {
                poll(&pfd, 1, 1000);
                char b;
                while (read(g_padPipe[0], &b, 1) > 0) {}
            }
            padUngrabPhantoms(grabbed, &ngrabbed);
            continue;
        }
        int kind = 1; /* 1 = steam hidraw 0x42, 2 = evdev gamepad */
        int fd = padOpenStreamer();
        if (fd < 0) {
            fd = padOpenEvdev();
            kind = 2;
        }
        if (fd < 0) {
            /* nothing streaming: sleep until woken (exit) or 1 s (rescan) */
            struct pollfd pfd{g_padPipe[0], POLLIN, 0};
            poll(&pfd, 1, 1000);
            /* drain the pipe */
            char b;
            while (read(g_padPipe[0], &b, 1) > 0) {}
            continue;
        }
        g_padActive = true;
        padQueueState();
        padGrabPhantoms(grabbed, &ngrabbed);
        traceGeom(std::string("gamepad: ") + (kind == 2 ? "evdev" : "hidraw 0x42") +
                  " open, phantoms grabbed=" + std::to_string(ngrabbed));
        PadApplyState st;
        if (kind == 2) {
            padReadEvdev(fd, st);
            padRelease(st);
        } else {
        /* stick rest calibration: sticks idle near (not at) 0, so the open
         * path used to average whatever the sticks happened to be doing —
         * including a held deflection right after toggle-on, which then
         * drifted the cursor forever. Instead, keep the previous rest and
         * adopt a new one only from a still window (30 consecutive reports
         * within ±400 on every axis); input runs on the old rest meanwhile.
         * First boot rest is 0, inside the 1500 deadzone of typical units. */
        double restLX = padRestLX, restLY = padRestLY, restRX = padRestRX, restRY = padRestRY;
        bool   restSettled = false;
        int16_t calLX[32], calLY[32], calRX[32], calRY[32];
        int calN = 0;
        int16_t  lastPX = 0, lastPY = 0;
        bool     padWasTouched = false;
        struct pollfd pfds[2]{{fd, POLLIN, 0}, {g_padPipe[0], POLLIN, 0}};
        bool alive = true;
        while (alive && g_padRunning && padInjectAllowed()) {
            int pr = poll(pfds, 2, 1000);
            if (!g_padRunning)
                break;
            if (pr > 0 && (pfds[1].revents & POLLIN)) {
                char b;
                while (read(g_padPipe[0], &b, 1) > 0) {} /* drain the wake (see evdev loop) */
                break; /* exit wake */
            }
            if (pr <= 0 || !(pfds[0].revents & POLLIN)) {
                if (pr < 0 && errno != EINTR)
                    alive = false;
                else if (pr > 0 && (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
                    alive = false; /* unplugged: poll reports ERR, not readable —
                                      without this the thread busy-spins here forever */
                continue;
            }
            ssize_t n;
            while ((n = read(fd, report, sizeof report)) > 0) {
                if (n < PAD_LEN_MIN || report[0] != 0x42)
                    continue; /* 0x79/0x7b telemetry etc. */
                uint32_t raw = (uint32_t)report[PAD_B0] | ((uint32_t)report[PAD_B0 + 1] << 8) |
                               ((uint32_t)(report[PAD_B0 + 2] & 0x7F) << 16);
                PadView view;
                if (raw & PB_A)
                    view.btn |= (1u << PC_A);
                if (raw & PB_B)
                    view.btn |= (1u << PC_B);
                if (raw & PB_X)
                    view.btn |= (1u << PC_X);
                if (raw & PB_Y)
                    view.btn |= (1u << PC_Y);
                if (raw & PB_DPAD_UP)
                    view.btn |= (1u << PC_DUP);
                if (raw & PB_DPAD_DOWN)
                    view.btn |= (1u << PC_DDN);
                if (raw & PB_DPAD_LEFT)
                    view.btn |= (1u << PC_DLT);
                if (raw & PB_DPAD_RIGHT)
                    view.btn |= (1u << PC_DRT);
                if (raw & PB_BACK)
                    view.btn |= (1u << PC_SELECT);
                if (raw & PB_START)
                    view.btn |= (1u << PC_START);
                if (raw & PB_GUIDE)
                    view.btn |= (1u << PC_GUIDE);
                if (raw & PB_RPAD_CLICK)
                    view.btn |= (1u << PC_RPCLICK);
                if (report[PAD_STATUS] & PB_LPAD_CLICK_STATUS)
                    view.btn |= (1u << PC_LPCLICK);
                view.lt = padU16(report, PAD_LTRIG) / 32767.0;
                view.rt = padU16(report, PAD_RTRIG) / 32767.0;
                int16_t px = padI16(report, PAD_RPAD_X), py = padI16(report, PAD_RPAD_Y);
                bool touched = (px != 0 || py != 0 || (raw & (PB_RPAD_TOUCH | PB_RPAD_CLICK)) != 0);
                if (touched && padWasTouched) {
                    double dx = (double)(px - lastPX), dy = (double)(py - lastPY);
                    double mag2 = padSq(dx, dy);
                    if (mag2 >= 22500.0 && mag2 < 64000000.0) {
                        double mag = std::sqrt(mag2);
                        double k = 0.03 * (1.0 + std::min(mag / 4000.0, 2.0));
                        view.rpadDx = dx * k;
                        view.rpadDy = -dy * k;
                    }
                }
                lastPX = px;
                lastPY = py;
                padWasTouched = touched;
                const double sdz2 = 1500.0 * 1500.0;
                int16_t sx = padI16(report, PAD_LSTICK_X), sy = padI16(report, PAD_LSTICK_Y);
                int16_t qx = padI16(report, PAD_RSTICK_X), qy = padI16(report, PAD_RSTICK_Y);
                double ldx = (double)sx - restLX, ldy = (double)sy - restLY;
                double rdx = (double)qx - restRX, rdy = (double)qy - restRY;
                if (padSq(ldx, ldy) > sdz2) {
                    view.lx = ldx / 32767.0;
                    view.ly = ldy / 32767.0;
                }
                if (padSq(rdx, rdy) > sdz2) {
                    view.rx = rdx / 32767.0;
                    view.ry = rdy / 32767.0;
                }
                padApply(view, st);
                /* stillness-gated rest adoption: feed this report's raw stick
                 * sample; a 30-report window with every axis within ±400 of
                 * its mean becomes the new rest (input above already ran on
                 * the old rest, so calibration never stalls input). */
                if (!restSettled) {
                    if (calN < 32) {
                        calLX[calN] = sx;
                        calLY[calN] = sy;
                        calRX[calN] = qx;
                        calRY[calN] = qy;
                        calN++;
                    } else {
                        memmove(calLX, calLX + 1, 31 * sizeof(int16_t));
                        memmove(calLY, calLY + 1, 31 * sizeof(int16_t));
                        memmove(calRX, calRX + 1, 31 * sizeof(int16_t));
                        memmove(calRY, calRY + 1, 31 * sizeof(int16_t));
                        calLX[31] = sx;
                        calLY[31] = sy;
                        calRX[31] = qx;
                        calRY[31] = qy;
                    }
                    if (calN >= 30) {
                        long aLX = 0, aLY = 0, aRX = 0, aRY = 0;
                        int16_t nLX = 32767, xLX = -32768, nLY = 32767, xLY = -32768;
                        int16_t nRX = 32767, xRX = -32768, nRY = 32767, xRY = -32768;
                        for (int i = calN - 30; i < calN; i++) {
                            aLX += calLX[i];
                            aLY += calLY[i];
                            aRX += calRX[i];
                            aRY += calRY[i];
                            if (calLX[i] < nLX) nLX = calLX[i];
                            if (calLX[i] > xLX) xLX = calLX[i];
                            if (calLY[i] < nLY) nLY = calLY[i];
                            if (calLY[i] > xLY) xLY = calLY[i];
                            if (calRX[i] < nRX) nRX = calRX[i];
                            if (calRX[i] > xRX) xRX = calRX[i];
                            if (calRY[i] < nRY) nRY = calRY[i];
                            if (calRY[i] > xRY) xRY = calRY[i];
                        }
                        if (xLX - nLX < 800 && xLY - nLY < 800 && xRX - nRX < 800 && xRY - nRY < 800) {
                            double mLX = (double)aLX / 30, mLY = (double)aLY / 30;
                            double mRX = (double)aRX / 30, mRY = (double)aRY / 30;
                            /* stillness is not centeredness: a steadily held
                             * deflection is still too. Reject windows far from
                             * the current rest (a real rest sits within ±6000
                             * of it; first boot rest is 0). */
                            if (std::hypot(mLX - restLX, mLY - restLY) < 6000.0 &&
                                std::hypot(mRX - restRX, mRY - restRY) < 6000.0) {
                                restLX = padRestLX = mLX;
                                restLY = padRestLY = mLY;
                                restRX = padRestRX = mRX;
                                restRY = padRestRY = mRY;
                                restSettled = true;
                                traceGeom("gamepad: stick rest settled");
                            }
                        }
                    }
                }
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                alive = false; /* unplugged: rescan */
            if (n == 0)
                alive = false;
        }
        /* device lost (or thread exiting): release everything the pad path
         * holds, or keys/buttons stay stuck — a release that happens while
         * unplugged would otherwise never reach the seat. Duplicate
         * releases after a clean lift are harmless. */
        if (!alive)
            traceGeom("gamepad: device lost, rescanning");
        padRelease(st);
        }
        padUngrabPhantoms(grabbed, &ngrabbed);
        close(fd);
        g_padActive = false;
        g_padButtons.store(0, std::memory_order_relaxed);
        padQueueState();
    }
    if (g_padPipe[0] >= 0)
        close(g_padPipe[0]);
    if (g_padPipe[1] >= 0)
        close(g_padPipe[1]);
    g_padPipe[0] = g_padPipe[1] = -1;
}

static void padTapChord(std::initializer_list<unsigned> keys)
{
    for (unsigned k : keys)
        execKey(k, 1);
    /* release in reverse so the chord reads as one gesture */
    unsigned buf[4];
    size_t   n = 0;
    for (unsigned k : keys)
        buf[n++] = k;
    while (n > 0)
        execKey(buf[--n], 0);
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
    refreshIntendedShell();
    {
        std::lock_guard<std::mutex> lg(g_ringMutex);
        while (g_ringCount > 0) {
            SOskCommand c = g_ring[g_ringHead];
            g_ringHead = (g_ringHead + 1) % RING_SIZE;
            g_ringCount--;
            g_ringMutex.unlock();
            const bool fromShell = intendedShellAllows(c.pid);
            switch (c.type) {
            case SOskCommand::EType::KEY:
                if (fromShell && layerAllowsInject(c.pid))
                    execKey((unsigned)c.a, c.b);
                break;
            case SOskCommand::EType::MOD: {
                if (!fromShell || !layerAllowsInject(c.pid))
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
                if (!fromShell || !layerAllowsInject(c.pid))
                    break;
                held_mods = 0;
                sendMods(0);
                break;
            }
            case SOskCommand::EType::TEXT:
                if (fromShell && layerAllowsInject(c.pid))
                    execText(c.text);
                break;
            case SOskCommand::EType::LAYOUT:
                if (fromShell)
                    execLayout(c.text);
                break;
            case SOskCommand::EType::FLING:
                if (!fromShell)
                    break;
                fling_tau = std::max(0.05, std::min(2.0, c.a / 1000.0));
                fling_cap = std::max(500.0, std::min(20000.0, (double)c.b));
                DBG("fling: tau=" + std::to_string(fling_tau) + " cap=" + std::to_string(fling_cap));
                break;
            case SOskCommand::EType::POINTER:
                if (!fromShell)
                    break;
                drag_slop_px  = std::max(4.0, std::min(40.0, (double)c.a));
                long_press_ms = std::max(0, std::min(2000, c.b));
                DBG("pointer: slop=" + std::to_string(drag_slop_px) + " long=" + std::to_string(long_press_ms));
                break;
            case SOskCommand::EType::SCROLL:
                if (!fromShell)
                    break;
                scroll_gain = std::max(0.5, std::min(2.0, c.a / 100.0));
                if (c.b >= 0)
                    scroll_axis_px = c.b != 0;
                DBG("scroll gain: " + std::to_string(scroll_gain) +
                    " axispx=" + std::to_string((int)scroll_axis_px));
                break;
            case SOskCommand::EType::SWALLOW:
                if (!fromShell)
                    break;
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
                if (!fromShell)
                    break;
                g_panelPid = c.pid;
                panel_nx = c.panel[0];
                panel_ny = c.panel[1];
                panel_nw = c.panel[2];
                panel_nh = c.panel[3];
                panel_rect_valid = (panel_nw > 0 && panel_nh > 0 && layerAllowsInject(c.pid));
                g_panelVisible.store(panel_rect_valid, std::memory_order_release);
                if (panel_rect_valid)
                    releaseInjectedMods();
                padRefreshYield();
                traceGeom("panel rect valid=" + std::to_string((int)panel_rect_valid));
                DBG("panel rect (norm): " + std::to_string(panel_nx) + " " + std::to_string(panel_ny) +
                    " " + std::to_string(panel_nw) + " " + std::to_string(panel_nh) +
                    " inject=" + std::to_string((int)panel_rect_valid));
                break;
            case SOskCommand::EType::PADKEY:
                /* hidraw path (pid=0): allowlisted keys, like a USB keyboard.
                 * Downs only while the reader is enabled and not yielded; releases always. */
                if (!padInjectAllowed() && c.b)
                    break;
                traceGeom("pad key evdev=" + std::to_string(c.a) + (c.b ? " down" : " up"));
                execKey((unsigned)c.a, c.b);
                break;
            case SOskCommand::EType::PADPTR: {
                if (!padInjectAllowed() && c.b)
                    break;
                uint32_t t = nowMs();
                traceGeom("pad ptr btn=" + std::to_string(c.a) + (c.b ? " down" : " up"));
                g_pSeatManager->sendPointerButton(t, (uint32_t)c.a,
                                                 c.b ? WL_POINTER_BUTTON_STATE_PRESSED
                                                     : WL_POINTER_BUTTON_STATE_RELEASED);
                g_pSeatManager->sendPointerFrame();
                break;
            }
            case SOskCommand::EType::PADCHORD:
                if (!padInjectAllowed())
                    break;
                if (c.a == 0)
                    sendToClient("toggle"); /* QML toggles; do not inject Super+Shift+K */
                else {
                    padTapChord({KEY_LEFTMETA, KEY_SPACE}); /* Omarchy menu bind */
                    releaseInjectedMods();
                }
                break;
            case SOskCommand::EType::PADNAV: {
                /* hidraw path, unsolicited push (like grid). Releases always
                 * so a disable mid-hold cannot leave QML repeating. */
                static const char *padNavNames[] = {"up",   "down",  "left", "right",
                                                    "commit", "back", "space", "close"};
                if (!padInjectAllowed() && c.b)
                    break;
                if (c.a >= 0 && c.a < 8) {
                    traceGeom(std::string("pad nav ") + padNavNames[c.a] + (c.b ? " 1" : " 0"));
                    sendToClient(std::string("nav ") + padNavNames[c.a] + (c.b ? " 1" : " 0"));
                }
                break;
            }
            case SOskCommand::EType::PADSTATE:
                padPushState();
                break;
            case SOskCommand::EType::PADMAP:
                if (!fromShell)
                    break;
                {
                    std::lock_guard<std::mutex> lg(g_padMapMutex);
                    g_padMapLive = g_padMapIncoming;
                    g_padMapGen.fetch_add(1, std::memory_order_release);
                }
                break;
            case SOskCommand::EType::PADWAKE: {
                g_padWakePending.store(false);
                if (!padInjectAllowed()) {
                    g_padDX.store(0.0);
                    g_padDY.store(0.0);
                    g_padSX.store(0.0);
                    g_padSY.store(0.0);
                    break;
                }
                double dx = g_padDX.exchange(0.0), dy = g_padDY.exchange(0.0);
                if (dx != 0.0 || dy != 0.0) {
                    /* full device-motion path, like a touchpad: relative move
                     * through the pointer manager + unify. warpTo alone never
                     * clears hide_on_key_press, so the cursor stayed invisible
                     * after any pad button press until a real device moved. */
                    IPointer::SMotionEvent ev;
                    ev.timeMs  = nowMs();
                    ev.delta   = Vector2D{dx * g_padGain, dy * g_padGain};
                    ev.unaccel = ev.delta;
                    ev.mouse   = false;
                    ev.device  = nullptr;
                    g_pInputManager->onMouseMoved(ev);
                }
                double sx = g_padSX.exchange(0.0), sy = g_padSY.exchange(0.0);
                if (sx != 0.0 || sy != 0.0)
                    emitScroll(sx * g_padGain, sy * g_padGain);
                break;
            }
            case SOskCommand::EType::MONREFRESH:
                pushMon(); /* touch frame wins; else the pointer's monitor */
                break;
            case SOskCommand::EType::GAMEPAD: {
                if (!fromShell)
                    break;
                bool en = g_padEnabled.load(std::memory_order_relaxed);
                if (c.a == 2)
                    en = !en; /* toggle */
                else
                    en = c.a != 0;
                g_padEnabled.store(en, std::memory_order_relaxed);
                if (en)
                    ensurePadThread(); /* lazy start when the env is unset */
                padKickIfHoldChanged();
                padWakeThread(); /* still wake: enable path may not change lastHold */
                padPushState();
                traceGeom(std::string("gamepad ") + (en ? "enabled" : "disabled"));
                break;
            }
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
/* main thread only: start the pad reader once (idempotent). Started at
 * load so a plugged-in controller is detected immediately; GAMEPAD off
 * parks it with no device open. */
static void ensurePadThread()
{
    if (g_padThread.joinable())
        return;
    if (g_padPipe[0] < 0 && pipe(g_padPipe) == 0) {
        fcntl(g_padPipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_padPipe[1], F_SETFL, O_NONBLOCK);
    }
    g_padRunning = true;
    g_padThread  = std::thread(padThreadFn);
    Log::logger->log(Log::INFO, "[hypr-osk] gamepad reader started");
}
static std::string socketPath()
{
    const char *rtd = getenv("XDG_RUNTIME_DIR");
    return std::string(rtd ? rtd : "/tmp") + "/hypr-osk.sock";
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static void unbindTouchHooks()
{
    auto drop = [](auto &sig, Hyprutils::Signal::CHyprSignalListener &slot) {
        if (!slot)
            return;
        std::erase_if(sig.m_vStaticListeners, [&](const auto &sp) { return sp.get() == slot.get(); });
        slot.reset();
    };
    if (!Event::bus())
        return;
    auto &t = Event::bus()->m_events.input.touch;
    drop(t.down, g_touchDownHook);
    drop(t.up, g_touchUpHook);
    drop(t.motion, g_touchMoveHook);
}

static void unbindPadYieldHooks()
{
    g_padWinActive.reset();
    g_padWinFs.reset();
    g_padWinClass.reset();
    g_padWinTitle.reset();
    g_padWinClose.reset();
    g_padWinOpen.reset();
    g_padWinFloat.reset();
}

static void bindPadYieldHooks()
{
    unbindPadYieldHooks();
    if (!Event::bus())
        return;
    auto &w = Event::bus()->m_events.window;
    g_padWinActive = w.active.listen([] { padRefreshYield(); });
    g_padWinFs     = w.fullscreen.listen([] { padRefreshYield(); });
    g_padWinClass  = w.class_.listen([] { padRefreshYield(); });
    g_padWinTitle  = w.title.listen([] { padRefreshYield(); });
    g_padWinClose  = w.close.listen([] { padRefreshYield(); });
    g_padWinOpen   = w.open.listen([] { padRefreshYield(); });
    g_padWinFloat  = w.floating.listen([] { padRefreshYield(); });
    padRefreshYield();
}

static void bindTouchHooks()
{
    /* listenStatic runs after every regular listener. hyprgrass uses listen()
     * and writes cancelled=false for a 1-finger tap; we must run after that
     * and set cancelled=true or Hyprland will send native wl_touch on top of
     * the virtual pointer. */
    unbindTouchHooks();
    auto &t = Event::bus()->m_events.input.touch;
    t.down.listenStatic([](ITouch::SDownEvent ev, Event::SCallbackInfo &info) { touchDown(ev, info); });
    g_touchDownHook = t.down.m_vStaticListeners.back();
    t.up.listenStatic([](ITouch::SUpEvent ev, Event::SCallbackInfo &info) { touchUp(ev, info); });
    g_touchUpHook = t.up.m_vStaticListeners.back();
    t.motion.listenStatic([](ITouch::SMotionEvent ev, Event::SCallbackInfo &info) { touchMotion(ev, info); });
    g_touchMoveHook = t.motion.m_vStaticListeners.back();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

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
    refreshIntendedShell();
    g_socketThread = std::thread(socket_thread_fn, socketPath());
    padMapDefault(&g_padMapLive);
    padMapDefault(&g_padMapIncoming);
    /* gamepad reader: auto-start so a plugged-in pad works before QML
     * handshakes. HYPR_OSK_GAMEPAD=0 disables at load; osk.json / GAMEPAD
     * off from the pinned shell parks it later. */
    if (const char *g = getenv("HYPR_OSK_PAD_GAIN")) {
        char *endp = nullptr;
        double v   = strtod(g, &endp);
        if (endp != g && v >= 0.1 && v <= 5.0)
            g_padGain = v;
    }
    if (const char *ge = getenv("HYPR_OSK_GAMEPAD"); ge && !strcmp(ge, "0"))
        g_padEnabled.store(false, std::memory_order_relaxed);
    if (g_padEnabled.load(std::memory_order_relaxed))
        ensurePadThread();
    bindTouchHooks();
    bindPadYieldHooks();

    Log::logger->log(Log::INFO, "[hypr-osk] plugin initialized, socket at " + socketPath());
    return {"hypr-osk", "On-screen keyboard: touch->pointer + keyboard synthesis", "ekollof", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    unbindPadYieldHooks();
    unbindTouchHooks();
    /* stop the gamepad reader first: same constraint as the socket thread
     * (its code lives in this .so); it releases EVIOCGRABs before returning */
    if (g_padThread.joinable()) {
        g_padRunning = false;
        if (g_padPipe[1] >= 0) {
            char b = 1;
            ssize_t r;
            do {
                r = write(g_padPipe[1], &b, 1);
            } while (r < 0 && errno == EINTR);
        }
        g_padThread.join();
    }
    if (g_padPipe[0] >= 0)
        close(g_padPipe[0]);
    if (g_padPipe[1] >= 0)
        close(g_padPipe[1]);
    g_padPipe[0] = g_padPipe[1] = -1;
    /* stop the socket thread and join it BEFORE the .so is unmapped: the
     * thread's code lives in this library */
    g_socketRunning = false;
    int lfd = g_listenFd.exchange(-1);
    if (lfd >= 0)
        shutdown(lfd, SHUT_RDWR);
    {
        std::lock_guard<std::mutex> lg(g_clientMutex);
        dropClientFd(g_clientFd);
    }
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
    clearIntendedShell();
    Log::logger->log(Log::INFO, "[hypr-osk] unloaded");
}
