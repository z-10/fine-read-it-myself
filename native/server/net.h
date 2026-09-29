// Outbound HTTP(S) (cpp-httplib + OpenSSL): page fetches and OpenAI-compatible director calls.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace rm {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;   // transport error ("" if a response arrived)
};

struct Url {
    std::string scheme, host, path;   // host may include ":port"; path includes the query
    std::string origin() const { return scheme + "://" + host; }
};

Url parse_url(const std::string & url);
std::string url_join(const std::string & base, const std::string & ref);   // urllib.parse.urljoin

HttpResponse http_get(const std::string & url, const std::map<std::string, std::string> & headers, int timeout_s);
// Streams url to `dest` (resuming from dest.part), following redirects across hosts. progress(done, total).
// Throws std::runtime_error on failure; renames .part to dest when complete.
void http_download(const std::string & url, const std::filesystem::path & dest,
                   const std::function<void(int64_t, int64_t)> & progress);

HttpResponse http_post(const std::string & url, const std::string & body, const std::string & content_type,
                       const std::map<std::string, std::string> & headers, int timeout_s);

}  // namespace rm
