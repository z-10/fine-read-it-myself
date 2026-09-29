// Voice pool builder (port of the Python pool.py): LibriTTS-R (CC BY 4.0) -> <data>/voices.
//   1. per reader: consecutive utterances of one chapter forming a 7-11 s reference clip + its transcript
//   2. median pitch (f0); per gender, readers spread evenly across the pitch range
//   3. QA: clone a test line twice with VoxCPM2; unstable voices are rejected (never shown or cast)
//   4. the sample line each usable voice plays in the UI
// The .tar.gz subsets are read as streams (no extraction).
#pragma once

#include "engines.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rm {

struct PoolProgress {
    std::function<void(const std::string &)> log;
    std::function<void(const std::string &, double)> stage;   // stage name, fraction 0..1
};

// subsets: LibriTTS-R .tar.gz files (e.g. dev_clean.tar.gz); doc: doc.tar.gz (speaker metadata).
// per_gender > 0: that many readers per gender, spread across the pitch range (ids M001.., F001..);
// per_gender <= 0: every reader, ids S<LibriTTS speaker>. keep: an earlier pool whose voices are taken over as
// they are (same ids and files, not tested again). Test results and samples are saved as they are made, so an
// interrupted build resumes where it stopped.
void build_voice_pool(const std::vector<std::filesystem::path> & subsets, const std::filesystem::path & doc,
                      const std::filesystem::path & pool_dir, int per_gender, Engines & engines, const PoolProgress & p,
                      const std::filesystem::path & keep = {});

// copies the usable voices (QA passed) of a built pool into `bundle` for shipping with the app:
// pool.json (usable entries), <id>.wav/.txt, samples/<id>.wav, ATTRIBUTION.txt
// only: export just these ids (empty: every usable voice)
void export_voice_pool(const std::filesystem::path & pool_dir, const std::filesystem::path & bundle,
                       const std::vector<std::string> & only = {});

}  // namespace rm
