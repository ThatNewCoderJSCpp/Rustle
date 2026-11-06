#include <rustle/control.hpp>

#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#define RUSTLE_HAVE_SOCKETS 1
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace rustle {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

} // namespace

std::string control_socket_path() {
    const std::string over = env("RUSTLE_CONTROL_SOCKET");
    if (!over.empty()) return over;
#if defined(RUSTLE_HAVE_SOCKETS)
    const std::string runtime = env("XDG_RUNTIME_DIR");
    if (!runtime.empty()) return runtime + "/rustle.sock";
    std::string tmp = env("TMPDIR");
    if (tmp.empty()) tmp = "/tmp";
    return tmp + "/rustle-" + std::to_string(static_cast<long>(getuid())) + ".sock";
#else
    return std::string();
#endif
}

bool control_supported() {
#if defined(RUSTLE_HAVE_SOCKETS)
    return true;
#else
    return false;
#endif
}

ControlServer::ControlServer() = default;

ControlServer::~ControlServer() {
    stop();
}

#if defined(RUSTLE_HAVE_SOCKETS)

namespace {

bool fill_address(const std::string& path, sockaddr_un& addr, std::string* error) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        if (error) *error = "the control socket path is too long: " + path;
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

bool read_line(int fd, std::string& out, int timeout_ms) {
    out.clear();
    char buf[512];
    while (out.size() < 65536) {
        pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, timeout_ms) <= 0) return !out.empty();
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return !out.empty();
        out.append(buf, static_cast<std::size_t>(n));
        const std::size_t nl = out.find('\n');
        if (nl != std::string::npos) {
            out.resize(nl);
            return true;
        }
    }
    return true;
}

void write_all(int fd, const std::string& text) {
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = send(fd, text.data() + done, text.size() - done, flags);
        if (n <= 0) return;
        done += static_cast<std::size_t>(n);
    }
}

} // namespace

bool ControlServer::start(Handler handler, std::string* error) {
    stop();
    m_path = control_socket_path();
    sockaddr_un addr;
    if (!fill_address(m_path, addr, error)) return false;
    const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            close(probe);
            if (error) *error = "another Rustle is already running (it answers on " + m_path + ")";
            return false;
        }
        close(probe);
    }
    unlink(m_path.c_str());
    m_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) {
        if (error) *error = std::string("can't make the control socket: ") + std::strerror(errno);
        return false;
    }
    fcntl(m_fd, F_SETFD, FD_CLOEXEC);
    if (bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(m_fd, 8) != 0) {
        if (error) *error = "can't listen on " + m_path + ": " + std::strerror(errno);
        close(m_fd);
        m_fd = -1;
        return false;
    }
    chmod(m_path.c_str(), 0600);
    m_run.store(true);
    m_thread = std::thread([this, handler] {
        while (m_run.load()) {
            pollfd p;
            p.fd = m_fd;
            p.events = POLLIN;
            p.revents = 0;
            if (poll(&p, 1, 100) <= 0) continue;
            const int client = accept(m_fd, nullptr, nullptr);
            if (client < 0) continue;
            std::string line;
            if (read_line(client, line, 1000) && handler) {
                std::string reply = handler(line);
                if (reply.empty() || reply.back() != '\n') reply += '\n';
                write_all(client, reply);
            }
            close(client);
        }
    });
    return true;
}

void ControlServer::stop() {
    m_run.store(false);
    if (m_thread.joinable()) m_thread.join();
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
        if (!m_path.empty()) unlink(m_path.c_str());
    }
}

bool send_control(const std::string& command, std::string& reply, std::string* error) {
    reply.clear();
    sockaddr_un addr;
    const std::string path = control_socket_path();
    if (!fill_address(path, addr, error)) return false;
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "Rustle isn't running (nothing answers on " + path + ")";
        close(fd);
        return false;
    }
    write_all(fd, command + "\n");
    read_line(fd, reply, 3000);
    close(fd);
    return true;
}

#else

bool ControlServer::start(Handler, std::string* error) {
    if (error) *error = "'rustle ctl' isn't available on Windows; use Rustle's global hotkeys instead";
    return false;
}

void ControlServer::stop() {
    m_run.store(false);
}

bool send_control(const std::string&, std::string& reply, std::string* error) {
    reply.clear();
    if (error) *error = "'rustle ctl' isn't available on Windows; use Rustle's global hotkeys instead";
    return false;
}

#endif

} // namespace rustle
