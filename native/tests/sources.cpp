#include "sources.h"
#include "scrape.h"
#include "settings.h"
#include "library.h"
#include "httplib.h"

#include <fstream>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace rm;

void check(bool condition, const char * message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void rejects(F f, const char * message) {
    try { f(); } catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}

int main() {
    try {
        const auto sources = load_sources({});
        const auto & nf = sources.at("novelfire");
        const std::string base = "https://novelfire.net/book/the-regressed-son-of-a-duke-is-an-assassin";
        auto book = resolve(base, sources), chapter = resolve(base + "/chapter-1?read=1#top", sources);
        check(book && chapter && book->first->id == "novelfire", "NovelFire URL resolution");
        check(nf.novel_url(chapter->second) == base, "chapter canonicalization");
        check(nf.novel_key(chapter->second) == nf.novel_key(book->second), "chapter and book share identity");
        check(nf.match_url("http://www.novelfire.net/book/another/chapter-2").has_value(), "www and http variants");
        check(!nf.match_url("https://novelfire.net.evil.test/book/test"), "lookalike host rejected");
        check(!nf.match_url("https://novelfire.net/book/"), "empty slug rejected");
        check(nf.novel_key(*nf.match_url(base)) != nf.novel_key(*nf.match_url("https://novelfire.net/book/other")), "distinct slug identities");
        const auto & rr = sources.at("royalroad");
        check(rr.novel_key(*rr.match_url("https://www.royalroad.com/fiction/123/a/chapter/456/b")) == "royalroad:123", "existing Royal Road identity preserved");
        const auto & ww = sources.at("wuxiaworld");
        check(ww.novel_key(*ww.match_url("https://www.wuxiaworld.com/novel/first")) !=
              ww.novel_key(*ww.match_url("https://www.wuxiaworld.com/novel/second")), "Wuxiaworld novels do not collide");

        const std::string novel = R"HTML(
            <h1 class="novel-title">Example Novel</h1>
            <div class="author"><a class="property-item"><span>Example Author</span></a></div>
            <div class="cover"><img data-src="/covers/example.jpg" src="/placeholder.jpg"></div>
            <div class="summary"><div class="content">An example summary.</div></div>
            <div class="header-stats"><span><i class="icon-book-open"></i>1,234 Chapters</span><span>42 Views</span></div>
        )HTML";
        const auto info = parse_novel(novel, base, nf);
        check(info.title == "Example Novel" && info.author == "Example Author", "NovelFire metadata");
        check(info.cover == "https://novelfire.net/covers/example.jpg", "lazy relative cover");
        check(info.chapters.size() == 1234, "comma-separated chapter count includes every page");
        check(info.chapters.front().url == base + "/chapter-1" && info.chapters.back().url == base + "/chapter-1234", "numbered chapter URLs");
        const auto fallback = parse_novel(R"(<meta property="og:title" content="Fallback"><meta property="og:image" content="/cover.jpg"><div class="cover"><img></div><div class="header-stats">2 Chapters</div>)", base, nf);
        check(fallback.title == "Fallback" && fallback.cover == "https://novelfire.net/cover.jpg", "missing attributes use fallbacks");
        rejects([&] { parse_novel("<h1>Challenge</h1>", base, nf); }, "missing count must not silently import an empty book");
        rejects([&] { parse_novel("<div class='header-stats'>100001 Chapters</div>", base, nf); }, "unreasonable chapter count rejected");
        const auto text = parse_chapter(R"HTML(
            <style>.invisible { display: none; }</style><h1 class="chapter-title">Chapter 1</h1>
            <div id="content"><p>First <em>paragraph</em>.</p>
            <div class="invisible"><p>Hidden text</p></div>
            <div class="ads"><div class="ads">Nested advertisement</div></div>
            <nfnoise><nfnoise>Injected text</nfnoise></nfnoise>
            <p>Second&nbsp;paragraph.</p><script>unreadable()</script></div>
        )HTML", nf);
        check(text.title == "Chapter 1" && text.paragraphs.size() == 2, "NovelFire chapter cleanup");
        check(text.paragraphs[0].find("First") == 0 && text.paragraphs[1] == "Second paragraph.", "chapter text retained");
        check(parse_chapter("<p>Challenge</p>", nf).paragraphs.empty(), "missing chapter container returns no prose");
        check(parse_chapter("<h1>RR</h1><div class='chapter-content'><p>Story.</p></div>", rr).paragraphs.size() == 1, "Royal Road parser regression");
        const auto paid = parse_chapter(R"(<script>window.__REACT_QUERY_STATE__ = {"queries":[{"queryKey":["chapter"],"state":{"data":{"item":{"name":"Paid chapter","pricingInfo":{"isFree":false}}}}}]};</script><div class="chapter-content"><p>Preview</p></div>)", ww);
        check(paid.locked && paid.title == "Paid chapter", "Wuxiaworld JSON and lock regression");

        const auto settings = Settings::from_json({{"site_logins", {{"novelfire", "Cookie: session=example"}}}, {"site_delays", {{"novelfire", 2.5}}}});
        check(settings.site_logins.at("novelfire") == "session=example", "cookie normalization");
        check(settings.to_json(true)["site_logins"]["novelfire"] == "***", "cookies are masked");
        check(Settings::from_json(settings.to_json(false)).site_delays.at("novelfire") == 2.5, "delay persistence");
        rejects([] { Settings::from_json({{"site_delays", {{"novelfire", -1}}}}); }, "negative delay rejected");
        rejects([] { Settings::from_json({{"site_delays", {{"novelfire", 61}}}}); }, "excessive delay rejected");
        rejects([] { Settings::from_json({{"site_delays", {{"novelfire", "oops"}}}}); }, "invalid delay type rejected");
        rejects([] { Settings::from_json({{"site_logins", {{"novelfire", "a=b\r\nInjected: header"}}}}); }, "cookie header injection rejected");

        // Real fetching + library identity migration against a loopback fixture server.
        httplib::Server server;
        std::atomic<bool> saw_cookie{false};
        server.Get(R"(/book/.*)", [&](const httplib::Request & req, httplib::Response & res) {
            saw_cookie = req.get_header_value("Cookie") == "session=example";
            res.set_content(novel, "text/html");
        });
        const int port = server.bind_to_any_port("127.0.0.1");
        check(port > 0, "bind fixture server");
        std::thread listener([&] { server.listen_after_bind(); });
        const auto dir = fs::temp_directory_path() / ("rm-source-tests-" + std::to_string(port));
        try {
            fs::create_directories(dir);
            auto local = nf;
            local.novel["url"] = "http://127.0.0.1:" + std::to_string(port) + "/book/{slug}";
            local.match.emplace_back(std::regex("^http://127\\.0\\.0\\.1:[0-9]+/book/([^/?#]+)"), std::vector<std::string>{"", "slug"});
            local.fetch["delay_seconds"] = 0;
            Sources local_sources{{local.id, local}};
            set_site_config(settings.site_logins, {{"novelfire", 0}});
            DB db(dir / "test.sqlite");
            Library library(db, local_sources);
            const auto first = library.add(base + "/chapter-1", "", "local");
            check(saw_cookie, "plugin cookie applied to fetch");
            db.run("UPDATE novels SET source_key='novelfire:' WHERE id=?", {first["id"]});
            const auto again = library.add(base, "", "local");
            check(first["id"] == again["id"], "legacy novel identity migration preserves row");
            check(library.chapters(first["id"].get<int64_t>()).size() == 1234, "refresh deduplicates chapters");
            const auto other = library.add("https://novelfire.net/book/other", "", "local");
            check(other["id"] != first["id"], "library stores different slug novels separately");
        } catch (...) { server.stop(); listener.join(); throw; }
        server.stop(); listener.join();
        for (const auto * file : {"test.sqlite", "test.sqlite-wal", "test.sqlite-shm"}) fs::remove(dir / file);
        fs::remove(dir);
        std::cout << "Source plugin regression tests passed\n";
        return 0;
    } catch (const std::exception & e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
