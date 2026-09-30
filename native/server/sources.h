// Web novel source definitions: built-in YAML (resources/sources) plus an optional user folder. A source
// says how to recognise a site's URLs, where the novel page and chapter list are, and how to pull the
// chapter text. Adding a site = dropping a new YAML file; no code changes.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace rm {

using json = nlohmann::json;

struct Source {
    std::string id, name, homepage;
    std::vector<std::pair<std::regex, std::vector<std::string>>> match;   // pattern + group names by index
    json novel, chapter, fetch;

    std::optional<std::map<std::string, std::string>> match_url(const std::string & url) const;
    std::string novel_url(const std::map<std::string, std::string> & vars) const;
    std::string novel_key(const std::map<std::string, std::string> & vars) const;
};

using Sources = std::map<std::string, Source>;   // ordered by id (file names sort the same way)

json yaml_to_json(const std::string & yaml_text);
// built-in plugins, then *.yaml from each folder (a later file with the same id replaces an earlier one)
Sources load_sources(const std::vector<std::filesystem::path> & dirs);
std::optional<std::pair<const Source *, std::map<std::string, std::string>>> resolve(const std::string & url,
                                                                                    const Sources & sources);

}  // namespace rm
