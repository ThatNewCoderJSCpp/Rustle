#include <rustle/session.hpp>
#include <rustle/json.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;
namespace fa = fizmo::audio;

namespace rustle {

namespace {

constexpr std::uint32_t k_rate = 48000;

std::string chord_action_label(const std::string& owner) {
    if (owner == "stop-all") return "Stop all";
    if (owner == "ptt") return "Push to talk";
    if (owner == "toggle-mic") return "Mic on/off";
    if (owner == "music-toggle") return "Music play/pause";
    if (owner == "music-next") return "Next song";
    if (owner == "music-previous") return "Previous song";
    return owner;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

} // namespace

std::string music_state_name(MusicState state) {
    switch (state) {
    case MusicState::loading:
        return "loading";
    case MusicState::playing:
        return "playing";
    case MusicState::paused:
        return "paused";
    case MusicState::stopped:
        break;
    }
    return "stopped";
}

Session::Session(SessionOptions options) : m_options(std::move(options)) {
    if (m_options.config_dir.empty()) m_options.config_dir = Settings::config_dir();
}

Session::~Session() {
    close();
}

fs::path Session::settings_path() const {
    return m_options.config_dir / "settings.json";
}

fs::path Session::presets_path() const {
    return m_options.config_dir / "presets";
}

fs::path Session::music_path() const {
    return m_options.config_dir / "music.json";
}

fs::path Session::playlists_path() const {
    return m_options.config_dir / "playlists.json";
}

void Session::open() {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (m_open) return;
    m_open = true;
    m_load = load_settings(settings_path(), m_settings);
    if (!m_load.ok) notice("warning", "Your settings couldn't be read: " + m_load.message);
    if (!m_settings.voice_json.empty()) {
        std::string why;
        if (!voice().load_json(m_settings.voice_json, &why) && !why.empty()) notice("warning", "Your voice settings couldn't be restored: " + why);
    }
    m_library.set_roots(m_settings.folders);
    load_music();
    if (m_options.worker) start_worker();
    rescan();
    if (m_options.audio) apply_audio();
    else m_engine.prepare_offline(k_rate);
    apply_levels();
    if (m_options.hotkeys) apply_hotkeys();
    if (m_options.control) {
        std::string why;
        if (!m_control.start([this](const std::string& line) { return control_command(line); }, &why) && control_supported()) {
            notice("info", "'rustle ctl' commands won't reach this window: " + why);
        }
    }
    m_last_scan_check = std::chrono::steady_clock::now();
    m_last_heal = m_last_scan_check;
    m_last_save = m_last_scan_check;
    m_last_music_save = m_last_scan_check;
}

void Session::close() {
    std::unique_lock<std::recursive_mutex> lock(m_api);
    if (!m_open) return;
    m_open = false;
    lock.unlock();
    m_control.stop();
    m_hotkeys.stop();
    stop_worker();
    lock.lock();
    {
        const MusicStatus st = m_engine.music_status();
        if (st.active) m_music_at = st.position;
    }
    m_engine.stop();
    if (m_options.virtual_mic) {
        if (m_settings.keep_virtual_mic) {
            m_vmic.keep();
        } else {
            std::string why;
            m_vmic.teardown(&why);
        }
    }
    std::string why;
    save(&why);
    save_music(&why);
}

bool Session::save(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_settings.voice_json = voice().save_json("Current voice");
    m_dirty = false;
    m_last_save = std::chrono::steady_clock::now();
    return save_settings(settings_path(), m_settings, error);
}

void Session::start_worker() {
    std::lock_guard<std::mutex> lock(m_job_mutex);
    if (m_worker_run) return;
    m_worker_run = true;
    m_worker = std::thread([this] {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_job_mutex);
                m_job_cv.wait_for(lock, std::chrono::milliseconds(500), [this] { return !m_worker_run || !m_jobs.empty(); });
                if (!m_worker_run) return;
                if (m_jobs.empty()) {
                    lock.unlock();
                    probe_durations();
                    continue;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            job();
        }
    });
}

void Session::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        m_worker_run = false;
        m_jobs.clear();
    }
    m_job_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void Session::post(std::function<void()> job) {
    bool inline_run = false;
    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        if (m_worker_run) m_jobs.push_back(std::move(job));
        else inline_run = true;
    }
    if (inline_run) job();
    else m_job_cv.notify_one();
}

void Session::probe_durations() {
    const std::vector<std::string> missing = m_library.missing_durations(8);
    for (const std::string& p : missing) {
        {
            std::lock_guard<std::mutex> lock(m_job_mutex);
            if (!m_worker_run || !m_jobs.empty()) return;
        }
        m_library.set_duration(p, probe_duration(p));
    }
}

void Session::rescan() {
    post([this] { m_library.rescan(); });
}

bool Session::add_folder(const std::string& path, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    const std::string n = normalize_folder(path);
    std::error_code ec;
    if (n.empty() || !fs::is_directory(n, ec)) {
        if (error) *error = path + " isn't a folder";
        return false;
    }
    for (const std::string& f : m_settings.folders) {
        if (normalize_folder(f) == n) {
            if (error) *error = "that folder is already in the library";
            return false;
        }
        if (path_inside(n, normalize_folder(f))) {
            if (error) *error = "that folder is already in the library, inside " + f;
            return false;
        }
    }
    m_settings.folders.push_back(n);
    m_library.set_roots(m_settings.folders);
    rescan();
    save();
    return true;
}

bool Session::remove_folder(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    const std::string n = normalize_folder(path);
    auto it = std::find_if(m_settings.folders.begin(), m_settings.folders.end(), [&](const std::string& f) { return normalize_folder(f) == n; });
    if (it == m_settings.folders.end()) return false;
    m_settings.folders.erase(it);
    m_library.set_roots(m_settings.folders);
    if (path_inside(normalize_folder(m_settings.last_folder), n)) m_settings.last_folder.clear();
    rescan();
    save();
    return true;
}

ClipEdit Session::edit_of(const std::string& path) const {
    auto it = m_settings.edits.find(path);
    return it == m_settings.edits.end() ? ClipEdit() : it->second;
}

void Session::set_edit(const std::string& path, const ClipEdit& edit) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    ClipEdit e = edit;
    e.clamp();
    if (e.is_default()) m_settings.edits.erase(path);
    else m_settings.edits[path] = e;
    m_dirty = true;
}

void Session::reset_edit(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_settings.edits.erase(path);
    m_dirty = true;
}

bool Session::favorite(const std::string& path) const {
    return m_settings.favorites.count(path) != 0;
}

void Session::set_favorite(const std::string& path, bool on) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (on) m_settings.favorites.insert(path);
    else m_settings.favorites.erase(path);
    m_dirty = true;
}

std::string Session::keybind_of(const std::string& path) const {
    auto it = m_settings.keybinds.find(path);
    return it == m_settings.keybinds.end() ? std::string() : it->second;
}

std::string Session::chord_owner(const std::string& chord) const {
    const std::string c = normalize_chord(chord);
    if (c.empty()) return std::string();
    if (normalize_chord(m_settings.key_stop_all) == c) return "stop-all";
    if (normalize_chord(m_settings.key_mic_toggle) == c) return "toggle-mic";
    if (normalize_chord(m_settings.key_push_to_talk) == c) return "ptt";
    if (normalize_chord(m_settings.key_music_toggle) == c) return "music-toggle";
    if (normalize_chord(m_settings.key_music_next) == c) return "music-next";
    if (normalize_chord(m_settings.key_music_previous) == c) return "music-previous";
    for (const auto& kv : m_settings.keybinds) {
        if (normalize_chord(kv.second) == c) return "play:" + kv.first;
    }
    return std::string();
}

bool Session::set_keybind(const std::string& path, const std::string& chord, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (chord.empty()) {
        m_settings.keybinds.erase(path);
        m_dirty = true;
        apply_hotkeys();
        return true;
    }
    const std::string c = normalize_chord(chord);
    if (c.empty()) {
        if (error) *error = chord + " isn't a key Rustle knows";
        return false;
    }
    const std::string owner = chord_owner(c);
    if (!owner.empty() && owner != "play:" + path) {
        if (error) {
            if (owner.compare(0, 5, "play:") == 0) {
                SoundEntry e;
                const std::string other = owner.substr(5);
                *error = c + " already plays " + (m_library.find(other, e) ? e.name : fs::path(other).filename().string());
            } else {
                *error = c + " is already used for " + chord_action_label(owner);
            }
        }
        return false;
    }
    m_settings.keybinds[path] = c;
    m_dirty = true;
    apply_hotkeys();
    return true;
}

void Session::record_play(const std::string& path) {
    ++m_settings.play_counts[path];
    m_dirty = true;
}

bool Session::start_play(const std::string& path, const ClipEdit& edit, bool count, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        if (error) *error = fs::path(path).filename().string() + " isn't there any more";
        return false;
    }
    if (!m_settings.overlap) m_engine.stop_all(0.03);
    if (count) record_play(path);
    const std::size_t channels = 2;
    fa::Sound ready = m_cache.peek(path, edit, k_rate, channels);
    if (ready) {
        if (!m_engine.play(ready, edit.volume, edit.loop, path)) {
            if (error) *error = "nothing is playing sound right now; check the Output page";
            return false;
        }
        return true;
    }
    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_loading.insert(path);
        ticket = ++m_ticket;
        m_tickets[path] = ticket;
    }
    post([this, path, edit, ticket] {
        std::string why;
        fa::Sound sound = m_cache.get(path, edit, k_rate, 2, &why);
        bool go = false;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto t = m_tickets.find(path);
            go = t != m_tickets.end() && t->second == ticket;
            if (go) {
                m_tickets.erase(t);
                m_loading.erase(path);
            }
        }
        if (!go) return;
        if (!sound) {
            notice("error", "Couldn't play " + fs::path(path).filename().string() + ": " + why);
            return;
        }
        if (!m_engine.play(sound, edit.volume, edit.loop, path)) notice("error", "Nothing is playing sound right now; check the Output page.");
    });
    return true;
}

bool Session::play(const std::string& path, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    return start_play(path, edit_of(path), true, error);
}

bool Session::preview(const std::string& path, const ClipEdit& edit, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    stop(path);
    return start_play(path, edit, false, error);
}

bool Session::toggle(const std::string& path, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (playing(path) || loading(path)) {
        stop(path);
        return true;
    }
    return play(path, error);
}

void Session::stop(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_tickets.erase(path);
        m_loading.erase(path);
    }
    m_engine.stop_tag(path, 0.03);
}

void Session::stop_all() {
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_tickets.clear();
        m_loading.clear();
    }
    m_engine.stop_all(0.03);
}

bool Session::loading(const std::string& path) const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_loading.count(path) != 0;
}

bool Session::playing(const std::string& path) const {
    return m_engine.tag_playing(path);
}

std::vector<PlayingSound> Session::playing() const {
    return m_engine.playing();
}

bool Session::find_by_name(const std::string& name, std::string& path) const {
    std::error_code ec;
    if (fs::is_regular_file(name, ec)) {
        path = name;
        return true;
    }
    const std::string want = lower_text(trim(name));
    if (want.empty()) return false;
    const std::vector<SoundEntry> all = m_library.all_sounds();
    for (const SoundEntry& s : all) {
        if (lower_text(s.name) == want || lower_text(fs::path(s.path).filename().string()) == want) {
            path = s.path;
            return true;
        }
    }
    const std::vector<SoundEntry> found = m_library.search(name, 1);
    if (found.empty()) return false;
    path = found.front().path;
    return true;
}

void Session::voice_changed() {
    m_dirty = true;
}

bool Session::apply_voice_preset(const std::string& name, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    bool ok = false;
    std::string canonical = name;
    bool user = false;
    for (const VoicePresetInfo& p : list_user_presets(presets_path())) {
        if (p.name == name) {
            user = true;
            canonical = p.name;
        }
    }
    if (!user) {
        for (const VoicePresetInfo& p : VoiceChain::builtin_presets()) {
            if (lower_text(p.name) == lower_text(trim(name))) canonical = p.name;
        }
    }
    if (user) ok = load_user_preset(presets_path(), voice(), canonical, error);
    else ok = voice().apply_builtin(canonical, error);
    if (ok) {
        m_settings.voice_preset = lower_text(canonical) == "none" ? std::string() : canonical;
        m_dirty = true;
    }
    return ok;
}

bool Session::save_voice_preset(const std::string& name, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    for (const VoicePresetInfo& p : VoiceChain::builtin_presets()) {
        if (lower_text(p.name) == lower_text(trim(name))) {
            if (error) *error = "that's the name of a built-in preset; pick another one";
            return false;
        }
    }
    if (!save_user_preset(presets_path(), voice(), name, error)) return false;
    m_settings.voice_preset = trim(name);
    m_dirty = true;
    return true;
}

bool Session::rename_voice_preset(const std::string& from, const std::string& to, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    for (const VoicePresetInfo& p : VoiceChain::builtin_presets()) {
        if (lower_text(p.name) == lower_text(trim(to))) {
            if (error) *error = "that's the name of a built-in preset; pick another one";
            return false;
        }
    }
    if (!rename_user_preset(presets_path(), from, to, error)) return false;
    if (m_settings.voice_preset == from) m_settings.voice_preset = trim(to);
    m_dirty = true;
    return true;
}

bool Session::delete_voice_preset(const std::string& name, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (!delete_user_preset(presets_path(), name, error)) return false;
    if (m_settings.voice_preset == name) m_settings.voice_preset.clear();
    m_dirty = true;
    return true;
}

std::vector<VoicePresetInfo> Session::voice_presets() const {
    std::vector<VoicePresetInfo> out = VoiceChain::builtin_presets();
    for (const VoicePresetInfo& p : list_user_presets(m_options.config_dir / "presets")) out.push_back(p);
    return out;
}

void Session::refresh_virtual_mic_state() {
    if (!m_options.virtual_mic) return;
    const VirtualMicState s = m_vmic.state();
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_vmic_state = s;
}

void Session::apply_audio(bool quiet) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (!m_options.audio) {
        m_engine.prepare_offline(k_rate);
        apply_levels();
        return;
    }
    Settings& s = m_settings;
    EngineConfig c;
    c.sample_rate = k_rate;
    c.capture = s.input_device != "none";
    c.input_device = s.auto_mode || s.input_device == "none" ? std::string() : s.input_device;
    c.monitor = s.monitor_device != "none";
    c.monitor_device = s.auto_mode || s.monitor_device == "none" ? std::string() : s.monitor_device;
    std::string vmic_message;
    bool vmic_ready = false;
    const bool want_vmic = m_options.virtual_mic && VirtualMic::platform_supported() && (s.virtual_mic || s.auto_mode);
    if (want_vmic) {
        std::string why;
        vmic_ready = m_vmic.setup(&why);
        if (!vmic_ready) vmic_message = why;
    } else if (m_options.virtual_mic && VirtualMic::platform_supported() && !s.virtual_mic) {
        std::string why;
        if (!s.keep_virtual_mic) m_vmic.teardown(&why);
    }
    if (vmic_ready) {
        c.send = true;
        c.send_device = VirtualMic::sink_name;
    } else if (!s.send_device.empty() && s.send_device != "none") {
        c.send = true;
        c.send_device = s.send_device;
    } else {
        c.send = false;
    }
    if (m_options.virtual_mic && VirtualMic::platform_supported()) {
        refresh_virtual_mic_state();
        std::string default_source, default_sink;
        {
            std::lock_guard<std::mutex> state(m_state_mutex);
            default_source = m_vmic_state.default_source;
            default_sink = m_vmic_state.default_sink;
        }
        if (c.capture && c.input_device.empty() && is_rustle_device(default_source)) {
            const std::vector<fa::DeviceInfo> inputs = usable_devices(fa::DeviceKind::input);
            if (inputs.empty()) c.capture = false;
            else c.input_device = inputs.front().id;
        }
        if (c.monitor && c.monitor_device.empty() && is_rustle_device(default_sink)) {
            const std::vector<fa::DeviceInfo> outputs = usable_devices(fa::DeviceKind::output);
            if (outputs.empty()) c.monitor = false;
            else c.monitor_device = outputs.front().id;
        }
    }
    std::string why;
    const bool ok = m_engine.start(c, &why);
    const bool nothing = !m_engine.capture_status().running && !m_engine.send_status().running && !m_engine.monitor_status().running;
    if (!ok && !why.empty() && !quiet) notice("warning", "Some audio couldn't start. " + why);
    if (quiet && m_audio_down && !nothing) notice("info", "The sound system is back, so Rustle's audio is working again.");
    m_audio_down = nothing && (c.capture || c.send || c.monitor);
    apply_levels();
    {
        std::lock_guard<std::mutex> state(m_state_mutex);
        m_vmic_message = vmic_message;
    }
    refresh_virtual_mic_state();
}

void Session::apply_levels() {
    const Settings& s = m_settings;
    m_engine.set_mic_enabled(s.mic_enabled);
    m_engine.set_push_to_talk(s.push_to_talk);
    m_engine.set_hear_myself(s.hear_myself);
    m_engine.set_mic_volume(s.mic_volume);
    m_engine.set_hear_volume(s.hear_myself_volume);
    m_engine.set_sound_gains(s.sounds_volume, s.others_volume, s.me_volume, s.sounds_to_others, s.sounds_to_me);
    apply_music_levels();
}

void Session::set_auto_mode(bool on) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_settings.auto_mode = on;
    if (on) {
        m_settings.virtual_mic = true;
        m_settings.mic_enabled = true;
        m_settings.sounds_to_me = true;
        m_settings.sounds_to_others = true;
        if (m_settings.input_device == "none") m_settings.input_device.clear();
        if (m_settings.monitor_device == "none") m_settings.monitor_device.clear();
    }
    m_dirty = true;
    apply_audio();
}

RouteStatus Session::route_status() const {
    RouteStatus r;
    r.mode = m_settings.auto_mode ? "auto" : "manual";
    r.virtual_mic_supported = VirtualMic::platform_supported() && m_options.virtual_mic;
    r.virtual_mic_wanted = r.virtual_mic_supported && (m_settings.virtual_mic || m_settings.auto_mode);
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        r.virtual_mic_ready = m_vmic_state.ready();
        r.server = m_vmic_state.server_name;
        r.virtual_mic_message = m_vmic_message.empty() ? m_vmic_state.error : m_vmic_message;
    }
    if (!r.virtual_mic_supported) {
        r.virtual_mic_message = VirtualMic::platform_supported() ? std::string("turned off for this run")
                                                                  : std::string("Rustle Mic needs PulseAudio or PipeWire, so it's only on Linux. Pick a virtual audio cable (VB-CABLE on Windows, BlackHole on macOS) as the 'send to others' device, then choose that cable as the microphone in Discord.");
    }
    r.capture = m_engine.capture_status();
    r.send = m_engine.send_status();
    r.monitor = m_engine.monitor_status();
    r.send_label = r.send.device == VirtualMic::sink_name ? std::string("Rustle Mic") : r.send.device;
    return r;
}

void Session::set_mic_enabled(bool on) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_settings.mic_enabled = on;
    m_engine.set_mic_enabled(on);
    m_dirty = true;
}

void Session::toggle_mic() {
    set_mic_enabled(!m_settings.mic_enabled);
}

void Session::set_talking(bool held) {
    m_engine.set_talking(held);
}

void Session::queue_key(const std::string& action, bool pressed) {
    if (action == "ptt") {
        m_engine.set_talking(pressed);
        return;
    }
    std::lock_guard<std::mutex> lock(m_key_mutex);
    m_key_queue.emplace_back(action, pressed);
}

void Session::run_action(const std::string& action, bool pressed) {
    if (action == "ptt") {
        m_engine.set_talking(pressed);
        return;
    }
    if (!pressed) return;
    if (action == "stop-all") stop_all();
    else if (action == "toggle-mic") toggle_mic();
    else if (action == "music-toggle" || action == "music-next" || action == "music-previous") {
        std::string why;
        const bool ok = action == "music-toggle" ? music_toggle(&why) : action == "music-next" ? music_next(&why) : music_previous(&why);
        if (!ok && !why.empty()) notice("info", why);
    }
    else if (action.compare(0, 5, "play:") == 0) {
        std::string why;
        if (!toggle(action.substr(5), &why)) notice("error", why);
    }
}

void Session::key_event(const std::string& chord, bool pressed, bool from_app) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    const std::string c = normalize_chord(chord);
    if (c.empty()) return;
    if (from_app && m_hotkeys.covers(c)) return;
    const std::string owner = chord_owner(c);
    if (!owner.empty()) run_action(owner, pressed);
}

void Session::apply_hotkeys() {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (!m_options.hotkeys) return;
    m_hotkeys.stop();
    if (!m_settings.global_hotkeys) return;
    std::vector<HotkeyBinding> binds;
    if (!m_settings.key_stop_all.empty()) binds.push_back({ "stop-all", m_settings.key_stop_all });
    if (!m_settings.key_mic_toggle.empty()) binds.push_back({ "toggle-mic", m_settings.key_mic_toggle });
    if (!m_settings.key_push_to_talk.empty()) binds.push_back({ "ptt", m_settings.key_push_to_talk });
    if (!m_settings.key_music_toggle.empty()) binds.push_back({ "music-toggle", m_settings.key_music_toggle });
    if (!m_settings.key_music_next.empty()) binds.push_back({ "music-next", m_settings.key_music_next });
    if (!m_settings.key_music_previous.empty()) binds.push_back({ "music-previous", m_settings.key_music_previous });
    for (const auto& kv : m_settings.keybinds) binds.push_back({ "play:" + kv.first, kv.second });
    m_hotkeys.start(binds, [this](const std::string& action, bool pressed) { queue_key(action, pressed); });
}

std::string Session::control_command(const std::string& raw) {
    std::unique_lock<std::recursive_mutex> lock(m_api, std::defer_lock);
    for (int i = 0; i < 200 && !lock.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!lock.owns_lock()) return "error\tRustle is busy; try again";
    const std::string line = trim(raw);
    const std::size_t space = line.find(' ');
    const std::string cmd = lower_text(space == std::string::npos ? line : line.substr(0, space));
    const std::string rest = space == std::string::npos ? std::string() : trim(line.substr(space + 1));
    if (cmd == "stop-all" || (cmd == "stop" && rest.empty())) {
        stop_all();
        return "ok";
    }
    if (cmd == "toggle-mic") {
        toggle_mic();
        return std::string("ok\tmic ") + (m_settings.mic_enabled ? "on" : "off");
    }
    if (cmd == "mic-on" || cmd == "mic-off") {
        set_mic_enabled(cmd == "mic-on");
        return "ok";
    }
    if (cmd == "ptt-down" || cmd == "ptt-up") {
        m_engine.set_talking(cmd == "ptt-down");
        return "ok";
    }
    if (cmd == "play" || cmd == "toggle" || cmd == "stop") {
        std::string path;
        if (!find_by_name(rest, path)) return "error\tno sound called " + rest;
        if (cmd == "stop") {
            stop(path);
            return "ok";
        }
        std::string why;
        const bool ok = cmd == "play" ? play(path, &why) : toggle(path, &why);
        return ok ? "ok\t" + path : "error\t" + why;
    }
    if (cmd == "preset") {
        std::string why;
        if (!apply_voice_preset(rest, &why)) return "error\t" + why;
        return "ok";
    }
    if (cmd == "voice-off") {
        voice().reset_all();
        m_settings.voice_preset.clear();
        m_dirty = true;
        return "ok";
    }
    if (cmd.compare(0, 6, "music-") == 0 || cmd == "music") return control_music(cmd, rest);
    if (cmd == "status") {
        const RouteStatus r = route_status();
        std::string s = "ok\tmic " + std::string(m_settings.mic_enabled ? "on" : "off");
        s += ", virtual mic " + std::string(r.virtual_mic_ready ? "ready" : "not ready");
        s += ", " + std::to_string(m_engine.playing().size()) + " playing";
        s += ", " + std::to_string(m_library.sound_count()) + " sounds";
        const MusicNow now = music_now();
        s += ", music " + music_state_name(now.state);
        if (!now.name.empty() && now.state != MusicState::stopped) s += " (" + now.name + ")";
        return s;
    }
    return "error\tunknown command '" + cmd + "' (try stop-all, toggle-mic, mic-on, mic-off, ptt-down, ptt-up, play NAME, toggle NAME, stop NAME, preset NAME, voice-off, status, music-play, music-pause, music-toggle, music-next, music-prev, music-stop, music-shuffle, music-repeat, music-add PATH, music-playlist NAME, music-status)";
}

void Session::tick() {
    std::vector<std::pair<std::string, bool>> keys;
    {
        std::lock_guard<std::mutex> lock(m_key_mutex);
        keys.swap(m_key_queue);
    }
    std::lock_guard<std::recursive_mutex> lock(m_api);
    for (const auto& k : keys) run_action(k.first, k.second);
    music_tick();
    const auto now = std::chrono::steady_clock::now();
    if (m_settings.rescan_seconds > 0.0 && now - m_last_scan_check > std::chrono::duration<double>(m_settings.rescan_seconds)) {
        m_last_scan_check = now;
        post([this] {
            if (m_library.changed_on_disk()) {
                const std::vector<SoundEntry> before = m_library.all_sounds();
                m_library.rescan();
                std::map<std::string, std::int64_t> stamp;
                for (const SoundEntry& s : m_library.all_sounds()) stamp[s.path] = s.modified;
                for (const SoundEntry& s : before) {
                    auto it = stamp.find(s.path);
                    if (it == stamp.end() || it->second != s.modified) m_cache.forget(s.path);
                }
            }
        });
    }
    if (m_options.audio && now - m_last_heal > std::chrono::seconds(2)) {
        m_last_heal = now;
        const bool want_vmic = m_options.virtual_mic && VirtualMic::platform_supported() && (m_settings.virtual_mic || m_settings.auto_mode);
        if (m_engine.any_stream_failed() && m_settings.auto_mode) {
            notice("info", "A sound device stopped working, so Rustle restarted its audio.");
            apply_audio();
        } else if (m_audio_down) {
            apply_audio(true);
        } else if (m_options.virtual_mic) {
            refresh_virtual_mic_state();
            bool ready = false;
            {
                std::lock_guard<std::mutex> state(m_state_mutex);
                ready = m_vmic_state.ready();
            }
            const StreamStatus send = m_engine.send_status();
            if (want_vmic && (!ready || !send.running) && m_engine.config().send_device == VirtualMic::sink_name) {
                m_engine.stop();
                notice("info", "Rustle Mic was removed by something else, so Rustle made it again.");
                apply_audio();
            }
        }
    }
    if (m_dirty && now - m_last_save > std::chrono::seconds(2)) {
        std::string why;
        if (!save(&why)) notice("error", "Couldn't save settings: " + why);
    }
    if (m_music_dirty && now - m_last_music_save > std::chrono::seconds(2)) {
        std::string why;
        if (!save_music(&why)) notice("error", "Couldn't save the music queue and playlists: " + why);
    }
}

void Session::load_music() {
    std::string text, why;
    std::error_code ec;
    if (fs::exists(playlists_path(), ec)) {
        Json root;
        if (read_text_file(playlists_path(), text, &why) && fizmo::audio::parse_json(text, root, &why) && root.is_object()) {
            m_playlists.from_json(root);
        } else {
            const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            fs::rename(playlists_path(), playlists_path().string() + ".broken-" + std::to_string(stamp), ec);
            notice("warning", "Your playlists couldn't be read (" + why + "), so the file was moved aside.");
        }
    }
    if (fs::exists(music_path(), ec)) {
        Json root;
        text.clear();
        if (read_text_file(music_path(), text, &why) && fizmo::audio::parse_json(text, root, &why) && root.is_object()) {
            if (const Json* q = root.find("queue")) m_queue.from_json(*q);
            const MusicNode* cur = m_queue.find(m_queue.current());
            if (cur && cur->kind == MusicKind::song) {
                m_music_id = cur->id;
                m_music_path = cur->path;
                m_music_name = cur->name;
                m_music_at = std::max(0.0, json_get_number(root, "position", 0.0));
                m_music_length = std::max(0.0, json_get_number(root, "length", 0.0));
                m_music_state = MusicState::paused;
            }
        } else {
            notice("warning", "The music queue couldn't be restored: " + why);
        }
    }
}

bool Session::save_music(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_music_dirty = false;
    m_last_music_save = std::chrono::steady_clock::now();
    Json root = Json::make_object();
    root.set("format", json_string("rustle-music"));
    root.set("version", json_number(1));
    root.set("queue", m_queue.to_json());
    root.set("position", json_number(m_music_at));
    root.set("length", json_number(m_music_length));
    std::string why;
    bool ok = write_text_file_atomic(music_path(), fizmo::audio::write_json(root, true) + "\n", &why);
    if (!ok && error) *error = why;
    if (!write_text_file_atomic(playlists_path(), fizmo::audio::write_json(m_playlists.to_json(), true) + "\n", &why)) {
        if (error && ok) *error = why;
        ok = false;
    }
    return ok;
}

void Session::music_changed() {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_music_dirty = true;
    ++m_music_rev;
}

std::uint64_t Session::music_revision() const {
    return m_music_rev;
}

void Session::apply_music_levels() {
    const Settings& s = m_settings;
    m_engine.set_music_gains(s.music_volume, s.music_to_others, s.music_to_me);
}

void Session::start_music(std::uint64_t id, double at, bool paused) {
    const MusicNode* n = m_queue.find(id);
    if (!n || n->kind != MusicKind::song) return;
    const std::string path = n->path;
    m_music_id = id;
    m_music_path = path;
    m_music_name = n->name;
    m_music_at = std::max(0.0, at);
    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        ticket = ++m_music_ticket;
        m_music_state = MusicState::loading;
        m_music_failed = false;
        m_music_length = 0.0;
    }
    ++m_music_rev;
    m_music_dirty = true;
    post([this, path, at, paused, ticket] {
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (ticket != m_music_ticket) return;
        }
        std::string why;
        const bool ok = m_engine.play_music(path, at, paused, &why);
        const std::uint64_t epoch = m_engine.epoch();
        const MusicStatus st = m_engine.music_status();
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (ticket != m_music_ticket) {
            if (ok) m_engine.stop_music(0.02);
            return;
        }
        if (ok) {
            m_music_state = paused ? MusicState::paused : MusicState::playing;
            m_music_epoch = epoch;
            m_music_length = st.length;
        } else {
            m_music_state = MusicState::stopped;
            m_music_failed = true;
            m_music_error = fs::path(path).filename().string() + ": " + why;
        }
    });
}

bool Session::music_add(const std::string& path, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    MusicNode node;
    if (!music_node_for_path(path, node, error)) return false;
    if (!m_queue.add(std::move(node))) {
        if (error) *error = "there's no music there";
        return false;
    }
    music_changed();
    return true;
}

bool Session::music_add_paths(const std::vector<std::string>& paths, std::size_t& added, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    added = 0;
    std::string problems;
    for (const std::string& p : paths) {
        if (p.empty()) continue;
        std::string why;
        if (music_add(p, &why)) ++added;
        else if (!why.empty()) problems += (problems.empty() ? "" : "; ") + why;
    }
    if (error) *error = problems;
    return added > 0;
}

bool Session::music_queue_playlist(std::uint64_t id, bool replace, bool play, bool shuffle, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    MusicNode node;
    if (!m_playlists.expanded(id, node, error)) return false;
    if (replace) {
        music_stop();
        m_queue.clear();
    }
    const std::uint64_t added = m_queue.add(std::move(node));
    if (!added) {
        if (error) *error = "there's no music in it";
        return false;
    }
    if (play) {
        m_settings.music_shuffle = shuffle;
        m_dirty = true;
        const std::uint64_t song = m_queue.jump(added, shuffle);
        if (song) start_music(song, 0.0, false);
    }
    music_changed();
    return true;
}

bool Session::music_play(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    MusicState state;
    {
        std::lock_guard<std::mutex> s(m_state_mutex);
        state = m_music_state;
    }
    if (state == MusicState::playing || state == MusicState::loading) return true;
    if (state == MusicState::paused) {
        const MusicStatus st = m_engine.music_status();
        if (st.active) {
            m_engine.set_music_paused(false);
            std::lock_guard<std::mutex> s(m_state_mutex);
            m_music_state = MusicState::playing;
            ++m_music_rev;
            return true;
        }
        if (m_queue.find(m_music_id)) {
            start_music(m_music_id, m_music_at, false);
            return true;
        }
    }
    if (m_queue.song_count() == 0) {
        if (error) *error = "the queue is empty; add some music first";
        return false;
    }
    m_music_failures = 0;
    const bool shuffle = m_settings.music_shuffle;
    const RepeatMode repeat = music_repeat();
    std::uint64_t id = m_queue.next(shuffle, repeat);
    if (!id) {
        m_queue.reset_played();
        m_queue.forget_current();
        id = m_queue.next(shuffle, repeat);
    }
    if (!id) {
        if (error) *error = "there's nothing to play";
        return false;
    }
    start_music(id, 0.0, false);
    music_changed();
    return true;
}

void Session::music_pause() {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    MusicState state;
    {
        std::lock_guard<std::mutex> s(m_state_mutex);
        state = m_music_state;
    }
    if (state == MusicState::playing) {
        const MusicStatus st = m_engine.music_status();
        if (st.active) m_music_at = st.position;
        m_engine.set_music_paused(true);
        std::lock_guard<std::mutex> s(m_state_mutex);
        m_music_state = MusicState::paused;
    } else if (state == MusicState::loading) {
        std::lock_guard<std::mutex> s(m_state_mutex);
        ++m_music_ticket;
        m_music_state = MusicState::paused;
    } else {
        return;
    }
    ++m_music_rev;
    m_music_dirty = true;
}

bool Session::music_toggle(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    MusicState state;
    {
        std::lock_guard<std::mutex> s(m_state_mutex);
        state = m_music_state;
    }
    if (state == MusicState::playing || state == MusicState::loading) {
        music_pause();
        return true;
    }
    return music_play(error);
}

void Session::music_stop() {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    {
        std::lock_guard<std::mutex> s(m_state_mutex);
        ++m_music_ticket;
        m_music_state = MusicState::stopped;
        m_music_failed = false;
    }
    m_engine.stop_music(0.05);
    m_music_at = 0.0;
    ++m_music_rev;
    m_music_dirty = true;
}

bool Session::music_next(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (m_queue.song_count() == 0) {
        if (error) *error = "the queue is empty";
        return false;
    }
    const RepeatMode repeat = music_repeat() == RepeatMode::one ? RepeatMode::all : music_repeat();
    const std::uint64_t id = m_queue.next(m_settings.music_shuffle, repeat);
    if (!id) {
        music_stop();
        if (error) *error = "that was the last song in the queue";
        return false;
    }
    m_music_failures = 0;
    start_music(id, 0.0, false);
    music_changed();
    return true;
}

bool Session::music_previous(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    MusicState state;
    {
        std::lock_guard<std::mutex> s(m_state_mutex);
        state = m_music_state;
    }
    double at = m_music_at;
    const MusicStatus st = m_engine.music_status();
    if (st.active) at = st.position;
    if (state != MusicState::stopped && at > 3.0) {
        if (st.active) m_engine.seek_music(0.0);
        else start_music(m_music_id, 0.0, false);
        m_music_at = 0.0;
        return true;
    }
    const std::uint64_t id = m_queue.previous();
    if (!id) {
        if (m_queue.find(m_music_id)) {
            if (st.active) m_engine.seek_music(0.0);
            else start_music(m_music_id, 0.0, false);
            m_music_at = 0.0;
            return true;
        }
        if (error) *error = "there's no song before this one";
        return false;
    }
    start_music(id, 0.0, false);
    music_changed();
    return true;
}

bool Session::music_jump(std::uint64_t id, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    const std::uint64_t song = m_queue.jump(id, m_settings.music_shuffle);
    if (!song) {
        if (error) *error = "that isn't in the queue any more";
        return false;
    }
    m_music_failures = 0;
    start_music(song, 0.0, false);
    music_changed();
    return true;
}

bool Session::music_seek(double seconds) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    const double to = std::max(0.0, seconds);
    const MusicStatus st = m_engine.music_status();
    m_music_at = st.length > 0.0 ? std::min(to, st.length) : to;
    ++m_music_rev;
    if (st.active) return m_engine.seek_music(to);
    return m_queue.find(m_music_id) != nullptr;
}

bool Session::music_remove(std::uint64_t id) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (!m_queue.remove(id)) return false;
    music_changed();
    return true;
}

bool Session::music_move(std::uint64_t id, int delta) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    if (!m_queue.move(id, delta)) return false;
    music_changed();
    return true;
}

void Session::music_clear() {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    music_stop();
    m_queue.clear();
    m_music_id = 0;
    m_music_path.clear();
    m_music_name.clear();
    m_music_length = 0.0;
    music_changed();
}

std::size_t Session::music_clear_played() {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    const std::size_t n = m_queue.clear_played();
    if (n) music_changed();
    return n;
}

void Session::set_music_shuffle(bool on) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_settings.music_shuffle = on;
    m_dirty = true;
    ++m_music_rev;
}

void Session::set_music_repeat(RepeatMode mode) {
    std::lock_guard<std::recursive_mutex> lock(m_api);
    m_settings.music_repeat = repeat_name(mode);
    m_dirty = true;
    ++m_music_rev;
}

RepeatMode Session::music_repeat() const {
    return repeat_from(m_settings.music_repeat);
}

MusicNow Session::music_now() const {
    MusicNow n;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        n.state = m_music_state;
        n.length = m_music_length;
    }
    n.id = m_music_id;
    n.path = m_music_path;
    n.name = m_music_name;
    n.position = m_music_at;
    if (n.state == MusicState::playing || n.state == MusicState::paused) {
        const MusicStatus st = m_engine.music_status();
        if (st.active) {
            n.position = st.position;
            if (st.length > 0.0) n.length = st.length;
        }
    }
    if (n.length > 0.0) n.position = std::min(n.position, n.length);
    const std::uint64_t parent = m_queue.parent_of(m_music_id);
    if (const MusicNode* p = m_queue.find(parent)) n.folder = p->name;
    else if (!n.path.empty()) n.folder = fs::path(n.path).parent_path().filename().string();
    return n;
}

void Session::music_finished() {
    if (music_repeat() == RepeatMode::one && m_queue.find(m_music_id)) {
        start_music(m_music_id, 0.0, false);
        return;
    }
    const std::uint64_t id = m_queue.next(m_settings.music_shuffle, music_repeat());
    if (!id) {
        {
            std::lock_guard<std::mutex> s(m_state_mutex);
            m_music_state = MusicState::stopped;
        }
        m_music_at = 0.0;
        ++m_music_rev;
        m_music_dirty = true;
        return;
    }
    start_music(id, 0.0, false);
    music_changed();
}

void Session::music_tick() {
    MusicState state;
    bool failed = false;
    std::string error;
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> s(m_state_mutex);
        state = m_music_state;
        failed = m_music_failed;
        error = m_music_error;
        epoch = m_music_epoch;
        m_music_failed = false;
    }
    if (failed) {
        ++m_music_failures;
        ++m_music_rev;
        const std::size_t limit = std::max<std::size_t>(3, std::min<std::size_t>(m_queue.song_count(), 25));
        if (static_cast<std::size_t>(m_music_failures) >= limit) {
            notice("error", "Couldn't play " + error + ". Stopped the music after " + std::to_string(m_music_failures) + " songs in a row wouldn't play.");
            m_music_failures = 0;
            return;
        }
        notice("error", "Couldn't play " + error + "; skipping it.");
        music_finished();
        return;
    }
    if (state != MusicState::playing && state != MusicState::paused) return;
    const MusicStatus st = m_engine.music_status();
    if (st.active) {
        m_music_at = st.position;
        if (st.length > 0.0) {
            std::lock_guard<std::mutex> s(m_state_mutex);
            m_music_length = st.length;
        }
        m_music_failures = 0;
        return;
    }
    if (state == MusicState::paused) return;
    if (st.epoch != epoch) {
        if (m_queue.find(m_music_id)) start_music(m_music_id, m_music_at, false);
        return;
    }
    music_finished();
}

std::string Session::control_music(const std::string& cmd, const std::string& rest) {
    std::string why;
    const std::string arg = lower_text(rest);
    if (cmd == "music-play") return music_play(&why) ? "ok" : "error\t" + why;
    if (cmd == "music-pause") {
        music_pause();
        return "ok";
    }
    if (cmd == "music-toggle" || cmd == "music") return music_toggle(&why) ? "ok\tmusic " + music_state_name(music_now().state) : "error\t" + why;
    if (cmd == "music-next" || cmd == "music-skip") return music_next(&why) ? "ok\t" + music_now().name : "error\t" + why;
    if (cmd == "music-prev" || cmd == "music-previous") return music_previous(&why) ? "ok\t" + music_now().name : "error\t" + why;
    if (cmd == "music-stop") {
        music_stop();
        return "ok";
    }
    if (cmd == "music-shuffle") {
        bool on = !m_settings.music_shuffle;
        if (arg == "on" || arg == "1" || arg == "yes") on = true;
        else if (arg == "off" || arg == "0" || arg == "no") on = false;
        set_music_shuffle(on);
        return std::string("ok\tshuffle ") + (on ? "on" : "off");
    }
    if (cmd == "music-repeat") {
        RepeatMode mode = repeat_from(arg);
        if (arg.empty() || arg == "cycle") {
            const RepeatMode now = music_repeat();
            mode = now == RepeatMode::off ? RepeatMode::all : now == RepeatMode::all ? RepeatMode::one : RepeatMode::off;
        } else if (arg != "off" && repeat_from(arg) == RepeatMode::off) {
            return "error\trepeat can be off, all or one";
        }
        set_music_repeat(mode);
        return "ok\trepeat " + repeat_name(mode);
    }
    if (cmd == "music-add") {
        if (rest.empty()) return "error\tgive a file or folder to add";
        return music_add(rest, &why) ? "ok" : "error\t" + why;
    }
    if (cmd == "music-playlist" || cmd == "music-shuffle-playlist" || cmd == "music-queue-playlist") {
        const std::uint64_t id = m_playlists.find_by_name(rest);
        if (!id) return "error\tno playlist called " + rest;
        const bool add_only = cmd == "music-queue-playlist";
        if (!music_queue_playlist(id, !add_only, !add_only, cmd == "music-shuffle-playlist", &why)) return "error\t" + why;
        return "ok";
    }
    if (cmd == "music-status") {
        const MusicNow n = music_now();
        std::string s = "ok\t" + music_state_name(n.state);
        if (!n.name.empty()) s += ": " + n.name;
        s += ", " + std::to_string(m_queue.played_count()) + " of " + std::to_string(m_queue.song_count()) + " songs played";
        s += std::string(", shuffle ") + (m_settings.music_shuffle ? "on" : "off") + ", repeat " + m_settings.music_repeat;
        return s;
    }
    return "error\tunknown music command '" + cmd + "' (try music-play, music-pause, music-toggle, music-next, music-prev, music-stop, music-shuffle, music-repeat, music-add PATH, music-playlist NAME, music-shuffle-playlist NAME, music-queue-playlist NAME, music-status)";
}

void Session::notice(const std::string& kind, const std::string& text) {
    std::lock_guard<std::mutex> lock(m_notice_mutex);
    Notice n;
    n.seq = ++m_notice_seq;
    n.kind = kind;
    n.text = text;
    m_notices.push_back(n);
    while (m_notices.size() > 50) m_notices.pop_front();
}

std::vector<Notice> Session::notices_since(std::uint64_t seq) const {
    std::lock_guard<std::mutex> lock(m_notice_mutex);
    std::vector<Notice> out;
    for (const Notice& n : m_notices) {
        if (n.seq > seq) out.push_back(n);
    }
    return out;
}

} // namespace rustle
