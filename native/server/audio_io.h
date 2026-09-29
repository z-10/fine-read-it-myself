// Mono PCM helpers: 16-bit WAV segment cache files and MP3 output (glint encoder + ID3v2.3 tags).
#pragma once

#include "engines.h"

#include <filesystem>
#include <map>
#include <string>

namespace rm {

void write_wav16(const std::filesystem::path & p, const Audio & a);
Audio read_wav16(const std::filesystem::path & p);   // mono 16-bit PCM as written above

// tags: title (TIT2), album (TALB), artist (TPE1), track (TRCK)
void write_mp3(const std::filesystem::path & p, const Audio & a, int bitrate_kbps,
               const std::map<std::string, std::string> & tags);

}  // namespace rm
