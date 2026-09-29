// Files compiled into the server (resources/: frozen schemas, director prompt, built-in sources).
#pragma once

#include <string>
#include <vector>

namespace rm {

// path relative to resources/, e.g. "schemas/direction.v1.json"; throws if unknown
const std::string & resource(const std::string & path);
std::vector<std::string> resource_names(const std::string & prefix);

}  // namespace rm
