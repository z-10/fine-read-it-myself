// Novels and chapters: add by URL, refresh chapter lists, find new chapters.
#pragma once

#include "db.h"
#include "scrape.h"
#include "sources.h"

namespace rm {

class Library {
public:
    Library(DB & db, const Sources & sources) : db_(db), sources_(sources) {}
    json supported() const;
    // throws std::invalid_argument when no source matches
    json add(const std::string & url, const std::string & narrator, const std::string & director);
    int refresh(int64_t novel_id);   // number of new chapters
    std::vector<json> novels();
    std::vector<json> chapters(int64_t novel_id);

private:
    void store_chapters(int64_t novel_id, const std::vector<ChapterRef> & chapters);
    DB & db_;
    const Sources & sources_;
};

}  // namespace rm
