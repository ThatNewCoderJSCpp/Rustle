#ifndef RUSTLE_JSON_HPP
#define RUSTLE_JSON_HPP

#include <audio.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rustle {

using Json = fizmo::audio::JsonValue;

Json json_string(const std::string& value);
Json json_number(double value);
Json json_bool(bool value);
Json json_strings(const std::vector<std::string>& values);

std::string json_get_string(const Json& object, const std::string& key, const std::string& fallback = std::string());
double json_get_number(const Json& object, const std::string& key, double fallback);
bool json_get_bool(const Json& object, const std::string& key, bool fallback);
std::vector<std::string> json_get_strings(const Json& object, const std::string& key);

bool read_text_file(const std::filesystem::path& path, std::string& out, std::string* error = nullptr);
bool write_text_file_atomic(const std::filesystem::path& path, const std::string& text, std::string* error = nullptr);

} // namespace rustle

#endif // RUSTLE_JSON_HPP
