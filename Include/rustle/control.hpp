#ifndef RUSTLE_CONTROL_HPP
#define RUSTLE_CONTROL_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace rustle {

std::string control_socket_path();
bool control_supported();

class ControlServer {
public:
    using Handler = std::function<std::string(const std::string& command)>;

    ControlServer();
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    bool start(Handler handler, std::string* error = nullptr);
    void stop();
    bool running() const { return m_run.load(); }
    std::string path() const { return m_path; }

private:
    std::atomic<bool> m_run{ false };
    std::thread m_thread;
    int m_fd = -1;
    std::string m_path;
};

bool send_control(const std::string& command, std::string& reply, std::string* error = nullptr);

} // namespace rustle

#endif // RUSTLE_CONTROL_HPP
