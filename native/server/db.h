// SQLite state: novels, chapters (with narration state), cast registry, jobs. Rows are JSON objects.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace rm {

using json = nlohmann::json;

double now();

class DB {
public:
    explicit DB(const std::filesystem::path & path);
    ~DB();

    // parameters: null, bool, number, string
    std::vector<json> all(const std::string & sql, const std::vector<json> & args = {});
    std::optional<json> one(const std::string & sql, const std::vector<json> & args = {});
    int64_t run(const std::string & sql, const std::vector<json> & args = {});   // lastrowid
    void exec(const std::string & sql);

    // runs f inside BEGIN/COMMIT (ROLLBACK on exception)
    template <class F> void tx(F && f) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        exec("BEGIN");
        try {
            f();
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
    }

    // characters: aliases decoded from their JSON column
    std::vector<json> cast(int64_t novel_id);
    // fields: column -> value; "aliases" is given as a JSON array (stored sorted, unique)
    void upsert_character(int64_t novel_id, const std::string & name, json fields);

private:
    void migrate();
    sqlite3 * db_ = nullptr;
    std::recursive_mutex mu_;
};

}  // namespace rm
