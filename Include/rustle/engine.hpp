#ifndef RUSTLE_ENGINE_HPP
#define RUSTLE_ENGINE_HPP

#include <rustle/ring.hpp>
#include <rustle/voice.hpp>

#include <audio.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rustle {

struct EngineConfig {
    std::uint32_t sample_rate = 48000;
    bool capture = true;
    std::string input_device;
    bool send = true;
    std::string send_device;
    bool monitor = true;
    std::string monitor_device;
    std::uint32_t latency_ms = 20;
};

struct StreamStatus {
    bool wanted = false;
    bool running = false;
    std::string device;
    std::string error;
};

struct PlayingSound {
    std::uint64_t id = 0;
    std::string tag;
    double position = 0.0;
    double length = 0.0;
    bool loop = false;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start(const EngineConfig& config, std::string* error = nullptr);
    void stop();
    void prepare_offline(std::uint32_t sample_rate);
    bool started() const;
    const EngineConfig& config() const { return m_config; }
    std::uint32_t sample_rate() const { return m_rate; }

    StreamStatus capture_status() const;
    StreamStatus send_status() const;
    StreamStatus monitor_status() const;
    bool any_stream_failed() const;

    VoiceChain& voice() { return m_voice; }
    const VoiceChain& voice() const { return m_voice; }

    void set_mic_enabled(bool on) { m_mic_on.store(on); }
    bool mic_enabled() const { return m_mic_on.load(); }
    void set_push_to_talk(bool on) { m_ptt.store(on); }
    bool push_to_talk() const { return m_ptt.load(); }
    void set_talking(bool held) { m_talk.store(held); }
    bool talking() const { return m_talk.load(); }
    bool mic_open() const;
    void set_hear_myself(bool on) { m_hear.store(on); }
    bool hear_myself() const { return m_hear.load(); }
    void set_mic_volume(float v) { m_mic_gain.store(v); }
    void set_hear_volume(float v) { m_hear_gain.store(v); }
    void set_sound_gains(float sounds, float others, float me, bool to_others, bool to_me);

    std::uint64_t play(const fizmo::audio::Sound& sound, float gain, bool loop, const std::string& tag);
    void stop_sound(std::uint64_t id, double fade = 0.02);
    std::size_t stop_tag(const std::string& tag, double fade = 0.02);
    void stop_all(double fade = 0.02);
    void set_sound_gain(std::uint64_t id, float gain);
    void set_sound_loop(std::uint64_t id, bool loop);
    bool is_playing(std::uint64_t id) const;
    std::vector<PlayingSound> playing() const;
    bool tag_playing(const std::string& tag) const;

    float take_mic_level();
    float take_voice_level();
    float take_send_level();
    float take_monitor_level();

    void process_capture(const float* input, std::size_t frames, std::size_t channels) noexcept;
    void render_send(float* output, std::size_t frames) noexcept;
    void render_monitor(float* output, std::size_t frames) noexcept;

    std::size_t voice_underruns() const { return m_send_link.underruns(); }

private:
    struct Play {
        std::uint64_t id = 0;
        std::string tag;
        fizmo::audio::Mixer::VoiceId send = 0;
        fizmo::audio::Mixer::VoiceId monitor = 0;
        double length = 0.0;
        bool loop = false;
    };

    void cleanup_locked() const;
    void allocate(std::uint32_t rate);
    void close_streams();

    EngineConfig m_config;
    std::uint32_t m_rate = 48000;
    bool m_started = false;
    bool m_offline = false;

    VoiceChain m_voice;
    std::unique_ptr<fizmo::audio::Mixer> m_send_mixer;
    std::unique_ptr<fizmo::audio::Mixer> m_monitor_mixer;
    std::atomic<bool> m_capture_live{ false };
    std::atomic<bool> m_send_live{ false };
    std::atomic<bool> m_monitor_live{ false };

    fizmo::audio::Stream m_capture;
    fizmo::audio::Stream m_send;
    fizmo::audio::Stream m_monitor;
    StreamStatus m_capture_status;
    StreamStatus m_send_status;
    StreamStatus m_monitor_status;

    VoiceLink m_send_link;
    VoiceLink m_hear_link;
    std::vector<float> m_mono;
    std::vector<float> m_send_voice;
    std::vector<float> m_hear_voice;

    std::atomic<bool> m_mic_on{ true };
    std::atomic<bool> m_ptt{ false };
    std::atomic<bool> m_talk{ false };
    std::atomic<bool> m_hear{ false };
    std::atomic<float> m_mic_gain{ 1.0f };
    std::atomic<float> m_hear_gain{ 0.7f };
    float m_gate = 0.0f;

    std::atomic<float> m_mic_peak{ 0.0f };
    std::atomic<float> m_voice_peak{ 0.0f };
    std::atomic<float> m_send_peak{ 0.0f };
    std::atomic<float> m_monitor_peak{ 0.0f };

    mutable std::mutex m_mutex;
    mutable std::map<std::uint64_t, Play> m_plays;
    std::uint64_t m_next = 1;
};

std::vector<fizmo::audio::DeviceInfo> usable_devices(fizmo::audio::DeviceKind kind);
bool is_rustle_device(const std::string& id);

} // namespace rustle

#endif // RUSTLE_ENGINE_HPP
