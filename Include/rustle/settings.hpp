#ifndef RUSTLE_SETTINGS_HPP
#define RUSTLE_SETTINGS_HPP

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rustle {

struct ClipEdit {
    float volume = 1.0f;
    float speed = 1.0f;
    float tempo = 1.0f;
    float pitch = 0.0f;
    bool reverse = false;
    float reverb = 0.0f;
    float reverb_size = 0.5f;
    double trim_start = 0.0;
    double trim_end = 0.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
    bool loop = false;

    bool is_default() const;
    bool changes_audio() const;
    std::string render_key() const;
    void clamp();
};

bool operator==(const ClipEdit& a, const ClipEdit& b);
bool operator!=(const ClipEdit& a, const ClipEdit& b);

struct Settings {
    std::vector<std::string> folders;
    std::set<std::string> favorites;
    std::map<std::string, ClipEdit> edits;
    std::map<std::string, std::string> keybinds;
    std::map<std::string, int> play_counts;

    std::string key_stop_all = "F8";
    std::string key_mic_toggle = "F9";
    std::string key_push_to_talk;
    bool global_hotkeys = false;

    bool auto_mode = true;
    bool virtual_mic = true;
    bool keep_virtual_mic = false;
    std::string input_device;
    std::string monitor_device;
    std::string send_device;

    bool mic_enabled = true;
    bool push_to_talk = false;
    bool hear_myself = false;
    bool sounds_to_me = true;
    bool sounds_to_others = true;

    float mic_volume = 1.0f;
    float sounds_volume = 1.0f;
    float others_volume = 1.0f;
    float me_volume = 0.8f;
    float hear_myself_volume = 0.7f;

    bool overlap = true;
    std::string sort = "name";
    std::string view = "tiles";
    bool favorites_first = false;
    std::string last_folder;
    std::string browser_path;
    bool show_hidden = false;
    double rescan_seconds = 3.0;

    std::string voice_json;
    std::string voice_preset;

    std::string to_json() const;
    bool from_json(const std::string& text, std::string* error = nullptr);

    static std::filesystem::path config_dir();
    static std::filesystem::path default_path();
    static std::filesystem::path presets_dir();
};

struct LoadResult {
    bool ok = true;
    bool existed = false;
    bool recovered = false;
    std::string message;
};

LoadResult load_settings(const std::filesystem::path& path, Settings& out);
bool save_settings(const std::filesystem::path& path, const Settings& settings, std::string* error = nullptr);

} // namespace rustle

#endif // RUSTLE_SETTINGS_HPP
