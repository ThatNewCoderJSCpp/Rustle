#include <rustle/clip.hpp>

#include <algorithm>
#include <cmath>

namespace fa = fizmo::audio;

namespace rustle {

bool decode_sound(const std::string& path, fa::AudioBuffer& out, std::string* error) {
    std::string why;
    if (!fa::load_audio(path, out, &why)) {
        if (error) *error = why.empty() ? "can't read " + path : why;
        return false;
    }
    if (out.empty() || out.channels() == 0 || out.sample_rate() == 0) {
        if (error) *error = "there's no sound in " + path;
        return false;
    }
    return true;
}

double probe_duration(const std::string& path) {
    fa::AudioFileInfo info;
    if (fa::read_audio_info(path, info) && info.duration > 0.0) return info.duration;
    fa::AudioBuffer buf;
    if (decode_sound(path, buf)) return buf.duration();
    return -2.0;
}

fa::AudioBuffer render_clip(const fa::AudioBuffer& source, const ClipEdit& raw, std::uint32_t rate, std::size_t channels) {
    ClipEdit edit = raw;
    edit.clamp();
    if (source.empty() || source.channels() == 0 || source.sample_rate() == 0) return fa::AudioBuffer(channels ? channels : 2, rate ? rate : 48000);
    fa::AudioBuffer buf = source;
    const std::size_t total = buf.frames();
    std::size_t start = std::min(total, buf.frame_at(edit.trim_start));
    std::size_t end = edit.trim_end > 0.0 ? std::min(total, buf.frame_at(edit.trim_end)) : total;
    if (end <= start) {
        start = 0;
        end = total;
    }
    if (start > 0 || end < total) buf = buf.slice(start, end - start);
    if (edit.reverse) fa::reverse(buf);
    if (std::fabs(edit.speed - 1.0f) > 1e-4f) buf = fa::change_speed(buf, edit.speed);
    const bool stretch = std::fabs(edit.tempo - 1.0f) > 1e-4f;
    const bool shift = std::fabs(edit.pitch) > 1e-4f;
    if (stretch && shift) buf = fa::change_tempo_and_pitch(buf, edit.tempo, edit.pitch);
    else if (stretch) buf = fa::time_stretch(buf, edit.tempo);
    else if (shift) buf = fa::change_pitch(buf, edit.pitch);
    const std::uint32_t out_rate = rate ? rate : buf.sample_rate();
    const std::size_t out_channels = channels ? channels : buf.channels();
    if (buf.sample_rate() != out_rate || buf.channels() != out_channels) buf = buf.converted(out_channels, out_rate);
    if (edit.reverb > 0.0f) {
        fa::Reverb reverb;
        reverb.size.set(edit.reverb_size);
        reverb.decay.set(0.4f + edit.reverb_size * 4.0f);
        reverb.mix.set(edit.reverb);
        reverb.prepare(buf.sample_rate(), buf.channels());
        fa::apply(reverb, buf, true);
        const std::size_t limit = buf.frames();
        std::size_t last = limit;
        while (last > 0) {
            const float* f = buf.frame(last - 1);
            float peak = 0.0f;
            for (std::size_t c = 0; c < buf.channels(); ++c) peak = std::max(peak, std::fabs(f[c]));
            if (peak > 1e-4f) break;
            --last;
        }
        if (last < limit) buf.resize(last);
    }
    if (edit.fade_in > 0.0) fa::fade_in(buf, std::min(edit.fade_in, buf.duration()));
    if (edit.fade_out > 0.0) fa::fade_out(buf, std::min(edit.fade_out, buf.duration()));
    return buf;
}

ClipCache::ClipCache(std::size_t budget_bytes) : m_budget(budget_bytes) {}

std::string ClipCache::key_of(const std::string& path, const ClipEdit& edit, std::uint32_t rate, std::size_t channels) {
    return path + "\n" + edit.render_key() + "\n" + std::to_string(rate) + "x" + std::to_string(channels);
}

fa::Sound ClipCache::peek(const std::string& path, const ClipEdit& edit, std::uint32_t rate, std::size_t channels) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_index.find(key_of(path, edit, rate, channels));
    return it == m_index.end() ? nullptr : it->second->sound;
}

fa::Sound ClipCache::get(const std::string& path, const ClipEdit& edit, std::uint32_t rate, std::size_t channels, std::string* error) {
    const std::string key = key_of(path, edit, rate, channels);
    const std::string source_key = "source\n" + path;
    fa::Sound source;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_index.find(key);
        if (it != m_index.end()) {
            m_lru.splice(m_lru.begin(), m_lru, it->second);
            return it->second->sound;
        }
        auto src = m_index.find(source_key);
        if (src != m_index.end()) {
            m_lru.splice(m_lru.begin(), m_lru, src->second);
            source = src->second->sound;
        }
    }
    if (!source) {
        fa::AudioBuffer decoded;
        if (!decode_sound(path, decoded, error)) return nullptr;
        source = fa::make_sound(std::move(decoded));
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_index.count(source_key)) {
            Entry e;
            e.key = source_key;
            e.sound = source;
            e.bytes = source->sample_count() * sizeof(float);
            m_lru.push_front(e);
            m_index[source_key] = m_lru.begin();
            m_bytes += e.bytes;
        }
    }
    fa::Sound rendered = fa::make_sound(render_clip(*source, edit, rate, channels));
    std::lock_guard<std::mutex> lock(m_mutex);
    auto again = m_index.find(key);
    if (again != m_index.end()) return again->second->sound;
    Entry e;
    e.key = key;
    e.sound = rendered;
    e.bytes = rendered->sample_count() * sizeof(float);
    m_lru.push_front(e);
    m_index[key] = m_lru.begin();
    m_bytes += e.bytes;
    trim();
    return rendered;
}

void ClipCache::trim() {
    while (m_bytes > m_budget && m_lru.size() > 1) {
        Entry& last = m_lru.back();
        m_bytes -= std::min(m_bytes, last.bytes);
        m_index.erase(last.key);
        m_lru.pop_back();
    }
}

void ClipCache::forget(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_lru.begin(); it != m_lru.end();) {
        const bool mine = it->key == "source\n" + path || it->key.compare(0, path.size() + 1, path + "\n") == 0;
        if (mine) {
            m_bytes -= std::min(m_bytes, it->bytes);
            m_index.erase(it->key);
            it = m_lru.erase(it);
        } else {
            ++it;
        }
    }
}

void ClipCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lru.clear();
    m_index.clear();
    m_bytes = 0;
}

std::size_t ClipCache::bytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes;
}

std::size_t ClipCache::entries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lru.size();
}

} // namespace rustle
