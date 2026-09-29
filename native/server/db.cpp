#include "db.h"

#include "sqlite3.h"

#include <chrono>
#include <set>
#include <stdexcept>

namespace rm {

static const char * kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS novels (
    id           INTEGER PRIMARY KEY,
    source       TEXT NOT NULL,
    source_key   TEXT NOT NULL,
    url          TEXT NOT NULL,
    title        TEXT NOT NULL,
    author       TEXT DEFAULT '',
    cover        TEXT DEFAULT '',
    description  TEXT DEFAULT '',
    narrator     TEXT DEFAULT '',
    director     TEXT DEFAULT 'local',
    added_at     REAL NOT NULL,
    checked_at   REAL,
    UNIQUE (source_key)
);
CREATE TABLE IF NOT EXISTS chapters (
    id           INTEGER PRIMARY KEY,
    novel_id     INTEGER NOT NULL REFERENCES novels(id) ON DELETE CASCADE,
    position     INTEGER NOT NULL,
    url          TEXT NOT NULL,
    title        TEXT NOT NULL,
    published    INTEGER,
    status       TEXT NOT NULL DEFAULT 'new',
    audio_path   TEXT DEFAULT '',
    duration_s   REAL,
    error        TEXT DEFAULT '',
    quality_issues TEXT DEFAULT '',
    director     TEXT DEFAULT '',
    narrated_at  REAL,
    UNIQUE (novel_id, url)
);
CREATE TABLE IF NOT EXISTS characters (
    id           INTEGER PRIMARY KEY,
    novel_id     INTEGER NOT NULL REFERENCES novels(id) ON DELETE CASCADE,
    name         TEXT NOT NULL,
    aliases      TEXT NOT NULL DEFAULT '[]',
    gender       TEXT NOT NULL DEFAULT 'male',
    age          TEXT NOT NULL DEFAULT 'adult',
    pitch        TEXT NOT NULL DEFAULT 'all',
    voice_hint   TEXT DEFAULT '',
    voice        TEXT NOT NULL,
    locked       INTEGER NOT NULL DEFAULT 0,
    first_chapter INTEGER,
    lines        INTEGER NOT NULL DEFAULT 0,
    UNIQUE (novel_id, name)
);
CREATE TABLE IF NOT EXISTS jobs (
    id           INTEGER PRIMARY KEY,
    chapter_id   INTEGER NOT NULL REFERENCES chapters(id) ON DELETE CASCADE,
    director     TEXT DEFAULT '',
    state        TEXT NOT NULL DEFAULT 'queued',
    stage        TEXT DEFAULT '',
    progress     REAL DEFAULT 0,
    log          TEXT DEFAULT '',
    created_at   REAL NOT NULL,
    started_at   REAL,
    finished_at  REAL
);
CREATE INDEX IF NOT EXISTS chapters_novel ON chapters(novel_id, position);
CREATE INDEX IF NOT EXISTS jobs_state ON jobs(state, created_at);
)SQL";

double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

DB::DB(const std::filesystem::path & path) {
    std::filesystem::create_directories(path.parent_path());
    if (sqlite3_open(path.u8string().c_str(), &db_) != SQLITE_OK) throw std::runtime_error("cannot open " + path.u8string());
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA journal_mode = WAL");
    exec(kSchema);
    migrate();
}

DB::~DB() {
    if (db_) sqlite3_close(db_);
}

void DB::exec(const std::string & sql) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    char * err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "sqlite error";
        sqlite3_free(err);
        throw std::runtime_error(m + " in: " + sql.substr(0, 200));
    }
}

namespace {

struct Stmt {
    sqlite3_stmt * s = nullptr;
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
};

void bind(sqlite3 * db, sqlite3_stmt * s, const std::vector<json> & args) {
    for (size_t i = 0; i < args.size(); ++i) {
        const int k = static_cast<int>(i) + 1;
        const json & a = args[i];
        int rc;
        if (a.is_null()) rc = sqlite3_bind_null(s, k);
        else if (a.is_boolean()) rc = sqlite3_bind_int(s, k, a.get<bool>() ? 1 : 0);
        else if (a.is_number_integer()) rc = sqlite3_bind_int64(s, k, a.get<int64_t>());
        else if (a.is_number()) rc = sqlite3_bind_double(s, k, a.get<double>());
        else if (a.is_string()) {
            const auto & str = a.get_ref<const std::string &>();
            rc = sqlite3_bind_text(s, k, str.c_str(), static_cast<int>(str.size()), SQLITE_TRANSIENT);
        } else {
            const std::string str = a.dump();
            rc = sqlite3_bind_text(s, k, str.c_str(), static_cast<int>(str.size()), SQLITE_TRANSIENT);
        }
        if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
}

json column(sqlite3_stmt * s, int c) {
    switch (sqlite3_column_type(s, c)) {
        case SQLITE_INTEGER: return sqlite3_column_int64(s, c);
        case SQLITE_FLOAT: return sqlite3_column_double(s, c);
        case SQLITE_TEXT:
            return std::string(reinterpret_cast<const char *>(sqlite3_column_text(s, c)), sqlite3_column_bytes(s, c));
        case SQLITE_BLOB:
            return std::string(static_cast<const char *>(sqlite3_column_blob(s, c)), sqlite3_column_bytes(s, c));
        default: return nullptr;
    }
}

}  // namespace

std::vector<json> DB::all(const std::string & sql, const std::vector<json> & args) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Stmt st;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string(sqlite3_errmsg(db_)) + " in: " + sql.substr(0, 200));
    bind(db_, st.s, args);
    std::vector<json> rows;
    int rc;
    while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
        json row = json::object();
        for (int c = 0; c < sqlite3_column_count(st.s); ++c) row[sqlite3_column_name(st.s, c)] = column(st.s, c);
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return rows;
}

std::optional<json> DB::one(const std::string & sql, const std::vector<json> & args) {
    auto rows = all(sql, args);
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

int64_t DB::run(const std::string & sql, const std::vector<json> & args) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    all(sql, args);
    return sqlite3_last_insert_rowid(db_);
}

void DB::migrate() {
    auto cols = [&](const std::string & t) {
        std::set<std::string> out;
        for (const auto & r : all("PRAGMA table_info(" + t + ")")) out.insert(r["name"].get<std::string>());
        return out;
    };
    const auto ch = cols("chapters"), jobs = cols("jobs"), chars = cols("characters");
    if (!chars.count("pitch")) {
        exec("ALTER TABLE characters ADD COLUMN pitch TEXT NOT NULL DEFAULT 'all'");
        exec("UPDATE characters SET pitch = CASE WHEN age IN ('child','teen') THEN 'high' "
             "WHEN age = 'elder' THEN 'low' ELSE 'all' END");
    }
    if (ch.count("needs_review") && !ch.count("quality_issues"))
        exec("ALTER TABLE chapters RENAME COLUMN needs_review TO quality_issues");
    else if (!ch.count("quality_issues"))
        exec("ALTER TABLE chapters ADD COLUMN quality_issues TEXT DEFAULT ''");
    if (!ch.count("director")) exec("ALTER TABLE chapters ADD COLUMN director TEXT DEFAULT ''");
    if (!jobs.count("director")) exec("ALTER TABLE jobs ADD COLUMN director TEXT DEFAULT ''");
    // chapter steps (analysis -> script -> narration)
    if (!jobs.count("kind")) exec("ALTER TABLE jobs ADD COLUMN kind TEXT NOT NULL DEFAULT 'narrate'");
    if (!ch.count("analyzed_at")) exec("ALTER TABLE chapters ADD COLUMN analyzed_at REAL");
    if (!ch.count("scripted_at")) exec("ALTER TABLE chapters ADD COLUMN scripted_at REAL");
    if (!ch.count("script_producer")) exec("ALTER TABLE chapters ADD COLUMN script_producer TEXT DEFAULT ''");
    if (!ch.count("to_check")) exec("ALTER TABLE chapters ADD COLUMN to_check INTEGER NOT NULL DEFAULT 0");   // disputed speakers left to check
}

std::vector<json> DB::cast(int64_t novel_id) {
    auto rows = all("SELECT * FROM characters WHERE novel_id=? ORDER BY lines DESC, name", {novel_id});
    for (auto & r : rows) r["aliases"] = json::parse(r["aliases"].get<std::string>());
    return rows;
}

void DB::upsert_character(int64_t novel_id, const std::string & name, json fields) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (fields.contains("aliases")) {
        std::set<std::string> a;
        for (const auto & x : fields["aliases"]) a.insert(x.get<std::string>());
        fields["aliases"] = json(std::vector<std::string>(a.begin(), a.end())).dump();
    }
    auto existing = one("SELECT id FROM characters WHERE novel_id=? AND name=?", {novel_id, name});
    std::vector<json> args;
    if (existing) {
        if (fields.empty()) return;
        std::string sets;
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            sets += (sets.empty() ? "" : ", ") + it.key() + "=?";
            args.push_back(it.value());
        }
        args.push_back((*existing)["id"]);
        run("UPDATE characters SET " + sets + " WHERE id=?", args);
    } else {
        std::string cols = "novel_id, name", qs = "?, ?";
        args = {novel_id, name};
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            cols += ", " + it.key();
            qs += ", ?";
            args.push_back(it.value());
        }
        run("INSERT INTO characters (" + cols + ") VALUES (" + qs + ")", args);
    }
}

}  // namespace rm
