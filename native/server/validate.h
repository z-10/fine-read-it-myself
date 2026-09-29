// Frozen JSON contracts between directors and the audio stage, and their validation:
//   1. JSON Schema (resources/schemas/*.v1.json, the keywords those schemas use),
//   2. semantics against the chapter: every dialogue span labelled, ids exist, speakers are cast.
#pragma once

#include "text.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace rm {

using json = nlohmann::json;

inline const char * kDirectionId = "readmyself.direction/v1";
bool is_special_speaker(const std::string & name);   // Narrator, Unknown, System
extern const std::vector<std::string> kSpecialSpeakers;

const json & schema(const std::string & name);          // "direction", "cast_pass", "labels_window"
std::vector<std::string> schema_errors(const json & data, const json & schema, size_t limit = 15);
std::vector<std::string> direction_errors(const json & direction, const std::vector<Span> & spans);
// parse a model reply that should be a JSON object; throws std::invalid_argument with a useful message
json parse_json_text(const std::string & text);

}  // namespace rm
