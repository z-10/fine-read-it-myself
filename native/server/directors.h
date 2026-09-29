// Directors decide who speaks each dialogue span and describe the characters. Every director returns a
// chapter direction in the frozen format resources/schemas/direction.v1.json:
//   {"schema": "readmyself.direction/v1", "cast": {name: {gender, age, voice, aliases}},
//    "labels": [{"id": span_id, "speaker": name}, ...]}
// plus "quality_issues" (not part of the contract). Every LLM reply is validated (JSON Schema + semantic
// checks); invalid replies go back to the model with the exact errors, up to 3 attempts.
//
// Producers work on a chapter's analysis (step 1, see steps.h): the speaker ModernBookNLP detected for each
// dialogue span, as corrected by the user (the user's edits, "by": "user", are final). Both producers then
// run the speaker check: the LLM labels every other quote without seeing step 1's speakers; where the two
// agree the label stands, where they disagree the LLM picks between them (logged in the script's "review").
// Then the cast pass describes new characters.
// - local:<model>: a local model from llm_presets.h ("local" = the default, Qwen3.5-4B); greedy for the speaker check.
// - OpenAI-compatible endpoint profiles from the user's settings.
#pragma once

#include "engines.h"
#include "settings.h"
#include "text.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rm {

using json = nlohmann::json;
using Log = std::function<void(const std::string &)>;

struct DirectorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};


class Director {
public:
    virtual ~Director() = default;
    // known: the novel's cast rows (name, gender, age, aliases, ...)
    virtual json direct(const json & analysis, const std::vector<json> & known, const Log & log) = 0;
};

// dialogue span id -> speaker, matching ModernBookNLP quotes to spans in reading order by text similarity
std::map<int, std::string> align_quotes(const std::vector<Span> & spans,
                                        const std::vector<std::pair<std::string, std::string>> & quotes);
std::vector<Span> analysis_spans(const json & analysis);

std::unique_ptr<Director> get_director(const Settings & s, const std::string & name, Engines & engines);

}  // namespace rm
