#include "directors.h"

#include "llm_presets.h"

#include "net.h"
#include "pipeline.h"
#include "resources.h"
#include "tokenizer.h"
#include "validate.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace rm {

using ojson = nlohmann::ordered_json;
using Check = std::function<std::vector<std::string>(const json &)>;

namespace {

constexpr int kMaxAttempts = 3;

std::string join(const std::vector<std::string> & v, const std::string & sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

std::string py_list(const std::vector<std::string> & v) {   // Python repr of a list of str
    std::vector<std::string> q;
    for (const auto & s : v) q.push_back("'" + s + "'");
    return "[" + join(q, ", ") + "]";
}

std::string py_list(const std::vector<int> & v) {
    std::vector<std::string> q;
    for (int i : v) q.push_back(std::to_string(i));
    return "[" + join(q, ", ") + "]";
}

std::string lower(const std::string & s) { return booknlp::py_lower(s); }

std::string truncate_cp(const std::string & s, size_t n) {
    const auto u = booknlp::utf8_to_u32(s);
    return u.size() <= n ? s : booknlp::u32_to_utf8(u.substr(0, n));
}

// ---------------------------------------------------------------- validated chat

class Chat {
public:
    virtual ~Chat() = default;
    virtual std::string complete(const json & messages, const json & schema, int max_tokens) = 0;

    // one validated request: parse -> JSON Schema -> semantic check; on failure send the errors back
    json ask(const std::string & user, const json & schema_obj, const Check & check, const Log & log,
             const std::string & what, int max_tokens, const std::string & system = "director.md") {
        json messages = json::array({{{"role", "system"}, {"content", resource(system)}},
                                     {{"role", "user"}, {"content", user}}});
        std::vector<std::string> errors;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            const std::string reply = complete(messages, schema_obj, max_tokens);
            json data;
            try {
                data = parse_json_text(reply);
                errors = schema_errors(data, schema_obj);
                if (errors.empty() && check) errors = check(data);
            } catch (const std::invalid_argument & ex) {
                errors = {ex.what()};
            }
            if (errors.empty()) return data;
            log(what + ": attempt " + std::to_string(attempt) + " invalid (" + std::to_string(errors.size()) +
                " errors: " + errors[0] + ")");
            std::string fix = "Your JSON is invalid:\n- ";
            for (size_t i = 0; i < errors.size() && i < 15; ++i) fix += (i ? "\n- " : "") + errors[i];
            fix += "\nReturn the complete corrected JSON object only, in exactly the required format.";
            messages.push_back({{"role", "assistant"}, {"content", reply.substr(0, 20000)}});
            messages.push_back({{"role", "user"}, {"content", fix}});
        }
        std::vector<std::string> first(errors.begin(), errors.begin() + std::min<size_t>(5, errors.size()));
        throw DirectorError(what + ": still invalid after " + std::to_string(kMaxAttempts) + " attempts: " + py_list(first));
    }
};

class LocalChat : public Chat {
public:
    LocalChat(Engines & e, std::string preset, float temperature = 0.2f) : e_(e), preset_(std::move(preset)), temperature_(temperature) {}
    std::string complete(const json & messages, const json & schema, int max_tokens) override {
        return e_.chat(messages, schema, max_tokens, temperature_, preset_);
    }

private:
    Engines & e_;
    std::string preset_;
    float temperature_;
};

bool rejects_structured_output(std::string body) {
    body = lower(body);
    for (const char * k : {"response_format", "json_schema", "structured output", "grammar"})
        if (body.find(k) != std::string::npos) return true;
    return false;
}

class OpenAIChat : public Chat {
public:
    explicit OpenAIChat(const DirectorProfile & p) : model_(p.model), key_(p.api_key), schema_ok_(p.json_schema) {
        url_ = p.base_url;
        while (!url_.empty() && url_.back() == '/') url_.pop_back();
        if (url_.size() < 3 || url_.compare(url_.size() - 3, 3, "/v1") != 0) url_ += "/v1";
    }
    std::string complete(const json & messages, const json & schema, int max_tokens) override {
        json body = {{"model", model_}, {"temperature", 0.2}, {"max_tokens", max_tokens}, {"messages", messages},
                     {"chat_template_kwargs", {{"enable_thinking", false}}}};
        if (schema_ok_)
            body["response_format"] = {{"type", "json_schema"}, {"json_schema", {{"name", "out"}, {"schema", schema}}}};
        std::map<std::string, std::string> headers;
        if (!key_.empty()) headers["Authorization"] = "Bearer " + key_;
        auto r = http_post(url_ + "/chat/completions", body.dump(), "application/json", headers, 1800);
        if ((r.status == 400 || r.status == 422) && schema_ok_ && rejects_structured_output(r.body)) {
            // the endpoint can't constrain output: plain JSON replies (still validated by ask()) from now on
            schema_ok_ = false;
            body.erase("response_format");
            r = http_post(url_ + "/chat/completions", body.dump(), "application/json", headers, 1800);
        }
        if (!r.error.empty()) throw DirectorError("director endpoint " + url_ + ": " + r.error);
        if (r.status < 200 || r.status >= 300)
            throw DirectorError("director endpoint HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 300));
        const json j = json::parse(r.body);
        const json & c = j["choices"][0]["message"]["content"];
        return c.is_string() ? c.get<std::string>() : "";
    }

private:
    std::string url_, model_, key_;
    bool schema_ok_;
};

// ---------------------------------------------------------------- passes

std::string known_cast_text(const std::vector<json> & cast) {
    std::vector<std::string> rows;
    for (const auto & c : cast) {
        std::string r = "- " + c["name"].get<std::string>() + " (" + c["gender"].get<std::string>() + ", " +
                        c["age"].get<std::string>() + ")";
        std::vector<std::string> al = c["aliases"].get<std::vector<std::string>>();
        if (!al.empty()) r += ", also called: " + join(al, ", ");
        rows.push_back(r);
    }
    return rows.empty() ? "(none yet)" : join(rows, "\n");
}

// name -> {gender, age, voice, aliases}
ojson cast_pass(Chat & chat, const std::string & markup, const std::vector<json> & known, const Log & log,
                const std::vector<std::string> & only = {}) {
    const std::string ask = "Output only the cast: every character who speaks at least one D span" +
                            (only.empty() ? std::string() : " - specifically these names, spelled exactly: " + join(only, ", ")) + ".";
    Check check = [&](const json & d) {
        std::vector<std::string> names, errs;
        for (const auto & c : d["cast"]) names.push_back(c["name"].get<std::string>());
        for (size_t i = 0; i < names.size(); ++i)
            if (is_special_speaker(names[i])) errs.push_back("cast/" + std::to_string(i) + ": '" + names[i] + "' is a reserved name");
        std::set<std::string> dup;
        for (const auto & n : names)
            if (std::count(names.begin(), names.end(), n) > 1) dup.insert(n);
        if (!dup.empty()) errs.push_back("duplicate names: " + py_list(std::vector<std::string>(dup.begin(), dup.end())));
        std::vector<std::string> miss;
        for (const auto & n : only)
            if (std::find(names.begin(), names.end(), n) == names.end()) miss.push_back(n);
        if (!miss.empty()) errs.push_back("missing required names: " + py_list(miss));
        return errs;
    };
    const json res = chat.ask("## KNOWN CAST\n" + known_cast_text(known) + "\n\n## CHAPTER\n" + markup + "\n\n" + ask +
                                  " Output JSON {\"cast\": [...]} only.",
                              schema("cast_pass"), check, log, "cast pass", 6000);
    ojson out = ojson::object();
    for (const auto & c : res["cast"])
        out[c["name"].get<std::string>()] = {{"gender", c["gender"]}, {"age", c["age"]}, {"voice", c["voice"]}, {"aliases", c["aliases"]}};
    return out;
}

ojson assemble(const ojson & cast, const json & labels, const std::vector<json> & known) {
    std::vector<std::string> used;
    for (const auto & l : labels) {
        const std::string sp = l["speaker"].get<std::string>();
        if (!is_special_speaker(sp) && std::find(used.begin(), used.end(), sp) == used.end()) used.push_back(sp);
    }
    ojson out = ojson::object();
    for (const auto & n : used) {
        const ojson * e = cast.contains(n) ? &cast[n] : nullptr;
        ojson from_known;
        if (!e)
            for (const auto & k : known)
                if (k["name"] == n) {
                    from_known = ojson::parse(k.dump());
                    e = &from_known;
                }
        if (!e) continue;
        // "voice" here is the producer's description; a known character's row keeps it in voice_hint ("voice" is the pool id)
        std::string voice;
        if (!from_known.is_null()) voice = from_known.value("voice_hint", "");
        else if (e->contains("voice") && (*e)["voice"].is_string()) voice = (*e)["voice"].get<std::string>();
        ojson aliases = ojson::array();
        if (e->contains("aliases"))
            for (const auto & a : (*e)["aliases"])
                if (aliases.size() < 8) aliases.push_back(a);
        out[n] = {{"gender", (*e)["gender"]}, {"age", (*e)["age"]}, {"voice", voice}, {"aliases", aliases}};
    }
    return ojson{{"schema", kDirectionId}, {"cast", out}, {"labels", ojson::parse(labels.dump())}};
}

// Whatever the director could not resolve gets code-generated (always valid) values, each recorded as a
// quality issue: unlabelled dialogue -> the neutral 'Unknown' voice; speakers without gender/age ->
// ModernBookNLP's pronoun gender (or male), adult.
json finalize(ojson cast, json labels, const std::vector<Span> & spans, const std::vector<json> & known, const Log & log,
              const std::map<std::string, std::string> & gender_hint = {}, std::vector<std::string> issues = {}) {
    std::set<int> labelled;
    for (const auto & l : labels) labelled.insert(l["id"].get<int>());
    std::vector<int> missing;
    for (const auto & s : spans)
        if (s.kind == "dialogue" && !labelled.count(s.id)) missing.push_back(s.id);
    std::set<std::string> known_names;
    for (const auto & k : known) known_names.insert(k["name"].get<std::string>());
    std::set<std::string> uncast;
    for (const auto & l : labels) {
        const std::string sp = l["speaker"].get<std::string>();
        if (!cast.contains(sp) && !known_names.count(sp) && !is_special_speaker(sp)) uncast.insert(sp);
    }
    for (int id : missing) labels.push_back({{"id", id}, {"speaker", "Unknown"}});
    for (const auto & n : uncast) {
        auto g = gender_hint.find(n);
        cast[n] = {{"gender", g == gender_hint.end() ? "male" : g->second}, {"age", "adult"}, {"voice", ""}, {"aliases", ojson::array()}};
    }
    if (!missing.empty())
        issues.push_back(std::to_string(missing.size()) + " dialogue lines read by the neutral voice (speaker not identified)");
    if (!uncast.empty()) issues.push_back("gender/age guessed for: " + join(std::vector<std::string>(uncast.begin(), uncast.end()), ", "));
    for (const auto & i : issues) log("QUALITY " + i);
    ojson direction = assemble(cast, labels, known);
    const auto errs = direction_errors(json::parse(direction.dump()), spans);
    if (!errs.empty()) {   // impossible after filling gaps; never render an invalid direction
        std::vector<std::string> first(errs.begin(), errs.begin() + std::min<size_t>(5, errs.size()));
        throw DirectorError("direction failed validation: " + join(first, "; "));
    }
    json out = json::parse(direction.dump());
    out["quality_issues"] = issues;
    return out;
}

// ---------------------------------------------------------------- directors

// match dialogue spans to ModernBookNLP quotes in reading order by text similarity
}  // namespace

std::map<int, std::string> align_quotes(const std::vector<Span> & spans,
                                        const std::vector<std::pair<std::string, std::string>> & quotes) {
    std::vector<std::pair<std::string, std::string>> qs;
    for (const auto & [q, who] : quotes) qs.emplace_back(norm_words(q), who);
    std::map<int, std::string> labels;
    size_t qi = 0;
    for (const auto & s : spans) {
        if (s.kind != "dialogue") continue;
        const std::string target = norm_words(s.text);
        double best = 0.0;
        long best_j = -1;
        for (size_t j = qi; j < std::min(qi + 12, qs.size()); ++j) {
            const double r = !target.empty() && qs[j].first.find(target) != std::string::npos ? 0.95 : seq_ratio(target, qs[j].first);
            if (r > best) {
                best = r;
                best_j = static_cast<long>(j);
            }
        }
        if (best_j >= 0 && best >= 0.6) {
            labels[s.id] = qs[best_j].second;
            qi = qs[best_j].first.size() > target.size() + 5 ? best_j : best_j + 1;
        } else {
            labels[s.id] = "Unknown";
        }
    }
    return labels;
}

std::vector<Span> analysis_spans(const json & analysis) {
    std::vector<Span> out;
    for (const auto & s : analysis.at("spans"))
        out.push_back({s["id"].get<int>(), s["para"].get<int>(), s["kind"].get<std::string>(), s["text"].get<std::string>()});
    return out;
}

namespace {

// ---------------------------------------------------------------- speaker check: agree or resolve
//
// ModernBookNLP (step 1) and the LLM label every quote independently; where they agree the label stands
// (98% right in our tests), where they disagree the LLM is asked a narrow question: which of the two
// (or someone unnamed / not speech). On 8 chapters of 6 novels this took the local 4B model from 85% to
// ~89% of quotes right. Quotes the user set in step 1 ("by": "user") are never touched.

std::string norm_name(const std::string & s) {   // lower-case letters and digits only
    std::string out;
    for (char ch : lower(s))
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (static_cast<unsigned char>(ch) >= 0x80)) out += ch;
    return out;
}

// "Mr. Collins" / "Collins", "Follower-Of-Horizons" / "Follower - Of - Horizons": the same speaker
bool same_speaker(const std::string & a, const std::string & b, const std::map<std::string, std::string> & alias_map) {
    auto canon = [&](const std::string & s) {
        auto it = alias_map.find(lower(s));
        return norm_name(it == alias_map.end() ? s : it->second);
    };
    const std::string x = canon(a), y = canon(b);
    if (x.empty() || y.empty()) return false;
    return x == y || x.rfind(y, 0) == 0 || y.rfind(x, 0) == 0 ||
           (x.size() >= 3 && y.size() >= 3 && (x.size() > y.size() ? x.find(y) : y.find(x)) != std::string::npos);
}

// paragraphs [lo, hi] as prompt text; `mark(id)` renders a dialogue span's prefix (empty: plain quote)
std::string excerpt(const json & spans, int lo, int hi, const std::function<std::string(int)> & mark) {
    std::string out;
    int cur = -1;
    for (const auto & s : spans) {
        const int para = s["para"].get<int>();
        if (para < lo || para > hi) continue;
        if (para != cur) {
            out += (out.empty() ? "" : "\n\n") + std::string("[P") + std::to_string(para) + "]";
            cur = para;
        }
        const std::string text = s["text"].get<std::string>();
        if (s["kind"] != "dialogue") out += " " + text;
        else out += " " + mark(s["id"].get<int>()) + "\xE2\x80\x9C" + text + "\xE2\x80\x9D";
    }
    return out;
}

std::string display_speaker(std::string who, const std::map<std::string, std::string> & alias_map) {
    who = strip(who);
    auto k = alias_map.find(lower(who));
    if (k != alias_map.end()) return k->second;
    if (lower(who) == "narrator") return "Narrator";
    if (lower(who) == "unknown") return "Unknown";
    who = truncate_cp(who, 40);   // an unnamed extra: "the EMT" -> "The EMT"
    if (!who.empty() && who[0] >= 'a' && who[0] <= 'z') who[0] = static_cast<char>(who[0] - 'a' + 'A');
    return who;
}

// 1. blind labels for every unlocked quote, in windows of 20 paragraphs (6 before / 2 after as context)
std::map<int, std::string> label_blind(Chat & chat, const json & analysis, const std::set<int> & locked,
                                       const std::map<int, std::string> & labels, const std::string & cast_line,
                                       const Log & log) {
    const json & spans = analysis["spans"];
    int n_para = 0;
    for (const auto & s : spans) n_para = std::max(n_para, s["para"].get<int>() + 1);
    std::map<int, std::string> out;
    std::function<void(int, int)> window = [&](int a, int b) {   // paragraphs [a, b)
        std::vector<int> ids;
        for (const auto & s : spans) {
            const int p = s["para"].get<int>(), id = s["id"].get<int>();
            if (s["kind"] == "dialogue" && p >= a && p < b && !locked.count(id)) ids.push_back(id);
        }
        if (ids.empty()) return;
        const std::set<int> asked(ids.begin(), ids.end());
        const std::string text = excerpt(spans, std::max(0, a - 6), b + 1, [&](int id) {
            if (asked.count(id)) return "<<S" + std::to_string(id) + ">>";
            if (locked.count(id)) return "[" + labels.at(id) + "]";   // the user's word
            return std::string();
        });
        json sc = schema("label");
        sc["properties"]["answers"]["items"]["properties"]["id"]["enum"] = ids;
        sc["properties"]["answers"]["minItems"] = ids.size();
        sc["properties"]["answers"]["maxItems"] = ids.size();
        Check check = [&](const json & d) {
            std::vector<int> bad;
            for (int id : ids) {
                int n = 0;
                for (const auto & x : d["answers"]) n += x["id"].get<int>() == id;
                if (n != 1) bad.push_back(id);
            }
            return bad.empty() ? std::vector<std::string>{}
                               : std::vector<std::string>{"label each marked quote exactly once; wrong for span ids " + py_list(bad)};
        };
        const std::string what = "speaker labels P" + std::to_string(a) + "-P" + std::to_string(b - 1);
        try {
            const json res = chat.ask("## CHARACTERS (detected, may be incomplete)\n" + cast_line + "\n\n## EXCERPT\n" + text +
                                          "\n\nLabel the speaker of every marked quote. Output JSON {\"answers\": [{\"id\", \"speaker\"}]}.",
                                      sc, check, log, what, 40 * static_cast<int>(ids.size()) + 200, "label.md");
            for (const auto & x : res["answers"]) out[x["id"].get<int>()] = x["speaker"].get<std::string>();
        } catch (const DirectorError & ex) {
            if (b - a <= 1) {
                log(what + ": no labels (" + ex.what() + ")");
                return;
            }
            const int mid = (a + b) / 2;
            log(what + ": splitting");
            window(a, mid);
            window(mid, b);
        }
    };
    for (int a = 0; a < n_para; a += 20) {
        window(a, std::min(n_para, a + 20));
        log("speaker check: labelled paragraphs up to P" + std::to_string(std::min(n_para, a + 20) - 1) + " of " + std::to_string(n_para));
    }
    return out;
}

struct Resolution {
    int id;
    std::string detected, blind, chosen, evidence;
    std::string level;   // for the user: "check" (no speech tag, ~1 in 3 wrong) or "likely" (tag or paragraph rule)
};

// speech tags next to a quote: '"..." James said', '"..." said the guard', 'He asked, "..."'
const std::string kSpeechVerbs =
    "(said|says|say|asked|asks|replied|replies|answered|added|continued|called|shouted|yelled|whispered|muttered|murmured|"
    "cried|exclaimed|snapped|told|offered|admitted|commented|insisted|protested|sighed|groaned|laughed|hissed|growled|barked|"
    "began|started|interrupted|repeated|agreed|corrected|explained|noted|demanded|pleaded|uttered|intoned|chuckled|grumbled|"
    "mumbled|breathed|spoke)";
bool tag_after(const std::string & t) {
    static const std::regex re("^\W*((\w+(\s+\w+){0,3})\s+" + kSpeechVerbs + "\b|" + kSpeechVerbs + "\s+\w+)", std::regex::icase);
    return std::regex_search(t, re);
}
bool tag_before(const std::string & t) {
    static const std::regex re("(\w+(\s+\w+){0,3})\s+" + kSpeechVerbs + "\b[^.!?]*[:,]\s*$", std::regex::icase);
    return std::regex_search(t, re);
}
std::string first_sentence(const std::string & t) {
    std::smatch m;
    static const std::regex re(R"(^\s*([\s\S]*?[.!?])(\s|$))");
    const std::string s = std::regex_search(t, m, re) ? m[1].str() : strip(t);
    return truncate_cp(s, 160);
}
std::string last_sentence(const std::string & t) {
    const std::string s = strip(t);
    size_t cut = 0;
    for (size_t i = 0; i + 1 < s.size(); ++i)
        if ((s[i] == '.' || s[i] == '!' || s[i] == '?') && s[i + 1] == ' ') cut = i + 2;
    const auto u = booknlp::utf8_to_u32(s.substr(cut));
    return booknlp::u32_to_utf8(u.size() > 160 ? u.substr(u.size() - 160) : u);
}

// 2. where step 1 and the blind labels disagree: which of the two (or someone unnamed / not speech)
std::vector<Resolution> resolve_disagreements(Chat & chat, const json & analysis, std::map<int, std::string> & labels,
                                              const std::map<int, std::string> & blind, const std::set<int> & locked,
                                              const std::map<std::string, std::string> & alias_map,
                                              const std::set<std::string> & cast_norm, const Log & log,
                                              std::vector<std::string> & issues) {
    const json & spans = analysis["spans"];
    std::map<int, int> para_of;
    for (const auto & s : spans) para_of[s["id"].get<int>()] = s["para"].get<int>();
    std::set<int> agreed;
    std::vector<int> todo;
    for (const auto & [id, b] : blind) {
        if (same_speaker(labels.at(id), b, alias_map)) agreed.insert(id);
        else todo.push_back(id);
    }
    log("speaker check: " + std::to_string(agreed.size()) + " agree, " + std::to_string(todo.size()) + " to resolve");
    std::vector<Resolution> out;
    int failed = 0;
    std::map<int, std::vector<const json *>> by_para;   // spans of each paragraph, in order
    for (const auto & s : spans) by_para[s["para"].get<int>()].push_back(&s);
    auto settled = [&](int sid) { return locked.count(sid) || agreed.count(sid); };
    for (size_t i = 0; i < todo.size(); ++i) {
        const int id = todo[i], p = para_of[id];
        const std::string detected = labels.at(id), other = display_speaker(blind.at(id), alias_map);
        // clues from the text around the quote
        const auto & ps = by_para[p];
        size_t at = 0;
        while (at < ps.size() && (*ps[at])["id"].get<int>() != id) ++at;
        const std::string before = at > 0 && (*ps[at - 1])["kind"] != "dialogue" ? (*ps[at - 1])["text"].get<std::string>() : "";
        const std::string after = at + 1 < ps.size() && (*ps[at + 1])["kind"] != "dialogue" ? (*ps[at + 1])["text"].get<std::string>() : "";
        const bool has_tag = tag_after(after) || tag_before(before);
        std::vector<std::string> same_para;
        for (const json * s : ps)
            if ((*s)["kind"] == "dialogue" && (*s)["id"].get<int>() != id && settled((*s)["id"].get<int>()))
                same_para.push_back(labels.at((*s)["id"].get<int>()));
        std::string prev = "none";
        for (int q = p - 1; q >= std::max(0, p - 3) && prev == "none"; --q)
            for (auto it = by_para[q].rbegin(); it != by_para[q].rend(); ++it)
                if ((**it)["kind"] == "dialogue") {
                    const int sid = (**it)["id"].get<int>();
                    prev = settled(sid) ? labels.at(sid) : "?";
                    break;
                }
        // no tag of its own, in a paragraph whose other quotes all have one settled speaker: that speaker
        if (!has_tag && !same_para.empty() && std::all_of(same_para.begin(), same_para.end(), [&](const std::string & x) { return x == same_para[0]; })) {
            out.push_back({id, detected, other, same_para[0], "no speech tag; continues the speaker of its paragraph", "likely"});
            labels[id] = same_para[0];
            continue;
        }
        const std::string text = excerpt(spans, std::max(0, p - 5), p + 2, [&](int sid) -> std::string {
            if (sid == id) return ">>>";
            if (settled(sid)) return "[" + labels.at(sid) + "]";
            return "[?]";
        });
        const std::string clues =
            "- right before the quote: " + (strip(before).empty() ? std::string("(start of paragraph)") : "\xC2\xAB" + last_sentence(before) + "\xC2\xBB") +
            "\n- right after the quote: " + (strip(after).empty() ? std::string("(end of paragraph)") : "\xC2\xAB" + first_sentence(after) + "\xC2\xBB") +
            "\n- other quotes in this paragraph: " + (same_para.empty() ? std::string("none with a known speaker") : join(same_para, ", ")) +
            "\n- previous paragraph's speaker: " + prev;
        auto opt = [](const std::string & s) { return s == "Unknown" ? std::string("someone unnamed") : s; };
        std::vector<std::string> opts{opt(detected), opt(other)};
        if (id % 2) std::swap(opts[0], opts[1]);   // no position bias towards step 1
        for (const char * extra : {"someone unnamed", "not speech"})
            if (std::find(opts.begin(), opts.end(), extra) == opts.end()) opts.push_back(extra);
        opts.erase(std::unique(opts.begin(), opts.end()), opts.end());
        json sc = schema("resolve");
        sc["properties"]["answer"]["enum"] = opts;
        try {
            const json res = chat.ask("Excerpt (known speakers in brackets, [?] = unsure; the quote in question is marked >>>):\n\n" + text +
                                          "\n\nClues:\n" + clues + "\n\nWho speaks the quote marked >>>? Options: " + join(opts, ", ") + ".",
                                      sc, nullptr, log, "resolve S" + std::to_string(id), 300, "resolve.md");
            std::string ans = res["answer"].get<std::string>();
            if (ans == "not speech") ans = "Narrator";
            else if (ans == "someone unnamed")   // keep the blind pass's description of the stranger, if it gave one
                ans = (!cast_norm.count(norm_name(other)) && other != "Unknown" && other != "Narrator") ? other : "Unknown";
            out.push_back({id, detected, other, ans, res["tag"].get<std::string>() + " - " + res["reason"].get<std::string>(),
                           has_tag ? "likely" : "check"});
            labels[id] = ans;
        } catch (const DirectorError & ex) {
            ++failed;
            log("resolve S" + std::to_string(id) + ": failed (" + ex.what() + "); step 1's speaker kept");
        }
        if (i % 10 == 9) log("speaker check: resolved " + std::to_string(i + 1) + " of " + std::to_string(todo.size()));
    }
    if (failed) issues.push_back(std::to_string(failed) + " disputed quotes could not be resolved; step 1's speakers kept");
    return out;
}

// Both producers: step 1's speakers (the user's edits are final) -> speaker check -> cast pass for new characters.
json direct_from_analysis(Chat & label_chat, Chat & cast_chat, const json & analysis, const std::vector<json> & known,
                          const Log & log) {
    if (analysis.value("detector", "") != "modernbooknlp")
        throw DirectorError("this analysis has no speaker detection (made without ModernBookNLP); run step 1 again");
    const std::vector<Span> spans = analysis_spans(analysis);
    const std::string markup = spans_markup(analysis.value("title", ""), spans);
    std::map<std::string, std::string> alias_map;
    std::vector<std::string> cast_names;
    for (const auto & k : known) {
        cast_names.push_back(k["name"].get<std::string>());
        alias_map[lower(k["name"].get<std::string>())] = k["name"].get<std::string>();
        for (const auto & a : k["aliases"]) alias_map[lower(a.get<std::string>())] = k["name"].get<std::string>();
    }
    std::map<std::string, std::string> genders;
    for (const auto & c : analysis.value("characters", json::array())) {
        const std::string n = c["name"].get<std::string>();
        genders[n] = c.value("gender", "male");
        if (!alias_map.count(lower(n))) {
            alias_map[lower(n)] = n;
            cast_names.push_back(n);
        }
    }
    std::set<std::string> cast_norm;
    for (const auto & n : cast_names) cast_norm.insert(norm_name(n));
    std::map<int, std::string> labels;
    std::set<int> locked;
    for (const auto & s : analysis["spans"]) {
        if (s["kind"] != "dialogue") continue;
        const std::string who = s.value("speaker", "Unknown").empty() ? "Unknown" : s.value("speaker", "Unknown");
        auto it = alias_map.find(lower(who));
        labels[s["id"].get<int>()] = truncate_cp(it == alias_map.end() ? who : it->second, 40);
        if (s.value("by", "") == "user") locked.insert(s["id"].get<int>());
    }
    std::vector<std::string> issues;
    json review_log = json::array();
    if (labels.size() > locked.size()) {
        const auto blind = label_blind(label_chat, analysis, locked, labels, join(cast_names, ", "), log);
        for (const auto & r : resolve_disagreements(label_chat, analysis, labels, blind, locked, alias_map, cast_norm, log, issues))
            review_log.push_back({{"id", r.id}, {"from", r.detected}, {"blind", r.blind}, {"to", r.chosen}, {"evidence", r.evidence},
                                  {"level", r.level}, {"resolved", false}});
        int unlabelled = 0;
        for (const auto & [id, sp] : labels) unlabelled += !locked.count(id) && !blind.count(id);
        if (unlabelled) issues.push_back(std::to_string(unlabelled) + " quotes were not checked (the model's replies were invalid); step 1's speakers kept");
    }
    json label_arr = json::array();
    std::set<std::string> used;
    for (const auto & [id, sp] : labels) {
        label_arr.push_back({{"id", id}, {"speaker", sp}});
        if (!is_special_speaker(sp)) used.insert(sp);
    }
    std::set<std::string> known_names;
    for (const auto & k : known) known_names.insert(k["name"].get<std::string>());
    std::vector<std::string> fresh;
    for (const auto & n : used)
        if (!known_names.count(n)) fresh.push_back(n);
    log("script: " + std::to_string(used.size()) + " speakers, " + std::to_string(fresh.size()) + " new");
    ojson cast = ojson::object();
    if (!fresh.empty()) {
        try {
            cast = cast_pass(cast_chat, markup, known, log, fresh);
        } catch (const DirectorError & ex) {
            log(std::string("cast pass failed (") + ex.what() + ")");
            issues.push_back("the producer could not describe the new characters; their gender/age are guessed");
        }
    }
    json out = finalize(cast, label_arr, spans, known, log, genders, issues);
    out["review"] = review_log;
    return out;
}

class LocalDirector : public Director {
public:
    LocalDirector(Engines & e, const LlmPreset & p) : e_(e), p_(p) {}
    json direct(const json & analysis, const std::vector<json> & known, const Log & log) override {
        if (!e_.llm_available(p_.id))
            throw DirectorError("the script producer model (" + p_.title + ") is not installed; " +
                                (p_.required ? "it is downloaded at start; see the banner at the top" : "download it in Settings"));
        LocalChat greedy(e_, p_.id, 0.0f), chat(e_, p_.id);   // labels: greedy (as tested); cast descriptions: default sampling
        return direct_from_analysis(greedy, chat, analysis, known, log);
    }

private:
    Engines & e_;
    const LlmPreset & p_;
};

class LlmDirector : public Director {
public:
    explicit LlmDirector(std::unique_ptr<Chat> chat) : chat_(std::move(chat)) {}
    json direct(const json & analysis, const std::vector<json> & known, const Log & log) override {
        return direct_from_analysis(*chat_, *chat_, analysis, known, log);
    }

private:
    std::unique_ptr<Chat> chat_;
};

}  // namespace

std::unique_ptr<Director> get_director(const Settings & s, const std::string & name, Engines & engines) {
    if (const LlmPreset * lp = find_llm_preset(llm_of_producer(name))) return std::make_unique<LocalDirector>(engines, *lp);
    const DirectorProfile * p = s.director_profile(name);
    if (!p) throw std::invalid_argument("unknown director profile: " + name);
    return std::make_unique<LlmDirector>(std::make_unique<OpenAIChat>(*p));
}

}  // namespace rm
