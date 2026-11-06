#ifndef RUSTLE_FILES_HPP
#define RUSTLE_FILES_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace rustle {

struct FileItem {
    std::string name;
    std::string path;
    bool folder = false;
    bool sound = false;
    bool hidden = false;
    std::uint64_t size = 0;
};

struct Place {
    std::string label;
    std::string path;
    std::string kind;
};

std::string home_folder();
std::vector<Place> places();
std::vector<Place> mounted_drives();
std::vector<Place> parse_mounts(const std::string& mounts_text, const std::string& home);
std::vector<Place> parse_user_dirs(const std::string& text, const std::string& home);

bool list_folder(const std::string& path, bool show_hidden, std::vector<FileItem>& out, std::string* error = nullptr);
std::string parent_folder(const std::string& path);
std::vector<std::pair<std::string, std::string>> path_parts(const std::string& path);
std::string format_size(std::uint64_t bytes);

std::string unique_destination(const std::string& folder, const std::string& file_name);
bool copy_into(const std::vector<std::string>& files, const std::string& folder, std::vector<std::string>& copied, std::string* error = nullptr);
bool make_folder(const std::string& parent, const std::string& name, std::string& made, std::string* error = nullptr);

} // namespace rustle

#endif // RUSTLE_FILES_HPP
