#include <rustle/session.hpp>
#include <rustle/ring.hpp>
#include <rustle/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define RUSTLE_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define RUSTLE_SANITIZED 1
#endif

namespace fs = std::filesystem;
namespace fa = fizmo::audio;

namespace {

std::atomic<long> g_allocations{ 0 };
std::atomic<bool> g_counting{ false };

} // namespace

#if !defined(RUSTLE_SANITIZED)

void* operator new(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (size == 0) size = 1;
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

#endif

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_MSG(c, msg) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s  (%s)\n", __FILE__, __LINE__, #c, std::string(msg).c_str()); } } while (0)

namespace {

constexpr double k_pi = 3.14159265358979323846;

fs::path g_root;

fs::path temp_root() {
    if (g_root.empty()) {
        long pid = 0;
#if !defined(_WIN32)
        pid = static_cast<long>(getpid());
#endif
        g_root = fs::temp_directory_path() / ("rustle-tests-" + std::to_string(pid) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000));
        fs::create_directories(g_root);
    }
    return g_root;
}

fa::AudioBuffer sine(double freq, double seconds, std::uint32_t rate = 48000, std::size_t channels = 1, float amp = 0.5f) {
    const std::size_t frames = static_cast<std::size_t>(seconds * rate);
    fa::AudioBuffer b(channels, rate, frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const float v = amp * static_cast<float>(std::sin(2.0 * k_pi * freq * static_cast<double>(i) / rate));
        for (std::size_t c = 0; c < channels; ++c) b.at(i, c) = v;
    }
    return b;
}

void write_wav(const fs::path& p, const fa::AudioBuffer& b) {
    fs::create_directories(p.parent_path());
    fa::SaveOptions o;
    o.wav_format = fa::SampleFormat::f32;
    std::string why;
    if (!fa::save_audio(p.string(), b, o, &why)) std::printf("could not write %s: %s\n", p.string().c_str(), why.c_str());
}

double tone_level(const float* data, std::size_t frames, std::size_t stride, double freq, double rate) {
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double a = 2.0 * k_pi * freq * static_cast<double>(i) / rate;
        re += data[i * stride] * std::cos(a);
        im += data[i * stride] * std::sin(a);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(std::max<std::size_t>(1, frames));
}

double dominant(const fa::AudioBuffer& b, double from, double to, double step) {
    const std::size_t start = b.frames() / 4;
    const std::size_t count = std::min<std::size_t>(b.frames() / 2, 16384);
    double best = 0.0, at = 0.0;
    for (double f = from; f <= to; f += step) {
        const double l = tone_level(b.data() + start * b.channels(), count, b.channels(), f, b.sample_rate());
        if (l > best) {
            best = l;
            at = f;
        }
    }
    return at;
}

double rms(const float* data, std::size_t count) {
    double s = 0.0;
    for (std::size_t i = 0; i < count; ++i) s += static_cast<double>(data[i]) * data[i];
    return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, count)));
}

double rms_diff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from) {
    double s = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < std::min(a.size(), b.size()); ++i, ++n) s += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
    return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, n)));
}

std::vector<float> test_voice(std::size_t frames, std::uint32_t rate = 48000) {
    std::vector<float> out(frames);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double env_db = -66.0 + 60.0 * (0.5 + 0.5 * std::sin(2.0 * k_pi * 0.7 * t));
        const double env = std::pow(10.0, env_db / 20.0);
        const double voice = 0.6 * std::sin(2.0 * k_pi * 180.0 * t) + 0.3 * std::sin(2.0 * k_pi * 360.0 * t) + 0.15 * std::sin(2.0 * k_pi * 2700.0 * t) + 0.08 * std::sin(2.0 * k_pi * 9000.0 * t);
        out[i] = static_cast<float>(env * voice) + 0.0004f * noise(rng);
    }
    return out;
}

std::vector<float> run_chain(rustle::VoiceChain& chain, std::vector<float> input, std::size_t block = 256) {
    chain.prepare(48000, 1, 1024);
    for (std::size_t i = 0; i < input.size(); i += block) chain.process(input.data() + i, std::min(block, input.size() - i));
    return input;
}

void test_settings() {
    const fs::path dir = temp_root() / "settings";
    fs::create_directories(dir);
    rustle::Settings s;
    s.folders = { "/music/a", "/music/b" };
    s.favorites = { "/music/a/x.wav" };
    rustle::ClipEdit e;
    e.speed = 1.5f;
    e.reverse = true;
    e.trim_start = 0.25;
    e.loop = true;
    e.volume = 0.5f;
    s.edits["/music/a/x.wav"] = e;
    s.keybinds["/music/a/x.wav"] = "Ctrl+1";
    s.play_counts["/music/a/x.wav"] = 3;
    s.key_push_to_talk = "F10";
    s.global_hotkeys = true;
    s.auto_mode = false;
    s.input_device = "alsa_input.usb";
    s.monitor_device = "none";
    s.mic_volume = 1.25f;
    s.overlap = false;
    s.sort = "played";
    s.view = "list";
    s.voice_preset = "Robot";
    rustle::VoiceChain chain;
    chain.set_value("pitch.semitones", 5.0f);
    s.voice_json = chain.save_json("x");
    std::string why;
    CHECK(rustle::save_settings(dir / "settings.json", s, &why));
    rustle::Settings back;
    const rustle::LoadResult r = rustle::load_settings(dir / "settings.json", back);
    CHECK(r.ok && r.existed && !r.recovered);
    CHECK(back.folders == s.folders);
    CHECK(back.favorites == s.favorites);
    CHECK(back.edits.size() == 1 && back.edits["/music/a/x.wav"] == e);
    CHECK(back.keybinds["/music/a/x.wav"] == "Ctrl+1");
    CHECK(back.play_counts["/music/a/x.wav"] == 3);
    CHECK(back.key_push_to_talk == "F10" && back.global_hotkeys && !back.auto_mode);
    CHECK(back.input_device == "alsa_input.usb" && back.monitor_device == "none");
    CHECK(std::fabs(back.mic_volume - 1.25f) < 1e-6f);
    CHECK(!back.overlap && back.sort == "played" && back.view == "list" && back.voice_preset == "Robot");
    rustle::VoiceChain restored;
    CHECK(restored.load_json(back.voice_json, &why));
    CHECK(restored.enabled("pitch") && std::fabs(restored.value("pitch.semitones") - 5.0f) < 1e-6f);

    rustle::Settings fresh;
    const rustle::LoadResult missing = rustle::load_settings(dir / "nope.json", fresh);
    CHECK(missing.ok && !missing.existed);
    CHECK(fresh.folders.empty() && fresh.auto_mode && fresh.key_stop_all == "F8");

    {
        std::ofstream bad(dir / "broken.json");
        bad << "{ \"folders\": [\"/x\", ";
    }
    rustle::Settings recovered;
    recovered.folders = { "leftover" };
    const rustle::LoadResult rb = rustle::load_settings(dir / "broken.json", recovered);
    CHECK(!rb.ok && rb.recovered && !rb.message.empty());
    CHECK(recovered.folders.empty());
    CHECK(!fs::exists(dir / "broken.json"));
    bool aside = false;
    for (const auto& entry : fs::directory_iterator(dir)) aside = aside || entry.path().filename().string().find("broken.json.broken-") == 0;
    CHECK(aside);

    {
        std::ofstream odd(dir / "odd.json");
        odd << "{ \"folders\": 7, \"favorites\": [1, \"/ok.wav\"], \"mic_volume\": 99, \"sort\": \"sideways\", \"edits\": { \"/a.wav\": { \"speed\": 100, \"trim_start\": 5, \"trim_end\": 2 } }, \"view\": 3, \"voice\": \"nope\" }";
    }
    rustle::Settings odd;
    const rustle::LoadResult ro = rustle::load_settings(dir / "odd.json", odd);
    CHECK(ro.ok);
    CHECK(odd.folders.empty());
    CHECK(odd.favorites.size() == 1 && odd.favorites.count("/ok.wav") == 1);
    CHECK(odd.mic_volume == 4.0f);
    CHECK(odd.sort == "name" && odd.view == "tiles");
    CHECK(odd.edits["/a.wav"].speed == 4.0f && odd.edits["/a.wav"].trim_end == 0.0);
    CHECK(odd.voice_json.empty());

    {
        std::ofstream arr(dir / "array.json");
        arr << "[1, 2, 3]";
    }
    rustle::Settings from_array;
    CHECK(rustle::load_settings(dir / "array.json", from_array).recovered);

    rustle::ClipEdit plain;
    CHECK(plain.is_default() && !plain.changes_audio());
    rustle::ClipEdit loud;
    loud.volume = 1.5f;
    loud.loop = true;
    CHECK(!loud.is_default() && !loud.changes_audio() && loud.render_key() == "plain");
    rustle::ClipEdit fast;
    fast.speed = 2.0f;
    CHECK(fast.changes_audio() && fast.render_key() != plain.render_key());
}

void test_library() {
    const fs::path root = temp_root() / "library" / "Sounds";
    const fa::AudioBuffer half = sine(440.0, 0.5);
    write_wav(root / "air_horn.wav", half);
    write_wav(root / "Memes" / "bruh.wav", sine(220.0, 1.0));
    write_wav(root / "Memes" / "Deep" / "vine boom.wav", sine(110.0, 2.0));
    write_wav(root / ".hidden" / "secret.wav", half);
    fs::create_directories(root / "Empty" / "AlsoEmpty");
    {
        std::ofstream(root / "notes.txt") << "not a sound";
        std::ofstream(root / "Memes" / "clip.mkv") << "x";
    }
    rustle::Library lib;
    lib.set_roots({ root.string(), root.string() + "/", (root / "Memes").string() + "/../" });
    CHECK(lib.roots().size() == 1);
    lib.rescan();
    CHECK(lib.sound_count() == 4);
    CHECK(lib.revision() == 1);
    const std::vector<rustle::FolderEntry> top = lib.subfolders("");
    CHECK(top.size() == 1 && top[0].path == rustle::normalize_folder(root.string()) && top[0].sounds == 4);
    const std::vector<rustle::FolderEntry> inside = lib.subfolders(root.string());
    CHECK(inside.size() == 1 && inside[0].name == "Memes" && inside[0].sounds == 3 && inside[0].folders == 1);
    const std::vector<rustle::SoundEntry> here = lib.sounds_in(root.string());
    CHECK(here.size() == 1 && here[0].name == "air horn");
    const std::vector<rustle::SoundEntry> memes = lib.sounds_in((root / "Memes").string());
    CHECK(memes.size() == 2);
    bool video = false;
    for (const rustle::SoundEntry& s : memes) video = video || (s.video && s.extension == "mkv");
    CHECK(video);
    const std::string deep = (root / "Memes" / "Deep").string();
    const std::vector<rustle::Crumb> crumbs = lib.breadcrumbs(deep);
    CHECK(crumbs.size() == 4 && crumbs[0].label == "Library" && crumbs[1].label == "Sounds" && crumbs[3].label == "Deep" && crumbs[3].path == deep);
    CHECK(lib.parent_of(deep) == (root / "Memes").string());
    CHECK(lib.parent_of(root.string()).empty());
    CHECK(lib.has_folder(deep) && !lib.has_folder((root / "Nowhere").string()));
    const std::vector<rustle::SoundEntry> found = lib.search("deep boom");
    CHECK(found.size() == 1 && found[0].name == "vine boom");
    const std::vector<rustle::SoundEntry> b = lib.search("B");
    CHECK(b.size() >= 2 && b[0].name == "bruh");
    CHECK(lib.search("zzz").empty() && lib.search("   ").empty());
    rustle::SoundEntry entry;
    CHECK(lib.find((root / "air_horn.wav").string(), entry) && entry.root == rustle::normalize_folder(root.string()));

    std::vector<rustle::SoundEntry> all = lib.all_sounds();
    for (const rustle::SoundEntry& s : all) lib.set_duration(s.path, rustle::probe_duration(s.path));
    CHECK(std::fabs(lib.duration((root / "Memes" / "Deep" / "vine boom.wav").string()) - 2.0) < 0.01);
    CHECK(lib.missing_durations(10).size() == 0);
    std::map<std::string, int> plays{ { (root / "Memes" / "bruh.wav").string(), 5 } };
    std::set<std::string> favs{ (root / "Memes" / "Deep" / "vine boom.wav").string() };
    CHECK(lib.duration((root / "Memes" / "clip.mkv").string()) < -1.5);
    lib.sort(all, rustle::SortOrder::duration, plays, favs, false);
    CHECK(all.size() == 4 && all[0].name == "air horn" && all[1].name == "bruh" && all[2].name == "vine boom" && all[3].name == "clip");
    lib.sort(all, rustle::SortOrder::played, plays, favs, false);
    CHECK(all.front().name == "bruh");
    lib.sort(all, rustle::SortOrder::name, plays, favs, true);
    CHECK(all.front().name == "vine boom");
    lib.sort(all, rustle::SortOrder::name, plays, favs, false);
    CHECK(all.front().name == "air horn");

    CHECK(!lib.changed_on_disk());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write_wav(root / "Empty" / "AlsoEmpty" / "new.wav", half);
    CHECK(lib.changed_on_disk());
    lib.rescan();
    CHECK(lib.sound_count() == 5 && lib.revision() == 2);
    CHECK(lib.subfolders(root.string()).size() == 2);
    CHECK(lib.duration((root / "air_horn.wav").string()) > 0.0);

    CHECK(rustle::is_sound_extension("MP3") && rustle::is_sound_extension("opus") && rustle::is_sound_extension("m4a") && rustle::is_sound_extension("webm"));
    CHECK(!rustle::is_sound_extension("txt") && !rustle::is_sound_extension(""));
    CHECK(rustle::display_name("big_bad_wolf.final.mp3") == "big bad wolf.final");
    CHECK(rustle::path_inside("/a/b/c", "/a/b") && !rustle::path_inside("/a/bc", "/a/b") && rustle::path_inside("/a/b", "/a/b"));
    CHECK(rustle::sort_order_from("newest") == rustle::SortOrder::newest && rustle::sort_order_from("x") == rustle::SortOrder::name);
}

void test_files() {
    const fs::path dir = temp_root() / "files";
    fs::create_directories(dir / "Zeta");
    fs::create_directories(dir / "alpha");
    fs::create_directories(dir / ".config");
    write_wav(dir / "b.wav", sine(440.0, 0.1));
    {
        std::ofstream(dir / "a.txt") << "hello";
        std::ofstream(dir / ".hidden.wav") << "x";
    }
    std::vector<rustle::FileItem> items;
    std::string why;
    CHECK(rustle::list_folder(dir.string(), false, items, &why));
    CHECK(items.size() == 4);
    CHECK(items.size() == 4 && items[0].folder && items[0].name == "alpha" && items[1].name == "Zeta" && items[2].name == "a.txt" && items[3].name == "b.wav");
    CHECK(items.size() == 4 && items[3].sound && !items[2].sound && items[2].size == 5);
    CHECK(rustle::list_folder(dir.string(), true, items, &why) && items.size() == 6);
    CHECK(!rustle::list_folder((dir / "missing").string(), false, items, &why) && !why.empty());
    CHECK(rustle::parent_folder((dir / "Zeta").string()) == rustle::normalize_folder(dir.string()));
#if !defined(_WIN32)
    CHECK(rustle::parent_folder("/") == "/");
    const auto parts = rustle::path_parts("/home/me/Music");
    CHECK(parts.size() == 4 && parts[0].first == "/" && parts[3].first == "Music" && parts[3].second == "/home/me/Music" && parts[1].second == "/home");
#endif

    const std::string mounts =
        "/dev/nvme0n1p2 / ext4 rw 0 0\n"
        "/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n"
        "proc /proc proc rw 0 0\n"
        "/dev/sdb1 /media/me/USB\\040Stick vfat rw 0 0\n"
        "/dev/sdc1 /run/media/me/Backup ext4 rw 0 0\n"
        "/dev/loop3 /snap/core/1 squashfs ro 0 0\n"
        "server:/share /mnt/nas nfs4 rw 0 0\n"
        "/dev/sda1 /data ext4 rw 0 0\n"
        "tmpfs /run/user/1000 tmpfs rw 0 0\n"
        "/dev/nvme0n1p3 /home ext4 rw 0 0\n";
    const std::vector<rustle::Place> drives = rustle::parse_mounts(mounts, "/home/me");
    std::vector<std::string> paths;
    for (const rustle::Place& p : drives) paths.push_back(p.path);
    CHECK(paths.size() == 4);
    CHECK(std::find(paths.begin(), paths.end(), "/media/me/USB Stick") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/run/media/me/Backup") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/mnt/nas") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/data") != paths.end());
    for (const rustle::Place& p : drives) {
        if (p.path == "/mnt/nas") CHECK(p.kind == "network");
        if (p.path == "/media/me/USB Stick") CHECK(p.label == "USB Stick");
    }
    const std::vector<rustle::Place> user = rustle::parse_user_dirs("# comment\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_MUSIC_DIR=\"$HOME/Musik\"\nXDG_TEMPLATES_DIR=\"$HOME/\"\n", "/home/me");
    CHECK(user.size() == 2 && user[1].label == "Music" && user[1].path == "/home/me/Musik");

    const fs::path dest = dir / "Zeta";
    std::vector<std::string> copied;
    CHECK(rustle::copy_into({ (dir / "b.wav").string() }, dest.string(), copied, &why) && copied.size() == 1);
    CHECK(rustle::copy_into({ (dir / "b.wav").string() }, dest.string(), copied, &why) && copied.size() == 1 && fs::path(copied[0]).filename() == "b (2).wav");
    CHECK(!rustle::copy_into({ (dir / "nope.wav").string() }, dest.string(), copied, &why) && copied.empty());
    CHECK(!rustle::copy_into({ (dest / "b.wav").string() }, dest.string(), copied, &why));
    std::string made;
    CHECK(rustle::make_folder(dir.string(), "New Sounds", made, &why) && fs::is_directory(made));
    CHECK(!rustle::make_folder(dir.string(), "New Sounds", made, &why));
    CHECK(!rustle::make_folder(dir.string(), "../escape", made, &why));
    CHECK(!rustle::make_folder(dir.string(), "  ", made, &why));
    CHECK(rustle::format_size(512) == "512 B" && rustle::format_size(1536) == "1.5 KB" && rustle::format_size(5ull * 1024 * 1024) == "5.0 MB");
    CHECK(!rustle::places().empty());
}

void test_clip_render() {
    const fa::AudioBuffer src = sine(440.0, 1.0);
    rustle::ClipEdit plain;
    const fa::AudioBuffer same = rustle::render_clip(src, plain, 48000, 1);
    CHECK(same.frames() == src.frames() && same.samples() == src.samples());

    rustle::ClipEdit speed;
    speed.speed = 2.0f;
    const fa::AudioBuffer fast = rustle::render_clip(src, speed, 48000, 1);
    CHECK(std::fabs(fast.duration() - 0.5) < 0.02);
    CHECK(std::fabs(dominant(fast, 300.0, 1200.0, 5.0) - 880.0) < 10.0);

    rustle::ClipEdit tempo;
    tempo.tempo = 2.0f;
    const fa::AudioBuffer stretched = rustle::render_clip(src, tempo, 48000, 1);
    CHECK(std::fabs(stretched.duration() - 0.5) < 0.05);
    CHECK(std::fabs(dominant(stretched, 300.0, 1200.0, 5.0) - 440.0) < 10.0);

    rustle::ClipEdit slow;
    slow.tempo = 0.5f;
    const fa::AudioBuffer longer = rustle::render_clip(src, slow, 48000, 1);
    CHECK(std::fabs(longer.duration() - 2.0) < 0.1);

    rustle::ClipEdit pitch;
    pitch.pitch = 12.0f;
    const fa::AudioBuffer up = rustle::render_clip(src, pitch, 48000, 1);
    CHECK(std::fabs(up.duration() - 1.0) < 0.05);
    CHECK(std::fabs(dominant(up, 300.0, 1200.0, 5.0) - 880.0) < 15.0);

    rustle::ClipEdit both;
    both.tempo = 1.5f;
    both.pitch = -12.0f;
    const fa::AudioBuffer mixed = rustle::render_clip(src, both, 48000, 1);
    CHECK(std::fabs(mixed.duration() - 1.0 / 1.5) < 0.05);
    CHECK(std::fabs(dominant(mixed, 100.0, 800.0, 5.0) - 220.0) < 10.0);

    fa::AudioBuffer ramp(1, 48000, 4800);
    for (std::size_t i = 0; i < ramp.frames(); ++i) ramp.at(i, 0) = static_cast<float>(i) / 4800.0f;
    rustle::ClipEdit rev;
    rev.reverse = true;
    const fa::AudioBuffer backwards = rustle::render_clip(ramp, rev, 48000, 1);
    CHECK(backwards.frames() == ramp.frames() && backwards.at(0, 0) == ramp.at(ramp.frames() - 1, 0) && backwards.at(backwards.frames() - 1, 0) == 0.0f);

    rustle::ClipEdit trim;
    trim.trim_start = 0.25;
    trim.trim_end = 0.75;
    const fa::AudioBuffer cut = rustle::render_clip(src, trim, 48000, 1);
    CHECK(std::fabs(cut.duration() - 0.5) < 0.001);
    CHECK(std::fabs(cut.at(0, 0) - src.at(12000, 0)) < 1e-6f);
    rustle::ClipEdit bad_trim;
    bad_trim.trim_start = 5.0;
    CHECK(rustle::render_clip(src, bad_trim, 48000, 1).frames() == src.frames());

    rustle::ClipEdit wet;
    wet.reverb = 0.6f;
    wet.reverb_size = 0.9f;
    const fa::AudioBuffer roomy = rustle::render_clip(sine(440.0, 0.3), wet, 48000, 1);
    CHECK(roomy.duration() > 0.5);
    const std::size_t after = 14400 + 2400;
    CHECK(roomy.frames() > after + 4800 && rms(roomy.data() + after, 4800) > 0.005);

    rustle::ClipEdit fades;
    fades.fade_in = 0.1;
    fades.fade_out = 0.1;
    const fa::AudioBuffer faded = rustle::render_clip(src, fades, 48000, 1);
    CHECK(std::fabs(faded.at(10, 0)) < 0.01f && rms(faded.data(), 240) < rms(faded.data() + 24000, 240));
    CHECK(rms(faded.data() + faded.frames() - 240, 240) < 0.05);

    const fa::AudioBuffer stereo = rustle::render_clip(sine(440.0, 1.0, 44100, 1), plain, 48000, 2);
    CHECK(stereo.channels() == 2 && stereo.sample_rate() == 48000 && std::fabs(stereo.duration() - 1.0) < 0.01);
    CHECK(std::fabs(dominant(stereo, 300.0, 600.0, 5.0) - 440.0) < 5.0);

    const fs::path wav = temp_root() / "clip" / "tone.wav";
    write_wav(wav, src);
    rustle::ClipCache cache(64ull * 1024 * 1024);
    std::string why;
    const fa::Sound a = cache.get(wav.string(), plain, 48000, 2, &why);
    CHECK(a && a->channels() == 2 && a->frames() == 48000);
    CHECK(cache.peek(wav.string(), plain, 48000, 2) == a);
    CHECK(cache.get(wav.string(), plain, 48000, 2) == a);
    const fa::Sound b = cache.get(wav.string(), speed, 48000, 2);
    CHECK(b && b != a && std::fabs(b->duration() - 0.5) < 0.02);
    CHECK(cache.entries() == 3);
    rustle::ClipEdit louder;
    louder.volume = 2.0f;
    CHECK(cache.peek(wav.string(), louder, 48000, 2) == a);
    cache.forget(wav.string());
    CHECK(cache.entries() == 0 && !cache.peek(wav.string(), plain, 48000, 2));
    CHECK(!cache.get((temp_root() / "missing.wav").string(), plain, 48000, 2, &why) && !why.empty());
    rustle::ClipCache tiny(100);
    tiny.get(wav.string(), plain, 48000, 2);
    tiny.get(wav.string(), speed, 48000, 2);
    CHECK(tiny.entries() == 1);
}

void test_voice_chain() {
    const std::vector<float> input = test_voice(96000);
    {
        rustle::VoiceChain neutral;
        CHECK(neutral.neutral() && neutral.active_count() == 0 && neutral.latency_frames() == 0);
        const std::vector<float> out = run_chain(neutral, input);
        CHECK(out == input);
    }
    CHECK(rustle::voice_slots().size() == 22);
    std::size_t controls = 0;
    for (const rustle::VoiceSlotInfo& slot : rustle::voice_slots()) {
        rustle::VoiceChain chain;
        CHECK_MSG(chain.effect(slot.id) != nullptr, slot.id);
        CHECK_MSG(chain.effect(slot.id) && chain.effect(slot.id)->type() == slot.type, slot.id);
        if (slot.id == "pitch") chain.set_value("pitch.semitones", 5.0f);
        else if (!slot.controls.empty()) chain.set_value(slot.controls[0].id, slot.controls[0].low + 0.85f * (slot.controls[0].high - slot.controls[0].low));
        chain.set_enabled(slot.id, true);
        CHECK(!chain.neutral() && chain.active_count() == 1);
        const std::vector<float> on = run_chain(chain, input);
        CHECK_MSG(rms_diff(on, input, 4800) > 1e-4, "slot " + slot.id + " changes the signal");
        for (const rustle::VoiceControl& c : slot.controls) {
            ++controls;
            CHECK_MSG(c.low < c.high && c.fallback >= c.low && c.fallback <= c.high, c.id);
            std::vector<std::vector<float>> outs;
            for (float frac : { 0.15f, 0.85f }) {
                rustle::VoiceChain each;
                if (slot.id == "pitch" && c.param != "semitones") each.set_value("pitch.semitones", 5.0f);
                if (slot.id == "voice_eq" && c.param != "peak_db") each.set_value("voice_eq.peak_db", 12.0f);
                float v = c.low + frac * (c.high - c.low);
                if (!c.choices.empty()) v = frac < 0.5f ? c.low : c.high;
                CHECK_MSG(each.set_value(c.id, v), c.id);
                CHECK_MSG(each.enabled(slot.id), c.id);
                outs.push_back(run_chain(each, input));
            }
            CHECK_MSG(rms_diff(outs[0], outs[1], 4800) > 1e-5, "control " + c.id + " changes the sound");
        }
    }
    CHECK(controls >= 45);

    {
        rustle::VoiceChain inv;
        inv.set_enabled("invert", true);
        const std::vector<float> out = run_chain(inv, input);
        bool exact = true;
        for (std::size_t i = 0; i < input.size(); ++i) exact = exact && out[i] == -input[i];
        CHECK(exact);
    }
    {
        rustle::VoiceChain gate;
        gate.set_value("gate.threshold", -30.0f);
        std::vector<float> quiet(48000);
        for (std::size_t i = 0; i < quiet.size(); ++i) quiet[i] = 0.003f * static_cast<float>(std::sin(i * 0.05));
        const std::vector<float> out = run_chain(gate, quiet);
        CHECK(rms(out.data() + 24000, 24000) < 0.1 * rms(quiet.data() + 24000, 24000));
    }
    {
        rustle::VoiceChain lim;
        lim.set_value("limiter.ceiling", -6.0f);
        std::vector<float> loud(48000);
        for (std::size_t i = 0; i < loud.size(); ++i) loud[i] = 1.5f * static_cast<float>(std::sin(i * 0.05));
        const std::vector<float> out = run_chain(lim, loud);
        float peak = 0.0f;
        for (std::size_t i = 4800; i < out.size(); ++i) peak = std::max(peak, std::fabs(out[i]));
        CHECK(peak < 0.55f);
    }
    {
        rustle::VoiceChain g;
        g.set_value("gain.db", 6.0f);
        std::vector<float> tone(48000);
        for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = 0.1f * static_cast<float>(std::sin(i * 0.05));
        const std::vector<float> out = run_chain(g, tone);
        CHECK(std::fabs(rms(out.data() + 24000, 24000) / rms(tone.data() + 24000, 24000) - 1.995) < 0.02);
    }

    rustle::VoiceChain chain;
    chain.set_value("reverb.mix", 0.5f);
    chain.set_value("pitch.semitones", 3.0f);
    CHECK(chain.enabled("reverb") && chain.enabled("pitch") && chain.active_count() == 2);
    chain.prepare(48000, 1, 1024);
    CHECK(chain.latency_frames() > 0);
    CHECK(chain.reset_control("reverb.mix") && std::fabs(chain.value("reverb.mix") - 0.3f) < 1e-6f && chain.enabled("reverb"));
    CHECK(chain.reset_slot("pitch") && !chain.enabled("pitch") && chain.value("pitch.semitones") == 0.0f);
    CHECK(!chain.set_value("nope.nothing", 1.0f) && !chain.set_enabled("nope", true));
    CHECK(chain.set_value("pitch.semitones", 99.0f) && chain.value("pitch.semitones") == 12.0f);
    CHECK(!chain.set_value("pitch.semitones", std::nanf("")));
    chain.reset_all();
    CHECK(chain.neutral() && chain.value("pitch.semitones") == 0.0f);

    for (const rustle::VoiceSlotInfo& slot : rustle::voice_slots()) {
        for (const rustle::VoiceControl& c : slot.controls) {
            const rustle::VoiceSlotInfo* owner = nullptr;
            CHECK_MSG(rustle::find_voice_control(c.id, &owner) == &c && owner == &slot, c.id);
            CHECK_MSG(chain.value(c.id) == c.fallback, c.id + " starts at its default");
        }
    }

    {
        rustle::VoiceChain moving;
        moving.prepare(48000, 1, 1024);
        std::vector<float> buf = input;
        moving.process(buf.data(), 4800);
        moving.set_value("robot.frequency", 50.0f);
        moving.process(buf.data() + 4800, 4800);
        moving.set_enabled("robot", false);
        moving.process(buf.data() + 9600, 4800);
        float jump = 0.0f;
        for (std::size_t i = 4801; i < 14400; ++i) jump = std::max(jump, std::fabs(buf[i] - buf[i - 1]));
        float base = 0.0f;
        for (std::size_t i = 4801; i < 14400; ++i) base = std::max(base, std::fabs(input[i] - input[i - 1]));
        CHECK(jump < base * 6.0f + 0.05f);
        CHECK(std::equal(buf.begin() + 9600 + 600, buf.begin() + 14400, input.begin() + 9600 + 600));
    }

    {
        rustle::VoiceChain full;
        for (const rustle::VoiceSlotInfo& slot : rustle::voice_slots()) full.set_enabled(slot.id, true);
        full.prepare(48000, 1, 1024);
        std::vector<float> buf = input;
        for (std::size_t i = 0; i < 24000; i += 480) full.process(buf.data() + i, 480);
        full.set_value("pitch.semitones", 4.0f);
        full.set_enabled("reverb", false);
        g_allocations.store(0);
        g_counting.store(true);
        for (std::size_t i = 24000; i < 48000; i += 480) full.process(buf.data() + i, 480);
        g_counting.store(false);
        CHECK_MSG(g_allocations.load() == 0, std::to_string(g_allocations.load()) + " allocations while processing the full voice chain");
        bool finite = true;
        for (float v : buf) finite = finite && std::isfinite(v);
        CHECK(finite);
    }
}

void test_presets() {
    const std::vector<rustle::VoicePresetInfo> builtins = rustle::VoiceChain::builtin_presets();
    CHECK(builtins.size() >= 18);
    std::size_t fizmo_count = 0;
    for (const std::string& name : fa::builtin_voice_preset_names()) {
        if (name == "none") continue;
        ++fizmo_count;
        std::shared_ptr<fa::EffectChain> reference = fa::builtin_voice_preset(name);
        CHECK_MSG(reference != nullptr, name);
        if (!reference) continue;
        rustle::VoiceChain mine;
        std::string why;
        CHECK_MSG(mine.apply_builtin(name, &why) && why.empty(), name + ": " + why);
        CHECK_MSG(!mine.neutral(), name);
        CHECK_MSG(mine.active_count() == reference->size(), name);
        std::vector<float> a = test_voice(24000);
        std::vector<float> b = a;
        reference->prepare(48000, 1);
        for (std::size_t i = 0; i < a.size(); i += 256) reference->process(a.data() + i, std::min<std::size_t>(256, a.size() - i));
        b = run_chain(mine, b, 256);
        double worst = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
        CHECK_MSG(worst < 1e-6, name + " sounds the same through Rustle's panel (" + std::to_string(worst) + ")");
    }
    CHECK(fizmo_count >= 12);
    for (const rustle::VoicePresetInfo& p : builtins) {
        rustle::VoiceChain c;
        std::string why;
        CHECK_MSG(c.apply_builtin(p.name, &why), p.name + ": " + why);
        CHECK_MSG(!c.neutral(), p.name);
    }
    rustle::VoiceChain none;
    none.set_value("pitch.semitones", 3.0f);
    CHECK(none.apply_builtin("none") && none.neutral());
    std::string why;
    CHECK(!none.apply_builtin("does-not-exist", &why) && !why.empty());

    const fs::path dir = temp_root() / "presets";
    rustle::VoiceChain chain;
    chain.set_value("pitch.semitones", -4.0f);
    chain.set_value("echo.time_ms", 500.0f);
    chain.set_value("eq.band3_gain", 6.0f);
    chain.set_enabled("invert", true);
    chain.set_value("highpass.frequency", 300.0f);
    chain.set_value("lowpass.frequency", 3000.0f);
    std::string file;
    CHECK(rustle::save_user_preset(dir, chain, "  My Demon  ", &why, &file) && fs::exists(file));
    CHECK(fs::path(file).filename() == "my-demon.json");
    std::string text;
    CHECK(rustle::read_text_file(file, text));
    fa::PresetInfo info;
    CHECK(fa::read_preset_info(text, info) && info.name == "My Demon");
    std::shared_ptr<fa::EffectChain> raw = fa::load_preset_chain(text, nullptr, &why);
    CHECK(raw && raw->size() == rustle::voice_slots().size());
    const std::vector<rustle::VoicePresetInfo> listed = rustle::list_user_presets(dir);
    CHECK(listed.size() == 1 && listed[0].name == "My Demon" && listed[0].kind == "user");
    rustle::VoiceChain other;
    other.set_value("reverb.mix", 0.9f);
    CHECK(rustle::load_user_preset(dir, other, "My Demon", &why));
    CHECK(!other.enabled("reverb") && other.enabled("pitch") && other.value("pitch.semitones") == -4.0f && other.value("echo.time_ms") == 500.0f);
    CHECK(other.enabled("invert") && other.value("eq.band3_gain") == 6.0f);
    CHECK(other.enabled("highpass") && other.value("highpass.frequency") == 300.0f && other.enabled("lowpass") && other.value("lowpass.frequency") == 3000.0f);
    const std::vector<float> x = test_voice(24000);
    CHECK(run_chain(other, x) == run_chain(chain, x));
    CHECK(rustle::save_user_preset(dir, chain, "Second", &why));
    CHECK(!rustle::rename_user_preset(dir, "My Demon", "second", &why));
    CHECK(rustle::rename_user_preset(dir, "My Demon", "Renamed", &why));
    CHECK(rustle::list_user_presets(dir).size() == 2 && !fs::exists(file) && fs::exists(dir / "renamed.json"));
    CHECK(rustle::load_user_preset(dir, other, "Renamed", &why));
    CHECK(rustle::delete_user_preset(dir, "Renamed", &why) && rustle::list_user_presets(dir).size() == 1);
    CHECK(!rustle::delete_user_preset(dir, "Renamed", &why));
    CHECK(!rustle::save_user_preset(dir, chain, "   ", &why));
    CHECK(rustle::preset_slug("Hello, World!") == "hello-world" && rustle::preset_slug("***") == "preset");

    {
        std::ofstream(dir / "junk.json") << "{ not json";
    }
    CHECK(rustle::list_user_presets(dir).size() == 1);
    rustle::VoiceChain junk;
    CHECK(!junk.load_json("{ not json", &why) && !why.empty());
    CHECK(!junk.load_json("{\"format\":\"something-else\",\"chain\":[]}", &why));
    const std::string foreign = "{\"format\":\"fizmo-audio-preset\",\"format_version\":1,\"name\":\"x\",\"chain\":{\"type\":\"chain\",\"effects\":[{\"type\":\"phaser\",\"enabled\":true},{\"type\":\"reverb\",\"enabled\":true,\"parameters\":{\"mix\":0.7}}]}}";
    rustle::VoiceChain partial;
    why.clear();
    CHECK(partial.load_json(foreign, &why));
    CHECK(partial.enabled("reverb") && why.find("phaser") != std::string::npos);
}

void test_virtual_mic_arguments() {
    CHECK(rustle::VirtualMic::sink_arguments() == "sink_name=rustle_sink rate=48000 channels=2 sink_properties='device.description=\"Rustle Output\"'");
    CHECK(rustle::VirtualMic::source_arguments() == "master=rustle_sink.monitor source_name=rustle_mic source_properties='device.description=\"Rustle Mic\"'");
    CHECK(rustle::VirtualMic::quote_argument("it's a \\ test") == "'it\\'s a \\\\ test'");
    CHECK(rustle::is_rustle_device("rustle_sink") && rustle::is_rustle_device("rustle_mic") && !rustle::is_rustle_device("alsa_output"));
#if defined(__linux__)
    CHECK(rustle::VirtualMic::platform_supported());
#else
    CHECK(!rustle::VirtualMic::platform_supported());
    rustle::VirtualMic unsupported;
    std::string why;
    CHECK(!unsupported.setup(&why) && !why.empty());
    CHECK(unsupported.teardown(&why));
#endif
    rustle::VirtualMic mic;
    CHECK(!mic.owns_modules());
    mic.keep();
    CHECK(!mic.owns_modules());
}

void test_ring() {
    rustle::VoiceLink link;
    link.allocate(1000, 100);
    std::vector<float> in(50, 1.0f), out(64, 9.0f);
    link.write(in.data(), in.size());
    CHECK(link.read(out.data(), 64) == 0 && out[0] == 0.0f && out[63] == 0.0f);
    link.write(in.data(), in.size());
    CHECK(link.buffered() == 100);
    CHECK(link.read(out.data(), 64) == 64 && out[0] == 1.0f);
    CHECK(link.read(out.data(), 64) == 36 && out[35] == 1.0f && out[36] == 0.0f && link.underruns() == 1);
    std::vector<float> lots(900, 0.5f);
    link.write(lots.data(), lots.size());
    CHECK(link.read(out.data(), 64) == 64 && link.drops() == 1 && link.buffered() == 100);
    rustle::SampleRing ring;
    ring.allocate(5);
    CHECK(ring.capacity() == 8);
    std::vector<float> ten(10, 2.0f);
    CHECK(ring.push(ten.data(), 10) == 8 && ring.available() == 8);
    CHECK(ring.skip(3) == 3 && ring.pull(out.data(), 10) == 5);
}

void test_engine_offline() {
    rustle::Engine engine;
    engine.prepare_offline(48000);
    CHECK(engine.started());
    const std::size_t block = 480;
    std::vector<float> mic(block), send(block * 2), mon(block * 2);
    auto feed = [&](int blocks, double freq, float amp) {
        static std::size_t t = 0;
        double peak_send = 0.0, peak_mon = 0.0;
        for (int b = 0; b < blocks; ++b) {
            for (std::size_t i = 0; i < block; ++i, ++t) mic[i] = amp * static_cast<float>(std::sin(2.0 * k_pi * freq * static_cast<double>(t) / 48000.0));
            engine.process_capture(mic.data(), block, 1);
            engine.render_send(send.data(), block);
            engine.render_monitor(mon.data(), block);
            if (b >= blocks / 2) {
                for (float v : send) peak_send = std::max(peak_send, static_cast<double>(std::fabs(v)));
                for (float v : mon) peak_mon = std::max(peak_mon, static_cast<double>(std::fabs(v)));
            }
        }
        return std::make_pair(peak_send, peak_mon);
    };
    auto p = feed(20, 300.0, 0.4f);
    CHECK(std::fabs(p.first - 0.4) < 0.01 && p.second < 1e-6);
    CHECK(engine.take_mic_level() > 0.39f && engine.take_voice_level() > 0.39f && engine.take_send_level() > 0.39f);
    CHECK(engine.take_mic_level() == 0.0f);
    engine.set_mic_volume(0.5f);
    p = feed(20, 300.0, 0.4f);
    CHECK(std::fabs(p.first - 0.2) < 0.01);
    engine.set_mic_volume(1.0f);
    engine.set_mic_enabled(false);
    p = feed(20, 300.0, 0.4f);
    CHECK(p.first < 1e-6);
    engine.take_voice_level();
    feed(4, 300.0, 0.4f);
    CHECK(engine.take_mic_level() > 0.39f && engine.take_voice_level() < 1e-6f);
    engine.set_mic_enabled(true);
    engine.set_push_to_talk(true);
    CHECK(!engine.mic_open());
    p = feed(20, 300.0, 0.4f);
    CHECK(p.first < 1e-6);
    engine.set_talking(true);
    CHECK(engine.mic_open());
    p = feed(20, 300.0, 0.4f);
    CHECK(std::fabs(p.first - 0.4) < 0.01);
    engine.set_push_to_talk(false);
    engine.set_talking(false);
    engine.set_hear_myself(true);
    engine.set_hear_volume(0.5f);
    p = feed(30, 300.0, 0.4f);
    CHECK(std::fabs(p.second - 0.2) < 0.01);
    engine.set_hear_myself(false);
    engine.voice().set_value("gain.db", -6.0206f);
    p = feed(20, 300.0, 0.4f);
    CHECK(std::fabs(p.first - 0.2) < 0.01);
    engine.voice().reset_all();

    const fa::Sound tone = fa::make_sound(rustle::render_clip(sine(1000.0, 0.5, 48000, 1, 0.3f), rustle::ClipEdit(), 48000, 2));
    engine.set_sound_gains(1.0f, 1.0f, 0.5f, true, true);
    const std::uint64_t id = engine.play(tone, 1.0f, false, "tone");
    CHECK(id != 0 && engine.is_playing(id) && engine.tag_playing("tone"));
    std::vector<float> silence(block, 0.0f);
    double a = 0.0, b = 0.0;
    for (int k = 0; k < 10; ++k) {
        engine.process_capture(silence.data(), block, 1);
        engine.render_send(send.data(), block);
        engine.render_monitor(mon.data(), block);
        a = std::max(a, tone_level(send.data(), block, 2, 1000.0, 48000.0));
        b = std::max(b, tone_level(mon.data(), block, 2, 1000.0, 48000.0));
    }
    CHECK(std::fabs(a - 0.3) < 0.02 && std::fabs(b - 0.15) < 0.02);
    const std::vector<rustle::PlayingSound> now = engine.playing();
    CHECK(now.size() == 1 && now[0].tag == "tone" && now[0].position > 0.05 && std::fabs(now[0].length - 0.5) < 0.01);
    engine.set_sound_gains(1.0f, 1.0f, 1.0f, false, true);
    engine.render_send(send.data(), block);
    CHECK(rms(send.data(), send.size()) < 1e-6);
    engine.set_sound_gains(1.0f, 1.0f, 1.0f, true, true);
    for (int k = 0; k < 60; ++k) {
        engine.render_send(send.data(), block);
        engine.render_monitor(mon.data(), block);
    }
    CHECK(!engine.is_playing(id) && engine.playing().empty());
    const std::uint64_t looped = engine.play(tone, 1.0f, true, "loop");
    for (int k = 0; k < 120; ++k) {
        engine.render_send(send.data(), block);
        engine.render_monitor(mon.data(), block);
    }
    CHECK(engine.is_playing(looped));
    const std::uint64_t second = engine.play(tone, 1.0f, false, "other");
    CHECK(engine.playing().size() == 2);
    CHECK(engine.stop_tag("loop", 0.0) == 1 && !engine.is_playing(looped) && engine.is_playing(second));
    engine.stop_all(0.0);
    for (int k = 0; k < 4; ++k) engine.render_send(send.data(), block);
    CHECK(engine.playing().empty() && rms(send.data(), send.size()) < 1e-6);
    CHECK(engine.play(nullptr, 1.0f, false, "x") == 0);

    rustle::VoiceChain& v = engine.voice();
    for (const rustle::VoiceSlotInfo& slot : rustle::voice_slots()) v.set_enabled(slot.id, true);
    const std::uint64_t busy = engine.play(tone, 1.0f, true, "busy");
    for (int k = 0; k < 40; ++k) {
        engine.process_capture(mic.data(), block, 1);
        engine.render_send(send.data(), block);
        engine.render_monitor(mon.data(), block);
    }
    g_allocations.store(0);
    g_counting.store(true);
    for (int k = 0; k < 40; ++k) {
        engine.process_capture(mic.data(), block, 1);
        engine.render_send(send.data(), block);
        engine.render_monitor(mon.data(), block);
    }
    g_counting.store(false);
    CHECK_MSG(g_allocations.load() == 0, std::to_string(g_allocations.load()) + " allocations on the audio path");
    engine.stop_sound(busy, 0.0);
    v.reset_all();
    std::vector<float> stereo_in(block * 2, 0.25f);
    engine.process_capture(stereo_in.data(), block, 2);
    CHECK(engine.take_mic_level() > 0.24f);
}

void test_engine_null_backend() {
    std::string why;
    if (!fa::set_backend("null", &why)) {
        std::printf("note: the null audio backend isn't available (%s)\n", why.c_str());
        return;
    }
    CHECK(fa::backend_name() == "null");
    CHECK(!rustle::usable_devices(fa::DeviceKind::output).empty());
    rustle::Engine engine;
    rustle::EngineConfig c;
    c.send_device = "";
    CHECK(engine.start(c, &why));
    CHECK(engine.capture_status().running && engine.send_status().running && engine.monitor_status().running);
    CHECK(!engine.any_stream_failed());
    const fa::Sound tone = fa::make_sound(rustle::render_clip(sine(500.0, 0.3), rustle::ClipEdit(), 48000, 2));
    const std::uint64_t id = engine.play(tone, 1.0f, false, "t");
    CHECK(id != 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    double seen = 0.0;
    while (engine.is_playing(id) && std::chrono::steady_clock::now() < deadline) {
        seen = std::max(seen, static_cast<double>(engine.take_send_level()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!engine.is_playing(id));
    CHECK(seen > 0.4);
    engine.stop();
    CHECK(!engine.capture_status().running && !engine.started());
    rustle::EngineConfig only_monitor;
    only_monitor.capture = false;
    only_monitor.send = false;
    CHECK(engine.start(only_monitor, &why));
    CHECK(!engine.capture_status().wanted && !engine.send_status().running && engine.monitor_status().running);
    CHECK(engine.play(tone, 1.0f, false, "t") != 0);
    engine.stop();
}

void test_chords() {
    rustle::Chord c;
    CHECK(rustle::parse_chord("ctrl+shift+f1", c) && c.ctrl && c.shift && !c.alt && c.key == "F1");
    CHECK(rustle::chord_text(c) == "Ctrl+Shift+F1");
    CHECK(rustle::normalize_chord("Shift+Ctrl+a") == "Ctrl+Shift+A");
    CHECK(rustle::normalize_chord("Alt+Meta+Numpad5") == "Alt+Meta+Numpad5");
    CHECK(rustle::normalize_chord("esc") == "Escape" && rustle::normalize_chord("Up") == "UpArrow");
    CHECK(rustle::normalize_chord("Ctrl+") == "" && rustle::normalize_chord("Hyper+A") == "" && rustle::normalize_chord("Ctrl+Banana") == "");
    CHECK(rustle::normalize_chord("Ctrl+Plus") == "" && rustle::normalize_chord("Minus") == "Minus");
    CHECK(rustle::is_modifier_key("LeftShift") && rustle::is_modifier_key("RightMeta") && !rustle::is_modifier_key("A"));
    CHECK(rustle::x11_keysym_name("A") == "a" && rustle::x11_keysym_name("7") == "7" && rustle::x11_keysym_name("F12") == "F12");
    CHECK(rustle::x11_keysym_name("PageUp") == "Prior" && rustle::x11_keysym_name("Numpad3") == "KP_3" && rustle::x11_keysym_name("Space") == "space");
    CHECK(rustle::x11_keysym_name("MediaPlayPause") == "XF86AudioPlay");
    CHECK(rustle::windows_virtual_key("A") == 'A' && rustle::windows_virtual_key("F1") == 0x70 && rustle::windows_virtual_key("F24") == 0x87);
    CHECK(rustle::windows_virtual_key("Numpad0") == 0x60 && rustle::windows_virtual_key("Escape") == 0x1B && rustle::windows_virtual_key("Bogus") == 0);
    for (const char* k : { "A", "Z", "0", "9", "F1", "F24", "Escape", "Enter", "Space", "Minus", "Tilde", "LeftArrow", "Numpad9", "NumpadAdd", "Pause", "MediaNext" }) {
        CHECK_MSG(!rustle::x11_keysym_name(k).empty(), k);
        CHECK_MSG(rustle::windows_virtual_key(k) != 0, k);
    }
    const std::string kind = rustle::desktop_session_kind();
    CHECK(kind == "x11" || kind == "wayland" || kind == "none" || kind == "windows" || kind == "macos");
}

void test_session() {
    const fs::path cfg = temp_root() / "session-config";
    const fs::path sounds = temp_root() / "session-sounds";
    write_wav(sounds / "Board" / "air_horn.wav", sine(440.0, 0.4));
    write_wav(sounds / "Board" / "sad_trombone.wav", sine(220.0, 0.6));
    write_wav(sounds / "elsewhere.wav", sine(330.0, 0.2));
    rustle::SessionOptions o;
    o.config_dir = cfg;
    o.audio = false;
    o.virtual_mic = false;
    o.hotkeys = false;
    o.control = false;
    const std::string horn = (sounds / "Board" / "air_horn.wav").string();
    const std::string trombone = (sounds / "Board" / "sad_trombone.wav").string();
    {
        rustle::Session s(o);
        s.open();
        CHECK(s.load_result().ok && !s.load_result().existed);
        std::string why;
        CHECK(s.add_folder((sounds / "Board").string(), &why));
        CHECK(!s.add_folder((sounds / "Board").string(), &why) && !why.empty());
        CHECK(!s.add_folder((sounds / "missing").string(), &why));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (s.library().sound_count() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(s.library().sound_count() == 2);
        std::string path;
        CHECK(s.find_by_name("Air Horn", path) && path == horn);
        CHECK(s.find_by_name("trom", path) && path == trombone);
        CHECK(!s.find_by_name("kazoo", path));
        CHECK(s.play(horn, &why));
        const auto wait = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!s.playing(horn) && std::chrono::steady_clock::now() < wait) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(s.playing(horn) && !s.loading(horn));
        CHECK(s.toggle(horn, &why) && !s.playing(horn));
        CHECK(!s.play((sounds / "gone.wav").string(), &why) && !why.empty());
        CHECK(s.settings().play_counts[horn] == 1);

        rustle::ClipEdit e;
        e.pitch = 7.0f;
        e.volume = 0.5f;
        s.set_edit(horn, e);
        CHECK(s.edit_of(horn) == e);
        s.set_favorite(trombone, true);
        CHECK(s.favorite(trombone));
        CHECK(s.set_keybind(horn, "ctrl+h", &why) && s.keybind_of(horn) == "Ctrl+H");
        CHECK(!s.set_keybind(trombone, "Ctrl+H", &why) && why.find("air horn") != std::string::npos);
        CHECK(!s.set_keybind(trombone, "F8", &why) && why.find("Stop all") != std::string::npos);
        CHECK(!s.set_keybind(trombone, "Ctrl+Nonsense", &why));
        CHECK(s.chord_owner("Ctrl+H") == "play:" + horn && s.chord_owner("F8") == "stop-all" && s.chord_owner("F9") == "toggle-mic");

        s.key_event("Ctrl+H", true, true);
        s.key_event("Ctrl+H", false, true);
        const auto keyed = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!s.playing(horn) && std::chrono::steady_clock::now() < keyed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(s.playing(horn));
        s.key_event("F8", true, true);
        CHECK(!s.playing(horn));
        CHECK(s.settings().mic_enabled);
        s.key_event("F9", true, true);
        CHECK(!s.settings().mic_enabled && !s.engine().mic_enabled());
        s.key_event("F9", true, true);
        CHECK(s.settings().mic_enabled);
        s.settings().key_push_to_talk = "F10";
        s.settings().push_to_talk = true;
        s.apply_levels();
        CHECK(!s.engine().mic_open());
        s.key_event("F10", true, true);
        CHECK(s.engine().mic_open());
        s.key_event("F10", false, true);
        CHECK(!s.engine().mic_open());

        CHECK(s.control_command("status").find("2 sounds") != std::string::npos);
        CHECK(s.control_command("toggle-mic") == "ok\tmic off");
        CHECK(s.control_command("mic-on") == "ok" && s.settings().mic_enabled);
        CHECK(s.control_command("play sad trombone").compare(0, 2, "ok") == 0);
        CHECK(s.control_command("play kazoo").compare(0, 5, "error") == 0);
        CHECK(s.control_command("stop-all") == "ok");
        CHECK(s.control_command("preset robot") == "ok" && !s.voice().neutral());
        CHECK(s.control_command("voice-off") == "ok" && s.voice().neutral());
        CHECK(s.control_command("preset nonsense").compare(0, 5, "error") == 0);
        CHECK(s.control_command("ptt-down") == "ok" && s.engine().talking());
        CHECK(s.control_command("ptt-up") == "ok" && !s.engine().talking());
        CHECK(s.control_command("dance").compare(0, 5, "error") == 0);

        CHECK(s.apply_voice_preset("Deep", &why) && s.settings().voice_preset == "Deep");
        s.voice().set_value("chorus.mix", 0.7f);
        CHECK(!s.save_voice_preset("robot", &why));
        CHECK(s.save_voice_preset("Chorus Deep", &why));
        bool listed = false;
        for (const rustle::VoicePresetInfo& p : s.voice_presets()) listed = listed || (p.name == "Chorus Deep" && p.kind == "user");
        CHECK(listed);
        s.voice().reset_all();
        CHECK(s.apply_voice_preset("Chorus Deep", &why) && s.voice().enabled("chorus") && std::fabs(s.voice().value("chorus.mix") - 0.7f) < 1e-6f);
        CHECK(!s.rename_voice_preset("Chorus Deep", "Helium", &why));
        CHECK(s.rename_voice_preset("Chorus Deep", "Wobble", &why) && s.settings().voice_preset == "Wobble");

        const rustle::RouteStatus r = s.route_status();
        CHECK(!r.virtual_mic_supported && r.mode == "auto");
        s.notice("info", "hello");
        const std::vector<rustle::Notice> n = s.notices_since(0);
        CHECK(!n.empty() && n.back().text == "hello");
        CHECK(s.notices_since(n.back().seq).empty());
        s.close();
    }
    CHECK(fs::exists(cfg / "settings.json"));
    {
        rustle::Session again(o);
        again.open();
        CHECK(again.load_result().ok && again.load_result().existed);
        CHECK(again.settings().folders.size() == 1);
        CHECK(again.edit_of(horn).pitch == 7.0f && again.favorite(trombone) && again.keybind_of(horn) == "Ctrl+H");
        CHECK(again.settings().voice_preset == "Wobble" && again.voice().enabled("chorus") && std::fabs(again.voice().value("chorus.mix") - 0.7f) < 1e-6f);
        CHECK(again.remove_folder((sounds / "Board").string()) && again.settings().folders.empty());
        CHECK(!again.remove_folder((sounds / "Board").string()));
        again.reset_edit(horn);
        CHECK(again.edit_of(horn).is_default());
        again.close();
    }
    {
        std::ofstream(cfg / "settings.json") << "garbage {";
        rustle::Session broken(o);
        broken.open();
        CHECK(!broken.load_result().ok && broken.load_result().recovered);
        bool warned = false;
        for (const rustle::Notice& n : broken.notices_since(0)) warned = warned || n.kind == "warning";
        CHECK(warned);
        broken.close();
        rustle::Settings fixed;
        CHECK(rustle::load_settings(cfg / "settings.json", fixed).ok);
    }
}

void test_control_socket() {
#if !defined(_WIN32)
    const std::string path = (temp_root() / "ctl.sock").string();
    setenv("RUSTLE_CONTROL_SOCKET", path.c_str(), 1);
    CHECK(rustle::control_socket_path() == path);
    rustle::ControlServer server;
    std::string why;
    CHECK(server.start([](const std::string& line) { return "ok\tgot " + line; }, &why));
    std::string reply;
    CHECK(rustle::send_control("play air horn", reply, &why) && reply == "ok\tgot play air horn");
    rustle::ControlServer second;
    CHECK(!second.start([](const std::string&) { return std::string("x"); }, &why) && why.find("already") != std::string::npos);
    server.stop();
    CHECK(!fs::exists(path));
    CHECK(!rustle::send_control("status", reply, &why) && !why.empty());
    unsetenv("RUSTLE_CONTROL_SOCKET");
#else
    std::string reply, why;
    CHECK(!rustle::send_control("status", reply, &why));
#endif
}

} // namespace

int main() {
    test_settings();
    test_library();
    test_files();
    test_clip_render();
    test_voice_chain();
    test_presets();
    test_virtual_mic_arguments();
    test_ring();
    test_engine_offline();
    test_engine_null_backend();
    test_chords();
    test_session();
    test_control_socket();
    std::error_code ec;
    fs::remove_all(temp_root(), ec);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
