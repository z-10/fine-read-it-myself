#include "text.h"

#include "tokenizer.h"   // utf8 <-> u32, py_isspace (booknlp)
#include "unicode.h"     // unicode_cpt_flags_from_cpt (llama)

#include <algorithm>
#include <map>
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

}  // namespace rm
