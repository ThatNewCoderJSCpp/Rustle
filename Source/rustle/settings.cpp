#include <rustle/settings.hpp>
#include <rustle/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;

namespace rustle {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

float clampf(float v, float lo, float hi, float fallback) {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

double clampd(double v, double lo, double hi, double fallback) {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

std::string number_key(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

Json edit_json(const ClipEdit& e) {
    Json o = Json::make_object();
    o.set("volume", json_number(e.volume));
    o.set("speed", json_number(e.speed));
    o.set("tempo", json_number(e.tempo));
    o.set("pitch", json_number(e.pitch));
    o.set("reverse", json_bool(e.reverse));
    o.set("reverb", json_number(e.reverb));
    o.set("reverb_size", json_number(e.reverb_size));
    o.set("trim_start", json_number(e.trim_start));
    o.set("trim_end", json_number(e.trim_end));
    o.set("fade_in", json_number(e.fade_in));
    o.set("fade_out", json_number(e.fade_out));
    o.set("loop", json_bool(e.loop));
    return o;
}

ClipEdit edit_from(const Json& o) {
    ClipEdit e;
    e.volume = static_cast<float>(json_get_number(o, "volume", e.volume));
    e.speed = static_cast<float>(json_get_number(o, "speed", e.speed));
    e.tempo = static_cast<float>(json_get_number(o, "tempo", e.tempo));
    e.pitch = static_cast<float>(json_get_number(o, "pitch", e.pitch));
    e.reverse = json_get_bool(o, "reverse", e.reverse);
    e.reverb = static_cast<float>(json_get_number(o, "reverb", e.reverb));
    e.reverb_size = static_cast<float>(json_get_number(o, "reverb_size", e.reverb_size));
    e.trim_start = json_get_number(o, "trim_start", e.trim_start);
    e.trim_end = json_get_number(o, "trim_end", e.trim_end);
    e.fade_in = json_get_number(o, "fade_in", e.fade_in);
    e.fade_out = json_get_number(o, "fade_out", e.fade_out);
    e.loop = json_get_bool(o, "loop", e.loop);
    e.clamp();
    return e;
}

} // namespace

bool operator==(const ClipEdit& a, const ClipEdit& b) {
    return a.volume == b.volume && a.speed == b.speed && a.tempo == b.tempo && a.pitch == b.pitch && a.reverse == b.reverse && a.reverb == b.reverb
        && a.reverb_size == b.reverb_size && a.trim_start == b.trim_start && a.trim_end == b.trim_end && a.fade_in == b.fade_in && a.fade_out == b.fade_out
        && a.loop == b.loop;
}

bool operator!=(const ClipEdit& a, const ClipEdit& b) {
    return !(a == b);
}

bool ClipEdit::is_default() const {
    return *this == ClipEdit();
}

bool ClipEdit::changes_audio() const {
    ClipEdit plain = *this;
    plain.volume = 1.0f;
    plain.loop = false;
    return !plain.is_default();
}

std::string ClipEdit::render_key() const {
    if (!changes_audio()) return "plain";
    std::string k;
    k += "s" + number_key(speed) + "t" + number_key(tempo) + "p" + number_key(pitch) + "r" + (reverse ? "1" : "0");
    k += "v" + number_key(reverb) + "z" + number_key(reverb_size) + "a" + number_key(trim_start) + "b" + number_key(trim_end);
    k += "i" + number_key(fade_in) + "o" + number_key(fade_out);
    return k;
}

void ClipEdit::clamp() {
    const ClipEdit d;
    volume = clampf(volume, 0.0f, 2.0f, d.volume);
    speed = clampf(speed, 0.25f, 4.0f, d.speed);
    tempo = clampf(tempo, 0.25f, 4.0f, d.tempo);
    pitch = clampf(pitch, -24.0f, 24.0f, d.pitch);
    reverb = clampf(reverb, 0.0f, 1.0f, d.reverb);
    reverb_size = clampf(reverb_size, 0.0f, 1.0f, d.reverb_size);
    trim_start = clampd(trim_start, 0.0, 86400.0, 0.0);
    trim_end = clampd(trim_end, 0.0, 86400.0, 0.0);
    if (trim_end > 0.0 && trim_end <= trim_start) trim_end = 0.0;
    fade_in = clampd(fade_in, 0.0, 60.0, 0.0);
    fade_out = clampd(fade_out, 0.0, 60.0, 0.0);
}

fs::path Settings::config_dir() {
    const std::string over = env("RUSTLE_CONFIG_DIR");
    if (!over.empty()) return over;
#if defined(_WIN32)
    const std::string appdata = env("APPDATA");
    if (!appdata.empty()) return fs::path(appdata) / "Rustle";
#elif defined(__APPLE__)
    const std::string home = env("HOME");
    if (!home.empty()) return fs::path(home) / "Library" / "Application Support" / "Rustle";
#else
    const std::string xdg = env("XDG_CONFIG_HOME");
    if (!xdg.empty()) return fs::path(xdg) / "rustle";
    const std::string home = env("HOME");
    if (!home.empty()) return fs::path(home) / ".config" / "rustle";
#endif
    return fs::current_path() / "rustle-config";
}

fs::path Settings::default_path() {
    return config_dir() / "settings.json";
}

fs::path Settings::presets_dir() {
    return config_dir() / "presets";
}

std::string Settings::to_json() const {
    Json root = Json::make_object();
    root.set("format", json_string("rustle-settings"));
    root.set("version", json_number(1));
    root.set("folders", json_strings(folders));
    root.set("favorites", json_strings(std::vector<std::string>(favorites.begin(), favorites.end())));
    Json ed = Json::make_object();
    for (const auto& kv : edits) {
        if (!kv.second.is_default()) ed.set(kv.first, edit_json(kv.second));
    }
    root.set("edits", std::move(ed));
    Json keys = Json::make_object();
    for (const auto& kv : keybinds) {
        if (!kv.second.empty()) keys.set(kv.first, json_string(kv.second));
    }
    root.set("keybinds", std::move(keys));
    Json plays = Json::make_object();
    for (const auto& kv : play_counts) plays.set(kv.first, json_number(kv.second));
    root.set("play_counts", std::move(plays));
    root.set("key_stop_all", json_string(key_stop_all));
    root.set("key_mic_toggle", json_string(key_mic_toggle));
    root.set("key_push_to_talk", json_string(key_push_to_talk));
    root.set("global_hotkeys", json_bool(global_hotkeys));
    root.set("auto_mode", json_bool(auto_mode));
    root.set("virtual_mic", json_bool(virtual_mic));
    root.set("keep_virtual_mic", json_bool(keep_virtual_mic));
    root.set("input_device", json_string(input_device));
    root.set("monitor_device", json_string(monitor_device));
    root.set("send_device", json_string(send_device));
    root.set("mic_enabled", json_bool(mic_enabled));
    root.set("push_to_talk", json_bool(push_to_talk));
    root.set("hear_myself", json_bool(hear_myself));
    root.set("sounds_to_me", json_bool(sounds_to_me));
    root.set("sounds_to_others", json_bool(sounds_to_others));
    root.set("mic_volume", json_number(mic_volume));
    root.set("sounds_volume", json_number(sounds_volume));
    root.set("others_volume", json_number(others_volume));
    root.set("me_volume", json_number(me_volume));
    root.set("hear_myself_volume", json_number(hear_myself_volume));
    root.set("overlap", json_bool(overlap));
    root.set("sort", json_string(sort));
    root.set("view", json_string(view));
    root.set("favorites_first", json_bool(favorites_first));
    root.set("last_folder", json_string(last_folder));
    root.set("browser_path", json_string(browser_path));
    root.set("show_hidden", json_bool(show_hidden));
    root.set("rescan_seconds", json_number(rescan_seconds));
    root.set("voice_preset", json_string(voice_preset));
    if (!voice_json.empty()) {
        Json voice;
        if (fizmo::audio::parse_json(voice_json, voice) && voice.is_object()) root.set("voice", std::move(voice));
    }
    return fizmo::audio::write_json(root, true) + "\n";
}

bool Settings::from_json(const std::string& text, std::string* error) {
    Json root;
    std::string why;
    if (!fizmo::audio::parse_json(text, root, &why)) {
        if (error) *error = "settings.json isn't valid JSON: " + why;
        return false;
    }
    if (!root.is_object()) {
        if (error) *error = "settings.json doesn't hold a JSON object";
        return false;
    }
    Settings s;
    s.folders = json_get_strings(root, "folders");
    for (const std::string& f : json_get_strings(root, "favorites")) s.favorites.insert(f);
    if (const Json* ed = root.find("edits")) {
        if (ed->is_object()) {
            for (std::size_t i = 0; i < ed->size(); ++i) {
                if (ed->at(i).is_object()) s.edits[ed->key(i)] = edit_from(ed->at(i));
            }
        }
    }
    if (const Json* keys = root.find("keybinds")) {
        if (keys->is_object()) {
            for (std::size_t i = 0; i < keys->size(); ++i) {
                if (keys->at(i).is_string() && !keys->at(i).as_string().empty()) s.keybinds[keys->key(i)] = keys->at(i).as_string();
            }
        }
    }
    if (const Json* plays = root.find("play_counts")) {
        if (plays->is_object()) {
            for (std::size_t i = 0; i < plays->size(); ++i) {
                const double n = plays->at(i).as_number(0.0);
                if (n > 0.0 && n < 1e9) s.play_counts[plays->key(i)] = static_cast<int>(n);
            }
        }
    }
    s.key_stop_all = json_get_string(root, "key_stop_all", s.key_stop_all);
    s.key_mic_toggle = json_get_string(root, "key_mic_toggle", s.key_mic_toggle);
    s.key_push_to_talk = json_get_string(root, "key_push_to_talk", s.key_push_to_talk);
    s.global_hotkeys = json_get_bool(root, "global_hotkeys", s.global_hotkeys);
    s.auto_mode = json_get_bool(root, "auto_mode", s.auto_mode);
    s.virtual_mic = json_get_bool(root, "virtual_mic", s.virtual_mic);
    s.keep_virtual_mic = json_get_bool(root, "keep_virtual_mic", s.keep_virtual_mic);
    s.input_device = json_get_string(root, "input_device", s.input_device);
    s.monitor_device = json_get_string(root, "monitor_device", s.monitor_device);
    s.send_device = json_get_string(root, "send_device", s.send_device);
    s.mic_enabled = json_get_bool(root, "mic_enabled", s.mic_enabled);
    s.push_to_talk = json_get_bool(root, "push_to_talk", s.push_to_talk);
    s.hear_myself = json_get_bool(root, "hear_myself", s.hear_myself);
    s.sounds_to_me = json_get_bool(root, "sounds_to_me", s.sounds_to_me);
    s.sounds_to_others = json_get_bool(root, "sounds_to_others", s.sounds_to_others);
    s.mic_volume = clampf(static_cast<float>(json_get_number(root, "mic_volume", s.mic_volume)), 0.0f, 4.0f, 1.0f);
    s.sounds_volume = clampf(static_cast<float>(json_get_number(root, "sounds_volume", s.sounds_volume)), 0.0f, 4.0f, 1.0f);
    s.others_volume = clampf(static_cast<float>(json_get_number(root, "others_volume", s.others_volume)), 0.0f, 4.0f, 1.0f);
    s.me_volume = clampf(static_cast<float>(json_get_number(root, "me_volume", s.me_volume)), 0.0f, 4.0f, 0.8f);
    s.hear_myself_volume = clampf(static_cast<float>(json_get_number(root, "hear_myself_volume", s.hear_myself_volume)), 0.0f, 4.0f, 0.7f);
    s.overlap = json_get_bool(root, "overlap", s.overlap);
    s.sort = json_get_string(root, "sort", s.sort);
    if (s.sort != "name" && s.sort != "duration" && s.sort != "newest" && s.sort != "played") s.sort = "name";
    s.view = json_get_string(root, "view", s.view);
    if (s.view != "tiles" && s.view != "list") s.view = "tiles";
    s.favorites_first = json_get_bool(root, "favorites_first", s.favorites_first);
    s.last_folder = json_get_string(root, "last_folder", s.last_folder);
    s.browser_path = json_get_string(root, "browser_path", s.browser_path);
    s.show_hidden = json_get_bool(root, "show_hidden", s.show_hidden);
    s.rescan_seconds = clampd(json_get_number(root, "rescan_seconds", s.rescan_seconds), 0.0, 3600.0, 3.0);
    s.voice_preset = json_get_string(root, "voice_preset", s.voice_preset);
    if (const Json* voice = root.find("voice")) {
        if (voice->is_object()) s.voice_json = fizmo::audio::write_json(*voice, true);
    }
    *this = std::move(s);
    return true;
}

LoadResult load_settings(const fs::path& path, Settings& out) {
    LoadResult r;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        out = Settings();
        return r;
    }
    r.existed = true;
    std::string text;
    std::string why;
    if (!read_text_file(path, text, &why)) {
        out = Settings();
        r.ok = false;
        r.message = why;
        return r;
    }
    Settings loaded;
    if (loaded.from_json(text, &why)) {
        out = std::move(loaded);
        return r;
    }
    out = Settings();
    r.ok = false;
    r.recovered = true;
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path aside = path.string() + ".broken-" + std::to_string(stamp);
    fs::rename(path, aside, ec);
    r.message = why + (ec ? std::string("; starting with default settings") : "; it was moved to " + aside.filename().string() + " and Rustle started with default settings");
    return r;
}

bool save_settings(const fs::path& path, const Settings& settings, std::string* error) {
    return write_text_file_atomic(path, settings.to_json(), error);
}

} // namespace rustle
