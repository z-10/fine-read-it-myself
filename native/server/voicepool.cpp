#include "voicepool.h"

#include "audio_io.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace rm {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char * kLicense = "CC BY 4.0 (LibriTTS-R, https://www.openslr.org/141/)";
const char * kTestLine = "Well, that took longer than I expected, but I think we finally have everything we need.";
const char * kSampleLine = "The road wound down through the hills toward the river, and by evening the lights of the "
                           "town were finally in sight.";
constexpr double kMaxRef = 11.0, kGap = 0.3;   // VoxCPM2's AudioVAE rejects longer reference clips

// ---------------------------------------------------------------- .tar.gz stream

class Gzip {
public:
    explicit Gzip(const fs::path & p) : f_(p, std::ios::binary), in_(1 << 20) {
        if (!f_) throw std::runtime_error("cannot open " + p.u8string());
        unsigned char h[10];
        f_.read(reinterpret_cast<char *>(h), 10);
        if (h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) throw std::runtime_error("not a gzip file: " + p.u8string());
        const int flags = h[3];
        if (flags & 4) {   // FEXTRA
            unsigned char x[2];
            f_.read(reinterpret_cast<char *>(x), 2);
            f_.ignore(x[0] | x[1] << 8);
        }
        if (flags & 8) while (f_.get() > 0) {}   // FNAME
        if (flags & 16) while (f_.get() > 0) {}  // FCOMMENT
        if (flags & 2) f_.ignore(2);             // FHCRC
        std::memset(&z_, 0, sizeof z_);
        if (mz_inflateInit2(&z_, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) throw std::runtime_error("inflate init failed");
        f_.seekg(0, std::ios::end);
        size_ = f_.tellg();
        f_.seekg(10);
        // re-skip header fields for the actual position
        f_.clear();
        f_.seekg(0);
        f_.read(reinterpret_cast<char *>(h), 10);
        if (flags & 4) {
            unsigned char x[2];
            f_.read(reinterpret_cast<char *>(x), 2);
            f_.ignore(x[0] | x[1] << 8);
        }
        if (flags & 8) while (f_.get() > 0) {}
        if (flags & 16) while (f_.get() > 0) {}
        if (flags & 2) f_.ignore(2);
    }
    ~Gzip() { mz_inflateEnd(&z_); }
    // reads exactly n bytes (throws at end of stream)
    void read(char * out, size_t n) {
        while (n) {
            if (z_.avail_in == 0) {
                f_.read(reinterpret_cast<char *>(in_.data()), static_cast<std::streamsize>(in_.size()));
                z_.next_in = in_.data();
                z_.avail_in = static_cast<unsigned>(f_.gcount());
                if (!z_.avail_in && done_) throw std::runtime_error("unexpected end of archive");
            }
            z_.next_out = reinterpret_cast<unsigned char *>(out);
            z_.avail_out = static_cast<unsigned>(std::min<size_t>(n, 1u << 30));
            const int rc = mz_inflate(&z_, MZ_NO_FLUSH);
            const size_t got = reinterpret_cast<char *>(z_.next_out) - out;
            out += got;
            n -= got;
            if (rc == MZ_STREAM_END) {
                done_ = true;
                if (n) throw std::runtime_error("unexpected end of archive");
            } else if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
                throw std::runtime_error("corrupt gzip data");
            }
        }
    }
    void skip(size_t n) {
        char buf[65536];
        while (n) {
            const size_t k = std::min(n, sizeof buf);
            read(buf, k);
            n -= k;
        }
    }
    double fraction() { return size_ > 0 ? static_cast<double>(f_.tellg()) / static_cast<double>(size_) : 0.0; }

private:
    std::ifstream f_;
    std::vector<unsigned char> in_;
    mz_stream z_;
    bool done_ = false;
    std::streamoff size_ = 0;
};

// calls fn(path, size, read_data, skip_data) for every regular file of a .tar.gz, in archive order
template <class F> void for_each_tar_file(Gzip & gz, F && fn) {
    std::string long_name;
    while (true) {
        char h[512];
        try {
            gz.read(h, 512);
        } catch (const std::exception &) {
            return;
        }
        if (h[0] == 0) return;   // end-of-archive block
        const uint64_t size = std::strtoull(std::string(h + 124, 12).c_str(), nullptr, 8);
        const char type = h[156];
        std::string name = long_name.empty() ? std::string(h, strnlen(h, 100)) : long_name;
        long_name.clear();
        if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) name = std::string(h + 345, strnlen(h + 345, 155)) + "/" + name;
        const uint64_t padded = (size + 511) / 512 * 512;
        if (type == 'L') {   // GNU long name
            std::string n(padded, '\0');
            gz.read(n.data(), padded);
            long_name = std::string(n.c_str());
            continue;
        }
        if (type == 'x') {   // pax header: path=...
            std::string pax(padded, '\0');
            gz.read(pax.data(), padded);
            const size_t p = pax.find(" path=");
            if (p != std::string::npos) long_name = pax.substr(p + 6, pax.find('\n', p) - p - 6);
            continue;
        }
        if (type != '0' && type != 0) {
            gz.skip(padded);
            continue;
        }
        bool consumed = false;
        auto read_data = [&]() {
            std::string d(size, '\0');
            gz.read(d.data(), size);
            gz.skip(padded - size);
            consumed = true;
            return d;
        };
        fn(name, size, read_data);
        if (!consumed) gz.skip(padded);
    }
}

// ---------------------------------------------------------------- audio

struct Pcm {
    std::vector<int16_t> x;
    int sr = 0;
};

Pcm parse_wav(const std::string & d) {
    Pcm p;
    if (d.size() < 44 || d.compare(0, 4, "RIFF") != 0) return p;
    size_t pos = 12;
    int channels = 1, bits = 16;
    while (pos + 8 <= d.size()) {
        uint32_t size;
        std::memcpy(&size, &d[pos + 4], 4);
        if (d.compare(pos, 4, "fmt ") == 0) {
            uint16_t ch, b;
            uint32_t sr;
            std::memcpy(&ch, &d[pos + 10], 2);
            std::memcpy(&sr, &d[pos + 12], 4);
            std::memcpy(&b, &d[pos + 22], 2);
            channels = ch;
            bits = b;
            p.sr = static_cast<int>(sr);
        } else if (d.compare(pos, 4, "data") == 0) {
            if (bits != 16) return Pcm{};
            const size_t n = std::min<size_t>(size, d.size() - pos - 8) / 2 / channels;
            p.x.resize(n);
            const auto * s = reinterpret_cast<const int16_t *>(&d[pos + 8]);
            for (size_t i = 0; i < n; ++i) p.x[i] = s[i * channels];   // first channel
            return p;
        }
        pos += 8 + size + (size & 1);
    }
    return p;
}

// pool.py median_f0: autocorrelation pitch of voiced 40 ms frames, 60-400 Hz
double median_f0(const std::vector<float> & x, int sr) {
    const int fr = static_cast<int>(sr * 0.04), lo = sr / 400, hi = sr / 60;
    std::vector<double> vals;
    std::vector<float> s(fr);
    for (size_t i = 0; i + fr < x.size(); i += fr / 2) {
        double mean = 0;
        for (int k = 0; k < fr; ++k) mean += x[i + k];
        mean /= fr;
        double e = 0;
        for (int k = 0; k < fr; ++k) {
            s[k] = static_cast<float>(x[i + k] - mean);
            e += static_cast<double>(s[k]) * s[k];
        }
        if (std::sqrt(e / fr) < 500) continue;
        auto ac = [&](int lag) {
            double a = 0;
            for (int t = 0; t + lag < fr; ++t) a += static_cast<double>(s[t]) * s[t + lag];
            return a;
        };
        const double a0 = ac(0);
        int best = lo;
        double bv = ac(lo);
        for (int k = lo + 1; k < hi; ++k) {
            const double v = ac(k);
            if (v > bv) { bv = v; best = k; }
        }
        if (bv > 0.3 * a0) vals.push_back(static_cast<double>(sr) / best);
    }
    if (vals.size() <= 5) return 0;
    std::sort(vals.begin(), vals.end());
    const size_t n = vals.size();
    return n % 2 ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
}

std::vector<float> to_float16scale(const std::vector<int16_t> & x) { return std::vector<float>(x.begin(), x.end()); }
std::vector<float> audio_to_int_scale(const Audio & a) {
    std::vector<float> v(a.samples.size());
    for (size_t i = 0; i < v.size(); ++i) v[i] = a.samples[i] * 32768.0f;
    return v;
}

struct Utt {
    std::string chapter, name, text;
    std::vector<int16_t> x;
    int sr;
    double dur;
};

struct Candidate {
    std::string speaker, subset, chapter, text, name;
    double f0;
    std::vector<int16_t> audio;
    int sr;
};

// pool.py build_clip: consecutive utterances of one chapter, 7-11 s including 0.3 s gaps
bool build_clip(std::vector<Utt> & utts, Candidate & c) {
    std::map<std::string, std::vector<Utt *>> by_chapter;
    for (auto & u : utts) by_chapter[u.chapter].push_back(&u);
    std::vector<Utt *> best;
    double best_total = -1;
    std::string best_chapter;
    for (auto & [chapter, us] : by_chapter) {
        std::sort(us.begin(), us.end(), [](const Utt * a, const Utt * b) { return a->name < b->name; });
        for (size_t i = 0; i < us.size(); ++i) {
            double total = 0;
            std::vector<Utt *> parts;
            for (size_t j = i; j < us.size(); ++j) {
                const double add = us[j]->dur + (parts.empty() ? 0.0 : kGap);
                if (total + add > kMaxRef) break;
                parts.push_back(us[j]);
                total += add;
            }
            if (total >= 7 && total > best_total) {
                best = parts;
                best_total = total;
                best_chapter = chapter;
            }
        }
        if (best_total >= kMaxRef - 1) break;
    }
    if (best.empty()) return false;
    const int sr = best.front()->sr;
    const size_t gap = static_cast<size_t>(sr * 0.3);
    c.audio.clear();
    std::string text;
    for (size_t i = 0; i < best.size(); ++i) {
        if (i) c.audio.insert(c.audio.end(), gap, 0);
        c.audio.insert(c.audio.end(), best[i]->x.begin(), best[i]->x.end());
        text += (i ? " " : "") + best[i]->text;
    }
    c.sr = sr;
    c.text = text;
    c.chapter = best_chapter;
    return true;
}

std::string trim(const std::string & s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

void build_voice_pool(const std::vector<fs::path> & subsets, const fs::path & doc, const fs::path & pool_dir,
                      int per_gender, Engines & engines, const PoolProgress & p, const fs::path & keep) {
    // voices of an earlier pool, by LibriTTS speaker: taken over unchanged
    std::map<std::string, json> kept;
    if (!keep.empty()) {
        std::ifstream in(keep / "pool.json", std::ios::binary);
        if (!in) throw std::runtime_error("no pool.json in " + keep.u8string());
        for (const auto & v : json::parse(in)) {
            const std::string src = v.value("source", "");
            const size_t a = src.find(" speaker "), b = src.find(" chapter ");
            if (a != std::string::npos && b != std::string::npos) kept[src.substr(a + 9, b - a - 9)] = v;
        }
        p.log(std::to_string(kept.size()) + " voices kept from " + keep.u8string());
    }
    // reader metadata (speakers.tsv: READER GENDER SUBSET NAME)
    std::map<std::string, std::pair<std::string, std::string>> meta;   // reader -> (gender, name)
    {
        Gzip gz(doc);
        for_each_tar_file(gz, [&](const std::string & path, uint64_t, auto read) {
            if (path.size() < 12 || path.compare(path.size() - 12, 12, "speakers.tsv") != 0) return;
            std::istringstream in(read());
            for (std::string line; std::getline(in, line);) {
                std::vector<std::string> cols;
                std::stringstream ss(line);
                for (std::string c; std::getline(ss, c, '\t');) cols.push_back(trim(c));
                if (cols.size() < 2 || cols[0].empty() || !std::all_of(cols[0].begin(), cols[0].end(), ::isdigit)) continue;
                if (cols[1] != "M" && cols[1] != "F") continue;
                meta[cols[0]] = {cols[1] == "M" ? "male" : "female", cols.size() > 3 ? cols[3] : ""};
            }
        });
    }
    if (meta.empty()) throw std::runtime_error("no reader metadata in " + doc.u8string());
    p.log(std::to_string(meta.size()) + " LibriTTS-R readers with metadata");

    // one pass per subset: utterances of the current reader in memory, best clip when the reader changes
    std::map<std::string, std::vector<Candidate>> cands{{"male", {}}, {"female", {}}};
    for (size_t si = 0; si < subsets.size(); ++si) {
        Gzip gz(subsets[si]);
        std::string cur_speaker, cur_subset;
        std::vector<Utt> utts;
        std::map<std::string, std::string> texts;   // utterance stem -> original text
        std::map<std::string, Utt> waiting;          // wav read before its text
        auto flush = [&] {
            if (cur_speaker.empty()) return;
            for (auto & [stem, u] : waiting) {
                auto t = texts.find(stem);
                if (t != texts.end()) {
                    u.text = t->second;
                    utts.push_back(std::move(u));
                }
            }
            auto m = meta.find(cur_speaker);
            Candidate c;
            if (kept.count(cur_speaker)) {   // taken over from the earlier pool
            } else if (m != meta.end() && build_clip(utts, c)) {
                const double f0 = median_f0(to_float16scale(c.audio), c.sr);
                if (f0 > 0) {
                    c.speaker = cur_speaker;
                    c.subset = cur_subset;
                    c.f0 = f0;
                    c.name = m->second.second;
                    cands[m->second.first].push_back(std::move(c));
                }
            }
            utts.clear();
            texts.clear();
            waiting.clear();
        };
        size_t n_files = 0;
        for_each_tar_file(gz, [&](const std::string & path, uint64_t, auto read) {
            // LibriTTS_R/<subset>/<speaker>/<chapter>/<utt>.wav|.original.txt
            std::vector<std::string> parts;
            std::stringstream ss(path);
            for (std::string c; std::getline(ss, c, '/');) parts.push_back(c);
            if (parts.size() < 5) return;
            const std::string subset = parts[parts.size() - 4], speaker = parts[parts.size() - 3], chapter = parts[parts.size() - 2];
            const std::string file = parts.back();
            if (speaker != cur_speaker) {
                flush();
                cur_speaker = speaker;
                cur_subset = subset;
            }
            if (++n_files % 2000 == 0) p.stage("Reading " + subsets[si].filename().u8string(), gz.fraction());
            const auto ends = [&](const char * suf) {
                const size_t n = std::strlen(suf);
                return file.size() > n && file.compare(file.size() - n, n, suf) == 0;
            };
            if (ends(".original.txt")) {
                texts[file.substr(0, file.size() - 13)] = trim(read());
            } else if (ends(".wav")) {
                Pcm pcm = parse_wav(read());
                if (!pcm.sr || pcm.x.empty()) return;
                const double dur = static_cast<double>(pcm.x.size()) / pcm.sr;
                int peak = 0;
                for (int16_t v : pcm.x) peak = std::max(peak, std::abs(static_cast<int>(v)));
                if (dur < 2.5 || dur > 9 || peak <= 8000 || peak >= 32000) return;
                const std::string stem = file.substr(0, file.size() - 4);
                waiting[stem] = Utt{chapter, stem, "", std::move(pcm.x), pcm.sr, dur};
            }
        });
        flush();
        p.log(subsets[si].filename().u8string() + ": " + std::to_string(cands["male"].size()) + " male / " +
              std::to_string(cands["female"].size()) + " female candidates so far");
    }

    // per gender: readers spread evenly across the pitch range (or all of them)
    fs::create_directories(pool_dir / "samples");
    json pool = json::array();
    for (const auto & [spk, v] : kept) {   // files copied as they are; their QA stands
        const std::string id = v["id"];
        for (const std::string f : {id + ".wav", id + ".txt", "samples/" + id + ".wav"})
            if (fs::exists(keep / fs::u8path(f))) fs::copy_file(keep / fs::u8path(f), pool_dir / fs::u8path(f), fs::copy_options::overwrite_existing);
        pool.push_back(v);
    }
    for (const auto & [g, prefix] : std::vector<std::pair<std::string, std::string>>{{"male", "M"}, {"female", "F"}}) {
        auto & c = cands[g];
        std::stable_sort(c.begin(), c.end(), [](const Candidate & a, const Candidate & b) { return a.f0 < b.f0; });
        const int n = per_gender > 0 ? std::min<int>(per_gender, static_cast<int>(c.size())) : static_cast<int>(c.size());
        for (int k = 0; k < n; ++k) {
            const double pos = per_gender <= 0 ? k : (n > 1 ? static_cast<double>(k) * (c.size() - 1) / (n - 1) : 0.0);
            const Candidate & v = c[static_cast<size_t>(std::nearbyint(pos))];
            char idbuf[16];
            if (per_gender > 0) std::snprintf(idbuf, sizeof idbuf, "%s%03d", prefix.c_str(), k + 1);
            else std::snprintf(idbuf, sizeof idbuf, "S%s", v.speaker.c_str());
            const std::string id = idbuf;
            Audio a;
            a.sample_rate = v.sr;
            a.samples.resize(v.audio.size());
            for (size_t i = 0; i < a.samples.size(); ++i) a.samples[i] = v.audio[i] / 32768.0f;
            write_wav16(pool_dir / (std::string(id) + ".wav"), a);
            std::ofstream(pool_dir / (std::string(id) + ".txt"), std::ios::binary) << v.text;
            pool.push_back({{"id", id}, {"gender", g}, {"f0", std::lround(v.f0)},
                            {"source", "LibriTTS-R " + v.subset + " speaker " + v.speaker + " chapter " + v.chapter},
                            {"license", kLicense}, {"reader", v.name.empty() ? "LibriVox volunteer" : v.name}});
        }
        p.log(g + ": " + std::to_string(c.size()) + " candidates -> " + std::to_string(n) + " voices");
    }

    // QA: clone a test line with two seeds; reject voices whose clone drifts, wavers or has an odd length.
    // Results go to qa_cache.json as they come (resume); voices taken over keep theirs.
    const fs::path cache_file = pool_dir / "qa_cache.json";
    json cache = json::object();
    if (fs::exists(cache_file)) {
        std::ifstream in(cache_file, std::ios::binary);
        cache = json::parse(in);
    }
    std::vector<double> all_secs;
    for (size_t i = 0; i < pool.size(); ++i) {
        json & v = pool[i];
        p.stage("Testing voices", static_cast<double>(i) / pool.size());
        const std::string id = v["id"];
        if (v.contains("qa") && v["qa"].contains("ok")) continue;   // taken over
        if (cache.contains(id)) {
            v["qa"] = cache[id];
            for (const auto & s : v["qa"]["secs"]) all_secs.push_back(s.get<double>());
            continue;
        }
        std::ifstream tin(pool_dir / (id + ".txt"), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(tin)), std::istreambuf_iterator<char>());
        json f0s = json::array(), secs = json::array();
        try {
            for (uint32_t seed : {1234u, 777u}) {
                const Audio a = engines.speak(kTestLine, pool_dir / (id + ".wav"), text, seed);
                const double f0 = median_f0(audio_to_int_scale(a), a.sample_rate);
                f0s.push_back(f0 > 0 ? json(std::lround(f0)) : json(nullptr));
                const double s = static_cast<double>(a.samples.size()) / a.sample_rate;
                secs.push_back(std::round(s * 100) / 100);
                all_secs.push_back(s);
            }
            v["qa"] = {{"clone_f0", f0s}, {"secs", secs}};
        } catch (const std::exception & e) {
            v["qa"] = {{"clone_f0", json::array()}, {"secs", json::array()}, {"error", std::string(e.what()).substr(0, 200)}};
        }
        cache[id] = v["qa"];
        if (i % 10 == 0 || i + 1 == pool.size()) {
            std::ofstream(pool_dir / "qa_cache.json.tmp", std::ios::binary) << cache.dump();
            fs::rename(pool_dir / "qa_cache.json.tmp", cache_file);
        }
        if (i % 50 == 0) p.log("tested " + std::to_string(i) + " / " + std::to_string(pool.size()));
    }
    std::sort(all_secs.begin(), all_secs.end());
    const double med = all_secs.empty() ? 0 : (all_secs.size() % 2 ? all_secs[all_secs.size() / 2]
                                                                     : 0.5 * (all_secs[all_secs.size() / 2 - 1] + all_secs[all_secs.size() / 2]));
    int ok = 0;
    for (auto & v : pool) {
        json & qa = v["qa"];
        if (qa.contains("ok")) {   // taken over from the earlier pool: its verdict stands
            ok += qa["ok"].get<bool>();
            continue;
        }
        std::vector<std::string> why;
        if (qa.contains("error")) {
            why.push_back("clone failed: " + qa["error"].get<std::string>());
        } else {
            const auto & f0s = qa["clone_f0"];
            if (f0s[0].is_null() || f0s[1].is_null()) {
                why.push_back("no pitch detected");
            } else {
                const double ref = v["f0"].get<double>(), a = f0s[0].get<double>(), b = f0s[1].get<double>();
                const double drift = std::max(std::fabs(a - ref), std::fabs(b - ref)) / ref;
                const double spread = std::fabs(a - b) / std::min(a, b);
                qa["drift"] = std::round(drift * 1000) / 1000;
                qa["spread"] = std::round(spread * 1000) / 1000;
                if (drift > 0.2) why.push_back("clone pitch drifts from reference");
                if (spread > 0.15) why.push_back("unstable between seeds");
            }
            for (const auto & s : qa["secs"])
                if (s.get<double>() < 0.6 * med || s.get<double>() > 1.6 * med) {
                    why.push_back("abnormal length");
                    break;
                }
        }
        qa["ok"] = why.empty();
        qa["reasons"] = why;
        ok += why.empty();
    }
    p.log("QA: " + std::to_string(ok) + "/" + std::to_string(pool.size()) + " voices usable");

    // pitch bands relative within each gender (Pool recomputes them after user gender flips too)
    for (const char * g : {"male", "female"}) {
        std::vector<json *> vs;
        for (auto & v : pool)
            if (v["gender"] == g) vs.push_back(&v);
        std::stable_sort(vs.begin(), vs.end(), [](const json * a, const json * b) { return (*a)["f0"] < (*b)["f0"]; });
        for (size_t r = 0; r < vs.size(); ++r) {
            const double frac = static_cast<double>(r + 1) / vs.size();
            (*vs[r])["band"] = frac <= 1.0 / 3 ? "low" : (frac <= 2.0 / 3 ? "mid" : "high");
        }
    }

    // the sample line each usable voice plays in the UI
    fs::create_directories(pool_dir / "samples");
    for (size_t i = 0; i < pool.size(); ++i) {
        const json & v = pool[i];
        p.stage("Rendering voice samples", static_cast<double>(i) / pool.size());
        if (!v["qa"]["ok"].get<bool>()) continue;
        const std::string id = v["id"];
        if (fs::exists(pool_dir / "samples" / (id + ".wav"))) continue;   // taken over, or made before a restart
        if (i % 50 == 0) p.log("samples " + std::to_string(i) + " / " + std::to_string(pool.size()));
        std::ifstream tin(pool_dir / (id + ".txt"), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(tin)), std::istreambuf_iterator<char>());
        write_wav16(pool_dir / "samples" / (id + ".wav"), engines.speak(kSampleLine, pool_dir / (id + ".wav"), text));
    }

    std::string attrib = "Reference voices derived from LibriTTS-R (Koizumi et al., 2023), CC BY 4.0, itself derived from\n"
                         "LibriTTS / LibriVox public-domain recordings.\n\n";
    for (const auto & v : pool) {
        const std::string src = v["source"];
        attrib += v["id"].get<std::string>() + ": " + src.substr(0, src.find(" chapter ")) + " (" + v["reader"].get<std::string>() +
                  "), " + kLicense + "\n";
    }
    std::ofstream(pool_dir / "ATTRIBUTION.txt", std::ios::binary) << attrib;
    std::ofstream(pool_dir / "pool.json", std::ios::binary) << pool.dump(1);   // last: its presence means "installed"
}

void export_voice_pool(const fs::path & pool_dir, const fs::path & bundle, const std::vector<std::string> & only) {
    std::ifstream in(pool_dir / "pool.json", std::ios::binary);
    if (!in) throw std::runtime_error("no pool.json in " + pool_dir.u8string());
    const json pool = json::parse(in);
    fs::create_directories(bundle / "samples");
    json out = json::array();
    std::string attrib = "Reference voices derived from LibriTTS-R (Koizumi et al., 2023), CC BY 4.0, itself derived from\n"
                         "LibriTTS / LibriVox public-domain recordings.\n"
                         "Changes: short reference clips were cut from the original recordings; the sample lines were\n"
                         "synthesized with VoxCPM2 from those clips.\n\n";
    for (const auto & v : pool) {
        if (!v.contains("qa") || !v["qa"].value("ok", false)) continue;   // voices that failed QA are never used
        const std::string id = v["id"];
        if (!only.empty() && std::find(only.begin(), only.end(), id) == only.end()) continue;
        for (const std::string & f : {id + ".wav", id + ".txt", "samples/" + id + ".wav"})
            fs::copy_file(pool_dir / fs::u8path(f), bundle / fs::u8path(f), fs::copy_options::overwrite_existing);
        out.push_back(v);
        const std::string src = v["source"];
        attrib += id + ": " + src.substr(0, src.find(" chapter ")) + " (" + v.value("reader", std::string("LibriVox volunteer")) +
                  "), " + kLicense + "\n";
    }
    std::ofstream(bundle / "ATTRIBUTION.txt", std::ios::binary) << attrib;
    std::ofstream(bundle / "pool.json", std::ios::binary) << out.dump(1);
    std::cout << out.size() << " voices -> " << bundle.u8string() << std::endl;
}

}  // namespace rm
