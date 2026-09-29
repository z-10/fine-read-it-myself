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

json voice_row(const json & v) {
    return {{"id", v["id"]}, {"label", v["label"]}, {"reader", v.value("reader", "")}, {"gender", v["gender"]},
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
        std::string director = b.value("director", "");
        if (director.empty()) director = app.get_settings().default_director;
        if (const std::string why = producer_problem(app, director, app.get_settings()); !why.empty()) throw HttpError{400, why};
        try {
            reply(res, app.library->add(b.at("url").get<std::string>(), narrator, director));
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
        if (b.contains("director") && !b["director"].is_null()) {
            const std::string d = b["director"].get<std::string>();
            const std::string why = producer_problem(app, d, app.get_settings());
            if (!why.empty()) throw HttpError{400, why};
            app.db->run("UPDATE novels SET director=? WHERE id=?", {d, nid});
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
            const std::string s2 = k.get<std::string>();
            if (std::find(kStepKinds.begin(), kStepKinds.end(), s2) == kStepKinds.end()) throw HttpError{400, "unknown step: " + s2};
            kinds.push_back(s2);
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
                           {"local", true}});
        }
        for (const auto & d : app.get_settings().directors)
            out.push_back({{"name", d.name}, {"label", d.name + " (" + d.model + ")"}, {"local", false}});
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
        for (const auto & v : app.pool().voices)   // voices that failed pool QA are never shown
            if (!v.contains("qa") || v["qa"].value("ok", true)) out.push_back(voice_row(v));
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
        reply(res, {{"voice", voice_row(*fresh.get(vid))}, {"recast", changes}});
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
        json out = json::array();
        std::string error;
        try {
            for (auto v : app.voice_catalog->list()) {
                v["installed"] = pool.get(v["id"].get<std::string>()) != nullptr;
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
    svr.Get("/api/voices/download", H([&](const httplib::Request &, httplib::Response & res) {
        reply(res, app.voice_catalog->state());
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
        reply(res, app.settings.to_json(true));
    }));
}

}  // namespace

int main(int argc, char ** argv) {
    std::string host = "127.0.0.1";
    int port = 8765;
    std::optional<fs::path> data, models, web, voices;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--host") host = v;
        else if (k == "--port") port = std::stoi(v);
        else if (k == "--data") data = fs::u8path(v);
        else if (k == "--models") models = fs::u8path(v);
        else if (k == "--web") web = fs::u8path(v);
        else if (k == "--voices") voices = fs::u8path(v);
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
        if (web && fs::is_directory(*web)) {
            svr.set_mount_point("/", web->u8string());
            const fs::path index = *web / "index.html";
            svr.set_error_handler([index](const httplib::Request & req, httplib::Response & res) {
                // single-page app: unknown non-API paths get index.html
                if (res.status == 404 && req.path.rfind("/api/", 0) != 0 && fs::exists(index)) {
                    res.status = 200;
                    res.set_content(read_file(index), "text/html");
                }
            });
        }
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
