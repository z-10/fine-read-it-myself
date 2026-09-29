// Fetch and parse novel pages using a Source definition (lexbor HTML parser + CSS selectors).
#pragma once

#include "sources.h"

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
};

// GET with the source's per-site politeness delay; throws on transport errors and non-2xx statuses
std::string fetch(const std::string & url, const Source & src);
NovelInfo parse_novel(const std::string & html, const std::string & url, const Source & src);
ChapterText parse_chapter(const std::string & html, const Source & src);

}  // namespace rm
