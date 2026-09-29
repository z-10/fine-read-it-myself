#include "tokenizer.h"

#include <algorithm>
#include <map>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace rm::booknlp {
namespace {

struct CpRange {
    char32_t lo, hi;
};
#include "spacy_en_classes.inc"

template <size_t N>
bool in(const CpRange (&table)[N], char32_t c) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (c < table[mid].lo) hi = mid;
        else if (c > table[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

bool alpha(char32_t c) { return in(kALPHA, c); }
bool lower(char32_t c) { return in(kALPHA_LOWER, c); }
bool upper(char32_t c) { return in(kALPHA_UPPER, c); }
bool icon(char32_t c) { return in(kICONS, c); }
bool digit(char32_t c) { return c >= U'0' && c <= U'9'; }

using S = std::u32string;
using SV = std::u32string_view;

// lang/char_classes.py
const std::vector<S> kPunct = {U"…", U"……", U",", U":", U";", U"!", U"?", U"¿", U"؟", U"¡", U"(", U")", U"[",
                               U"]", U"{", U"}", U"<", U">", U"_", U"#", U"*", U"&", U"。", U"？", U"！", U"，",
                               U"、", U"；", U"：", U"～", U"·", U"।", U"،", U"۔", U"؛", U"٪"};
const std::vector<S> kQuotes = {U"'", U"\"", U"”", U"“", U"`", U"‘", U"´", U"’", U"‚", U",", U"„", U"»", U"«",
                                U"「", U"」", U"『", U"』", U"（", U"）", U"〔", U"〕", U"【", U"】", U"《", U"》",
                                U"〈", U"〉", U"〈", U"〉", U"⟦", U"⟧"};
const std::vector<S> kCurrency = {U"$", U"£", U"€", U"¥", U"฿", U"US$", U"C$", U"A$", U"₽", U"﷼", U"₴", U"₠",
                                  U"₡", U"₢", U"₣", U"₤", U"₥", U"₦", U"₧", U"₨", U"₩", U"₪", U"₫", U"€",
                                  U"₭", U"₮", U"₯", U"₰", U"₱", U"₲", U"₳", U"₴", U"₵", U"₶", U"₷", U"₸",
                                  U"₹", U"₺", U"₻", U"₼", U"₽", U"₾", U"₿"};
const std::vector<S> kUnits = {
    U"km", U"km²", U"km³", U"m", U"m²", U"m³", U"dm", U"dm²", U"dm³", U"cm", U"cm²", U"cm³", U"mm", U"mm²",
    U"mm³", U"ha", U"µm", U"nm", U"yd", U"in", U"ft", U"kg", U"g", U"mg", U"µg", U"t", U"lb", U"oz", U"m/s",
    U"km/h", U"kmh", U"mph", U"hPa", U"Pa", U"mbar", U"mb", U"MB", U"kb", U"KB", U"gb", U"GB", U"tb", U"TB",
    U"T", U"G", U"M", U"K", U"%", U"км", U"км²", U"км³", U"м", U"м²", U"м³", U"дм", U"дм²", U"дм³", U"см",
    U"см²", U"см³", U"мм", U"мм²", U"мм³", U"нм", U"кг", U"г", U"мг", U"м/с", U"км/ч", U"кПа", U"Па",
    U"мбар", U"Кб", U"КБ", U"кб", U"Мб", U"МБ", U"мб", U"Гб", U"ГБ", U"гб", U"Тб", U"ТБ", U"тбكم", U"كم²",
    U"كم³", U"م", U"م²", U"م³", U"سم", U"سم²", U"سم³", U"مم", U"مم²", U"مم³", U"كم", U"غرام", U"جرام", U"جم",
    U"كغ", U"ملغ", U"كوب", U"اكواب"};
const std::vector<S> kHyphens = {U"-", U"–", U"—", U"--", U"---", U"——", U"~"};

bool starts_with(SV s, SV p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

bool is_quote_char(char32_t c) {
    for (const auto & q : kQuotes)
        if (q.size() == 1 && q[0] == c) return true;
    return false;
}

// TOKENIZER_PREFIXES, first alternative that matches at position 0 (re.search with ^ anchors)
size_t find_prefix(SV s) {
    if (s.empty()) return 0;
    for (SV lit : {SV(U"§"), SV(U"%"), SV(U"="), SV(U"—"), SV(U"–")})
        if (starts_with(s, lit)) return lit.size();
    if (s[0] == U'+' && !(s.size() > 1 && digit(s[1]))) return 1;
    for (const auto & p : kPunct)
        if (starts_with(s, p)) return p.size();
    if (s.size() >= 2 && s[0] == U'.' && s[1] == U'.') {
        size_t n = 2;
        while (n < s.size() && s[n] == U'.') ++n;
        return n;
    }
    if (s[0] == U'…') return 1;
    for (const auto & q : kQuotes)
        if (starts_with(s, q)) return q.size();
    for (const auto & c : kCurrency)
        if (starts_with(s, c)) return c.size();
    if (icon(s[0])) return 1;
    return 0;
}

// does one TOKENIZER_SUFFIXES alternative match exactly s[b:] (lookbehinds see s[:b])
bool suffix_at(SV s, size_t b) {
    const SV t = s.substr(b);
    for (const auto & p : kPunct)
        if (t == p) return true;
    if (t.size() >= 2 && std::all_of(t.begin(), t.end(), [](char32_t c) { return c == U'.'; })) return true;
    if (t == U"…") return true;
    for (const auto & q : kQuotes)
        if (t == q) return true;
    if (t.size() == 1 && icon(t[0])) return true;
    for (SV lit : {SV(U"'s"), SV(U"'S"), SV(U"’s"), SV(U"’S"), SV(U"—"), SV(U"–")})
        if (t == lit) return true;
    const bool after_digit = b > 0 && digit(s[b - 1]);
    if (after_digit && t == U"+") return true;
    if (t == U"." && b >= 2 && s[b - 2] == U'°' && SV(U"FfCcKk").find(s[b - 1]) != SV::npos) return true;
    if (after_digit) {
        for (const auto & c : kCurrency)
            if (t == c) return true;
        for (const auto & u : kUnits)
            if (t == u) return true;
    }
    if (t == U"." && b >= 1 && in(kDOT_SUFFIX_AFTER, s[b - 1])) return true;
    if (t == U"." && b >= 2 && upper(s[b - 2]) && upper(s[b - 1])) return true;
    return false;
}

// re.search over "(?:alt$|...)": the earliest start position wins, so the longest matching suffix
size_t find_suffix(SV s) {
    for (size_t b = 0; b < s.size(); ++b)
        if (suffix_at(s, b)) return s.size() - b;
    return 0;
}

// TOKENIZER_INFIXES (lang/en/punctuation.py) at position i: match length, 0 if none
size_t infix_at(SV s, size_t i) {
    const char32_t c = s[i];
    const bool has_prev = i > 0, has_next = i + 1 < s.size();
    if (c == U'.' && i + 1 < s.size() && s[i + 1] == U'.') {
        size_t n = 2;
        while (i + n < s.size() && s[i + n] == U'.') ++n;
        return n;
    }
    if (c == U'…') return 1;
    if (icon(c)) return 1;
    if (has_prev && digit(s[i - 1]) && SV(U"+-*^").find(c) != SV::npos && has_next &&
        (digit(s[i + 1]) || s[i + 1] == U'-'))
        return 1;
    if (c == U'.' && has_prev && (lower(s[i - 1]) || is_quote_char(s[i - 1])) && has_next &&
        (upper(s[i + 1]) || is_quote_char(s[i + 1])))
        return 1;
    if (c == U',' && has_prev && alpha(s[i - 1]) && has_next && alpha(s[i + 1])) return 1;
    if (has_prev && (alpha(s[i - 1]) || digit(s[i - 1]))) {
        for (const auto & h : kHyphens) {
            if (starts_with(s.substr(i), h) && i + h.size() < s.size() && alpha(s[i + h.size()])) return h.size();
        }
        if (SV(U":<>=/").find(c) != SV::npos && has_next && alpha(s[i + 1])) return 1;
    }
    return 0;
}

std::vector<std::pair<size_t, size_t>> find_infixes(SV s) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < s.size();) {
        const size_t n = infix_at(s, i);
        if (n) {
            out.emplace_back(i, i + n);
            i += n;
        } else {
            ++i;
        }
    }
    return out;
}

bool py_word(char32_t c) {  // Python re \w (unicode)
    return c == U'_' || alpha(c) || digit(c) || (c > 0x7F && (in(kALPHA, c)));
}

bool url_label_char(char32_t c) {
    return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || digit(c) || (c >= 0xA1 && c <= 0xFFFF);
}

// spacy/lang/tokenizer_exceptions.py URL_PATTERN, without the IPv4 branch (not needed for prose)
bool url_match(SV s) {
    size_t p = 0;
    // optional scheme: [\w+\-.]{2,}://
    if (const size_t k = s.find(U"://"); k != SV::npos && k >= 2) {
        bool ok = true;
        for (size_t i = 0; i < k; ++i)
            if (!(py_word(s[i]) || s[i] == U'+' || s[i] == U'-' || s[i] == U'.')) ok = false;
        if (ok) p = k + 3;
    }
    auto host_rest = [&](size_t h) -> bool {
        // host: ([label-start][label-mid]{0,62})?[label-end]\.)+ tld{2,63}
        size_t e = h;
        while (e < s.size() && SV(U":/?#").find(s[e]) == SV::npos) ++e;
        const SV host = s.substr(h, e - h);
        const size_t dot = host.rfind(U'.');
        if (dot == SV::npos) return false;
        const SV tld = host.substr(dot + 1);
        if (tld.size() < 2 || tld.size() > 63 || !std::all_of(tld.begin(), tld.end(), lower)) return false;
        size_t a = 0;
        while (a < dot + 1) {
            const size_t d = host.find(U'.', a);
            const SV label = host.substr(a, d - a);
            if (label.empty() || label.size() > 64 || !url_label_char(label.front()) ||
                !url_label_char(label.back()))
                return false;
            for (char32_t c : label)
                if (!(url_label_char(c) || c == U'_' || c == U'-')) return false;
            a = d + 1;
        }
        size_t r = e;
        if (r < s.size() && s[r] == U':') {  // (?::\d{2,5})?
            size_t n = 0;
            while (r + 1 + n < s.size() && digit(s[r + 1 + n])) ++n;
            if (n >= 2 && n <= 5) r += 1 + n;
        }
        if (r == s.size()) return true;
        if (SV(U"/?#").find(s[r]) == SV::npos) return false;
        for (size_t i = r; i < s.size(); ++i)
            if (py_isspace(s[i])) return false;
        return true;
    };
    if (host_rest(p)) return true;
    // optional userinfo: \S+(?::\S*)?@
    for (size_t at = s.find(U'@', p); at != SV::npos; at = s.find(U'@', at + 1))
        if (at > p && host_rest(at + 1)) return true;
    return false;
}

class Tokenizer {
public:
    Tokenizer() {
        static const std::pair<const char *, const char *> kRules[] = {
#include "spacy_en_special_cases.inc"
        };
        for (const auto & [k, v] : kRules) {
            std::vector<S> parts;
            const std::string joined = v;
            size_t a = 0;
            for (size_t b; (b = joined.find('\x1f', a)) != std::string::npos; a = b + 1)
                parts.push_back(utf8_to_u32(joined.substr(a, b - a)));
            parts.push_back(utf8_to_u32(joined.substr(a)));
            specials_[utf8_to_u32(k)] = parts;
        }
        // special cases containing affixes are re-applied after tokenization (_apply_special_cases);
        // their pattern is the affix tokenization without special cases
        for (const auto & [k, v] : specials_) {
            if (find_prefix(k) || !find_infixes(k).empty() || find_suffix(k) || k.find(U' ') != S::npos) {
                std::vector<S> pat;
                for (const auto & t : tokenize_span(k, false)) pat.push_back(t);
                matcher_.emplace_back(pat, k);
            }
        }
        std::sort(matcher_.begin(), matcher_.end(),
                  [](const auto & a, const auto & b) { return a.first.size() > b.first.size(); });
    }

    std::vector<RawToken> operator()(const S & text) const {
        std::vector<RawToken> doc;
        if (text.empty()) return doc;
        size_t start = 0;
        bool in_ws = py_isspace(text[0]);
        auto flush = [&](size_t b, size_t e) {
            const S span = text.substr(b, e - b);
            size_t idx = b;
            for (auto & t : tokenize_span(span, true)) {
                RawToken rt{t, idx, false};
                rt.space = std::all_of(t.begin(), t.end(), py_isspace);
                idx += t.size();
                doc.push_back(std::move(rt));
            }
        };
        for (size_t i = 0; i < text.size(); ++i) {
            const char32_t uc = text[i];
            if (py_isspace(uc) != in_ws) {
                if (start < i) flush(start, i);
                start = uc == U' ' ? i + 1 : i;
                in_ws = !in_ws;
            }
        }
        if (start < text.size()) flush(start, text.size());
        apply_special_cases(doc);
        return doc;
    }

private:
    bool special(const S & s) const { return specials_.count(s) != 0; }

    std::vector<S> tokenize_span(S string, bool with_special) const {
        std::vector<S> out;
        if (with_special) {
            if (auto it = specials_.find(string); it != specials_.end()) return it->second;
        }
        std::vector<S> prefixes, suffixes;
        size_t last_size = 0;
        while (!string.empty() && string.size() != last_size) {
            if (with_special && special(string)) break;
            last_size = string.size();
            const size_t pre_len = find_prefix(string);
            S prefix, minus_pre;
            if (pre_len) {
                prefix = string.substr(0, pre_len);
                minus_pre = string.substr(pre_len);
                if (!minus_pre.empty() && with_special && special(minus_pre)) {
                    string = minus_pre;
                    prefixes.push_back(prefix);
                    break;
                }
            }
            const size_t suf_len = find_suffix(SV(string).substr(pre_len));
            S suffix, minus_suf;
            if (suf_len) {
                suffix = string.substr(string.size() - suf_len);
                minus_suf = string.substr(0, string.size() - suf_len);
                if (!minus_suf.empty() && with_special && special(minus_suf)) {
                    string = minus_suf;
                    suffixes.push_back(suffix);
                    break;
                }
            }
            if (pre_len && suf_len && pre_len + suf_len <= string.size()) {
                string = string.substr(pre_len, string.size() - pre_len - suf_len);
                prefixes.push_back(prefix);
                suffixes.push_back(suffix);
            } else if (pre_len) {
                string = minus_pre;
                prefixes.push_back(prefix);
            } else if (suf_len) {
                string = minus_suf;
                suffixes.push_back(suffix);
            }
        }
        for (auto & p : prefixes) out.push_back(p);
        if (!string.empty()) {
            if (auto it = specials_.find(string); with_special && it != specials_.end()) {
                for (auto & t : it->second) out.push_back(t);
            } else if (url_match(string)) {
                out.push_back(string);
            } else {
                const auto matches = find_infixes(string);
                if (matches.empty()) {
                    out.push_back(string);
                } else {
                    size_t start = 0;
                    for (const auto & [a, b] : matches) {
                        if (a == 0) continue;
                        if (a != start) out.push_back(string.substr(start, a - start));
                        if (a != b) out.push_back(string.substr(a, b - a));
                        start = b;
                    }
                    if (start < string.size()) out.push_back(string.substr(start));
                }
            }
        }
        for (auto it = suffixes.rbegin(); it != suffixes.rend(); ++it) out.push_back(*it);
        return out;
    }

    // Tokenizer._apply_special_cases: PhraseMatcher over token texts, longest matches first,
    // no overlaps, each match replaced by the special case's tokens
    void apply_special_cases(std::vector<RawToken> & doc) const {
        std::vector<std::pair<size_t, size_t>> spans;  // [start, end)
        std::vector<const S *> keys;
        for (const auto & [pat, key] : matcher_) {
            for (size_t i = 0; i + pat.size() <= doc.size(); ++i) {
                bool ok = true;
                for (size_t j = 0; j < pat.size() && ok; ++j) ok = doc[i + j].text == pat[j];
                if (ok) {
                    spans.emplace_back(i, i + pat.size());
                    keys.push_back(&key);
                }
            }
        }
        if (spans.empty()) return;
        // _filter_special_spans: sort by (length, start); take from the back (longest, latest start)
        std::vector<size_t> order(spans.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const size_t la = spans[a].second - spans[a].first, lb = spans[b].second - spans[b].first;
            return la != lb ? la < lb : spans[a].first < spans[b].first;
        });
        std::unordered_set<size_t> seen;
        std::map<size_t, size_t> chosen;  // start -> span index
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const auto [a, b] = spans[*it];
            if (!seen.count(a) && !seen.count(b - 1)) chosen[a] = *it;
            for (size_t k = a; k < b; ++k) seen.insert(k);
        }
        std::vector<RawToken> out;
        for (size_t i = 0; i < doc.size();) {
            auto c = chosen.find(i);
            if (c == chosen.end()) {
                out.push_back(doc[i++]);
                continue;
            }
            const auto [a, b] = spans[c->second];
            size_t idx = doc[a].idx;
            for (const auto & t : specials_.at(*keys[c->second])) {
                out.push_back(RawToken{t, idx, std::all_of(t.begin(), t.end(), py_isspace)});
                idx += t.size();
            }
            i = b;
        }
        doc.swap(out);
    }

    struct U32Hash {
        size_t operator()(const S & s) const { return std::hash<std::u32string>()(s); }
    };
    std::unordered_map<S, std::vector<S>, U32Hash> specials_;
    std::vector<std::pair<std::vector<S>, S>> matcher_;
};

}  // namespace

bool py_isspace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

std::u32string utf8_to_u32(const std::string & s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + n > s.size()) { out.push_back(0xFFFD); break; }
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string u32_to_utf8(const std::u32string & s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t cp : s) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::vector<RawToken> tokenize(const std::u32string & text) {
    static const Tokenizer tok;
    return tok(text);
}

}  // namespace rm::booknlp
