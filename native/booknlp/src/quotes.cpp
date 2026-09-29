#include "quotes.h"

#include "tokenizer.h"

#include <string>

namespace rm::booknlp {

std::vector<std::pair<int, int>> tag_quotes(std::vector<Token> & tokens) {
    // Counter of quote symbols; most_common() breaks ties by first insertion
    const std::string kDouble = "DOUBLE_QUOTE", kSingle = "SINGLE_QUOTE", kDash = "DASH";
    std::vector<std::pair<std::string, int>> counts;
    auto bump = [&](const std::string & k) {
        for (auto & [key, n] : counts)
            if (key == k) { ++n; return; }
        counts.emplace_back(k, 1);
    };
    for (const auto & t : tokens) {
        if (t.text == "“" || t.text == "”" || t.text == "\"") bump(kDouble);
        else if (t.text == "‘" || t.text == "’" || t.text == "'") bump(kSingle);
        else if (t.text == "—") bump(kDash);
    }
    std::string symbol = kDouble;
    int best = 0;
    for (const auto & [key, n] : counts)
        if (n > best) { best = n; symbol = key; }

    std::vector<std::pair<int, int>> predictions;
    int start = -1, last_par = -1;
    bool have_last = false;
    size_t quote_len = 0;
    for (const auto & tok : tokens) {
        const std::u32string u = utf8_to_u32(tok.text);
        std::string w = tok.text;
        bool dbl = false, sgl = false;
        for (size_t i = 0; i < u.size(); ++i) {
            const char32_t c = u[i];
            if (c == U'“' || c == U'”' || c == U'"') {
                dbl = true;
            } else if ((c == U'‘' || c == U'’' || c == U'\'') && i == 0) {
                const std::u32string suff = u.substr(1);
                if (suff != U"s" && suff != U"d" && suff != U"ll" && suff != U"ve") sgl = true;
            }
        }
        if (dbl) w = kDouble;
        else if (sgl) w = kSingle;

        if (have_last && tok.paragraph_id != last_par) {   // start over at each new paragraph
            if (quote_len > 0) predictions.emplace_back(start, tok.token_id - 1);
            start = -1;
            quote_len = 0;
        }
        if (w == symbol) {
            if (start != -1) {
                if (quote_len > 0) predictions.emplace_back(start, tok.token_id);
                start = -1;
                quote_len = 0;
            } else {
                start = tok.token_id;
            }
        }
        if (start != -1) ++quote_len;
        last_par = tok.paragraph_id;
        have_last = true;
    }
    for (const auto & [s, e] : predictions)
        for (int i = s; i <= e; ++i) tokens[i].in_quote = true;
    return predictions;
}

}  // namespace rm::booknlp
