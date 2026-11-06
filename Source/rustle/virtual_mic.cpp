#include <rustle/virtual_mic.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#define RUSTLE_HAVE_PULSE 1
#include <dlfcn.h>
#if defined(__has_include)
#if __has_include(<pulse/introspect.h>)
#define RUSTLE_PULSE_HEADERS 1
#include <pulse/introspect.h>
#endif
#endif
#endif

namespace rustle {

namespace {

constexpr std::uint32_t k_invalid = 0xFFFFFFFFu;

} // namespace

std::string VirtualMic::quote_argument(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'' || c == '\\') out += '\\';
        out += c;
    }
    out += "'";
    return out;
}

std::string VirtualMic::sink_arguments() {
    return std::string("sink_name=") + sink_name + " rate=48000 channels=2 sink_properties=" + quote_argument(std::string("device.description=\"") + sink_description + "\"");
}

std::string VirtualMic::source_arguments() {
    return std::string("master=") + sink_name + ".monitor source_name=" + source_name + " source_properties=" + quote_argument(std::string("device.description=\"") + source_description + "\"");
}

bool VirtualMic::platform_supported() {
#if defined(RUSTLE_HAVE_PULSE)
    return true;
#else
    return false;
#endif
}

#if defined(RUSTLE_HAVE_PULSE)

namespace {

namespace pa {

struct mainloop;
struct mainloop_api;
struct context;
struct operation;
struct proplist;

struct sample_spec {
    int format;
    std::uint32_t rate;
    std::uint8_t channels;
};

struct channel_map {
    std::uint8_t channels;
    int map[32];
};

struct server_info {
    const char* user_name;
    const char* host_name;
    const char* server_version;
    const char* server_name;
    sample_spec spec;
    const char* default_sink_name;
    const char* default_source_name;
};

struct sink_info {
    const char* name;
    std::uint32_t index;
    const char* description;
    sample_spec spec;
    channel_map map;
    std::uint32_t owner_module;
};

struct source_info {
    const char* name;
    std::uint32_t index;
    const char* description;
    sample_spec spec;
    channel_map map;
    std::uint32_t owner_module;
};

struct module_info {
    std::uint32_t index;
    const char* name;
    const char* argument;
    std::uint32_t n_used;
    int auto_unload;
    proplist* props;
};

constexpr int context_ready = 4;
constexpr int context_failed = 5;
constexpr int context_terminated = 6;
constexpr int operation_running = 0;
constexpr int context_noautospawn = 1;

using context_notify_cb = void (*)(context*, void*);
using server_info_cb = void (*)(context*, const server_info*, void*);
using sink_info_cb = void (*)(context*, const sink_info*, int, void*);
using source_info_cb = void (*)(context*, const source_info*, int, void*);
using module_info_cb = void (*)(context*, const module_info*, int, void*);
using index_cb = void (*)(context*, std::uint32_t, void*);
using success_cb = void (*)(context*, int, void*);

} // namespace pa

#if defined(RUSTLE_PULSE_HEADERS)
static_assert(offsetof(pa::server_info, default_sink_name) == offsetof(pa_server_info, default_sink_name), "pa_server_info layout");
static_assert(offsetof(pa::server_info, default_source_name) == offsetof(pa_server_info, default_source_name), "pa_server_info layout");
static_assert(offsetof(pa::sink_info, owner_module) == offsetof(pa_sink_info, owner_module), "pa_sink_info layout");
static_assert(offsetof(pa::source_info, owner_module) == offsetof(pa_source_info, owner_module), "pa_source_info layout");
static_assert(offsetof(pa::module_info, name) == offsetof(pa_module_info, name), "pa_module_info layout");
static_assert(offsetof(pa::module_info, argument) == offsetof(pa_module_info, argument), "pa_module_info layout");
static_assert(PA_CONTEXT_READY == pa::context_ready && PA_CONTEXT_FAILED == pa::context_failed && PA_CONTEXT_TERMINATED == pa::context_terminated, "context state values");
static_assert(PA_OPERATION_RUNNING == pa::operation_running && PA_CONTEXT_NOAUTOSPAWN == pa::context_noautospawn, "pulse constants");
static_assert(PA_INVALID_INDEX == k_invalid, "invalid index");
#endif

class Library {
public:
    ~Library() {
        if (m_handle) dlclose(m_handle);
    }

    bool open(std::initializer_list<const char*> names) {
        for (const char* n : names) {
            m_handle = dlopen(n, RTLD_NOW | RTLD_LOCAL);
            if (m_handle) return true;
        }
        return false;
    }

    template <typename T>
    bool load(T& out, const char* name) const {
        void* p = m_handle ? dlsym(m_handle, name) : nullptr;
        out = reinterpret_cast<T>(p);
        return p != nullptr;
    }

private:
    void* m_handle = nullptr;
};

struct Api {
    Library lib;
    bool loaded = false;
    std::string why;

    pa::mainloop* (*threaded_mainloop_new)() = nullptr;
    int (*threaded_mainloop_start)(pa::mainloop*) = nullptr;
    void (*threaded_mainloop_stop)(pa::mainloop*) = nullptr;
    void (*threaded_mainloop_free)(pa::mainloop*) = nullptr;
    void (*threaded_mainloop_lock)(pa::mainloop*) = nullptr;
    void (*threaded_mainloop_unlock)(pa::mainloop*) = nullptr;
    pa::mainloop_api* (*threaded_mainloop_get_api)(pa::mainloop*) = nullptr;
    pa::context* (*context_new)(pa::mainloop_api*, const char*) = nullptr;
    int (*context_connect)(pa::context*, const char*, int, const void*) = nullptr;
    int (*context_get_state)(const pa::context*) = nullptr;
    void (*context_disconnect)(pa::context*) = nullptr;
    void (*context_unref)(pa::context*) = nullptr;
    int (*context_errno)(const pa::context*) = nullptr;
    pa::operation* (*context_get_server_info)(pa::context*, pa::server_info_cb, void*) = nullptr;
    pa::operation* (*context_get_sink_info_by_name)(pa::context*, const char*, pa::sink_info_cb, void*) = nullptr;
    pa::operation* (*context_get_source_info_by_name)(pa::context*, const char*, pa::source_info_cb, void*) = nullptr;
    pa::operation* (*context_get_module_info_list)(pa::context*, pa::module_info_cb, void*) = nullptr;
    pa::operation* (*context_load_module)(pa::context*, const char*, const char*, pa::index_cb, void*) = nullptr;
    pa::operation* (*context_unload_module)(pa::context*, std::uint32_t, pa::success_cb, void*) = nullptr;
    pa::operation* (*context_set_default_sink)(pa::context*, const char*, pa::success_cb, void*) = nullptr;
    pa::operation* (*context_set_default_source)(pa::context*, const char*, pa::success_cb, void*) = nullptr;
    int (*operation_get_state)(const pa::operation*) = nullptr;
    void (*operation_cancel)(pa::operation*) = nullptr;
    void (*operation_unref)(pa::operation*) = nullptr;
    const char* (*strerror)(int) = nullptr;

    bool load() {
        if (loaded) return true;
        if (!why.empty()) return false;
        if (!lib.open({ "libpulse.so.0", "libpulse.so" })) {
            why = "libpulse isn't installed, so Rustle can't make a virtual microphone. Install PulseAudio's client library (the libpulse0 or pulseaudio-libs package; PipeWire systems use it too through pipewire-pulse)";
            return false;
        }
        bool ok = true;
        ok &= lib.load(threaded_mainloop_new, "pa_threaded_mainloop_new");
        ok &= lib.load(threaded_mainloop_start, "pa_threaded_mainloop_start");
        ok &= lib.load(threaded_mainloop_stop, "pa_threaded_mainloop_stop");
        ok &= lib.load(threaded_mainloop_free, "pa_threaded_mainloop_free");
        ok &= lib.load(threaded_mainloop_lock, "pa_threaded_mainloop_lock");
        ok &= lib.load(threaded_mainloop_unlock, "pa_threaded_mainloop_unlock");
        ok &= lib.load(threaded_mainloop_get_api, "pa_threaded_mainloop_get_api");
        ok &= lib.load(context_new, "pa_context_new");
        ok &= lib.load(context_connect, "pa_context_connect");
        ok &= lib.load(context_get_state, "pa_context_get_state");
        ok &= lib.load(context_disconnect, "pa_context_disconnect");
        ok &= lib.load(context_unref, "pa_context_unref");
        ok &= lib.load(context_errno, "pa_context_errno");
        ok &= lib.load(context_get_server_info, "pa_context_get_server_info");
        ok &= lib.load(context_get_sink_info_by_name, "pa_context_get_sink_info_by_name");
        ok &= lib.load(context_get_source_info_by_name, "pa_context_get_source_info_by_name");
        ok &= lib.load(context_get_module_info_list, "pa_context_get_module_info_list");
        ok &= lib.load(context_load_module, "pa_context_load_module");
        ok &= lib.load(context_unload_module, "pa_context_unload_module");
        ok &= lib.load(context_set_default_sink, "pa_context_set_default_sink");
        ok &= lib.load(context_set_default_source, "pa_context_set_default_source");
        ok &= lib.load(operation_get_state, "pa_operation_get_state");
        ok &= lib.load(operation_cancel, "pa_operation_cancel");
        ok &= lib.load(operation_unref, "pa_operation_unref");
        ok &= lib.load(strerror, "pa_strerror");
        if (!ok) {
            why = "the installed libpulse is missing functions Rustle needs";
            return false;
        }
        loaded = true;
        return true;
    }

    std::string error_text(int code) const {
        const char* s = strerror ? strerror(code) : nullptr;
        return s ? std::string(s) : "PulseAudio error " + std::to_string(code);
    }
};

struct Found {
    bool done = false;
    bool exists = false;
    std::uint32_t owner = k_invalid;
    std::string text_a;
    std::string text_b;
    std::string text_c;
};

struct ModuleRow {
    std::uint32_t index = k_invalid;
    std::string name;
    std::string argument;
};

struct ModuleList {
    bool done = false;
    std::vector<ModuleRow> rows;
};

struct IndexResult {
    bool done = false;
    std::uint32_t index = k_invalid;
};

struct SuccessResult {
    bool done = false;
    bool ok = false;
};

void on_server(pa::context*, const pa::server_info* info, void* user) {
    Found* f = static_cast<Found*>(user);
    if (info) {
        f->exists = true;
        f->text_a = info->default_sink_name ? info->default_sink_name : "";
        f->text_b = info->default_source_name ? info->default_source_name : "";
        f->text_c = std::string(info->server_name ? info->server_name : "sound server") + (info->server_version ? std::string(" ") + info->server_version : std::string());
    }
    f->done = true;
}

void on_sink(pa::context*, const pa::sink_info* info, int eol, void* user) {
    Found* f = static_cast<Found*>(user);
    if (eol || !info) {
        f->done = true;
        return;
    }
    f->exists = true;
    f->owner = info->owner_module;
    f->text_a = info->description ? info->description : "";
}

void on_source(pa::context*, const pa::source_info* info, int eol, void* user) {
    Found* f = static_cast<Found*>(user);
    if (eol || !info) {
        f->done = true;
        return;
    }
    f->exists = true;
    f->owner = info->owner_module;
    f->text_a = info->description ? info->description : "";
}

void on_module(pa::context*, const pa::module_info* info, int eol, void* user) {
    ModuleList* l = static_cast<ModuleList*>(user);
    if (eol || !info) {
        l->done = true;
        return;
    }
    ModuleRow r;
    r.index = info->index;
    r.name = info->name ? info->name : "";
    r.argument = info->argument ? info->argument : "";
    l->rows.push_back(r);
}

void on_index(pa::context*, std::uint32_t index, void* user) {
    IndexResult* r = static_cast<IndexResult*>(user);
    r->index = index;
    r->done = true;
}

void on_success(pa::context*, int success, void* user) {
    SuccessResult* r = static_cast<SuccessResult*>(user);
    r->ok = success != 0;
    r->done = true;
}

class Connection {
public:
    explicit Connection(Api& api) : m_api(api) {}

    ~Connection() {
        close();
    }

    bool open(std::string& error) {
        m_loop = m_api.threaded_mainloop_new();
        if (!m_loop) {
            error = "can't create a PulseAudio main loop";
            return false;
        }
        m_ctx = m_api.context_new(m_api.threaded_mainloop_get_api(m_loop), "Rustle");
        if (!m_ctx) {
            error = "can't create a PulseAudio context";
            return false;
        }
        if (m_api.context_connect(m_ctx, nullptr, pa::context_noautospawn, nullptr) < 0) {
            error = "can't reach the sound server (" + m_api.error_text(m_api.context_errno(m_ctx)) + "). Is PulseAudio or PipeWire (with pipewire-pulse) running?";
            return false;
        }
        if (m_api.threaded_mainloop_start(m_loop) < 0) {
            error = "can't start the PulseAudio main loop";
            return false;
        }
        m_started = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (true) {
            m_api.threaded_mainloop_lock(m_loop);
            const int s = m_api.context_get_state(m_ctx);
            m_api.threaded_mainloop_unlock(m_loop);
            if (s == pa::context_ready) return true;
            if (s == pa::context_failed || s == pa::context_terminated) {
                m_api.threaded_mainloop_lock(m_loop);
                const int code = m_api.context_errno(m_ctx);
                m_api.threaded_mainloop_unlock(m_loop);
                error = "can't reach the sound server (" + m_api.error_text(code) + "). Is PulseAudio or PipeWire (with pipewire-pulse) running?";
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                error = "the sound server didn't answer";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    template <typename Start, typename Done>
    bool run(Start start, Done done, std::string& error) {
        m_api.threaded_mainloop_lock(m_loop);
        pa::operation* op = start();
        m_api.threaded_mainloop_unlock(m_loop);
        if (!op) {
            m_api.threaded_mainloop_lock(m_loop);
            error = m_api.error_text(m_api.context_errno(m_ctx));
            m_api.threaded_mainloop_unlock(m_loop);
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool ok = true;
        while (true) {
            m_api.threaded_mainloop_lock(m_loop);
            const bool finished = done() || m_api.operation_get_state(op) != pa::operation_running;
            const int s = m_api.context_get_state(m_ctx);
            m_api.threaded_mainloop_unlock(m_loop);
            if (finished) break;
            if (s == pa::context_failed || s == pa::context_terminated) {
                error = "the sound server connection dropped";
                ok = false;
                break;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                error = "the sound server didn't answer in time";
                ok = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_api.threaded_mainloop_lock(m_loop);
        if (!ok) m_api.operation_cancel(op);
        m_api.operation_unref(op);
        m_api.threaded_mainloop_unlock(m_loop);
        return ok;
    }

    std::string last_error() {
        m_api.threaded_mainloop_lock(m_loop);
        const int code = m_api.context_errno(m_ctx);
        m_api.threaded_mainloop_unlock(m_loop);
        return m_api.error_text(code);
    }

    pa::context* ctx() const { return m_ctx; }

    void close() {
        if (m_loop && m_ctx && m_started) {
            m_api.threaded_mainloop_lock(m_loop);
            m_api.context_disconnect(m_ctx);
            m_api.threaded_mainloop_unlock(m_loop);
        }
        if (m_loop && m_started) m_api.threaded_mainloop_stop(m_loop);
        if (m_ctx) {
            if (!m_started) m_api.context_disconnect(m_ctx);
            m_api.context_unref(m_ctx);
        }
        if (m_loop) m_api.threaded_mainloop_free(m_loop);
        m_ctx = nullptr;
        m_loop = nullptr;
        m_started = false;
    }

private:
    Api& m_api;
    pa::mainloop* m_loop = nullptr;
    pa::context* m_ctx = nullptr;
    bool m_started = false;
};

bool server_info(Api& api, Connection& c, Found& out, std::string& error) {
    out = Found();
    return c.run([&] { return api.context_get_server_info(c.ctx(), on_server, &out); }, [&] { return out.done; }, error);
}

bool find_sink(Api& api, Connection& c, const char* name, Found& out, std::string& error) {
    out = Found();
    return c.run([&] { return api.context_get_sink_info_by_name(c.ctx(), name, on_sink, &out); }, [&] { return out.done; }, error);
}

bool find_source(Api& api, Connection& c, const char* name, Found& out, std::string& error) {
    out = Found();
    return c.run([&] { return api.context_get_source_info_by_name(c.ctx(), name, on_source, &out); }, [&] { return out.done; }, error);
}

bool modules(Api& api, Connection& c, ModuleList& out, std::string& error) {
    out = ModuleList();
    return c.run([&] { return api.context_get_module_info_list(c.ctx(), on_module, &out); }, [&] { return out.done; }, error);
}

std::uint32_t load_module(Api& api, Connection& c, const char* name, const std::string& args, std::string& error) {
    IndexResult r;
    if (!c.run([&] { return api.context_load_module(c.ctx(), name, args.c_str(), on_index, &r); }, [&] { return r.done; }, error)) return k_invalid;
    if (r.index == k_invalid) error = "the sound server refused to load " + std::string(name) + " (" + c.last_error() + ")";
    return r.index;
}

bool unload_module(Api& api, Connection& c, std::uint32_t index, std::string& error) {
    SuccessResult r;
    if (!c.run([&] { return api.context_unload_module(c.ctx(), index, on_success, &r); }, [&] { return r.done; }, error)) return false;
    if (!r.ok) error = "the sound server wouldn't unload module " + std::to_string(index);
    return r.ok;
}

bool set_default(Api& api, Connection& c, bool sink, const std::string& name, std::string& error) {
    SuccessResult r;
    auto start = [&] {
        return sink ? api.context_set_default_sink(c.ctx(), name.c_str(), on_success, &r) : api.context_set_default_source(c.ctx(), name.c_str(), on_success, &r);
    };
    if (!c.run(start, [&] { return r.done; }, error)) return false;
    return r.ok;
}

std::string module_name_of(const ModuleList& list, std::uint32_t index) {
    for (const ModuleRow& r : list.rows) {
        if (r.index == index) return r.name;
    }
    return std::string();
}

} // namespace

struct VirtualMic::Impl {
    Api api;
};

#else

struct VirtualMic::Impl {};

#endif

VirtualMic::VirtualMic() : m_impl(std::make_unique<Impl>()) {}

VirtualMic::~VirtualMic() = default;

bool VirtualMic::owns_modules() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sink_module != k_invalid || m_source_module != k_invalid;
}

void VirtualMic::keep() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink_module = k_invalid;
    m_source_module = k_invalid;
}

#if defined(RUSTLE_HAVE_PULSE)

bool VirtualMic::library_available(std::string* why) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl->api.load()) return true;
    if (why) *why = m_impl->api.why;
    return false;
}

bool VirtualMic::setup(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Api& api = m_impl->api;
    if (!api.load()) {
        if (error) *error = api.why;
        return false;
    }
    Connection c(api);
    std::string why;
    if (!c.open(why)) {
        if (error) *error = why;
        return false;
    }
    Found before;
    server_info(api, c, before, why);
    ModuleList list;
    modules(api, c, list, why);

    Found sink;
    if (!find_sink(api, c, sink_name, sink, why)) {
        if (error) *error = why;
        return false;
    }
    if (sink.exists) {
        if (m_sink_module == k_invalid && sink.owner != k_invalid && module_name_of(list, sink.owner) == "module-null-sink") m_sink_module = sink.owner;
    } else {
        const std::uint32_t idx = load_module(api, c, "module-null-sink", sink_arguments(), why);
        if (idx == k_invalid) {
            if (error) *error = "couldn't make the Rustle Output device: " + why;
            return false;
        }
        m_sink_module = idx;
    }

    Found source;
    if (!find_source(api, c, source_name, source, why)) {
        if (error) *error = why;
        return false;
    }
    if (source.exists) {
        if (m_source_module == k_invalid && source.owner != k_invalid && module_name_of(list, source.owner) == "module-remap-source") m_source_module = source.owner;
    } else {
        const std::uint32_t idx = load_module(api, c, "module-remap-source", source_arguments(), why);
        if (idx == k_invalid) {
            if (error) *error = "made Rustle Output but couldn't make the Rustle Mic device: " + why;
            return false;
        }
        m_source_module = idx;
    }

    Found after;
    if (before.exists && server_info(api, c, after, why) && after.exists) {
        std::string ignored;
        if (after.text_a == sink_name && before.text_a != sink_name && !before.text_a.empty()) set_default(api, c, true, before.text_a, ignored);
        if ((after.text_b == source_name || after.text_b == std::string(sink_name) + ".monitor") && before.text_b != after.text_b && !before.text_b.empty()) set_default(api, c, false, before.text_b, ignored);
    }
    return true;
}

bool VirtualMic::teardown(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sink_module == k_invalid && m_source_module == k_invalid) return true;
    Api& api = m_impl->api;
    if (!api.load()) {
        if (error) *error = api.why;
        return false;
    }
    Connection c(api);
    std::string why;
    if (!c.open(why)) {
        if (error) *error = why;
        return false;
    }
    ModuleList list;
    modules(api, c, list, why);
    bool ok = true;
    std::string problems;
    if (m_source_module != k_invalid) {
        if (module_name_of(list, m_source_module) == "module-remap-source") {
            std::string e;
            if (!unload_module(api, c, m_source_module, e)) {
                ok = false;
                problems += e;
            }
        }
        m_source_module = k_invalid;
    }
    if (m_sink_module != k_invalid) {
        if (module_name_of(list, m_sink_module) == "module-null-sink") {
            std::string e;
            if (!unload_module(api, c, m_sink_module, e)) {
                ok = false;
                problems += (problems.empty() ? "" : "; ") + e;
            }
        }
        m_sink_module = k_invalid;
    }
    if (!ok && error) *error = problems;
    return ok;
}

VirtualMicState VirtualMic::state() {
    std::lock_guard<std::mutex> lock(m_mutex);
    VirtualMicState s;
    s.supported = true;
    Api& api = m_impl->api;
    if (!api.load()) {
        s.error = api.why;
        return s;
    }
    s.library = true;
    Connection c(api);
    std::string why;
    if (!c.open(why)) {
        s.error = why;
        return s;
    }
    s.server = true;
    Found info;
    if (server_info(api, c, info, why) && info.exists) {
        s.server_name = info.text_c;
        s.default_sink = info.text_a;
        s.default_source = info.text_b;
    }
    Found f;
    if (find_sink(api, c, sink_name, f, why)) s.sink = f.exists;
    if (find_source(api, c, source_name, f, why)) s.source = f.exists;
    s.owned_sink = m_sink_module != k_invalid;
    s.owned_source = m_source_module != k_invalid;
    return s;
}

#else

bool VirtualMic::library_available(std::string* why) {
    if (why) *why = "the Rustle Mic virtual microphone needs PulseAudio or PipeWire, so it only exists on Linux";
    return false;
}

bool VirtualMic::setup(std::string* error) {
    if (error) *error = "the Rustle Mic virtual microphone needs PulseAudio or PipeWire, so it only exists on Linux. Pick a virtual audio cable (like VB-CABLE on Windows or BlackHole on macOS) as the 'send to' device instead";
    return false;
}

bool VirtualMic::teardown(std::string*) {
    return true;
}

VirtualMicState VirtualMic::state() {
    VirtualMicState s;
    s.error = "the Rustle Mic virtual microphone needs PulseAudio or PipeWire, so it only exists on Linux";
    return s;
}

#endif

} // namespace rustle
