// BookNLP QuoteTagger (litbank_quote.py): quotation spans by the chapter's dominant quote symbol.
#pragma once

#include "frontend.h"

#include <utility>
#include <vector>

namespace rm::booknlp {

// Returns [start, end] token ids of each quotation and sets Token::in_quote.
std::vector<std::pair<int, int>> tag_quotes(std::vector<Token> & tokens);

}  // namespace rm::booknlp
