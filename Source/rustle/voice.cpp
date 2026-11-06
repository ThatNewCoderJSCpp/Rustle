#include <rustle/voice.hpp>
#include <rustle/json.hpp>
#include <rustle/library.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;
namespace fa = fizmo::audio;

namespace rustle {

namespace {

VoiceControl control(const std::string& slot, const std::string& param, const std::string& label, const std::string& unit, float low, float high, float step, float fallback,
    int decimals, const std::string& choices = std::string()) {
    VoiceControl c;
    c.id = slot + "." + param;
    c.param = param;
    c.label = label;
    c.unit = unit;
    c.low = low;
    c.high = high;
    c.step = step;
    c.fallback = fallback;
    c.decimals = decimals;
    c.choices = choices;
    return c;
}

std::vector<VoiceSlotInfo> make_slots() {
    std::vector<VoiceSlotInfo> s;
    auto add = [&](const std::string& id, const std::string& type, const std::string& label, const std::string& group, const std::string& hint) -> VoiceSlotInfo& {
        VoiceSlotInfo info;
        info.id = id;
        info.type = type;
        info.label = label;
        info.group = group;
        info.hint = hint;
        s.push_back(info);
        return s.back();
    };
    {
        VoiceSlotInfo& g = add("gate", "noise_gate", "Noise gate", "Clean up", "Silences the mic while you aren't talking, so keyboard and fan noise don't get through.");
        g.controls.push_back(control("gate", "threshold", "Open above", "dB", -80.0f, 0.0f, 1.0f, -45.0f, 0));
        g.controls.push_back(control("gate", "release", "Release", "ms", 10.0f, 1000.0f, 5.0f, 150.0f, 0));
    }
    {
        VoiceSlotInfo& d = add("denoise", "noise_reduction", "Noise reduction", "Clean up", "Learns the steady background hiss and hum and takes it out of your voice.");
        d.controls.push_back(control("denoise", "reduction_db", "Amount", "dB", 0.0f, 40.0f, 1.0f, 15.0f, 0));
        d.controls.push_back(control("denoise", "sensitivity", "Sensitivity", "", 0.0f, 1.0f, 0.05f, 0.5f, 2));
    }
    {
        VoiceSlotInfo& h = add("highpass", "filter", "Low cut", "Clean up", "Cuts rumble and booming below this frequency (a high-pass filter).");
        h.controls.push_back(control("highpass", "frequency", "Cut below", "Hz", 20.0f, 1500.0f, 5.0f, 100.0f, 0));
    }
    {
        VoiceSlotInfo& p = add("pitch", "pitch_shifter", "Pitch", "Character", "Moves your voice up or down in semitones; 12 is an octave.");
        p.controls.push_back(control("pitch", "semitones", "Semitones", "st", -12.0f, 12.0f, 0.5f, 0.0f, 1));
        p.controls.push_back(control("pitch", "cents", "Fine tune", "ct", -100.0f, 100.0f, 1.0f, 0.0f, 0));
        p.controls.push_back(control("pitch", "preserve_formants", "Keep formants", "", 0.0f, 1.0f, 1.0f, 0.0f, 0, "0=Off|1=On"));
        p.controls.push_back(control("pitch", "mode", "Method", "", 0.0f, 1.0f, 1.0f, 0.0f, 0, "0=Fast (voice)|1=Smooth (vocoder)"));
    }
    {
        VoiceSlotInfo& f = add("formant", "formant_shifter", "Formant", "Character", "Changes the size of the throat the voice seems to come from, without changing the pitch.");
        f.controls.push_back(control("formant", "shift", "Shift", "st", -12.0f, 12.0f, 0.5f, 0.0f, 1));
    }
    {
        VoiceSlotInfo& c = add("crush", "bitcrusher", "Bitcrush", "Character", "Lo-fi: fewer bits and a lower sample rate, like an old game console.");
        c.controls.push_back(control("crush", "bits", "Bits", "", 1.0f, 16.0f, 1.0f, 8.0f, 0));
        c.controls.push_back(control("crush", "rate_hz", "Sample rate", "Hz", 500.0f, 48000.0f, 100.0f, 11025.0f, 0));
        c.controls.push_back(control("crush", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 1.0f, 2));
    }
    {
        VoiceSlotInfo& v = add("voice_eq", "voice_eq", "Voice tone", "Tone", "A band-pass with one bell: what the radio, telephone and megaphone presets use.");
        v.controls.push_back(control("voice_eq", "low_cut", "Low cut", "Hz", 20.0f, 2000.0f, 10.0f, 20.0f, 0));
        v.controls.push_back(control("voice_eq", "high_cut", "High cut", "Hz", 500.0f, 22000.0f, 100.0f, 22000.0f, 0));
        v.controls.push_back(control("voice_eq", "peak_hz", "Bell at", "Hz", 50.0f, 12000.0f, 10.0f, 1000.0f, 0));
        v.controls.push_back(control("voice_eq", "peak_db", "Bell", "dB", -24.0f, 24.0f, 0.5f, 0.0f, 1));
    }
    {
        VoiceSlotInfo& v = add("vocoder", "vocoder", "Synth voice", "Character", "A vocoder: your words played on a synthesizer note. Great for robots and daft-punk voices.");
        v.controls.push_back(control("vocoder", "carrier", "Sound", "", 0.0f, 2.0f, 1.0f, 0.0f, 0, "0=Saw|1=Square|2=Noise (whisper)"));
        v.controls.push_back(control("vocoder", "pitch", "Note", "Hz", 20.0f, 1000.0f, 1.0f, 110.0f, 0));
        v.controls.push_back(control("vocoder", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 1.0f, 2));
    }
    {
        VoiceSlotInfo& r = add("robot", "ring_modulator", "Robot", "Character", "Ring modulation: multiplies your voice with a tone for a metallic, robotic sound.");
        r.controls.push_back(control("robot", "frequency", "Tone", "Hz", 10.0f, 2000.0f, 1.0f, 70.0f, 0));
        r.controls.push_back(control("robot", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 1.0f, 2));
    }
    {
        VoiceSlotInfo& d = add("distortion", "distortion", "Distortion", "Character", "Overdrives the voice, from warm crunch to blown-out.");
        d.controls.push_back(control("distortion", "drive", "Drive", "dB", 0.0f, 48.0f, 0.5f, 12.0f, 1));
        d.controls.push_back(control("distortion", "mode", "Type", "", 0.0f, 3.0f, 1.0f, 0.0f, 0, "0=Soft|1=Hard|2=Tube|3=Fold"));
        d.controls.push_back(control("distortion", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 1.0f, 2));
    }
    {
        VoiceSlotInfo& f = add("flanger", "flanger", "Flanger", "Movement", "A sweeping jet-plane whoosh.");
        f.controls.push_back(control("flanger", "rate", "Speed", "Hz", 0.05f, 5.0f, 0.05f, 0.25f, 2));
        f.controls.push_back(control("flanger", "feedback", "Feedback", "", -0.95f, 0.95f, 0.05f, 0.5f, 2));
        f.controls.push_back(control("flanger", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 0.5f, 2));
    }
    {
        VoiceSlotInfo& c = add("chorus", "chorus", "Chorus", "Movement", "Sounds like several of you talking at once.");
        c.controls.push_back(control("chorus", "rate", "Speed", "Hz", 0.05f, 5.0f, 0.05f, 0.8f, 2));
        c.controls.push_back(control("chorus", "depth_ms", "Depth", "ms", 0.0f, 20.0f, 0.5f, 3.0f, 1));
        c.controls.push_back(control("chorus", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 0.5f, 2));
    }
    {
        VoiceSlotInfo& v = add("vibrato", "vibrato", "Vibrato", "Movement", "Wobbles the pitch up and down.");
        v.controls.push_back(control("vibrato", "rate", "Speed", "Hz", 0.1f, 15.0f, 0.1f, 5.0f, 1));
        v.controls.push_back(control("vibrato", "depth_ms", "Depth", "ms", 0.0f, 10.0f, 0.1f, 1.5f, 1));
    }
    {
        VoiceSlotInfo& t = add("tremolo", "tremolo", "Tremolo", "Movement", "Wobbles the volume up and down.");
        t.controls.push_back(control("tremolo", "rate", "Speed", "Hz", 0.1f, 20.0f, 0.1f, 5.0f, 1));
        t.controls.push_back(control("tremolo", "depth", "Depth", "", 0.0f, 1.0f, 0.01f, 0.5f, 2));
    }
    {
        VoiceSlotInfo& e = add("echo", "delay", "Echo", "Space", "Repeats what you say after a short delay.");
        e.controls.push_back(control("echo", "time_ms", "Delay", "ms", 20.0f, 1500.0f, 5.0f, 300.0f, 0));
        e.controls.push_back(control("echo", "feedback", "Repeats", "", 0.0f, 0.95f, 0.01f, 0.35f, 2));
        e.controls.push_back(control("echo", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 0.3f, 2));
    }
    {
        VoiceSlotInfo& r = add("reverb", "reverb", "Reverb", "Space", "Puts you in a room, a hall or a cave.");
        r.controls.push_back(control("reverb", "size", "Room size", "", 0.0f, 1.0f, 0.01f, 0.5f, 2));
        r.controls.push_back(control("reverb", "decay", "Decay", "s", 0.1f, 10.0f, 0.1f, 2.0f, 1));
        r.controls.push_back(control("reverb", "mix", "Mix", "", 0.0f, 1.0f, 0.01f, 0.3f, 2));
    }
    {
        VoiceSlotInfo& l = add("lowpass", "filter", "High cut", "Tone", "Cuts hiss and brightness above this frequency (a low-pass filter); low values sound muffled.");
        l.controls.push_back(control("lowpass", "frequency", "Cut above", "Hz", 300.0f, 20000.0f, 50.0f, 8000.0f, 0));
    }
    {
        VoiceSlotInfo& q = add("eq", "parametric_eq", "Equalizer", "Tone", "Bass, middle and treble.");
        q.controls.push_back(control("eq", "band1_gain", "Bass", "dB", -18.0f, 18.0f, 0.5f, 0.0f, 1));
        q.controls.push_back(control("eq", "band2_gain", "Middle", "dB", -18.0f, 18.0f, 0.5f, 0.0f, 1));
        q.controls.push_back(control("eq", "band3_gain", "Treble", "dB", -18.0f, 18.0f, 0.5f, 0.0f, 1));
    }
    {
        VoiceSlotInfo& c = add("compressor", "compressor", "Compressor", "Level", "Evens out loud and quiet parts so you're easier to hear.");
        c.controls.push_back(control("compressor", "threshold", "Threshold", "dB", -60.0f, 0.0f, 1.0f, -20.0f, 0));
        c.controls.push_back(control("compressor", "ratio", "Ratio", ":1", 1.0f, 20.0f, 0.5f, 4.0f, 1));
        c.controls.push_back(control("compressor", "makeup", "Makeup", "dB", 0.0f, 24.0f, 0.5f, 6.0f, 1));
    }
    {
        VoiceSlotInfo& g = add("gain", "gain", "Volume", "Level", "Makes your voice louder or quieter after the effects.");
        g.controls.push_back(control("gain", "db", "Gain", "dB", -24.0f, 24.0f, 0.5f, 0.0f, 1));
    }
    add("invert", "rustle_invert", "Phase invert", "Level", "Flips the waveform upside down (the live 'inverse'). It sounds the same on its own; it matters when mixed with the original. Reversing a voice needs the whole sentence first, so it's only offered for soundboard clips.");
    {
        VoiceSlotInfo& l = add("limiter", "limiter", "Limiter", "Level", "Stops loud moments from clipping. Turn it on when you use lots of gain or distortion.");
        l.controls.push_back(control("limiter", "ceiling", "Ceiling", "dB", -24.0f, 0.0f, 0.5f, -1.0f, 1));
    }
    return s;
}

std::shared_ptr<fa::Effect> make_effect(const VoiceSlotInfo& info) {
    if (info.id == "highpass") return std::make_shared<fa::Filter>(fa::FilterType::highpass, 100.0f);
    if (info.id == "lowpass") return std::make_shared<fa::Filter>(fa::FilterType::lowpass, 8000.0f);
    if (info.id == "eq") {
        auto eq = std::make_shared<fa::ParametricEq>(3);
        eq->set_band(0, fa::FilterType::lowshelf, 150.0f, 0.7071f, 0.0f, true);
        eq->set_band(1, fa::FilterType::peak, 1000.0f, 0.8f, 0.0f, true);
        eq->set_band(2, fa::FilterType::highshelf, 5000.0f, 0.7071f, 0.0f, true);
        return eq;
    }
    if (info.id == "invert") return std::make_shared<PhaseInvert>();
    if (info.type == "noise_gate") return std::make_shared<fa::NoiseGate>();
    if (info.type == "noise_reduction") return std::make_shared<fa::NoiseReduction>();
    if (info.type == "pitch_shifter") return std::make_shared<fa::PitchShifter>();
    if (info.type == "formant_shifter") return std::make_shared<fa::FormantShifter>();
    if (info.type == "bitcrusher") return std::make_shared<fa::Bitcrusher>();
    if (info.type == "voice_eq") return std::make_shared<fa::VoiceEQ>();
    if (info.type == "vocoder") return std::make_shared<fa::Vocoder>();
    if (info.type == "ring_modulator") return std::make_shared<fa::RingModulator>();
    if (info.type == "distortion") return std::make_shared<fa::Distortion>();
    if (info.type == "flanger") return std::make_shared<fa::Flanger>();
    if (info.type == "chorus") return std::make_shared<fa::Chorus>();
    if (info.type == "vibrato") return std::make_shared<fa::Vibrato>();
    if (info.type == "tremolo") return std::make_shared<fa::Tremolo>();
    if (info.type == "delay") return std::make_shared<fa::Delay>();
    if (info.type == "reverb") return std::make_shared<fa::Reverb>();
    if (info.type == "compressor") return std::make_shared<fa::Compressor>();
    if (info.type == "gain") return std::make_shared<fa::Gain>();
    if (info.type == "limiter") return std::make_shared<fa::Limiter>();
    return nullptr;
}

std::string effect_type(const fa::Effect& e) {
    return e.type();
}

int filter_kind(const fa::Effect& e) {
    const fa::Parameter* p = e.parameter("type");
    if (!p) return -1;
    return static_cast<int>(std::lround(p->get()));
}

struct RustlePreset {
    const char* name;
    const char* description;
    std::vector<std::pair<const char*, float>> values;
};

const std::vector<RustlePreset>& rustle_presets() {
    static const std::vector<RustlePreset> list = {
        { "Chipmunk", "Fast and squeaky", { { "pitch.semitones", 9.0f }, { "formant.shift", 4.0f }, { "highpass.frequency", 180.0f } } },
        { "Demon", "Deep, rough and echoing", { { "pitch.semitones", -8.0f }, { "formant.shift", -3.0f }, { "distortion.drive", 9.0f }, { "distortion.mix", 0.35f }, { "reverb.size", 0.75f }, { "reverb.mix", 0.25f }, { "eq.band1_gain", 5.0f }, { "limiter.ceiling", -1.0f } } },
        { "Walkie-talkie", "Crackly handheld radio", { { "gate.threshold", -42.0f }, { "highpass.frequency", 450.0f }, { "crush.bits", 9.0f }, { "crush.rate_hz", 9000.0f }, { "distortion.drive", 16.0f }, { "distortion.mix", 0.5f }, { "lowpass.frequency", 3000.0f }, { "compressor.threshold", -24.0f }, { "compressor.ratio", 6.0f } } },
        { "Stadium", "Announcer in a huge arena", { { "compressor.threshold", -22.0f }, { "compressor.ratio", 4.0f }, { "echo.time_ms", 240.0f }, { "echo.feedback", 0.3f }, { "echo.mix", 0.22f }, { "reverb.size", 0.95f }, { "reverb.decay", 4.5f }, { "reverb.mix", 0.35f } } },
        { "Ghost", "Floaty and haunted", { { "pitch.semitones", 3.0f }, { "chorus.rate", 0.3f }, { "chorus.mix", 0.5f }, { "tremolo.rate", 3.0f }, { "tremolo.depth", 0.35f }, { "reverb.size", 0.85f }, { "reverb.mix", 0.5f }, { "lowpass.frequency", 6000.0f } } },
        { "Broadcast", "Clean, close and level, like a radio host", { { "highpass.frequency", 90.0f }, { "gate.threshold", -50.0f }, { "denoise.reduction_db", 12.0f }, { "compressor.threshold", -22.0f }, { "compressor.ratio", 3.0f }, { "compressor.makeup", 5.0f }, { "eq.band3_gain", 3.0f }, { "limiter.ceiling", -1.0f } } },
        { "Space robot", "Vocoder robot with a metallic ring", { { "vocoder.pitch", 98.0f }, { "vocoder.mix", 0.8f }, { "robot.frequency", 45.0f }, { "robot.mix", 0.4f }, { "echo.time_ms", 120.0f }, { "echo.mix", 0.15f } } },
    };
    return list;
}

std::string title_case(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string builtin_description(const std::string& name) {
    static const std::map<std::string, std::string> d = {
        { "helium", "High and squeaky (the classic chipmunk)" }, { "deep", "Lower and bigger" }, { "robot", "Flat, buzzing machine voice" },
        { "alien", "Wobbly and otherworldly" }, { "monster", "Huge, growling and low" }, { "radio", "Old AM radio" }, { "telephone", "Thin phone line" },
        { "megaphone", "Loud, distorted bullhorn" }, { "cave", "Big echoing cave" }, { "underwater", "Muffled and bubbly" }, { "whisper", "Breathy whisper" },
        { "feminine", "Higher with lighter formants" }, { "masculine", "Lower with deeper formants" },
    };
    auto it = d.find(name);
    return it == d.end() ? std::string() : it->second;
}

} // namespace

const std::vector<VoiceSlotInfo>& voice_slots() {
    static const std::vector<VoiceSlotInfo> slots = make_slots();
    return slots;
}

const VoiceSlotInfo* find_voice_slot(const std::string& id) {
    for (const VoiceSlotInfo& s : voice_slots()) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const VoiceControl* find_voice_control(const std::string& id, const VoiceSlotInfo** slot) {
    for (const VoiceSlotInfo& s : voice_slots()) {
        for (const VoiceControl& c : s.controls) {
            if (c.id == id) {
                if (slot) *slot = &s;
                return &c;
            }
        }
    }
    return nullptr;
}

void PhaseInvert::process(float* interleaved, std::size_t frames) noexcept {
    const std::size_t n = frames * channels();
    for (std::size_t i = 0; i < n; ++i) interleaved[i] = -interleaved[i];
}

void register_rustle_effects() {
    static std::once_flag once;
    std::call_once(once, [] {
        fa::register_effect("rustle_invert", [] { return std::make_shared<PhaseInvert>(); }, "Phase invert", "utility");
    });
}

struct VoiceChain::Slot {
    const VoiceSlotInfo* info = nullptr;
    std::shared_ptr<fa::Effect> effect;
    std::map<std::string, float> defaults;
    bool active = false;
    float mix = 0.0f;
};

VoiceChain::VoiceChain() : m_chain(std::make_shared<fa::EffectChain>()) {
    register_rustle_effects();
    for (const VoiceSlotInfo& info : voice_slots()) {
        auto s = std::make_unique<Slot>();
        s->info = &info;
        s->effect = make_effect(info);
        for (fa::Parameter* p : s->effect->parameters()) s->defaults[p->name()] = p->get();
        for (const VoiceControl& c : info.controls) s->defaults[c.param] = c.fallback;
        apply_defaults(*s);
        s->effect->set_enabled(false);
        m_chain->add(s->effect);
        m_slots.push_back(std::move(s));
    }
}

VoiceChain::~VoiceChain() = default;

void VoiceChain::apply_defaults(Slot& s) {
    for (fa::Parameter* p : s.effect->parameters()) {
        auto it = s.defaults.find(p->name());
        if (it != s.defaults.end()) p->set(it->second);
    }
}

VoiceChain::Slot* VoiceChain::slot(const std::string& id) {
    for (auto& s : m_slots) {
        if (s->info->id == id) return s.get();
    }
    return nullptr;
}

const VoiceChain::Slot* VoiceChain::slot(const std::string& id) const {
    for (const auto& s : m_slots) {
        if (s->info->id == id) return s.get();
    }
    return nullptr;
}

void VoiceChain::prepare(std::uint32_t sample_rate, std::size_t channels, std::size_t max_block) {
    m_rate = sample_rate ? sample_rate : 48000;
    m_channels = channels ? channels : 1;
    m_block = std::max<std::size_t>(64, max_block);
    m_fade = std::max<std::size_t>(16, static_cast<std::size_t>(m_rate / 100));
    m_dry.assign(m_block * m_channels, 0.0f);
    for (auto& s : m_slots) {
        s->effect->prepare(m_rate, m_channels);
        s->effect->reset();
        s->active = s->effect->enabled();
        s->mix = s->active ? 1.0f : 0.0f;
    }
}

void VoiceChain::process(float* interleaved, std::size_t frames) noexcept {
    if (!m_rate || !interleaved) return;
    const std::size_t ch = m_channels;
    const float step = 1.0f / static_cast<float>(m_fade);
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t n = std::min(m_block, frames - done);
        float* buf = interleaved + done * ch;
        for (auto& sp : m_slots) {
            Slot& s = *sp;
            const bool want = s.effect->enabled();
            if (!s.active) {
                if (!want) continue;
                s.effect->reset();
                s.active = true;
                s.mix = 0.0f;
            }
            const float target = want ? 1.0f : 0.0f;
            if (want && s.mix >= 1.0f) {
                s.effect->process(buf, n);
                continue;
            }
            std::copy(buf, buf + n * ch, m_dry.begin());
            s.effect->process(buf, n);
            float g = s.mix;
            for (std::size_t f = 0; f < n; ++f) {
                if (g < target) g = std::min(target, g + step);
                else if (g > target) g = std::max(target, g - step);
                for (std::size_t c = 0; c < ch; ++c) {
                    const std::size_t i = f * ch + c;
                    buf[i] = m_dry[i] + (buf[i] - m_dry[i]) * g;
                }
            }
            s.mix = g;
            if (!want && g <= 0.0f) s.active = false;
        }
        done += n;
    }
}

std::shared_ptr<fa::Effect> VoiceChain::effect(const std::string& id) const {
    const Slot* s = slot(id);
    return s ? s->effect : nullptr;
}

bool VoiceChain::set_enabled(const std::string& id, bool on) {
    Slot* s = slot(id);
    if (!s) return false;
    s->effect->set_enabled(on);
    return true;
}

bool VoiceChain::enabled(const std::string& id) const {
    const Slot* s = slot(id);
    return s && s->effect->enabled();
}

bool VoiceChain::set_value(const std::string& id, float value, bool enable_slot) {
    const VoiceSlotInfo* info = nullptr;
    const VoiceControl* c = find_voice_control(id, &info);
    if (!c || !std::isfinite(value)) return false;
    Slot* s = slot(info->id);
    if (!s) return false;
    fa::Parameter* p = s->effect->parameter(c->param);
    if (!p) return false;
    p->set(std::clamp(value, c->low, c->high));
    if (enable_slot) s->effect->set_enabled(true);
    return true;
}

float VoiceChain::value(const std::string& id) const {
    const VoiceSlotInfo* info = nullptr;
    const VoiceControl* c = find_voice_control(id, &info);
    if (!c) return 0.0f;
    const Slot* s = slot(info->id);
    const fa::Parameter* p = s ? static_cast<const fa::Effect&>(*s->effect).parameter(c->param) : nullptr;
    return p ? p->get() : c->fallback;
}

bool VoiceChain::reset_control(const std::string& id) {
    const VoiceSlotInfo* info = nullptr;
    const VoiceControl* c = find_voice_control(id, &info);
    if (!c) return false;
    Slot* s = slot(info->id);
    fa::Parameter* p = s ? s->effect->parameter(c->param) : nullptr;
    if (!p) return false;
    p->set(c->fallback);
    return true;
}

bool VoiceChain::reset_slot(const std::string& id) {
    Slot* s = slot(id);
    if (!s) return false;
    s->effect->set_enabled(false);
    apply_defaults(*s);
    return true;
}

void VoiceChain::reset_all() {
    for (auto& s : m_slots) {
        s->effect->set_enabled(false);
        apply_defaults(*s);
    }
}

bool VoiceChain::neutral() const {
    for (const auto& s : m_slots) {
        if (s->effect->enabled()) return false;
    }
    return true;
}

std::size_t VoiceChain::active_count() const {
    std::size_t n = 0;
    for (const auto& s : m_slots) n += s->effect->enabled() ? 1 : 0;
    return n;
}

std::size_t VoiceChain::latency_frames() const {
    std::size_t n = 0;
    for (const auto& s : m_slots) {
        if (s->effect->enabled()) n += s->effect->latency_frames();
    }
    return n;
}

bool VoiceChain::apply_chain(const fa::EffectChain& chain, std::string* error) {
    reset_all();
    std::vector<bool> used(m_slots.size(), false);
    std::string skipped;
    for (const std::shared_ptr<fa::Effect>& e : chain.effects()) {
        if (!e) continue;
        const std::string type = effect_type(*e);
        std::size_t pick = m_slots.size();
        for (std::size_t i = 0; i < m_slots.size(); ++i) {
            if (used[i] || m_slots[i]->info->type != type) continue;
            if (type == "filter") {
                const int k = filter_kind(*e);
                const bool wants_low = m_slots[i]->info->id == "highpass";
                if (k == static_cast<int>(fa::FilterType::highpass) && !wants_low) continue;
                if (k == static_cast<int>(fa::FilterType::lowpass) && wants_low) continue;
            }
            pick = i;
            break;
        }
        if (pick == m_slots.size()) {
            skipped += (skipped.empty() ? "" : ", ") + type;
            continue;
        }
        used[pick] = true;
        Slot& s = *m_slots[pick];
        for (const fa::Parameter* p : static_cast<const fa::Effect&>(*e).parameters()) {
            fa::Parameter* mine = s.effect->parameter(p->name());
            if (mine) mine->set(p->get());
        }
        s.effect->set_enabled(e->enabled());
    }
    if (!skipped.empty() && error) *error = "Rustle has no place for: " + skipped;
    return true;
}

bool VoiceChain::apply_builtin(const std::string& name, std::string* error) {
    const std::string key = lower_text(name);
    if (key == "none" || key == "off" || key == "neutral") {
        reset_all();
        return true;
    }
    for (const RustlePreset& p : rustle_presets()) {
        if (lower_text(p.name) != key) continue;
        reset_all();
        for (const auto& v : p.values) set_value(v.first, v.second, true);
        return true;
    }
    std::shared_ptr<fa::EffectChain> chain = fa::builtin_voice_preset(key);
    if (!chain) {
        if (error) *error = "there's no built-in preset called " + name;
        return false;
    }
    return apply_chain(*chain, error);
}

std::vector<VoicePresetInfo> VoiceChain::builtin_presets() {
    std::vector<VoicePresetInfo> out;
    for (const std::string& n : fa::builtin_voice_preset_names()) {
        if (n == "none") continue;
        out.push_back({ title_case(n), "fizmo", std::string(), builtin_description(n) });
    }
    for (const RustlePreset& p : rustle_presets()) out.push_back({ p.name, "rustle", std::string(), p.description });
    return out;
}

std::string VoiceChain::save_json(const std::string& name) const {
    fa::PresetInfo info;
    info.name = name;
    info.author = "Rustle";
    info.category = "voice";
    info.version = "1";
    return fa::save_preset(*m_chain, info);
}

bool VoiceChain::load_json(const std::string& json, std::string* error, std::string* name) {
    fa::PresetInfo info;
    std::string why;
    std::shared_ptr<fa::EffectChain> chain = fa::load_preset_chain(json, &info, &why);
    if (!chain) {
        if (error) *error = why.empty() ? "that isn't a preset" : why;
        return false;
    }
    if (name) *name = info.name;
    return apply_chain(*chain, error);
}

std::string preset_slug(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out += static_cast<char>(std::tolower(u));
        else if (u >= 0x80) out += c;
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "preset";
    return out;
}

std::vector<VoicePresetInfo> list_user_presets(const fs::path& folder) {
    std::vector<VoicePresetInfo> out;
    std::error_code ec;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->path().extension() != ".json") continue;
        std::string text;
        if (!read_text_file(it->path(), text)) continue;
        fa::PresetInfo info;
        if (!fa::read_preset_info(text, info)) continue;
        VoicePresetInfo p;
        p.name = info.name.empty() ? it->path().stem().string() : info.name;
        p.kind = "user";
        p.file = it->path().string();
        p.description = info.description;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const VoicePresetInfo& a, const VoicePresetInfo& b) { return lower_text(a.name) < lower_text(b.name); });
    return out;
}

namespace {

fs::path find_user_preset(const fs::path& folder, const std::string& name) {
    for (const VoicePresetInfo& p : list_user_presets(folder)) {
        if (p.name == name) return p.file;
    }
    for (const VoicePresetInfo& p : list_user_presets(folder)) {
        if (lower_text(p.name) == lower_text(name)) return p.file;
    }
    return fs::path();
}

} // namespace

bool save_user_preset(const fs::path& folder, const VoiceChain& chain, const std::string& name, std::string* error, std::string* file) {
    std::string clean = name;
    while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.back()))) clean.pop_back();
    while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.front()))) clean.erase(0, 1);
    if (clean.empty()) {
        if (error) *error = "give the preset a name";
        return false;
    }
    fs::path target = find_user_preset(folder, clean);
    if (target.empty()) {
        target = folder / (preset_slug(clean) + ".json");
        std::error_code ec;
        for (int i = 2; fs::exists(target, ec) && i < 1000; ++i) target = folder / (preset_slug(clean) + "-" + std::to_string(i) + ".json");
    }
    if (!write_text_file_atomic(target, chain.save_json(clean), error)) return false;
    if (file) *file = target.string();
    return true;
}

bool load_user_preset(const fs::path& folder, VoiceChain& chain, const std::string& name, std::string* error) {
    const fs::path file = find_user_preset(folder, name);
    if (file.empty()) {
        if (error) *error = "there's no saved preset called " + name;
        return false;
    }
    std::string text;
    if (!read_text_file(file, text, error)) return false;
    return chain.load_json(text, error);
}

bool rename_user_preset(const fs::path& folder, const std::string& from, const std::string& to, std::string* error) {
    const fs::path file = find_user_preset(folder, from);
    if (file.empty()) {
        if (error) *error = "there's no saved preset called " + from;
        return false;
    }
    std::string clean = to;
    while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.back()))) clean.pop_back();
    while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.front()))) clean.erase(0, 1);
    if (clean.empty()) {
        if (error) *error = "give the preset a name";
        return false;
    }
    if (clean != from && !find_user_preset(folder, clean).empty() && lower_text(clean) != lower_text(from)) {
        if (error) *error = "there's already a preset called " + clean;
        return false;
    }
    std::string text;
    if (!read_text_file(file, text, error)) return false;
    fa::PresetInfo info;
    std::string why;
    std::shared_ptr<fa::EffectChain> chain = fa::load_preset_chain(text, &info, &why);
    if (!chain) {
        if (error) *error = why;
        return false;
    }
    info.name = clean;
    fs::path target = folder / (preset_slug(clean) + ".json");
    std::error_code ec;
    if (target != file) {
        for (int i = 2; fs::exists(target, ec) && i < 1000; ++i) target = folder / (preset_slug(clean) + "-" + std::to_string(i) + ".json");
    }
    if (!write_text_file_atomic(target, fa::save_preset(*chain, info), error)) return false;
    if (target != file) fs::remove(file, ec);
    return true;
}

bool delete_user_preset(const fs::path& folder, const std::string& name, std::string* error) {
    const fs::path file = find_user_preset(folder, name);
    if (file.empty()) {
        if (error) *error = "there's no saved preset called " + name;
        return false;
    }
    std::error_code ec;
    if (!fs::remove(file, ec)) {
        if (error) *error = "can't delete " + file.string() + (ec ? ": " + ec.message() : std::string());
        return false;
    }
    return true;
}

} // namespace rustle
