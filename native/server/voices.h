// Voice pool + automatic, persistent casting.
//
// Pool layout: pool.json + <id>.wav + <id>.txt (reference clip and its transcript) + samples/<id>.wav, in two
// places: the voices shipped with the app (read-only) and the ones the user downloaded from the voice catalog
// (<data>/voices/downloaded). The user's gender flips and bans live in <data>/voices/overrides.json, applied on top.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rm {

using json = nlohmann::json;

class Pool {
public:
    // dirs: voice pool folders, first wins on duplicate ids; overrides: the user's gender flips and bans (JSON file)
    Pool(const std::vector<std::filesystem::path> & dirs, const std::filesystem::path & overrides);
    // voices as JSON objects with effective "gender", "metadata_gender", "banned", "band", "label"
    std::vector<json> voices;
    std::vector<const json *> usable(const std::optional<std::string> & gender = std::nullopt) const;
    const json * get(const std::string & id) const;
    std::pair<std::filesystem::path, std::string> ref(const std::string & id) const;   // wav, transcript
    std::filesystem::path sample(const std::string & id) const;                          // the UI's sample line
    bool bundled(const std::string & id) const;   // shipped with the app (read-only) rather than downloaded
    // gender: "male"/"female" to override, reset_gender to drop the override; banned when given
    void set_override(const std::string & id, const std::optional<std::string> & gender,
                      const std::optional<bool> & banned, bool reset_gender);

private:
    std::map<std::string, std::filesystem::path> dir_of_;   // voice id -> its folder
    std::filesystem::path first_dir_;                        // the voices shipped with the app
    std::filesystem::path overrides_path_;
    json overrides_;
};

// Voices the user removed (overrides "banned": true): hidden if shipped with the app, deleted if downloaded;
// "Download all" skips them. Works for ids no longer in the pool.
std::set<std::string> removed_voices(const std::filesystem::path & overrides);
void set_voice_removed(const std::filesystem::path & overrides, const std::string & id, bool removed);

std::string display_name(const std::string & reader);   // "JenniferRutters" -> "Jennifer Rutters"
std::string voice_label(const json & v);                  // "Tim Bower (male, low pitch)"
// initial pitch preference from the director's age / voice description: "low", "high" or "all"
std::string pitch_from_director(const json & entry);
std::string default_narrator(const Pool & pool, const std::string & gender = "male");
// voices for new characters (most lines first), far in pitch from same-gender voices already in use
std::map<std::string, std::string> assign(const Pool & pool, const std::vector<json> & cast, const std::string & narrator,
                                          const std::vector<std::pair<std::string, json>> & fresh,
                                          const std::map<std::string, int> & counts);

}  // namespace rm
