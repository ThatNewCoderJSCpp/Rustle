#include <rustle/hotkeys.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#define RUSTLE_HAVE_X11 1
#include <dlfcn.h>
#include <poll.h>
#endif

namespace rustle {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::map<std::string, std::string>& key_aliases() {
    static const std::map<std::string, std::string> m = {
        { "esc", "Escape" }, { "return", "Enter" }, { "spacebar", "Space" }, { "left", "LeftArrow" }, { "right", "RightArrow" }, { "up", "UpArrow" },
        { "down", "DownArrow" }, { "del", "Delete" }, { "ins", "Insert" }, { "pgup", "PageUp" }, { "pgdn", "PageDown" }, { "grave", "Tilde" },
        { "backquote", "Tilde" }, { "apostrophe", "Quote" },
    };
    return m;
}

const std::vector<std::string>& known_keys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (char c = 'A'; c <= 'Z'; ++c) k.push_back(std::string(1, c));
        for (char c = '0'; c <= '9'; ++c) k.push_back(std::string(1, c));
        for (int i = 1; i <= 24; ++i) k.push_back("F" + std::to_string(i));
        for (const char* n : { "Escape", "Enter", "Tab", "Backspace", "Space", "Minus", "Equals", "LeftBracket", "RightBracket", "Backslash", "Semicolon", "Quote", "Tilde",
                 "Comma", "Period", "Slash", "CapsLock", "ScrollLock", "NumLock", "PrintScreen", "Pause", "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "LeftArrow",
                 "RightArrow", "UpArrow", "DownArrow", "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9",
                 "NumpadDivide", "NumpadMultiply", "NumpadSubtract", "NumpadAdd", "NumpadEnter", "NumpadDecimal", "Menu", "VolumeMute", "VolumeDown", "VolumeUp",
                 "MediaPlayPause", "MediaStop", "MediaNext", "MediaPrevious" }) {
            k.push_back(n);
        }
        return k;
    }();
    return keys;
}

std::string canonical_key(const std::string& raw) {
    const std::string l = lower(raw);
    auto a = key_aliases().find(l);
    if (a != key_aliases().end()) return a->second;
    for (const std::string& k : known_keys()) {
        if (lower(k) == l) return k;
    }
    return std::string();
}

} // namespace

bool is_modifier_key(const std::string& key) {
    const std::string l = lower(key);
    return l == "leftshift" || l == "rightshift" || l == "leftcontrol" || l == "rightcontrol" || l == "leftalt" || l == "rightalt" || l == "leftmeta" || l == "rightmeta"
        || l == "shift" || l == "ctrl" || l == "control" || l == "alt" || l == "meta" || l == "super";
}

bool parse_chord(const std::string& text, Chord& out) {
    out = Chord();
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+' && !cur.empty()) {
            parts.push_back(cur);
            cur.clear();
        } else if (c != ' ') {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return false;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string m = lower(parts[i]);
        if (m == "ctrl" || m == "control") out.ctrl = true;
        else if (m == "shift") out.shift = true;
        else if (m == "alt") out.alt = true;
        else if (m == "meta" || m == "super" || m == "cmd" || m == "win") out.meta = true;
        else return false;
    }
    const std::string key = canonical_key(parts.back());
    if (key.empty()) return false;
    out.key = key;
    return true;
}

std::string chord_text(const Chord& c) {
    if (!c.valid()) return std::string();
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.alt) s += "Alt+";
    if (c.shift) s += "Shift+";
    if (c.meta) s += "Meta+";
    return s + c.key;
}

std::string normalize_chord(const std::string& text) {
    Chord c;
    if (!parse_chord(text, c)) return std::string();
    return chord_text(c);
}

std::string x11_keysym_name(const std::string& key) {
    static const std::map<std::string, std::string> m = {
        { "Escape", "Escape" }, { "Enter", "Return" }, { "Tab", "Tab" }, { "Backspace", "BackSpace" }, { "Space", "space" }, { "Minus", "minus" }, { "Equals", "equal" },
        { "LeftBracket", "bracketleft" }, { "RightBracket", "bracketright" }, { "Backslash", "backslash" }, { "Semicolon", "semicolon" }, { "Quote", "apostrophe" },
        { "Tilde", "grave" }, { "Comma", "comma" }, { "Period", "period" }, { "Slash", "slash" }, { "CapsLock", "Caps_Lock" }, { "ScrollLock", "Scroll_Lock" },
        { "NumLock", "Num_Lock" }, { "PrintScreen", "Print" }, { "Pause", "Pause" }, { "Insert", "Insert" }, { "Delete", "Delete" }, { "Home", "Home" }, { "End", "End" },
        { "PageUp", "Prior" }, { "PageDown", "Next" }, { "LeftArrow", "Left" }, { "RightArrow", "Right" }, { "UpArrow", "Up" }, { "DownArrow", "Down" },
        { "NumpadDivide", "KP_Divide" }, { "NumpadMultiply", "KP_Multiply" }, { "NumpadSubtract", "KP_Subtract" }, { "NumpadAdd", "KP_Add" }, { "NumpadEnter", "KP_Enter" },
        { "NumpadDecimal", "KP_Decimal" }, { "Menu", "Menu" }, { "VolumeMute", "XF86AudioMute" }, { "VolumeDown", "XF86AudioLowerVolume" },
        { "VolumeUp", "XF86AudioRaiseVolume" }, { "MediaPlayPause", "XF86AudioPlay" }, { "MediaStop", "XF86AudioStop" }, { "MediaNext", "XF86AudioNext" },
        { "MediaPrevious", "XF86AudioPrev" },
    };
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    if (key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0]))) return std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(key[0]))));
    if (key.size() == 1 && std::isdigit(static_cast<unsigned char>(key[0]))) return key;
    if (key.size() >= 2 && key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1]))) return key;
    if (key.size() == 7 && key.compare(0, 6, "Numpad") == 0) return "KP_" + key.substr(6);
    return std::string();
}

int windows_virtual_key(const std::string& key) {
    static const std::map<std::string, int> m = {
        { "Escape", 0x1B }, { "Enter", 0x0D }, { "Tab", 0x09 }, { "Backspace", 0x08 }, { "Space", 0x20 }, { "Minus", 0xBD }, { "Equals", 0xBB }, { "LeftBracket", 0xDB },
        { "RightBracket", 0xDD }, { "Backslash", 0xDC }, { "Semicolon", 0xBA }, { "Quote", 0xDE }, { "Tilde", 0xC0 }, { "Comma", 0xBC }, { "Period", 0xBE }, { "Slash", 0xBF },
        { "CapsLock", 0x14 }, { "ScrollLock", 0x91 }, { "NumLock", 0x90 }, { "PrintScreen", 0x2C }, { "Pause", 0x13 }, { "Insert", 0x2D }, { "Delete", 0x2E }, { "Home", 0x24 },
        { "End", 0x23 }, { "PageUp", 0x21 }, { "PageDown", 0x22 }, { "LeftArrow", 0x25 }, { "RightArrow", 0x27 }, { "UpArrow", 0x26 }, { "DownArrow", 0x28 },
        { "NumpadDivide", 0x6F }, { "NumpadMultiply", 0x6A }, { "NumpadSubtract", 0x6D }, { "NumpadAdd", 0x6B }, { "NumpadEnter", 0x0D }, { "NumpadDecimal", 0x6E },
        { "Menu", 0x5D }, { "VolumeMute", 0xAD }, { "VolumeDown", 0xAE }, { "VolumeUp", 0xAF }, { "MediaPlayPause", 0xB3 }, { "MediaStop", 0xB2 }, { "MediaNext", 0xB0 },
        { "MediaPrevious", 0xB1 },
    };
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    if (key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0]))) return std::toupper(static_cast<unsigned char>(key[0]));
    if (key.size() == 1 && std::isdigit(static_cast<unsigned char>(key[0]))) return key[0];
    if (key.size() >= 2 && key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1]))) {
        const int n = std::atoi(key.c_str() + 1);
        if (n >= 1 && n <= 24) return 0x70 + n - 1;
    }
    if (key.size() == 7 && key.compare(0, 6, "Numpad") == 0 && std::isdigit(static_cast<unsigned char>(key[6]))) return 0x60 + (key[6] - '0');
    return 0;
}

std::string desktop_session_kind() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    const std::string type = lower(env("XDG_SESSION_TYPE"));
    if (type == "wayland" || (!env("WAYLAND_DISPLAY").empty() && type != "x11")) return "wayland";
    if (!env("DISPLAY").empty()) return "x11";
    return "none";
#endif
}

#if defined(RUSTLE_HAVE_X11)

namespace {

namespace xl {

struct Display;
using Window = unsigned long;
using KeySym = unsigned long;
using KeyCode = unsigned char;

struct ErrorEvent {
    int type;
    Display* display;
    unsigned long resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
};

struct KeyEvent {
    int type;
    unsigned long serial;
    int send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    unsigned long time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int keycode;
    int same_screen;
};

union Event {
    int type;
    KeyEvent key;
    long pad[24];
};

using ErrorHandler = int (*)(Display*, ErrorEvent*);

constexpr int key_press = 2;
constexpr int key_release = 3;
constexpr unsigned shift_mask = 1u << 0;
constexpr unsigned lock_mask = 1u << 1;
constexpr unsigned control_mask = 1u << 2;
constexpr unsigned mod1_mask = 1u << 3;
constexpr unsigned mod2_mask = 1u << 4;
constexpr unsigned mod4_mask = 1u << 6;
constexpr int grab_async = 1;

} // namespace xl

struct XApi {
    void* handle = nullptr;
    xl::Display* (*open_display)(const char*) = nullptr;
    int (*close_display)(xl::Display*) = nullptr;
    xl::KeySym (*string_to_keysym)(const char*) = nullptr;
    xl::KeyCode (*keysym_to_keycode)(xl::Display*, xl::KeySym) = nullptr;
    int (*grab_key)(xl::Display*, int, unsigned int, xl::Window, int, int, int) = nullptr;
    int (*ungrab_key)(xl::Display*, int, unsigned int, xl::Window) = nullptr;
    xl::Window (*default_root_window)(xl::Display*) = nullptr;
    int (*connection_number)(xl::Display*) = nullptr;
    int (*pending)(xl::Display*) = nullptr;
    int (*next_event)(xl::Display*, xl::Event*) = nullptr;
    int (*sync)(xl::Display*, int) = nullptr;
    xl::ErrorHandler (*set_error_handler)(xl::ErrorHandler) = nullptr;
    int (*detectable_repeat)(xl::Display*, int, int*) = nullptr;

    bool load(std::string& why) {
        if (handle) return true;
        handle = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!handle) handle = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            why = "libX11 isn't installed";
            return false;
        }
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            void* p = dlsym(handle, name);
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
            ok = ok && p != nullptr;
        };
        get(open_display, "XOpenDisplay");
        get(close_display, "XCloseDisplay");
        get(string_to_keysym, "XStringToKeysym");
        get(keysym_to_keycode, "XKeysymToKeycode");
        get(grab_key, "XGrabKey");
        get(ungrab_key, "XUngrabKey");
        get(default_root_window, "XDefaultRootWindow");
        get(connection_number, "XConnectionNumber");
        get(pending, "XPending");
        get(next_event, "XNextEvent");
        get(sync, "XSync");
        get(set_error_handler, "XSetErrorHandler");
        get(detectable_repeat, "XkbSetDetectableAutoRepeat");
        if (!ok) why = "the installed libX11 is missing functions Rustle needs";
        return ok;
    }
};

XApi& xapi() {
    static XApi api;
    return api;
}

std::atomic<xl::Display*> g_grab_display{ nullptr };
std::atomic<int> g_grab_errors{ 0 };
xl::ErrorHandler g_previous = nullptr;

int on_x_error(xl::Display* d, xl::ErrorEvent* e) {
    if (d == g_grab_display.load()) {
        g_grab_errors.fetch_add(1);
        return 0;
    }
    return g_previous ? g_previous(d, e) : 0;
}

struct Grab {
    std::string action;
    std::string chord;
    int keycode = 0;
    unsigned mods = 0;
    bool down = false;
};

} // namespace

struct GlobalHotkeys::Impl {
    std::thread thread;
    std::atomic<bool> run{ false };
};

bool GlobalHotkeys::available(std::string* why) {
    const std::string kind = desktop_session_kind();
    if (kind == "none") {
        if (why) *why = "there's no X11 display to listen on";
        return false;
    }
    std::string e;
    if (!xapi().load(e)) {
        if (why) *why = e;
        return false;
    }
    if (kind == "wayland" && why) *why = "wayland";
    return true;
}

bool GlobalHotkeys::start(const std::vector<HotkeyBinding>& bindings, Callback callback) {
    stop();
    HotkeyReport report;
    report.backend = "x11";
    std::string why;
    XApi& x = xapi();
    if (desktop_session_kind() == "none") {
        report.message = "There's no X11 display, so global hotkeys can't be caught. Use the in-app keys, or bind desktop shortcuts to 'rustle ctl ...' commands.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_report = report;
        return false;
    }
    if (!x.load(why)) {
        report.message = "Global hotkeys need libX11 (" + why + ").";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_report = report;
        return false;
    }
    xl::Display* d = x.open_display(nullptr);
    if (!d) {
        report.message = "Couldn't open the X display for global hotkeys.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_report = report;
        return false;
    }
    int supported = 0;
    x.detectable_repeat(d, 1, &supported);
    const xl::Window root = x.default_root_window(d);
    std::vector<Grab> grabs;
    g_grab_display.store(d);
    g_previous = x.set_error_handler(on_x_error);
    for (const HotkeyBinding& b : bindings) {
        Chord c;
        if (!parse_chord(b.chord, c)) continue;
        const std::string sym = x11_keysym_name(c.key);
        const xl::KeySym ks = sym.empty() ? 0 : x.string_to_keysym(sym.c_str());
        const int code = ks ? x.keysym_to_keycode(d, ks) : 0;
        if (!code) {
            report.failed.push_back(chord_text(c));
            continue;
        }
        unsigned mods = 0;
        if (c.shift) mods |= xl::shift_mask;
        if (c.ctrl) mods |= xl::control_mask;
        if (c.alt) mods |= xl::mod1_mask;
        if (c.meta) mods |= xl::mod4_mask;
        g_grab_errors.store(0);
        for (unsigned extra : { 0u, xl::lock_mask, xl::mod2_mask, xl::lock_mask | xl::mod2_mask }) x.grab_key(d, code, mods | extra, root, 0, xl::grab_async, xl::grab_async);
        x.sync(d, 0);
        if (g_grab_errors.load() > 0) {
            for (unsigned extra : { 0u, xl::lock_mask, xl::mod2_mask, xl::lock_mask | xl::mod2_mask }) x.ungrab_key(d, code, mods | extra, root);
            x.sync(d, 0);
            report.failed.push_back(chord_text(c));
            continue;
        }
        Grab g;
        g.action = b.action;
        g.chord = chord_text(c);
        g.keycode = code;
        g.mods = mods;
        grabs.push_back(g);
        report.working.push_back(g.chord);
    }
    x.set_error_handler(g_previous);
    g_grab_display.store(nullptr);
    if (grabs.empty()) {
        x.close_display(d);
        report.message = bindings.empty() ? "No keys are set up yet." : "None of the keys could be grabbed; another program may already use them.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_report = report;
        return false;
    }
    report.active = true;
    if (desktop_session_kind() == "wayland") {
        report.message = "You're on Wayland. These keys are grabbed through XWayland, so most desktops only pass them on while an X11 window has focus. For keys that work everywhere, add custom shortcuts in your desktop's keyboard settings that run 'rustle ctl stop-all', 'rustle ctl toggle-mic' or 'rustle ctl play NAME'.";
    } else {
        report.message = "Global hotkeys work while other programs have focus.";
    }
    if (!report.failed.empty()) report.message += " Some keys are taken by another program.";
    m_impl->run.store(true);
    m_impl->thread = std::thread([this, d, grabs, callback]() mutable {
        XApi& xa = xapi();
        const int fd = xa.connection_number(d);
        while (m_impl->run.load()) {
            while (xa.pending(d) > 0) {
                xl::Event ev;
                std::memset(&ev, 0, sizeof(ev));
                xa.next_event(d, &ev);
                if (ev.type != xl::key_press && ev.type != xl::key_release) continue;
                const unsigned state = ev.key.state & (xl::shift_mask | xl::control_mask | xl::mod1_mask | xl::mod4_mask);
                const bool press = ev.type == xl::key_press;
                for (Grab& g : grabs) {
                    if (static_cast<unsigned>(g.keycode) != ev.key.keycode) continue;
                    if (press && state != g.mods) continue;
                    if (press == g.down) continue;
                    g.down = press;
                    if (callback) callback(g.action, press);
                }
            }
            pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            poll(&p, 1, 50);
        }
        const xl::Window r = xa.default_root_window(d);
        for (const Grab& g : grabs) {
            for (unsigned extra : { 0u, xl::lock_mask, xl::mod2_mask, xl::lock_mask | xl::mod2_mask }) xa.ungrab_key(d, g.keycode, g.mods | extra, r);
        }
        xa.sync(d, 0);
        xa.close_display(d);
    });
    std::lock_guard<std::mutex> lock(m_mutex);
    m_report = report;
    m_chords.clear();
    for (const std::string& w : report.working) m_chords.insert(w);
    return true;
}

#elif defined(_WIN32)

struct GlobalHotkeys::Impl {
    std::thread thread;
    std::atomic<bool> run{ false };
};

bool GlobalHotkeys::available(std::string*) {
    return true;
}

bool GlobalHotkeys::start(const std::vector<HotkeyBinding>& bindings, Callback callback) {
    stop();
    HotkeyReport report;
    report.backend = "windows";
    struct Watch {
        std::string action;
        std::string chord;
        int vk = 0;
        Chord c;
        bool down = false;
    };
    std::vector<Watch> watches;
    for (const HotkeyBinding& b : bindings) {
        Watch w;
        if (!parse_chord(b.chord, w.c)) continue;
        w.vk = windows_virtual_key(w.c.key);
        if (!w.vk) {
            report.failed.push_back(chord_text(w.c));
            continue;
        }
        w.action = b.action;
        w.chord = chord_text(w.c);
        watches.push_back(w);
        report.working.push_back(w.chord);
    }
    if (watches.empty()) {
        report.message = bindings.empty() ? "No keys are set up yet." : "None of the keys can be watched.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_report = report;
        return false;
    }
    report.active = true;
    report.message = "Global hotkeys work while other programs have focus.";
    m_impl->run.store(true);
    m_impl->thread = std::thread([this, watches, callback]() mutable {
        auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        while (m_impl->run.load()) {
            const bool ctrl = held(VK_CONTROL);
            const bool shift = held(VK_SHIFT);
            const bool alt = held(VK_MENU);
            const bool meta = held(VK_LWIN) || held(VK_RWIN);
            for (Watch& w : watches) {
                const bool key = held(w.vk);
                const bool now = w.down ? key : (key && ctrl == w.c.ctrl && shift == w.c.shift && alt == w.c.alt && meta == w.c.meta);
                if (now != w.down) {
                    w.down = now;
                    if (callback) callback(w.action, now);
                }
            }
            Sleep(10);
        }
    });
    std::lock_guard<std::mutex> lock(m_mutex);
    m_report = report;
    m_chords.clear();
    for (const std::string& w : report.working) m_chords.insert(w);
    return true;
}

#else

struct GlobalHotkeys::Impl {
    std::thread thread;
    std::atomic<bool> run{ false };
};

bool GlobalHotkeys::available(std::string* why) {
    if (why) *why = "global hotkeys aren't supported on this system yet; the in-app keys still work while Rustle has focus";
    return false;
}

bool GlobalHotkeys::start(const std::vector<HotkeyBinding>&, Callback) {
    HotkeyReport report;
    report.backend = "none";
    report.message = "Global hotkeys aren't supported on macOS yet (they need Accessibility access). The in-app keys work while Rustle has focus.";
    std::lock_guard<std::mutex> lock(m_mutex);
    m_report = report;
    return false;
}

#endif

GlobalHotkeys::GlobalHotkeys() : m_impl(std::make_unique<Impl>()) {}

GlobalHotkeys::~GlobalHotkeys() {
    stop();
}

void GlobalHotkeys::stop() {
    m_impl->run.store(false);
    if (m_impl->thread.joinable()) m_impl->thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_report.active = false;
    m_chords.clear();
}

bool GlobalHotkeys::active() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_report.active;
}

bool GlobalHotkeys::covers(const std::string& chord) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_report.active && m_chords.count(normalize_chord(chord)) != 0;
}

HotkeyReport GlobalHotkeys::report() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_report;
}

} // namespace rustle
