#include "library.h"

#include "text.h"

#include <stdexcept>

namespace rm {

json Library::supported() const {
    json out = json::array();
    for (const auto & [_, s] : sources_) out.push_back({{"id", s.id}, {"name", s.name}, {"homepage", s.homepage},
        {"login", s.fetch.value("login", false)}, {"delay_seconds", s.fetch.value("delay_seconds", 1.0)}});
    return out;
}

json Library::add(const std::string & url, const std::string & narrator, const std::string & director) {
    const auto hit = resolve(url, sources_);
    if (!hit) {
        std::string names;
        for (const auto & [_, s] : sources_) names += (names.empty() ? "" : ", ") + s.name;
        throw std::invalid_argument("No source matches this URL. Supported: " + names);
    }
    const Source & src = *hit->first;
    const std::string key = src.novel_key(hit->second);
    const std::string novel_url = src.novel_url(hit->second);
    // Also find novels stored with older plugin identity rules, preserving their chapters and audio.
    if (auto existing = db_.one("SELECT * FROM novels WHERE source_key=? OR (source=? AND url=?)", {key, src.id, novel_url})) {
        db_.run("UPDATE novels SET source_key=? WHERE id=?", {key, (*existing)["id"]});
        (*existing)["source_key"] = key;
        refresh((*existing)["id"].get<int64_t>());
        return *existing;
    }
    const NovelInfo info = parse_novel(fetch(novel_url, src), novel_url, src);
    if (info.chapters.empty())
        throw std::runtime_error(src.name + ": no chapters found; the site may have blocked the request or changed its layout");
    const int64_t nid = db_.run(
        "INSERT INTO novels (source, source_key, url, title, author, cover, description, narrator, director, added_at, "
        "checked_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        {src.id, key, novel_url, info.title.empty() ? novel_url : info.title, info.author, info.cover, info.description,
         narrator, director, now(), now()});
    store_chapters(nid, info.chapters);
    return *db_.one("SELECT * FROM novels WHERE id=?", {nid});
}

int Library::refresh(int64_t novel_id) {
    const auto n = db_.one("SELECT * FROM novels WHERE id=?", {novel_id});
    if (!n) throw std::invalid_argument("unknown novel");
    const Source & src = sources_.at((*n)["source"].get<std::string>());
    const std::string url = (*n)["url"].get<std::string>();
    const NovelInfo info = parse_novel(fetch(url, src), url, src);
    const auto count = [&] { return (*db_.one("SELECT COUNT(*) c FROM chapters WHERE novel_id=?", {novel_id}))["c"].get<int>(); };
    const int before = count();
    store_chapters(novel_id, info.chapters);
    db_.run("UPDATE novels SET checked_at=? WHERE id=?", {now(), novel_id});
    return count() - before;
}

void Library::store_chapters(int64_t novel_id, const std::vector<ChapterRef> & chapters) {
    db_.tx([&] {
        for (const auto & ch : chapters)
            db_.run("INSERT INTO chapters (novel_id, position, url, title, published) VALUES (?,?,?,?,?) "
                    "ON CONFLICT (novel_id, url) DO UPDATE SET position=excluded.position, "
                    "title=excluded.title, published=excluded.published",
                    {novel_id, ch.index, ch.url, ch.title, ch.published ? json(*ch.published) : json(nullptr)});
    });
}

std::vector<json> Library::novels() {
    return db_.all(
        "SELECT n.*, (SELECT COUNT(*) FROM chapters WHERE novel_id=n.id) AS chapters, "
        "(SELECT COUNT(*) FROM chapters WHERE novel_id=n.id AND status='done') AS narrated "
        "FROM novels n ORDER BY n.added_at DESC");
}

std::vector<json> Library::chapters(int64_t novel_id) {
    auto rows = db_.all("SELECT * FROM chapters WHERE novel_id=? ORDER BY position", {novel_id});
    for (auto & r : rows) r["story"] = looks_like_chapter(r["title"].get<std::string>());
    return rows;
}

}  // namespace rm
