#include "frontend.h"

#include "tokenizer.h"
#include "unicode.h"

#include <unordered_set>

namespace rm::booknlp {
namespace {

const std::unordered_set<std::u32string> kClosers = {U"”", U"’", U")", U"]", U"}", U"»"};
const std::unordered_set<std::u32string> kAmbig = {U"\"", U"'", U"*", U"_"};
const std::unordered_set<std::u32string> kOpeners = {U"“", U"‘", U"(", U"[", U"{", U"«"};

const std::unordered_set<std::string> kClosedClass = {
    "a", "an", "the", "this", "that", "these", "those", "my", "your", "his", "her", "its", "our", "their",
    "whose", "which", "what", "and", "or", "but", "nor", "so", "yet", "for", "of", "in", "on", "at", "by",
    "to", "from", "with", "without", "into", "onto", "over", "under", "about", "above", "below", "after",
    "before", "during", "through", "between", "among", "against", "as", "than", "then", "if", "when",
    "while", "because", "although", "though", "i", "you", "he", "she", "it", "we", "they", "me", "him",
    "us", "them", "mine", "yours", "hers", "ours", "theirs", "myself", "yourself", "himself", "herself",
    "itself", "ourselves", "themselves", "is", "am", "are", "was", "were", "be", "been", "being", "do",
    "does", "did", "have", "has", "had", "will", "would", "shall", "should", "can", "could", "may",
    "might", "must", "not", "no", "yes", "oh", "ah", "hey", "well"};

const std::unordered_set<std::string> kHeadStop = {
    "of", "with", "who", "whom", "whose", "that", "which", "in", "on", "at", "from", "for", "to", "by",
    "about", "like", "near", "under", "over", "behind", "than", ",", "'s", "’s"};

struct View {
    std::u32string text;
    size_t idx;
    int paragraph;
};

bool is_term(const std::u32string & w) {
    if (w.empty()) return false;
    for (char32_t c : w)
        if (c != U'.' && c != U'?' && c != U'!' && c != U'…') return false;
    return true;
}

bool attached(const std::vector<View> & t, size_t i) { return t[i].idx == t[i - 1].idx + t[i - 1].text.size(); }

bool is_closer(const std::vector<View> & t, size_t j) {
    return kClosers.count(t[j].text) || (kAmbig.count(t[j].text) && j > 0 && attached(t, j));
}

bool boundary_before(const std::vector<View> & t, size_t i) {
    if (t[i].paragraph == t[i - 1].paragraph && is_closer(t, i)) return false;
    size_t j = i - 1;
    while (j > 0 && is_closer(t, j)) --j;
    if (!is_term(t[j].text)) return false;
    const std::u32string & w = t[i].text;
    const char32_t c = w[0];
    const auto f = unicode_cpt_flags_from_cpt(c);
    return f.is_uppercase || f.is_number || kOpeners.count(w) || kAmbig.count(w) ||
           std::u32string(U"“‘([{«").find(c) != std::u32string::npos;
}

std::u32string filter_ws(const std::u32string & s) {
    std::u32string o = s;
    for (auto & c : o) {
        if (c == U' ') c = U'S';
        else if (c == U'\n' || c == U'\r') c = U'N';
        else if (c == U'\t') c = U'T';
    }
    return o;
}

}  // namespace

std::string py_lower(const std::string & s) {
    std::u32string u = utf8_to_u32(s);
    for (auto & c : u) c = static_cast<char32_t>(unicode_tolower(c));
    return u32_to_utf8(u);
}

bool first_is_upper(const std::string & s) {
    const auto u = utf8_to_u32(s);
    return !u.empty() && static_cast<char32_t>(unicode_tolower(u[0])) != u[0];
}

bool first_is_alpha(const std::string & s) {
    const auto u = utf8_to_u32(s);
    return !u.empty() && unicode_cpt_flags_from_cpt(u[0]).is_letter;
}

std::vector<Token> make_tokens(const std::string & utf8_text) {
    // Python universal newlines
    std::string text;
    text.reserve(utf8_text.size());
    for (size_t i = 0; i < utf8_text.size(); ++i) {
        if (utf8_text[i] == '\r') {
            text += '\n';
            if (i + 1 < utf8_text.size() && utf8_text[i + 1] == '\n') ++i;
        } else {
            text += utf8_text[i];
        }
    }
    std::vector<View> views;
    int paragraph = 0;
    std::u32string ws;
    for (const auto & t : tokenize(utf8_to_u32(text))) {
        if (t.space) {
            ws += t.text;
            continue;
        }
        if (ws.find(U"\n\n") != std::u32string::npos) ++paragraph;
        ws.clear();
        views.push_back({t.text, t.idx, paragraph});
    }
    std::vector<Token> out(views.size());
    int sid = 0;
    for (size_t i = 0; i < views.size(); ++i) {
        if (i > 0 && (views[i].paragraph != views[i - 1].paragraph || boundary_before(views, i))) ++sid;
        Token & t = out[i];
        t.paragraph_id = views[i].paragraph;
        t.sentence_id = sid;
        t.token_id = static_cast<int>(i);
        t.text = u32_to_utf8(filter_ws(views[i].text));
        t.idx = views[i].idx;
        t.len = views[i].text.size();
        t.pos = first_is_alpha(t.text) && !kClosedClass.count(py_lower(t.text)) ? "PROPN" : "X";
    }
    return out;
}

int nom_head(int start, int end, const std::vector<Token> & tokens) {
    int head = start;
    for (int i = start; i <= end; ++i) {
        if (i > start && kHeadStop.count(py_lower(tokens[i].text))) break;
        head = i;
    }
    return head;
}

}  // namespace rm::booknlp
