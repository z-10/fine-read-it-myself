#include "worker.h"

#include "audio_io.h"
#include "directors.h"
#include "scrape.h"
#include "text.h"
#include "tokenizer.h"
#include "unicode.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace rm {

namespace fs = std::filesystem;

static std::string hhmmss() {
    const std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S ", std::localtime(&t));
    return buf;
}

static std::string read_text(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Worker::Worker(DB & db, const Deploy & d, Steps & steps) : db_(db), d_(d), steps_(steps) {}

Worker::~Worker() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Worker::start() { thread_ = std::thread([this] { loop(); }); }

int Worker::enqueue(const std::vector<int64_t> & chapter_ids, const std::vector<std::string> & kinds,
                    const std::string & producer) {
    int n = 0;
    for (int64_t cid : chapter_ids)
        for (const auto & kind : kStepKinds) {   // step order, whatever order they were asked in
            if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) continue;
            if (db_.one("SELECT id FROM jobs WHERE chapter_id=? AND kind=? AND state IN ('queued','running')", {cid, kind})) continue;
            db_.run("INSERT INTO jobs (chapter_id, kind, director, created_at) VALUES (?,?,?,?)",
                    {cid, kind, kind == "script" ? producer : "", now()});
            ++n;
        }
    return n;
}

void Worker::log(int64_t job_id, const std::string & msg, const std::string & stage, std::optional<double> progress) {
    if (job_id < 0) {   // maintenance outside a job (startup repairs): server log only
        std::cerr << hhmmss() << msg << std::endl;
        return;
    }
    std::string sql = "UPDATE jobs SET log = log || ?";
    std::vector<json> args{hhmmss() + msg + "\n"};
    if (!stage.empty()) {
        sql += ", stage=?";
        args.push_back(stage);
    }
    if (progress) {
        sql += ", progress=?";
        args.push_back(*progress);
    }
    sql += " WHERE id=?";
    args.push_back(job_id);
    db_.run(sql, args);
}

void Worker::loop() {
    // jobs left 'running' by a previous crash go back to the queue
    db_.run("UPDATE jobs SET state='queued' WHERE state='running'");
    try {   // the pool may have changed while the app was closed: fix casts now
        Pool pool(d_.pool_dirs(), d_.voice_overrides());
        if (!pool.usable().empty())
            for (const auto & n : db_.all("SELECT * FROM novels")) repair_voices(-1, n, pool);
    } catch (const std::exception & e) {
        std::cerr << "voice repair failed: " << e.what() << std::endl;
    }
    while (!stop_) {
        auto job = db_.one("SELECT * FROM jobs WHERE state='queued' ORDER BY created_at LIMIT 1");
        if (!job) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        const int64_t jid = (*job)["id"].get<int64_t>();
        db_.run("UPDATE jobs SET state='running', started_at=? WHERE id=?", {now(), jid});
        try {
            run(*job);
            db_.run("UPDATE jobs SET state='done', progress=1, finished_at=? WHERE id=?", {now(), jid});
            db_.run("UPDATE chapters SET error='' WHERE id=?", {(*job)["chapter_id"]});
        } catch (const std::exception & ex) {
            log(jid, std::string("ERROR ") + ex.what());
            db_.run("UPDATE jobs SET state='failed', finished_at=? WHERE id=?", {now(), jid});
            db_.run("UPDATE chapters SET error=? WHERE id=?",
                    {(*job).value("kind", "") + ": " + std::string(ex.what()).substr(0, 500), (*job)["chapter_id"]});
            // later steps of this chapter depend on this one
            db_.run("UPDATE jobs SET state='cancelled', finished_at=? WHERE chapter_id=? AND state='queued'",
                    {now(), (*job)["chapter_id"]});
        }
    }
}

void Worker::repair_voices(int64_t jid, json novel, const Pool & pool, bool missing) {
    std::set<std::string> usable;
    for (const auto * v : pool.usable()) usable.insert((*v)["id"].get<std::string>());
    if (!missing) {   // not installed is not unusable: keep it (narration downloads it from the voice catalog)
        for (const auto & c : db_.cast(novel["id"].get<int64_t>())) {
            const std::string v = c["voice"].get<std::string>();
            if (!pool.get(v)) usable.insert(v);
        }
        if (novel["narrator"].is_string() && !pool.get(novel["narrator"].get<std::string>()))
            usable.insert(novel["narrator"].get<std::string>());
    }
    const int64_t nid = novel["id"].get<int64_t>();
    const std::string narrator = novel["narrator"].is_string() ? novel["narrator"].get<std::string>() : "";
    if (!usable.count(narrator)) {
        const json * old = pool.get(narrator);
        const std::string g = old ? (*old)["gender"].get<std::string>() : "male";
        const std::string fresh = default_narrator(pool, g);
        db_.run("UPDATE novels SET narrator=? WHERE id=?", {fresh, nid});
        log(jid, "narrator voice " + narrator + " was withdrawn from the pool -> " + fresh);
        novel["narrator"] = fresh;
    }
    const auto cast = db_.cast(nid);
    std::map<std::string, std::string> gender_of;
    for (const auto & v : pool.voices) gender_of[v["id"].get<std::string>()] = v["gender"].get<std::string>();
    std::vector<json> broken, keep;
    for (const auto & c : cast) {
        const std::string v = c["voice"].get<std::string>();
        // a voice not installed has no known gender here: only `missing` (not in the catalog either) re-casts it
        const bool bad = !c["locked"].get<int>() &&
                         (!usable.count(v) || (gender_of.count(v) && gender_of[v] != c["gender"].get<std::string>()));
        (bad ? broken : keep).push_back(c);
    }
    if (broken.empty()) return;
    std::vector<std::pair<std::string, json>> entries;
    std::map<std::string, int> counts;
    for (const auto & c : broken) {
        entries.emplace_back(c["name"].get<std::string>(),
                             json{{"gender", c["gender"]}, {"age", c["age"]}, {"voice", c["voice_hint"]}, {"pitch", c["pitch"]}});
        counts[c["name"].get<std::string>()] = c["lines"].get<int>();
    }
    const auto picks = assign(pool, keep, novel["narrator"].get<std::string>(), entries, counts);
    for (const auto & c : broken) {
        const std::string name = c["name"].get<std::string>(), v = c["voice"].get<std::string>();
        auto it = picks.find(name);
        if (it == picks.end()) continue;
        db_.upsert_character(nid, name, {{"voice", it->second}});
        const std::string why = !pool.get(v) ? "is not installed and not in the voice catalog"
                                : !usable.count(v) ? "was withdrawn from the pool" : "is no longer a " + c["gender"].get<std::string>() + " voice";
        log(jid, name + ": voice " + v + " " + why + " -> " + it->second);
    }
}

std::vector<std::string> Worker::replace_voice(const std::string & id, const std::string & gender, const Pool & pool) {
    std::vector<std::string> out;
    for (auto novel : db_.all("SELECT * FROM novels")) {
        const int64_t nid = novel["id"].get<int64_t>();
        const std::string title = novel["title"].get<std::string>();
        if (novel["narrator"].is_string() && novel["narrator"] == id) {
            const std::string fresh = default_narrator(pool, gender);   // same gender as the removed voice
            db_.run("UPDATE novels SET narrator=? WHERE id=?", {fresh, nid});
            novel["narrator"] = fresh;
            out.push_back(title + ": narrator " + id + " -> " + fresh);
        }
        std::vector<json> hit, keep;
        for (const auto & c : db_.cast(nid)) (c["voice"] == id ? hit : keep).push_back(c);
        if (hit.empty()) continue;
        std::vector<std::pair<std::string, json>> entries;
        std::map<std::string, int> counts;
        for (const auto & c : hit) {
            entries.emplace_back(c["name"].get<std::string>(),
                                 json{{"gender", c["gender"]}, {"age", c["age"]}, {"voice", c["voice_hint"]}, {"pitch", c["pitch"]}});
            counts[c["name"].get<std::string>()] = c["lines"].get<int>();
        }
        const auto picks = assign(pool, keep, novel["narrator"].is_string() ? novel["narrator"].get<std::string>() : "", entries, counts);
        for (const auto & c : hit) {
            auto p = picks.find(c["name"].get<std::string>());
            if (p == picks.end()) continue;
            db_.upsert_character(nid, c["name"].get<std::string>(), {{"voice", p->second}, {"locked", 0}});
            out.push_back(title + ": " + c["name"].get<std::string>() + " " + id + " -> " + p->second);
        }
    }
    return out;
}

void Worker::run(const json & job) {
    const int64_t jid = job["id"].get<int64_t>(), cid = job["chapter_id"].get<int64_t>();
    const std::string kind = job.value("kind", "narrate");
    const StepLog step_log = [&](const std::string & m, const std::string & stage, double progress) { log(jid, m, stage, progress); };
    if (kind == "analyze") {
        steps_.analyze(cid, step_log);
    } else if (kind == "script") {
        steps_.produce(cid, job.value("director", ""), step_log);
    } else if (kind == "prepare") {   // both in one job (queued by a short-lived version): one progress bar
        const StepLog part1 = [&](const std::string & m, const std::string & stage, double p) { step_log(m, stage, 0.3 * p); };
        const StepLog part2 = [&](const std::string & m, const std::string & stage, double p) { step_log(m, stage, 0.3 + 0.7 * p); };
        steps_.analyze(cid, part1);
        steps_.produce(cid, job.value("director", ""), part2);
    } else if (kind == "narrate") {
        const json ch = *db_.one("SELECT novel_id FROM chapters WHERE id=?", {cid});
        const json novel = *db_.one("SELECT * FROM novels WHERE id=?", {ch["novel_id"]});
        {   // voices the cast uses but this install lacks (e.g. after reinstalling): from the voice catalog
            const Pool pool(d_.pool_dirs(), d_.voice_overrides());
            std::set<std::string> need;
            if (novel["narrator"].is_string() && !pool.get(novel["narrator"].get<std::string>()))
                need.insert(novel["narrator"].get<std::string>());
            for (const auto & c : db_.cast(ch["novel_id"].get<int64_t>()))
                if (!pool.get(c["voice"].get<std::string>())) need.insert(c["voice"].get<std::string>());
            if (!need.empty() && fetch_voices_)
                fetch_voices_({need.begin(), need.end()}, [&](const std::string & m) { log(jid, m, "voices"); });
        }
        const Pool pool(d_.pool_dirs(), d_.voice_overrides());
        if (!pool.usable().empty()) repair_voices(jid, novel, pool, true);
        steps_.narrate(cid, step_log);
    } else {
        throw std::runtime_error("unknown job kind " + kind);
    }
}

}  // namespace rm
