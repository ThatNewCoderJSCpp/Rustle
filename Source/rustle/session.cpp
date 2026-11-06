#include <rustle/session.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;
namespace fa = fizmo::audio;

namespace rustle {

namespace {

constexpr std::uint32_t k_rate = 48000;

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

} // namespace

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
                *error = c + " is already used for " + (owner == "stop-all" ? "Stop all" : owner == "ptt" ? "Push to talk" : "Mic on/off");
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
    if (cmd == "status") {
        const RouteStatus r = route_status();
        std::string s = "ok\tmic " + std::string(m_settings.mic_enabled ? "on" : "off");
        s += ", virtual mic " + std::string(r.virtual_mic_ready ? "ready" : "not ready");
        s += ", " + std::to_string(m_engine.playing().size()) + " playing";
        s += ", " + std::to_string(m_library.sound_count()) + " sounds";
        return s;
    }
    return "error\tunknown command '" + cmd + "' (try stop-all, toggle-mic, mic-on, mic-off, ptt-down, ptt-up, play NAME, toggle NAME, stop NAME, preset NAME, voice-off, status)";
}

void Session::tick() {
    std::vector<std::pair<std::string, bool>> keys;
    {
        std::lock_guard<std::mutex> lock(m_key_mutex);
        keys.swap(m_key_queue);
    }
    std::lock_guard<std::recursive_mutex> lock(m_api);
    for (const auto& k : keys) run_action(k.first, k.second);
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
