// The three steps of a chapter. Each writes one artifact in <data>/work/chapters/<chapter id>/ that the
// user can read and edit through the API; editing a step marks the later steps outdated (timestamps).
//
// 1. analysis.json - ModernBookNLP pre-processing of the fetched chapter:
//    {"version": 1, "title", "url", "detector": "modernbooknlp" | "none",
//     "paragraphs": [text, ...],
//     "spans": [{"id", "para", "kind": "narration" | "dialogue", "text", "speaker"}],   speaker: dialogue only
//     "characters": [{"name", "gender"}]}                                               ModernBookNLP genders
// 2. script.json - what gets narrated, written by a producer (local or OpenAI-compatible):
//    {"version": 1, "producer", "title",
//     "cast": {name: {"gender", "age", "voice", "aliases"}},                          voice = description
//     "lines": [{"id", "para", "kind": "title" | "narration" | "dialogue", "speaker", "text"}],
//     "quality_issues": [...]}
// 3. narration: one WAV per script line (cached by voice + text), narration.json, and the chapter MP3.
#pragma once

#include "db.h"
#include "engines.h"
#include "settings.h"
#include "sources.h"
#include "voices.h"

#include <filesystem>
#include <functional>

namespace rm {

using StepLog = std::function<void(const std::string & msg, const std::string & stage, double progress)>;

class Steps {
public:
    Steps(DB & db, const Deploy & d, std::function<Settings()> settings, const Sources & sources, Engines & engines)
        : db_(db), d_(d), settings_(std::move(settings)), sources_(sources), engines_(engines) {}

    std::filesystem::path dir(int64_t chapter_id) const;
    std::filesystem::path analysis_path(int64_t cid) const { return dir(cid) / "analysis.json"; }
    std::filesystem::path script_path(int64_t cid) const { return dir(cid) / "script.json"; }
    std::filesystem::path narration_path(int64_t cid) const { return dir(cid) / "narration.json"; }
    std::filesystem::path line_wav(int64_t cid, int line) const;

    void analyze(int64_t chapter_id, const StepLog & log);
    void produce(int64_t chapter_id, const std::string & producer, const StepLog & log);
    void narrate(int64_t chapter_id, const StepLog & log);

    // user edits (validated; throw std::invalid_argument with the problems)
    void save_analysis(int64_t chapter_id, const json & analysis);
    void save_script(int64_t chapter_id, const json & script);

    // characters.lines = dialogue lines over all scripts of the novel
    void recount_lines(int64_t novel_id);
    // new speakers of a script get a novel cast entry and a voice
    void cast_new_characters(int64_t novel_id, const json & script, int position, const Pool & pool,
                             const std::function<void(const std::string &)> & log);

private:
    json chapter(int64_t id) const;
    DB & db_;
    const Deploy d_;
    std::function<Settings()> settings_;
    const Sources & sources_;
    Engines & engines_;
};

json read_json_file(const std::filesystem::path & p);
void write_json_file(const std::filesystem::path & p, const json & j);

}  // namespace rm
