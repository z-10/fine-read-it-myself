// BookNLP GenderEM (gender_inference_model_1.py): IBM-model-1 style EM aligning PER mentions with the
// gendered pronouns tagged within 25 tokens after them, with name/honorific priors.
#pragma once

#include "entity_tagger.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace rm::booknlp {

struct GenderInfo {
    std::vector<double> inference;   // per gender category
    int argmax = -1;                 // category index, -1 if none
    double max = 0, total = 0;
};

// Referential gender categories (the ModernBookNLP pipeline default)
const std::vector<std::vector<std::string>> & gender_categories();

class GenderPriors {
public:
    explicit GenderPriors(const std::string & terms_tsv);
    std::unordered_map<std::string, std::vector<double>> hyper;       // "term\tprop" -> prior
    std::unordered_map<std::string, std::vector<double>> honorific;   // "mr." -> prior
    std::vector<std::string> order;                                   // hyper keys in file order
};

// genders by coref id (GenderEM.tag), then update_gender_from_coref
std::map<int, GenderInfo> infer_genders(const GenderPriors & priors, const std::vector<Token> & tokens,
                                        const std::vector<Entity> & entities, const std::vector<int> & refs);
void update_gender_from_coref(std::map<int, GenderInfo> & genders, const std::vector<Entity> & entities,
                              const std::vector<int> & corefs);

}  // namespace rm::booknlp
