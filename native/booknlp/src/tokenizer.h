// English tokenizer reproducing spaCy 3.8 (en_core_web_sm) tokenization: whitespace split, special
// cases, prefix/suffix/infix rules and url_match, as in spacy/tokenizer.pyx + lang/punctuation.py.
#pragma once

#include <string>
#include <vector>

namespace rm::booknlp {

struct RawToken {
    std::u32string text;
    size_t idx = 0;          // codepoint offset in the input
    bool space = false;      // a whitespace token (spaCy is_space)
};

// Tokens in document order, including spaCy's whitespace tokens (runs of whitespace other than a
// single ' ' after a word), which the BookNLP front end uses to find paragraph breaks.
std::vector<RawToken> tokenize(const std::u32string & text);

std::u32string utf8_to_u32(const std::string & s);
std::string u32_to_utf8(const std::u32string & s);
bool py_isspace(char32_t c);

}  // namespace rm::booknlp
