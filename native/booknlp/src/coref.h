// ModernBookNLP LitBank coreference (litbank_coref.py + bert_coref_quote_pronouns.py, pronominal-only):
// BERT-12L word representations, attention-pooled span representations, and a greedy left-to-right
// antecedent search for pronouns (outside quotes first, then inside quotes).
#pragma once

#include "bert.h"
#include "entity_tagger.h"
#include "gender.h"

#include <map>
#include <utility>
#include <vector>

namespace rm::booknlp {

class Coref {
public:
    explicit Coref(const GgufModel & m);
    std::vector<int> tag(const std::vector<Token> & tokens, const std::vector<Entity> & entities,
                         const std::vector<int> & refs, const std::map<int, GenderInfo> & ref_genders,
                         const std::vector<int> & attributions, const std::vector<std::pair<int, int>> & quotes) const;

private:
    const GgufModel & m_;
    Bert bert_;
    std::vector<float> dist_emb_, speaker_emb_, nested_emb_, width_emb_, quote_emb_;
    float att2_b_ = 0;
    std::vector<float> u1_w_, u1_b_, u2_w_, u2_b_, u3_w_;
    float u3_b_ = 0;
    std::vector<float> m1_rest_, m1_b_, m2_w_, m2_b_, m3_w_;
    float m3_b_ = 0;
};

}  // namespace rm::booknlp
