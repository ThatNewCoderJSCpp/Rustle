#ifndef RUSTLE_HOTKEYS_HPP
#define RUSTLE_HOTKEYS_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rustle {

struct Chord {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;
    std::string key;

    bool valid() const { return !key.empty(); }
};

bool parse_chord(const std::string& text, Chord& out);
std::string chord_text(const Chord& chord);
std::string normalize_chord(const std::string& text);
bool is_modifier_key(const std::string& key);
std::string x11_keysym_name(const std::string& key);
int windows_virtual_key(const std::string& key);

struct HotkeyBinding {
    std::string action;
    std::string chord;
};

struct HotkeyReport {
    bool active = false;
    std::string backend;
    std::string message;
    std::vector<std::string> working;
    std::vector<std::string> failed;
};

std::string desktop_session_kind();

class GlobalHotkeys {
public:
    using Callback = std::function<void(const std::string& action, bool pressed)>;

    GlobalHotkeys();
    ~GlobalHotkeys();
    GlobalHotkeys(const GlobalHotkeys&) = delete;
    GlobalHotkeys& operator=(const GlobalHotkeys&) = delete;

    static bool available(std::string* why = nullptr);
    bool start(const std::vector<HotkeyBinding>& bindings, Callback callback);
    void stop();
    bool active() const;
    bool covers(const std::string& chord) const;
    HotkeyReport report() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex;
    HotkeyReport m_report;
    std::set<std::string> m_chords;
};

} // namespace rustle

#endif // RUSTLE_HOTKEYS_HPP
