#include "scrape.h"

#include "net.h"
#include "text.h"

#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>

#include "tokenizer.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace rm {

static const char * kUA = "Mozilla/5.0 (X11; Linux x86_64) readmyself/0.1 (+self-hosted audiobook reader)";

static std::mutex cookies_mu;
static std::map<std::string, std::string> cookies;   // source id -> Cookie header (the user's own login on that site)

void set_site_cookies(const std::map<std::string, std::string> & by_source) {
    std::lock_guard<std::mutex> lk(cookies_mu);
    cookies = by_source;
}

std::string fetch(const std::string & url, const Source & src) {
    static std::mutex mu;
    static std::unordered_map<std::string, double> last;
    const std::string host = parse_url(url).host;
    const double delay = src.fetch.value("delay_seconds", 1.0);
    {
        std::unique_lock<std::mutex> lk(mu);
        const double t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const double wait = last[host] + delay - t;
        if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        last[host] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::map<std::string, std::string> headers{{"User-Agent", kUA}};
    {
        std::lock_guard<std::mutex> lk(cookies_mu);
        auto it = cookies.find(src.id);
        if (it != cookies.end() && !it->second.empty()) headers["Cookie"] = it->second;
    }
    const auto r = http_get(url, headers, 60);
    if (!r.error.empty()) throw std::runtime_error("fetch " + url + ": " + r.error);
    if (r.status < 200 || r.status >= 300) throw std::runtime_error("fetch " + url + ": HTTP " + std::to_string(r.status));
    return r.body;
}

namespace {

std::string lower(std::string s) {
    for (auto & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class Html {
public:
    explicit Html(const std::string & html) {
        doc_ = lxb_html_document_create();
        if (!doc_ || lxb_html_document_parse(doc_, reinterpret_cast<const lxb_char_t *>(html.data()), html.size()) != LXB_STATUS_OK)
            throw std::runtime_error("HTML parse failed");
        parser_ = lxb_css_parser_create();
        lxb_css_parser_init(parser_, nullptr);
        sel_ = lxb_selectors_create();
        lxb_selectors_init(sel_);
        lxb_selectors_opt_set(sel_, LXB_SELECTORS_OPT_MATCH_FIRST);
    }
    ~Html() {
        lxb_selectors_destroy(sel_, true);
        if (parser_->memory) lxb_css_memory_destroy(parser_->memory, true);
        lxb_css_parser_destroy(parser_, true);
        lxb_html_document_destroy(doc_);
    }
    lxb_dom_node_t * root() const { return lxb_dom_interface_node(doc_); }
    std::string text_of_root() const { return text(root()); }
    lxb_dom_document_t * dom() const { return lxb_dom_interface_document(doc_); }

    // BeautifulSoup select(): matching descendants of `node`, document order, each once
    std::vector<lxb_dom_node_t *> select(lxb_dom_node_t * node, const std::string & css) {
        std::vector<lxb_dom_node_t *> out;
        lxb_css_selector_list_t * list =
            lxb_css_selectors_parse(parser_, reinterpret_cast<const lxb_char_t *>(css.data()), css.size());
        if (!list) throw std::runtime_error("bad CSS selector: " + css);
        lxb_selectors_find(sel_, node, list,
                           [](lxb_dom_node_t * n, lxb_css_selector_specificity_t, void * ctx) -> lxb_status_t {
                               static_cast<std::vector<lxb_dom_node_t *> *>(ctx)->push_back(n);
                               return LXB_STATUS_OK;
                           },
                           &out);
        // lists live in the parser's memory pool, freed with the parser
        // selectors_find walks the tree in order; keep only the first report of each node
        std::set<lxb_dom_node_t *> seen;
        std::vector<lxb_dom_node_t *> uniq;
        for (auto * n : out)
            if (seen.insert(n).second) uniq.push_back(n);
        return uniq;
    }
    lxb_dom_node_t * select_one(lxb_dom_node_t * node, const std::string & css) {
        auto v = select(node, css);
        return v.empty() ? nullptr : v.front();
    }

    static std::string name(lxb_dom_node_t * n) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return "";
        size_t len = 0;
        const lxb_char_t * s = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
        return lower(std::string(reinterpret_cast<const char *>(s), len));
    }
    static std::string attr(lxb_dom_node_t * n, const std::string & a) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return "";
        size_t len = 0;
        const lxb_char_t * v = lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                                             reinterpret_cast<const lxb_char_t *>(a.data()), a.size(), &len);
        return v ? std::string(reinterpret_cast<const char *>(v), len) : "";
    }
    static void strings(lxb_dom_node_t * n, std::vector<std::string> & out) {   // text nodes, no script/style
        for (lxb_dom_node_t * c = n->first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_TEXT) {
                const auto * t = lxb_dom_interface_character_data(c);
                out.emplace_back(reinterpret_cast<const char *>(t->data.data), t->data.length);
            } else if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                const std::string nm = name(c);
                if (nm != "script" && nm != "style" && nm != "template") strings(c, out);
            }
        }
    }
    // get_text(" ", strip=True)
    static std::string text(lxb_dom_node_t * n) {
        std::vector<std::string> parts;
        strings(n, parts);
        std::string out;
        for (const auto & p : parts) {
            const std::string s = strip(p);
            if (!s.empty()) out += (out.empty() ? "" : " ") + s;
        }
        return out;
    }
    static void elements(lxb_dom_node_t * n, std::vector<lxb_dom_node_t *> & out) {   // descendants, document order
        for (lxb_dom_node_t * c = n->first_child; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                out.push_back(c);
                elements(c, out);
            }
    }

private:
    lxb_html_document_t * doc_ = nullptr;
    lxb_css_parser_t * parser_ = nullptr;
    lxb_selectors_t * sel_ = nullptr;
};

// The JSON a page assigns in a script (`window.__STATE__ = {...}`): the first object/array after `marker`.
json page_data(const std::string & html, const json & marker_j) {
    if (!marker_j.is_string()) return nullptr;
    const std::string marker = marker_j.get<std::string>();
    size_t p = html.find(marker);
    if (p == std::string::npos) return nullptr;
    p = html.find_first_of("{[", p + marker.size());
    if (p == std::string::npos) return nullptr;
    int depth = 0;
    bool str = false, esc = false;
    for (size_t i = p; i < html.size(); ++i) {   // matching close bracket, skipping strings
        const char c = html[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
        } else if (c == '"') str = true;
        else if (c == '{' || c == '[') ++depth;
        else if ((c == '}' || c == ']') && --depth == 0) {
            const json j = json::parse(html.begin() + static_cast<std::ptrdiff_t>(p), html.begin() + static_cast<std::ptrdiff_t>(i + 1), nullptr, false);
            return j.is_discarded() ? json(nullptr) : j;
        }
    }
    return nullptr;
}

// "$.a.b.0.c" walks objects/arrays; "list[key.path=value]" is the first element of `list` whose key.path equals value;
// "|lower" / "|not" at the end transform the result
json json_path(const json & data, std::string path) {
    std::vector<std::string> mods;
    for (size_t bar; (bar = path.rfind('|')) != std::string::npos && path.find(']', bar) == std::string::npos;) {
        mods.insert(mods.begin(), path.substr(bar + 1));
        path.erase(bar);
    }
    std::function<json(const json &, const std::string &)> walk = [&](const json & cur0, const std::string & p) -> json {
        json cur = cur0;
        size_t i = 0;
        while (i < p.size()) {
            if (p[i] == '.') { ++i; continue; }
            if (p[i] == '[') {
                const size_t close = p.find(']', i);
                const std::string f = p.substr(i + 1, close - i - 1);
                const size_t eq = f.find('=');
                json hit = nullptr;
                if (cur.is_array())
                    for (const auto & e : cur) {
                        const json v = walk(e, f.substr(0, eq));
                        if ((v.is_string() ? v.get<std::string>() : v.dump()) == f.substr(eq + 1)) { hit = e; break; }
                    }
                cur = hit;
                i = close + 1;
                continue;
            }
            size_t j = i;
            while (j < p.size() && p[j] != '.' && p[j] != '[') ++j;
            const std::string key = p.substr(i, j - i);
            i = j;
            if (cur.is_object() && cur.contains(key)) cur = cur[key];
            else if (cur.is_array() && !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit) && std::stoul(key) < cur.size())
                cur = cur[std::stoul(key)];
            else return nullptr;
        }
        return cur;
    };
    json v = walk(data, path.rfind("$", 0) == 0 ? path.substr(1) : path);
    for (const auto & m : mods) {
        if (m == "lower" && v.is_string()) v = lower(v.get<std::string>());
        else if (m == "not") v = !(v.is_boolean() ? v.get<bool>() : !v.is_null());
    }
    return v;
}

std::string json_text(const json & v) {
    if (v.is_null()) return "";
    return v.is_string() ? v.get<std::string>() : v.dump();
}

// 'css' -> text, 'css@attr' -> attribute, '@attr' -> attribute of the node itself, '$.path' -> from the page's JSON
std::string select_spec(Html & h, lxb_dom_node_t * node, const json & spec_j, const std::string & base_url = "",
                        const json & data = nullptr) {
    if (!spec_j.is_string()) return "";
    const std::string spec = spec_j.get<std::string>();
    if (spec.empty()) return "";
    if (spec[0] == '$') {
        const std::string v = json_text(json_path(data, spec));
        return !base_url.empty() && v.rfind("/", 0) == 0 ? url_join(base_url, v) : v;
    }
    const size_t at = spec.find('@');
    const std::string css = spec.substr(0, at);
    const std::string a = at == std::string::npos ? "" : spec.substr(at + 1);
    lxb_dom_node_t * el = css.empty() ? node : h.select_one(node, css);
    if (!el) return "";
    if (!a.empty()) {
        const std::string v = Html::attr(el, a);
        if ((a == "href" || a == "src" || a == "data-url") && !base_url.empty()) return url_join(base_url, v);
        return v;
    }
    return Html::text(el);
}

std::string collapse_ws(const std::string & s) {   // re.sub(r"\s+", " ", s)
    const auto u = booknlp::utf8_to_u32(s);
    std::u32string out;
    bool ws = false;
    for (char32_t c : u) {
        if (booknlp::py_isspace(c)) {
            if (!ws) out += U' ';
            ws = true;
        } else {
            out += c;
            ws = false;
        }
    }
    return booknlp::u32_to_utf8(out);
}

}  // namespace

NovelInfo parse_novel(const std::string & html, const std::string & url, const Source & src) {
    Html h(html);
    const json & n = src.novel;
    const json data = page_data(html, n.value("data", json(nullptr)));
    NovelInfo info;
    info.url = url;
    const json & spec = n.at("chapters");
    if (spec.contains("count")) {   // no list on the page: chapters 1..count from a URL pattern
        std::map<std::string, std::string> vars;
        if (auto m = src.match_url(url)) vars = *m;
        vars["novel_url"] = url;
        const json var_specs = spec.value("vars", json::object());
        for (const auto & [k, v] : var_specs.items()) vars[k] = select_spec(h, h.root(), v, "", data);
        auto fill = [&](std::string t, int i) {
            vars["n"] = std::to_string(i);
            for (const auto & [k, v] : vars) t = std::regex_replace(t, std::regex("\\{" + k + "\\}"), v);
            return t;
        };
        const std::string count = select_spec(h, h.root(), spec.at("count"), "", data);
        const int total = count.empty() ? 0 : std::stoi(count);
        for (int i = 1; i <= total; ++i) {
            ChapterRef c;
            c.index = i;
            c.url = fill(spec.at("url").get<std::string>(), i);
            c.title = fill(spec.value("title", std::string("Chapter {n}")), i);
            info.chapters.push_back(std::move(c));
        }
    } else
    for (auto * item : h.select(h.root(), spec.at("item").get<std::string>())) {
        const std::string curl = select_spec(h, item, spec.at("url"), url);
        if (curl.empty()) continue;
        const std::string pub = select_spec(h, item, spec.value("published", json(nullptr)));
        ChapterRef c;
        c.index = static_cast<int>(info.chapters.size()) + 1;
        c.url = curl;
        c.title = select_spec(h, item, spec.value("title", json(nullptr)));
        if (c.title.empty()) c.title = "Chapter " + std::to_string(c.index);
        if (!pub.empty() && std::all_of(pub.begin(), pub.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
            c.published = std::stoll(pub);
        info.chapters.push_back(std::move(c));
    }
    info.title = select_spec(h, h.root(), n.value("title", json(nullptr)), "", data);
    info.author = select_spec(h, h.root(), n.value("author", json(nullptr)), "", data);
    info.cover = select_spec(h, h.root(), n.value("cover", json(nullptr)), url, data);
    info.description = select_spec(h, h.root(), n.value("description", json(nullptr)), "", data);
    if (info.description.find('<') != std::string::npos) info.description = collapse_ws(Html(info.description).text_of_root());
    return info;
}

ChapterText parse_chapter(const std::string & html, const Source & src) {
    Html h(html);
    const json & c = src.chapter;
    const json data = page_data(html, c.value("data", json(nullptr)));
    ChapterText out;
    out.title = select_spec(h, h.root(), c.value("title", json(nullptr)), "", data);
    out.locked = select_spec(h, h.root(), c.value("locked", json(nullptr)), "", data) == "true";
    lxb_dom_node_t * root = h.select_one(h.root(), c.at("content").get<std::string>());
    if (!root) return out;
    // classes declared display:none in <style> blocks (anti-scraping paragraphs)
    std::set<std::string> hidden;
    if (c.value("drop_css_hidden", false)) {
        static const std::regex rule(R"(([^{}]+)\{([^}]*)\})"), none(R"(display\s*:\s*none)"), cls(R"(\.([A-Za-z0-9_-]+))");
        for (auto * st : h.select(h.root(), "style")) {
            std::vector<std::string> parts;
            Html::strings(st, parts);
            std::string css;
            for (const auto & p : parts) css += p;
            // <style> content is raw text (the element itself is skipped by strings())
            if (css.empty()) {
                size_t len = 0;
                const lxb_char_t * t = lxb_dom_node_text_content(st, &len);
                if (t) css.assign(reinterpret_cast<const char *>(t), len);
            }
            for (std::sregex_iterator it(css.begin(), css.end(), rule), e; it != e; ++it) {
                const std::string sel = (*it)[1], body = (*it)[2];
                if (!std::regex_search(body, none)) continue;
                for (std::sregex_iterator k(sel.begin(), sel.end(), cls); k != std::sregex_iterator(); ++k)
                    hidden.insert((*k)[1]);
            }
        }
    }
    auto destroy = [](lxb_dom_node_t * n) { lxb_dom_node_destroy_deep(n); };
    if (!hidden.empty()) {
        std::vector<lxb_dom_node_t *> els;
        Html::elements(root, els);
        std::set<lxb_dom_node_t *> gone;
        for (auto * el : els) {
            bool inside = false;   // already removed with an ancestor
            for (lxb_dom_node_t * p = el->parent; p && p != root; p = p->parent) inside = inside || gone.count(p);
            if (inside) continue;
            const std::string classes = Html::attr(el, "class");
            std::istringstream ss(classes);
            bool hit = false;
            for (std::string k; ss >> k;) hit = hit || hidden.count(k);
            if (hit) gone.insert(el);
        }
        for (auto * el : gone) {
            bool nested = false;
            for (lxb_dom_node_t * p = el->parent; p && p != root; p = p->parent) nested = nested || gone.count(p);
            if (!nested) destroy(el);
        }
    }
    if (c.contains("drop"))
        for (const auto & sel : c["drop"]) {
            auto nodes = h.select(root, sel.get<std::string>());
            std::set<lxb_dom_node_t *> set(nodes.begin(), nodes.end());
            for (auto * el : nodes) {
                bool nested = false;
                for (lxb_dom_node_t * p = el->parent; p && p != root; p = p->parent) nested = nested || set.count(p);
                if (!nested) destroy(el);
            }
        }
    for (auto * br : h.select(root, "br")) {
        lxb_dom_text_t * sp = lxb_dom_document_create_text_node(h.dom(), reinterpret_cast<const lxb_char_t *>(" "), 1);
        lxb_dom_node_insert_before(br, lxb_dom_interface_node(sp));
        destroy(br);
    }
    static const std::set<std::string> blocks = {"p", "div", "li", "h1", "h2", "h3", "h4", "blockquote", "td", "th"};
    std::vector<lxb_dom_node_t *> els;
    Html::elements(root, els);
    for (auto * el : els) {
        if (!blocks.count(Html::name(el))) continue;
        std::vector<lxb_dom_node_t *> inner;
        Html::elements(el, inner);
        if (std::any_of(inner.begin(), inner.end(), [](lxb_dom_node_t * x) { return blocks.count(Html::name(x)) > 0; }))
            continue;
        const std::string t = collapse_ws(Html::text(el));
        if (!t.empty()) out.paragraphs.push_back(t);
    }
    if (out.paragraphs.empty()) {
        std::vector<std::string> parts;
        Html::strings(root, parts);
        std::string all;
        for (size_t i = 0; i < parts.size(); ++i) all += (i ? "\n" : "") + parts[i];
        std::istringstream ss(all);
        for (std::string line; std::getline(ss, line);) {
            const std::string s = strip(line);
            if (!s.empty()) out.paragraphs.push_back(s);
        }
    }
    return out;
}

}  // namespace rm
