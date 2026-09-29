// ModernBookNLP GraphQuotationAttribution (modern_qa.py + inp_to_graph.py) at inference: overlapping
// 2000-token context windows, ModernBERT over each window, [start;end] span features for quotes and
// PER mentions, an MLP scoring every (quote, mention) pair in the window, softmax per quote, and the
// most confident window wins.
#pragma once

#include "entity_tagger.h"
#include "modernbert.h"

#include <utility>
#include <vector>

namespace rm::booknlp {

class QuoteAttribution {
public:
    explicit QuoteAttribution(const GgufModel & m);
    // attributed[i] = index into `entities` of quote i's speaker mention, -1 if none; sized like
    // ModernBookNLP (up to the last quote that got a window)
    std::vector<int> tag(const std::vector<std::pair<int, int>> & quotes, const std::vector<Entity> & entities,
                         const std::vector<Token> & tokens) const;

private:
    const GgufModel & m_;
    ModernBert bert_;
    float b3_ = 0.f;   // proj.3.bias
};

}  // namespace rm::booknlp
