// Background worker: runs chapter steps (analyze / script / narrate, see steps.h) one job at a time.
#pragma once

#include "db.h"
#include "engines.h"
#include "settings.h"
#include "sources.h"
#include "steps.h"
#include "voices.h"

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

namespace rm {

inline const std::vector<std::string> kStepKinds = {"analyze", "script", "narrate"};

class Worker {
public:
    Worker(DB & db, const Deploy & d, Steps & steps);
    ~Worker();
    void start();
    // queue steps for chapters, in step order per chapter; producer: "" = the novel's producer.
    // A step already queued or running for a chapter is not queued twice.
    int enqueue(const std::vector<int64_t> & chapter_ids, const std::vector<std::string> & kinds, const std::string & producer);
    // Give any character or narrator whose voice is no longer usable (banned, gender changed; with `missing`
    // also: not installed) a new one; voices the user picked (locked) are kept. job_id < 0: log to stderr only.
    // Without `missing`, a voice that is simply not installed is left alone: narration downloads it first.
    void repair_voices(int64_t job_id, json novel, const Pool & pool, bool missing = false);
    // the user removed voice `id`: every character using it (picked by the user or not) and a narrator using it
    // get a new voice from `pool` (which no longer offers it); returns "novel: who old -> new" lines
    std::vector<std::string> replace_voice(const std::string & id, const std::string & gender, const Pool & pool);
    // downloads voices missing here from the voice catalog; returns the ids installed
    using VoiceFetcher = std::function<std::vector<std::string>(const std::vector<std::string> &,
                                                                const std::function<void(const std::string &)> &)>;
    void set_voice_fetcher(VoiceFetcher f) { fetch_voices_ = std::move(f); }

private:
    void loop();
    void run(const json & job);
    void log(int64_t job_id, const std::string & msg, const std::string & stage = "", std::optional<double> progress = {});

    DB & db_;
    const Deploy d_;
    Steps & steps_;
    VoiceFetcher fetch_voices_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace rm
