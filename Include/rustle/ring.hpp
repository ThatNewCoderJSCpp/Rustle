#ifndef RUSTLE_RING_HPP
#define RUSTLE_RING_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace rustle {

class SampleRing {
public:
    void allocate(std::size_t capacity) {
        std::size_t n = 1;
        while (n < capacity) n <<= 1;
        m_data.assign(n, 0.0f);
        m_mask = n - 1;
        m_write.store(0);
        m_read.store(0);
    }

    std::size_t capacity() const noexcept { return m_data.size(); }

    std::size_t available() const noexcept {
        return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
    }

    std::size_t push(const float* data, std::size_t count) noexcept {
        if (m_data.empty()) return 0;
        const std::size_t w = m_write.load(std::memory_order_relaxed);
        const std::size_t r = m_read.load(std::memory_order_acquire);
        const std::size_t room = m_data.size() - (w - r);
        const std::size_t n = std::min(room, count);
        for (std::size_t i = 0; i < n; ++i) m_data[(w + i) & m_mask] = data[i];
        m_write.store(w + n, std::memory_order_release);
        return n;
    }

    std::size_t pull(float* out, std::size_t count) noexcept {
        if (m_data.empty()) return 0;
        const std::size_t r = m_read.load(std::memory_order_relaxed);
        const std::size_t w = m_write.load(std::memory_order_acquire);
        const std::size_t n = std::min(w - r, count);
        for (std::size_t i = 0; i < n; ++i) out[i] = m_data[(r + i) & m_mask];
        m_read.store(r + n, std::memory_order_release);
        return n;
    }

    std::size_t skip(std::size_t count) noexcept {
        const std::size_t r = m_read.load(std::memory_order_relaxed);
        const std::size_t w = m_write.load(std::memory_order_acquire);
        const std::size_t n = std::min(w - r, count);
        m_read.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> m_data;
    std::size_t m_mask = 0;
    std::atomic<std::size_t> m_write{ 0 };
    std::atomic<std::size_t> m_read{ 0 };
};

class VoiceLink {
public:
    void allocate(std::size_t capacity, std::size_t target) {
        m_ring.allocate(capacity);
        m_target = std::max<std::size_t>(1, target);
        m_primed = false;
    }

    void write(const float* mono, std::size_t frames) noexcept {
        m_ring.push(mono, frames);
    }

    std::size_t read(float* mono, std::size_t frames) noexcept {
        const std::size_t have = m_ring.available();
        if (!m_primed) {
            if (have < m_target) {
                std::fill(mono, mono + frames, 0.0f);
                return 0;
            }
            m_primed = true;
        }
        if (have > m_target * 4 + frames) {
            m_ring.skip(have - m_target - frames);
            m_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        const std::size_t got = m_ring.pull(mono, frames);
        if (got < frames) {
            std::fill(mono + got, mono + frames, 0.0f);
            m_primed = false;
            m_underruns.fetch_add(1, std::memory_order_relaxed);
        }
        return got;
    }

    std::size_t buffered() const noexcept { return m_ring.available(); }
    std::size_t underruns() const noexcept { return m_underruns.load(std::memory_order_relaxed); }
    std::size_t drops() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
    SampleRing m_ring;
    std::size_t m_target = 1;
    bool m_primed = false;
    std::atomic<std::size_t> m_underruns{ 0 };
    std::atomic<std::size_t> m_dropped{ 0 };
};

} // namespace rustle

#endif // RUSTLE_RING_HPP
