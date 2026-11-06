#include <rustle/files.hpp>
#include <rustle/json.hpp>
#include <rustle/library.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace rustle {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool is_folder(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(path, ec);
}

std::string unescape_mount(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
            const int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out += static_cast<char>(v);
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void add_place(std::vector<Place>& out, const std::string& label, const std::string& path, const std::string& kind) {
    if (!is_folder(path)) return;
    for (const Place& p : out) {
        if (p.path == path) return;
    }
    out.push_back({ label, path, kind });
}

} // namespace

std::string home_folder() {
#if defined(_WIN32)
    std::string h = env("USERPROFILE");
    if (h.empty()) h = env("HOMEDRIVE") + env("HOMEPATH");
    return h;
#else
    return env("HOME");
#endif
}

std::vector<Place> parse_user_dirs(const std::string& text, const std::string& home) {
    std::vector<Place> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || !starts_with(line, "XDG_")) continue;
        std::string key = line.substr(4, eq - 4);
        std::string value = line.substr(eq + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (starts_with(value, "$HOME")) value = home + value.substr(5);
        if (key.size() > 4 && key.compare(key.size() - 4, 4, "_DIR") == 0) key = key.substr(0, key.size() - 4);
        if (value.empty() || value == home || value == home + "/") continue;
        std::string label = lower_text(key);
        if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        if (label == "Publicshare") label = "Public";
        out.push_back({ label, value, lower_text(key) });
    }
    return out;
}

std::vector<Place> parse_mounts(const std::string& mounts_text, const std::string& home) {
    std::vector<Place> out;
    std::istringstream in(mounts_text);
    std::string line;
    static const char* system_prefixes[] = { "/boot", "/snap", "/var", "/usr", "/etc", "/opt", "/srv", "/tmp", "/efi", "/recovery", "/proc", "/sys", "/dev", "/run/user", "/run/snapd", "/run/credentials", "/nix", "/gnu" };
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string device, mount, type;
        if (!(fields >> device >> mount >> type)) continue;
        mount = unescape_mount(mount);
        device = unescape_mount(device);
        if (mount == "/" || mount == home) continue;
        bool system = false;
        for (const char* p : system_prefixes) {
            if (path_inside(mount, p)) system = true;
        }
        if (system) continue;
        const bool removable = path_inside(mount, "/media") || path_inside(mount, "/run/media") || path_inside(mount, "/mnt") || path_inside(mount, "/Volumes");
        const bool network = type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3" || type == "fuse.sshfs" || type == "fuse.rclone";
        const bool block = starts_with(device, "/dev/") && !starts_with(device, "/dev/loop");
        if (!(removable || network || (block && type != "squashfs" && !path_inside(mount, "/home")))) continue;
        if (!removable && !network && path_inside(mount, home)) continue;
        std::string label = fs::path(mount).filename().string();
        if (label.empty()) label = mount;
        bool dup = false;
        for (const Place& p : out) dup = dup || p.path == mount;
        if (!dup) out.push_back({ label, mount, network ? "network" : "drive" });
    }
    return out;
}

std::vector<Place> mounted_drives() {
    std::vector<Place> out;
#if defined(_WIN32)
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
        const UINT type = GetDriveTypeA(root.c_str());
        if (type == DRIVE_NO_ROOT_DIR) continue;
        std::string kind = "drive";
        if (type == DRIVE_REMOTE) kind = "network";
        out.push_back({ root.substr(0, 2), root, kind });
    }
#elif defined(__APPLE__)
    std::error_code ec;
    fs::directory_iterator it("/Volumes", fs::directory_options::skip_permission_denied, ec);
    if (!ec) {
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            out.push_back({ it->path().filename().string(), it->path().string(), "drive" });
        }
    }
#else
    std::string text;
    if (read_text_file("/proc/self/mounts", text)) out = parse_mounts(text, home_folder());
    std::vector<Place> kept;
    for (const Place& p : out) {
        if (is_folder(p.path)) kept.push_back(p);
    }
    out = kept;
#endif
    return out;
}

std::vector<Place> places() {
    std::vector<Place> out;
    const std::string home = home_folder();
    add_place(out, "Home", home, "home");
#if defined(_WIN32) || defined(__APPLE__)
    for (const char* name : { "Desktop", "Documents", "Downloads", "Music", "Videos", "Movies" }) {
        add_place(out, name, (fs::path(home) / name).string(), lower_text(name));
    }
#else
    std::string config = env("XDG_CONFIG_HOME");
    if (config.empty()) config = home + "/.config";
    std::string text;
    std::vector<Place> user;
    if (read_text_file(config + "/user-dirs.dirs", text)) user = parse_user_dirs(text, home);
    if (user.empty()) {
        for (const char* name : { "Desktop", "Documents", "Downloads", "Music", "Videos" }) user.push_back({ name, home + "/" + name, lower_text(name) });
    }
    for (const Place& p : user) {
        if (p.kind == "templates" || p.kind == "publicshare") continue;
        add_place(out, p.label, p.path, p.kind);
    }
    add_place(out, "Computer", "/", "root");
#endif
    for (const Place& d : mounted_drives()) add_place(out, d.label, d.path, d.kind);
    return out;
}

bool list_folder(const std::string& path, bool show_hidden, std::vector<FileItem>& out, std::string* error) {
    out.clear();
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        if (error) *error = path + " isn't a folder that can be opened";
        return false;
    }
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (error) *error = "can't open " + path + ": " + ec.message();
        return false;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        FileItem item;
        item.path = it->path().string();
        item.name = it->path().filename().string();
        item.hidden = !item.name.empty() && item.name[0] == '.';
#if defined(_WIN32)
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) item.hidden = true;
#endif
        if (item.hidden && !show_hidden) continue;
        std::error_code fec;
        item.folder = it->is_directory(fec);
        if (!item.folder) {
            if (!it->is_regular_file(fec)) continue;
            item.size = static_cast<std::uint64_t>(it->file_size(fec));
            std::string ext = it->path().extension().string();
            if (!ext.empty()) ext.erase(0, 1);
            item.sound = is_sound_extension(ext);
        }
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(), [](const FileItem& a, const FileItem& b) {
        if (a.folder != b.folder) return a.folder;
        const std::string la = lower_text(a.name), lb = lower_text(b.name);
        if (la != lb) return la < lb;
        return a.name < b.name;
    });
    return true;
}

std::string parent_folder(const std::string& path) {
    const std::string n = normalize_folder(path);
    if (n.empty()) return n;
    const fs::path p(n);
    if (p == p.root_path() || !p.has_relative_path()) return n;
    return normalize_folder(p.parent_path().string());
}

std::vector<std::pair<std::string, std::string>> path_parts(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> out;
    const fs::path p(normalize_folder(path));
    fs::path cur;
    for (const fs::path& part : p) {
        if (part.empty()) continue;
        cur /= part;
        std::string label = part.string();
        if (label == "/" || label == "\\") label = "/";
        if (out.empty() && p.has_root_name() && cur == p.root_name()) continue;
        std::string at = cur.string();
        if (out.empty() && p.has_root_name()) {
            at = p.root_path().string();
            label = p.root_name().string();
        }
        out.emplace_back(label, at);
    }
    return out;
}

std::string format_size(std::uint64_t bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024ull) std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024ull * 1024ull) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string unique_destination(const std::string& folder, const std::string& file_name) {
    const fs::path name(file_name);
    const std::string stem = name.stem().string();
    const std::string ext = name.extension().string();
    std::error_code ec;
    fs::path candidate = fs::path(folder) / file_name;
    for (int i = 2; fs::exists(candidate, ec) && i < 10000; ++i) {
        candidate = fs::path(folder) / (stem + " (" + std::to_string(i) + ")" + ext);
    }
    return candidate.string();
}

bool copy_into(const std::vector<std::string>& files, const std::string& folder, std::vector<std::string>& copied, std::string* error) {
    copied.clear();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        if (error) *error = folder + " isn't a folder";
        return false;
    }
    std::string problems;
    for (const std::string& f : files) {
        if (!fs::is_regular_file(f, ec)) {
            problems += (problems.empty() ? "" : "; ") + fs::path(f).filename().string() + " isn't a file";
            continue;
        }
        if (normalize_folder(fs::path(f).parent_path().string()) == normalize_folder(folder)) {
            problems += (problems.empty() ? "" : "; ") + fs::path(f).filename().string() + " is already in that folder";
            continue;
        }
        const std::string dest = unique_destination(folder, fs::path(f).filename().string());
        fs::copy_file(f, dest, fs::copy_options::none, ec);
        if (ec) {
            problems += (problems.empty() ? "" : "; ") + fs::path(f).filename().string() + ": " + ec.message();
            ec.clear();
            continue;
        }
        copied.push_back(dest);
    }
    if (!problems.empty() && error) *error = problems;
    return problems.empty();
}

bool make_folder(const std::string& parent, const std::string& name, std::string& made, std::string* error) {
    std::string clean = name;
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
    if (clean.empty() || clean == "." || clean == ".." || clean.find('/') != std::string::npos || clean.find('\\') != std::string::npos) {
        if (error) *error = "that isn't a usable folder name";
        return false;
    }
    std::error_code ec;
    const fs::path target = fs::path(parent) / clean;
    if (fs::exists(target, ec)) {
        if (error) *error = clean + " already exists there";
        return false;
    }
    if (!fs::create_directory(target, ec)) {
        if (error) *error = "can't make " + target.string() + (ec ? ": " + ec.message() : std::string());
        return false;
    }
    made = target.string();
    return true;
}

} // namespace rustle
