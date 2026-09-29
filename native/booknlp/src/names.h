// BookNLP NameCoref (name_coref.py): narrator cluster, identical non-PER proper names, and proper-name
// clustering of PER mentions (aliases, sub-name variants, recency).
#pragma once

#include "entity_tagger.h"
#include "ordered.h"

#include <string>
#include <vector>

namespace rm::booknlp {

class NameCoref {
public:
    explicit NameCoref(const std::string & aliases_tsv);
    std::vector<int> cluster_narrator(const std::vector<Entity> & entities, const std::vector<int> & in_quotes) const;
    std::vector<int> cluster_identical_propers(const std::vector<Entity> & entities, std::vector<int> refs) const;
    std::vector<int> cluster_only_nouns(const std::vector<Entity> & entities, std::vector<int> refs,
                                        const std::vector<Token> & tokens) const;

private:
    using Names = std::vector<std::vector<std::string>>;
    Names get_canonical(const std::vector<std::string> & name_tokens) const;
    std::vector<int> name_cluster(const Names & entities, const std::vector<int> & is_named,
                                  const std::vector<int> & existing) const;
    std::vector<int> cluster(const Names & entities, const std::vector<int> & is_named, const std::vector<int> & refs) const;
    OrderedMap<std::string, OrderedMap<std::string, int>> aliases_;
};

}  // namespace rm::booknlp
