#include "validate.h"

#include "resources.h"
#include "tokenizer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace rm {

const std::vector<std::string> kSpecialSpeakers = {"Narrator", "Unknown", "System"};

bool is_special_speaker(const std::string & name) {
    return std::find(kSpecialSpeakers.begin(), kSpecialSpeakers.end(), name) != kSpecialSpeakers.end();
}

const json & schema(const std::string & name) {
    static std::map<std::string, json> cache;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(name);
    if (it == cache.end()) it = cache.emplace(name, json::parse(resource("schemas/" + name + ".v1.json"))).first;
    return it->second;
}

namespace {

std::string repr(const json & v) {   // close to Python's repr for error messages
    if (v.is_string()) return "'" + v.get<std::string>() + "'";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_null()) return "None";
    std::string s = v.dump();
    return s.size() > 120 ? s.substr(0, 117) + "..." : s;
}

size_t cp_len(const std::string & s) { return booknlp::utf8_to_u32(s).size(); }

bool is_type(const json & v, const std::string & t) {
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "null") return v.is_null();
    if (t == "number") return v.is_number();
    if (t == "integer")
        return v.is_number_integer() || (v.is_number_float() && std::floor(v.get<double>()) == v.get<double>());
    return true;
}

struct Error {
    std::vector<std::string> path;
    std::string msg;
};

void check(const json & v, const json & s, std::vector<std::string> & path, std::vector<Error> & out) {
    auto err = [&](const std::string & m) { out.push_back({path, m}); };
    if (s.contains("const") && v != s["const"]) err(repr(s["const"]) + " was expected");
    if (s.contains("enum")) {
        bool ok = false;
        for (const auto & e : s["enum"]) ok = ok || e == v;
        if (!ok) err(repr(v) + " is not one of " + repr(s["enum"]));
    }
    if (s.contains("type") && !is_type(v, s["type"].get<std::string>())) {
        err(repr(v) + " is not of type '" + s["type"].get<std::string>() + "'");
        return;
    }
    if (s.contains("not")) {
        std::vector<Error> sub;
        check(v, s["not"], path, sub);
        if (sub.empty()) err(repr(v) + " should not be valid under " + s["not"].dump());
    }
    if (v.is_string()) {
        const size_t n = cp_len(v.get<std::string>());
        if (s.contains("minLength") && n < s["minLength"].get<size_t>())
            err(repr(v) + (s["minLength"].get<size_t>() == 1 ? " should be non-empty" : " is too short"));
        if (s.contains("maxLength") && n > s["maxLength"].get<size_t>()) err(repr(v) + " is too long");
    }
    if (v.is_number() && s.contains("minimum") && v.get<double>() < s["minimum"].get<double>())
        err(repr(v) + " is less than the minimum of " + s["minimum"].dump());
    if (v.is_array()) {
        if (s.contains("maxItems") && v.size() > s["maxItems"].get<size_t>()) err(repr(v) + " is too long");
        if (s.contains("items"))
            for (size_t i = 0; i < v.size(); ++i) {
                path.push_back(std::to_string(i));
                check(v[i], s["items"], path, out);
                path.pop_back();
            }
    }
    if (v.is_object()) {
        if (s.contains("maxProperties") && v.size() > s["maxProperties"].get<size_t>())
            err(repr(v) + " has too many properties");
        if (s.contains("required"))
            for (const auto & r : s["required"])
                if (!v.contains(r.get<std::string>())) err(repr(r) + " is a required property");
        const json props = s.value("properties", json::object());
        std::vector<std::string> extra;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (s.contains("propertyNames")) {
                std::vector<Error> sub;
                check(json(it.key()), s["propertyNames"], path, sub);
                for (auto & e : sub) out.push_back({path, e.msg});
            }
            path.push_back(it.key());
            if (props.contains(it.key())) check(it.value(), props[it.key()], path, out);
            else if (s.contains("additionalProperties")) {
                const json & ap = s["additionalProperties"];
                if (ap.is_boolean()) {
                    if (!ap.get<bool>()) extra.push_back(it.key());
                } else {
                    check(it.value(), ap, path, out);
                }
            }
            path.pop_back();
        }
        if (!extra.empty()) {
            std::string names;
            for (const auto & e : extra) names += (names.empty() ? "" : ", ") + repr(e);
            err("Additional properties are not allowed (" + names + (extra.size() == 1 ? " was" : " were") + " unexpected)");
        }
    }
}

}  // namespace

std::vector<std::string> schema_errors(const json & data, const json & s, size_t limit) {
    std::vector<Error> errs;
    std::vector<std::string> path;
    check(data, s, path, errs);
    std::stable_sort(errs.begin(), errs.end(), [](const Error & a, const Error & b) { return a.path < b.path; });
    std::vector<std::string> out;
    for (size_t i = 0; i < errs.size() && i < limit; ++i) {
        std::string where;
        for (const auto & p : errs[i].path) where += (where.empty() ? "" : "/") + p;
        out.push_back((where.empty() ? "(root)" : where) + ": " + errs[i].msg);
    }
    return out;
}

std::vector<std::string> direction_errors(const json & direction, const std::vector<Span> & spans) {
    auto errs = schema_errors(direction, schema("direction"));
    if (!errs.empty()) return errs;
    std::map<int, const Span *> ids;
    for (const auto & s : spans) ids[s.id] = &s;
    std::set<int> seen;
    const json & cast = direction["cast"];
    const json & labels = direction["labels"];
    for (size_t i = 0; i < labels.size(); ++i) {
        const int sid = labels[i]["id"].get<int>();
        const std::string who = labels[i]["speaker"].get<std::string>();
        const std::string at = "labels/" + std::to_string(i) + ": ";
        if (!ids.count(sid)) errs.push_back(at + "span id " + std::to_string(sid) + " does not exist");
        else if (seen.count(sid)) errs.push_back(at + "span id " + std::to_string(sid) + " labelled twice");
        seen.insert(sid);
        if (!cast.contains(who) && !is_special_speaker(who)) errs.push_back(at + "speaker '" + who + "' is not in cast");
    }
    std::vector<int> missing;
    for (const auto & s : spans)
        if (s.kind == "dialogue" && !seen.count(s.id)) missing.push_back(s.id);
    if (!missing.empty()) {
        std::string m;
        for (size_t i = 0; i < missing.size() && i < 30; ++i) m += (i ? ", " : "") + std::to_string(missing[i]);
        errs.push_back("dialogue spans without a label: [" + m + "]" + (missing.size() > 30 ? " ..." : ""));
    }
    std::set<std::string> used;
    for (const auto & l : labels) used.insert(l["speaker"].get<std::string>());
    std::vector<std::string> unused;
    for (auto it = cast.begin(); it != cast.end(); ++it)
        if (!used.count(it.key())) unused.push_back(it.key());
    if (!unused.empty()) {
        std::string m;
        for (size_t i = 0; i < unused.size() && i < 10; ++i) m += (i ? ", " : "") + ("'" + unused[i] + "'");
        errs.push_back("cast entries never used as speaker: [" + m + "]");
    }
    return errs;
}

json parse_json_text(const std::string & text) {
    std::string t = strip(text);
    if (t.rfind("```", 0) == 0) {
        size_t a = 0, b = t.size();
        while (a < b && t[a] == '`') ++a;
        while (b > a && t[b - 1] == '`') --b;
        t = t.substr(a, b - a);
        const size_t nl = t.find('\n');
        if (nl != std::string::npos) t = t.substr(nl + 1);
    }
    const size_t start = t.find('{'), end = t.rfind('}');
    if (start == std::string::npos) throw std::invalid_argument("reply contains no JSON object");
    if (end == std::string::npos || end <= start)
        throw std::invalid_argument("invalid JSON: the object is truncated (no closing brace) - return the complete object");
    try {
        return json::parse(t.substr(start, end - start + 1));
    } catch (const json::parse_error & e) {
        throw std::invalid_argument(std::string("invalid JSON: ") + e.what());
    }
}

}  // namespace rm
