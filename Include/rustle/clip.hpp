#ifndef RUSTLE_CLIP_HPP
#define RUSTLE_CLIP_HPP

#include <rustle/settings.hpp>

#include <audio.hpp>

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rustle {

bool decode_sound(const std::string& path, fizmo::audio::AudioBuffer& out, std::string* error = nullptr);
double probe_duration(const std::string& path);

fizmo::audio::AudioBuffer render_clip(const fizmo::audio::AudioBuffer& source, const ClipEdit& edit, std::uint32_t rate, std::size_t channels);

class ClipCache {
public:
    explicit ClipCache(std::size_t budget_bytes = 512ull * 1024ull * 1024ull);

    fizmo::audio::Sound get(const std::string& path, const ClipEdit& edit, std::uint32_t rate, std::size_t channels, std::string* error = nullptr);
    fizmo::audio::Sound peek(const std::string& path, const ClipEdit& edit, std::uint32_t rate, std::size_t channels) const;
    void forget(const std::string& path);
    void clear();
    std::size_t bytes() const;
    std::size_t entries() const;

private:
    struct Entry {
        std::string key;
        fizmo::audio::Sound sound;
        std::size_t bytes = 0;
    };
    static std::string key_of(const std::string& path, const ClipEdit& edit, std::uint32_t rate, std::size_t channels);
    void trim();

    mutable std::mutex m_mutex;
    std::list<Entry> m_lru;
    std::map<std::string, std::list<Entry>::iterator> m_index;
    std::size_t m_budget;
    std::size_t m_bytes = 0;
};

} // namespace rustle

#endif // RUSTLE_CLIP_HPP
