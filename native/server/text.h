// Deterministic chapter splitting: paragraphs -> narration / quoted-dialogue spans (directors only label
// spans by id; they never rewrite text), plus small text helpers.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace rm {

struct Span {
    int id, para;
    std::string kind;   // "narration" | "dialogue"
    std::string text;
};

std::vector<Span> split_spans(const std::vector<std::string> & paragraphs);
nlohmann::json spans_json(const std::vector<Span> & spans);
// chapter as `[P<n>] {S<id>:N|D} text ...` for LLM directors
std::string spans_markup(const std::string & title, const std::vector<Span> & spans);
// heuristic flag for non-story entries (announcements, Q&A, hiatus notes)
bool looks_like_chapter(const std::string & title);
// lower-case words [a-z0-9']+ joined by spaces (’ counts as ')
std::string norm_words(const std::string & t);
// difflib.SequenceMatcher(None, a, b).ratio()
double seq_ratio(const std::string & a, const std::string & b);
// Python str.strip() on UTF-8 (unicode whitespace)
std::string strip(const std::string & s);
bool has_word_char(const std::string & s);   // re.search(r"\w", s)

}  // namespace rm
