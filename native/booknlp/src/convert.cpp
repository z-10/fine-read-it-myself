#include "convert.h"

#include "ggml.h"
#include "gguf.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace rm::booknlp {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string read_text(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("missing " + p.u8string());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str(), out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r') out += c;   // Python read_text: universal newlines
    return out;
}

// ---------------------------------------------------------------- zip (stored entries only)

class Zip {
public:
    explicit Zip(const fs::path & p) : f_(p, std::ios::binary), path_(p) {
        if (!f_) throw std::runtime_error("cannot open " + p.u8string());
        f_.seekg(0, std::ios::end);
        const int64_t size = f_.tellg();
        const int64_t tail = std::min<int64_t>(size, 65536 + 22);
        std::vector<unsigned char> buf(static_cast<size_t>(tail));
        f_.seekg(size - tail);
        f_.read(reinterpret_cast<char *>(buf.data()), tail);
        int64_t eocd = -1;
        for (int64_t i = tail - 22; i >= 0; --i)
            if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
                eocd = i;
                break;
            }
        if (eocd < 0) throw std::runtime_error("not a zip file: " + p.u8string());
        uint64_t n = u16(&buf[eocd + 10]), cd_size = u32(&buf[eocd + 12]), cd_off = u32(&buf[eocd + 16]);
        if (cd_off == 0xFFFFFFFF || n == 0xFFFF) {   // zip64: locator just before the EOCD record
            const int64_t loc = eocd - 20;
            if (loc < 0 || u32(&buf[loc]) != 0x07064b50) throw std::runtime_error("bad zip64 locator");
            uint64_t rec = u64(&buf[loc + 8]);
            unsigned char r[56];
            f_.seekg(static_cast<std::streamoff>(rec));
            f_.read(reinterpret_cast<char *>(r), 56);
            n = u64(&r[32]);
            cd_size = u64(&r[40]);
            cd_off = u64(&r[48]);
        }
        std::vector<unsigned char> cd(static_cast<size_t>(cd_size));
        f_.seekg(static_cast<std::streamoff>(cd_off));
        f_.read(reinterpret_cast<char *>(cd.data()), static_cast<std::streamsize>(cd_size));
        size_t p0 = 0;
        for (uint64_t k = 0; k < n; ++k) {
            const unsigned char * e = &cd[p0];
            if (u32(e) != 0x02014b50) throw std::runtime_error("bad zip central directory");
            const uint16_t method = u16(e + 10), nlen = u16(e + 28), xlen = u16(e + 30), clen = u16(e + 32);
            uint64_t csize = u32(e + 20), usize = u32(e + 24), off = u32(e + 42);
            const std::string name(reinterpret_cast<const char *>(e + 46), nlen);
            const unsigned char * x = e + 46 + nlen;
            for (size_t q = 0; q + 4 <= xlen;) {   // zip64 extended information
                const uint16_t id = u16(x + q), sz = u16(x + q + 2);
                if (id == 1) {
                    size_t r = q + 4;
                    if (usize == 0xFFFFFFFF) { usize = u64(x + r); r += 8; }
                    if (csize == 0xFFFFFFFF) { csize = u64(x + r); r += 8; }
                    if (off == 0xFFFFFFFF) { off = u64(x + r); }
                }
                q += 4 + sz;
            }
            entries_[name] = {method, csize, off};
            p0 += 46 + nlen + xlen + clen;
        }
    }
    // name suffix match ("data.pkl" matches "archive/data.pkl")
    std::string find(const std::string & suffix) const {
        for (const auto & [k, _] : entries_)
            if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                (k.size() == suffix.size() || k[k.size() - suffix.size() - 1] == '/'))
                return k;
        throw std::runtime_error("zip entry not found: " + suffix + " in " + path_.u8string());
    }
    std::vector<char> read(const std::string & name, uint64_t offset = 0, uint64_t count = UINT64_MAX) {
        const auto it = entries_.find(name);
        if (it == entries_.end()) throw std::runtime_error("zip entry not found: " + name);
        if (it->second.method != 0) throw std::runtime_error("compressed zip entry not supported: " + name);
        unsigned char lh[30];
        f_.seekg(static_cast<std::streamoff>(it->second.offset));
        f_.read(reinterpret_cast<char *>(lh), 30);
        const uint64_t data = it->second.offset + 30 + u16(lh + 26) + u16(lh + 28);
        count = std::min(count, it->second.size - offset);
        std::vector<char> out(static_cast<size_t>(count));
        f_.seekg(static_cast<std::streamoff>(data + offset));
        f_.read(out.data(), static_cast<std::streamsize>(count));
        if (!f_) throw std::runtime_error("short read in " + path_.u8string());
        return out;
    }

private:
    static uint16_t u16(const unsigned char * p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
    static uint32_t u32(const unsigned char * p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
    static uint64_t u64(const unsigned char * p) { return u32(p) | static_cast<uint64_t>(u32(p + 4)) << 32; }
    struct Entry {
        uint16_t method;
        uint64_t size, offset;
    };
    std::ifstream f_;
    fs::path path_;
    std::map<std::string, Entry> entries_;
};

// ---------------------------------------------------------------- pickle (what torch.save writes for a state_dict)

struct PV;
using PVP = std::shared_ptr<PV>;
struct PV {
    enum Kind { None, Bool, Int, Float, Str, Tuple, List, Dict, Global, Storage, Tensor, Mark, Opaque } kind = None;
    int64_t i = 0;
    double f = 0;
    std::string s, s2;                       // Str; Global module/name; Storage key/dtype
    std::vector<PVP> items;                  // Tuple / List
    std::vector<std::pair<PVP, PVP>> dict;   // Dict (insertion order)
    // Tensor
    PVP storage;
    int64_t offset = 0;
    std::vector<int64_t> size, stride;
};

PVP make(PV::Kind k) {
    auto p = std::make_shared<PV>();
    p->kind = k;
    return p;
}

PVP unpickle(const std::vector<char> & data) {
    std::vector<PVP> stack;
    std::vector<size_t> marks;
    std::unordered_map<int64_t, PVP> memo;
    size_t p = 0;
    auto need = [&](size_t n) {
        if (p + n > data.size()) throw std::runtime_error("truncated pickle");
    };
    auto rd = [&](size_t n) {
        need(n);
        const char * s = &data[p];
        p += n;
        return s;
    };
    auto le = [&](size_t n) {
        const unsigned char * b = reinterpret_cast<const unsigned char *>(rd(n));
        uint64_t v = 0;
        for (size_t k = 0; k < n; ++k) v |= static_cast<uint64_t>(b[k]) << (8 * k);
        return v;
    };
    auto line = [&] {
        std::string s;
        while (true) {
            const char c = *rd(1);
            if (c == '\n') return s;
            s += c;
        }
    };
    auto pop = [&] {
        if (stack.empty()) throw std::runtime_error("pickle stack underflow");
        PVP v = stack.back();
        stack.pop_back();
        return v;
    };
    auto pop_mark = [&] {
        if (marks.empty()) throw std::runtime_error("pickle mark missing");
        const size_t m = marks.back();
        marks.pop_back();
        std::vector<PVP> items(stack.begin() + m, stack.end());
        stack.resize(m);
        return items;
    };
    auto str = [&](const std::string & s) {
        PVP v = make(PV::Str);
        v->s = s;
        return v;
    };
    auto tuple = [&](std::vector<PVP> items) {
        PVP v = make(PV::Tuple);
        v->items = std::move(items);
        return v;
    };
    while (true) {
        const unsigned char op = static_cast<unsigned char>(*rd(1));
        switch (op) {
            case 0x80: rd(1); break;                               // PROTO
            case 0x95: rd(8); break;                               // FRAME
            case '.': return pop();                                // STOP
            case '(': marks.push_back(stack.size()); break;        // MARK
            case 'N': stack.push_back(make(PV::None)); break;
            case 0x88: case 0x89: {                                // NEWTRUE / NEWFALSE
                PVP v = make(PV::Bool);
                v->i = op == 0x88;
                stack.push_back(v);
                break;
            }
            case 'J': { PVP v = make(PV::Int); v->i = static_cast<int32_t>(le(4)); stack.push_back(v); break; }
            case 'K': { PVP v = make(PV::Int); v->i = static_cast<int64_t>(le(1)); stack.push_back(v); break; }
            case 'M': { PVP v = make(PV::Int); v->i = static_cast<int64_t>(le(2)); stack.push_back(v); break; }
            case 0x8a: {                                           // LONG1
                const size_t n = static_cast<size_t>(le(1));
                uint64_t v = n ? le(n) : 0;
                if (n && n < 8 && (v >> (8 * n - 1)) & 1) v |= ~0ull << (8 * n);   // sign-extend
                PVP x = make(PV::Int);
                x->i = static_cast<int64_t>(v);
                stack.push_back(x);
                break;
            }
            case 'G': {                                            // BINFLOAT (big endian)
                const unsigned char * b = reinterpret_cast<const unsigned char *>(rd(8));
                uint64_t v = 0;
                for (int k = 0; k < 8; ++k) v = v << 8 | b[k];
                PVP x = make(PV::Float);
                std::memcpy(&x->f, &v, 8);
                stack.push_back(x);
                break;
            }
            case 'X': { const size_t n = static_cast<size_t>(le(4)); stack.push_back(str(std::string(rd(n), n))); break; }
            case 0x8c: { const size_t n = static_cast<size_t>(le(1)); stack.push_back(str(std::string(rd(n), n))); break; }
            case ')': stack.push_back(tuple({})); break;
            case 't': stack.push_back(tuple(pop_mark())); break;
            case 0x85: { auto a = pop(); stack.push_back(tuple({a})); break; }
            case 0x86: { auto b = pop(), a = pop(); stack.push_back(tuple({a, b})); break; }
            case 0x87: { auto c = pop(), b = pop(), a = pop(); stack.push_back(tuple({a, b, c})); break; }
            case '}': stack.push_back(make(PV::Dict)); break;
            case ']': stack.push_back(make(PV::List)); break;
            case 'a': { auto v = pop(); stack.back()->items.push_back(v); break; }
            case 'e': { auto items = pop_mark(); for (auto & v : items) stack.back()->items.push_back(v); break; }
            case 's': { auto v = pop(), k = pop(); stack.back()->dict.emplace_back(k, v); break; }
            case 'u': {
                auto items = pop_mark();
                for (size_t k = 0; k + 1 < items.size(); k += 2) stack.back()->dict.emplace_back(items[k], items[k + 1]);
                break;
            }
            case 'q': memo[static_cast<int64_t>(le(1))] = stack.back(); break;   // BINPUT
            case 'r': memo[static_cast<int64_t>(le(4))] = stack.back(); break;   // LONG_BINPUT
            case 0x94: memo[static_cast<int64_t>(memo.size())] = stack.back(); break;   // MEMOIZE
            case 'h': stack.push_back(memo.at(static_cast<int64_t>(le(1)))); break;
            case 'j': stack.push_back(memo.at(static_cast<int64_t>(le(4)))); break;
            case 'c': {                                            // GLOBAL
                PVP g = make(PV::Global);
                g->s = line();
                g->s2 = line();
                stack.push_back(g);
                break;
            }
            case 0x93: {                                           // STACK_GLOBAL
                auto name = pop(), mod = pop();
                PVP g = make(PV::Global);
                g->s = mod->s;
                g->s2 = name->s;
                stack.push_back(g);
                break;
            }
            case 'Q': {                                            // BINPERSID: ('storage', cls, key, location, numel)
                auto pid = pop();
                if (pid->kind != PV::Tuple || pid->items.size() < 3) throw std::runtime_error("unexpected persistent id");
                PVP st = make(PV::Storage);
                st->s = pid->items[2]->s;         // key -> data/<key>
                st->s2 = pid->items[1]->s2;       // FloatStorage / LongStorage / ...
                stack.push_back(st);
                break;
            }
            case 'R': {                                            // REDUCE
                auto args = pop(), fn = pop();
                if (fn->kind == PV::Global && fn->s2 == "OrderedDict") {
                    stack.push_back(make(PV::Dict));
                } else if (fn->kind == PV::Global && fn->s2 == "_rebuild_tensor_v2") {
                    PVP t = make(PV::Tensor);
                    t->storage = args->items.at(0);
                    t->offset = args->items.at(1)->i;
                    for (auto & v : args->items.at(2)->items) t->size.push_back(v->i);
                    for (auto & v : args->items.at(3)->items) t->stride.push_back(v->i);
                    stack.push_back(t);
                } else {
                    stack.push_back(make(PV::Opaque));
                }
                break;
            }
            case 'b': pop(); break;                                // BUILD: state ignored (_metadata)
            default: {
                char msg[64];
                std::snprintf(msg, sizeof msg, "unsupported pickle opcode 0x%02x", op);
                throw std::runtime_error(msg);
            }
        }
    }
}

// ---------------------------------------------------------------- tensor sources

struct Tensor {
    std::vector<int64_t> shape;   // PyTorch order (outer first)
    std::function<std::vector<float>()> data;
};
using Tensors = std::vector<std::pair<std::string, Tensor>>;   // insertion order

Tensors torch_state_dict(const fs::path & path) {
    auto zip = std::make_shared<Zip>(path);
    const PVP root = unpickle(zip->read(zip->find("data.pkl")));
    if (root->kind != PV::Dict) throw std::runtime_error("checkpoint is not a state_dict: " + path.u8string());
    const std::string prefix = zip->find("data.pkl").substr(0, zip->find("data.pkl").size() - 8);   // "archive/"
    Tensors out;
    for (const auto & [k, v] : root->dict) {
        if (v->kind != PV::Tensor) continue;
        Tensor t;
        t.shape = v->size;
        const std::string dtype = v->storage->s2, key = v->storage->s;
        const int64_t offset = v->offset;
        const auto shape = v->size, stride = v->stride;
        t.data = [zip, prefix, key, dtype, offset, shape, stride]() {
            if (dtype != "FloatStorage") throw std::runtime_error("unsupported tensor storage " + dtype);
            int64_t n = 1;
            for (int64_t d : shape) n *= d;
            // row-major contiguous check; otherwise gather
            bool contiguous = true;
            int64_t expect = 1;
            for (size_t d = shape.size(); d-- > 0;) {
                if (shape[d] != 1 && stride[d] != expect) contiguous = false;
                expect *= shape[d];
            }
            std::vector<float> out(static_cast<size_t>(n));
            if (contiguous) {
                const auto raw = zip->read(prefix + "data/" + key, static_cast<uint64_t>(offset) * 4, static_cast<uint64_t>(n) * 4);
                std::memcpy(out.data(), raw.data(), raw.size());
            } else {
                const auto raw = zip->read(prefix + "data/" + key);
                const float * src = reinterpret_cast<const float *>(raw.data());
                std::vector<int64_t> idx(shape.size(), 0);
                for (int64_t k = 0; k < n; ++k) {
                    int64_t o = offset;
                    for (size_t d = 0; d < shape.size(); ++d) o += idx[d] * stride[d];
                    out[static_cast<size_t>(k)] = src[o];
                    for (size_t d = shape.size(); d-- > 0;) {
                        if (++idx[d] < shape[d]) break;
                        idx[d] = 0;
                    }
                }
            }
            return out;
        };
        out.emplace_back(k->s, std::move(t));
    }
    return out;
}

Tensors safetensors(const fs::path & path) {
    auto f = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*f) throw std::runtime_error("missing " + path.u8string());
    uint64_t hlen = 0;
    f->read(reinterpret_cast<char *>(&hlen), 8);
    std::string header(static_cast<size_t>(hlen), '\0');
    f->read(header.data(), static_cast<std::streamsize>(hlen));
    const nlohmann::ordered_json h = nlohmann::ordered_json::parse(header);
    const uint64_t base = 8 + hlen;
    Tensors out;
    for (auto it = h.begin(); it != h.end(); ++it) {
        if (it.key() == "__metadata__") continue;
        const auto & d = it.value();
        Tensor t;
        t.shape = d["shape"].get<std::vector<int64_t>>();
        const std::string dtype = d["dtype"].get<std::string>();
        const uint64_t a = d["data_offsets"][0].get<uint64_t>(), b = d["data_offsets"][1].get<uint64_t>();
        const std::string name = it.key();
        t.data = [f, base, a, b, dtype, name]() {
            if (dtype != "F32") throw std::runtime_error("unsupported safetensors dtype " + dtype + " for " + name);
            std::vector<float> out(static_cast<size_t>((b - a) / 4));
            f->seekg(static_cast<std::streamoff>(base + a));
            f->read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(b - a));
            return out;
        };
        out.emplace_back(name, std::move(t));
    }
    return out;
}

Tensor * find(Tensors & ts, const std::string & name) {
    for (auto & [n, t] : ts)
        if (n == name) return &t;
    return nullptr;
}

void erase(Tensors & ts, const std::string & name) {
    for (auto it = ts.begin(); it != ts.end(); ++it)
        if (it->first == name) {
            ts.erase(it);
            return;
        }
}

// columns [c0, c1) of a 2-D tensor
Tensor columns(const Tensor & t, int64_t c0, int64_t c1) {
    Tensor out;
    out.shape = {t.shape[0], c1 - c0};
    auto src = t.data;
    const int64_t rows = t.shape[0], cols = t.shape[1];
    out.data = [src, rows, cols, c0, c1]() {
        const auto full = src();
        std::vector<float> o(static_cast<size_t>(rows * (c1 - c0)));
        for (int64_t r = 0; r < rows; ++r)
            std::memcpy(&o[static_cast<size_t>(r * (c1 - c0))], &full[static_cast<size_t>(r * cols + c0)], static_cast<size_t>(c1 - c0) * 4);
        return o;
    };
    return out;
}

// ---------------------------------------------------------------- GGUF writer (metadata first, tensors streamed)

bool skipped(const std::string & name) {
    for (const char * s : {"position_ids", "pooler.", "supersense", "wn_embedding", "num_batches_tracked"})
        if (name.find(s) != std::string::npos) return true;
    return false;
}

class Writer {
public:
    explicit Writer(const std::string & arch) : ctx_(gguf_init_empty()) { gguf_set_val_str(ctx_, "general.architecture", arch.c_str()); }
    ~Writer() { gguf_free(ctx_); }
    gguf_context * ctx() { return ctx_; }
    void set_strings(const std::string & key, const std::vector<std::string> & v) {
        std::vector<const char *> p;
        for (const auto & s : v) p.push_back(s.c_str());
        gguf_set_arr_str(ctx_, key.c_str(), p.data(), p.size());
    }
    void write(const fs::path & out, const Tensors & tensors, bool f16, const std::function<void(const std::string &)> & log) {
        const size_t overhead = ggml_tensor_overhead() * (tensors.size() + 1);
        ggml_init_params ip{overhead, nullptr, true};
        ggml_context * meta = ggml_init(ip);
        std::vector<std::pair<const Tensor *, bool>> plan;
        for (const auto & [name, t] : tensors) {
            if (skipped(name)) continue;
            // 2-D matrices go to f16 on request; vectors and small head matrices stay f32
            const bool half = f16 && t.shape.size() == 2 && t.shape[0] >= 256 && t.shape[1] >= 256;
            int64_t ne[4] = {1, 1, 1, 1};
            for (size_t d = 0; d < t.shape.size(); ++d) ne[d] = t.shape[t.shape.size() - 1 - d];
            ggml_tensor * g = ggml_new_tensor(meta, half ? GGML_TYPE_F16 : GGML_TYPE_F32, static_cast<int>(std::max<size_t>(1, t.shape.size())), ne);
            ggml_set_name(g, name.c_str());
            gguf_add_tensor(ctx_, g);
            plan.emplace_back(&t, half);
        }
        const fs::path tmp = out.u8string() + ".part";
        if (!gguf_write_to_file(ctx_, tmp.u8string().c_str(), true)) throw std::runtime_error("cannot write " + tmp.u8string());
        std::ofstream f(tmp, std::ios::binary | std::ios::app);
        const size_t align = gguf_get_alignment(ctx_);
        for (const auto & [t, half] : plan) {
            const auto v = t->data();
            size_t bytes;
            if (half) {
                std::vector<ggml_fp16_t> h(v.size());
                ggml_fp32_to_fp16_row(v.data(), h.data(), static_cast<int64_t>(v.size()));
                bytes = h.size() * 2;
                f.write(reinterpret_cast<const char *>(h.data()), static_cast<std::streamsize>(bytes));
            } else {
                bytes = v.size() * 4;
                f.write(reinterpret_cast<const char *>(v.data()), static_cast<std::streamsize>(bytes));
            }
            const size_t pad = (align - bytes % align) % align;
            static const char zeros[64] = {};
            f.write(zeros, static_cast<std::streamsize>(pad));
        }
        f.close();
        if (!f) throw std::runtime_error("write failed: " + tmp.u8string());
        ggml_free(meta);
        fs::rename(tmp, out);
        if (log) log(out.filename().u8string() + ": " + std::to_string(plan.size()) + " tensors");
    }

private:
    gguf_context * ctx_;
};

std::vector<std::string> vocab_lines(const fs::path & p) {
    std::vector<std::string> v;
    std::istringstream in(read_text(p));
    for (std::string line; std::getline(in, line);) v.push_back(line);
    v.push_back("[CAP]");   // ModernBookNLP adds [CAP] as a special token (id 30522)
    return v;
}

void bert_meta(Writer & w, const fs::path & src, const std::string & stem, uint32_t n_layer) {
    const json cfg = json::parse(read_text(src / (stem + "-config.json")));
    gguf_context * c = w.ctx();
    gguf_set_val_u32(c, "bnlp.bert.n_layer", n_layer);
    gguf_set_val_u32(c, "bnlp.bert.n_embd", cfg["hidden_size"].get<uint32_t>());
    gguf_set_val_u32(c, "bnlp.bert.n_head", cfg["num_attention_heads"].get<uint32_t>());
    gguf_set_val_u32(c, "bnlp.bert.n_ff", cfg["intermediate_size"].get<uint32_t>());
    gguf_set_val_f32(c, "bnlp.bert.norm_eps", cfg.value("layer_norm_eps", 1e-12f));
    gguf_set_val_u32(c, "bnlp.bert.n_ctx", cfg["max_position_embeddings"].get<uint32_t>());
    w.set_strings("bnlp.vocab", vocab_lines(src / (stem + "-vocab.txt")));
}

}  // namespace

void convert_models(const fs::path & src, const fs::path & out, bool f16, const std::function<void(const std::string &)> & log) {
    fs::create_directories(out);
    {   // entity tagger
        Tensors t = torch_state_dict(src / "entities.model");
        Writer w("bnlp-entities");
        gguf_set_val_str(w.ctx(), "bnlp.source", "entities_google_bert_uncased_L-6_H-768_A-12-v1.0.model");
        bert_meta(w, src, "bert6", 6);
        std::map<int, std::string> tags;
        std::istringstream in(read_text(src / "entity_cat.tagset"));
        for (std::string line; std::getline(in, line);) {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos) tags[std::stoi(line.substr(tab + 1))] = line.substr(0, tab);
        }
        std::vector<std::string> tagset;
        for (const auto & [_, v] : tags) tagset.push_back(v);
        w.set_strings("bnlp.tagset", tagset);
        gguf_set_val_str(w.ctx(), "bnlp.aliases", read_text(src / "aliases.txt").c_str());
        gguf_set_val_str(w.ctx(), "bnlp.gender_terms", read_text(src / "gender_terms.txt").c_str());
        w.write(out / "bnlp-entities.gguf", t, f16, log);
    }
    {   // coreference: split mention_mention1 into [a ; b ; a*b ; distance/nested/speaker]
        Tensors t = torch_state_dict(src / "coref.model");
        Tensor * m1 = find(t, "mention_mention1.weight");
        Tensor * u1 = find(t, "unary1.weight");
        if (!m1 || !u1) throw std::runtime_error("coref checkpoint lacks mention_mention1/unary1");
        const Tensor w1 = *m1;
        const int64_t r = u1->shape[1];
        erase(t, "mention_mention1.weight");
        t.emplace_back("mention_mention1.weight_a", columns(w1, 0, r));
        t.emplace_back("mention_mention1.weight_b", columns(w1, r, 2 * r));
        t.emplace_back("mention_mention1.weight_ab", columns(w1, 2 * r, 3 * r));
        t.emplace_back("mention_mention1.weight_rest", columns(w1, 3 * r, w1.shape[1]));
        Writer w("bnlp-coref");
        gguf_set_val_str(w.ctx(), "bnlp.source", "coref_google_bert_uncased_L-12_H-768_A-12-v1.0.model");
        bert_meta(w, src, "bert12", 12);
        w.write(out / "bnlp-coref.gguf", t, f16, log);
    }
    {   // quote attribution: fold eval-mode BatchNorm1d (proj.1) into proj.0, split proj.0 by input half
        Tensors t = safetensors(src / "modernqa.safetensors");
        auto get = [&](const std::string & n) {
            Tensor * x = find(t, n);
            if (!x) throw std::runtime_error("quote model lacks " + n);
            return *x;
        };
        const Tensor w0 = get("proj.0.weight"), b0 = get("proj.0.bias"), g = get("proj.1.weight"), beta = get("proj.1.bias"),
                     mean = get("proj.1.running_mean"), var = get("proj.1.running_var");
        auto scale = [g, var]() {
            const auto gv = g.data(), vv = var.data();
            std::vector<float> s(gv.size());
            for (size_t i = 0; i < s.size(); ++i) s[i] = gv[i] / std::sqrt(vv[i] + 1e-5f);
            return s;
        };
        Tensor folded;
        folded.shape = w0.shape;
        folded.data = [w0, scale]() {
            auto w = w0.data();
            const auto s = scale();
            const size_t cols = w.size() / s.size();
            for (size_t r2 = 0; r2 < s.size(); ++r2)
                for (size_t c = 0; c < cols; ++c) w[r2 * cols + c] *= s[r2];
            return w;
        };
        Tensor bias;
        bias.shape = b0.shape;
        bias.data = [b0, beta, mean, scale]() {
            auto b = b0.data();
            const auto s = scale(), be = beta.data(), m = mean.data();
            for (size_t i = 0; i < b.size(); ++i) b[i] = (b[i] - m[i]) * s[i] + be[i];
            return b;
        };
        *find(t, "proj.0.bias") = bias;
        for (const char * n : {"proj.1.weight", "proj.1.bias", "proj.1.running_mean", "proj.1.running_var", "proj.1.num_batches_tracked", "proj.0.weight"})
            erase(t, n);
        const int64_t half = w0.shape[1] / 2;
        t.emplace_back("proj.0.weight_q", columns(folded, 0, half));
        t.emplace_back("proj.0.weight_m", columns(folded, half, w0.shape[1]));
        const json cfg = json::parse(read_text(src / "modernbert-config.json"));
        Writer w("bnlp-quote");
        gguf_context * c = w.ctx();
        gguf_set_val_str(c, "bnlp.source", "ModernBERT_T2000.safetensors");
        gguf_set_val_u32(c, "bnlp.mbert.n_layer", cfg["num_hidden_layers"].get<uint32_t>());
        gguf_set_val_u32(c, "bnlp.mbert.n_embd", cfg["hidden_size"].get<uint32_t>());
        gguf_set_val_u32(c, "bnlp.mbert.n_head", cfg["num_attention_heads"].get<uint32_t>());
        gguf_set_val_u32(c, "bnlp.mbert.n_ff", cfg["intermediate_size"].get<uint32_t>());
        gguf_set_val_f32(c, "bnlp.mbert.norm_eps", cfg["norm_eps"].get<float>());
        gguf_set_val_u32(c, "bnlp.mbert.global_every", cfg["global_attn_every_n_layers"].get<uint32_t>());
        gguf_set_val_u32(c, "bnlp.mbert.local_window", cfg["local_attention"].get<uint32_t>());
        gguf_set_val_f32(c, "bnlp.mbert.global_rope_theta", cfg["global_rope_theta"].get<float>());
        gguf_set_val_f32(c, "bnlp.mbert.local_rope_theta", cfg["local_rope_theta"].get<float>());
        gguf_set_val_str(c, "bnlp.tokenizer_json", read_text(src / "modernbert-tokenizer.json").c_str());
        w.write(out / "bnlp-quote.gguf", t, f16, log);
    }
}

}  // namespace rm::booknlp
