#include <rustle/session.hpp>

#include <grove/grove.hpp>
#include <grove/input/input.hpp>
#include <grove/native.hpp>
#include <grove/runtime/runtime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef RUSTLE_VERSION_STRING
#define RUSTLE_VERSION_STRING "0.1.0"
#endif

namespace fs = std::filesystem;
namespace fa = fizmo::audio;

namespace {

std::atomic<bool> g_quit{ false };

extern "C" void on_quit_signal(int) {
    g_quit.store(true);
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string clean(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> fields_of(const std::vector<std::string>& args) {
    if (args.size() == 1) return split(args[0], '\t');
    return args;
}

std::string arg(const std::vector<std::string>& args, std::size_t i) {
    return i < args.size() ? args[i] : std::string();
}

std::string number(double v, int decimals) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, std::isfinite(v) ? v : 0.0);
    return buf;
}

double to_number(const std::string& s, double fallback) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) return fallback;
    return v;
}

bool truthy(const std::string& s) {
    return s == "1" || s == "true" || s == "on" || s == "yes";
}

std::string time_text(double seconds) {
    if (seconds < 0.0) return "";
    const int total = static_cast<int>(std::floor(seconds + 0.05));
    if (seconds < 10.0) return number(seconds, 1) + "s";
    const int h = total / 3600;
    const int m = (total / 60) % 60;
    const int s = total % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

std::string row(const std::vector<std::string>& f) {
    std::string line;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (i) line += '\t';
        line += clean(f[i]);
    }
    return line + "\n";
}

std::string single(std::string line) {
    while (!line.empty() && line.back() == '\n') line.pop_back();
    return line;
}

std::string find_ui() {
    std::vector<fs::path> candidates;
    const std::string over = env("RUSTLE_UI_DIR");
    if (!over.empty()) candidates.push_back(over);
    const fs::path exe = fs::path(grove::executable_path()).parent_path();
    candidates.push_back(exe / "ui");
    candidates.push_back(exe.parent_path() / "share" / "rustle" / "ui");
#ifdef RUSTLE_SOURCE_UI_DIR
    candidates.push_back(RUSTLE_SOURCE_UI_DIR);
#endif
    for (const fs::path& c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c / "rustle.grove", ec)) return (c / "rustle.grove").string();
    }
    return std::string();
}

void windows_static_scripts(grove::ScriptTools& tools) {
#if defined(_WIN32)
    if (std::find(tools.link_flags.begin(), tools.link_flags.end(), "-static") == tools.link_flags.end()) tools.link_flags.push_back("-static");
#else
    (void)tools;
#endif
}

std::string edit_value(const rustle::ClipEdit& e, const std::string& key) {
    if (key == "volume") return number(e.volume, 2);
    if (key == "speed") return number(e.speed, 2);
    if (key == "tempo") return number(e.tempo, 2);
    if (key == "pitch") return number(e.pitch, 1);
    if (key == "reverse") return e.reverse ? "1" : "0";
    if (key == "reverb") return number(e.reverb, 2);
    if (key == "reverb_size") return number(e.reverb_size, 2);
    if (key == "trim_start") return number(e.trim_start, 2);
    if (key == "trim_end") return number(e.trim_end, 2);
    if (key == "fade_in") return number(e.fade_in, 2);
    if (key == "fade_out") return number(e.fade_out, 2);
    if (key == "loop") return e.loop ? "1" : "0";
    return "";
}

bool set_edit_value(rustle::ClipEdit& e, const std::string& key, const std::string& value) {
    const double v = to_number(value, 0.0);
    if (key == "volume") e.volume = static_cast<float>(v);
    else if (key == "speed") e.speed = static_cast<float>(v);
    else if (key == "tempo") e.tempo = static_cast<float>(v);
    else if (key == "pitch") e.pitch = static_cast<float>(v);
    else if (key == "reverse") e.reverse = truthy(value);
    else if (key == "reverb") e.reverb = static_cast<float>(v);
    else if (key == "reverb_size") e.reverb_size = static_cast<float>(v);
    else if (key == "trim_start") e.trim_start = v;
    else if (key == "trim_end") e.trim_end = v;
    else if (key == "fade_in") e.fade_in = v;
    else if (key == "fade_out") e.fade_out = v;
    else if (key == "loop") e.loop = truthy(value);
    else return false;
    e.clamp();
    return true;
}

const char* k_edit_keys[] = { "volume", "speed", "tempo", "pitch", "reverse", "reverb", "reverb_size", "trim_start", "trim_end", "fade_in", "fade_out", "loop" };

class App {
public:
    App(grove::Document& doc, rustle::Session& session) : m_doc(doc), m_session(session) {
        register_natives();
    }

    ~App() {
        m_beating = false;
        if (m_heartbeat.joinable()) m_heartbeat.join();
    }

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void install() {
        m_beating = true;
        m_heartbeat = std::thread([this] {
            while (m_beating.load()) {
                m_session.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }

private:
    void native(const std::string& name, std::function<std::string(const std::vector<std::string>&)> fn) {
        grove::register_native(m_doc, "rustle." + name, [this, fn = std::move(fn)](const std::vector<std::string>& args) {
            std::lock_guard<std::recursive_mutex> lock(m_session.api_mutex());
            return fn(fields_of(args));
        });
    }

    std::string sound_row(const rustle::SoundEntry& s) {
        const double d = m_session.library().duration(s.path);
        const rustle::ClipEdit e = m_session.edit_of(s.path);
        return row({ "sound", s.path, s.name, d < -1.5 ? std::string("?") : d < 0.0 ? std::string("\u2026") : time_text(d), m_session.favorite(s.path) ? "1" : "0", m_session.keybind_of(s.path),
            e.is_default() ? "0" : "1", s.extension, s.video ? "1" : "0", fs::path(s.folder).filename().string() });
    }

    std::string browse(const std::string& folder_in, const std::string& mode, const std::string& query) {
        rustle::Library& lib = m_session.library();
        const rustle::Settings& st = m_session.settings();
        std::string out;
        std::vector<rustle::SoundEntry> sounds;
        if (mode == "search" && !query.empty()) {
            out += row({ "view", "search", "", "Results for “" + query + "”" });
            sounds = lib.search(query, 400);
            for (rustle::SoundEntry& s : sounds) (void)s;
        } else if (mode == "favorites") {
            out += row({ "view", "favorites", "", "Favorites" });
            for (const rustle::SoundEntry& s : lib.all_sounds()) {
                if (st.favorites.count(s.path)) sounds.push_back(s);
            }
            lib.sort(sounds, rustle::sort_order_from(st.sort), st.play_counts, st.favorites, false);
        } else if (mode == "recent") {
            out += row({ "view", "recent", "", "Most played" });
            for (const rustle::SoundEntry& s : lib.all_sounds()) {
                if (st.play_counts.count(s.path)) sounds.push_back(s);
            }
            lib.sort(sounds, rustle::SortOrder::played, st.play_counts, st.favorites, false);
            if (sounds.size() > 60) sounds.resize(60);
        } else {
            std::string folder = rustle::normalize_folder(folder_in);
            if (!folder.empty() && !lib.has_folder(folder)) folder.clear();
            std::string title = "Library";
            const std::vector<rustle::Crumb> crumbs = lib.breadcrumbs(folder);
            if (!crumbs.empty()) title = crumbs.back().label;
            out += row({ "view", "folder", folder, title, lib.parent_of(folder) });
            for (const rustle::Crumb& c : crumbs) out += row({ "crumb", c.label, c.path });
            for (const rustle::FolderEntry& f : lib.subfolders(folder)) out += row({ "dir", f.path, f.name, std::to_string(f.sounds), std::to_string(f.folders) });
            if (!folder.empty()) {
                sounds = lib.sounds_in(folder);
                lib.sort(sounds, rustle::sort_order_from(st.sort), st.play_counts, st.favorites, st.favorites_first);
            }
        }
        m_shown.clear();
        for (const rustle::SoundEntry& s : sounds) {
            m_shown.push_back(s.path);
            out += sound_row(s);
        }
        return out;
    }

    std::string sound_states() {
        std::string out;
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < m_shown.size(); ++i) index.emplace(m_shown[i], i);
        std::string now;
        std::size_t count = 0;
        for (const rustle::PlayingSound& p : m_session.playing()) {
            ++count;
            const double frac = p.length > 0.0 ? std::clamp(p.position / p.length, 0.0, 1.0) : 0.0;
            if (now.empty()) {
                rustle::SoundEntry e;
                now = m_session.library().find(p.tag, e) ? e.name : rustle::display_name(fs::path(p.tag).filename().string());
                if (p.loop) now += " (looping)";
                else now += "  " + time_text(std::max(0.0, p.length - p.position)) + " left";
            }
            auto it = index.find(p.tag);
            if (it != index.end()) out += row({ std::to_string(it->second), "playing", number(std::floor(frac * 200.0) / 2.0, 1), time_text(std::max(0.0, p.length - p.position)) });
        }
        for (std::size_t i = 0; i < m_shown.size(); ++i) {
            if (m_session.loading(m_shown[i])) out += row({ std::to_string(i), "loading", "0", "" });
        }
        if (count == 0) now = "Nothing playing";
        else if (count > 1) now = std::to_string(count) + " sounds playing: " + now;
        else now = "Playing " + now;
        out += row({ "now", now });
        return out;
    }

    static std::string meter_percent(float peak) {
        if (!(peak > 1e-6f)) return "0";
        const double db = 20.0 * std::log10(static_cast<double>(peak));
        const double pct = std::clamp((db + 60.0) / 60.0 * 100.0, 0.0, 100.0);
        return number(pct >= 99.0 ? 100.0 : std::floor(pct / 2.0) * 2.0, 0);
    }

    std::string audio_settings() {
        const rustle::Settings& s = m_session.settings();
        std::string out;
        auto put = [&](const std::string& k, const std::string& v) { out += row({ k, v }); };
        put("auto_mode", s.auto_mode ? "1" : "0");
        put("virtual_mic", s.virtual_mic ? "1" : "0");
        put("keep_virtual_mic", s.keep_virtual_mic ? "1" : "0");
        put("input_device", s.input_device);
        put("monitor_device", s.monitor_device);
        put("send_device", s.send_device);
        put("mic_enabled", s.mic_enabled ? "1" : "0");
        put("push_to_talk", s.push_to_talk ? "1" : "0");
        put("hear_myself", s.hear_myself ? "1" : "0");
        put("sounds_to_me", s.sounds_to_me ? "1" : "0");
        put("sounds_to_others", s.sounds_to_others ? "1" : "0");
        put("mic_volume", number(s.mic_volume, 2));
        put("sounds_volume", number(s.sounds_volume, 2));
        put("others_volume", number(s.others_volume, 2));
        put("me_volume", number(s.me_volume, 2));
        put("hear_myself_volume", number(s.hear_myself_volume, 2));
        put("overlap", s.overlap ? "1" : "0");
        put("sort", s.sort);
        put("view", s.view);
        put("favorites_first", s.favorites_first ? "1" : "0");
        put("last_folder", s.last_folder);
        put("browser_path", s.browser_path.empty() ? rustle::home_folder() : s.browser_path);
        put("show_hidden", s.show_hidden ? "1" : "0");
        put("rescan_seconds", number(s.rescan_seconds, 0));
        put("global_hotkeys", s.global_hotkeys ? "1" : "0");
        put("key_stop_all", s.key_stop_all);
        put("key_mic_toggle", s.key_mic_toggle);
        put("key_push_to_talk", s.key_push_to_talk);
        put("voice_preset", s.voice_preset);
        put("config_dir", m_session.settings_path().parent_path().string());
        put("control_socket", rustle::control_supported() ? rustle::control_socket_path() : std::string());
        put("control_running", m_session.control_running() ? "1" : "0");
        put("session_kind", rustle::desktop_session_kind());
        put("platform_vmic", rustle::VirtualMic::platform_supported() ? "1" : "0");
        put("backend", fa::backend_name());
        put("start_page", env("RUSTLE_START_PAGE"));
        return out;
    }

    std::string set_setting(const std::string& key, const std::string& value) {
        rustle::Settings& s = m_session.settings();
        const bool on = truthy(value);
        bool restart = false;
        bool levels = false;
        if (key == "auto_mode") {
            m_session.set_auto_mode(on);
            m_session.save();
            return "ok";
        } else if (key == "virtual_mic") {
            s.virtual_mic = on;
            if (!on) s.auto_mode = false;
            restart = true;
        } else if (key == "keep_virtual_mic") {
            s.keep_virtual_mic = on;
        } else if (key == "input_device") {
            s.input_device = value;
            s.auto_mode = false;
            restart = true;
        } else if (key == "monitor_device") {
            s.monitor_device = value;
            s.auto_mode = false;
            restart = true;
        } else if (key == "send_device") {
            s.send_device = value;
            restart = true;
        } else if (key == "mic_enabled") {
            m_session.set_mic_enabled(on);
        } else if (key == "push_to_talk") {
            s.push_to_talk = on;
            levels = true;
        } else if (key == "hear_myself") {
            s.hear_myself = on;
            levels = true;
        } else if (key == "sounds_to_me") {
            s.sounds_to_me = on;
            levels = true;
        } else if (key == "sounds_to_others") {
            s.sounds_to_others = on;
            levels = true;
        } else if (key == "mic_volume") {
            s.mic_volume = static_cast<float>(std::clamp(to_number(value, 1.0), 0.0, 4.0));
            levels = true;
        } else if (key == "sounds_volume") {
            s.sounds_volume = static_cast<float>(std::clamp(to_number(value, 1.0), 0.0, 4.0));
            levels = true;
        } else if (key == "others_volume") {
            s.others_volume = static_cast<float>(std::clamp(to_number(value, 1.0), 0.0, 4.0));
            levels = true;
        } else if (key == "me_volume") {
            s.me_volume = static_cast<float>(std::clamp(to_number(value, 0.8), 0.0, 4.0));
            levels = true;
        } else if (key == "hear_myself_volume") {
            s.hear_myself_volume = static_cast<float>(std::clamp(to_number(value, 0.7), 0.0, 4.0));
            levels = true;
        } else if (key == "overlap") {
            s.overlap = on;
        } else if (key == "sort") {
            if (value != "name" && value != "duration" && value != "newest" && value != "played") return "error\tunknown sort order";
            s.sort = value;
        } else if (key == "view") {
            s.view = value == "list" ? "list" : "tiles";
        } else if (key == "favorites_first") {
            s.favorites_first = on;
        } else if (key == "last_folder") {
            s.last_folder = value;
        } else if (key == "browser_path") {
            s.browser_path = value;
        } else if (key == "show_hidden") {
            s.show_hidden = on;
        } else if (key == "rescan_seconds") {
            s.rescan_seconds = std::clamp(to_number(value, 3.0), 0.0, 3600.0);
        } else if (key == "global_hotkeys") {
            s.global_hotkeys = on;
            m_session.apply_hotkeys();
        } else if (key == "key_stop_all" || key == "key_mic_toggle" || key == "key_push_to_talk") {
            const std::string chord = value.empty() ? std::string() : rustle::normalize_chord(value);
            if (!value.empty() && chord.empty()) return "error\t" + value + " isn't a key Rustle knows";
            const std::string owner = chord.empty() ? std::string() : m_session.chord_owner(chord);
            const std::string mine = key == "key_stop_all" ? "stop-all" : key == "key_mic_toggle" ? "toggle-mic" : "ptt";
            if (!owner.empty() && owner != mine) return "error\t" + chord + " is already used for something else";
            if (key == "key_stop_all") s.key_stop_all = chord;
            else if (key == "key_mic_toggle") s.key_mic_toggle = chord;
            else s.key_push_to_talk = chord;
            m_session.apply_hotkeys();
        } else {
            return "error\tno setting called " + key;
        }
        if (levels) m_session.apply_levels();
        if (restart) m_session.apply_audio();
        std::string why;
        if (!m_session.save(&why)) return "error\t" + why;
        return "ok";
    }

    std::string route_text() {
        const rustle::RouteStatus r = m_session.route_status();
        std::string out;
        auto put = [&](const std::string& k, const std::string& v) { out += row({ k, v }); };
        put("mode", r.mode);
        put("vmic_supported", r.virtual_mic_supported ? "1" : "0");
        put("vmic_wanted", r.virtual_mic_wanted ? "1" : "0");
        put("vmic_ready", r.virtual_mic_ready ? "1" : "0");
        put("vmic_message", r.virtual_mic_message);
        put("server", r.server);
        put("capture_running", r.capture.running ? "1" : "0");
        put("capture_wanted", r.capture.wanted ? "1" : "0");
        put("capture_device", r.capture.device);
        put("capture_error", r.capture.error);
        put("send_running", r.send.running ? "1" : "0");
        put("send_wanted", r.send.wanted ? "1" : "0");
        put("send_device", r.send_label);
        put("send_error", r.send.error);
        put("monitor_running", r.monitor.running ? "1" : "0");
        put("monitor_wanted", r.monitor.wanted ? "1" : "0");
        put("monitor_device", r.monitor.device);
        put("monitor_error", r.monitor.error);
        return out;
    }

    void register_library_natives() {
        native("version", [](const std::vector<std::string>&) { return std::string(RUSTLE_VERSION_STRING); });
        native("lib_state", [this](const std::vector<std::string>&) {
            rustle::Library& lib = m_session.library();
            return single(row({ std::to_string(lib.revision()), std::to_string(lib.sound_count()), std::to_string(lib.folder_count()), std::to_string(lib.roots().size()) }));
        });
        native("browse", [this](const std::vector<std::string>& f) { return browse(arg(f, 0), arg(f, 1), arg(f, 2)); });
        native("states", [this](const std::vector<std::string>&) { return sound_states(); });
        native("toggle", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.toggle(arg(f, 0), &why)) return "error\t" + clean(why);
            return std::string("ok");
        });
        native("play", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.play(arg(f, 0), &why)) return "error\t" + clean(why);
            return std::string("ok");
        });
        native("stop", [this](const std::vector<std::string>& f) {
            m_session.stop(arg(f, 0));
            return std::string("ok");
        });
        native("stop_all", [this](const std::vector<std::string>&) {
            m_session.stop_all();
            return std::string("ok");
        });
        native("favorite", [this](const std::vector<std::string>& f) {
            m_session.set_favorite(arg(f, 0), truthy(arg(f, 1)));
            return std::string("ok");
        });
        native("rescan", [this](const std::vector<std::string>&) {
            m_session.rescan();
            return std::string("ok");
        });
        native("folders", [this](const std::vector<std::string>&) {
            std::string out;
            for (const rustle::FolderEntry& f : m_session.library().subfolders("")) {
                std::error_code ec;
                const bool there = fs::is_directory(f.path, ec);
                out += row({ f.path, f.name, std::to_string(f.sounds), there ? "1" : "0" });
            }
            return out;
        });
        native("library_targets", [this](const std::vector<std::string>&) {
            std::string out;
            rustle::Library& lib = m_session.library();
            std::vector<std::pair<std::string, int>> stack;
            for (const std::string& r : lib.roots()) stack.emplace_back(r, 0);
            std::reverse(stack.begin(), stack.end());
            while (!stack.empty()) {
                const auto cur = stack.back();
                stack.pop_back();
                const std::vector<rustle::Crumb> crumbs = lib.breadcrumbs(cur.first);
                std::string label;
                for (std::size_t i = 1; i < crumbs.size(); ++i) label += (i > 1 ? " / " : "") + crumbs[i].label;
                if (label.empty()) label = cur.first;
                out += row({ cur.first, label });
                if (cur.second > 6) continue;
                std::vector<rustle::FolderEntry> subs = lib.subfolders(cur.first);
                std::reverse(subs.begin(), subs.end());
                for (const rustle::FolderEntry& s : subs) stack.emplace_back(s.path, cur.second + 1);
            }
            return out;
        });
        native("add_folder", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.add_folder(arg(f, 0), &why)) return "error\t" + clean(why);
            return std::string("ok");
        });
        native("remove_folder", [this](const std::vector<std::string>& f) {
            return std::string(m_session.remove_folder(arg(f, 0)) ? "ok" : "error\tthat folder isn't in the library");
        });
        native("edit_get", [this](const std::vector<std::string>& f) {
            const std::string path = arg(f, 0);
            const rustle::ClipEdit e = m_session.edit_of(path);
            std::string out;
            for (const char* k : k_edit_keys) out += row({ k, edit_value(e, k) });
            rustle::SoundEntry s;
            const bool known = m_session.library().find(path, s);
            out += row({ "name", known ? s.name : rustle::display_name(fs::path(path).filename().string()) });
            out += row({ "file", fs::path(path).filename().string() });
            out += row({ "folder", fs::path(path).parent_path().string() });
            const double d = m_session.library().duration(path);
            out += row({ "duration", number(d == -1.0 ? std::max(0.0, rustle::probe_duration(path)) : std::max(0.0, d), 2) });
            out += row({ "keybind", m_session.keybind_of(path) });
            out += row({ "favorite", m_session.favorite(path) ? "1" : "0" });
            out += row({ "plays", std::to_string(m_session.settings().play_counts.count(path) ? m_session.settings().play_counts.at(path) : 0) });
            return out;
        });
        native("edit_set", [this](const std::vector<std::string>& f) {
            const std::string path = arg(f, 0);
            rustle::ClipEdit e = m_session.edit_of(path);
            if (!set_edit_value(e, arg(f, 1), arg(f, 2))) return std::string("error\tunknown edit setting");
            m_session.set_edit(path, e);
            return std::string("ok\t") + edit_value(m_session.edit_of(path), arg(f, 1));
        });
        native("edit_reset", [this](const std::vector<std::string>& f) {
            m_session.reset_edit(arg(f, 0));
            return std::string("ok");
        });
        native("edit_preview", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.preview(arg(f, 0), m_session.edit_of(arg(f, 0)), &why)) return "error\t" + clean(why);
            return std::string("ok");
        });
        native("keybind_set", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.set_keybind(arg(f, 0), arg(f, 1), &why)) return "error\t" + clean(why);
            return "ok\t" + m_session.keybind_of(arg(f, 0));
        });
        native("key", [this](const std::vector<std::string>& f) {
            const std::string chord = rustle::normalize_chord(arg(f, 0));
            if (chord.empty()) return std::string();
            const std::string owner = m_session.chord_owner(chord);
            if (owner.empty()) return std::string();
            m_session.key_event(chord, truthy(arg(f, 1)), true);
            return owner;
        });
        native("chord", [](const std::vector<std::string>& f) { return rustle::normalize_chord(arg(f, 0)); });
        native("key_up", [this](const std::vector<std::string>& f) {
            rustle::Chord ptt;
            if (rustle::parse_chord(m_session.settings().key_push_to_talk, ptt) && ptt.key == arg(f, 0) && !m_session.hotkey_report().active) m_session.set_talking(false);
            return std::string();
        });
        native("fmt", [](const std::vector<std::string>& f) { return number(to_number(arg(f, 0), 0.0), static_cast<int>(std::clamp(to_number(arg(f, 1), 2.0), 0.0, 6.0))); });
        native("notices", [this](const std::vector<std::string>& f) {
            std::string out;
            const std::uint64_t since = std::strtoull(arg(f, 0).c_str(), nullptr, 10);
            for (const rustle::Notice& n : m_session.notices_since(since)) out += row({ std::to_string(n.seq), n.kind, n.text });
            return out;
        });
    }

    void register_audio_natives() {
        native("settings", [this](const std::vector<std::string>&) { return audio_settings(); });
        native("set", [this](const std::vector<std::string>& f) { return set_setting(arg(f, 0), arg(f, 1)); });
        native("route", [this](const std::vector<std::string>&) { return route_text(); });
        native("restart_audio", [this](const std::vector<std::string>&) {
            m_session.apply_audio();
            return std::string("ok");
        });
        native("devices", [](const std::vector<std::string>& f) {
            const fa::DeviceKind kind = arg(f, 0) == "input" ? fa::DeviceKind::input : fa::DeviceKind::output;
            std::string out;
            for (const fa::DeviceInfo& d : rustle::usable_devices(kind)) out += row({ d.id, d.name, d.is_default ? "1" : "0" });
            return out;
        });
        native("meters", [this](const std::vector<std::string>&) {
            rustle::Engine& e = m_session.engine();
            return single(row({ meter_percent(e.take_mic_level()), meter_percent(e.take_voice_level()), meter_percent(e.take_send_level()), meter_percent(e.take_monitor_level()),
                e.mic_open() ? "1" : "0", e.push_to_talk() ? "1" : "0", e.talking() ? "1" : "0", e.mic_enabled() ? "1" : "0", std::to_string(e.playing().size()),
                g_quit.load() ? "1" : "0" }));
        });
        native("talk", [this](const std::vector<std::string>& f) {
            m_session.set_talking(truthy(arg(f, 0)));
            return std::string("ok");
        });
        native("hotkeys", [this](const std::vector<std::string>&) {
            const rustle::HotkeyReport r = m_session.hotkey_report();
            std::string working, failed;
            for (const std::string& w : r.working) working += (working.empty() ? "" : ", ") + w;
            for (const std::string& w : r.failed) failed += (failed.empty() ? "" : ", ") + w;
            std::string why;
            const bool possible = rustle::GlobalHotkeys::available(&why);
            return single(row({ r.active ? "1" : "0", r.backend, r.message, working, failed, possible ? "1" : "0", why }));
        });
        native("keybinds", [this](const std::vector<std::string>&) {
            std::string out;
            for (const auto& kv : m_session.settings().keybinds) {
                rustle::SoundEntry s;
                const bool known = m_session.library().find(kv.first, s);
                out += row({ kv.first, known ? s.name : rustle::display_name(fs::path(kv.first).filename().string()), kv.second, known ? "1" : "0" });
            }
            return out;
        });
    }

    void register_voice_natives() {
        native("voice_slots", [](const std::vector<std::string>&) {
            std::string out;
            for (const rustle::VoiceSlotInfo& s : rustle::voice_slots()) {
                out += row({ "slot", s.id, s.label, s.group, s.hint });
                for (const rustle::VoiceControl& c : s.controls) {
                    out += row({ "control", s.id, c.id, c.label, c.unit, number(c.low, 4), number(c.high, 4), number(c.step, 4), number(c.fallback, 4), std::to_string(c.decimals), c.choices });
                }
            }
            return out;
        });
        native("voice_state", [this](const std::vector<std::string>&) {
            std::string out;
            rustle::VoiceChain& v = m_session.voice();
            for (const rustle::VoiceSlotInfo& s : rustle::voice_slots()) {
                out += row({ "slot", s.id, v.enabled(s.id) ? "1" : "0" });
                for (const rustle::VoiceControl& c : s.controls) out += row({ "value", c.id, number(v.value(c.id), c.decimals + 2) });
            }
            out += row({ "preset", m_session.settings().voice_preset });
            out += row({ "active", std::to_string(v.active_count()) });
            out += row({ "latency", number(1000.0 * static_cast<double>(v.latency_frames()) / 48000.0, 0) });
            return out;
        });
        native("voice_enable", [this](const std::vector<std::string>& f) {
            if (!m_session.voice().set_enabled(arg(f, 0), truthy(arg(f, 1)))) return std::string("error\tno such effect");
            m_session.voice_changed();
            return std::string("ok");
        });
        native("voice_set", [this](const std::vector<std::string>& f) {
            if (!m_session.voice().set_value(arg(f, 0), static_cast<float>(to_number(arg(f, 1), 0.0)), true)) return std::string("error\tno such control");
            m_session.voice_changed();
            return "ok\t" + number(m_session.voice().value(arg(f, 0)), 4);
        });
        native("voice_reset_control", [this](const std::vector<std::string>& f) {
            m_session.voice().reset_control(arg(f, 0));
            m_session.voice_changed();
            return number(m_session.voice().value(arg(f, 0)), 4);
        });
        native("voice_reset_slot", [this](const std::vector<std::string>& f) {
            m_session.voice().reset_slot(arg(f, 0));
            m_session.voice_changed();
            return std::string("ok");
        });
        native("voice_reset_all", [this](const std::vector<std::string>&) {
            m_session.voice().reset_all();
            m_session.settings().voice_preset.clear();
            m_session.voice_changed();
            return std::string("ok");
        });
        native("presets", [this](const std::vector<std::string>&) {
            std::string out;
            for (const rustle::VoicePresetInfo& p : m_session.voice_presets()) out += row({ p.name, p.kind, p.description });
            return out;
        });
        native("preset_apply", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.apply_voice_preset(arg(f, 0), &why)) return "error\t" + clean(why);
            return why.empty() ? std::string("ok") : "ok\t" + clean(why);
        });
        native("preset_save", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.save_voice_preset(arg(f, 0), &why)) return "error\t" + clean(why);
            return std::string("ok");
        });
        native("preset_rename", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.rename_voice_preset(arg(f, 0), arg(f, 1), &why)) return "error\t" + clean(why);
            return std::string("ok");
        });
        native("preset_delete", [this](const std::vector<std::string>& f) {
            std::string why;
            if (!m_session.delete_voice_preset(arg(f, 0), &why)) return "error\t" + clean(why);
            return std::string("ok");
        });
    }

    void register_file_natives() {
        native("places", [](const std::vector<std::string>&) {
            std::string out;
            for (const rustle::Place& p : rustle::places()) out += row({ p.label, p.path, p.kind });
            return out;
        });
        native("list_dir", [this](const std::vector<std::string>& f) {
            std::string path = arg(f, 0);
            if (path.empty()) path = rustle::home_folder();
            path = rustle::normalize_folder(path);
            std::vector<rustle::FileItem> items;
            std::string why;
            if (!rustle::list_folder(path, truthy(arg(f, 1)), items, &why)) return "error\t" + clean(why);
            const std::vector<std::string> roots = m_session.library().roots();
            bool in_library = false;
            for (const std::string& r : roots) in_library = in_library || rustle::path_inside(path, r);
            std::string out = row({ "ok", path, rustle::parent_folder(path), in_library ? "1" : "0" });
            for (const auto& part : rustle::path_parts(path)) out += row({ "crumb", part.first, part.second });
            std::size_t sounds = 0;
            for (const rustle::FileItem& i : items) {
                if (i.sound) ++sounds;
                out += row({ "item", i.name, i.path, i.folder ? "1" : "0", i.sound ? "1" : "0", i.folder ? std::string() : rustle::format_size(i.size), i.hidden ? "1" : "0" });
            }
            out += row({ "count", std::to_string(items.size()), std::to_string(sounds) });
            return out;
        });
        native("copy_into", [this](const std::vector<std::string>& f) {
            const std::string dest = arg(f, 0);
            std::vector<std::string> files;
            for (std::size_t i = 1; i < f.size(); ++i) {
                if (!f[i].empty()) files.push_back(f[i]);
            }
            if (files.empty()) return std::string("error\tpick some sound files first");
            std::vector<std::string> copied;
            std::string why;
            rustle::copy_into(files, dest, copied, &why);
            m_session.rescan();
            if (copied.empty()) return "error\t" + clean(why);
            return "ok\t" + std::to_string(copied.size()) + "\t" + clean(why);
        });
        native("mkdir", [this](const std::vector<std::string>& f) {
            std::string made, why;
            if (!rustle::make_folder(arg(f, 0), arg(f, 1), made, &why)) return "error\t" + clean(why);
            m_session.rescan();
            return "ok\t" + made;
        });
        native("home", [](const std::vector<std::string>&) { return rustle::home_folder(); });
    }

    void register_natives() {
        register_library_natives();
        register_audio_natives();
        register_voice_natives();
        register_file_natives();
    }

    grove::Document& m_doc;
    rustle::Session& m_session;
    std::vector<std::string> m_shown;
    std::atomic<bool> m_beating{ false };
    std::thread m_heartbeat;
};

int bundle_ui(const std::string& output, bool portable) {
    const std::string ui = find_ui();
    if (ui.empty()) {
        std::cerr << "rustle: can't find the UI (rustle.grove) to bundle; set RUSTLE_UI_DIR to the folder that holds it\n";
        return 1;
    }
    grove::AppBuildOptions options;
    options.output = output;
    options.stub = grove::executable_path();
    options.tools = grove::default_script_tools(ui);
    windows_static_scripts(options.tools);
    options.portable = portable;
    grove::Diagnostics diagnostics;
    const grove::AppBuildResult result = grove::build_app(ui, options, diagnostics, &std::cout);
    if (!result.ok) {
        diagnostics.print(std::cerr);
        std::cerr << "rustle: couldn't bundle the UI into " << output << "\n";
        return 1;
    }
    std::cout << "rustle: wrote " << result.output << " (" << result.files << " files, " << result.modules << " compiled scripts)\n";
    return 0;
}

int audio_check() {
    rustle::register_rustle_effects();
    rustle::VoiceChain chain;
    chain.prepare(48000, 1, 1024);
    std::vector<float> probe(1024);
    for (std::size_t i = 0; i < probe.size(); ++i) probe[i] = 0.25f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
    std::vector<float> copy = probe;
    chain.process(copy.data(), copy.size());
    const bool transparent = copy == probe;
    std::string vmic;
    rustle::VirtualMic mic;
    if (!rustle::VirtualMic::platform_supported()) vmic = "not on this system";
    else if (mic.library_available(&vmic)) vmic = "libpulse found";
    std::cout << "rustle: audio backend " << fa::backend_name() << ", " << rustle::usable_devices(fa::DeviceKind::output).size() << " output(s), "
              << rustle::usable_devices(fa::DeviceKind::input).size() << " input(s); voice chain " << chain.slot_count() << " effects ("
              << (transparent ? "neutral is transparent" : "NOT transparent") << "), " << rustle::VoiceChain::builtin_presets().size() << " presets; virtual mic: " << vmic << "\n";
    return transparent ? 0 : 1;
}

int cli_devices() {
    std::cout << "audio backend: " << fa::backend_name() << "\n";
    for (fa::DeviceKind k : { fa::DeviceKind::output, fa::DeviceKind::input }) {
        std::cout << (k == fa::DeviceKind::output ? "outputs:\n" : "inputs:\n");
        for (const fa::DeviceInfo& d : fa::devices(k)) std::cout << "  " << (d.is_default ? "* " : "  ") << d.id << "  (" << d.name << ")\n";
    }
    return 0;
}

bool parse_edit_flags(int argc, char** argv, int from, rustle::ClipEdit& e, std::string& device, bool& to_mic) {
    for (int i = from; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](double& out) {
            if (i + 1 >= argc) return false;
            out = to_number(argv[++i], out);
            return true;
        };
        double v = 0.0;
        if (a == "--speed" && value(v)) e.speed = static_cast<float>(v);
        else if (a == "--tempo" && value(v)) e.tempo = static_cast<float>(v);
        else if (a == "--pitch" && value(v)) e.pitch = static_cast<float>(v);
        else if (a == "--reverb" && value(v)) e.reverb = static_cast<float>(v);
        else if (a == "--volume" && value(v)) e.volume = static_cast<float>(v);
        else if (a == "--start" && value(v)) e.trim_start = v;
        else if (a == "--end" && value(v)) e.trim_end = v;
        else if (a == "--reverse") e.reverse = true;
        else if (a == "--loop") e.loop = true;
        else if (a == "--device" && i + 1 < argc) device = argv[++i];
        else if (a == "--to-mic") to_mic = true;
        else {
            std::cerr << "rustle: unknown option " << a << "\n";
            return false;
        }
    }
    e.clamp();
    return true;
}

int cli_play(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: rustle play FILE [--device ID] [--to-mic] [--speed X] [--tempo X] [--pitch ST] [--reverse] [--reverb MIX] [--volume X] [--start S] [--end S]\n";
        return 2;
    }
    rustle::ClipEdit e;
    std::string device;
    bool to_mic = false;
    if (!parse_edit_flags(argc, argv, 3, e, device, to_mic)) return 2;
    e.loop = false;
    fa::AudioBuffer source;
    std::string why;
    if (!rustle::decode_sound(argv[2], source, &why)) {
        std::cerr << "rustle: " << why << "\n";
        return 1;
    }
    const fa::Sound sound = fa::make_sound(rustle::render_clip(source, e, 48000, 2));
    rustle::VirtualMic mic;
    rustle::EngineConfig c;
    c.capture = false;
    c.monitor = !to_mic;
    c.monitor_device = device;
    c.send = to_mic;
    if (to_mic) {
        if (!mic.setup(&why)) {
            std::cerr << "rustle: " << why << "\n";
            return 1;
        }
        c.send_device = rustle::VirtualMic::sink_name;
    }
    rustle::Engine engine;
    if (!engine.start(c, &why)) {
        std::cerr << "rustle: " << why << "\n";
        if (to_mic) mic.teardown(&why);
        return 1;
    }
    const std::uint64_t id = engine.play(sound, e.volume, false, argv[2]);
    std::cout << "rustle: playing " << fs::path(argv[2]).filename().string() << " (" << number(sound->duration(), 2) << " s)" << (to_mic ? " into Rustle Mic" : "") << "\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(sound->duration() + 5.0);
    while (engine.is_playing(id) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    engine.stop();
    if (to_mic) mic.teardown(&why);
    return 0;
}

int cli_render(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: rustle render IN OUT.wav [--speed X] [--tempo X] [--pitch ST] [--reverse] [--reverb MIX] [--volume X] [--start S] [--end S]\n";
        return 2;
    }
    rustle::ClipEdit e;
    std::string device;
    bool to_mic = false;
    if (!parse_edit_flags(argc, argv, 4, e, device, to_mic)) return 2;
    fa::AudioBuffer source;
    std::string why;
    if (!rustle::decode_sound(argv[2], source, &why)) {
        std::cerr << "rustle: " << why << "\n";
        return 1;
    }
    fa::AudioBuffer out = rustle::render_clip(source, e, source.sample_rate(), source.channels());
    out.apply_gain(e.volume);
    if (!fa::save_audio(argv[3], out, fa::SaveOptions(), &why)) {
        std::cerr << "rustle: " << why << "\n";
        return 1;
    }
    std::cout << "rustle: wrote " << argv[3] << " (" << number(out.duration(), 2) << " s)\n";
    return 0;
}

int cli_voice(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: rustle voice IN OUT.wav [--preset NAME] [--set CONTROL=VALUE]...\n";
        return 2;
    }
    rustle::VoiceChain chain;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        std::string why;
        if (a == "--preset" && i + 1 < argc) {
            if (!chain.apply_builtin(argv[++i], &why)) {
                std::cerr << "rustle: " << why << "\n";
                return 2;
            }
        } else if (a == "--set" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const std::size_t eq = kv.find('=');
            if (eq == std::string::npos || !chain.set_value(kv.substr(0, eq), static_cast<float>(to_number(kv.substr(eq + 1), 0.0)), true)) {
                std::cerr << "rustle: can't set " << kv << "\n";
                return 2;
            }
        } else {
            std::cerr << "rustle: unknown option " << a << "\n";
            return 2;
        }
    }
    fa::AudioBuffer in;
    std::string why;
    if (!rustle::decode_sound(argv[2], in, &why)) {
        std::cerr << "rustle: " << why << "\n";
        return 1;
    }
    fa::AudioBuffer mono = in.channels() == 1 ? in : in.with_channels(1);
    chain.prepare(mono.sample_rate(), 1, 1024);
    chain.process(mono.data(), mono.frames());
    if (!fa::save_audio(argv[3], mono, fa::SaveOptions(), &why)) {
        std::cerr << "rustle: " << why << "\n";
        return 1;
    }
    std::cout << "rustle: wrote " << argv[3] << " with " << chain.active_count() << " effect(s)\n";
    return 0;
}

int cli_ctl(int argc, char** argv) {
    std::string command;
    for (int i = 2; i < argc; ++i) command += (i > 2 ? " " : "") + std::string(argv[i]);
    if (command.empty()) command = "status";
    std::string reply, why;
    if (!rustle::send_control(command, reply, &why)) {
        std::cerr << "rustle: " << why << "\n";
        return 1;
    }
    const bool ok = reply.compare(0, 2, "ok") == 0;
    for (char& c : reply) {
        if (c == '\t') c = ' ';
    }
    (ok ? std::cout : std::cerr) << reply << "\n";
    return ok ? 0 : 1;
}

void usage() {
    std::cout << "usage: rustle [--check] [--bundle OUTPUT [--portable]]\n"
                 "       rustle play FILE [--device ID] [--to-mic] [edits]\n"
                 "       rustle render IN OUT.wav [edits]\n"
                 "       rustle voice IN OUT.wav [--preset NAME] [--set CONTROL=VALUE]\n"
                 "       rustle devices\n"
                 "       rustle ctl COMMAND\n\n"
                 "  --check          load the window and its script without opening it, check the audio code, then exit\n"
                 "  --bundle OUTPUT  write a copy of rustle with the window and its compiled script built in\n"
                 "  --portable       with --bundle, link the script with the static C++ runtime (when the compiler has one)\n"
                 "  play             play a sound file to your speakers (or with --to-mic into Rustle Mic) and wait for it\n"
                 "  render           apply sound edits to a file and save the result as WAV, FLAC or MP3\n"
                 "  voice            run a recording through the voice changer and save the result\n"
                 "  devices          list the sound devices Rustle can see\n"
                 "  ctl              tell the running Rustle to do something: stop-all, toggle-mic, mic-on, mic-off,\n"
                 "                   ptt-down, ptt-up, play NAME, toggle NAME, stop NAME, preset NAME, voice-off, status\n\n"
                 "edits: --speed X --tempo X --pitch ST --reverse --reverb MIX --volume X --start S --end S\n\n"
                 "Set RUSTLE_UI_DIR to use a different UI folder, RUSTLE_CONFIG_DIR to keep settings somewhere else,\n"
                 "and FIZMO_AUDIO_BACKEND=null to run without a sound card.\n";
}

} // namespace

int main(int argc, char** argv) {
    bool check = false;
    if (argc >= 2) {
        const std::string cmd = argv[1];
        if (cmd == "play") return cli_play(argc, argv);
        if (cmd == "render") return cli_render(argc, argv);
        if (cmd == "voice") return cli_voice(argc, argv);
        if (cmd == "devices") return cli_devices();
        if (cmd == "ctl") return cli_ctl(argc, argv);
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version") {
            std::cout << "rustle " << RUSTLE_VERSION_STRING << "\n";
            return 0;
        }
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        }
        if (a == "--bundle") {
            if (i + 1 >= argc) {
                std::cerr << "rustle: --bundle needs an output path\n";
                return 2;
            }
            bool portable = false;
            for (int j = 1; j < argc; ++j) portable = portable || std::string(argv[j]) == "--portable";
            return bundle_ui(argv[i + 1], portable);
        }
        if (a == "--check") check = true;
        else if (a != "--portable") {
            std::cerr << "rustle: unknown option " << a << " (try --help)\n";
            return 2;
        }
    }

    grove::LoadedApp loaded;
    grove::Project from_files;
    grove::Project* project = nullptr;
    grove::runtime::RunOptions options;
    std::optional<grove::Bundle> bundle;
    if (env("RUSTLE_UI_DIR").empty()) bundle = grove::read_embedded_bundle(grove::executable_path());
    if (bundle) {
        std::string error;
        if (!grove::load_app(*bundle, loaded, error)) {
            std::cerr << "rustle: " << error << "\n";
            return 1;
        }
        project = &loaded.project;
        options.window_id = loaded.window_id;
        options.prebuilt = &loaded.scripts;
        if (check) {
            std::cout << "rustle " << RUSTLE_VERSION_STRING << ": built-in window ok, " << loaded.scripts.runnable_count() << " compiled script(s)\n";
            const int audio = audio_check();
            return loaded.scripts.ok() && audio == 0 ? 0 : 1;
        }
    } else {
        const std::string ui = find_ui();
        if (ui.empty()) {
            std::cerr << "rustle: can't find the UI (rustle.grove); set RUSTLE_UI_DIR to the folder that holds it\n";
            return 1;
        }
        from_files = grove::load_project(ui);
        if (!from_files.diagnostics.ok()) {
            from_files.diagnostics.print(std::cerr);
            return 1;
        }
        if (check) {
            std::cout << "rustle " << RUSTLE_VERSION_STRING << ": window ok (" << ui << ")\n";
            return audio_check();
        }
        project = &from_files;
        options.tools = grove::default_script_tools(ui);
        windows_static_scripts(options.tools);
        options.tools.cache_dir = (rustle::Settings::config_dir() / "script-cache").string();
    }
    if (rustle::control_supported()) {
        std::string reply, why;
        if (rustle::send_control("status", reply, &why) && reply.compare(0, 2, "ok") == 0) {
            std::cout << "rustle: Rustle is already running; use its window, or 'rustle ctl' to control it\n";
            return 0;
        }
    }
    std::signal(SIGINT, on_quit_signal);
    std::signal(SIGTERM, on_quit_signal);
#if defined(SIGHUP)
    std::signal(SIGHUP, on_quit_signal);
#endif
    rustle::Session session;
    session.open();
    int result = 0;
    {
        auto app = std::make_unique<App>(project->document, session);
        app->install();
        result = grove::runtime::run(*project, options);
    }
    session.close();
    return result;
}
