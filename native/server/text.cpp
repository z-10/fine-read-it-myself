#include "text.h"

#include "tokenizer.h"   // utf8 <-> u32, py_isspace (booknlp)
#include "unicode.h"     // unicode_cpt_flags_from_cpt (llama)

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <regex>
#include <unordered_map>

namespace rm {

using booknlp::py_isspace;
using booknlp::u32_to_utf8;
using booknlp::utf8_to_u32;

std::string strip(const std::string & s) {
    const auto u = utf8_to_u32(s);
    size_t a = 0, b = u.size();
    while (a < b && py_isspace(u[a])) ++a;
    while (b > a && py_isspace(u[b - 1])) --b;
    return u32_to_utf8(u.substr(a, b - a));
}

bool has_word_char(const std::string & s) {
    for (char32_t c : utf8_to_u32(s)) {
        const auto f = unicode_cpt_flags_from_cpt(c);
        if (c == U'_' || f.is_letter || f.is_number) return true;
    }
    return false;
}

// A quote inside a running sentence is a quoted term, not speech: 'just as "random-number-generator" as everything',
// 'its "legs", made of'. The text before it ends in a word (no ',' / ':' introducing speech) and the sentence goes on
// after it in lower case or with a comma. Such quotes stay in the narration.
static bool embedded_quote(const std::u32string & p, size_t pos, size_t open, size_t end, const std::u32string & inner) {
    if (end >= p.size() || (p[end - 1] != U'”' && p[end - 1] != U'"')) return false;   // unclosed: speech running on
    size_t b = open;
    while (b > pos && py_isspace(p[b - 1])) --b;
    if (b == pos) return false;   // the quote opens the paragraph (or follows another quote)
    const auto fb = unicode_cpt_flags_from_cpt(p[b - 1]);
    if (!(fb.is_letter || fb.is_number)) return false;
    size_t a = end;
    while (a < p.size() && py_isspace(p[a])) ++a;
    if (a == p.size()) return false;
    const char32_t n = p[a];
    const auto fa = unicode_cpt_flags_from_cpt(n);
    const bool goes_on = fa.is_lowercase || n == U',' || n == U';' || n == U')';
    if (!goes_on) return false;
    // exclaimed or trailing off: speech ('a chorus of "Cute!" from')
    size_t q = inner.size();
    while (q > 0 && py_isspace(inner[q - 1])) --q;
    if (q > 0 && (inner[q - 1] == U'!' || inner[q - 1] == U'?' || inner[q - 1] == U'…' || inner[q - 1] == U'.')) return false;
    // introduced by a speech verb ('said "No", ...')
    size_t w0 = b;
    while (w0 > pos && unicode_cpt_flags_from_cpt(p[w0 - 1]).is_letter) --w0;
    std::string verb = u32_to_utf8(p.substr(w0, b - w0));
    std::transform(verb.begin(), verb.end(), verb.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::regex speech(R"((say|said|says|saying|ask|asked|asking|whisper\w*|shout\w*|yell\w*|mutter\w*|mumbl\w*|scream\w*|cr(y|ied|ies|ying)|call\w*|repl\w*|answer\w*|exclaim\w*|murmur\w*|growl\w*|hiss\w*|snap\w*|bark\w*|sigh\w*|groan\w*|chant\w*|sang|sing\w*|tell|told|go|goes|went|like))");
    if (std::regex_match(verb, speech)) return false;
    int words = 0;
    bool in_word = false;
    for (char32_t c : inner) {
        const bool w = !py_isspace(c);
        if (w && !in_word) ++words;
        in_word = w;
    }
    return words <= 6;
}

// An epigraph: a paragraph that is a quote followed by its source ('"Do no harm..." - Moira Fowley-Doyle, ...').
// Read by the narrator as a whole, quote marks kept.
static bool epigraph(const std::u32string & p) {
    size_t i = 0;
    while (i < p.size() && py_isspace(p[i])) ++i;
    if (i == p.size() || (p[i] != U'“' && p[i] != U'"' && p[i] != U'”')) return false;
    size_t e = p.find_first_of(U"”\"", i + 1);
    if (e == std::u32string::npos) return false;
    ++e;
    while (e < p.size() && py_isspace(p[e])) ++e;
    if (e >= p.size() || (p[e] != U'-' && p[e] != U'—' && p[e] != U'–' && p[e] != U'~')) return false;
    return p.find_first_of(U"“”\"", e) == std::u32string::npos;   // nothing quoted after the source
}

std::vector<Span> split_spans(const std::vector<std::string> & paragraphs) {
    std::vector<Span> spans;
    for (size_t pi = 0; pi < paragraphs.size(); ++pi) {
        const std::u32string p = utf8_to_u32(paragraphs[pi]);
        if (epigraph(p)) {
            spans.push_back({0, static_cast<int>(pi), "narration", strip(paragraphs[pi])});
            continue;
        }
        size_t pos = 0;
        auto add = [&](const std::string & kind, const std::string & text) {
            spans.push_back({0, static_cast<int>(pi), kind, text});
        };
        // “…” or "…"; an unclosed quote runs to the end of the paragraph (multi-paragraph speech)
        for (size_t i = 0; i < p.size();) {
            char32_t close;
            if (p[i] == U'“') close = U'”';
            else if (p[i] == U'"') close = U'"';
            else {
                ++i;
                continue;
            }
            size_t e = p.find(close, i + 1);
            e = e == std::u32string::npos ? p.size() : e + 1;
            if (embedded_quote(p, pos, i, e, p.substr(i + 1, e - i - 2))) {
                i = e;   // stays part of the narration around it
                continue;
            }
            const std::string before = strip(u32_to_utf8(p.substr(pos, i - pos)));
            if (!before.empty() && has_word_char(before)) add("narration", before);
            std::u32string inner = p.substr(i, e - i);
            size_t a = 0, b = inner.size();   // .strip("“”\"")
            while (a < b && (inner[a] == U'“' || inner[a] == U'”' || inner[a] == U'"')) ++a;
            while (b > a && (inner[b - 1] == U'“' || inner[b - 1] == U'”' || inner[b - 1] == U'"')) --b;
            const std::string in = strip(u32_to_utf8(inner.substr(a, b - a)));
            if (has_word_char(in)) add("dialogue", in);
            pos = i = e;
        }
        const std::string rest = strip(u32_to_utf8(p.substr(pos)));
        if (!rest.empty() && has_word_char(rest)) add("narration", rest);
    }
    for (size_t i = 0; i < spans.size(); ++i) spans[i].id = static_cast<int>(i);
    return spans;
}

nlohmann::json spans_json(const std::vector<Span> & spans) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto & s : spans) a.push_back({{"id", s.id}, {"para", s.para}, {"kind", s.kind}, {"text", s.text}});
    return a;
}

std::string spans_markup(const std::string & title, const std::vector<Span> & spans) {
    std::vector<std::string> lines{"# " + title};
    int cur = -1;
    bool any = false;
    for (const auto & sp : spans) {
        if (!any || sp.para != cur) {
            lines.push_back("[P" + std::to_string(sp.para) + "]");
            cur = sp.para;
            any = true;
        }
        lines.back() += " {S" + std::to_string(sp.id) + ":" + (sp.kind == "dialogue" ? "D" : "N") + "} " + sp.text;
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n\n" : "") + lines[i];
    return out + "\n";
}

bool looks_like_chapter(const std::string & title) {
    std::string t = title;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::regex re(
        R"(\b(announcement|hiatus|q\s*&\s*a|questions and answers|author'?s? note|update|notice)\b)");
    return !std::regex_search(t, re);
}

std::string norm_words(const std::string & text) {
    std::string t;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 3, "\xE2\x80\x99") == 0) {   // ’
            t += '\'';
            i += 3;
        } else {
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
            ++i;
        }
    }
    std::string out, word;
    for (char c : t + " ") {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'') {
            word += c;
        } else if (!word.empty()) {
            out += (out.empty() ? "" : " ") + word;
            word.clear();
        }
    }
    return out;
}

// ---------------------------------------------------------------- difflib.SequenceMatcher (isjunk=None)

namespace {

struct Matcher {
    const std::u32string & a;
    const std::u32string & b;
    std::unordered_map<char32_t, std::vector<int>> b2j;

    Matcher(const std::u32string & a_, const std::u32string & b_) : a(a_), b(b_) {
        const int n = static_cast<int>(b.size());
        for (int i = 0; i < n; ++i) b2j[b[i]].push_back(i);
        if (n >= 200) {   // autojunk: drop popular elements
            const size_t ntest = n / 100 + 1;
            for (auto it = b2j.begin(); it != b2j.end();)
                it = it->second.size() > ntest ? b2j.erase(it) : std::next(it);
        }
    }

    // (i, j, size) of the longest matching block in a[alo:ahi], b[blo:bhi]
    std::tuple<int, int, int> longest(int alo, int ahi, int blo, int bhi) const {
        int besti = alo, bestj = blo, bestsize = 0;
        std::unordered_map<int, int> j2len;
        for (int i = alo; i < ahi; ++i) {
            std::unordered_map<int, int> newj2len;
            auto it = b2j.find(a[i]);
            if (it != b2j.end())
                for (int j : it->second) {
                    if (j < blo) continue;
                    if (j >= bhi) break;
                    auto pj = j2len.find(j - 1);
                    const int k = (pj == j2len.end() ? 0 : pj->second) + 1;
                    newj2len[j] = k;
                    if (k > bestsize) {
                        besti = i - k + 1;
                        bestj = j - k + 1;
                        bestsize = k;
                    }
                }
            j2len.swap(newj2len);
        }
        // extend over popular (non-junk) elements
        while (besti > alo && bestj > blo && a[besti - 1] == b[bestj - 1]) {
            --besti;
            --bestj;
            ++bestsize;
        }
        while (besti + bestsize < ahi && bestj + bestsize < bhi && a[besti + bestsize] == b[bestj + bestsize]) ++bestsize;
        return {besti, bestj, bestsize};
    }

    int matched() const {
        int total = 0;
        std::vector<std::tuple<int, int, int, int>> queue{{0, static_cast<int>(a.size()), 0, static_cast<int>(b.size())}};
        while (!queue.empty()) {
            auto [alo, ahi, blo, bhi] = queue.back();
            queue.pop_back();
            auto [i, j, k] = longest(alo, ahi, blo, bhi);
            if (k) {
                total += k;
                if (alo < i && blo < j) queue.emplace_back(alo, i, blo, j);
                if (i + k < ahi && j + k < bhi) queue.emplace_back(i + k, ahi, j + k, bhi);
            }
        }
        return total;
    }
};

}  // namespace

double seq_ratio(const std::string & sa, const std::string & sb) {
    const std::u32string a = utf8_to_u32(sa), b = utf8_to_u32(sb);
    const size_t len = a.size() + b.size();
    if (len == 0) return 1.0;
    return 2.0 * Matcher(a, b).matched() / static_cast<double>(len);
}

// ---------------------------------------------------------------- what the TTS reads

static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static std::string replace_all(std::string s, const std::string & from, const std::string & to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

// Tested by ear on VoxCPM2 (2026-09-29): it reads abbreviations fine as written; it invents sounds for marks that
// are not words (***, _____, *word*); CAPS is how it emphasizes a word; "..." reads an interruption better than a
// dash, and commas read an aside set off by a pair of dashes better than the dashes.
std::string speech_text(const std::string & text) {
    std::string t = strip(text);
    if (!has_word_char(t)) return "";   // a separator line (=====, ***, _____, ——): a pause, nothing to say
    for (const auto & [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"\xE2\x80\x99", "'"}, {"\xE2\x80\x98", "'"}, {"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""},   // ’ ‘ “ ”
             {"\xE2\x80\xA6", "..."}, {"&", " and "}})                                                        // … &
        t = replace_all(t, from, to);

    // *emphasis* / _emphasis_: a word or a few in capitals; a longer passage just loses its marks
    static const std::regex emph(R"((^|[^A-Za-z0-9])[*_]+([^*_\s](?:[^*_]*[^*_\s])?)[*_]+(?![A-Za-z0-9]))");
    std::string out;
    auto last = t.cbegin();
    for (std::sregex_iterator it(t.begin(), t.end(), emph), end; it != end; ++it) {
        const auto & m = *it;
        std::string inner = m[2].str();
        if (std::count(inner.begin(), inner.end(), ' ') < 4)
            for (char & c : inner) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        out.append(last, m[0].first).append(m[1].str()).append(inner);
        last = m[0].second;
    }
    t = out.append(last, t.cend());
    t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == '*' || c == '_' || c == '[' || c == ']' || c == '='; }),
            t.end());

    // dashes between words (— – -- " - "): a pair around an aside -> commas; a single one (an interruption) -> "..."
    const std::string D = "\x01";
    for (const char * d : {" \xE2\x80\x94 ", "\xE2\x80\x94", " \xE2\x80\x93 ", "\xE2\x80\x93", " -- ", "--", " - "})
        t = replace_all(t, d, D);
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, " -") == 0) t.replace(t.size() - 2, 2, D);   // "... Found -"
    t = std::regex_replace(t, std::regex("\x01([^\x01.!?]+)\x01"), ", $1, ");
    t = std::regex_replace(t, std::regex("\x01\\s*$"), "...");
    t = replace_all(t, D, "... ");

    t = std::regex_replace(t, std::regex(R"((^|\s)\+(\d))"), "$1plus $2");      // +1 Skill Rank
    t = std::regex_replace(t, std::regex(R"((^|\s)~\s*(\d))"), "$1about $2");   // ~2 tons
    t = std::regex_replace(t, std::regex(R"((\d)\s*%)"), "$1 percent");
    t = std::regex_replace(t, std::regex(R"(\s+([,.!?;:]))"), "$1");              // tidy what the above left
    t = std::regex_replace(t, std::regex(R"(,(\s*,)+)"), ",");
    t = std::regex_replace(t, std::regex(R"([,;:]\.\.\.)"), "...");                // "said,--" -> "said..."
    t = std::regex_replace(t, std::regex(R"(\s{2,})"), " ");
    t = std::regex_replace(strip(t), std::regex(R"(,$)"), ".");
    return has_word_char(t) ? t : "";
}

std::vector<std::string> speech_chunks(const std::string & text) {
    const std::string s = speech_text(text);
    static const std::set<std::string> abbrev = {"Mr", "Mrs", "Ms", "Dr", "St", "vs", "Jr", "Sr", "Prof", "Mt"};
    std::vector<std::string> sents;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '.' && s[i] != '!' && s[i] != '?') continue;
        size_t j = i + 1;
        while (j < s.size() && (s[j] == '.' || s[j] == '!' || s[j] == '?' || s[j] == '"' || s[j] == '\'' || s[j] == ')')) ++j;
        while (j + 2 < s.size() && s.compare(j, 2, "\xE2\x80") == 0 && (s[j + 2] == '\x9D' || s[j + 2] == '\x99')) j += 3;   // ” ’
        if (j >= s.size() || s[j] != ' ') continue;
        size_t k = j;
        while (k < s.size() && s[k] == ' ') ++k;
        if (k >= s.size()) continue;
        const bool next_starts = is_upper(s[k]) || s[k] == '"' || s[k] == '\'' || s.compare(k, 3, "\xE2\x80\x9C") == 0 ||
                                 s.compare(k, 3, "\xE2\x80\x98") == 0;
        if (!next_starts) continue;
        if (s[i] == '.') {   // "Mr. Smith", initials "J. Smith"
            size_t w = i;
            while (w > start && is_alpha(s[w - 1])) --w;
            const std::string word = s.substr(w, i - w);
            if (abbrev.count(word) || (word.size() == 1 && is_upper(word[0]))) continue;
        }
        sents.push_back(strip(s.substr(start, j - start)));
        start = k;
        i = k - 1;
    }
    if (start < s.size() && has_word_char(s.substr(start))) sents.push_back(strip(s.substr(start)));
    // long sentences: split at a comma/semicolon; short ones: merged into the one before
    std::vector<std::string> out;
    for (std::string x : sents) {
        while (x.size() > 320) {
            size_t cut = std::string::npos;
            for (const char * sep : {"; ", ", "}) {
                const size_t p = x.rfind(sep, 240);
                if (p != std::string::npos && p > 80) { cut = p + 1; break; }
            }
            if (cut == std::string::npos) break;
            out.push_back(strip(x.substr(0, cut)));
            x = strip(x.substr(cut));
        }
        if (!out.empty() && (x.size() < 40 || out.back().size() < 40) && out.back().size() + x.size() < 200) out.back() += " " + x;
        else out.push_back(x);
    }
    return out;
}

}  // namespace rm
