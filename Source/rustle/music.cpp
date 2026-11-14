#include <rustle/music.hpp>
#include <rustle/library.hpp>

#include <../Source/Random/random_std_int.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace rustle {

namespace {

constexpr int k_max_depth = 40;
constexpr std::size_t k_max_songs = 50000;

MusicNode* find_in(std::vector<MusicNode>& list, std::uint64_t id) {
    for (MusicNode& n : list) {
        if (n.id == id) return &n;
        if (MusicNode* hit = find_in(n.children, id)) return hit;
    }
    return nullptr;
}

const MusicNode* find_in(const std::vector<MusicNode>& list, std::uint64_t id) {
    for (const MusicNode& n : list) {
        if (n.id == id) return &n;
        if (const MusicNode* hit = find_in(n.children, id)) return hit;
    }
    return nullptr;
}

bool parent_in(const std::vector<MusicNode>& list, std::uint64_t id, std::uint64_t parent, std::uint64_t& out) {
    for (const MusicNode& n : list) {
        if (n.id == id) {
            out = parent;
            return true;
        }
        if (parent_in(n.children, id, n.id, out)) return true;
    }
    return false;
}

std::vector<MusicNode>* siblings_of(std::vector<MusicNode>& list, std::uint64_t id) {
    for (MusicNode& n : list) {
        if (n.id == id) return &list;
    }
    for (MusicNode& n : list) {
        if (std::vector<MusicNode>* hit = siblings_of(n.children, id)) return hit;
    }
    return nullptr;
}

bool erase_in(std::vector<MusicNode>& list, std::uint64_t id) {
    std::vector<MusicNode>* sibs = siblings_of(list, id);
    if (!sibs) return false;
    auto it = std::find_if(sibs->begin(), sibs->end(), [id](const MusicNode& n) { return n.id == id; });
    if (it == sibs->end()) return false;
    sibs->erase(it);
    return true;
}

bool move_in(std::vector<MusicNode>& list, std::uint64_t id, int delta) {
    std::vector<MusicNode>* sibs = siblings_of(list, id);
    if (!sibs || delta == 0) return false;
    auto it = std::find_if(sibs->begin(), sibs->end(), [id](const MusicNode& n) { return n.id == id; });
    const long from = static_cast<long>(it - sibs->begin());
    const long to = std::clamp(from + static_cast<long>(delta), 0L, static_cast<long>(sibs->size()) - 1L);
    if (to == from) return false;
    MusicNode moving = std::move((*sibs)[static_cast<std::size_t>(from)]);
    sibs->erase(sibs->begin() + from);
    sibs->insert(sibs->begin() + to, std::move(moving));
    return true;
}

void collect_songs(std::vector<MusicNode>& list, std::vector<MusicNode*>& out) {
    for (MusicNode& n : list) {
        if (n.kind == MusicKind::song) out.push_back(&n);
        else collect_songs(n.children, out);
    }
}

void set_played(std::vector<MusicNode>& list, bool played) {
    for (MusicNode& n : list) {
        n.played = played && n.kind == MusicKind::song;
        set_played(n.children, played);
    }
}

std::size_t drop_played(std::vector<MusicNode>& list, std::uint64_t keep) {
    std::size_t dropped = 0;
    for (auto it = list.begin(); it != list.end();) {
        if (it->kind == MusicKind::song) {
            if (it->played && it->id != keep) {
                it = list.erase(it);
                ++dropped;
                continue;
            }
        } else {
            dropped += drop_played(it->children, keep);
            if (it->song_count() == 0) {
                it = list.erase(it);
                continue;
            }
        }
        ++it;
    }
    return dropped;
}

void prune_empty(std::vector<MusicNode>& list) {
    for (auto it = list.begin(); it != list.end();) {
        if (it->container()) {
            prune_empty(it->children);
            if (it->song_count() == 0) {
                it = list.erase(it);
                continue;
            }
        }
        ++it;
    }
}

bool index_path(const std::vector<MusicNode>& list, std::uint64_t id, std::vector<std::size_t>& path) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        path.push_back(i);
        if (list[i].id == id) return true;
        if (index_path(list[i].children, id, path)) return true;
        path.pop_back();
    }
    return false;
}

std::string trim_text(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string folder_label(const std::string& path) {
    const fs::path p(normalize_folder(path));
    std::string name = p.filename().string();
    return name.empty() ? p.string() : name;
}

bool expand_into(const fs::path& folder, MusicNode& node, int depth, std::set<std::string>& visited, std::size_t& total) {
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(folder, ec);
    if (!visited.insert(ec ? folder.string() : canon.string()).second) return false;
    std::vector<fs::directory_entry> entries;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        entries.push_back(*it);
    }
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return natural_less(a.path().filename().string(), b.path().filename().string());
    });
    std::vector<MusicNode> songs;
    std::vector<MusicNode> folders;
    for (const fs::directory_entry& e : entries) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code dec;
        if (e.is_directory(dec)) {
            if (depth + 1 > k_max_depth) continue;
            MusicNode sub;
            sub.kind = MusicKind::folder;
            sub.name = name;
            sub.path = e.path().string();
            if (expand_into(e.path(), sub, depth + 1, visited, total) && sub.song_count() > 0) folders.push_back(std::move(sub));
            continue;
        }
        if (!e.is_regular_file(dec) || !is_music_file(e.path().string())) continue;
        if (total >= k_max_songs) continue;
        songs.push_back(music_song(e.path().string()));
        ++total;
    }
    node.children = std::move(songs);
    for (MusicNode& f : folders) node.children.push_back(std::move(f));
    return true;
}

bool expand_folders(MusicNode& node, std::string* error) {
    if (node.kind == MusicKind::folder) {
        const std::string path = node.path;
        const std::string name = node.name;
        MusicNode fresh;
        if (!expand_music_folder(path, fresh, error)) return false;
        if (!name.empty()) fresh.name = name;
        node = std::move(fresh);
        return true;
    }
    for (auto it = node.children.begin(); it != node.children.end();) {
        std::string why;
        if (!expand_folders(*it, &why) || (it->container() && it->song_count() == 0)) {
            if (error && error->empty() && !why.empty()) *error = why;
            it = node.children.erase(it);
            continue;
        }
        ++it;
    }
    return true;
}

} // namespace

std::string music_kind_name(MusicKind kind) {
    switch (kind) {
    case MusicKind::folder:
        return "folder";
    case MusicKind::group:
        return "group";
    case MusicKind::song:
        break;
    }
    return "song";
}

MusicKind music_kind_from(const std::string& text) {
    if (text == "folder") return MusicKind::folder;
    if (text == "group" || text == "playlist") return MusicKind::group;
    return MusicKind::song;
}

std::string repeat_name(RepeatMode mode) {
    switch (mode) {
    case RepeatMode::all:
        return "all";
    case RepeatMode::one:
        return "one";
    case RepeatMode::off:
        break;
    }
    return "off";
}

RepeatMode repeat_from(const std::string& text) {
    const std::string t = lower_text(trim_text(text));
    if (t == "all" || t == "queue" || t == "on") return RepeatMode::all;
    if (t == "one" || t == "song" || t == "track") return RepeatMode::one;
    return RepeatMode::off;
}

std::size_t MusicNode::song_count() const {
    if (kind == MusicKind::song) return 1;
    std::size_t n = 0;
    for (const MusicNode& c : children) n += c.song_count();
    return n;
}

std::size_t MusicNode::played_count() const {
    if (kind == MusicKind::song) return played ? 1 : 0;
    std::size_t n = 0;
    for (const MusicNode& c : children) n += c.played_count();
    return n;
}

std::size_t MusicNode::remaining() const {
    return song_count() - played_count();
}

bool is_music_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    return is_sound_extension(ext);
}

bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            std::size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            std::size_t si = i, sj = j;
            while (si + 1 < ei && a[si] == '0') ++si;
            while (sj + 1 < ej && b[sj] == '0') ++sj;
            const std::size_t la = ei - si, lb = ej - sj;
            if (la != lb) return la < lb;
            const int c = a.compare(si, la, b, sj, lb);
            if (c != 0) return c < 0;
            i = ei;
            j = ej;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
    return a < b;
}

MusicNode music_song(const std::string& path) {
    MusicNode n;
    n.kind = MusicKind::song;
    n.path = path;
    n.name = display_name(fs::path(path).filename().string());
    return n;
}

MusicNode music_group(const std::string& name) {
    MusicNode n;
    n.kind = MusicKind::group;
    n.name = name;
    return n;
}

bool expand_music_folder(const std::string& path, MusicNode& out, std::string* error) {
    const std::string folder = normalize_folder(path);
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) {
        if (error) *error = path + " isn't a folder";
        return false;
    }
    MusicNode node;
    node.kind = MusicKind::folder;
    node.path = folder;
    node.name = folder_label(folder);
    std::set<std::string> visited;
    std::size_t total = 0;
    expand_into(folder, node, 0, visited, total);
    if (node.song_count() == 0) {
        if (error) *error = "there's no music in " + node.name;
        return false;
    }
    out = std::move(node);
    return true;
}

bool music_node_for_path(const std::string& path, MusicNode& out, std::string* error) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return expand_music_folder(path, out, error);
    if (!fs::is_regular_file(path, ec)) {
        if (error) *error = fs::path(path).filename().string() + " isn't there";
        return false;
    }
    if (!is_music_file(path)) {
        if (error) *error = fs::path(path).filename().string() + " isn't a sound file Rustle can play";
        return false;
    }
    out = music_song(path);
    return true;
}

Json music_node_json(const MusicNode& node, bool with_folder_items, bool with_played) {
    Json o = Json::make_object();
    o.set("kind", json_string(music_kind_name(node.kind)));
    if (!node.name.empty()) o.set("name", json_string(node.name));
    if (!node.path.empty()) o.set("path", json_string(node.path));
    if (with_played && node.kind == MusicKind::song && node.played) o.set("played", json_bool(true));
    if (node.kind == MusicKind::group || (node.kind == MusicKind::folder && with_folder_items)) {
        Json items = Json::make_array();
        for (const MusicNode& c : node.children) items.push(music_node_json(c, with_folder_items, with_played));
        o.set("items", std::move(items));
    }
    return o;
}

bool music_node_from_json(const Json& value, MusicNode& out) {
    if (!value.is_object()) return false;
    MusicNode n;
    n.kind = music_kind_from(json_get_string(value, "kind", "song"));
    n.name = json_get_string(value, "name");
    n.path = json_get_string(value, "path");
    n.played = n.kind == MusicKind::song && json_get_bool(value, "played", false);
    if (n.kind != MusicKind::group && n.path.empty()) return false;
    if (n.kind == MusicKind::song && n.name.empty()) n.name = display_name(fs::path(n.path).filename().string());
    if (n.kind == MusicKind::folder && n.name.empty()) n.name = folder_label(n.path);
    if (const Json* items = value.find("items")) {
        if (items->is_array()) {
            for (std::size_t i = 0; i < items->size(); ++i) {
                MusicNode child;
                if (music_node_from_json(items->at(i), child)) n.children.push_back(std::move(child));
            }
        }
    }
    out = std::move(n);
    return true;
}

std::uint64_t secure_random_below(std::uint64_t count) {
    if (count <= 1) return 0;
    try {
        return fizmo::random_int<std::uint64_t>(0, count - 1);
    } catch (...) {
        return fizmo::unsecure_random_int<std::uint64_t>(0, count - 1);
    }
}

MusicQueue::MusicQueue() : m_random(secure_random_below) {}

void MusicQueue::set_random(RandomBelow random) {
    m_random = random ? std::move(random) : RandomBelow(secure_random_below);
}

void MusicQueue::assign_ids(MusicNode& node) {
    node.id = m_next_id++;
    for (MusicNode& c : node.children) assign_ids(c);
}

std::uint64_t MusicQueue::add(MusicNode node) {
    if (node.container() && node.song_count() == 0) return 0;
    assign_ids(node);
    const std::uint64_t id = node.id;
    m_items.push_back(std::move(node));
    return id;
}

MusicNode* MusicQueue::find_mutable(std::uint64_t id) {
    return id ? find_in(m_items, id) : nullptr;
}

const MusicNode* MusicQueue::find(std::uint64_t id) const {
    return id ? find_in(m_items, id) : nullptr;
}

std::uint64_t MusicQueue::parent_of(std::uint64_t id) const {
    std::uint64_t out = 0;
    parent_in(m_items, id, 0, out);
    return out;
}

void MusicQueue::prune_history() {
    std::vector<std::uint64_t> kept;
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < m_history.size(); ++i) {
        if (!find(m_history[i])) continue;
        if (i <= m_cursor) cursor = kept.size();
        kept.push_back(m_history[i]);
    }
    m_history = std::move(kept);
    m_cursor = m_history.empty() ? 0 : std::min(cursor, m_history.size() - 1);
    if (m_current && !find(m_current)) m_current = 0;
}

bool MusicQueue::remove(std::uint64_t id) {
    if (!erase_in(m_items, id)) return false;
    prune_empty(m_items);
    prune_history();
    return true;
}

bool MusicQueue::move(std::uint64_t id, int delta) {
    return move_in(m_items, id, delta);
}

void MusicQueue::clear() {
    m_items.clear();
    m_history.clear();
    m_cursor = 0;
    m_current = 0;
}

std::size_t MusicQueue::clear_played() {
    const std::size_t n = drop_played(m_items, m_current);
    prune_history();
    return n;
}

void MusicQueue::reset_played() {
    set_played(m_items, false);
}

std::size_t MusicQueue::song_count() const {
    std::size_t n = 0;
    for (const MusicNode& i : m_items) n += i.song_count();
    return n;
}

std::size_t MusicQueue::played_count() const {
    std::size_t n = 0;
    for (const MusicNode& i : m_items) n += i.played_count();
    return n;
}

bool MusicQueue::has_forward() const {
    return !m_history.empty() && m_cursor + 1 < m_history.size();
}

bool MusicQueue::has_back() const {
    return !m_history.empty() && m_cursor > 0;
}

void MusicQueue::push_history(std::uint64_t id) {
    if (!m_history.empty() && m_cursor + 1 < m_history.size()) m_history.resize(m_cursor + 1);
    if (m_history.empty() || m_history.back() != id) m_history.push_back(id);
    while (m_history.size() > 500) m_history.erase(m_history.begin());
    m_cursor = m_history.size() - 1;
}

std::uint64_t MusicQueue::start(MusicNode* song) {
    if (!song) return 0;
    song->played = true;
    m_current = song->id;
    push_history(song->id);
    return song->id;
}

MusicNode* MusicQueue::pick_random(std::vector<MusicNode>& list) {
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const MusicNode& n = list[i];
        if (n.kind == MusicKind::song ? !n.played : n.remaining() > 0) open.push_back(i);
    }
    if (open.empty()) return nullptr;
    const std::uint64_t r = m_random(static_cast<std::uint64_t>(open.size()));
    MusicNode& chosen = list[open[static_cast<std::size_t>(std::min<std::uint64_t>(r, open.size() - 1))]];
    if (chosen.kind == MusicKind::song) return &chosen;
    return pick_random(chosen.children);
}

MusicNode* MusicQueue::pick_in_order() {
    std::vector<MusicNode*> songs;
    collect_songs(m_items, songs);
    if (songs.empty()) return nullptr;
    std::size_t from = 0;
    for (std::size_t i = 0; i < songs.size(); ++i) {
        if (songs[i]->id == m_current) {
            from = i + 1;
            break;
        }
    }
    for (std::size_t k = 0; k < songs.size(); ++k) {
        MusicNode* s = songs[(from + k) % songs.size()];
        if (!s->played) return s;
    }
    return nullptr;
}

MusicNode* MusicQueue::first_unplayed(MusicNode& node) {
    if (node.kind == MusicKind::song) return node.played ? nullptr : &node;
    for (MusicNode& c : node.children) {
        if (MusicNode* hit = first_unplayed(c)) return hit;
    }
    return nullptr;
}

std::uint64_t MusicQueue::next(bool shuffle, RepeatMode repeat) {
    if (has_forward()) {
        ++m_cursor;
        m_current = m_history[m_cursor];
        return m_current;
    }
    MusicNode* pick = shuffle ? pick_random(m_items) : pick_in_order();
    if (!pick) {
        if (repeat == RepeatMode::off) return 0;
        reset_played();
        MusicNode* last = find_mutable(m_current);
        const bool avoid = shuffle && last && song_count() > 1;
        if (avoid) last->played = true;
        pick = shuffle ? pick_random(m_items) : pick_in_order();
        if (avoid) last->played = false;
        if (!shuffle && !pick) {
            std::vector<MusicNode*> songs;
            collect_songs(m_items, songs);
            if (!songs.empty()) pick = songs.front();
        }
    }
    return start(pick);
}

std::uint64_t MusicQueue::previous() {
    if (!has_back()) return 0;
    --m_cursor;
    m_current = m_history[m_cursor];
    return m_current;
}

std::uint64_t MusicQueue::jump(std::uint64_t id, bool shuffle) {
    MusicNode* node = find_mutable(id);
    if (!node) return 0;
    if (node->kind == MusicKind::song) return start(node);
    if (node->remaining() == 0) set_played(node->children, false);
    MusicNode* pick = shuffle ? pick_random(node->children) : first_unplayed(*node);
    return start(pick);
}

void MusicQueue::forget_current() {
    m_current = 0;
}

Json MusicQueue::to_json() const {
    Json o = Json::make_object();
    Json items = Json::make_array();
    for (const MusicNode& n : m_items) items.push(music_node_json(n, true, true));
    o.set("items", std::move(items));
    std::vector<std::size_t> path;
    if (m_current && index_path(m_items, m_current, path)) {
        Json p = Json::make_array();
        for (std::size_t i : path) p.push(json_number(static_cast<double>(i)));
        o.set("current", std::move(p));
    }
    return o;
}

bool MusicQueue::from_json(const Json& value) {
    if (!value.is_object()) return false;
    clear();
    if (const Json* items = value.find("items")) {
        if (items->is_array()) {
            for (std::size_t i = 0; i < items->size(); ++i) {
                MusicNode n;
                if (music_node_from_json(items->at(i), n)) add(std::move(n));
            }
        }
    }
    if (const Json* cur = value.find("current")) {
        if (cur->is_array()) {
            const std::vector<MusicNode>* list = &m_items;
            const MusicNode* at = nullptr;
            for (std::size_t i = 0; i < cur->size(); ++i) {
                const double d = cur->at(i).as_number(-1.0);
                if (d < 0.0 || static_cast<std::size_t>(d) >= list->size()) {
                    at = nullptr;
                    break;
                }
                at = &(*list)[static_cast<std::size_t>(d)];
                list = &at->children;
            }
            if (at && at->kind == MusicKind::song) {
                m_current = at->id;
                m_history.push_back(at->id);
                m_cursor = 0;
            }
        }
    }
    return true;
}

MusicNode* Playlists::find_mutable(std::uint64_t id) {
    return id ? find_in(m_lists, id) : nullptr;
}

const MusicNode* Playlists::find(std::uint64_t id) const {
    return id ? find_in(m_lists, id) : nullptr;
}

std::uint64_t Playlists::parent_of(std::uint64_t id) const {
    std::uint64_t out = 0;
    parent_in(m_lists, id, 0, out);
    return out;
}

bool Playlists::is_playlist(std::uint64_t id) const {
    for (const MusicNode& p : m_lists) {
        if (p.id == id) return true;
    }
    return false;
}

std::uint64_t Playlists::find_by_name(const std::string& name) const {
    const std::string want = lower_text(trim_text(name));
    if (want.empty()) return 0;
    for (const MusicNode& p : m_lists) {
        if (lower_text(p.name) == want) return p.id;
    }
    for (const MusicNode& p : m_lists) {
        if (lower_text(p.name).find(want) != std::string::npos) return p.id;
    }
    return 0;
}

std::vector<std::pair<std::uint64_t, std::string>> Playlists::crumbs(std::uint64_t id) const {
    std::vector<std::pair<std::uint64_t, std::string>> out;
    for (std::uint64_t at = id; at;) {
        const MusicNode* n = find(at);
        if (!n) break;
        out.emplace_back(n->id, n->name);
        at = parent_of(at);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void Playlists::assign_ids(MusicNode& node) {
    node.id = m_next_id++;
    for (MusicNode& c : node.children) assign_ids(c);
}

std::uint64_t Playlists::create(const std::string& raw, std::string* error) {
    const std::string name = trim_text(raw);
    if (name.empty()) {
        if (error) *error = "give the playlist a name";
        return 0;
    }
    for (const MusicNode& p : m_lists) {
        if (lower_text(p.name) == lower_text(name)) {
            if (error) *error = "there's already a playlist called " + p.name;
            return 0;
        }
    }
    MusicNode p = music_group(name);
    assign_ids(p);
    const std::uint64_t id = p.id;
    m_lists.push_back(std::move(p));
    return id;
}

bool Playlists::rename(std::uint64_t id, const std::string& raw, std::string* error) {
    MusicNode* n = find_mutable(id);
    const std::string name = trim_text(raw);
    if (!n) {
        if (error) *error = "that isn't in a playlist any more";
        return false;
    }
    if (name.empty()) {
        if (error) *error = "the name can't be empty";
        return false;
    }
    if (is_playlist(id)) {
        for (const MusicNode& p : m_lists) {
            if (p.id != id && lower_text(p.name) == lower_text(name)) {
                if (error) *error = "there's already a playlist called " + p.name;
                return false;
            }
        }
    }
    n->name = name;
    return true;
}

bool Playlists::remove(std::uint64_t id) {
    return erase_in(m_lists, id);
}

bool Playlists::move(std::uint64_t id, int delta) {
    return move_in(m_lists, id, delta);
}

std::uint64_t Playlists::add_group(std::uint64_t parent, const std::string& raw, std::string* error) {
    MusicNode* p = find_mutable(parent);
    const std::string name = trim_text(raw);
    if (!p || p->kind != MusicKind::group) {
        if (error) *error = "pick a playlist or one of its folders first";
        return 0;
    }
    if (name.empty()) {
        if (error) *error = "give the folder a name";
        return 0;
    }
    MusicNode g = music_group(name);
    assign_ids(g);
    const std::uint64_t id = g.id;
    p->children.push_back(std::move(g));
    return id;
}

std::uint64_t Playlists::add_path(std::uint64_t parent, const std::string& path, std::string* error) {
    MusicNode* p = find_mutable(parent);
    if (!p || p->kind != MusicKind::group) {
        if (error) *error = "pick a playlist or one of its folders first";
        return 0;
    }
    std::error_code ec;
    MusicNode n;
    if (fs::is_directory(path, ec)) {
        MusicNode probe;
        if (!expand_music_folder(path, probe, error)) return 0;
        n.kind = MusicKind::folder;
        n.path = probe.path;
        n.name = probe.name;
    } else if (!music_node_for_path(path, n, error)) {
        return 0;
    }
    for (const MusicNode& c : p->children) {
        if (c.kind == n.kind && !c.path.empty() && c.path == n.path) {
            if (error) *error = n.name + " is already in " + p->name;
            return 0;
        }
    }
    assign_ids(n);
    const std::uint64_t id = n.id;
    p->children.push_back(std::move(n));
    return id;
}

bool Playlists::expanded(std::uint64_t id, MusicNode& out, std::string* error) const {
    const MusicNode* n = find(id);
    if (!n) {
        if (error) *error = "that playlist isn't there any more";
        return false;
    }
    MusicNode copy = *n;
    std::string why;
    if (!expand_folders(copy, &why)) {
        if (error) *error = why;
        return false;
    }
    if (copy.song_count() == 0) {
        if (error) *error = (copy.kind == MusicKind::song ? copy.name : copy.name + " has no music in it") + (why.empty() ? std::string() : " (" + why + ")");
        return false;
    }
    set_played(copy.children, false);
    copy.played = false;
    out = std::move(copy);
    return true;
}

Json Playlists::to_json() const {
    Json o = Json::make_object();
    o.set("format", json_string("rustle-playlists"));
    o.set("version", json_number(1));
    Json lists = Json::make_array();
    for (const MusicNode& p : m_lists) lists.push(music_node_json(p, false, false));
    o.set("playlists", std::move(lists));
    return o;
}

bool Playlists::from_json(const Json& value) {
    if (!value.is_object()) return false;
    m_lists.clear();
    if (const Json* lists = value.find("playlists")) {
        if (lists->is_array()) {
            for (std::size_t i = 0; i < lists->size(); ++i) {
                MusicNode n;
                if (!music_node_from_json(lists->at(i), n) || n.kind != MusicKind::group) continue;
                if (n.name.empty()) n.name = "Playlist " + std::to_string(m_lists.size() + 1);
                assign_ids(n);
                m_lists.push_back(std::move(n));
            }
        }
    }
    return true;
}

} // namespace rustle
