// BookNLP token list from raw chapter text: spaCy-compatible tokens (tokenizer.h), paragraph ids from
// blank lines, and rule replacements for the spaCy parser outputs the pipeline reads (sentence ids,
// coarse POS for name coreference, head of a nominal mention).
#pragma once

#include <string>
#include <vector>

namespace rm::booknlp {

struct Token {
    int paragraph_id = 0;
    int sentence_id = 0;
    int token_id = 0;         // index among non-space tokens
    std::string text;         // UTF-8, whitespace inside replaced as in BookNLP (S/N/T)
    size_t idx = 0;           // codepoint offset in the chapter text
    size_t len = 0;           // codepoint length
    std::string pos;          // "PROPN" or "X" (only compared against NOUN/PROPN downstream)
    bool in_quote = false;    // set by the quote tagger
};

std::vector<Token> make_tokens(const std::string & utf8_text);

// head token of the nominal mention [start, end] (GenderEM.get_head replacement)
int nom_head(int start, int end, const std::vector<Token> & tokens);

// Python str.lower() / str.isupper()-style helpers on UTF-8 strings (per codepoint)
std::string py_lower(const std::string & s);
bool first_is_upper(const std::string & s);
bool first_is_alpha(const std::string & s);

}  // namespace rm::booknlp
