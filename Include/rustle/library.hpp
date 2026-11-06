#ifndef RUSTLE_LIBRARY_HPP
#define RUSTLE_LIBRARY_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rustle {

struct SoundEntry {
    std::string path;
    std::string name;
    std::string folder;
    std::string root;
    std::string extension;
    bool video = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

struct FolderEntry {
    std::string path;
    std::string name;
    std::size_t sounds = 0;
    std::size_t folders = 0;
};

struct Crumb {
    std::string label;
    std::string path;
};

enum class SortOrder {
    name,
    duration,
    newest,
    played
};

SortOrder sort_order_from(const std::string& text);

bool is_sound_extension(const std::string& extension);
bool is_video_extension(const std::string& extension);
std::string lower_text(std::string s);
std::string display_name(const std::string& file_name);
std::string normalize_folder(const std::string& path);
bool path_inside(const std::string& path, const std::string& folder);

class Library {
public:
    void set_roots(const std::vector<std::string>& roots);
    std::vector<std::string> roots() const;

    void rescan();
    bool changed_on_disk() const;
    std::uint64_t revision() const;

    std::size_t sound_count() const;
    std::size_t folder_count() const;

    bool has_folder(const std::string& folder) const;
    std::vector<FolderEntry> subfolders(const std::string& folder) const;
    std::vector<SoundEntry> sounds_in(const std::string& folder) const;
    std::vector<SoundEntry> all_sounds() const;
    std::vector<SoundEntry> search(const std::string& query, std::size_t limit = 500) const;
    bool find(const std::string& path, SoundEntry& out) const;
    std::vector<Crumb> breadcrumbs(const std::string& folder) const;
    std::string parent_of(const std::string& folder) const;
    std::string root_of(const std::string& path) const;

    void set_duration(const std::string& path, double seconds);
    double duration(const std::string& path) const;
    std::vector<std::string> missing_durations(std::size_t limit) const;

    void sort(std::vector<SoundEntry>& sounds, SortOrder order, const std::map<std::string, int>& plays, const std::set<std::string>& favorites, bool favorites_first) const;

private:
    struct Folder {
        std::string name;
        std::string root;
        std::vector<std::string> children;
        std::vector<std::size_t> sounds;
        std::size_t total = 0;
    };

    void scan_root(const std::string& root, std::map<std::string, Folder>& folders, std::vector<SoundEntry>& sounds, std::map<std::string, std::int64_t>& stamps,
        std::set<std::string>& visited) const;
    std::size_t count_total(std::map<std::string, Folder>& folders, const std::string& path, int depth) const;

    mutable std::mutex m_mutex;
    std::vector<std::string> m_roots;
    std::map<std::string, Folder> m_folders;
    std::vector<SoundEntry> m_sounds;
    std::map<std::string, std::size_t> m_by_path;
    std::map<std::string, std::int64_t> m_stamps;
    struct Duration {
        std::int64_t modified = 0;
        std::uint64_t size = 0;
        double seconds = -1.0;
    };
    std::map<std::string, Duration> m_durations;
    std::uint64_t m_revision = 0;
};

} // namespace rustle

#endif // RUSTLE_LIBRARY_HPP
