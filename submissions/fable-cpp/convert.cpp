// NullTorch PTH-Read cell — a from-scratch PyTorch .pth reader.
// C++20, standard library only. Build: g++ -std=c++20 -O2 convert.cpp -o convert
// Usage: ./convert <file.pth> <out_dir>
//
// Reads a torch zip (STORED, optional zip64/deflate) checkpoint, walks the
// pickle object graph (_rebuild_tensor_v2 over typed storages), and emits a
// nulltorch manifest + contiguous row-major tensor .bin files.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;
using std::string;
using std::vector;

// ------------------------------------------------------------------ helpers
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

[[noreturn]] static void die(const string& msg) {
    std::fprintf(stderr, "error: %s\n", msg.c_str());
    std::exit(1);
}

// ------------------------------------------------------------------ inflate (RFC1951, for deflate entries)
struct BitReader {
    const uint8_t* d; size_t n; size_t byte = 0; int bit = 0;
    BitReader(const uint8_t* d_, size_t n_) : d(d_), n(n_) {}
    int getbit() {
        if (byte >= n) throw std::runtime_error("deflate: out of bits");
        int b = (d[byte] >> bit) & 1;
        if (++bit == 8) { bit = 0; byte++; }
        return b;
    }
    uint32_t getbits(int c) { uint32_t v = 0; for (int i = 0; i < c; i++) v |= (uint32_t)getbit() << i; return v; }
    void align() { if (bit) { bit = 0; byte++; } }
};
struct Huff {
    // canonical decode via counts
    vector<int> counts, symbols;
    void build(const vector<int>& lens) {
        int maxbits = 0; for (int l : lens) maxbits = std::max(maxbits, l);
        counts.assign(maxbits + 1, 0);
        for (int l : lens) if (l) counts[l]++;
        vector<int> offs(maxbits + 2, 0);
        for (int i = 1; i <= maxbits; i++) offs[i + 1] = offs[i] + counts[i];
        symbols.assign(lens.size(), 0);
        for (int s = 0; s < (int)lens.size(); s++) if (lens[s]) symbols[offs[lens[s]]++] = s;
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < (int)counts.size(); len++) {
            code |= br.getbit();
            int cnt = counts[len];
            if (code - first < cnt) return symbols[index + (code - first)];
            index += cnt; first += cnt; first <<= 1; code <<= 1;
        }
        throw std::runtime_error("deflate: bad code");
    }
};
static vector<uint8_t> inflate_raw(const uint8_t* src, size_t srclen) {
    static const int lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int lext[]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int dext[]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    BitReader br(src, srclen);
    vector<uint8_t> out;
    for (;;) {
        int bfinal = br.getbit();
        int btype = br.getbits(2);
        if (btype == 0) {
            br.align();
            if (br.byte + 4 > br.n) throw std::runtime_error("deflate: stored short");
            uint16_t len = rd16(src + br.byte); br.byte += 4; // skip LEN + NLEN
            for (int i = 0; i < len; i++) out.push_back(src[br.byte++]);
        } else if (btype == 1 || btype == 2) {
            Huff lit, dist;
            if (btype == 1) {
                vector<int> ll(288), dl(30, 5);
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                lit.build(ll); dist.build(dl);
            } else {
                int hlit = br.getbits(5) + 257, hdist = br.getbits(5) + 1, hclen = br.getbits(4) + 4;
                static const int ord[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                vector<int> cl(19, 0);
                for (int i = 0; i < hclen; i++) cl[ord[i]] = br.getbits(3);
                Huff clh; clh.build(cl);
                vector<int> lens; lens.reserve(hlit + hdist);
                while ((int)lens.size() < hlit + hdist) {
                    int sym = clh.decode(br);
                    if (sym < 16) lens.push_back(sym);
                    else if (sym == 16) { int r = br.getbits(2) + 3; int prev = lens.back(); while (r--) lens.push_back(prev); }
                    else if (sym == 17) { int r = br.getbits(3) + 3; while (r--) lens.push_back(0); }
                    else { int r = br.getbits(7) + 11; while (r--) lens.push_back(0); }
                }
                vector<int> ll(lens.begin(), lens.begin() + hlit);
                vector<int> dl(lens.begin() + hlit, lens.end());
                lit.build(ll); dist.build(dl);
            }
            for (;;) {
                int sym = lit.decode(br);
                if (sym == 256) break;
                if (sym < 256) { out.push_back((uint8_t)sym); continue; }
                sym -= 257;
                int len = lbase[sym] + br.getbits(lext[sym]);
                int dsym = dist.decode(br);
                int d = dbase[dsym] + br.getbits(dext[dsym]);
                size_t start = out.size() - d;
                for (int i = 0; i < len; i++) out.push_back(out[start + i]);
            }
        } else throw std::runtime_error("deflate: bad btype");
        if (bfinal) break;
    }
    return out;
}

// ------------------------------------------------------------------ zip container
struct ZipEntry { string name; uint16_t method; uint64_t comp; uint64_t uncomp; uint64_t local_off; };

struct Zip {
    vector<uint8_t> buf;
    std::unordered_map<string, ZipEntry> entries;

    void load(const string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) die("cannot open " + path);
        f.seekg(0, std::ios::end);
        std::streamoff sz = f.tellg();
        f.seekg(0);
        buf.resize((size_t)sz);
        f.read((char*)buf.data(), sz);
        parse();
    }

    void parse() {
        const uint8_t* b = buf.data();
        size_t n = buf.size();
        // locate EOCD by scanning backward for PK\x05\x06
        size_t eocd = SIZE_MAX;
        size_t scan_lo = (n > 22 + 65535) ? n - (22 + 65535) : 0;
        for (size_t i = n - 22 + 1; i-- > scan_lo; ) {
            if (b[i] == 0x50 && b[i+1] == 0x4b && b[i+2] == 0x05 && b[i+3] == 0x06) { eocd = i; break; }
            if (i == 0) break;
        }
        if (eocd == SIZE_MAX) die("no EOCD found");
        uint64_t cd_off = rd32(b + eocd + 16);
        uint64_t cd_cnt = rd16(b + eocd + 10);
        // zip64?
        if (cd_off == 0xFFFFFFFFu || cd_cnt == 0xFFFFu) {
            if (eocd >= 20 && b[eocd-20] == 0x50 && b[eocd-19] == 0x4b && b[eocd-18] == 0x06 && b[eocd-17] == 0x07) {
                uint64_t z64 = rd64(b + eocd - 20 + 8);
                if (z64 + 56 <= n && b[z64] == 0x50 && b[z64+1] == 0x4b && b[z64+2] == 0x06 && b[z64+3] == 0x06) {
                    cd_cnt = rd64(b + z64 + 32);
                    cd_off = rd64(b + z64 + 48);
                }
            }
        }
        // walk central directory
        size_t p = cd_off;
        for (uint64_t k = 0; k < cd_cnt || cd_cnt == 0; k++) {
            if (p + 46 > n) break;
            if (!(b[p] == 0x50 && b[p+1] == 0x4b && b[p+2] == 0x01 && b[p+3] == 0x02)) break;
            ZipEntry e;
            e.method = rd16(b + p + 10);
            e.comp = rd32(b + p + 20);
            e.uncomp = rd32(b + p + 24);
            uint16_t nlen = rd16(b + p + 28);
            uint16_t elen = rd16(b + p + 30);
            uint16_t clen = rd16(b + p + 32);
            e.local_off = rd32(b + p + 42);
            e.name.assign((const char*)b + p + 46, nlen);
            // zip64 extra field
            const uint8_t* ex = b + p + 46 + nlen;
            const uint8_t* exend = ex + elen;
            while (ex + 4 <= exend) {
                uint16_t id = rd16(ex); uint16_t ds = rd16(ex + 2);
                if (ex + 4 + ds > exend) break;
                if (id == 0x0001) {
                    const uint8_t* q = ex + 4;
                    const uint8_t* qend = q + ds;
                    if (e.uncomp == 0xFFFFFFFFu && q + 8 <= qend) { e.uncomp = rd64(q); q += 8; }
                    if (e.comp == 0xFFFFFFFFu && q + 8 <= qend) { e.comp = rd64(q); q += 8; }
                    if (e.local_off == 0xFFFFFFFFu && q + 8 <= qend) { e.local_off = rd64(q); q += 8; }
                }
                ex += 4 + ds;
            }
            entries[e.name] = e;
            p += 46 + nlen + elen + clen;
            if (cd_cnt != 0 && k + 1 >= cd_cnt) break;
        }
    }

    // return uncompressed data for an entry
    vector<uint8_t> data(const string& name) {
        auto it = entries.find(name);
        if (it == entries.end()) die("zip entry not found: " + name);
        const ZipEntry& e = it->second;
        const uint8_t* b = buf.data();
        size_t lh = e.local_off;
        if (lh + 30 > buf.size()) die("bad local header offset");
        if (!(b[lh] == 0x50 && b[lh+1] == 0x4b && b[lh+2] == 0x03 && b[lh+3] == 0x04)) die("bad local header sig");
        uint16_t nlen = rd16(b + lh + 26);
        uint16_t elen = rd16(b + lh + 28);
        size_t doff = lh + 30 + nlen + elen;
        if (doff + e.comp > buf.size()) die("data out of range");
        if (e.method == 0) {
            return vector<uint8_t>(b + doff, b + doff + e.comp);
        } else if (e.method == 8) {
            return inflate_raw(b + doff, e.comp);
        }
        die("unsupported compression method");
    }
};

// ------------------------------------------------------------------ pickle VM
enum class Tag { None, Bool, Int, Float, Str, Bytes, Tuple, List, Dict, Global, Storage, Tensor, Opaque, Mark };

struct Obj;
using OP = std::shared_ptr<Obj>;

struct Obj {
    Tag tag = Tag::None;
    bool b = false;
    long long i = 0;
    double f = 0;
    string s;            // Str/Bytes value; Global module
    string s2;           // Global name
    vector<OP> items;    // Tuple/List
    vector<std::pair<OP, OP>> dict;  // Dict (insertion-ordered)
    // Storage
    string st_key, st_dtype;
    int st_itemsize = 0;
    long long st_numel = 0;
    // Tensor
    OP storage;
    long long offset = 0;
    vector<long long> shape, stride;
};

static OP mk(Tag t) { auto o = std::make_shared<Obj>(); o->tag = t; return o; }

static bool storage_dtype(const string& cls, string& tok, int& isz) {
    struct M { const char* n; const char* t; int s; };
    static const M m[] = {
        {"DoubleStorage", "f64", 8}, {"FloatStorage", "f32", 4}, {"HalfStorage", "f16", 2},
        {"BFloat16Storage", "bf16", 2}, {"LongStorage", "i64", 8}, {"IntStorage", "i32", 4},
        {"ShortStorage", "i16", 2}, {"CharStorage", "i8", 1}, {"ByteStorage", "u8", 1},
        {"BoolStorage", "bool", 1},
    };
    for (auto& e : m) if (cls == e.n) { tok = e.t; isz = e.s; return true; }
    return false;
}

struct Pickle {
    const uint8_t* d; size_t n; size_t pos = 0;
    vector<OP> stack;
    vector<size_t> marks;
    std::unordered_map<uint32_t, OP> memo;

    Pickle(const uint8_t* d_, size_t n_) : d(d_), n(n_) {}

    uint8_t byte() { if (pos >= n) die("pickle: EOF"); return d[pos++]; }
    string line() { string r; while (pos < n && d[pos] != '\n') r.push_back((char)d[pos++]); if (pos < n) pos++; return r; }
    string readstr(size_t len) { if (pos + len > n) die("pickle: str OOB"); string r((const char*)d + pos, len); pos += len; return r; }

    OP pop() { if (stack.empty()) die("pickle: stack underflow"); OP o = stack.back(); stack.pop_back(); return o; }
    void push(OP o) { stack.push_back(o); }

    OP int_obj(long long v) { OP o = mk(Tag::Int); o->i = v; return o; }

    OP run() {
        while (pos < n) {
            uint8_t op = byte();
            switch (op) {
                case 0x80: byte(); break;                 // PROTO
                case 0x95: pos += 8; break;               // FRAME
                case '.': return pop();                   // STOP
                case '(': marks.push_back(stack.size()); break; // MARK
                case '1': { // POP_MARK
                    if (marks.empty()) die("POP_MARK no mark");
                    stack.resize(marks.back()); marks.pop_back(); break;
                }
                case '0': pop(); break;                   // POP
                case '2': push(stack.back()); break;      // DUP
                case 'N': push(mk(Tag::None)); break;     // NONE
                case 0x88: { OP o = mk(Tag::Bool); o->b = true; push(o); break; }  // NEWTRUE
                case 0x89: { OP o = mk(Tag::Bool); o->b = false; push(o); break; } // NEWFALSE
                case 'J': { int32_t v = (int32_t)rd32(d + pos); pos += 4; push(int_obj(v)); break; } // BININT
                case 'K': push(int_obj(byte())); break;   // BININT1
                case 'M': { uint16_t v = rd16(d + pos); pos += 2; push(int_obj(v)); break; } // BININT2
                case 'I': { string t = line(); push(int_obj(t.empty() ? 0 : std::stoll(t))); break; } // INT
                case 'L': { string t = line(); if (!t.empty() && (t.back()=='L'||t.back()=='l')) t.pop_back(); push(int_obj(t.empty()?0:std::stoll(t))); break; } // LONG
                case 0x8a: { int ln = byte(); long long v = 0; for (int k = 0; k < ln; k++) v |= (long long)byte() << (8*k);
                             if (ln > 0 && (d[pos-1] & 0x80)) { for (int k = ln; k < 8; k++) v |= (long long)0xFF << (8*k); } push(int_obj(v)); break; } // LONG1
                case 0x8b: { uint32_t ln = rd32(d + pos); pos += 4; long long v = 0; for (uint32_t k = 0; k < ln && k < 8; k++) v |= (long long)byte() << (8*k); for (uint32_t k = 8; k < ln; k++) byte(); push(int_obj(v)); break; } // LONG4
                case 'F': { string t = line(); OP o = mk(Tag::Float); o->f = t.empty()?0:std::stod(t); push(o); break; } // FLOAT
                case 'G': { OP o = mk(Tag::Float); uint8_t tmp[8]; for (int k=0;k<8;k++) tmp[k]=d[pos+7-k]; double v; std::memcpy(&v,tmp,8); o->f=v; pos+=8; push(o); break; } // BINFLOAT (big-endian)
                case 'X': { uint32_t l = rd32(d + pos); pos += 4; OP o = mk(Tag::Str); o->s = readstr(l); push(o); break; } // BINUNICODE
                case 0x8c: { uint8_t l = byte(); OP o = mk(Tag::Str); o->s = readstr(l); push(o); break; } // SHORT_BINUNICODE
                case 0x8d: { uint64_t l = rd64(d + pos); pos += 8; OP o = mk(Tag::Str); o->s = readstr(l); push(o); break; } // BINUNICODE8
                case 'V': { OP o = mk(Tag::Str); o->s = line(); push(o); break; } // UNICODE
                case 'U': { uint8_t l = byte(); OP o = mk(Tag::Str); o->s = readstr(l); push(o); break; } // SHORT_BINSTRING
                case 'T': { uint32_t l = rd32(d + pos); pos += 4; OP o = mk(Tag::Str); o->s = readstr(l); push(o); break; } // BINSTRING
                case 'S': { string t = line(); if (t.size()>=2) t = t.substr(1, t.size()-2); OP o = mk(Tag::Str); o->s = t; push(o); break; } // STRING
                case 'B': { uint32_t l = rd32(d + pos); pos += 4; OP o = mk(Tag::Bytes); o->s = readstr(l); push(o); break; } // BINBYTES
                case 'C': { uint8_t l = byte(); OP o = mk(Tag::Bytes); o->s = readstr(l); push(o); break; } // SHORT_BINBYTES
                case 0x8e: { uint64_t l = rd64(d + pos); pos += 8; OP o = mk(Tag::Bytes); o->s = readstr(l); push(o); break; } // BINBYTES8
                case '}': push(mk(Tag::Dict)); break;     // EMPTY_DICT
                case ']': push(mk(Tag::List)); break;     // EMPTY_LIST
                case ')': push(mk(Tag::Tuple)); break;    // EMPTY_TUPLE
                case 0x85: { OP a = pop(); OP t = mk(Tag::Tuple); t->items = {a}; push(t); break; } // TUPLE1
                case 0x86: { OP b2 = pop(), a = pop(); OP t = mk(Tag::Tuple); t->items = {a, b2}; push(t); break; } // TUPLE2
                case 0x87: { OP c = pop(), b2 = pop(), a = pop(); OP t = mk(Tag::Tuple); t->items = {a, b2, c}; push(t); break; } // TUPLE3
                case 't': { // TUPLE
                    size_t m = marks.back(); marks.pop_back();
                    OP t = mk(Tag::Tuple); t->items.assign(stack.begin() + m, stack.end());
                    stack.resize(m); push(t); break;
                }
                case 'l': { // LIST
                    size_t m = marks.back(); marks.pop_back();
                    OP t = mk(Tag::List); t->items.assign(stack.begin() + m, stack.end());
                    stack.resize(m); push(t); break;
                }
                case 'd': { // DICT
                    size_t m = marks.back(); marks.pop_back();
                    OP t = mk(Tag::Dict);
                    for (size_t k = m; k + 1 < stack.size(); k += 2) t->dict.emplace_back(stack[k], stack[k+1]);
                    stack.resize(m); push(t); break;
                }
                case 's': { OP v = pop(), k = pop(); stack.back()->dict.emplace_back(k, v); break; } // SETITEM
                case 'u': { // SETITEMS
                    size_t m = marks.back(); marks.pop_back();
                    OP dd = stack[m - 1];
                    for (size_t k = m; k + 1 < stack.size(); k += 2) dd->dict.emplace_back(stack[k], stack[k+1]);
                    stack.resize(m); break;
                }
                case 'a': { OP v = pop(); stack.back()->items.push_back(v); break; } // APPEND
                case 'e': { // APPENDS
                    size_t m = marks.back(); marks.pop_back();
                    OP ls = stack[m - 1];
                    for (size_t k = m; k < stack.size(); k++) ls->items.push_back(stack[k]);
                    stack.resize(m); break;
                }
                case 'q': { uint8_t idx = byte(); memo[idx] = stack.back(); break; } // BINPUT
                case 'r': { uint32_t idx = rd32(d + pos); pos += 4; memo[idx] = stack.back(); break; } // LONG_BINPUT
                case 'p': { string t = line(); memo[(uint32_t)std::stoul(t)] = stack.back(); break; } // PUT
                case 0x94: { memo[(uint32_t)memo.size()] = stack.back(); break; } // MEMOIZE
                case 'h': { uint8_t idx = byte(); push(memo.at(idx)); break; } // BINGET
                case 'j': { uint32_t idx = rd32(d + pos); pos += 4; push(memo.at(idx)); break; } // LONG_BINGET
                case 'g': { string t = line(); push(memo.at((uint32_t)std::stoul(t))); break; } // GET
                case 'c': { OP o = mk(Tag::Global); o->s = line(); o->s2 = line(); push(o); break; } // GLOBAL
                case 0x93: { OP name = pop(), mod = pop(); OP o = mk(Tag::Global); o->s = mod->s; o->s2 = name->s; push(o); break; } // STACK_GLOBAL
                case 'Q': push(persid(pop())); break;     // BINPERSID
                case 'P': { OP id = mk(Tag::Str); id->s = line(); push(persid(id)); break; } // PERSID
                case 'R': { OP args = pop(), fn = pop(); push(reduce(fn, args)); break; } // REDUCE
                case 0x81: { OP args = pop(), cls = pop(); push(newobj(cls, args)); break; } // NEWOBJ
                case 0x92: { pop(); OP args = pop(), cls = pop(); push(newobj(cls, args)); break; } // NEWOBJ_EX
                case 'b': { pop(); break; } // BUILD: discard state, leave object
                default: die("unhandled pickle opcode 0x" + [](uint8_t x){ char buf[3]; std::snprintf(buf,3,"%02x",x); return string(buf); }(op));
            }
        }
        die("pickle: no STOP");
    }

    OP persid(OP idobj) {
        // idobj is a tuple: ("storage", storage_type_global, key, location, numel)
        OP o = mk(Tag::Storage);
        if (idobj->tag == Tag::Tuple && idobj->items.size() >= 5) {
            OP typ = idobj->items[1];
            if (typ && typ->tag == Tag::Global) storage_dtype(typ->s2, o->st_dtype, o->st_itemsize);
            OP key = idobj->items[2];
            if (key) o->st_key = key->s;
            OP numel = idobj->items[4];
            if (numel) o->st_numel = numel->i;
        }
        return o;
    }

    OP reduce(OP fn, OP args) {
        if (fn && fn->tag == Tag::Global) {
            const string& nm = fn->s2;
            if (nm == "_rebuild_tensor_v2" || nm == "_rebuild_tensor") {
                OP t = mk(Tag::Tensor);
                auto& a = args->items;
                if (a.size() >= 1) t->storage = a[0];
                if (a.size() >= 2) t->offset = a[1]->i;
                if (a.size() >= 3 && a[2]->tag == Tag::Tuple) for (auto& x : a[2]->items) t->shape.push_back(x->i);
                if (a.size() >= 4 && a[3]->tag == Tag::Tuple) for (auto& x : a[3]->items) t->stride.push_back(x->i);
                return t;
            }
            if (nm == "_rebuild_parameter" || nm == "_rebuild_parameter_with_state") {
                if (!args->items.empty()) return args->items[0]; // wrapped tensor
                return mk(Tag::Opaque);
            }
            if (nm == "OrderedDict" || nm == "dict") {
                OP dd = mk(Tag::Dict);
                if (!args->items.empty() && args->items[0]->tag == Tag::List)
                    for (auto& pr : args->items[0]->items)
                        if (pr->tag == Tag::Tuple && pr->items.size() == 2) dd->dict.emplace_back(pr->items[0], pr->items[1]);
                return dd;
            }
        }
        return mk(Tag::Opaque);
    }

    OP newobj(OP cls, OP /*args*/) {
        (void)cls;
        return mk(Tag::Opaque);
    }
};

// ------------------------------------------------------------------ output
static string keystr(const OP& k) {
    if (!k) return "";
    if (k->tag == Tag::Str || k->tag == Tag::Bytes) return k->s;
    if (k->tag == Tag::Int) return std::to_string(k->i);
    if (k->tag == Tag::Bool) return k->b ? "True" : "False";
    return "";
}

struct TensorOut { string path; OP tensor; };

static void collect(const OP& v, const string& path, vector<TensorOut>& out) {
    if (!v) return;
    switch (v->tag) {
        case Tag::Tensor: out.push_back({path, v}); break;
        case Tag::Dict:
            for (auto& kv : v->dict) {
                string np = path.empty() ? keystr(kv.first) : path + "/" + keystr(kv.first);
                collect(kv.second, np, out);
            }
            break;
        case Tag::List:
        case Tag::Tuple:
            for (size_t i = 0; i < v->items.size(); i++) {
                string np = path.empty() ? std::to_string(i) : path + "/" + std::to_string(i);
                collect(v->items[i], np, out);
            }
            break;
        default: break;
    }
}

static string json_escape(const string& s) {
    string r;
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, 8, "\\u%04x", c); r += b; }
                else r += c;
        }
    }
    return r;
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s <file.pth> <out_dir>\n", argv[0]); return 1; }
    string in = argv[1], outdir = argv[2];

    Zip zip;
    zip.load(in);

    // find <prefix>/data.pkl
    string pklname, prefix;
    for (auto& kv : zip.entries) {
        const string& nm = kv.first;
        if (nm.size() >= 9 && nm.compare(nm.size() - 9, 9, "/data.pkl") == 0) { pklname = nm; prefix = nm.substr(0, nm.size() - 9); break; }
        if (nm == "data.pkl") { pklname = nm; prefix = ""; }
    }
    if (pklname.empty()) die("no data.pkl in archive");

    vector<uint8_t> pkl = zip.data(pklname);
    Pickle vm(pkl.data(), pkl.size());
    OP root = vm.run();

    vector<TensorOut> tensors;
    collect(root, "", tensors);

    // sort by path for deterministic output
    std::sort(tensors.begin(), tensors.end(), [](const TensorOut& a, const TensorOut& b){ return a.path < b.path; });

    fs::create_directories(fs::path(outdir) / "tensors");

    // materialize tensors + build manifest
    string tj; // tensors json body
    for (size_t ti = 0; ti < tensors.size(); ti++) {
        const auto& t = tensors[ti].tensor;
        const string& path = tensors[ti].path;
        OP stg = t->storage;
        string dtype = stg ? stg->st_dtype : "";
        int isz = stg ? stg->st_itemsize : 0;
        string key = stg ? stg->st_key : "";
        if (dtype.empty()) die("tensor '" + path + "' has unknown storage dtype");

        // compute nelems (contiguous)
        long long nel = 1; bool zero = false;
        for (long long dsz : t->shape) { if (dsz == 0) zero = true; nel *= dsz; }
        if (t->shape.empty()) nel = 1;
        if (zero) nel = 0;
        long long nbytes = nel * isz;

        // read storage blob (deterministic re-read is fine)
        vector<uint8_t> blob = zip.data(prefix + "/data/" + key);

        // gather contiguous row-major
        vector<uint8_t> outbuf;
        outbuf.reserve((size_t)nbytes);
        if (nel > 0) {
            int nd = (int)t->shape.size();
            vector<long long> idx(nd, 0);
            long long total = nel;
            for (long long e = 0; e < total; e++) {
                long long src = t->offset;
                for (int dd = 0; dd < nd; dd++) src += idx[dd] * t->stride[dd];
                size_t bpos = (size_t)src * isz;
                if (bpos + isz > blob.size()) die("gather OOB for '" + path + "'");
                outbuf.insert(outbuf.end(), blob.begin() + bpos, blob.begin() + bpos + isz);
                // increment multi-index (row-major, last dim fastest)
                for (int dd = nd - 1; dd >= 0; dd--) { if (++idx[dd] < t->shape[dd]) break; idx[dd] = 0; }
            }
        }

        // write .bin  (tensor path with '/' replaced by '__')
        string binname;
        for (char c : path) { if (c == '/') binname += "__"; else binname += c; }
        std::ofstream bf(fs::path(outdir) / "tensors" / (binname + ".bin"), std::ios::binary);
        bf.write((const char*)outbuf.data(), outbuf.size());
        bf.close();

        // manifest entry
        if (ti) tj += ",\n";
        tj += "  \"" + json_escape(path) + "\": {\n";
        tj += "   \"dtype\": \"" + dtype + "\",\n";
        tj += "   \"nbytes\": " + std::to_string(nbytes) + ",\n";
        tj += "   \"shape\": [";
        if (t->shape.empty()) tj += "]";
        else { tj += "\n"; for (size_t k = 0; k < t->shape.size(); k++) { tj += "    " + std::to_string(t->shape[k]); if (k+1<t->shape.size()) tj += ","; tj += "\n"; } tj += "   ]"; }
        tj += ",\n";
        tj += "   \"storage_key\": \"" + json_escape(key) + "\",\n";
        tj += "   \"storage_offset\": " + std::to_string(t->offset) + ",\n";
        tj += "   \"stride\": [";
        if (t->stride.empty()) tj += "]";
        else { tj += "\n"; for (size_t k = 0; k < t->stride.size(); k++) { tj += "    " + std::to_string(t->stride[k]); if (k+1<t->stride.size()) tj += ","; tj += "\n"; } tj += "   ]"; }
        tj += "\n  }";
    }

    string manifest;
    manifest += "{\n";
    manifest += " \"byteorder\": \"little\",\n";
    manifest += " \"nulltorch_manifest\": 1,\n";
    manifest += " \"tensors\": {";
    if (tensors.empty()) manifest += "}\n";
    else manifest += "\n" + tj + "\n }\n";
    manifest += "}\n";

    std::ofstream mf(fs::path(outdir) / "manifest.json", std::ios::binary);
    mf.write(manifest.data(), manifest.size());
    mf.close();

    return 0;
}
