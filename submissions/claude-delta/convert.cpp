// NullTorch DELTA-variant .pth reader — C++20, stdlib only.
//
// Reads a modified PyTorch .pth (delta "pth-prime"):
//   * zip record signatures are DZ.. instead of PK.. (Deviation 1)
//   * persistent-id tuple order ('storage', key, class_str, location, numel)
//     with a plain-string class name (Deviation 2 & 3)
//   * storage class names end in "Vault" instead of "Storage" (Deviation 3)
//
// Emits <out>/manifest.json and <out>/tensors/<name>.bin per the task contract.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

// ------------------------------------------------------------------ util
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n);
    if (n) f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// ------------------------------------------------------------------ zip
struct ZipEntry {
    std::string name;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct Archive {
    std::vector<uint8_t> raw;
    std::vector<ZipEntry> entries;

    const ZipEntry* find_suffix(const std::string& suffix) const {
        for (auto& e : entries)
            if (e.name.size() >= suffix.size() &&
                e.name.compare(e.name.size() - suffix.size(), suffix.size(), suffix) == 0)
                return &e;
        return nullptr;
    }
};

// Delta signatures: 'D','Z', ...
static const uint8_t SIG_LOCAL[4] = {'D', 'Z', 0x03, 0x04};
static const uint8_t SIG_CENTRAL[4] = {'D', 'Z', 0x01, 0x02};
static const uint8_t SIG_EOCD[4] = {'D', 'Z', 0x05, 0x06};
static const uint8_t SIG_EOCD64[4] = {'D', 'Z', 0x06, 0x06};
static const uint8_t SIG_LOC64[4] = {'D', 'Z', 0x06, 0x07};

static bool match(const uint8_t* p, const uint8_t* sig) {
    return p[0] == sig[0] && p[1] == sig[1] && p[2] == sig[2] && p[3] == sig[3];
}

static Archive parse_zip(const std::string& path) {
    Archive ar;
    ar.raw = read_file(path);
    const uint8_t* base = ar.raw.data();
    size_t n = ar.raw.size();
    if (n < 22) throw std::runtime_error("file too small for zip");

    // Locate EOCD by scanning backward.
    ssize_t eocd = -1;
    size_t maxback = std::min<size_t>(n, 22 + 65535);
    for (size_t i = 0; i < maxback - 3; i++) {
        size_t off = n - 22 - i;
        if (match(base + off, SIG_EOCD)) { eocd = (ssize_t)off; break; }
    }
    if (eocd < 0) throw std::runtime_error("EOCD (DZ\\x05\\x06) not found");

    const uint8_t* e = base + eocd;
    uint64_t cd_count = rd16(e + 10);
    uint64_t cd_size = rd32(e + 12);
    uint64_t cd_off = rd32(e + 16);

    // zip64 handling if saturated.
    if (cd_off == 0xFFFFFFFFu || cd_count == 0xFFFFu || cd_size == 0xFFFFFFFFu) {
        if (eocd >= 20 && match(base + eocd - 20, SIG_LOC64)) {
            uint64_t z64off = rd64(base + eocd - 20 + 8);
            if (z64off + 56 <= n && match(base + z64off, SIG_EOCD64)) {
                const uint8_t* z = base + z64off;
                cd_count = rd64(z + 32);
                cd_size = rd64(z + 40);
                cd_off = rd64(z + 48);
            }
        }
    }

    // Walk the central directory.
    size_t p = cd_off;
    for (uint64_t i = 0; i < cd_count; i++) {
        if (p + 46 > n || !match(base + p, SIG_CENTRAL))
            throw std::runtime_error("bad central directory header");
        uint16_t method = rd16(base + p + 10);
        uint32_t comp_size = rd32(base + p + 20);
        uint16_t nlen = rd16(base + p + 28);
        uint16_t mlen = rd16(base + p + 30);
        uint16_t klen = rd16(base + p + 32);
        uint64_t lho = rd32(base + p + 42);
        std::string name((const char*)base + p + 46, nlen);

        // zip64 extra: pull real comp_size / local-header-offset if saturated.
        if (comp_size == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) {
            const uint8_t* ex = base + p + 46 + nlen;
            const uint8_t* exend = ex + mlen;
            while (ex + 4 <= exend) {
                uint16_t id = rd16(ex);
                uint16_t sz = rd16(ex + 2);
                const uint8_t* d = ex + 4;
                if (id == 0x0001) {
                    const uint8_t* q = d;
                    uint32_t usz = rd32(base + p + 24);
                    if (usz == 0xFFFFFFFFu) q += 8;                 // uncompressed
                    if (comp_size == 0xFFFFFFFFu) { comp_size = (uint32_t)rd64(q); q += 8; }
                    if (lho == 0xFFFFFFFFu) { lho = rd64(q); q += 8; }
                }
                ex = d + sz;
            }
        }

        // Resolve local header to find data start.
        if (lho + 30 > n || !match(base + lho, SIG_LOCAL))
            throw std::runtime_error("bad local file header for " + name);
        uint16_t lnlen = rd16(base + lho + 26);
        uint16_t lmlen = rd16(base + lho + 28);
        size_t data_off = lho + 30 + lnlen + lmlen;

        ZipEntry ze;
        ze.name = name;
        if (method == 0) {                 // STORED
            ze.data = base + data_off;
            ze.size = comp_size;
        } else {
            ze.data = nullptr;             // unsupported (not needed for these fixtures)
            ze.size = 0;
        }
        ar.entries.push_back(std::move(ze));
        p += 46 + nlen + mlen + klen;
    }
    return ar;
}

// ------------------------------------------------------------------ dtype
struct DType { std::string token; int itemsize; };

static bool dtype_from_class(const std::string& cls, DType& out) {
    // Map both delta "...Vault" and stock "...Storage" prefixes.
    static const std::vector<std::pair<std::string, DType>> tbl = {
        {"Double", {"f64", 8}}, {"Float8_e4m3fn", {"f8_e4m3", 1}},
        {"Float8_e5m2", {"f8_e5m2", 1}}, {"Float", {"f32", 4}},
        {"Half", {"f16", 2}}, {"BFloat16", {"bf16", 2}},
        {"Long", {"i64", 8}}, {"Int", {"i32", 4}}, {"Short", {"i16", 2}},
        {"Char", {"i8", 1}}, {"Byte", {"u8", 1}}, {"Bool", {"bool", 1}},
    };
    // Strip trailing "Vault" or "Storage".
    std::string stem = cls;
    for (const std::string suf : {std::string("Vault"), std::string("Storage")}) {
        if (stem.size() >= suf.size() &&
            stem.compare(stem.size() - suf.size(), suf.size(), suf) == 0) {
            stem = stem.substr(0, stem.size() - suf.size());
            break;
        }
    }
    for (auto& kv : tbl) {
        if (stem == kv.first) { out = kv.second; return true; }
    }
    return false;
}

// ------------------------------------------------------------------ pickle VM
struct Value;
using VP = std::shared_ptr<Value>;

enum class K { None, Bool, Int, Str, Bytes, Tuple, List, Dict, Global, Storage, Tensor, Mark, Opaque };

struct Value {
    K kind = K::None;
    long long i = 0;              // Int / Bool
    std::string s;               // Str / Bytes
    std::string mod, name;       // Global module + attr
    std::vector<VP> items;       // Tuple / List
    std::vector<std::pair<VP, VP>> dict;  // Dict (ordered)
    // Storage
    std::string st_key, st_dtype; int st_itemsize = 0; std::string st_loc; long long st_numel = 0;
    // Tensor
    std::string t_key, t_dtype; int t_itemsize = 0;
    long long t_offset = 0;
    std::vector<long long> t_size, t_stride;
};

static VP mk(K k) { auto v = std::make_shared<Value>(); v->kind = k; return v; }
static VP mkint(long long x) { auto v = mk(K::Int); v->i = x; return v; }
static VP mkstr(std::string x) { auto v = mk(K::Str); v->s = std::move(x); return v; }

struct Unpickler {
    const uint8_t* p;
    const uint8_t* end;
    std::vector<VP> stack;
    std::vector<size_t> marks;
    std::unordered_map<long long, VP> memo;

    explicit Unpickler(const uint8_t* data, size_t n) : p(data), end(data + n) {}

    uint8_t u8() { if (p >= end) throw std::runtime_error("pickle EOF"); return *p++; }
    void need(size_t k) { if ((size_t)(end - p) < k) throw std::runtime_error("pickle truncated"); }

    std::string read_line() {
        std::string out;
        while (p < end && *p != '\n') out.push_back((char)*p++);
        if (p < end) p++;  // consume newline
        return out;
    }

    void push(VP v) { stack.push_back(std::move(v)); }
    VP pop() { if (stack.empty()) throw std::runtime_error("stack underflow"); VP v = stack.back(); stack.pop_back(); return v; }

    long long as_int(const VP& v) {
        if (v->kind == K::Int || v->kind == K::Bool) return v->i;
        throw std::runtime_error("expected int");
    }
    std::string obj_name(const VP& v) {  // for class strings: Str or Global
        if (v->kind == K::Str) return v->s;
        if (v->kind == K::Global) return v->name;
        return "";
    }

    VP run() {
        while (p < end) {
            uint8_t op = u8();
            switch (op) {
                case 0x80: u8(); break;                                  // PROTO
                case 0x95: need(8); p += 8; break;                       // FRAME (skip len)
                case '.': return pop();                                   // STOP

                // ints / bools
                case 'I': {                                              // INT
                    std::string l = read_line();
                    if (l == "01") push([]{auto v=mk(K::Bool);v->i=1;return v;}());
                    else if (l == "00") push([]{auto v=mk(K::Bool);v->i=0;return v;}());
                    else push(mkint(std::stoll(l)));
                    break;
                }
                case 'J': { need(4); int32_t x = (int32_t)rd32(p); p += 4; push(mkint(x)); break; }  // BININT
                case 'K': { push(mkint(u8())); break; }                  // BININT1
                case 'M': { need(2); push(mkint(rd16(p))); p += 2; break; }  // BININT2
                case 'L': { std::string l = read_line(); if (!l.empty() && l.back()=='L') l.pop_back(); push(mkint(std::stoll(l))); break; }  // LONG
                case 0x8a: {                                             // LONG1
                    int len = u8(); need(len);
                    long long v = 0; for (int k = 0; k < len; k++) v |= (long long)p[k] << (8*k);
                    if (len > 0 && (p[len-1] & 0x80)) v -= ((long long)1 << (8*len));  // sign extend
                    p += len; push(mkint(v)); break;
                }
                case 0x8b: {                                             // LONG4
                    need(4); uint32_t len = rd32(p); p += 4; need(len);
                    long long v = 0; for (uint32_t k = 0; k < len && k < 8; k++) v |= (long long)p[k] << (8*k);
                    if (len > 0 && (p[len-1] & 0x80) && len < 8) v -= ((long long)1 << (8*len));
                    p += len; push(mkint(v)); break;
                }
                case 'N': push(mk(K::None)); break;                      // NONE
                case 0x88: { auto v = mk(K::Bool); v->i = 1; push(v); break; }  // NEWTRUE
                case 0x89: { auto v = mk(K::Bool); v->i = 0; push(v); break; }  // NEWFALSE

                // floats (kept as Opaque; not needed for tensor extraction)
                case 'F': { read_line(); push(mk(K::Opaque)); break; }   // FLOAT
                case 'G': { need(8); p += 8; push(mk(K::Opaque)); break; }  // BINFLOAT

                // strings
                case 'X': { need(4); uint32_t l = rd32(p); p += 4; need(l); push(mkstr(std::string((const char*)p, l))); p += l; break; }  // BINUNICODE
                case 0x8c: { uint8_t l = u8(); need(l); push(mkstr(std::string((const char*)p, l))); p += l; break; }  // SHORT_BINUNICODE
                case 0x8d: { need(8); uint64_t l = rd64(p); p += 8; need(l); push(mkstr(std::string((const char*)p, l))); p += l; break; }  // BINUNICODE8
                case 'V': { push(mkstr(read_line())); break; }           // UNICODE
                case 'S': {                                              // STRING (repr-quoted)
                    std::string l = read_line();
                    if (l.size() >= 2 && (l.front()=='\'' || l.front()=='"')) l = l.substr(1, l.size()-2);
                    push(mkstr(l)); break;
                }
                case 'T': { need(4); uint32_t l = rd32(p); p += 4; need(l); push(mkstr(std::string((const char*)p, l))); p += l; break; }  // BINSTRING
                case 'U': { uint8_t l = u8(); need(l); push(mkstr(std::string((const char*)p, l))); p += l; break; }  // SHORT_BINSTRING

                // bytes
                case 'B': { need(4); uint32_t l = rd32(p); p += 4; need(l); auto v=mk(K::Bytes); v->s.assign((const char*)p,l); push(v); p += l; break; }
                case 'C': { uint8_t l = u8(); need(l); auto v=mk(K::Bytes); v->s.assign((const char*)p,l); push(v); p += l; break; }
                case 0x8e: { need(8); uint64_t l = rd64(p); p += 8; need(l); auto v=mk(K::Bytes); v->s.assign((const char*)p,l); push(v); p += l; break; }

                // containers
                case ')': push(mk(K::Tuple)); break;                     // EMPTY_TUPLE
                case 0x85: { auto t = mk(K::Tuple); t->items = { pop() }; push(t); break; }  // TUPLE1
                case 0x86: { VP b = pop(), a = pop(); auto t = mk(K::Tuple); t->items = {a, b}; push(t); break; }  // TUPLE2
                case 0x87: { VP c = pop(), b = pop(), a = pop(); auto t = mk(K::Tuple); t->items = {a, b, c}; push(t); break; }  // TUPLE3
                case 't': {                                              // TUPLE
                    size_t m = pop_mark(); auto t = mk(K::Tuple);
                    for (size_t k = m; k < stack.size(); k++) t->items.push_back(stack[k]);
                    stack.resize(m); push(t); break;
                }
                case ']': push(mk(K::List)); break;                      // EMPTY_LIST
                case 'l': {                                              // LIST
                    size_t m = pop_mark(); auto t = mk(K::List);
                    for (size_t k = m; k < stack.size(); k++) t->items.push_back(stack[k]);
                    stack.resize(m); push(t); break;
                }
                case 'a': { VP x = pop(); stack.back()->items.push_back(x); break; }  // APPEND
                case 'e': {                                              // APPENDS
                    size_t m = pop_mark();
                    std::vector<VP> tmp(stack.begin() + m, stack.end());
                    stack.resize(m);
                    for (auto& x : tmp) stack.back()->items.push_back(x);
                    break;
                }
                case '}': push(mk(K::Dict)); break;                      // EMPTY_DICT
                case 'd': {                                              // DICT
                    size_t m = pop_mark(); auto t = mk(K::Dict);
                    for (size_t k = m; k + 1 < stack.size(); k += 2) t->dict.push_back({stack[k], stack[k+1]});
                    stack.resize(m); push(t); break;
                }
                case 's': { VP val = pop(), key = pop(); stack.back()->dict.push_back({key, val}); break; }  // SETITEM
                case 'u': {                                              // SETITEMS
                    size_t m = pop_mark();
                    std::vector<VP> tmp(stack.begin() + m, stack.end());
                    stack.resize(m);
                    for (size_t k = 0; k + 1 < tmp.size(); k += 2) stack.back()->dict.push_back({tmp[k], tmp[k+1]});
                    break;
                }
                case 0x8f: { push(mk(K::Opaque)); break; }               // EMPTY_SET
                case 0x90: { pop_mark_discard(); break; }                // ADDITEMS
                case 0x91: { pop_mark_discard(); push(mk(K::Opaque)); break; }  // FROZENSET

                // stack manip
                case '0': pop(); break;                                   // POP
                case '2': push(stack.back()); break;                     // DUP
                case '(': marks.push_back(stack.size()); break;          // MARK
                case '1': pop_mark_discard(); break;                     // POP_MARK

                // memo
                case 'p': { long long idx = std::stoll(read_line()); memo[idx] = stack.back(); break; }  // PUT
                case 'q': { long long idx = u8(); memo[idx] = stack.back(); break; }  // BINPUT
                case 'r': { need(4); long long idx = rd32(p); p += 4; memo[idx] = stack.back(); break; }  // LONG_BINPUT
                case 0x94: { memo[(long long)memo.size()] = stack.back(); break; }  // MEMOIZE
                case 'g': { long long idx = std::stoll(read_line()); push(memo.at(idx)); break; }  // GET
                case 'h': { long long idx = u8(); push(memo.at(idx)); break; }  // BINGET
                case 'j': { need(4); long long idx = rd32(p); p += 4; push(memo.at(idx)); break; }  // LONG_BINGET

                // globals
                case 'c': { std::string m = read_line(); std::string nm = read_line(); auto v = mk(K::Global); v->mod = m; v->name = nm; push(v); break; }  // GLOBAL
                case 0x93: { VP nm = pop(), m = pop(); auto v = mk(K::Global); v->mod = m->s; v->name = nm->s; push(v); break; }  // STACK_GLOBAL

                // persistent id
                case 'Q': { push(persistent_load(pop())); break; }       // BINPERSID
                case 'P': { read_line(); push(mk(K::Opaque)); break; }    // PERSID (string form; unused)

                // build / reduce / new
                case 'R': { VP args = pop(), fn = pop(); push(reduce(fn, args)); break; }  // REDUCE
                case 0x81: { VP args = pop(), cls = pop(); push(newobj(cls, args)); break; }  // NEWOBJ
                case 0x92: { VP kw = pop(), args = pop(), cls = pop(); (void)kw; push(newobj(cls, args)); break; }  // NEWOBJ_EX
                case 'b': { VP state = pop(); build(stack.back(), state); break; }  // BUILD
                case 'o': {                                              // OBJ
                    size_t m = pop_mark();
                    if (m >= stack.size()) throw std::runtime_error("OBJ underflow");
                    VP cls = stack[m];
                    auto args = mk(K::Tuple);
                    for (size_t k = m + 1; k < stack.size(); k++) args->items.push_back(stack[k]);
                    stack.resize(m); push(newobj(cls, args)); break;
                }
                case 'i': {                                              // INST
                    std::string m = read_line(); std::string nm = read_line(); (void)m; (void)nm;
                    pop_mark_discard(); push(mk(K::Opaque)); break;
                }

                default:
                    throw std::runtime_error("unhandled pickle opcode 0x" + [](uint8_t o){char b[3];snprintf(b,3,"%02x",o);return std::string(b);}(op));
            }
        }
        throw std::runtime_error("pickle ended without STOP");
    }

    size_t pop_mark() {
        if (marks.empty()) throw std::runtime_error("no mark");
        size_t m = marks.back(); marks.pop_back(); return m;
    }
    void pop_mark_discard() { size_t m = pop_mark(); stack.resize(m); }

    VP persistent_load(VP pid) {
        // Delta tuple: ('storage', key, class_str, location, numel).
        // Robust: pick whichever of elements 1/2 is a known storage class.
        if (pid->kind != K::Tuple || pid->items.size() < 5)
            throw std::runtime_error("bad persistent id");
        auto& it = pid->items;
        std::string a = obj_name(it[1]);
        std::string b = obj_name(it[2]);
        DType dt; std::string cls, key;
        if (dtype_from_class(a, dt)) { cls = a; key = b; }
        else if (dtype_from_class(b, dt)) { cls = b; key = a; }
        else throw std::runtime_error("unknown storage class in persistent id");
        auto v = mk(K::Storage);
        v->st_key = key;
        v->st_dtype = dt.token;
        v->st_itemsize = dt.itemsize;
        v->st_loc = obj_name(it[3]);
        v->st_numel = as_int(it[4]);
        return v;
    }

    VP reduce(VP fn, VP args) {
        if (fn->kind == K::Global) {
            if (fn->name.find("_rebuild_tensor") != std::string::npos)
                return build_tensor(args);
            if (fn->name.find("OrderedDict") != std::string::npos) {
                auto d = mk(K::Dict);
                // OrderedDict(list_of_pairs) — populate if given.
                if (!args->items.empty() && args->items[0]->kind == K::List) {
                    for (auto& pr : args->items[0]->items)
                        if (pr->kind == K::Tuple && pr->items.size() == 2)
                            d->dict.push_back({pr->items[0], pr->items[1]});
                }
                return d;
            }
        }
        return mk(K::Opaque);  // graceful skip for unknown callables
    }

    VP newobj(VP cls, VP /*args*/) {
        (void)cls;
        return mk(K::Opaque);  // module/parameter objects: skipped in traversal
    }

    void build(VP obj, VP state) {
        // Only meaningful for opaque objects we skip; ignore.
        (void)obj; (void)state;
    }

    VP build_tensor(VP args) {
        // _rebuild_tensor_v2(storage, storage_offset, size, stride, requires_grad, hooks)
        if (args->kind != K::Tuple || args->items.size() < 4)
            throw std::runtime_error("bad _rebuild_tensor_v2 args");
        VP storage = args->items[0];
        if (storage->kind != K::Storage) throw std::runtime_error("tensor arg0 not a storage");
        auto v = mk(K::Tensor);
        v->t_key = storage->st_key;
        v->t_dtype = storage->st_dtype;
        v->t_itemsize = storage->st_itemsize;
        v->t_offset = as_int(args->items[1]);
        for (auto& x : args->items[2]->items) v->t_size.push_back(as_int(x));
        for (auto& x : args->items[3]->items) v->t_stride.push_back(as_int(x));
        return v;
    }
};

// ------------------------------------------------------------------ traversal / output
struct TensorRec {
    std::string path;
    Value* t;
};

static std::string key_to_string(const VP& k) {
    if (k->kind == K::Str) return k->s;
    if (k->kind == K::Int) return std::to_string(k->i);
    if (k->kind == K::Bool) return k->i ? "True" : "False";
    return "";
}

static void walk(const VP& v, const std::string& prefix, std::vector<TensorRec>& out) {
    switch (v->kind) {
        case K::Tensor:
            out.push_back({prefix, v.get()});
            break;
        case K::Dict:
            for (auto& kv : v->dict) {
                std::string key = key_to_string(kv.first);
                std::string np = prefix.empty() ? key : prefix + "/" + key;
                walk(kv.second, np, out);
            }
            break;
        case K::List:
        case K::Tuple:
            for (size_t idx = 0; idx < v->items.size(); idx++) {
                std::string key = std::to_string(idx);
                std::string np = prefix.empty() ? key : prefix + "/" + key;
                walk(v->items[idx], np, out);
            }
            break;
        default:
            break;  // scalars, strings, None, opaque — not tensors
    }
}

static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <file.pth> <out_dir>\n", argv[0]);
        return 2;
    }
    try {
        std::string in = argv[1];
        std::string out = argv[2];

        Archive ar = parse_zip(in);
        const ZipEntry* pkl = ar.find_suffix("/data.pkl");
        if (!pkl) pkl = ar.find_suffix("data.pkl");
        if (!pkl || !pkl->data) throw std::runtime_error("data.pkl not found");

        Unpickler up(pkl->data, pkl->size);
        VP root = up.run();

        std::vector<TensorRec> tensors;
        walk(root, "", tensors);
        std::sort(tensors.begin(), tensors.end(),
                  [](const TensorRec& a, const TensorRec& b) { return a.path < b.path; });

        // Storage lookup by key -> bytes.
        auto storage_bytes = [&](const std::string& key, size_t& len) -> const uint8_t* {
            const ZipEntry* e = ar.find_suffix("/data/" + key);
            if (!e) e = ar.find_suffix("data/" + key);
            if (!e || !e->data) throw std::runtime_error("storage data/" + key + " not found");
            len = e->size;
            return e->data;
        };

        fs::create_directories(out);
        fs::create_directories(fs::path(out) / "tensors");

        // Emit tensors + manifest.
        std::string mj;
        mj += "{\n \"byteorder\": \"little\",\n \"nulltorch_manifest\": 1,\n \"tensors\": {\n";
        for (size_t ti = 0; ti < tensors.size(); ti++) {
            Value* t = tensors[ti].t;
            const std::string& path = tensors[ti].path;
            int isz = t->t_itemsize;

            long long nelem = 1;
            for (long long d : t->t_size) nelem *= d;
            long long nbytes = nelem * isz;

            size_t slen = 0;
            const uint8_t* sdata = storage_bytes(t->t_key, slen);

            // Materialize contiguous, row-major.
            std::string filebase;
            for (char c : path) { if (c == '/') filebase += "__"; else filebase += c; }
            fs::path binpath = fs::path(out) / "tensors" / (filebase + ".bin");
            std::ofstream bf(binpath, std::ios::binary);
            if (!bf) throw std::runtime_error("cannot write " + binpath.string());

            int nd = (int)t->t_size.size();
            std::vector<long long> idx(nd, 0);
            std::vector<char> buf;
            buf.reserve((size_t)std::max<long long>(nbytes, 0));
            if (nelem > 0) {
                for (long long lin = 0; lin < nelem; lin++) {
                    long long off = t->t_offset;
                    for (int d = 0; d < nd; d++) off += idx[d] * t->t_stride[d];
                    size_t bytepos = (size_t)off * isz;
                    if (bytepos + isz > slen) throw std::runtime_error("storage OOB for " + path);
                    buf.insert(buf.end(), (const char*)sdata + bytepos, (const char*)sdata + bytepos + isz);
                    for (int d = nd - 1; d >= 0; d--) { if (++idx[d] < t->t_size[d]) break; idx[d] = 0; }
                }
            }
            if (!buf.empty()) bf.write(buf.data(), buf.size());
            bf.close();

            // Manifest entry.
            mj += "  \"" + json_escape(path) + "\": {\n";
            mj += "   \"dtype\": \"" + t->t_dtype + "\",\n";
            mj += "   \"nbytes\": " + std::to_string(nbytes) + ",\n";
            mj += "   \"shape\": [";
            for (size_t k = 0; k < t->t_size.size(); k++) { if (k) mj += ", "; mj += std::to_string(t->t_size[k]); }
            mj += "],\n";
            mj += "   \"storage_key\": \"" + json_escape(t->t_key) + "\",\n";
            mj += "   \"storage_offset\": " + std::to_string(t->t_offset) + ",\n";
            mj += "   \"stride\": [";
            for (size_t k = 0; k < t->t_stride.size(); k++) { if (k) mj += ", "; mj += std::to_string(t->t_stride[k]); }
            mj += "]\n";
            mj += ti + 1 < tensors.size() ? "  },\n" : "  }\n";
        }
        mj += " }\n}\n";

        std::ofstream mf(fs::path(out) / "manifest.json", std::ios::binary);
        mf << mj;
        mf.close();

        return 0;
    } catch (const std::exception& ex) {
        fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
