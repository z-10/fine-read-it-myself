#include "sources.h"

#include "resources.h"
#include "text.h"

#include <yaml.h>

#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace rm {

namespace {

json scalar(const yaml_event_t & e) {
    const std::string v(reinterpret_cast<const char *>(e.data.scalar.value), e.data.scalar.length);
    if (e.data.scalar.style != YAML_PLAIN_SCALAR_STYLE) return v;
    if (v == "true" || v == "True" || v == "TRUE") return true;
    if (v == "false" || v == "False" || v == "FALSE") return false;
    if (v.empty() || v == "~" || v == "null" || v == "Null" || v == "NULL") return nullptr;
    static const std::regex int_re(R"([-+]?[0-9]+)"), float_re(R"([-+]?([0-9]+\.[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?)");
    if (std::regex_match(v, int_re)) return std::stoll(v);
    if (std::regex_match(v, float_re)) return std::stod(v);
    return v;
}

struct Parser {
    yaml_parser_t p;
    explicit Parser(const std::string & text) {
        yaml_parser_initialize(&p);
        yaml_parser_set_input_string(&p, reinterpret_cast<const unsigned char *>(text.data()), text.size());
    }
    ~Parser() { yaml_parser_delete(&p); }
    yaml_event_type_t next(yaml_event_t & e) {
        if (!yaml_parser_parse(&p, &e))
            throw std::runtime_error(std::string("YAML error: ") + (p.problem ? p.problem : "?") + " at line " +
                                     std::to_string(p.problem_mark.line + 1));
        return e.type;
    }
    json value(yaml_event_t & e) {   // e = the value's first event (already read)
        switch (e.type) {
            case YAML_SCALAR_EVENT: {
                json v = scalar(e);
                yaml_event_delete(&e);
                return v;
            }
            case YAML_SEQUENCE_START_EVENT: {
                yaml_event_delete(&e);
                json a = json::array();
                for (;;) {
                    yaml_event_t n;
                    if (next(n) == YAML_SEQUENCE_END_EVENT) {
                        yaml_event_delete(&n);
                        return a;
                    }
                    a.push_back(value(n));
                }
            }
            case YAML_MAPPING_START_EVENT: {
                yaml_event_delete(&e);
                json o = json::object();
                for (;;) {
                    yaml_event_t k;
                    if (next(k) == YAML_MAPPING_END_EVENT) {
                        yaml_event_delete(&k);
                        return o;
                    }
                    const json key = value(k);
                    yaml_event_t v;
                    next(v);
                    o[key.is_string() ? key.get<std::string>() : key.dump()] = value(v);
                }
            }
            default:
                yaml_event_delete(&e);
                throw std::runtime_error("unsupported YAML construct (anchors/aliases are not supported)");
        }
    }
};

// Python (?P<name>...) groups -> ECMAScript groups + names by capture index
std::pair<std::string, std::vector<std::string>> convert_pattern(const std::string & py) {
    std::string out;
    std::vector<std::string> names{""};   // index 0 = whole match
    for (size_t i = 0; i < py.size(); ++i) {
        if (py[i] == '\\' && i + 1 < py.size()) {
            out += py.substr(i, 2);
            ++i;
            continue;
        }
        if (py[i] == '(') {
            if (py.compare(i, 4, "(?P<") == 0) {
                const size_t e = py.find('>', i);
                names.push_back(py.substr(i + 4, e - i - 4));
                out += '(';
                i = e;
                continue;
            }
            if (i + 1 < py.size() && py[i + 1] != '?') names.push_back("");
        }
        out += py[i];
    }
    return {out, names};
}

Source parse_source(const std::string & yaml_text) {
    const json d = yaml_to_json(yaml_text);
    Source s;
    s.id = d.at("id").get<std::string>();
    s.name = d.value("name", s.id);
    s.homepage = d.value("homepage", "");
    for (const auto & m : d.at("match")) {
        auto [pat, names] = convert_pattern(m.get<std::string>());
        s.match.emplace_back(std::regex(pat, std::regex::ECMAScript), names);
    }
    s.novel = d.at("novel");
    s.chapter = d.at("chapter");
    s.fetch = d.value("fetch", json::object());
    return s;
}

}  // namespace

json yaml_to_json(const std::string & text) {
    Parser p(text);
    yaml_event_t e;
    json result = nullptr;
    for (;;) {
        const auto t = p.next(e);
        if (t == YAML_STREAM_END_EVENT) {
            yaml_event_delete(&e);
            break;
        }
        if (t == YAML_MAPPING_START_EVENT || t == YAML_SEQUENCE_START_EVENT || t == YAML_SCALAR_EVENT) {
            if (result.is_null()) result = p.value(e);
            else yaml_event_delete(&e);
            continue;
        }
        yaml_event_delete(&e);
    }
    return result;
}

std::optional<std::map<std::string, std::string>> Source::match_url(const std::string & url) const {
    const std::string u = strip(url);
    for (const auto & [re, names] : match) {
        std::smatch m;
        if (std::regex_search(u, m, re, std::regex_constants::match_continuous)) {
            std::map<std::string, std::string> vars;
            for (size_t g = 1; g < m.size() && g < names.size(); ++g)
                if (!names[g].empty() && m[g].matched) vars[names[g]] = m[g].str();
            return vars;
        }
    }
    return std::nullopt;
}

std::string Source::novel_url(const std::map<std::string, std::string> & vars) const {
    std::string t = novel.at("url").get<std::string>();
    for (const auto & [k, v] : vars) {
        const std::string key = "{" + k + "}";
        for (size_t p; (p = t.find(key)) != std::string::npos;) t.replace(p, key.size(), v);
    }
    return t;
}

Sources load_sources(const std::vector<std::filesystem::path> & dirs) {
    Sources out;
    for (const auto & name : resource_names("sources/")) {
        Source s = parse_source(resource(name));
        out[s.id] = std::move(s);
    }
    for (const auto & dir : dirs) {
        if (!std::filesystem::is_directory(dir)) continue;
        std::vector<std::filesystem::path> files;
        for (const auto & f : std::filesystem::directory_iterator(dir))
            if (f.path().extension() == ".yaml") files.push_back(f.path());
        std::sort(files.begin(), files.end());
        for (const auto & f : files) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            try {
                Source s = parse_source(ss.str());
                out[s.id] = std::move(s);
            } catch (const std::exception & e) {
                std::cerr << "plugin " << f.u8string() << " skipped: " << e.what() << std::endl;
            }
        }
    }
    return out;
}

std::optional<std::pair<const Source *, std::map<std::string, std::string>>> resolve(const std::string & url,
                                                                                    const Sources & sources) {
    for (const auto & [_, s] : sources)
        if (auto vars = s.match_url(url)) return std::make_pair(&s, *vars);
    return std::nullopt;
}

}  // namespace rm
