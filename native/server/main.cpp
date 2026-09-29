// readmyself-server: JSON API under /api (used by the Electron app and, later, mobile companions) and
// the web UI build served at /.
//
//   readmyself-server [--host 127.0.0.1] [--port 8765] [--data DIR] [--models DIR] [--web DIR]
#include "db.h"
#include "directors.h"
#include "engines.h"
#include "library.h"
#include "llm_presets.h"
#include "settings.h"
#include "sources.h"
#include "voices.h"
#include "setup.h"
#include "voicecat.h"
#include "voicepool.h"
#include "steps.h"
#include "text.h"
#include "worker.h"

#include "convert.h"

#include "httplib.h"

#include <atomic>
#include <fstream>
#include <functional>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace rm;
namespace fs = std::filesystem;

namespace {

struct HttpError {
    int status;
    std::string detail;
};

struct App {
    Deploy deploy;
    std::mutex settings_mu;
    Settings settings;
    std::unique_ptr<DB> db;
    Sources sources;
    std::unique_ptr<Library> library;
    std::unique_ptr<Engines> engines;
    std::unique_ptr<Setup> setup;
    std::unique_ptr<VoiceCatalog> voice_catalog;
    std::unique_ptr<Steps> steps;
    std::unique_ptr<Worker> worker;

    Settings get_settings() {
        std::lock_guard<std::mutex> lk(settings_mu);
        return settings;
    }
    Pool pool() const { return Pool(deploy.pool_dirs(), deploy.voice_overrides()); }
    // network sharing (see NetworkShare): forced on by --share-port (headless mode), else Settings
    std::optional<int> forced_share_port;
    std::function<void()> share_refresh;
    std::function<json()> share_state;
};

void reply(httplib::Response & res, const json & j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

json body_of(const httplib::Request & req) {
    if (req.body.empty()) return json::object();
    try {
        return json::parse(req.body);
    } catch (const json::exception &) {
        throw HttpError{422, "request body is not valid JSON"};
    }
}

int64_t id_param(const httplib::Request & req, size_t i = 1) { return std::stoll(req.matches[i]); }

// "" when the producer can be used: a local model that is downloaded, or one of the user's endpoints
std::string producer_problem(App & app, const std::string & name, const Settings & s) {
    if (const LlmPreset * lp = find_llm_preset(llm_of_producer(name)))
        return app.engines->llm_available(lp->id) ? "" : lp->title + " is not downloaded (Settings)";
    return s.director_profile(name) ? "" : "unknown producer: " + name;
}

std::set<std::string> usable_ids(const Pool & pool) {
    std::set<std::string> ids;
    for (const auto * v : pool.usable()) ids.insert((*v)["id"].get<std::string>());
    return ids;
}

json voice_row(const json & v, const Pool & pool) {
    return {{"id", v["id"]}, {"label", v["label"]}, {"reader", v.value("reader", "")}, {"gender", v["gender"]},
            {"bundled", pool.bundled(v["id"].get<std::string>())},
            {"metadata_gender", v["metadata_gender"]}, {"gender_overridden", v["gender"] != v["metadata_gender"]},
            {"banned", v["banned"]}, {"usable", !v["banned"].get<bool>()}, {"band", v.value("band", "")},
            {"f0", v.contains("f0") ? v["f0"] : json(nullptr)}};
}

json novel_or_404(App & app, int64_t nid) {
    auto n = app.db->one("SELECT * FROM novels WHERE id=?", {nid});
    if (!n) throw HttpError{404, "Not Found"};
    return *n;
}

std::string read_file(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void routes(httplib::Server & svr, App & app) {
    // wrap handlers: HttpError -> {"detail": ...} like the Python service
    auto H = [](auto fn) {
        return [fn](const httplib::Request & req, httplib::Response & res) {
            try {
                fn(req, res);
            } catch (const HttpError & e) {
                reply(res, {{"detail", e.detail}}, e.status);
            } catch (const std::exception & e) {
                reply(res, {{"detail", e.what()}}, 500);
            }
        };
    };

    svr.Get("/api/status", H([&](const httplib::Request &, httplib::Response & res) {
        const Settings s = app.get_settings();
        json warnings = json::array();
        const Pool pool = app.pool();
        const auto q = app.db->one("SELECT COUNT(*) c FROM jobs WHERE state IN ('queued','running')");
        reply(res, {{"warnings", warnings},
                    {"voices", pool.usable().size()},
                    {"queue", (*q)["c"]},
                    {"engines", {{"tts", {{"model", app.deploy.tts_model.u8string()}, {"installed", app.engines->tts_available()}}},
                                 {"director_llm", [&] {
                                      json m = json::object();
                                      for (const auto & lp : llm_presets()) m[lp.id] = app.engines->llm_available(lp.id);
                                      return m;
                                  }()},
                                 {"modernbooknlp", {{"models", app.deploy.booknlp_models.u8string()}, {"installed", app.engines->booknlp_available()}}},
                                 {"gpu", app.deploy.gpu.empty() ? "auto" : app.deploy.gpu},
                                 {"data_dir", app.deploy.data_dir.u8string()}}}});
    }));

    svr.Get("/api/setup", H([&](const httplib::Request &, httplib::Response & res) { reply(res, app.setup->state()); }));
    svr.Post("/api/setup/install", H([&](const httplib::Request & req, httplib::Response & res) {
        std::vector<std::string> optional;   // {"optional": ["llm:qwen3.5-9b", ...]}: extra models to download
        for (const auto & id : body_of(req).value("optional", json::array())) optional.push_back(id.get<std::string>());
        app.setup->install(optional);
        reply(res, app.setup->state());
    }));

    svr.Get("/api/sources", H([&](const httplib::Request &, httplib::Response & res) { reply(res, app.library->supported()); }));
    svr.Get("/api/novels", H([&](const httplib::Request &, httplib::Response & res) { reply(res, app.library->novels()); }));

    svr.Post("/api/novels", H([&](const httplib::Request & req, httplib::Response & res) {
        const json b = body_of(req);
        const Pool pool = app.pool();
        std::string narrator = b.value("narrator", "");
        if (narrator.empty() && !pool.usable().empty()) narrator = default_narrator(pool);
        try {   // novels have no producer of their own: steps use the run's producer or the default one
            reply(res, app.library->add(b.at("url").get<std::string>(), narrator, ""));
        } catch (const std::invalid_argument & e) {
            throw HttpError{400, e.what()};
        }
    }));

    svr.Get(R"(/api/novels/(\d+))", H([&](const httplib::Request & req, httplib::Response & res) {
        reply(res, novel_or_404(app, id_param(req)));
    }));

    svr.Patch(R"(/api/novels/(\d+))", H([&](const httplib::Request & req, httplib::Response & res) {
        const int64_t nid = id_param(req);
        const json b = body_of(req);
        if (b.contains("narrator") && !b["narrator"].is_null()) {
            const std::string v = b["narrator"].get<std::string>();
            if (!usable_ids(app.pool()).count(v)) throw HttpError{400, "voice is unknown, banned or failed QA"};
            app.db->run("UPDATE novels SET narrator=? WHERE id=?", {v, nid});
        }
        reply(res, novel_or_404(app, nid));
    }));

    svr.Delete(R"(/api/novels/(\d+))", H([&](const httplib::Request & req, httplib::Response & res) {
        app.db->run("DELETE FROM novels WHERE id=?", {id_param(req)});
        reply(res, {{"ok", true}});
    }));

    svr.Post(R"(/api/novels/(\d+)/refresh)", H([&](const httplib::Request & req, httplib::Response & res) {
        reply(res, {{"new_chapters", app.library->refresh(id_param(req))}});
    }));

    svr.Get(R"(/api/novels/(\d+)/chapters)", H([&](const httplib::Request & req, httplib::Response & res) {
        const int64_t nid = id_param(req);
        auto rows = app.library->chapters(nid);
        std::map<int64_t, json> jobs;   // chapter -> queued/running steps
        for (const auto & j : app.db->all("SELECT j.id, j.chapter_id, j.kind, j.state, j.stage, j.progress FROM jobs j JOIN chapters c "
                                          "ON c.id=j.chapter_id WHERE c.novel_id=? AND j.state IN ('queued','running') ORDER BY j.id", {nid}))
            jobs[j["chapter_id"].get<int64_t>()].push_back(j);
        for (auto & r : rows) {
            auto it = jobs.find(r["id"].get<int64_t>());
            r["jobs"] = it == jobs.end() ? json::array() : it->second;
        }
        reply(res, rows);
    }));

    auto steps_of = [](const json & b) {
        std::vector<std::string> kinds;
        for (const auto & k : b.value("steps", json::array())) {
            std::string s2 = k.get<std::string>();
            const std::vector<std::string> want = s2 == "prepare" ? std::vector<std::string>{"analyze", "script"} : std::vector<std::string>{s2};
            for (const auto & w : want) {
                if (std::find(kStepKinds.begin(), kStepKinds.end(), w) == kStepKinds.end()) throw HttpError{400, "unknown step: " + w};
                if (std::find(kinds.begin(), kinds.end(), w) == kinds.end()) kinds.push_back(w);
            }
        }
        if (kinds.empty()) throw HttpError{400, "no steps given (analyze, script, narrate)"};
        return kinds;
    };
    auto check_producer = [&](const std::string & p) {
        if (p.empty()) return;
        const std::string why = producer_problem(app, p, app.get_settings());
        if (!why.empty()) throw HttpError{400, why};
    };

    svr.Post(R"(/api/novels/(\d+)/process)", H([&, steps_of, check_producer](const httplib::Request & req, httplib::Response & res) {
        const int64_t nid = id_param(req);
        const json b = body_of(req);
        const auto kinds = steps_of(b);
        const std::string producer = b.value("producer", "");
        check_producer(producer);
        std::vector<int64_t> ids;
        for (const auto & cid : b.value("chapter_ids", json::array()))
            if (app.db->one("SELECT id FROM chapters WHERE novel_id=? AND id=?", {nid, cid})) ids.push_back(cid.get<int64_t>());
        reply(res, {{"queued", app.worker->enqueue(ids, kinds, producer)}});
    }));

    auto chapter_or_404 = [&](int64_t cid) {
        auto c = app.db->one("SELECT * FROM chapters WHERE id=?", {cid});
        if (!c) throw HttpError{404, "Not Found"};
        (*c)["jobs"] = app.db->all("SELECT id, kind, state, stage, progress FROM jobs WHERE chapter_id=? AND state IN ('queued','running') ORDER BY id", {cid});
        (*c)["story"] = looks_like_chapter((*c)["title"].get<std::string>());
        return *c;
    };
    svr.Get(R"(/api/chapters/(\d+))", H([&, steps_of, check_producer, chapter_or_404](const httplib::Request & req, httplib::Response & res) {
        reply(res, chapter_or_404(id_param(req)));
    }));
    svr.Post(R"(/api/chapters/(\d+)/run)", H([&, steps_of, check_producer, chapter_or_404](const httplib::Request & req, httplib::Response & res) {
        const int64_t cid = id_param(req);
        chapter_or_404(cid);
        const json b = body_of(req);
        const auto kinds = steps_of(b);
        const std::string producer = b.value("producer", "");
        check_producer(producer);
        reply(res, {{"queued", app.worker->enqueue({cid}, kinds, producer)}});
    }));
    // artifacts: GET returns the JSON (404 until the step ran), PUT saves the user's edit
    for (const std::string art : {"analysis", "script"}) {
        svr.Get(R"(/api/chapters/(\d+)/)" + art, H([&, art, chapter_or_404](const httplib::Request & req, httplib::Response & res) {
            const int64_t cid = id_param(req);
            chapter_or_404(cid);
            const fs::path p = art == "analysis" ? app.steps->analysis_path(cid) : app.steps->script_path(cid);
            if (!fs::exists(p)) throw HttpError{404, "this step has not run yet"};
            reply(res, read_json_file(p));
        }));
        svr.Put(R"(/api/chapters/(\d+)/)" + art, H([&, art, chapter_or_404](const httplib::Request & req, httplib::Response & res) {
            const int64_t cid = id_param(req);
            chapter_or_404(cid);
            try {
                if (art == "analysis") app.steps->save_analysis(cid, body_of(req));
                else app.steps->save_script(cid, body_of(req));
            } catch (const std::invalid_argument & e) {
                throw HttpError{400, e.what()};
            }
            reply(res, read_json_file(art == "analysis" ? app.steps->analysis_path(cid) : app.steps->script_path(cid)));
        }));
    }
    svr.Get(R"(/api/chapters/(\d+)/narration)", H([&, steps_of, check_producer, chapter_or_404](const httplib::Request & req, httplib::Response & res) {
        const int64_t cid = id_param(req);
        if (!fs::exists(app.steps->narration_path(cid))) throw HttpError{404, "not narrated yet"};
        reply(res, read_json_file(app.steps->narration_path(cid)));
    }));
    svr.Get(R"(/api/chapters/(\d+)/lines/(\d+)/audio)", H([&, steps_of, check_producer, chapter_or_404](const httplib::Request & req, httplib::Response & res) {
        const fs::path p = app.steps->line_wav(id_param(req), std::stoi(req.matches[2]));
        if (!fs::exists(p)) throw HttpError{404, "Not Found"};
        res.set_content(read_file(p), "audio/wav");
    }));

    svr.Get("/api/directors", H([&](const httplib::Request &, httplib::Response & res) {
        json out = json::array();   // producers: the local models ("local" = the default one), then the user's endpoints
        for (const auto & lp : llm_presets()) {
            if (!app.engines->llm_available(lp.id)) continue;   // not downloaded: not offered
            char acc[16];
            std::snprintf(acc, sizeof acc, "%.1f%%", lp.accuracy);
            out.push_back({{"name", lp.id == kDefaultLlm ? std::string("local") : "local:" + lp.id},
                           {"label", "Local · " + lp.title + " (" + acc + " speaker check, " + std::to_string(static_cast<int>(lp.vram_gb)) + " GB VRAM)"},
                           {"local", true}, {"default", false}});
        }
        for (const auto & d : app.get_settings().directors)
            out.push_back({{"name", d.name}, {"label", d.name + " (" + d.model + ")"}, {"local", false}, {"default", false}});
        const std::string dflt = app.get_settings().default_director;   // the one menus start on
        for (auto & o : out)
            if (o["name"] == dflt || (dflt == "local:" + std::string(kDefaultLlm) && o["name"] == "local")) o["default"] = true;
        reply(res, out);
    }));

    svr.Get(R"(/api/novels/(\d+)/cast)", H([&](const httplib::Request & req, httplib::Response & res) {
        reply(res, app.db->cast(id_param(req)));
    }));

    svr.Patch(R"(/api/novels/(\d+)/cast/(.+))", H([&](const httplib::Request & req, httplib::Response & res) {
        const int64_t nid = id_param(req);
        const std::string name = req.matches[2];
        const json b = body_of(req);
        json fields = json::object();
        auto str = [&](const char * k) -> std::optional<std::string> {
            if (!b.contains(k) || b[k].is_null()) return std::nullopt;
            return b[k].get<std::string>();
        };
        if (auto v = str("voice")) {
            if (!usable_ids(app.pool()).count(*v)) throw HttpError{400, "voice is unknown, banned or failed QA"};
            fields["voice"] = *v;
            fields["locked"] = 1;
        }
        if (b.contains("locked") && !b["locked"].is_null()) fields["locked"] = b["locked"].get<bool>() ? 1 : 0;
        auto gender = str("gender");
        if (gender) {
            if (*gender != "male" && *gender != "female") throw HttpError{400, "gender must be male or female"};
            fields["gender"] = *gender;
        }
        if (auto a = str("age")) {
            if (*a != "child" && *a != "teen" && *a != "adult" && *a != "elder")
                throw HttpError{400, "age must be child, teen, adult or elder"};
            fields["age"] = *a;
        }
        if (auto p = str("pitch")) {
            if (*p != "all" && *p != "low" && *p != "mid" && *p != "high") throw HttpError{400, "pitch must be all, low, mid or high"};
            fields["pitch"] = *p;
        }
        app.db->upsert_character(nid, name, fields);
        if (gender) app.worker->repair_voices(-1, novel_or_404(app, nid), app.pool());   // voice must match the new gender
        reply(res, app.db->cast(nid));
    }));

    svr.Get(R"(/api/chapters/(\d+)/audio)", H([&](const httplib::Request & req, httplib::Response & res) {
        auto ch = app.db->one("SELECT audio_path FROM chapters WHERE id=?", {id_param(req)});
        const std::string path = ch && (*ch)["audio_path"].is_string() ? (*ch)["audio_path"].get<std::string>() : "";
        if (path.empty() || !fs::exists(fs::u8path(path))) throw HttpError{404, "Not Found"};
        res.set_content(read_file(fs::u8path(path)), "audio/mpeg");
        res.set_header("Content-Disposition", "inline; filename=\"" + fs::u8path(path).filename().u8string() + "\"");
    }));

    svr.Get("/api/voices", H([&](const httplib::Request &, httplib::Response & res) {
        json out = json::array();
        const Pool pool = app.pool();
        for (const auto & v : pool.voices)   // voices that failed pool QA are never shown
            if (!v.contains("qa") || v["qa"].value("ok", true)) out.push_back(voice_row(v, pool));
        reply(res, out);
    }));

    svr.Patch(R"(/api/voices/([^/]+))", H([&](const httplib::Request & req, httplib::Response & res) {
        const std::string vid = req.matches[1];
        Pool pool = app.pool();
        if (!pool.get(vid)) throw HttpError{404, "Not Found"};
        const json b = body_of(req);
        std::optional<std::string> gender;
        if (b.contains("gender") && !b["gender"].is_null()) {
            gender = b["gender"].get<std::string>();
            if (*gender != "male" && *gender != "female" && *gender != "reset") throw HttpError{400, "gender must be male, female or reset"};
        }
        std::optional<bool> banned;
        if (b.contains("banned") && !b["banned"].is_null()) banned = b["banned"].get<bool>();
        pool.set_override(vid, gender && *gender != "reset" ? gender : std::nullopt, banned, gender && *gender == "reset");
        const Pool fresh = app.pool();
        json changes = json::array();
        for (const auto & n : app.db->all("SELECT * FROM novels")) {   // re-cast characters that no longer fit
            const int64_t nid = n["id"].get<int64_t>();
            auto snapshot = [&] {
                std::map<std::string, std::string> m;
                for (const auto & c : app.db->cast(nid)) m[c["name"].get<std::string>()] = c["voice"].get<std::string>();
                m["(narrator)"] = (*app.db->one("SELECT narrator FROM novels WHERE id=?", {nid}))["narrator"].get<std::string>();
                return m;
            };
            const auto before = snapshot();
            app.worker->repair_voices(-1, n, fresh);
            for (const auto & [k, v] : snapshot()) {
                auto it = before.find(k);
                if (it == before.end() || it->second != v)
                    changes.push_back(n["title"].get<std::string>() + ": " + k + " " + (it == before.end() ? "" : it->second) + " -> " + v);
            }
        }
        reply(res, {{"voice", voice_row(*fresh.get(vid), fresh)}, {"recast", changes}});
    }));

    svr.Get(R"(/api/voices/([^/]+)/sample)", H([&](const httplib::Request & req, httplib::Response & res) {
        const std::string vid = req.matches[1];
        const Pool pool = app.pool();
        if (!pool.get(vid)) throw HttpError{404, "Not Found"};
        res.set_content(read_file(pool.sample(vid)), "audio/wav");
    }));

    // the voice catalog: tested voices to download (the app ships with 3 male + 3 female)
    svr.Get("/api/voices/catalog", H([&](const httplib::Request &, httplib::Response & res) {
        const Pool pool = app.pool();
        const auto removed = removed_voices(app.deploy.voice_overrides());
        json out = json::array();
        std::string error;
        try {
            for (auto v : app.voice_catalog->list()) {
                v["installed"] = pool.get(v["id"].get<std::string>()) != nullptr;
                v["removed"] = removed.count(v["id"].get<std::string>()) > 0;   // "Download all" skips these
                out.push_back(v);
            }
        } catch (const std::exception & e) {
            error = e.what();
        }
        reply(res, {{"url", app.get_settings().voice_catalog}, {"voices", out}, {"error", error}, {"download", app.voice_catalog->state()}});
    }));
    svr.Post("/api/voices/download", H([&](const httplib::Request & req, httplib::Response & res) {
        const json b = body_of(req);
        std::vector<std::string> ids;
        for (const auto & id : b.value("ids", json::array())) ids.push_back(id.get<std::string>());
        if (ids.empty()) throw HttpError{400, "ids must list the voices to download"};
        app.voice_catalog->download(ids);
        reply(res, app.voice_catalog->state());
    }));
    // remove a voice: a downloaded one is deleted, one shipped with the app is hidden. Voices that characters or a
    // narrator use need ?force=1 (the reply lists them first); those get new voices, even ones the user picked.
    svr.Delete(R"(/api/voices/([^/]+))", H([&](const httplib::Request & req, httplib::Response & res) {
        const std::string vid = req.matches[1];
        const Pool pool = app.pool();
        if (!pool.get(vid)) throw HttpError{404, "Not Found"};
        json users = json::array();
        for (const auto & n : app.db->all("SELECT * FROM novels")) {
            if (n["narrator"].is_string() && n["narrator"] == vid) users.push_back(n["title"].get<std::string>() + ": narrator");
            for (const auto & c : app.db->cast(n["id"].get<int64_t>()))
                if (c["voice"] == vid)
                    users.push_back(n["title"].get<std::string>() + ": " + c["name"].get<std::string>() +
                                    (c["locked"].get<int>() ? " (picked by you)" : ""));
        }
        if (!users.empty() && req.get_param_value("force") != "1") {
            reply(res, {{"detail", "voice in use"}, {"users", users}}, 409);
            return;
        }
        const bool bundled = pool.bundled(vid);
        const std::string gender = (*pool.get(vid))["gender"].get<std::string>();   // before it is gone
        set_voice_removed(app.deploy.voice_overrides(), vid, true);
        if (!bundled) app.voice_catalog->uninstall(vid);
        const json recast = app.worker->replace_voice(vid, gender, app.pool());
        reply(res, {{"removed", vid}, {"deleted", !bundled}, {"recast", recast}});
    }));

    svr.Get("/api/voices/download", H([&](const httplib::Request &, httplib::Response & res) {
        reply(res, app.voice_catalog->state());
    }));

    svr.Get("/api/share", H([&](const httplib::Request &, httplib::Response & res) {
        reply(res, app.share_state ? app.share_state() : json::object());
    }));

    svr.Get("/api/jobs", H([&](const httplib::Request & req, httplib::Response & res) {
        const int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 50;
        reply(res, app.db->all("SELECT j.*, c.title AS chapter_title, c.novel_id, c.position FROM jobs j "
                               "JOIN chapters c ON c.id=j.chapter_id ORDER BY j.id DESC LIMIT ?", {limit}));
    }));

    svr.Post(R"(/api/jobs/(\d+)/cancel)", H([&](const httplib::Request & req, httplib::Response & res) {
        const int64_t jid = id_param(req);
        auto j = app.db->one("SELECT * FROM jobs WHERE id=?", {jid});
        if (j && (*j)["state"] == "queued") {
            app.db->run("UPDATE jobs SET state='cancelled' WHERE id=?", {jid});
            app.db->run("UPDATE chapters SET status='new' WHERE id=?", {(*j)["chapter_id"]});
        }
        reply(res, {{"ok", true}});
    }));

    svr.Get("/api/settings", H([&](const httplib::Request &, httplib::Response & res) {
        reply(res, app.get_settings().to_json(true));
    }));

    svr.Put("/api/settings", H([&](const httplib::Request & req, httplib::Response & res) {
        json b = body_of(req);
        std::lock_guard<std::mutex> lk(app.settings_mu);
        std::map<std::string, std::string> old_keys;
        for (const auto & p : app.settings.directors) old_keys[p.name] = p.api_key;
        if (b.contains("directors"))   // keep stored keys when the UI sends the mask back
            for (auto & p : b["directors"])
                if (p.value("api_key", "") == "***") p["api_key"] = old_keys[p.value("name", "")];
        json merged = app.settings.to_json(false);
        for (auto it = b.begin(); it != b.end(); ++it) merged[it.key()] = it.value();
        Settings next;
        try {
            next = Settings::from_json(merged);
        } catch (const std::invalid_argument & e) {
            throw HttpError{400, e.what()};
        }
        if (const std::string why = producer_problem(app, next.default_director, next); !why.empty())
            throw HttpError{400, "default producer: " + why};
        app.settings = next;
        save_settings(app.deploy, app.settings);
        if (app.share_refresh) app.share_refresh();
        reply(res, app.settings.to_json(true));
    }));
}

}  // namespace

// the UI (renderer/dist) at "/", single-page app: unknown non-API paths get index.html
void mount_web(httplib::Server & svr, const std::optional<fs::path> & web) {
    if (!web || !fs::is_directory(*web)) return;
    // browsers must check with the server before reusing a cached page (unchanged files cost a 304), so an
    // updated UI shows up on the next load instead of whenever the browser's own cache heuristic expires
    svr.set_mount_point("/", web->u8string(), {{"Cache-Control", "no-cache"}});
    const fs::path index = *web / "index.html";
    svr.set_error_handler([index](const httplib::Request & req, httplib::Response & res) {
        if (res.status == 404 && req.path.rfind("/api/", 0) != 0 && fs::exists(index)) {
            res.status = 200;
            res.set_header("Cache-Control", "no-cache");
            res.set_content(read_file(index), "text/html");
        }
    });
}

// this computer's address on the local network: the interface a packet to the internet would leave from
// (a UDP "connect" sends nothing)
std::string lan_address() {
    std::string out;
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return out;
#else
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return out;
#endif
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr *>(&to), sizeof to) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        char buf[INET_ADDRSTRLEN] = {};
        if (getsockname(sock, reinterpret_cast<sockaddr *>(&me), &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf))
            out = buf;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    return out;
}

// Shares the UI + API with other devices: a second listener on all interfaces (the desktop window keeps its
// private 127.0.0.1 one). A manager thread starts/stops it to match the settings, so switching it off from a
// browser connected through it doesn't stop the server from inside its own request.
class NetworkShare {
public:
    NetworkShare(App & app, std::optional<fs::path> web) : app_(app), web_(std::move(web)), manager_([this] { loop(); }) {}
    ~NetworkShare() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        manager_.join();
    }
    void refresh() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            dirty_ = true;
        }
        cv_.notify_all();
    }
    json state() {
        std::lock_guard<std::mutex> lk(mu_);
        json urls = json::array();
        if (running_) {
            if (const std::string ip = lan_address(); !ip.empty()) urls.push_back("http://" + ip + ":" + std::to_string(port_));
            char host[256] = {};
            if (gethostname(host, sizeof host) == 0 && host[0]) urls.push_back("http://" + std::string(host) + ":" + std::to_string(port_));
        }
        return {{"enabled", want_}, {"running", running_}, {"port", port_}, {"urls", urls}, {"error", error_},
                {"forced", app_.forced_share_port.has_value()}};
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lk(mu_);
        while (!quit_) {
            cv_.wait(lk, [this] { return dirty_ || quit_; });
            if (quit_) break;
            dirty_ = false;
            const Settings s = app_.get_settings();
            const bool want = app_.forced_share_port.has_value() || s.share_network;
            const int port = app_.forced_share_port.value_or(s.share_port);
            want_ = want;
            if (running_ && (!want || port != port_)) {   // off, or another port: stop the current listener
                srv_->stop();
                lk.unlock();
                thread_.join();
                lk.lock();
                srv_.reset();
                running_ = false;
            }
            if (want && !running_) {
                auto srv = std::make_unique<httplib::Server>();
                routes(*srv, app_);
                mount_web(*srv, web_);
                port_ = port;
                if (!srv->bind_to_port("0.0.0.0", port)) {
                    error_ = "port " + std::to_string(port) + " is in use or not allowed";
                    continue;
                }
                error_.clear();
                srv_ = std::move(srv);
                thread_ = std::thread([srv = srv_.get()] { srv->listen_after_bind(); });
                running_ = true;
                std::cerr << "sharing on the network: port " << port << std::endl;
            }
            if (!want) error_.clear();
        }
        if (running_) {
            srv_->stop();
            lk.unlock();
            thread_.join();
        }
    }

    App & app_;
    std::optional<fs::path> web_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool dirty_ = true, quit_ = false, want_ = false, running_ = false;
    int port_ = 0;
    std::string error_;
    std::unique_ptr<httplib::Server> srv_;
    std::thread thread_;
    std::thread manager_;
};

int main(int argc, char ** argv) {
    std::string host = "127.0.0.1";
    int port = 8765;
    std::optional<fs::path> data, models, web, voices;
    std::optional<int> share_port;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--host") host = v;
        else if (k == "--port") port = std::stoi(v);
        else if (k == "--data") data = fs::u8path(v);
        else if (k == "--models") models = fs::u8path(v);
        else if (k == "--web") web = fs::u8path(v);
        else if (k == "--voices") voices = fs::u8path(v);
        else if (k == "--share-port") share_port = std::stoi(v);   // headless mode: share on the network regardless of Settings
    }
    if (argc >= 4 && std::string(argv[1]) == "--convert-booknlp") {   // first-run model conversion (desktop setup)
        try {
            booknlp::convert_models(fs::u8path(argv[2]), fs::u8path(argv[3]), true,
                                    [](const std::string & m) { std::cout << m << std::endl; });
            return 0;
        } catch (const std::exception & e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
    }
    // developer commands for the voice pool that ships with the app:
    //   --build-voice-pool <LibriTTS-R dir: doc + subset .tar.gz> <out dir> [--models DIR] [--all] [--keep <earlier pool>]
    //   --export-voice-pool <built pool dir> <out dir> [id,id,...]   (usable voices only; ids unchanged)
    if (argc >= 4 && std::string(argv[1]) == "--build-voice-pool") {
        try {
            std::optional<fs::path> m;
            fs::path keep;
            int per_gender = 60;
            for (int i = 4; i < argc; ++i) {
                const std::string k = argv[i];
                if (k == "--models" && i + 1 < argc) m = fs::u8path(argv[++i]);
                else if (k == "--keep" && i + 1 < argc) keep = fs::u8path(argv[++i]);
                else if (k == "--all") per_gender = 0;
            }
            const Deploy d = load_deploy(fs::temp_directory_path() / "readmyself-pool-build", m);
            Engines engines(d);
            const fs::path lib = fs::u8path(argv[2]);
            std::vector<fs::path> subsets;   // every LibriTTS-R subset archive in the folder
            for (const char * s : {"dev_clean", "test_clean", "train_clean_100", "train_clean_360", "train_other_500"})
                if (fs::exists(lib / (std::string(s) + ".tar.gz"))) subsets.push_back(lib / (std::string(s) + ".tar.gz"));
            PoolProgress pp;
            pp.log = [](const std::string & msg) { std::cout << msg << std::endl; };
            pp.stage = [](const std::string &, double) {};
            build_voice_pool(subsets, lib / "doc.tar.gz", fs::u8path(argv[3]), per_gender, engines, pp, keep);
            return 0;
        } catch (const std::exception & e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
    }
    if (argc >= 4 && std::string(argv[1]) == "--export-voice-pool") {
        try {
            std::vector<std::string> only;   // optional 4th argument: comma-separated ids
            if (argc >= 5)
                for (std::stringstream ss(argv[4]); ss.good();) {
                    std::string id;
                    std::getline(ss, id, ',');
                    if (!id.empty()) only.push_back(id);
                }
            export_voice_pool(fs::u8path(argv[2]), fs::u8path(argv[3]), only);
            return 0;
        } catch (const std::exception & e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
    }
    if (argc >= 3 && std::string(argv[1]) == "--split-spans") {   // diagnostics: chapter text (blank-line paragraphs) -> spans
        std::ifstream in(fs::u8path(argv[2]), std::ios::binary);
        std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        txt.erase(std::remove(txt.begin(), txt.end(), '\r'), txt.end());   // CRLF files
        std::vector<std::string> paras;
        size_t a = 0;
        while (a <= txt.size()) {
            size_t b = txt.find("\n\n", a);
            if (b == std::string::npos) b = txt.size();
            const std::string par = strip(txt.substr(a, b - a));
            if (!par.empty()) paras.push_back(par);
            a = b + 2;
        }
        std::cout << spans_json(split_spans(paras)).dump(1) << std::endl;
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--test-fetch") {   // diagnostics: fetch + parse one novel page
        try {
            const Sources srcs = load_sources({});
            auto hit = resolve(argv[2], srcs);
            if (!hit) throw std::runtime_error("no source matches");
            const std::string url = hit->first->novel_url(hit->second);
            std::cerr << "GET " << url << std::endl;
            const std::string html = fetch(url, *hit->first);
            std::cerr << html.size() << " bytes" << std::endl;
            const NovelInfo info = parse_novel(html, url, *hit->first);
            std::cerr << info.title << " / " << info.author << ": " << info.chapters.size() << " chapters" << std::endl;
            if (!info.chapters.empty()) {
                const ChapterText t = parse_chapter(fetch(info.chapters[0].url, *hit->first), *hit->first);
                std::cerr << t.title << ": " << t.paragraphs.size() << " paragraphs; first: "
                          << (t.paragraphs.empty() ? "" : t.paragraphs[0].substr(0, 120)) << std::endl;
            }
            return 0;
        } catch (const std::exception & e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
    }
    try {
        App app;
        app.deploy = load_deploy(data, models, voices);
        app.settings = load_settings(app.deploy);
        app.db = std::make_unique<DB>(app.deploy.db_path());
        std::vector<fs::path> plugin_dirs{app.deploy.data_dir / "plugins"};   // site plugins the user drops in
        fs::create_directories(plugin_dirs.front());
        if (app.settings.user_sources_dir) plugin_dirs.push_back(*app.settings.user_sources_dir);
        app.sources = load_sources(plugin_dirs);
        app.library = std::make_unique<Library>(*app.db, app.sources);
        app.engines = std::make_unique<Engines>(app.deploy);
        app.setup = std::make_unique<Setup>(app.deploy, *app.engines);
        if (!app.setup->complete()) app.setup->install();   // first launch: fetch the required models right away
        app.steps = std::make_unique<Steps>(*app.db, app.deploy, [&app] { return app.get_settings(); }, app.sources, *app.engines);
        app.worker = std::make_unique<Worker>(*app.db, app.deploy, *app.steps);
        app.voice_catalog = std::make_unique<VoiceCatalog>(app.deploy, [&app] { return app.get_settings().voice_catalog; });
        app.worker->set_voice_fetcher([&app](const std::vector<std::string> & ids, const std::function<void(const std::string &)> & log) {
            return app.voice_catalog->ensure(ids, log);
        });
        app.worker->start();

        std::atomic<bool> stop{false};
        std::thread checker([&] {   // new-chapter checker
            while (!stop) {
                const double due = now() - app.get_settings().check_interval_hours * 3600;
                for (const auto & n : app.db->all("SELECT id FROM novels WHERE checked_at IS NULL OR checked_at < ?", {due})) {
                    try {
                        app.library->refresh(n["id"].get<int64_t>());
                    } catch (const std::exception & e) {
                        std::cerr << "refresh failed: " << e.what() << std::endl;
                    }
                }
                for (int i = 0; i < 300 && !stop; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });

        httplib::Server svr;
        routes(svr, app);
        mount_web(svr, web);
        app.forced_share_port = share_port;
        NetworkShare share(app, web);
        app.share_refresh = [&share] { share.refresh(); };
        app.share_state = [&share] { return share.state(); };
        std::cerr << "readmyself-server on http://" << host << ":" << port << " (data " << app.deploy.data_dir.u8string() << ")" << std::endl;
        if (!svr.listen(host, port)) {
            std::cerr << "cannot listen on " << host << ":" << port << std::endl;
            stop = true;
            checker.join();
            return 1;
        }
        stop = true;
        checker.join();
    } catch (const std::exception & e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
