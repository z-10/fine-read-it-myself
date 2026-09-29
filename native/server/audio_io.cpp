#include "audio_io.h"

#include <glint/glint.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace rm {

void write_wav16(const std::filesystem::path & p, const Audio & a) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.u8string());
    const uint32_t n = static_cast<uint32_t>(a.samples.size()), sr = static_cast<uint32_t>(a.sample_rate);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + n * 2);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(sr);
    u32(sr * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(n * 2);
    std::vector<int16_t> pcm(n);
    for (uint32_t i = 0; i < n; ++i) pcm[i] = static_cast<int16_t>(std::clamp(a.samples[i], -1.0f, 1.0f) * 32767.0f);
    f.write(reinterpret_cast<const char *>(pcm.data()), n * 2);
}

Audio read_wav16(const std::filesystem::path & p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0) throw std::runtime_error("bad WAV " + p.u8string());
    Audio a;
    size_t pos = 12;
    uint16_t channels = 1, bits = 16;
    while (pos + 8 <= d.size()) {
        uint32_t size;
        std::memcpy(&size, &d[pos + 4], 4);
        if (std::memcmp(&d[pos], "fmt ", 4) == 0) {
            std::memcpy(&channels, &d[pos + 10], 2);
            uint32_t sr;
            std::memcpy(&sr, &d[pos + 12], 4);
            a.sample_rate = static_cast<int>(sr);
            std::memcpy(&bits, &d[pos + 22], 2);
        } else if (std::memcmp(&d[pos], "data", 4) == 0) {
            if (bits != 16) throw std::runtime_error("expected 16-bit WAV " + p.u8string());
            const size_t n = std::min<size_t>(size, d.size() - pos - 8) / 2 / channels;
            a.samples.resize(n);
            const auto * s = reinterpret_cast<const int16_t *>(&d[pos + 8]);
            for (size_t i = 0; i < n; ++i) {
                float acc = 0;
                for (int c = 0; c < channels; ++c) acc += s[i * channels + c] / 32768.0f;
                a.samples[i] = acc / channels;
            }
            break;
        }
        pos += 8 + size + (size & 1);
    }
    return a;
}

static std::vector<uint8_t> id3v23(const std::map<std::string, std::string> & tags) {
    static const std::map<std::string, std::string> ids = {{"title", "TIT2"}, {"album", "TALB"}, {"artist", "TPE1"}, {"track", "TRCK"}};
    std::vector<uint8_t> frames;
    for (const auto & [k, v] : tags) {
        auto it = ids.find(k);
        if (it == ids.end() || v.empty()) continue;
        // UTF-16 with BOM (encoding 1): the ID3v2.3 way to store non-Latin-1 text
        std::vector<uint8_t> body{1, 0xFF, 0xFE};
        for (size_t i = 0; i < v.size();) {
            uint32_t cp;
            const unsigned char c = v[i];
            int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
            for (int k2 = 1; k2 < n && i + k2 < v.size(); ++k2) cp = (cp << 6) | (static_cast<unsigned char>(v[i + k2]) & 0x3F);
            i += n;
            auto put = [&](uint16_t u) {
                body.push_back(static_cast<uint8_t>(u & 0xFF));
                body.push_back(static_cast<uint8_t>(u >> 8));
            };
            if (cp >= 0x10000) {
                cp -= 0x10000;
                put(static_cast<uint16_t>(0xD800 + (cp >> 10)));
                put(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
            } else {
                put(static_cast<uint16_t>(cp));
            }
        }
        const uint32_t size = static_cast<uint32_t>(body.size());
        frames.insert(frames.end(), it->second.begin(), it->second.end());
        for (int s = 3; s >= 0; --s) frames.push_back(static_cast<uint8_t>(size >> (8 * s)));
        frames.push_back(0);
        frames.push_back(0);
        frames.insert(frames.end(), body.begin(), body.end());
    }
    std::vector<uint8_t> out{'I', 'D', '3', 3, 0, 0};
    const uint32_t n = static_cast<uint32_t>(frames.size());   // synchsafe size
    for (int s = 3; s >= 0; --s) out.push_back(static_cast<uint8_t>((n >> (7 * s)) & 0x7F));
    out.insert(out.end(), frames.begin(), frames.end());
    return out;
}

void write_mp3(const std::filesystem::path & p, const Audio & a, int bitrate_kbps,
               const std::map<std::string, std::string> & tags) {
    glint_config cfg{};
    cfg.sample_rate = a.sample_rate;
    cfg.num_channels = 1;
    cfg.mode = GLINT_MONO;
    cfg.bitrate = bitrate_kbps;
    cfg.quality = GLINT_QUALITY_NORMAL;
    cfg.vbr = GLINT_VBR_OFF;
    if (glint_check_config(cfg.sample_rate, cfg.bitrate) < 0)
        throw std::runtime_error("MP3 encoder does not support " + std::to_string(a.sample_rate) + " Hz at " +
                                 std::to_string(bitrate_kbps) + " kbps");
    glint_t enc = glint_create(&cfg);
    if (!enc) throw std::runtime_error("MP3 encoder init failed");
    std::ofstream f(p, std::ios::binary);
    if (!f) {
        glint_destroy(enc);
        throw std::runtime_error("cannot write " + p.u8string());
    }
    const auto head = id3v23(tags);
    f.write(reinterpret_cast<const char *>(head.data()), static_cast<std::streamsize>(head.size()));
    const int spf = glint_samples_per_frame(enc);
    std::vector<float> block(spf);
    for (size_t off = 0; off < a.samples.size(); off += spf) {
        const size_t n = std::min<size_t>(spf, a.samples.size() - off);
        std::fill(block.begin(), block.end(), 0.0f);
        std::copy_n(a.samples.begin() + off, n, block.begin());
        const float * ch[1] = {block.data()};
        int size = 0;
        const uint8_t * out = glint_encode_float(enc, ch, &size);
        if (out && size > 0) f.write(reinterpret_cast<const char *>(out), size);
    }
    int size = 0;
    const uint8_t * out = glint_flush(enc, &size);
    if (out && size > 0) f.write(reinterpret_cast<const char *>(out), size);
    glint_destroy(enc);
}

}  // namespace rm
