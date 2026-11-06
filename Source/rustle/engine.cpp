#include <rustle/engine.hpp>

#include <algorithm>
#include <cmath>

namespace fa = fizmo::audio;

namespace rustle {

namespace {

constexpr std::size_t k_chunk = 4096;

void raise_peak(std::atomic<float>& slot, float value) noexcept {
    float cur = slot.load(std::memory_order_relaxed);
    while (value > cur && !slot.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

float peak_of(const float* data, std::size_t count) noexcept {
    float p = 0.0f;
    for (std::size_t i = 0; i < count; ++i) p = std::max(p, std::fabs(data[i]));
    return p;
}

} // namespace

bool is_rustle_device(const std::string& id) {
    return id.compare(0, 7, "rustle_") == 0;
}

std::vector<fa::DeviceInfo> usable_devices(fa::DeviceKind kind) {
    std::vector<fa::DeviceInfo> out;
    for (const fa::DeviceInfo& d : fa::devices(kind)) {
        if (is_rustle_device(d.id)) continue;
        out.push_back(d);
    }
    return out;
}

Engine::Engine() = default;

Engine::~Engine() {
    stop();
}

void Engine::allocate(std::uint32_t rate) {
    m_rate = rate ? rate : 48000;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_send_mixer = std::make_unique<fa::Mixer>(m_rate, 2);
        m_monitor_mixer = std::make_unique<fa::Mixer>(m_rate, 2);
        m_plays.clear();
    }
    m_voice.prepare(m_rate, 1, k_chunk);
    m_mono.assign(k_chunk, 0.0f);
    m_send_voice.assign(k_chunk, 0.0f);
    m_hear_voice.assign(k_chunk, 0.0f);
    const std::size_t target = std::max<std::size_t>(256, static_cast<std::size_t>(m_rate) * 3 / 100);
    m_send_link.allocate(static_cast<std::size_t>(m_rate), target);
    m_hear_link.allocate(static_cast<std::size_t>(m_rate), target);
    m_gate = 0.0f;
}

void Engine::prepare_offline(std::uint32_t sample_rate) {
    stop();
    allocate(sample_rate);
    m_offline = true;
    m_started = true;
    m_send_live.store(true);
    m_monitor_live.store(true);
    m_capture_live.store(true);
    m_capture_status = StreamStatus();
    m_send_status = StreamStatus();
    m_monitor_status = StreamStatus();
}

void Engine::close_streams() {
    m_capture.close();
    m_send.close();
    m_monitor.close();
}

bool Engine::start(const EngineConfig& config, std::string* error) {
    stop();
    m_config = config;
    allocate(config.sample_rate);
    m_offline = false;
    std::string problems;

    m_send_status = StreamStatus();
    m_send_status.wanted = config.send;
    m_send_status.device = config.send_device;
    if (config.send) {
        fa::StreamConfig sc;
        sc.kind = fa::DeviceKind::output;
        sc.device = config.send_device;
        sc.sample_rate = m_rate;
        sc.channels = 2;
        sc.latency_ms = config.latency_ms;
        sc.name = "Rustle (to others)";
        sc.pin_device = !config.send_device.empty();
        std::string why;
        if (m_send.open_playback(sc, [this](float* out, std::size_t frames) { render_send(out, frames); }, &why) && m_send.start(&why)) {
            m_send_status.running = true;
            m_send_status.device = m_send.device_name();
            m_send_live.store(true);
        } else {
            m_send_status.error = why;
            m_send.close();
            problems += "sending to others: " + why + "\n";
        }
    }

    m_monitor_status = StreamStatus();
    m_monitor_status.wanted = config.monitor;
    m_monitor_status.device = config.monitor_device;
    if (config.monitor) {
        fa::StreamConfig sc;
        sc.kind = fa::DeviceKind::output;
        sc.device = config.monitor_device;
        sc.sample_rate = m_rate;
        sc.channels = 2;
        sc.latency_ms = config.latency_ms;
        sc.name = "Rustle (to me)";
        std::string why;
        if (m_monitor.open_playback(sc, [this](float* out, std::size_t frames) { render_monitor(out, frames); }, &why) && m_monitor.start(&why)) {
            m_monitor_status.running = true;
            m_monitor_status.device = m_monitor.device_name();
            m_monitor_live.store(true);
        } else {
            m_monitor_status.error = why;
            m_monitor.close();
            problems += "playing to you: " + why + "\n";
        }
    }

    m_capture_status = StreamStatus();
    m_capture_status.wanted = config.capture;
    m_capture_status.device = config.input_device;
    if (config.capture) {
        fa::StreamConfig sc;
        sc.kind = fa::DeviceKind::input;
        sc.device = config.input_device;
        sc.sample_rate = m_rate;
        sc.channels = 1;
        sc.latency_ms = config.latency_ms;
        sc.name = "Rustle (microphone)";
        std::string why;
        if (m_capture.open_capture(sc, [this](const float* in, std::size_t frames) { process_capture(in, frames, m_capture.channels()); }, &why) && m_capture.start(&why)) {
            m_capture_status.running = true;
            m_capture_status.device = m_capture.device_name();
            m_capture_live.store(true);
        } else {
            m_capture_status.error = why;
            m_capture.close();
            problems += "microphone: " + why + "\n";
        }
    }
    m_started = true;
    if (!problems.empty() && problems.back() == '\n') problems.pop_back();
    if (error) *error = problems;
    return problems.empty();
}

void Engine::stop() {
    close_streams();
    m_capture_live.store(false);
    m_send_live.store(false);
    m_monitor_live.store(false);
    m_capture_status.running = false;
    m_send_status.running = false;
    m_monitor_status.running = false;
    m_started = false;
    m_offline = false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_plays.clear();
}

bool Engine::started() const {
    return m_started;
}

StreamStatus Engine::capture_status() const {
    StreamStatus s = m_capture_status;
    if (s.running && m_capture.failed()) {
        s.running = false;
        s.error = m_capture.error();
    }
    return s;
}

StreamStatus Engine::send_status() const {
    StreamStatus s = m_send_status;
    if (s.running && m_send.failed()) {
        s.running = false;
        s.error = m_send.error();
    }
    return s;
}

StreamStatus Engine::monitor_status() const {
    StreamStatus s = m_monitor_status;
    if (s.running && m_monitor.failed()) {
        s.running = false;
        s.error = m_monitor.error();
    }
    return s;
}

bool Engine::any_stream_failed() const {
    return (m_capture_status.running && m_capture.failed()) || (m_send_status.running && m_send.failed()) || (m_monitor_status.running && m_monitor.failed());
}

bool Engine::mic_open() const {
    return m_mic_on.load() && (!m_ptt.load() || m_talk.load());
}

void Engine::set_sound_gains(float sounds, float others, float me, bool to_others, bool to_me) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_send_mixer) m_send_mixer->set_master_gain(to_others ? sounds * others : 0.0f);
    if (m_monitor_mixer) m_monitor_mixer->set_master_gain(to_me ? sounds * me : 0.0f);
}

void Engine::cleanup_locked() const {
    for (auto it = m_plays.begin(); it != m_plays.end();) {
        const bool a = it->second.send && m_send_mixer && m_send_live.load() && m_send_mixer->is_playing(it->second.send);
        const bool b = it->second.monitor && m_monitor_mixer && m_monitor_live.load() && m_monitor_mixer->is_playing(it->second.monitor);
        if (!a && !b) it = m_plays.erase(it);
        else ++it;
    }
}

std::uint64_t Engine::play(const fa::Sound& sound, float gain, bool loop, const std::string& tag) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!sound || sound->empty() || !m_send_mixer || !m_monitor_mixer) return 0;
    fa::PlayOptions o;
    o.gain = gain;
    o.loop = loop;
    Play p;
    if (m_send_live.load()) p.send = m_send_mixer->play(sound, o);
    if (m_monitor_live.load()) p.monitor = m_monitor_mixer->play(sound, o);
    if (!p.send && !p.monitor) return 0;
    p.tag = tag;
    p.loop = loop;
    p.length = sound->duration();
    p.id = m_next++;
    m_plays[p.id] = p;
    return p.id;
}

void Engine::stop_sound(std::uint64_t id, double fade) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_plays.find(id);
    if (it == m_plays.end()) return;
    if (it->second.send) m_send_mixer->stop(it->second.send, fade);
    if (it->second.monitor) m_monitor_mixer->stop(it->second.monitor, fade);
    m_plays.erase(it);
}

std::size_t Engine::stop_tag(const std::string& tag, double fade) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t n = 0;
    for (auto it = m_plays.begin(); it != m_plays.end();) {
        if (it->second.tag == tag) {
            if (it->second.send) m_send_mixer->stop(it->second.send, fade);
            if (it->second.monitor) m_monitor_mixer->stop(it->second.monitor, fade);
            it = m_plays.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

void Engine::stop_all(double fade) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_send_mixer) m_send_mixer->stop_all(fade);
    if (m_monitor_mixer) m_monitor_mixer->stop_all(fade);
    m_plays.clear();
}

void Engine::set_sound_gain(std::uint64_t id, float gain) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_plays.find(id);
    if (it == m_plays.end()) return;
    if (it->second.send) m_send_mixer->set_gain(it->second.send, gain);
    if (it->second.monitor) m_monitor_mixer->set_gain(it->second.monitor, gain);
}

void Engine::set_sound_loop(std::uint64_t id, bool loop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_plays.find(id);
    if (it == m_plays.end()) return;
    it->second.loop = loop;
    if (it->second.send) m_send_mixer->set_loop(it->second.send, loop);
    if (it->second.monitor) m_monitor_mixer->set_loop(it->second.monitor, loop);
}

bool Engine::is_playing(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    cleanup_locked();
    return m_plays.count(id) != 0;
}

bool Engine::tag_playing(const std::string& tag) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    cleanup_locked();
    for (const auto& kv : m_plays) {
        if (kv.second.tag == tag) return true;
    }
    return false;
}

std::vector<PlayingSound> Engine::playing() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    cleanup_locked();
    std::vector<PlayingSound> out;
    for (const auto& kv : m_plays) {
        PlayingSound s;
        s.id = kv.first;
        s.tag = kv.second.tag;
        s.length = kv.second.length;
        s.loop = kv.second.loop;
        if (kv.second.monitor && m_monitor_live.load()) s.position = m_monitor_mixer->position(kv.second.monitor);
        else if (kv.second.send) s.position = m_send_mixer->position(kv.second.send);
        out.push_back(s);
    }
    return out;
}

float Engine::take_mic_level() {
    return m_mic_peak.exchange(0.0f);
}

float Engine::take_voice_level() {
    return m_voice_peak.exchange(0.0f);
}

float Engine::take_send_level() {
    return m_send_peak.exchange(0.0f);
}

float Engine::take_monitor_level() {
    return m_monitor_peak.exchange(0.0f);
}

void Engine::process_capture(const float* input, std::size_t frames, std::size_t channels) noexcept {
    if (!input || m_mono.empty()) return;
    if (channels == 0) channels = 1;
    const float step = 1.0f / std::max(1.0f, static_cast<float>(m_rate) * 0.008f);
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t n = std::min(m_mono.size(), frames - done);
        const float* in = input + done * channels;
        if (channels == 1) {
            std::copy(in, in + n, m_mono.begin());
        } else {
            const float scale = 1.0f / static_cast<float>(channels);
            for (std::size_t f = 0; f < n; ++f) {
                float sum = 0.0f;
                for (std::size_t c = 0; c < channels; ++c) sum += in[f * channels + c];
                m_mono[f] = sum * scale;
            }
        }
        raise_peak(m_mic_peak, peak_of(m_mono.data(), n));
        const float target = mic_open() ? 1.0f : 0.0f;
        if (m_gate != target || target != 1.0f) {
            float g = m_gate;
            for (std::size_t f = 0; f < n; ++f) {
                if (g < target) g = std::min(target, g + step);
                else if (g > target) g = std::max(target, g - step);
                m_mono[f] *= g;
            }
            m_gate = g;
        }
        m_voice.process(m_mono.data(), n);
        raise_peak(m_voice_peak, peak_of(m_mono.data(), n));
        if (m_send_live.load(std::memory_order_relaxed)) m_send_link.write(m_mono.data(), n);
        if (m_hear.load(std::memory_order_relaxed) && m_monitor_live.load(std::memory_order_relaxed)) m_hear_link.write(m_mono.data(), n);
        done += n;
    }
}

void Engine::render_send(float* output, std::size_t frames) noexcept {
    if (!m_send_mixer) {
        std::fill(output, output + frames * 2, 0.0f);
        return;
    }
    const bool voice = m_capture_live.load(std::memory_order_relaxed);
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t n = std::min(m_send_voice.size(), frames - done);
        float* out = output + done * 2;
        m_send_mixer->render(out, n);
        if (voice) {
            m_send_link.read(m_send_voice.data(), n);
            const float g = m_mic_gain.load(std::memory_order_relaxed);
            for (std::size_t f = 0; f < n; ++f) {
                const float v = m_send_voice[f] * g;
                out[f * 2] += v;
                out[f * 2 + 1] += v;
            }
        }
        raise_peak(m_send_peak, peak_of(out, n * 2));
        done += n;
    }
}

void Engine::render_monitor(float* output, std::size_t frames) noexcept {
    if (!m_monitor_mixer) {
        std::fill(output, output + frames * 2, 0.0f);
        return;
    }
    const bool voice = m_capture_live.load(std::memory_order_relaxed) && m_hear.load(std::memory_order_relaxed);
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t n = std::min(m_hear_voice.size(), frames - done);
        float* out = output + done * 2;
        m_monitor_mixer->render(out, n);
        if (voice) {
            m_hear_link.read(m_hear_voice.data(), n);
            const float g = m_hear_gain.load(std::memory_order_relaxed);
            for (std::size_t f = 0; f < n; ++f) {
                const float v = m_hear_voice[f] * g;
                out[f * 2] += v;
                out[f * 2 + 1] += v;
            }
        }
        raise_peak(m_monitor_peak, peak_of(out, n * 2));
        done += n;
    }
}

} // namespace rustle
