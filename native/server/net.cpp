#include "net.h"

#include "httplib.h"

#include <fstream>
#include <thread>
#include <chrono>
#include <stdexcept>

namespace rm {

Url parse_url(const std::string & url) {
    Url u;
    const size_t s = url.find("://");
    if (s == std::string::npos) throw std::invalid_argument("not an absolute URL: " + url);
    u.scheme = url.substr(0, s);
    const size_t h = s + 3;
    const size_t p = url.find_first_of("/?#", h);
    u.host = url.substr(h, p == std::string::npos ? std::string::npos : p - h);
    u.path = p == std::string::npos ? "/" : url.substr(p);
    if (!u.path.empty() && u.path[0] != '/') u.path = "/" + u.path;
    const size_t frag = u.path.find('#');
    if (frag != std::string::npos) u.path = u.path.substr(0, frag);
    return u;
}

std::string url_join(const std::string & base, const std::string & ref) {
    if (ref.empty()) return base;
    if (ref.find("://") != std::string::npos && ref.find("://") < ref.find_first_of("/?#")) return ref;
    const Url b = parse_url(base);
    if (ref.rfind("//", 0) == 0) return b.scheme + ":" + ref;
    if (ref[0] == '/') return b.origin() + ref;
    if (ref[0] == '?') return b.origin() + b.path.substr(0, b.path.find('?')) + ref;
    if (ref[0] == '#') return base.substr(0, base.find('#')) + ref;
    std::string dir = b.path.substr(0, b.path.find('?'));
    dir = dir.substr(0, dir.rfind('/') + 1);
    std::string path = dir + ref;
    // resolve ./ and ../
    std::vector<std::string> parts;
    size_t a = 0;
    std::string query;
    const size_t q = path.find('?');
    if (q != std::string::npos) {
        query = path.substr(q);
        path = path.substr(0, q);
    }
    while (a <= path.size()) {
        const size_t e = path.find('/', a);
        const std::string seg = path.substr(a, e == std::string::npos ? std::string::npos : e - a);
        if (seg == "..") {
            if (parts.size() > 1) parts.pop_back();
        } else if (seg != ".") {
            parts.push_back(seg);
        }
        if (e == std::string::npos) break;
        a = e + 1;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
    if (out.empty() || out[0] != '/') out = "/" + out;
    return b.origin() + out + query;
}

namespace {

httplib::Client client(const Url & u, int timeout_s) {
    httplib::Client c(u.origin());
    c.set_follow_location(true);
    c.set_connection_timeout(30);
    c.set_read_timeout(timeout_s);
    c.set_write_timeout(timeout_s);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    c.enable_server_certificate_verification(true);
#endif
    return c;
}

HttpResponse convert(const httplib::Result & r) {
    HttpResponse out;
    if (!r) {
        out.error = httplib::to_string(r.error());
        return out;
    }
    out.status = r->status;
    out.body = r->body;
    return out;
}

httplib::Headers to_headers(const std::map<std::string, std::string> & h) {
    httplib::Headers out;
    for (const auto & [k, v] : h) out.emplace(k, v);
    return out;
}

}  // namespace

HttpResponse http_get(const std::string & url, const std::map<std::string, std::string> & headers, int timeout_s) {
    const Url u = parse_url(url);
    auto c = client(u, timeout_s);
    return convert(c.Get(u.path, to_headers(headers)));
}

void http_download(const std::string & url_in, const std::filesystem::path & dest,
                   const std::function<void(int64_t, int64_t)> & progress) {
    namespace fs = std::filesystem;
    fs::create_directories(dest.parent_path());
    const fs::path part = dest.u8string() + ".part";
    std::string url = url_in;
    int busy = 0;   // HTTP 429/503 answers so far: wait (Retry-After, else backoff) and ask again
    for (int redirects = 0; redirects < 10; ++redirects) {
        const Url u = parse_url(url);
        httplib::Client c(u.origin());
        c.set_follow_location(false);
        c.set_connection_timeout(30);
        c.set_read_timeout(120);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        c.enable_server_certificate_verification(true);
#endif
        const int64_t have = fs::exists(part) ? static_cast<int64_t>(fs::file_size(part)) : 0;
        httplib::Headers headers{{"User-Agent", "fine-read-it/0.2"}};
        if (have > 0) headers.emplace("Range", "bytes=" + std::to_string(have) + "-");
        std::string location;
        std::ofstream out;
        int64_t done = 0, total = 0;
        int status = 0, retry_after = 0;
        auto res = c.Get(
            u.path, headers,
            [&](const httplib::Response & r) {
                status = r.status;
                if (r.status == 429 || r.status == 503) {
                    retry_after = r.has_header("Retry-After") ? std::atoi(r.get_header_value("Retry-After").c_str()) : 0;
                    return false;
                }
                if (r.status >= 300 && r.status < 400) {
                    location = r.get_header_value("Location");
                    return false;   // stop here, follow below
                }
                if (r.status != 200 && r.status != 206) return false;
                const bool resumed = r.status == 206;
                const int64_t len = r.has_header("Content-Length") ? std::stoll(r.get_header_value("Content-Length")) : 0;
                done = resumed ? have : 0;
                total = resumed ? have + len : len;
                out.open(part, std::ios::binary | (resumed ? std::ios::app : std::ios::trunc));
                return static_cast<bool>(out);
            },
            [&](const char * data, size_t n) {
                out.write(data, static_cast<std::streamsize>(n));
                done += static_cast<int64_t>(n);
                if (progress) progress(done, total);
                return static_cast<bool>(out);
            });
        if (!location.empty()) {
            url = location.find("://") == std::string::npos ? url_join(url, location) : location;
            continue;
        }
        if ((status == 429 || status == 503) && busy < 8) {
            const int wait = retry_after > 0 ? std::min(retry_after, 300) : std::min(5 << busy, 120);
            std::this_thread::sleep_for(std::chrono::seconds(wait));
            ++busy;
            --redirects;
            continue;
        }
        if (status == 416 && have > 0) {   // .part already complete
            fs::rename(part, dest);
            return;
        }
        if (status != 200 && status != 206)
            throw std::runtime_error("download " + url_in + ": " + (status ? "HTTP " + std::to_string(status) : httplib::to_string(res.error())));
        out.close();
        if (!res) throw std::runtime_error("download " + url_in + " interrupted: " + httplib::to_string(res.error()) + " (it resumes on retry)");
        if (total && done != total) throw std::runtime_error("download " + url_in + " incomplete (it resumes on retry)");
        fs::rename(part, dest);
        return;
    }
    throw std::runtime_error("download " + url_in + ": too many redirects");
}

HttpResponse http_post(const std::string & url, const std::string & body, const std::string & content_type,
                       const std::map<std::string, std::string> & headers, int timeout_s) {
    const Url u = parse_url(url);
    auto c = client(u, timeout_s);
    return convert(c.Post(u.path, to_headers(headers), body, content_type));
}

}  // namespace rm
