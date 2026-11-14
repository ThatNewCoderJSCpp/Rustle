#ifndef RUSTLE_MUSIC_HPP
#define RUSTLE_MUSIC_HPP

#include <rustle/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rustle {

enum class MusicKind {
    song,
    folder,
    group
};

enum class RepeatMode {
    off,
    all,
    one
};

std::string music_kind_name(MusicKind kind);
MusicKind music_kind_from(const std::string& text);
std::string repeat_name(RepeatMode mode);
RepeatMode repeat_from(const std::string& text);

struct MusicNode {
    std::uint64_t id = 0;
    MusicKind kind = MusicKind::song;
    std::string name;
    std::string path;
    bool played = false;
    std::vector<MusicNode> children;

    bool container() const { return kind != MusicKind::song; }
    std::size_t song_count() const;
    std::size_t played_count() const;
    std::size_t remaining() const;
};

bool is_music_file(const std::string& path);
bool natural_less(const std::string& a, const std::string& b);
MusicNode music_song(const std::string& path);
MusicNode music_group(const std::string& name);
bool expand_music_folder(const std::string& path, MusicNode& out, std::string* error = nullptr);
bool music_node_for_path(const std::string& path, MusicNode& out, std::string* error = nullptr);

Json music_node_json(const MusicNode& node, bool with_folder_items, bool with_played);
bool music_node_from_json(const Json& value, MusicNode& out);

using RandomBelow = std::function<std::uint64_t(std::uint64_t count)>;
std::uint64_t secure_random_below(std::uint64_t count);

class MusicQueue {
public:
    MusicQueue();

    void set_random(RandomBelow random);

    std::uint64_t add(MusicNode node);
    bool remove(std::uint64_t id);
    bool move(std::uint64_t id, int delta);
    void clear();
    std::size_t clear_played();
    void reset_played();

    const std::vector<MusicNode>& items() const { return m_items; }
    const MusicNode* find(std::uint64_t id) const;
    std::uint64_t parent_of(std::uint64_t id) const;
    bool empty() const { return m_items.empty(); }
    std::size_t song_count() const;
    std::size_t played_count() const;

    std::uint64_t current() const { return m_current; }
    bool has_forward() const;
    bool has_back() const;
    std::uint64_t next(bool shuffle, RepeatMode repeat);
    std::uint64_t previous();
    std::uint64_t jump(std::uint64_t id, bool shuffle);
    void forget_current();

    Json to_json() const;
    bool from_json(const Json& value);

private:
    MusicNode* find_mutable(std::uint64_t id);
    void assign_ids(MusicNode& node);
    void prune_history();
    void push_history(std::uint64_t id);
    MusicNode* pick_random(std::vector<MusicNode>& list);
    MusicNode* pick_in_order();
    MusicNode* first_unplayed(MusicNode& node);
    std::uint64_t start(MusicNode* song);

    std::vector<MusicNode> m_items;
    std::vector<std::uint64_t> m_history;
    std::size_t m_cursor = 0;
    std::uint64_t m_current = 0;
    std::uint64_t m_next_id = 1;
    RandomBelow m_random;
};

class Playlists {
public:
    const std::vector<MusicNode>& lists() const { return m_lists; }
    const MusicNode* find(std::uint64_t id) const;
    std::uint64_t parent_of(std::uint64_t id) const;
    bool is_playlist(std::uint64_t id) const;
    std::uint64_t find_by_name(const std::string& name) const;
    std::vector<std::pair<std::uint64_t, std::string>> crumbs(std::uint64_t id) const;

    std::uint64_t create(const std::string& name, std::string* error = nullptr);
    bool rename(std::uint64_t id, const std::string& name, std::string* error = nullptr);
    bool remove(std::uint64_t id);
    bool move(std::uint64_t id, int delta);
    std::uint64_t add_group(std::uint64_t parent, const std::string& name, std::string* error = nullptr);
    std::uint64_t add_path(std::uint64_t parent, const std::string& path, std::string* error = nullptr);

    bool expanded(std::uint64_t id, MusicNode& out, std::string* error = nullptr) const;

    Json to_json() const;
    bool from_json(const Json& value);

private:
    MusicNode* find_mutable(std::uint64_t id);
    void assign_ids(MusicNode& node);

    std::vector<MusicNode> m_lists;
    std::uint64_t m_next_id = 1;
};

} // namespace rustle

#endif // RUSTLE_MUSIC_HPP
