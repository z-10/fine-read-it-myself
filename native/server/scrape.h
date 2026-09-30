// Fetch and parse novel pages using a Source definition (lexbor HTML parser + CSS selectors).
#pragma once

#include "sources.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rm {

struct ChapterRef {
    int index;
    std::string url, title;
    std::optional<int64_t> published;
};

struct NovelInfo {
    std::string url, title, author, cover, description;
    std::vector<ChapterRef> chapters;
};

struct ChapterText {
    std::string title;
    std::vector<std::string> paragraphs;
    bool locked = false;   // paid/unlockable on the site: the page only has a preview
};

// Cookie header to send per source id: the user's own login on that site (Settings -> Sites)
void set_site_cookies(const std::map<std::string, std::string> & by_source);

// GET with the source's per-site politeness delay; throws on transport errors and non-2xx statuses
std::string fetch(const std::string & url, const Source & src);
NovelInfo parse_novel(const std::string & html, const std::string & url, const Source & src);
ChapterText parse_chapter(const std::string & html, const Source & src);

}  // namespace rm
