#include "steps.h"

#include "audio_io.h"
#include "directors.h"
#include "pipeline.h"
#include "scrape.h"
#include "text.h"
#include "tokenizer.h"
#include "unicode.h"
#include "validate.h"

#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

namespace rm {

namespace fs = std::filesystem;

json read_json_file(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("missing " + p.u8string());
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

void write_json_file(const fs::path & p, const json & j) {
    fs::create_directories(p.parent_path());
    const fs::path tmp = p.u8string() + ".tmp";
    std::ofstream(tmp, std::ios::binary) << j.dump(1);
    fs::rename(tmp, p);
}

static std::string read_text(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path Steps::dir(int64_t chapter_id) const { return d_.work_dir() / "chapters" / std::to_string(chapter_id); }

fs::path Steps::line_wav(int64_t cid, int line) const {
    char name[16];
    std::snprintf(name, sizeof name, "%04d.wav", line);
    return dir(cid) / "lines" / name;
}

json Steps::chapter(int64_t id) const {
    auto ch = db_.one("SELECT * FROM chapters WHERE id=?", {id});
    if (!ch) throw std::invalid_argument("unknown chapter " + std::to_string(id));
    return *ch;
}

// ---------------------------------------------------------------- 1. analysis

void Steps::analyze(int64_t cid, const StepLog & log) {
    const json ch = chapter(cid);
    const json novel = *db_.one("SELECT * FROM novels WHERE id=?", {ch["novel_id"]});
    const Source & src = sources_.at(novel["source"].get<std::string>());
    log("fetching " + ch["url"].get<std::string>(), "fetch", 0.05);
    const ChapterText text = parse_chapter(fetch(ch["url"].get<std::string>(), src), src);
    if (text.paragraphs.empty()) throw std::runtime_error("no chapter text found (the site plugin's selectors may need updating)");
    const std::string title = text.title.empty() ? ch["title"].get<std::string>() : text.title;
    const auto spans = split_spans(text.paragraphs);
    int n_dialogue = 0;
    for (const auto & s : spans) n_dialogue += s.kind == "dialogue";
    log(std::to_string(text.paragraphs.size()) + " paragraphs, " + std::to_string(spans.size()) + " spans (" +
            std::to_string(n_dialogue) + " dialogue)", "analyze", 0.2);

    if (!engines_.booknlp_available())
        throw std::runtime_error("the speaker detection models (ModernBookNLP) are not downloaded yet; they download at start (see the banner at the top)");
    json analysis = {{"version", 1}, {"title", title}, {"url", ch["url"]}, {"paragraphs", text.paragraphs}};
    log("ModernBookNLP: detecting speakers", "analyze", 0.3);
    std::string joined;
    for (size_t i = 0; i < text.paragraphs.size(); ++i) joined += (i ? "\n\n" : "") + text.paragraphs[i];
    const booknlp::Result bn = engines_.booknlp(joined + "\n");
    const std::map<int, std::string> speakers = align_quotes(spans, bn.speaker_quotes);
    json characters = json::array();
    for (const auto & [name, gender] : bn.speaker_genders) characters.push_back({{"name", name}, {"gender", gender}});
    analysis["detector"] = "modernbooknlp";
    log("ModernBookNLP: " + std::to_string(characters.size()) + " named characters", "analyze", 0.9);
    json js = json::array();
    for (const auto & s : spans) {
        json o = {{"id", s.id}, {"para", s.para}, {"kind", s.kind}, {"text", s.text}};
        if (s.kind == "dialogue") {
            auto it = speakers.find(s.id);
            o["speaker"] = it == speakers.end() ? "Unknown" : it->second;
        }
        js.push_back(o);
    }
    // quotes the detection is unsure about: the producer's speaker review (or the user) resolves them
    std::map<int, std::set<std::string>> para_speakers;
    for (const auto & o : js)
        if (o["kind"] == "dialogue") para_speakers[o["para"].get<int>()].insert(o["speaker"].get<std::string>());
    for (auto & o : js) {
        if (o["kind"] != "dialogue") continue;
        if (o["speaker"] == "Unknown") o["check"] = "no speaker detected";
        else if (para_speakers[o["para"].get<int>()].size() > 1) o["check"] = "different speakers in one paragraph";
    }
    analysis["spans"] = js;
    analysis["characters"] = characters;
    write_json_file(analysis_path(cid), analysis);
    db_.run("UPDATE chapters SET analyzed_at=? WHERE id=?", {now(), cid});
}

void Steps::save_analysis(int64_t cid, const json & a) {
    chapter(cid);
    std::vector<std::string> errs;
    if (!a.is_object() || !a.contains("spans") || !a["spans"].is_array() || !a.contains("paragraphs") || !a["paragraphs"].is_array())
        throw std::invalid_argument("analysis needs \"paragraphs\" and \"spans\" arrays");
    std::set<int> ids;
    for (size_t i = 0; i < a["spans"].size(); ++i) {
        const json & s = a["spans"][i];
        const std::string at = "spans/" + std::to_string(i) + ": ";
        if (!s.contains("id") || !s["id"].is_number_integer()) { errs.push_back(at + "id must be an integer"); continue; }
        if (!ids.insert(s["id"].get<int>()).second) errs.push_back(at + "duplicate id");
        if (!s.contains("para") || !s["para"].is_number_integer()) errs.push_back(at + "para must be an integer");
        if (s.value("kind", "") != "narration" && s.value("kind", "") != "dialogue") errs.push_back(at + "kind must be narration or dialogue");
        if (!s.contains("text") || !s["text"].is_string() || strip(s["text"].get<std::string>()).empty()) errs.push_back(at + "text must not be empty");
        if (s.value("kind", "") == "dialogue" && (!s.contains("speaker") || !s["speaker"].is_string()))
            errs.push_back(at + "dialogue needs a speaker");
    }
    if (!errs.empty()) {
        std::string m;
        for (size_t i = 0; i < errs.size() && i < 10; ++i) m += (i ? "; " : "") + errs[i];
        throw std::invalid_argument(m);
    }
    json out = read_json_file(analysis_path(cid));
    for (const char * k : {"title", "paragraphs", "characters"})
        if (a.contains(k)) out[k] = a[k];
    // spans in reading order, renumbered: the user may have split ("make quote") or merged ("not a quote") them
    json spans = a["spans"];
    for (size_t i = 0; i < spans.size(); ++i) spans[i]["id"] = static_cast<int>(i);
    out["spans"] = spans;
    write_json_file(analysis_path(cid), out);
    db_.run("UPDATE chapters SET analyzed_at=? WHERE id=?", {now(), cid});
}

// ---------------------------------------------------------------- 2. script

// disputed speakers (speaker check) the user has not looked at yet, of the "check" kind (~1 in 3 wrong)
static int to_check(const json & script) {
    int n = 0;
    for (const auto & r : script.value("review", json::array())) n += r.value("level", "check") == "check" && !r.value("resolved", false);
    return n;
}

void Steps::cast_new_characters(int64_t nid, const json & script, int position, const Pool & pool,
                                 const std::function<void(const std::string &)> & log) {
    const auto known = db_.cast(nid);
    std::set<std::string> known_names;
    for (const auto & c : known) known_names.insert(c["name"].get<std::string>());
    std::map<std::string, int> counts;
    for (const auto & l : script["lines"])
        if (l.value("kind", "") == "dialogue") counts[l["speaker"].get<std::string>()] += 1;
    std::vector<std::pair<std::string, json>> fresh;
    for (auto it = script["cast"].begin(); it != script["cast"].end(); ++it)
        if (!known_names.count(it.key()) && counts[it.key()] > 0) fresh.emplace_back(it.key(), it.value());
    const auto novel = *db_.one("SELECT narrator FROM novels WHERE id=?", {nid});
    const auto picks = assign(pool, known, novel["narrator"].is_string() ? novel["narrator"].get<std::string>() : "", fresh, counts);
    for (const auto & [name, e] : fresh) {
        auto p = picks.find(name);
        if (p == picks.end()) continue;
        db_.upsert_character(nid, name, {{"aliases", e.value("aliases", json::array())}, {"gender", e.value("gender", "male")},
                                         {"age", e.value("age", "adult")}, {"pitch", pitch_from_director(e)},
                                         {"voice_hint", e.value("voice", "")}, {"voice", p->second},
                                         {"first_chapter", position}, {"lines", 0}});
        log("new character " + name + " (" + e.value("gender", "") + ", " + e.value("age", "") + ") -> voice " + p->second);
    }
    for (const auto & c : known) {   // aliases the producer found for known characters
        const std::string name = c["name"].get<std::string>();
        if (!script["cast"].contains(name)) continue;
        json aliases = c["aliases"];
        for (const auto & a : script["cast"][name].value("aliases", json::array())) aliases.push_back(a);
        db_.upsert_character(nid, name, {{"aliases", aliases}});
    }
}

void Steps::recount_lines(int64_t nid) {
    std::map<std::string, int> counts;
    for (const auto & ch : db_.all("SELECT id FROM chapters WHERE novel_id=? AND scripted_at IS NOT NULL", {nid})) {
        const fs::path p = script_path(ch["id"].get<int64_t>());
        if (!fs::exists(p)) continue;
        for (const auto & l : read_json_file(p)["lines"])
            if (l.value("kind", "") == "dialogue") counts[l["speaker"].get<std::string>()] += 1;
    }
    for (const auto & c : db_.cast(nid)) {
        const std::string name = c["name"].get<std::string>();
        db_.upsert_character(nid, name, {{"lines", counts.count(name) ? counts[name] : 0}});
    }
}

void Steps::produce(int64_t cid, const std::string & producer, const StepLog & log) {
    const json ch = chapter(cid);
    const int64_t nid = ch["novel_id"].get<int64_t>();
    if (!fs::exists(analysis_path(cid))) throw std::runtime_error("the chapter has no analysis yet (step 1)");
    const json analysis = read_json_file(analysis_path(cid));
    const Settings s = settings_();
    std::string name = producer;
    if (name.empty()) name = s.default_director;
    log("producer: " + name, "script", 0.1);
    auto director = get_director(s, name, engines_);
    json result = director->direct(analysis, db_.cast(nid), [&](const std::string & m) { log(m, "script", 0.5); });

    std::map<int, std::string> labels;
    for (const auto & l : result["labels"]) labels[l["id"].get<int>()] = l["speaker"].get<std::string>();
    json lines = json::array({{{"id", 0}, {"para", -1}, {"kind", "title"}, {"speaker", "Narrator"}, {"text", analysis.value("title", "")}}});
    for (const auto & sp : analysis["spans"]) {
        const bool dialogue = sp["kind"] == "dialogue";
        std::string who = "Narrator";
        if (dialogue) {
            auto it = labels.find(sp["id"].get<int>());
            who = it == labels.end() ? "Unknown" : it->second;
        }
        lines.push_back({{"id", static_cast<int>(lines.size())}, {"para", sp["para"]}, {"kind", sp["kind"]}, {"speaker", who}, {"text", sp["text"]}});
    }
    const json script = {{"version", 1}, {"producer", name}, {"title", analysis.value("title", "")}, {"cast", result["cast"]},
                         {"lines", lines}, {"quality_issues", result.value("quality_issues", json::array())},
                         {"review", result.value("review", json::array())}};
    write_json_file(script_path(cid), script);
    std::string issues;
    for (const auto & i : script["quality_issues"]) issues += (issues.empty() ? "" : "\n") + i.get<std::string>();
    db_.run("UPDATE chapters SET scripted_at=?, script_producer=?, quality_issues=?, to_check=? WHERE id=?",
            {now(), name, issues, to_check(script), cid});
    const Pool pool(d_.pool_dirs(), d_.voice_overrides());
    if (!pool.usable().empty())
        cast_new_characters(nid, script, ch["position"].get<int>(), pool, [&](const std::string & m) { log(m, "script", 0.95); });
    recount_lines(nid);
}

void Steps::save_script(int64_t cid, const json & sc) {
    const json ch = chapter(cid);
    const int64_t nid = ch["novel_id"].get<int64_t>();
    if (!sc.is_object() || !sc.contains("lines") || !sc["lines"].is_array() || !sc.contains("cast") || !sc["cast"].is_object())
        throw std::invalid_argument("script needs \"lines\" (array) and \"cast\" (object)");
    std::set<std::string> known;
    for (const auto & c : db_.cast(nid)) known.insert(c["name"].get<std::string>());
    std::vector<std::string> errs;
    for (auto it = sc["cast"].begin(); it != sc["cast"].end(); ++it) {
        const json & e = it.value();
        const std::string at = "cast/" + it.key() + ": ";
        if (e.value("gender", "") != "male" && e.value("gender", "") != "female") errs.push_back(at + "gender must be male or female");
        const std::string age = e.value("age", "adult");
        if (age != "child" && age != "teen" && age != "adult" && age != "elder") errs.push_back(at + "age must be child, teen, adult or elder");
    }
    for (size_t i = 0; i < sc["lines"].size(); ++i) {
        const json & l = sc["lines"][i];
        const std::string at = "lines/" + std::to_string(i) + ": ";
        if (!l.contains("text") || !l["text"].is_string() || strip(l["text"].get<std::string>()).empty()) errs.push_back(at + "text must not be empty");
        if (!l.contains("speaker") || !l["speaker"].is_string()) {
            errs.push_back(at + "speaker missing");
            continue;
        }
        const std::string sp = l["speaker"].get<std::string>();
        if (!is_special_speaker(sp) && !sc["cast"].contains(sp) && !known.count(sp))
            errs.push_back(at + "speaker '" + sp + "' is not in the cast");
    }
    if (!errs.empty()) {
        std::string m;
        for (size_t i = 0; i < errs.size() && i < 10; ++i) m += (i ? "; " : "") + errs[i];
        throw std::invalid_argument(m);
    }
    json out = fs::exists(script_path(cid)) ? read_json_file(script_path(cid)) : json{{"version", 1}, {"producer", "you"}};
    // speaker changes (and "confirmed" disputed lines) are the user's word: mark those disputes resolved and carry
    // the speaker into step 1, so producing the script again keeps it. Line n+1 is analysis span n.
    const json old_lines = out.value("lines", json::array());
    json analysis = fs::exists(analysis_path(cid)) ? read_json_file(analysis_path(cid)) : json();
    const bool mapped = analysis.is_object() && analysis.contains("spans") && old_lines.size() == analysis["spans"].size() + 1 &&
                        sc["lines"].size() == old_lines.size();
    bool analysis_changed = false;
    if (mapped) {
        std::map<int, json *> review;
        if (out.contains("review"))
            for (auto & r : out["review"]) review[r["id"].get<int>()] = &r;
        for (size_t i = 1; i < old_lines.size(); ++i) {
            const json & l = sc["lines"][i];
            if (l.value("kind", "") != "dialogue") continue;
            const int sid = static_cast<int>(i) - 1;
            const bool changed = l["speaker"] != old_lines[i]["speaker"];
            if (!changed && !l.value("confirmed", false)) continue;
            if (review.count(sid)) (*review[sid])["resolved"] = true;
            json & span = analysis["spans"][sid];
            if (span.value("kind", "") == "dialogue" && (changed || span.value("speaker", "") != l["speaker"])) {
                span["speaker"] = l["speaker"];
                span["by"] = "user";
                span.erase("check");
                analysis_changed = true;
            }
        }
    }
    if (analysis_changed) write_json_file(analysis_path(cid), analysis);   // mirrored in the script: step 1 stays up to date
    out["cast"] = sc["cast"];
    json lines = json::array();
    for (size_t i = 0; i < sc["lines"].size(); ++i) {
        json l = sc["lines"][i];
        l.erase("confirmed");
        l["id"] = static_cast<int>(i);
        if (!l.contains("kind")) l["kind"] = l["speaker"] == "Narrator" ? "narration" : "dialogue";
        if (!l.contains("para")) l["para"] = i ? sc["lines"][i - 1].value("para", 0) : -1;
        lines.push_back(l);
    }
    out["lines"] = lines;
    if (sc.contains("title")) out["title"] = sc["title"];
    write_json_file(script_path(cid), out);
    db_.run("UPDATE chapters SET scripted_at=?, to_check=? WHERE id=?", {now(), to_check(out), cid});
    const Pool pool(d_.pool_dirs(), d_.voice_overrides());
    if (!pool.usable().empty()) cast_new_characters(nid, out, ch["position"].get<int>(), pool, [](const std::string &) {});
    recount_lines(nid);
}

// ---------------------------------------------------------------- 3. narration

void Steps::narrate(int64_t cid, const StepLog & log) {
    const json ch = chapter(cid);
    const int64_t nid = ch["novel_id"].get<int64_t>();
    if (!fs::exists(script_path(cid))) throw std::runtime_error("the chapter has no script yet (step 2)");
    if (!engines_.tts_available()) throw std::runtime_error("the speech model is not downloaded yet; it downloads at start (see the banner at the top)");
    const json script = read_json_file(script_path(cid));
    const json novel = *db_.one("SELECT * FROM novels WHERE id=?", {nid});
    const Pool pool(d_.pool_dirs(), d_.voice_overrides());
    if (pool.usable().empty()) throw std::runtime_error("the voice pool is empty");
    const std::string narrator = novel["narrator"].is_string() && !novel["narrator"].get<std::string>().empty()
                                     ? novel["narrator"].get<std::string>()
                                     : default_narrator(pool);
    std::map<std::string, std::string> voices;
    for (const auto & c : db_.cast(nid)) voices[c["name"].get<std::string>()] = c["voice"].get<std::string>();

    Audio chapter_audio;
    json timeline = json::array();
    std::optional<int> prev;
    const auto t0 = std::chrono::steady_clock::now();
    const auto & lines = script["lines"];
    fs::create_directories(dir(cid) / "lines");
    for (size_t n = 0; n < lines.size(); ++n) {
        const json & l = lines[n];
        auto v = voices.find(l["speaker"].get<std::string>());
        const std::string vid = v != voices.end() ? v->second : narrator;
        const std::string text = l["text"].get<std::string>();
        const fs::path seg = line_wav(cid, static_cast<int>(n));
        const fs::path meta = fs::path(seg).replace_extension(".key");
        const std::string key = vid + "|" + text;
        Audio a;
        if (fs::exists(seg) && fs::exists(meta) && read_text(meta) == key) {
            a = read_wav16(seg);
        } else {
            const auto [ref_wav, ref_text] = pool.ref(vid);
            write_wav16(seg, engines_.speak(text, ref_wav, ref_text));
            std::ofstream(meta, std::ios::binary) << key;
            a = read_wav16(seg);
        }
        if (!chapter_audio.sample_rate) chapter_audio.sample_rate = a.sample_rate;
        const int para = l.value("para", 0);
        if (prev) chapter_audio.samples.insert(chapter_audio.samples.end(),
                                               static_cast<size_t>(chapter_audio.sample_rate * (para == *prev ? 0.25 : 0.6)), 0.0f);
        const double start = static_cast<double>(chapter_audio.samples.size()) / chapter_audio.sample_rate;
        chapter_audio.samples.insert(chapter_audio.samples.end(), a.samples.begin(), a.samples.end());
        timeline.push_back({{"id", l["id"]}, {"voice", vid}, {"start", start},
                            {"duration", static_cast<double>(a.samples.size()) / a.sample_rate}});
        prev = para;
        if (n % 5 == 0) log("line " + std::to_string(n + 1) + "/" + std::to_string(lines.size()), "narrate",
                            0.05 + 0.9 * static_cast<double>(n + 1) / lines.size());
    }
    // stale line files from a longer earlier script
    for (size_t n = lines.size();; ++n) {
        const fs::path seg = line_wav(cid, static_cast<int>(n));
        if (!fs::exists(seg)) break;
        fs::remove(seg);
        fs::remove(fs::path(seg).replace_extension(".key"));
    }
    const std::string title = script.value("title", ch["title"].get<std::string>());
    std::string safe;   // re.sub(r"[^\w\- ]+", "", title).strip()[:80]
    for (char32_t c : booknlp::utf8_to_u32(title)) {
        const auto f = unicode_cpt_flags_from_cpt(c);
        if (c == U'_' || c == U'-' || c == U' ' || f.is_letter || f.is_number) safe += booknlp::u32_to_utf8(std::u32string(1, c));
    }
    safe = strip(safe);
    if (auto u = booknlp::utf8_to_u32(safe); u.size() > 80) safe = booknlp::u32_to_utf8(u.substr(0, 80));
    if (safe.empty()) safe = "Chapter " + std::to_string(ch["position"].get<int>());
    char prefix[16];
    std::snprintf(prefix, sizeof prefix, "%04d - ", ch["position"].get<int>());
    const fs::path out_dir = d_.audio_dir() / ("novel" + std::to_string(nid));
    fs::create_directories(out_dir);
    const std::string old = ch["audio_path"].is_string() ? ch["audio_path"].get<std::string>() : "";
    const fs::path mp3 = out_dir / fs::u8path(std::string(prefix) + safe + ".mp3");
    int kbps = std::atoi(settings_().mp3_bitrate.c_str());
    if (kbps <= 0) kbps = 96;
    write_mp3(mp3, chapter_audio, kbps,
              {{"title", title}, {"album", novel.value("title", "")}, {"artist", novel.value("author", "")},
               {"track", std::to_string(ch["position"].get<int>())}});
    if (!old.empty() && fs::u8path(old) != mp3 && fs::exists(fs::u8path(old))) fs::remove(fs::u8path(old));
    const double secs = chapter_audio.sample_rate ? static_cast<double>(chapter_audio.samples.size()) / chapter_audio.sample_rate : 0.0;
    write_json_file(narration_path(cid), {{"version", 1}, {"duration", secs}, {"lines", timeline}});
    db_.run("UPDATE chapters SET audio_path=?, duration_s=?, narrated_at=?, error='' WHERE id=?", {mp3.u8string(), secs, now(), cid});
    const double mins = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60.0;
    char done[96];
    std::snprintf(done, sizeof done, "done: %.1f min audio in %.1f min", secs / 60.0, mins);
    log(done, "done", 1.0);
}

}  // namespace rm
