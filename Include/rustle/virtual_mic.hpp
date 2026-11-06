#ifndef RUSTLE_VIRTUAL_MIC_HPP
#define RUSTLE_VIRTUAL_MIC_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace rustle {

struct VirtualMicState {
    bool supported = false;
    bool library = false;
    bool server = false;
    bool sink = false;
    bool source = false;
    bool owned_sink = false;
    bool owned_source = false;
    std::string server_name;
    std::string default_sink;
    std::string default_source;
    std::string error;

    bool ready() const { return sink && source; }
};

class VirtualMic {
public:
    static constexpr const char* sink_name = "rustle_sink";
    static constexpr const char* source_name = "rustle_mic";
    static constexpr const char* sink_description = "Rustle Output";
    static constexpr const char* source_description = "Rustle Mic";

    VirtualMic();
    ~VirtualMic();
    VirtualMic(const VirtualMic&) = delete;
    VirtualMic& operator=(const VirtualMic&) = delete;

    static std::string sink_arguments();
    static std::string source_arguments();
    static std::string quote_argument(const std::string& value);
    static bool platform_supported();

    bool library_available(std::string* why = nullptr);
    bool setup(std::string* error = nullptr);
    bool teardown(std::string* error = nullptr);
    void keep();
    VirtualMicState state();
    bool owns_modules() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex;
    std::uint32_t m_sink_module = 0xFFFFFFFFu;
    std::uint32_t m_source_module = 0xFFFFFFFFu;
};

} // namespace rustle

#endif // RUSTLE_VIRTUAL_MIC_HPP
