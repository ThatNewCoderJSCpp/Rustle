#ifndef RUSTLE_SESSION_HPP
#define RUSTLE_SESSION_HPP

#include <rustle/clip.hpp>
#include <rustle/control.hpp>
#include <rustle/engine.hpp>
#include <rustle/files.hpp>
#include <rustle/hotkeys.hpp>
#include <rustle/library.hpp>
#include <rustle/settings.hpp>
#include <rustle/virtual_mic.hpp>
#include <rustle/voice.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rustle {

struct SessionOptions {
    std::filesystem::path config_dir;
    bool audio = true;
    bool virtual_mic = true;
    bool hotkeys = true;
    bool control = true;
    bool worker = true;
};

struct RouteStatus {
    std::string mode;
    bool virtual_mic_supported = false;
    bool virtual_mic_wanted = false;
    bool virtual_mic_ready = false;
    std::string virtual_mic_message;
    std::string server;
    StreamStatus capture;
    StreamStatus send;
    StreamStatus monitor;
    std::string send_label;
};

struct Notice {
    std::uint64_t seq = 0;
    std::string kind;
    std::string text;
};

class Session {
public:
    explicit Session(SessionOptions options = SessionOptions());
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void open();
    void close();
    void tick();
    std::recursive_mutex& api_mutex() { return m_api; }

    Settings& settings() { return m_settings; }
    const Settings& settings() const { return m_settings; }
    const LoadResult& load_result() const { return m_load; }
    bool save(std::string* error = nullptr);
    std::filesystem::path settings_path() const;
    std::filesystem::path presets_path() const;

    Library& library() { return m_library; }
    bool add_folder(const std::string& path, std::string* error = nullptr);
    bool remove_folder(const std::string& path);
    void rescan();
    std::uint64_t library_revision() const { return m_library.revision(); }

    bool play(const std::string& path, std::string* error = nullptr);
    bool preview(const std::string& path, const ClipEdit& edit, std::string* error = nullptr);
    bool toggle(const std::string& path, std::string* error = nullptr);
    void stop(const std::string& path);
    void stop_all();
    bool loading(const std::string& path) const;
    bool playing(const std::string& path) const;
    std::vector<PlayingSound> playing() const;
    bool find_by_name(const std::string& name, std::string& path) const;

    ClipEdit edit_of(const std::string& path) const;
    void set_edit(const std::string& path, const ClipEdit& edit);
    void reset_edit(const std::string& path);
    bool favorite(const std::string& path) const;
    void set_favorite(const std::string& path, bool on);
    std::string keybind_of(const std::string& path) const;
    bool set_keybind(const std::string& path, const std::string& chord, std::string* error = nullptr);
    std::string chord_owner(const std::string& chord) const;

    Engine& engine() { return m_engine; }
    VoiceChain& voice() { return m_engine.voice(); }
    void voice_changed();
    bool apply_voice_preset(const std::string& name, std::string* error = nullptr);
    bool save_voice_preset(const std::string& name, std::string* error = nullptr);
    bool rename_voice_preset(const std::string& from, const std::string& to, std::string* error = nullptr);
    bool delete_voice_preset(const std::string& name, std::string* error = nullptr);
    std::vector<VoicePresetInfo> voice_presets() const;

    void apply_audio(bool quiet = false);
    void apply_levels();
    void set_auto_mode(bool on);
    RouteStatus route_status() const;
    VirtualMic& virtual_mic() { return m_vmic; }
    void refresh_virtual_mic_state();

    void key_event(const std::string& chord, bool pressed, bool from_app);
    void apply_hotkeys();
    HotkeyReport hotkey_report() const { return m_hotkeys.report(); }
    void set_mic_enabled(bool on);
    void toggle_mic();
    void set_talking(bool held);

    std::string control_command(const std::string& line);
    bool control_running() const { return m_control.running(); }

    std::vector<Notice> notices_since(std::uint64_t seq) const;
    void notice(const std::string& kind, const std::string& text);

private:
    void start_worker();
    void stop_worker();
    void post(std::function<void()> job);
    void probe_durations();
    bool start_play(const std::string& path, const ClipEdit& edit, bool count, std::string* error);
    void record_play(const std::string& path);

    void queue_key(const std::string& action, bool pressed);
    void run_action(const std::string& action, bool pressed);

    std::recursive_mutex m_api;
    std::mutex m_key_mutex;
    std::vector<std::pair<std::string, bool>> m_key_queue;
    SessionOptions m_options;
    Settings m_settings;
    LoadResult m_load;
    Library m_library;
    ClipCache m_cache;
    Engine m_engine;
    VirtualMic m_vmic;
    GlobalHotkeys m_hotkeys;
    ControlServer m_control;
    bool m_open = false;

    mutable std::mutex m_state_mutex;
    VirtualMicState m_vmic_state;
    std::string m_vmic_message;
    std::set<std::string> m_loading;
    std::map<std::string, std::uint64_t> m_tickets;
    std::uint64_t m_ticket = 0;

    std::mutex m_job_mutex;
    std::condition_variable m_job_cv;
    std::deque<std::function<void()>> m_jobs;
    std::thread m_worker;
    bool m_worker_run = false;

    mutable std::mutex m_notice_mutex;
    std::deque<Notice> m_notices;
    std::uint64_t m_notice_seq = 0;

    std::chrono::steady_clock::time_point m_last_scan_check;
    std::chrono::steady_clock::time_point m_last_heal;
    std::chrono::steady_clock::time_point m_last_save;
    bool m_dirty = false;
    bool m_audio_down = false;
};

} // namespace rustle

#endif // RUSTLE_SESSION_HPP
