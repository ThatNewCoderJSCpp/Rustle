#ifndef RUSTLE_VOICE_HPP
#define RUSTLE_VOICE_HPP

#include <audio.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rustle {

struct VoiceControl {
    std::string id;
    std::string param;
    std::string label;
    std::string unit;
    float low = 0.0f;
    float high = 1.0f;
    float step = 0.01f;
    float fallback = 0.0f;
    int decimals = 2;
    std::string choices;
};

struct VoiceSlotInfo {
    std::string id;
    std::string type;
    std::string label;
    std::string group;
    std::string hint;
    std::vector<VoiceControl> controls;
};

const std::vector<VoiceSlotInfo>& voice_slots();
const VoiceSlotInfo* find_voice_slot(const std::string& id);
const VoiceControl* find_voice_control(const std::string& id, const VoiceSlotInfo** slot = nullptr);

class PhaseInvert : public fizmo::audio::Effect {
public:
    const char* name() const noexcept override { return "rustle_invert"; }
    void prepare(std::uint32_t sample_rate, std::size_t channels) override { set_format(sample_rate, channels); }
    void process(float* interleaved, std::size_t frames) noexcept override;
    void reset() noexcept override {}
};

void register_rustle_effects();

struct VoicePresetInfo {
    std::string name;
    std::string kind;
    std::string file;
    std::string description;
};

class VoiceChain {
public:
    VoiceChain();
    ~VoiceChain();
    VoiceChain(const VoiceChain&) = delete;
    VoiceChain& operator=(const VoiceChain&) = delete;

    void prepare(std::uint32_t sample_rate, std::size_t channels, std::size_t max_block = 4096);
    void process(float* interleaved, std::size_t frames) noexcept;
    std::uint32_t sample_rate() const noexcept { return m_rate; }
    std::size_t channels() const noexcept { return m_channels; }

    std::size_t slot_count() const noexcept { return m_slots.size(); }
    std::shared_ptr<fizmo::audio::Effect> effect(const std::string& slot) const;
    bool set_enabled(const std::string& slot, bool on);
    bool enabled(const std::string& slot) const;
    bool set_value(const std::string& control, float value, bool enable_slot = true);
    float value(const std::string& control) const;
    bool reset_control(const std::string& control);
    bool reset_slot(const std::string& slot);
    void reset_all();
    bool neutral() const;
    std::size_t active_count() const;
    std::size_t latency_frames() const;

    bool apply_chain(const fizmo::audio::EffectChain& chain, std::string* error = nullptr);
    bool apply_builtin(const std::string& name, std::string* error = nullptr);
    std::string save_json(const std::string& name) const;
    bool load_json(const std::string& json, std::string* error = nullptr, std::string* name = nullptr);

    static std::vector<VoicePresetInfo> builtin_presets();

private:
    struct Slot;
    void apply_defaults(Slot& s);
    Slot* slot(const std::string& id);
    const Slot* slot(const std::string& id) const;

    std::vector<std::unique_ptr<Slot>> m_slots;
    std::shared_ptr<fizmo::audio::EffectChain> m_chain;
    std::uint32_t m_rate = 0;
    std::size_t m_channels = 0;
    std::size_t m_block = 0;
    std::size_t m_fade = 480;
    std::vector<float> m_dry;
};

std::string preset_slug(const std::string& name);
std::vector<VoicePresetInfo> list_user_presets(const std::filesystem::path& folder);
bool save_user_preset(const std::filesystem::path& folder, const VoiceChain& chain, const std::string& name, std::string* error = nullptr, std::string* file = nullptr);
bool load_user_preset(const std::filesystem::path& folder, VoiceChain& chain, const std::string& name, std::string* error = nullptr);
bool rename_user_preset(const std::filesystem::path& folder, const std::string& from, const std::string& to, std::string* error = nullptr);
bool delete_user_preset(const std::filesystem::path& folder, const std::string& name, std::string* error = nullptr);

} // namespace rustle

#endif // RUSTLE_VOICE_HPP
