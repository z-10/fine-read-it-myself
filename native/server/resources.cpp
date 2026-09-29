#include "resources.h"

#include <map>
#include <stdexcept>

namespace rm {

static const std::map<std::string, std::string> & table() {
    static const std::map<std::string, std::string> t = {
#include "resources.inc"
    };
    return t;
}

const std::string & resource(const std::string & path) {
    auto it = table().find(path);
    if (it == table().end()) throw std::runtime_error("no built-in resource " + path);
    return it->second;
}

std::vector<std::string> resource_names(const std::string & prefix) {
    std::vector<std::string> out;
    for (const auto & [k, _] : table())
        if (k.rfind(prefix, 0) == 0) out.push_back(k);
    return out;
}

}  // namespace rm
