#include <rustle/json.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace rustle {

Json json_string(const std::string& value) {
    return Json::make_string(value);
}

Json json_number(double value) {
    return Json::make_number(std::isfinite(value) ? value : 0.0);
}

Json json_bool(bool value) {
    return Json::make_bool(value);
}

Json json_strings(const std::vector<std::string>& values) {
    Json list = Json::make_array();
    for (const std::string& v : values) list.push(Json::make_string(v));
    return list;
}

std::string json_get_string(const Json& object, const std::string& key, const std::string& fallback) {
    const Json* v = object.is_object() ? object.find(key) : nullptr;
    if (!v || !v->is_string()) return fallback;
    return v->as_string();
}

double json_get_number(const Json& object, const std::string& key, double fallback) {
    const Json* v = object.is_object() ? object.find(key) : nullptr;
    if (!v) return fallback;
    if (v->is_number()) {
        const double d = v->as_number(fallback);
        return std::isfinite(d) ? d : fallback;
    }
    if (v->is_bool()) return v->as_bool() ? 1.0 : 0.0;
    return fallback;
}

bool json_get_bool(const Json& object, const std::string& key, bool fallback) {
    const Json* v = object.is_object() ? object.find(key) : nullptr;
    if (!v) return fallback;
    if (v->is_bool()) return v->as_bool();
    if (v->is_number()) return v->as_number() != 0.0;
    return fallback;
}

std::vector<std::string> json_get_strings(const Json& object, const std::string& key) {
    std::vector<std::string> out;
    const Json* v = object.is_object() ? object.find(key) : nullptr;
    if (!v || !v->is_array()) return out;
    for (std::size_t i = 0; i < v->size(); ++i) {
        if (v->at(i).is_string()) out.push_back(v->at(i).as_string());
    }
    return out;
}

bool read_text_file(const fs::path& path, std::string& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "can't open " + path.string();
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_text_file_atomic(const fs::path& path, const std::string& text, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "can't write " + temp.string();
            return false;
        }
        out << text;
        out.flush();
        if (!out) {
            if (error) *error = "couldn't finish writing " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(temp, path, ec);
        if (ec) {
            if (error) *error = "can't replace " + path.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace rustle
