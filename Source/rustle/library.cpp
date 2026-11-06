#include <rustle/library.hpp>

#include <audio.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace rustle {

namespace {

constexpr std::size_t k_max_sounds = 200000;
constexpr int k_max_depth = 40;

std::int64_t stamp_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return -1;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::int64_t entry_count(const fs::path& p) {
    std::error_code ec;
    fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec);
    if (ec) return -1;
    std::int64_t n = 0;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        ++n;
    }
    return n;
}

std::int64_t folder_stamp(const fs::path& p) {
    const std::int64_t t = stamp_of(p);
    const std::int64_t n = entry_count(p);
    return t * 1000003 + n;
}

std::string extension_of(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower_text(e);
}

bool hidden_name(const std::string& name) {
    return !name.empty() && name[0] == '.';
}

std::vector<std::string> words_of(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : lower_text(text)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

SortOrder sort_order_from(const std::string& text) {
    if (text == "duration") return SortOrder::duration;
    if (text == "newest") return SortOrder::newest;
    if (text == "played") return SortOrder::played;
    return SortOrder::name;
}

bool is_sound_extension(const std::string& extension) {
    const std::string e = lower_text(extension);
    if (e.empty()) return false;
    static const std::vector<std::string> known = fizmo::audio::readable_audio_extensions();
    return std::find(known.begin(), known.end(), e) != known.end();
}

bool is_video_extension(const std::string& extension) {
    const std::string e = lower_text(extension);
    return e == "mp4" || e == "m4v" || e == "mov" || e == "3gp" || e == "3g2" || e == "mkv" || e == "webm" || e == "ogv";
}

std::string lower_text(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string display_name(const std::string& file_name) {
    std::string stem = fs::path(file_name).stem().string();
    for (char& c : stem) {
        if (c == '_') c = ' ';
    }
    return stem.empty() ? file_name : stem;
}

std::string normalize_folder(const std::string& path) {
    if (path.empty()) return path;
    std::string s = fs::path(path).lexically_normal().string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
        if (s.size() == 3 && s[1] == ':') break;
        s.pop_back();
    }
    return s;
}

bool path_inside(const std::string& path, const std::string& folder) {
    if (folder.empty()) return false;
    if (path == folder) return true;
    if (path.size() <= folder.size() || path.compare(0, folder.size(), folder) != 0) return false;
    const char next = path[folder.size()];
    const char last = folder.back();
    return next == '/' || next == '\\' || last == '/' || last == '\\';
}

void Library::set_roots(const std::vector<std::string>& roots) {
    std::vector<std::string> clean;
    for (const std::string& r : roots) {
        const std::string n = normalize_folder(r);
        if (n.empty() || std::find(clean.begin(), clean.end(), n) != clean.end()) continue;
        clean.push_back(n);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_roots = clean;
}

std::vector<std::string> Library::roots() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_roots;
}

void Library::scan_root(const std::string& root, std::map<std::string, Folder>& folders, std::vector<SoundEntry>& sounds, std::map<std::string, std::int64_t>& stamps,
    std::set<std::string>& visited) const {
    std::error_code ec;
    stamps[root] = folder_stamp(root);
    if (!fs::is_directory(root, ec)) return;
    struct Pending {
        std::string path;
        int depth;
    };
    std::vector<Pending> stack{ { root, 0 } };
    while (!stack.empty()) {
        const Pending cur = stack.back();
        stack.pop_back();
        const fs::path canon = fs::weakly_canonical(cur.path, ec);
        const std::string key = ec ? cur.path : canon.string();
        if (!visited.insert(key).second) continue;
        Folder& folder = folders[cur.path];
        folder.name = cur.path == root ? fs::path(root).filename().string() : fs::path(cur.path).filename().string();
        if (folder.name.empty()) folder.name = root;
        folder.root = root;
        stamps[cur.path] = folder_stamp(cur.path);
        std::vector<fs::directory_entry> entries;
        fs::directory_iterator it(cur.path, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            entries.push_back(*it);
        }
        std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return lower_text(a.path().filename().string()) < lower_text(b.path().filename().string());
        });
        for (const fs::directory_entry& e : entries) {
            const std::string name = e.path().filename().string();
            if (hidden_name(name)) continue;
            std::error_code dec;
            if (e.is_directory(dec)) {
                if (cur.depth + 1 > k_max_depth) continue;
                const std::string child = e.path().string();
                folder.children.push_back(child);
                stack.push_back({ child, cur.depth + 1 });
                continue;
            }
            if (!e.is_regular_file(dec)) continue;
            const std::string ext = extension_of(e.path());
            if (!is_sound_extension(ext)) continue;
            if (sounds.size() >= k_max_sounds) continue;
            SoundEntry s;
            s.path = e.path().string();
            s.name = display_name(name);
            s.folder = cur.path;
            s.root = root;
            s.extension = ext;
            s.video = is_video_extension(ext);
            s.size = static_cast<std::uint64_t>(e.file_size(dec));
            s.modified = stamp_of(e.path());
            folder.sounds.push_back(sounds.size());
            sounds.push_back(std::move(s));
        }
    }
}

std::size_t Library::count_total(std::map<std::string, Folder>& folders, const std::string& path, int depth) const {
    auto it = folders.find(path);
    if (it == folders.end() || depth > k_max_depth + 2) return 0;
    std::size_t total = it->second.sounds.size();
    const std::vector<std::string> children = it->second.children;
    for (const std::string& c : children) total += count_total(folders, c, depth + 1);
    folders[path].total = total;
    return total;
}

void Library::rescan() {
    const std::vector<std::string> roots = this->roots();
    std::map<std::string, Folder> folders;
    std::vector<SoundEntry> sounds;
    std::map<std::string, std::int64_t> stamps;
    std::set<std::string> visited;
    for (const std::string& r : roots) scan_root(r, folders, sounds, stamps, visited);
    for (const std::string& r : roots) count_total(folders, r, 0);
    std::map<std::string, std::size_t> by_path;
    for (std::size_t i = 0; i < sounds.size(); ++i) by_path[sounds[i].path] = i;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_roots != roots) return;
    std::map<std::string, Duration> durations;
    for (const SoundEntry& s : sounds) {
        auto d = m_durations.find(s.path);
        if (d != m_durations.end() && d->second.modified == s.modified && d->second.size == s.size) durations[s.path] = d->second;
    }
    m_folders = std::move(folders);
    m_sounds = std::move(sounds);
    m_by_path = std::move(by_path);
    m_stamps = std::move(stamps);
    m_durations = std::move(durations);
    ++m_revision;
}

bool Library::changed_on_disk() const {
    std::map<std::string, std::int64_t> stamps;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stamps = m_stamps;
    }
    for (const auto& kv : stamps) {
        if (folder_stamp(kv.first) != kv.second) return true;
    }
    return false;
}

std::uint64_t Library::revision() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_revision;
}

std::size_t Library::sound_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sounds.size();
}

std::size_t Library::folder_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_folders.size();
}

bool Library::has_folder(const std::string& folder) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_folders.count(normalize_folder(folder)) != 0;
}

std::vector<FolderEntry> Library::subfolders(const std::string& folder) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<FolderEntry> out;
    std::vector<std::string> paths;
    if (folder.empty()) {
        paths = m_roots;
    } else {
        auto it = m_folders.find(normalize_folder(folder));
        if (it == m_folders.end()) return out;
        paths = it->second.children;
    }
    for (const std::string& p : paths) {
        auto f = m_folders.find(p);
        FolderEntry e;
        e.path = p;
        if (f == m_folders.end()) {
            if (!folder.empty()) continue;
            e.name = fs::path(p).filename().string();
            if (e.name.empty()) e.name = p;
            out.push_back(e);
            continue;
        }
        if (!folder.empty() && f->second.total == 0) continue;
        e.name = f->second.name;
        e.sounds = f->second.total;
        for (const std::string& c : f->second.children) {
            auto cf = m_folders.find(c);
            if (cf != m_folders.end() && cf->second.total > 0) ++e.folders;
        }
        out.push_back(e);
    }
    return out;
}

std::vector<SoundEntry> Library::sounds_in(const std::string& folder) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SoundEntry> out;
    auto it = m_folders.find(normalize_folder(folder));
    if (it == m_folders.end()) return out;
    for (std::size_t i : it->second.sounds) out.push_back(m_sounds[i]);
    return out;
}

std::vector<SoundEntry> Library::all_sounds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sounds;
}

std::vector<SoundEntry> Library::search(const std::string& query, std::size_t limit) const {
    const std::vector<std::string> words = words_of(query);
    std::vector<SoundEntry> out;
    if (words.empty()) return out;
    const std::string whole = lower_text(query);
    std::vector<std::pair<int, SoundEntry>> ranked;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const SoundEntry& s : m_sounds) {
            const std::string name = lower_text(s.name);
            std::string rel = s.path.size() > s.root.size() ? s.path.substr(s.root.size()) : s.path;
            rel = lower_text(rel);
            bool all = true;
            for (const std::string& w : words) {
                if (name.find(w) == std::string::npos && rel.find(w) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (!all) continue;
            int rank = 3;
            if (name.compare(0, whole.size(), whole) == 0) rank = 0;
            else if (name.find(whole) != std::string::npos) rank = 1;
            else {
                bool in_name = true;
                for (const std::string& w : words) in_name = in_name && name.find(w) != std::string::npos;
                if (in_name) rank = 2;
            }
            ranked.emplace_back(rank, s);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return lower_text(a.second.name) < lower_text(b.second.name);
    });
    for (auto& r : ranked) {
        if (out.size() >= limit) break;
        out.push_back(std::move(r.second));
    }
    return out;
}

bool Library::find(const std::string& path, SoundEntry& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_by_path.find(path);
    if (it == m_by_path.end()) return false;
    out = m_sounds[it->second];
    return true;
}

std::string Library::root_of(const std::string& path) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string best;
    for (const std::string& r : m_roots) {
        if (path_inside(path, r) && r.size() > best.size()) best = r;
    }
    return best;
}

std::vector<Crumb> Library::breadcrumbs(const std::string& folder) const {
    std::vector<Crumb> out;
    out.push_back({ "Library", "" });
    const std::string f = normalize_folder(folder);
    if (f.empty()) return out;
    const std::string root = root_of(f);
    if (root.empty()) return out;
    std::string root_name = fs::path(root).filename().string();
    if (root_name.empty()) root_name = root;
    out.push_back({ root_name, root });
    if (f == root) return out;
    const fs::path rel = fs::path(f).lexically_relative(root);
    fs::path cur = root;
    for (const fs::path& part : rel) {
        if (part.empty() || part == ".") continue;
        cur /= part;
        out.push_back({ part.string(), cur.string() });
    }
    return out;
}

std::string Library::parent_of(const std::string& folder) const {
    const std::string f = normalize_folder(folder);
    if (f.empty()) return std::string();
    const std::string root = root_of(f);
    if (root.empty() || f == root) return std::string();
    return fs::path(f).parent_path().string();
}

void Library::set_duration(const std::string& path, double seconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_by_path.find(path);
    if (it == m_by_path.end()) return;
    Duration d;
    d.modified = m_sounds[it->second].modified;
    d.size = m_sounds[it->second].size;
    d.seconds = seconds;
    m_durations[path] = d;
}

double Library::duration(const std::string& path) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_durations.find(path);
    return it == m_durations.end() ? -1.0 : it->second.seconds;
}

std::vector<std::string> Library::missing_durations(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    for (const SoundEntry& s : m_sounds) {
        if (out.size() >= limit) break;
        if (m_durations.find(s.path) == m_durations.end()) out.push_back(s.path);
    }
    return out;
}

void Library::sort(std::vector<SoundEntry>& sounds, SortOrder order, const std::map<std::string, int>& plays, const std::set<std::string>& favorites,
    bool favorites_first) const {
    std::map<std::string, double> durations;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const SoundEntry& s : sounds) {
            auto it = m_durations.find(s.path);
            durations[s.path] = it == m_durations.end() ? -1.0 : it->second.seconds;
        }
    }
    auto played = [&](const std::string& p) {
        auto it = plays.find(p);
        return it == plays.end() ? 0 : it->second;
    };
    std::stable_sort(sounds.begin(), sounds.end(), [&](const SoundEntry& a, const SoundEntry& b) {
        if (favorites_first) {
            const bool fa = favorites.count(a.path) != 0;
            const bool fb = favorites.count(b.path) != 0;
            if (fa != fb) return fa;
        }
        switch (order) {
        case SortOrder::duration: {
            const double da = durations[a.path];
            const double db = durations[b.path];
            if ((da < 0.0) != (db < 0.0)) return db < 0.0;
            if (da != db) return da < db;
            break;
        }
        case SortOrder::newest:
            if (a.modified != b.modified) return a.modified > b.modified;
            break;
        case SortOrder::played: {
            const int pa = played(a.path);
            const int pb = played(b.path);
            if (pa != pb) return pa > pb;
            break;
        }
        case SortOrder::name:
            break;
        }
        const std::string na = lower_text(a.name);
        const std::string nb = lower_text(b.name);
        if (na != nb) return na < nb;
        return a.path < b.path;
    });
}

} // namespace rustle
