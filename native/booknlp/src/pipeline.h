// ModernBookNLP pipeline ("entity,quote,coref", big models, modern_qa) in C++: chapter text -> quotes
// with speaker names and character genders, plus every intermediate for parity checks.
#pragma once

#include "entity_tagger.h"
#include "gender.h"
#include "names.h"
#include "quote_attrib.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rm::booknlp {

struct Character {
    int id;
    int count;
    std::vector<std::pair<std::string, int>> proper;   // most_common order
    const GenderInfo * g = nullptr;
};

struct Result {
    std::vector<Token> tokens;
    std::vector<Entity> entities;
    std::vector<std::pair<int, int>> quotes;
    std::vector<int> attributions;           // entity index or -1, sized as in ModernBookNLP
    std::vector<int> refs;                   // name coreference
    std::map<int, GenderInfo> genders_em;    // before coreference
    std::vector<int> assignments;            // coreference
    std::map<int, GenderInfo> genders;
    std::vector<Character> characters;
    // readmyself reduction (booknlp_runner.py): quote text + speaker name, name -> "male"/"female"
    std::vector<std::pair<std::string, std::string>> speaker_quotes;
    std::vector<std::pair<std::string, std::string>> speaker_genders;
    std::map<std::string, double> seconds;
};

class Pipeline {
public:
    // models_dir holds bnlp-entities.gguf, bnlp-quote.gguf, bnlp-coref.gguf
    Pipeline(const std::string & models_dir, const Backend & backend);
    ~Pipeline();
    Result run(const std::string & chapter_text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rm::booknlp
