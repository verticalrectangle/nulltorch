// NullTorch PTH reader — reads PyTorch .pth checkpoints (ZIP + pickle)
// using only the C++ standard library. Single process, no subprocesses,
// no network. See harness/TASK.md for the output contract.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>
#include <filesystem>

struct Err : std::runtime_error { using std::runtime_error::runtime_error; };

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

// ---------------------------------------------------------------- inflate --
struct BitRd {
    const uint8_t* p; size_t n; size_t pos = 0; uint32_t cur = 0; int nb = 0;
    int get1() {
        if (!nb) { if (pos >= n) throw Err("inflate: input overrun"); cur = p[pos++]; nb = 8; }
        int b = cur & 1; cur >>= 1; nb--; return b;
    }
    uint32_t get(int k) { uint32_t v = 0; for (int i = 0; i < k; i++) v |= (uint32_t)get1() << i; return v; }
    void align() { nb = 0; }
};

struct Huff {
    uint16_t counts[16]{};
    std::vector<uint16_t> syms;
    void build(const uint8_t* lens, int n) {
        for (int i = 0; i < 16; i++) counts[i] = 0;
        for (int i = 0; i < n; i++) {
            if (lens[i] > 15) throw Err("inflate: code length > 15");
            counts[lens[i]]++;
        }
        counts[0] = 0;
        uint32_t offs[16]{};
        for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + counts[l];
        syms.resize(n);
        for (int i = 0; i < n; i++) if (lens[i]) syms[offs[lens[i]]++] = (uint16_t)i;
    }
    int decode(BitRd& br) const {
        uint32_t code = 0, first = 0, idx = 0;
        for (int l = 1; l <= 15; l++) {
            code |= (uint32_t)br.get1();
            uint32_t cnt = counts[l];
            if (code < first + cnt) return syms[idx + (code - first)];
            idx += cnt; first = (first + cnt) << 1; code <<= 1;
        }
        throw Err("inflate: invalid huffman code");
    }
};

static const int LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const int LEN_EXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const int DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const int DIST_EXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void inflate_block_compressed(BitRd& br, const Huff& lit, const Huff& dst,
                                     std::vector<uint8_t>& out, size_t expect) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym == 256) return;
        if (sym < 256) {
            if (out.size() >= expect) throw Err("inflate: output overflow");
            out.push_back((uint8_t)sym);
            continue;
        }
        if (sym > 285) throw Err("inflate: bad length symbol");
        int li = sym - 257;
        size_t len = (size_t)LEN_BASE[li] + br.get(LEN_EXT[li]);
        int dsym = dst.decode(br);
        if (dsym > 29) throw Err("inflate: bad distance symbol");
        size_t dist = (size_t)DIST_BASE[dsym] + br.get(DIST_EXT[dsym]);
        if (dist > out.size()) throw Err("inflate: distance too far back");
        if (out.size() + len > expect) throw Err("inflate: output overflow");
        for (size_t i = 0; i < len; i++) out.push_back(out[out.size() - dist]);
    }
}

static std::vector<uint8_t> inflate(const uint8_t* src, size_t n, size_t expect) {
    if (expect > (size_t)1 << 30) throw Err("inflate: declared size too large");
    std::vector<uint8_t> out;
    out.reserve(expect);
    BitRd br{src, n, 0, 0, 0};
    for (;;) {
        int bfinal = br.get1();
        int btype = (int)br.get(2);
        if (btype == 0) {
            br.align();
            if (br.pos + 4 > n) throw Err("inflate: truncated stored block");
            uint32_t len = rd16(src + br.pos), nlen = rd16(src + br.pos + 2);
            br.pos += 4;
            if (len != (~nlen & 0xFFFF)) throw Err("inflate: NLEN mismatch");
            if (br.pos + len > n) throw Err("inflate: truncated stored data");
            if (out.size() + len > expect) throw Err("inflate: output overflow");
            out.insert(out.end(), src + br.pos, src + br.pos + len);
            br.pos += len;
        } else if (btype == 1 || btype == 2) {
            Huff lit, dst;
            if (btype == 1) {
                uint8_t ll[288], dl[32];
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                for (int i = 0; i < 32; i++) dl[i] = 5;
                lit.build(ll, 288);
                dst.build(dl, 30);
            } else {
                uint32_t hlit = br.get(5) + 257, hdist = br.get(5) + 1, hclen = br.get(4) + 4;
                if (hlit > 288 || hdist > 32) throw Err("inflate: bad HLIT/HDIST");
                static const int ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                uint8_t cll[19]{};
                for (uint32_t i = 0; i < hclen; i++) cll[ORDER[i]] = (uint8_t)br.get(3);
                Huff cl;
                cl.build(cll, 19);
                std::vector<uint8_t> lens;
                lens.reserve(hlit + hdist);
                while (lens.size() < hlit + hdist) {
                    int s = cl.decode(br);
                    if (s < 16) { lens.push_back((uint8_t)s); }
                    else if (s == 16) {
                        if (lens.empty()) throw Err("inflate: repeat with no previous length");
                        int rep = 3 + (int)br.get(2);
                        uint8_t prev = lens.back();
                        while (rep-- > 0) {
                            if (lens.size() >= hlit + hdist) throw Err("inflate: too many code lengths");
                            lens.push_back(prev);
                        }
                    } else if (s == 17) {
                        int rep = 3 + (int)br.get(3);
                        while (rep-- > 0) {
                            if (lens.size() >= hlit + hdist) throw Err("inflate: too many code lengths");
                            lens.push_back(0);
                        }
                    } else if (s == 18) {
                        int rep = 11 + (int)br.get(7);
                        while (rep-- > 0) {
                            if (lens.size() >= hlit + hdist) throw Err("inflate: too many code lengths");
                            lens.push_back(0);
                        }
                    } else throw Err("inflate: bad code-length symbol");
                }
                lit.build(lens.data(), (int)hlit);
                dst.build(lens.data() + hlit, (int)hdist);
            }
            inflate_block_compressed(br, lit, dst, out, expect);
        } else throw Err("inflate: bad BTYPE");
        if (bfinal) break;
    }
    if (out.size() != expect) throw Err("inflate: size mismatch");
    return out;
}

// -------------------------------------------------------------------- zip --
struct Entry {
    std::string name;
    uint16_t method = 0, flags = 0;
    uint64_t csz = 0, usz = 0, lho = 0;
    uint64_t data_off = 0;
    bool mem_loaded = false;
    std::vector<uint8_t> mem;
};

struct Zip {
    FILE* f = nullptr;
    uint64_t fsz = 0;
    std::vector<Entry> entries;
    std::unordered_map<std::string, size_t> byname;

    ~Zip() { if (f) fclose(f); }

    void read_at(uint64_t off, uint8_t* dst, size_t n) {
        if (off + n > fsz) throw Err("zip: read past end of file");
        if (n == 0) return;
        if (fseeko(f, (off_t)off, SEEK_SET) != 0) throw Err("zip: seek failed");
        if (fread(dst, 1, n, f) != n) throw Err("zip: read failed");
    }

    void open(const char* path) {
        f = fopen(path, "rb");
        if (!f) throw Err(std::string("cannot open ") + path);
        if (fseeko(f, 0, SEEK_END) != 0) throw Err("zip: seek failed");
        fsz = (uint64_t)ftello(f);
        if (fsz < 22) throw Err("zip: file too small for EOCD");
        // locate EOCD scanning backwards
        size_t win = (size_t)std::min<uint64_t>(fsz, 22 + 65535);
        std::vector<uint8_t> tail(win);
        read_at(fsz - win, tail.data(), win);
        int64_t eocd = -1;
        for (int64_t i = (int64_t)win - 22; i >= 0; i--) {
            if (rd32(tail.data() + i) == 0x06054b50u &&
                (uint64_t)(i + 22 + rd16(tail.data() + i + 20)) == win) { eocd = i; break; }
        }
        if (eocd < 0) throw Err("zip: EOCD not found");
        const uint8_t* ep = tail.data() + eocd;
        uint64_t eocd_abs = fsz - win + (uint64_t)eocd;
        uint16_t disk = rd16(ep + 4), cd_disk = rd16(ep + 6);
        uint64_t count = rd16(ep + 10), cd_size = rd32(ep + 12), cd_off = rd32(ep + 16);
        if (disk != 0 || cd_disk != 0) throw Err("zip: multi-disk archives unsupported");
        bool sat = (count == 0xFFFF) || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu;
        if (sat) {
            // zip64 locator immediately before EOCD
            if (eocd_abs < 20) throw Err("zip: missing zip64 locator");
            uint8_t loc[20];
            read_at(eocd_abs - 20, loc, 20);
            if (rd32(loc) != 0x07064b50u) throw Err("zip: missing zip64 locator");
            uint64_t z64 = rd64(loc + 8);
            uint8_t zh[56];
            read_at(z64, zh, 56);
            if (rd32(zh) != 0x06064b50u) throw Err("zip: bad zip64 EOCD");
            if (rd32(zh + 16) != 0 || rd32(zh + 20) != 0) throw Err("zip: multi-disk archives unsupported");
            count = rd64(zh + 32);
            cd_size = rd64(zh + 40);
            cd_off = rd64(zh + 48);
        }
        if (cd_off + cd_size > fsz) throw Err("zip: central directory out of bounds");
        if (count > (1u << 24)) throw Err("zip: implausible entry count");
        std::vector<uint8_t> cd(cd_size);
        read_at(cd_off, cd.data(), cd.size());
        size_t p = 0;
        for (uint64_t i = 0; i < count; i++) {
            if (p + 46 > cd.size() || rd32(cd.data() + p) != 0x02014b50u)
                throw Err("zip: corrupt central directory");
            Entry e;
            e.flags = rd16(cd.data() + p + 8);
            e.method = rd16(cd.data() + p + 10);
            uint64_t usz = rd32(cd.data() + p + 24), csz = rd32(cd.data() + p + 20), lho = rd32(cd.data() + p + 42);
            uint16_t nl = rd16(cd.data() + p + 28), ml = rd16(cd.data() + p + 30), kl = rd16(cd.data() + p + 32);
            uint16_t dstart = rd16(cd.data() + p + 34);
            if (p + 46 + nl + ml + kl > cd.size()) throw Err("zip: corrupt central directory");
            e.name.assign((const char*)cd.data() + p + 46, nl);
            if (dstart != 0 && dstart != 0xFFFF) throw Err("zip: multi-disk archives unsupported");
            // zip64 extra field
            if (usz == 0xFFFFFFFFu || csz == 0xFFFFFFFFu || lho == 0xFFFFFFFFu || dstart == 0xFFFF) {
                size_t q = p + 46 + nl, qend = q + ml;
                while (q + 4 <= qend) {
                    uint16_t id = rd16(cd.data() + q), ds = rd16(cd.data() + q + 2);
                    q += 4;
                    if (q + ds > qend) throw Err("zip: corrupt extra field");
                    if (id == 0x0001) {
                        size_t r = q;
                        if (usz == 0xFFFFFFFFu) { if (r + 8 > q + ds) throw Err("zip: corrupt zip64 extra"); usz = rd64(cd.data() + r); r += 8; }
                        if (csz == 0xFFFFFFFFu) { if (r + 8 > q + ds) throw Err("zip: corrupt zip64 extra"); csz = rd64(cd.data() + r); r += 8; }
                        if (lho == 0xFFFFFFFFu) { if (r + 8 > q + ds) throw Err("zip: corrupt zip64 extra"); lho = rd64(cd.data() + r); r += 8; }
                    }
                    q += ds;
                }
            }
            e.usz = usz; e.csz = csz; e.lho = lho;
            byname[e.name] = entries.size();
            entries.push_back(std::move(e));
            p += 46 + nl + ml + kl;
        }
    }

    const Entry* find(const std::string& name) const {
        auto it = byname.find(name);
        return it == byname.end() ? nullptr : &entries[it->second];
    }

    uint64_t data_offset(const Entry& e) {
        if (e.flags & 1) throw Err("zip: encrypted entry unsupported");
        if (e.flags & (1 << 13)) throw Err("zip: masked central-directory entry unsupported");
        uint8_t lh[30];
        read_at(e.lho, lh, 30);
        if (rd32(lh) != 0x04034b50u) throw Err("zip: bad local file header");
        uint16_t nl = rd16(lh + 26), ml = rd16(lh + 28);
        uint64_t off = e.lho + 30 + nl + ml;
        if (off + e.csz > fsz) throw Err("zip: entry data out of bounds");
        return off;
    }

    // Load a full entry into memory (inflating when method 8).
    const std::vector<uint8_t>& load(Entry& e, uint64_t cap = (uint64_t)1 << 30) {
        if (e.mem_loaded) return e.mem;
        if (e.usz > cap) throw Err("zip: entry too large");
        uint64_t off = data_offset(e);
        if (e.method == 0) {
            if (e.csz != e.usz) throw Err("zip: stored entry size mismatch");
            e.mem.resize((size_t)e.usz);
            read_at(off, e.mem.data(), e.mem.size());
        } else if (e.method == 8) {
            std::vector<uint8_t> c((size_t)e.csz);
            read_at(off, c.data(), c.size());
            e.mem = inflate(c.data(), c.size(), (size_t)e.usz);
        } else throw Err("zip: unsupported compression method");
        e.mem_loaded = true;
        return e.mem;
    }
};

// ----------------------------------------------------------------- pickle --
struct DType { const char* tok; int size; };
static const DType* dtype_for(const std::string& cls) {
    static const std::pair<const char*, DType> tab[] = {
        {"FloatStorage", {"f32", 4}}, {"DoubleStorage", {"f64", 8}},
        {"HalfStorage", {"f16", 2}}, {"BFloat16Storage", {"bf16", 2}},
        {"LongStorage", {"i64", 8}}, {"IntStorage", {"i32", 4}},
        {"ShortStorage", {"i16", 2}}, {"CharStorage", {"i8", 1}},
        {"ByteStorage", {"u8", 1}}, {"BoolStorage", {"bool", 1}},
    };
    for (auto& kv : tab) if (cls == kv.first) return &kv.second;
    return nullptr;
}

struct Value {
    enum K : uint8_t { NONE, TF, INT, BIGINT, FLOAT, STR, BYTES, LIST, TUPLE, DICT, SET,
                       GLOBALREF, STORAGE, TENSOR, OPAQUE } k = NONE;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;                     // STR/BYTES bytes, GLOBALREF attr name
    std::string s2;                    // GLOBALREF module
    std::vector<Value*> it;            // LIST/TUPLE/SET
    std::vector<std::pair<Value*, Value*>> kv;  // DICT (insertion order)
    // STORAGE
    std::string key;
    const DType* dt = nullptr;
    // TENSOR
    Value* storage = nullptr;
    int64_t off = 0;
    std::vector<int64_t> shape, stride;
    uint8_t mark = 0;                  // walker state: 1 = on path, 2 = done
};

struct Unpickler {
    const uint8_t* p; size_t n; size_t pos = 0;
    std::deque<Value> arena;
    std::vector<Value*> memo;
    std::vector<Value*> st;            // nullptr = MARK sentinel

    Value* mk(Value::K k) { arena.emplace_back(); arena.back().k = k; return &arena.back(); }

    uint8_t u8() { if (pos >= n) throw Err("pickle: truncated stream"); return p[pos++]; }
    void need(size_t k) { if (k > n - pos) throw Err("pickle: declared length exceeds stream"); }
    uint32_t le32() { need(4); uint32_t v = rd32(p + pos); pos += 4; return v; }
    uint64_t le64() { need(8); uint64_t v = rd64(p + pos); pos += 8; return v; }

    std::string line() {
        const uint8_t* q = (const uint8_t*)memchr(p + pos, '\n', n - pos);
        if (!q) throw Err("pickle: truncated line");
        std::string s((const char*)p + pos, (size_t)(q - (p + pos)));
        pos = (size_t)(q - p) + 1;
        return s;
    }

    Value* pop() {
        if (st.empty()) throw Err("pickle: stack underflow");
        Value* v = st.back(); st.pop_back();
        if (!v) throw Err("pickle: unexpected mark");
        return v;
    }
    std::vector<Value*> pop_mark() {
        size_t i = st.size();
        while (i > 0 && st[i - 1]) i--;
        if (i == 0) throw Err("pickle: mark not found");
        std::vector<Value*> out(st.begin() + (ptrdiff_t)i, st.end());
        st.resize(i - 1);
        return out;
    }
    Value* mk_str(const uint8_t* d, size_t len, Value::K k = Value::STR) {
        Value* v = mk(k); v->s.assign((const char*)d, len); return v;
    }

    Value* parse_long_bytes(const uint8_t* d, size_t len) {
        // two's complement little-endian
        if (len == 0) { Value* v = mk(Value::INT); v->i = 0; return v; }
        if (len > 8) return mk(Value::BIGINT);
        uint64_t x = 0;
        for (size_t i = 0; i < len; i++) x |= (uint64_t)d[i] << (8 * i);
        if (d[len - 1] & 0x80) {  // negative
            if (len == 8) { Value* v = mk(Value::INT); v->i = (int64_t)x; return v; }
            x |= ~((uint64_t)1 << (8 * len)) + 1;  // sign-extend
            Value* v = mk(Value::INT); v->i = (int64_t)x; return v;
        }
        if (len == 8 && (x >> 63)) return mk(Value::BIGINT);
        Value* v = mk(Value::INT); v->i = (int64_t)x; return v;
    }

    Value* parse_int_line(const std::string& s, bool is_long) {
        std::string t = s;
        if (is_long && !t.empty() && t.back() == 'L') t.pop_back();
        if (t == "01") { Value* v = mk(Value::TF); v->b = true; return v; }
        if (t == "00") { Value* v = mk(Value::TF); v->b = false; return v; }
        if (t.size() > 19) return mk(Value::BIGINT);
        char* end = nullptr;
        errno = 0;
        long long x = strtoll(t.c_str(), &end, 10);
        if (errno || !end || *end != 0 || end == t.c_str()) {
            if (t.size() > 18) return mk(Value::BIGINT);
            throw Err("pickle: bad integer literal");
        }
        Value* v = mk(Value::INT); v->i = x; return v;
    }

    Value* parse_quoted(const std::string& s) {
        if (s.size() < 2 || (s[0] != '\'' && s[0] != '"')) throw Err("pickle: bad STRING literal");
        char quote = s[0];
        if (s.back() != quote) throw Err("pickle: bad STRING literal");
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); i++) {
            char c = s[i];
            if (c != '\\') { out.push_back(c); continue; }
            if (++i + 1 > s.size()) throw Err("pickle: bad escape");
            char e = s[i];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '\'': out.push_back('\''); break;
                case '"': out.push_back('"'); break;
                case 'x': {
                    if (i + 2 >= s.size()) throw Err("pickle: bad \\x escape");
                    auto hx = [&](char h) -> int {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                        throw Err("pickle: bad \\x escape");
                    };
                    out.push_back((char)(hx(s[i + 1]) * 16 + hx(s[i + 2])));
                    i += 2; break;
                }
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0', cnt = 1;
                        while (cnt < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
                            v = v * 8 + (s[++i] - '0'); cnt++;
                        }
                        out.push_back((char)v);
                    } else out.push_back(e);
            }
        }
        Value* v = mk(Value::STR); v->s = std::move(out); return v;
    }

    void memo_put(uint64_t idx) {
        if (idx >= (1u << 22)) throw Err("pickle: memo index too large");
        if (st.empty() || !st.back()) throw Err("pickle: memo of mark");
        if (idx >= memo.size()) memo.resize((size_t)idx + 1, nullptr);
        memo[(size_t)idx] = st.back();
    }
    void memo_get(uint64_t idx) {
        if (idx >= memo.size() || !memo[(size_t)idx]) throw Err("pickle: bad memo reference");
        st.push_back(memo[(size_t)idx]);
    }

    Value* do_reduce(Value* callable, std::vector<Value*>& args) {
        if (callable->k != Value::GLOBALREF) return mk(Value::OPAQUE);
        const std::string& mod = callable->s2;
        const std::string& nm = callable->s;
        // Refuse code-execution primitives outright.
        static const char* bad_mods[] = {"os","posix","nt","subprocess","sys","shutil","ctypes",
            "socket","signal","code","runpy","importlib","pty","multiprocessing","builtins","__builtin__"};
        static const char* bad_names[] = {"system","popen","spawnl","spawnv","execl","execv","execve",
            "execlp","execvp","eval","exec","__import__","compile","fork","kill","remove","unlink"};
        for (auto m : bad_mods) if (mod == m) throw Err("pickle: refusing dangerous callable " + mod + "." + nm);
        for (auto m : bad_names) if (nm == m) throw Err("pickle: refusing dangerous callable " + mod + "." + nm);

        auto arg_int = [&](Value* v, const char* what) -> int64_t {
            if (v->k == Value::INT) return v->i;
            if (v->k == Value::TF) return v->b ? 1 : 0;
            throw Err(std::string("pickle: bad ") + what);
        };
        auto int_seq = [&](Value* v, const char* what) -> std::vector<int64_t> {
            if (v->k != Value::TUPLE && v->k != Value::LIST) throw Err(std::string("pickle: bad ") + what);
            std::vector<int64_t> out;
            out.reserve(v->it.size());
            for (Value* e : v->it) out.push_back(arg_int(e, what));
            return out;
        };
        auto build_tensor = [&](size_t nargs) -> Value* {
            if (args.size() < nargs) throw Err("pickle: _rebuild_tensor: too few arguments");
            Value* sto = args[0];
            if (sto->k != Value::STORAGE) throw Err("pickle: _rebuild_tensor: bad storage argument");
            if (!sto->dt) {
                fprintf(stderr, "convert: skipping tensor with unknown storage class\n");
                return mk(Value::OPAQUE);
            }
            int64_t off = arg_int(args[1], "storage_offset");
            std::vector<int64_t> shape = int_seq(args[2], "shape");
            std::vector<int64_t> stride = int_seq(args[3], "stride");
            if (shape.size() != stride.size()) throw Err("pickle: shape/stride rank mismatch");
            if (off < 0) throw Err("pickle: negative storage_offset");
            for (int64_t d : shape) if (d < 0) throw Err("pickle: negative shape");
            for (int64_t s : stride) if (s < 0) throw Err("pickle: negative stride");
            Value* v = mk(Value::TENSOR);
            v->storage = sto; v->off = off;
            v->shape = std::move(shape); v->stride = std::move(stride);
            return v;
        };
        auto pairs_to_dict = [&](Value* src) -> Value* {
            Value* d = mk(Value::DICT);
            if (!src) return d;
            if (src->k != Value::LIST && src->k != Value::TUPLE) throw Err("pickle: bad OrderedDict argument");
            for (Value* pr : src->it) {
                if ((pr->k != Value::LIST && pr->k != Value::TUPLE) || pr->it.size() != 2)
                    throw Err("pickle: bad OrderedDict pair");
                d->kv.emplace_back(pr->it[0], pr->it[1]);
            }
            return d;
        };

        if (nm == "_rebuild_tensor_v2" || nm == "_rebuild_tensor_v3") return build_tensor(6);
        if (nm == "_rebuild_tensor") return build_tensor(4);
        if (nm == "_rebuild_parameter" || nm == "_rebuild_parameter_with_state") {
            if (args.empty()) throw Err("pickle: _rebuild_parameter: no arguments");
            return args[0]->k == Value::TENSOR ? args[0] : mk(Value::OPAQUE);
        }
        if (mod == "collections" && (nm == "OrderedDict" || nm == "defaultdict")) {
            Value* src = nullptr;
            if (nm == "OrderedDict" && !args.empty()) src = args[0];
            if (nm == "defaultdict" && args.size() >= 2) src = args[1];
            return pairs_to_dict(src);
        }
        if (nm == "Size" && (mod == "torch" || mod == "torch.types")) {
            Value* t = mk(Value::TUPLE);
            if (!args.empty() && (args[0]->k == Value::LIST || args[0]->k == Value::TUPLE)) t->it = args[0]->it;
            return t;
        }
        if (mod == "builtins" || mod == "__builtin__") {  // unreachable (refused above), kept for clarity
            return mk(Value::OPAQUE);
        }
        return mk(Value::OPAQUE);
    }

    Value* run() {
        for (;;) {
            if (pos >= n) throw Err("pickle: missing STOP");
            uint8_t op = u8();
            switch (op) {
                case 0x80: {  // PROTO
                    uint8_t proto = u8();
                    if (proto > 5) throw Err("pickle: unsupported protocol");
                    break;
                }
                case 0x95: (void)le64(); break;             // FRAME: skip length, data follows inline
                case 0x2e: {                                // STOP
                    Value* v = pop();
                    return v;
                }
                case 0x28: st.push_back(nullptr); break;    // MARK
                case 0x31: (void)pop_mark(); break;         // POP_MARK
                case 0x30: (void)pop(); break;              // POP
                case 0x32: {                                // DUP
                    if (st.empty() || !st.back()) throw Err("pickle: stack underflow");
                    st.push_back(st.back()); break;
                }
                case 0x4e: st.push_back(mk(Value::NONE)); break;  // NONE
                case 0x88: { Value* v = mk(Value::TF); v->b = true; st.push_back(v); break; }
                case 0x89: { Value* v = mk(Value::TF); v->b = false; st.push_back(v); break; }
                case 0x49: st.push_back(parse_int_line(line(), false)); break;   // INT
                case 0x4c: st.push_back(parse_int_line(line(), true)); break;    // LONG
                case 0x4a: {                              // BININT
                    uint32_t x = le32();
                    Value* v = mk(Value::INT); v->i = (int32_t)x; st.push_back(v); break;
                }
                case 0x4b: { Value* v = mk(Value::INT); v->i = u8(); st.push_back(v); break; }
                case 0x4d: { need(2); Value* v = mk(Value::INT); v->i = rd16(p + pos); pos += 2; st.push_back(v); break; }
                case 0x8a: {                              // LONG1
                    size_t len = u8(); need(len);
                    st.push_back(parse_long_bytes(p + pos, len)); pos += len; break;
                }
                case 0x8b: {                              // LONG4
                    uint64_t len = le32();
                    if (len > (1u << 20)) throw Err("pickle: LONG4 too large");
                    need((size_t)len);
                    st.push_back(parse_long_bytes(p + pos, (size_t)len)); pos += (size_t)len; break;
                }
                case 0x46: {                              // FLOAT
                    std::string s = line();
                    Value* v = mk(Value::FLOAT); v->f = strtod(s.c_str(), nullptr); st.push_back(v); break;
                }
                case 0x47: {                              // BINFLOAT: 8-byte big-endian double
                    need(8);
                    uint64_t x = 0;
                    for (int i = 0; i < 8; i++) x = (x << 8) | p[pos + i];
                    pos += 8;
                    Value* v = mk(Value::FLOAT);
                    double d; memcpy(&d, &x, 8); v->f = d;
                    st.push_back(v); break;
                }
                case 0x53: st.push_back(parse_quoted(line())); break;            // STRING
                case 0x56: {                              // UNICODE (raw-unicode-escape)
                    std::string s = line();
                    std::string out;
                    for (size_t i = 0; i < s.size(); i++) {
                        if (s[i] == '\\' && i + 5 <= s.size() && s[i + 1] == 'u') {
                            auto hx = [&](char h) -> int {
                                if (h >= '0' && h <= '9') return h - '0';
                                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                                throw Err("pickle: bad \\u escape");
                            };
                            uint32_t cp = (uint32_t)(hx(s[i+2]) << 12 | hx(s[i+3]) << 8 | hx(s[i+4]) << 4 | hx(s[i+5]));
                            i += 5;
                            if (cp < 0x80) out.push_back((char)cp);
                            else if (cp < 0x800) {
                                out.push_back((char)(0xC0 | (cp >> 6)));
                                out.push_back((char)(0x80 | (cp & 63)));
                            } else {
                                out.push_back((char)(0xE0 | (cp >> 12)));
                                out.push_back((char)(0x80 | ((cp >> 6) & 63)));
                                out.push_back((char)(0x80 | (cp & 63)));
                            }
                        } else out.push_back(s[i]);
                    }
                    Value* v = mk(Value::STR); v->s = std::move(out); st.push_back(v); break;
                }
                case 0x54: {                              // BINSTRING
                    int32_t len = (int32_t)le32();
                    if (len < 0) throw Err("pickle: negative BINSTRING length");
                    need((size_t)len);
                    st.push_back(mk_str(p + pos, (size_t)len)); pos += (size_t)len; break;
                }
                case 0x55: {                              // SHORT_BINSTRING
                    size_t len = u8(); need(len);
                    st.push_back(mk_str(p + pos, len)); pos += len; break;
                }
                case 0x58: {                              // BINUNICODE
                    uint64_t len = le32(); need((size_t)len);
                    st.push_back(mk_str(p + pos, (size_t)len)); pos += (size_t)len; break;
                }
                case 0x8c: {                              // SHORT_BINUNICODE
                    size_t len = u8(); need(len);
                    st.push_back(mk_str(p + pos, len)); pos += len; break;
                }
                case 0x8d: {                              // BINUNICODE8
                    uint64_t len = le64();
                    if (len > (uint64_t)1 << 31) throw Err("pickle: BINUNICODE8 too large");
                    need((size_t)len);
                    st.push_back(mk_str(p + pos, (size_t)len)); pos += (size_t)len; break;
                }
                case 0x42: {                              // BINBYTES
                    uint64_t len = le32(); need((size_t)len);
                    st.push_back(mk_str(p + pos, (size_t)len, Value::BYTES)); pos += (size_t)len; break;
                }
                case 0x43: {                              // SHORT_BINBYTES
                    size_t len = u8(); need(len);
                    st.push_back(mk_str(p + pos, len, Value::BYTES)); pos += len; break;
                }
                case 0x8e: {                              // BINBYTES8
                    uint64_t len = le64();
                    if (len > (uint64_t)1 << 31) throw Err("pickle: BINBYTES8 too large");
                    need((size_t)len);
                    st.push_back(mk_str(p + pos, (size_t)len, Value::BYTES)); pos += (size_t)len; break;
                }
                case 0x96: {                              // BYTEARRAY8
                    uint64_t len = le64();
                    if (len > (uint64_t)1 << 31) throw Err("pickle: BYTEARRAY8 too large");
                    need((size_t)len);
                    st.push_back(mk_str(p + pos, (size_t)len, Value::BYTES)); pos += (size_t)len; break;
                }
                case 0x67: {                              // GET
                    std::string s = line();
                    char* end = nullptr; unsigned long long idx = strtoull(s.c_str(), &end, 10);
                    if (!end || *end != 0) throw Err("pickle: bad GET index");
                    memo_get(idx); break;
                }
                case 0x68: memo_get(u8()); break;           // BINGET
                case 0x6a: memo_get(le32()); break;         // LONG_BINGET
                case 0x70: {                              // PUT
                    std::string s = line();
                    char* end = nullptr; unsigned long long idx = strtoull(s.c_str(), &end, 10);
                    if (!end || *end != 0) throw Err("pickle: bad PUT index");
                    memo_put(idx); break;
                }
                case 0x71: memo_put(u8()); break;           // BINPUT
                case 0x72: memo_put(le32()); break;         // LONG_BINPUT
                case 0x94: memo_put(memo.size()); break;    // MEMOIZE
                case 0x5d: st.push_back(mk(Value::LIST)); break;   // EMPTY_LIST
                case 0x29: st.push_back(mk(Value::TUPLE)); break;  // EMPTY_TUPLE
                case 0x7d: st.push_back(mk(Value::DICT)); break;   // EMPTY_DICT
                case 0x8f: st.push_back(mk(Value::SET)); break;    // EMPTY_SET
                case 0x61: {                              // APPEND
                    Value* item = pop();
                    if (st.empty() || !st.back()) throw Err("pickle: stack underflow");
                    Value* c = st.back();
                    if (c->k != Value::LIST && c->k != Value::SET) throw Err("pickle: APPEND on non-list");
                    c->it.push_back(item); break;
                }
                case 0x65: {                              // APPENDS
                    auto items = pop_mark();
                    if (st.empty() || !st.back()) throw Err("pickle: stack underflow");
                    Value* c = st.back();
                    if (c->k != Value::LIST && c->k != Value::SET) throw Err("pickle: APPENDS on non-list");
                    for (Value* v : items) c->it.push_back(v);
                    break;
                }
                case 0x90: {                              // ADDITEMS
                    auto items = pop_mark();
                    if (st.empty() || !st.back()) throw Err("pickle: stack underflow");
                    Value* c = st.back();
                    if (c->k != Value::SET) throw Err("pickle: ADDITEMS on non-set");
                    for (Value* v : items) c->it.push_back(v);
                    break;
                }
                case 0x6c: {                              // LIST
                    auto items = pop_mark();
                    Value* v = mk(Value::LIST); v->it = std::move(items); st.push_back(v); break;
                }
                case 0x74: {                              // TUPLE
                    auto items = pop_mark();
                    Value* v = mk(Value::TUPLE); v->it = std::move(items); st.push_back(v); break;
                }
                case 0x91: {                              // FROZENSET
                    auto items = pop_mark();
                    Value* v = mk(Value::SET); v->it = std::move(items); st.push_back(v); break;
                }
                case 0x85: {                              // TUPLE1
                    Value* a = pop();
                    Value* v = mk(Value::TUPLE); v->it = {a}; st.push_back(v); break;
                }
                case 0x86: {                              // TUPLE2
                    Value* b = pop(); Value* a = pop();
                    Value* v = mk(Value::TUPLE); v->it = {a, b}; st.push_back(v); break;
                }
                case 0x87: {                              // TUPLE3
                    Value* c = pop(); Value* b = pop(); Value* a = pop();
                    Value* v = mk(Value::TUPLE); v->it = {a, b, c}; st.push_back(v); break;
                }
                case 0x64: {                              // DICT
                    auto items = pop_mark();
                    if (items.size() % 2) throw Err("pickle: odd DICT slice");
                    Value* v = mk(Value::DICT);
                    for (size_t i = 0; i < items.size(); i += 2) v->kv.emplace_back(items[i], items[i + 1]);
                    st.push_back(v); break;
                }
                case 0x73: {                              // SETITEM
                    Value* val = pop(); Value* key = pop();
                    if (st.empty() || !st.back() || st.back()->k != Value::DICT)
                        throw Err("pickle: SETITEM on non-dict");
                    st.back()->kv.emplace_back(key, val); break;
                }
                case 0x75: {                              // SETITEMS
                    auto items = pop_mark();
                    if (items.size() % 2) throw Err("pickle: odd SETITEMS slice");
                    if (st.empty() || !st.back() || st.back()->k != Value::DICT)
                        throw Err("pickle: SETITEMS on non-dict");
                    Value* d = st.back();
                    for (size_t i = 0; i < items.size(); i += 2) d->kv.emplace_back(items[i], items[i + 1]);
                    break;
                }
                case 0x63: {                              // GLOBAL
                    std::string mod = line(), nm = line();
                    Value* v = mk(Value::GLOBALREF); v->s2 = std::move(mod); v->s = std::move(nm);
                    st.push_back(v); break;
                }
                case 0x93: {                              // STACK_GLOBAL
                    Value* nm = pop(); Value* mod = pop();
                    if (nm->k != Value::STR || mod->k != Value::STR) throw Err("pickle: bad STACK_GLOBAL");
                    Value* v = mk(Value::GLOBALREF); v->s2 = mod->s; v->s = nm->s;
                    st.push_back(v); break;
                }
                case 0x52: {                              // REDUCE
                    Value* argt = pop(); Value* callable = pop();
                    if (argt->k != Value::TUPLE && argt->k != Value::LIST)
                        throw Err("pickle: REDUCE argument is not a tuple");
                    st.push_back(do_reduce(callable, argt->it)); break;
                }
                case 0x62: {                              // BUILD
                    Value* state = pop();
                    if (st.empty() || !st.back()) throw Err("pickle: stack underflow");
                    Value* obj = st.back();
                    if (obj->k == Value::DICT && state->k == Value::DICT) {
                        for (auto& pr : state->kv) obj->kv.push_back(pr);
                    } else if (obj->k != Value::OPAQUE && obj->k != Value::TENSOR) {
                        obj->k = Value::OPAQUE;
                    }
                    break;
                }
                case 0x69: {                              // INST
                    (void)line(); (void)line();
                    (void)pop_mark();
                    st.push_back(mk(Value::OPAQUE)); break;
                }
                case 0x6f: {                              // OBJ
                    (void)pop_mark();
                    st.push_back(mk(Value::OPAQUE)); break;
                }
                case 0x81: {                              // NEWOBJ
                    (void)pop(); (void)pop();
                    st.push_back(mk(Value::OPAQUE)); break;
                }
                case 0x92: {                              // NEWOBJ_EX
                    (void)pop(); (void)pop(); (void)pop();
                    st.push_back(mk(Value::OPAQUE)); break;
                }
                case 0x82: (void)u8(); st.push_back(mk(Value::OPAQUE)); break;   // EXT1
                case 0x83: need(2); pos += 2; st.push_back(mk(Value::OPAQUE)); break;  // EXT2
                case 0x84: (void)le32(); st.push_back(mk(Value::OPAQUE)); break; // EXT4
                case 0x50: {                              // PERSID
                    (void)line();
                    st.push_back(mk(Value::OPAQUE)); break;
                }
                case 0x51: {                              // BINPERSID
                    Value* pid = pop();
                    st.push_back(persistent_load(pid)); break;
                }
                case 0x97: throw Err("pickle: out-of-band buffers unsupported");  // NEXT_BUFFER
                case 0x98: throw Err("pickle: out-of-band buffers unsupported");  // READONLY_BUFFER
                default: throw Err("pickle: unknown opcode 0x" +
                    std::string(1, "0123456789abcdef"[op >> 4]) + std::string(1, "0123456789abcdef"[op & 15]));
            }
        }
    }

    Value* persistent_load(Value* pid) {
        if (pid->k != Value::TUPLE || pid->it.size() < 4) return mk(Value::OPAQUE);
        Value* tag = pid->it[0];
        if (tag->k != Value::STR || tag->s != "storage") return mk(Value::OPAQUE);
        Value* cls = pid->it[1];
        if (cls->k != Value::GLOBALREF) return mk(Value::OPAQUE);
        Value* keyv = pid->it[2];
        Value* v = mk(Value::STORAGE);
        v->dt = dtype_for(cls->s);
        if (keyv->k == Value::STR) v->key = keyv->s;
        else if (keyv->k == Value::INT) v->key = std::to_string(keyv->i);
        else throw Err("pickle: bad storage key");
        return v;
    }
};

// --------------------------------------------------------------- emit ------
struct Rec {
    std::string dtype;
    int is = 0;
    std::vector<int64_t> shape, stride;
    std::string key;
    int64_t off = 0;
    uint64_t nbytes = 0;
};

struct Src {
    Zip& z; const Entry* e; uint64_t base = 0;
    std::vector<uint8_t> win; uint64_t w0 = 0, wn = 0;
    static const uint64_t WIN = 4u << 20;

    Src(Zip& zz, const Entry* ee) : z(zz), e(ee) {}
    uint64_t size() const { return e->usz; }
    void ensure() {
        if (e->method == 0) { base = z.data_offset(*e); }
        else if (e->method == 8) { z.load(*const_cast<Entry*>(e)); }
        else throw Err("zip: unsupported compression method for storage");
    }
    void read(uint64_t off, uint8_t* dst, size_t n) {
        if (off + n > e->usz) throw Err("storage: read out of bounds");
        if (e->method == 8) { memcpy(dst, e->mem.data() + off, n); return; }
        if (n >= (1u << 20)) { z.read_at(base + off, dst, n); return; }
        if (off < w0 || off + n > w0 + wn) {
            w0 = off & ~(WIN - 1);
            wn = std::min<uint64_t>(WIN, e->usz - w0);
            if (off < w0 || off + n > w0 + wn) { z.read_at(base + off, dst, n); return; }
            win.resize((size_t)wn);
            z.read_at(base + w0, win.data(), (size_t)wn);
        }
        memcpy(dst, win.data() + (off - w0), n);
    }
};

struct Emitter {
    Zip& z; std::string prefix; bool big;
    std::string outdir;
    bool rvc = false;
    std::map<std::string, Rec> recs;

    static std::string binname(std::string path) {
        if (path.empty()) return "_";
        std::string out;
        for (char c : path) { if (c == '/') out += "__"; else out += c; }
        return out;
    }

    static void byteswap(uint8_t* d, size_t n, int is) {
        if (is == 2) for (size_t i = 0; i + 2 <= n; i += 2) std::swap(d[i], d[i + 1]);
        else if (is == 4) for (size_t i = 0; i + 4 <= n; i += 4) {
            std::swap(d[i], d[i + 3]); std::swap(d[i + 1], d[i + 2]);
        } else if (is == 8) for (size_t i = 0; i + 8 <= n; i += 8) {
            std::swap(d[i], d[i + 7]); std::swap(d[i + 1], d[i + 6]);
            std::swap(d[i + 2], d[i + 5]); std::swap(d[i + 3], d[i + 4]);
        }
    }

    static uint32_t f16_to_f32_bits(uint16_t h) {
        uint32_t s = (uint32_t)(h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        if (e == 0) {
            if (m == 0) return s << 31;
            int ex = -14;
            while (!(m & 0x400)) { m <<= 1; ex--; }
            m &= 0x3FF;
            return (s << 31) | ((uint32_t)(ex + 127) << 23) | (m << 13);
        }
        if (e == 31) return (s << 31) | 0x7F800000u | (m << 13);
        return (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
    }

    void emit(const std::string& path, Value* t) {
        const DType* dt = t->storage->dt;
        int rs = dt->size;                     // source element size
        bool upcast = rvc && strcmp(dt->tok, "f16") == 0;
        int ws = upcast ? 4 : rs;              // written element size
        const char* out_tok = upcast ? "f32" : dt->tok;
        size_t nd = t->shape.size();
        unsigned __int128 total = 1;
        for (size_t d = 0; d < nd; d++) {
            total *= (uint64_t)t->shape[d];
            if (total > (unsigned __int128)1 << 62) throw Err("tensor: shape product too large");
        }
        Rec r;
        r.dtype = out_tok; r.is = ws; r.shape = t->shape; r.stride = t->stride;
        r.key = t->storage->key; r.off = t->off;
        r.nbytes = (uint64_t)total * (uint64_t)ws;

        const Entry* e = z.find(prefix + "data/" + r.key);
        if (!e) throw Err("storage entry missing for key " + r.key);
        Src src(z, e); src.ensure();

        if ((uint64_t)total > 0) {
            unsigned __int128 maxoff = (uint64_t)t->off;
            for (size_t d = 0; d < nd; d++)
                if (t->shape[d] > 0) maxoff += (unsigned __int128)(uint64_t)(t->shape[d] - 1) * (uint64_t)t->stride[d];
            unsigned __int128 need = (maxoff + 1) * (unsigned)rs;
            if (need > (unsigned __int128)src.size()) throw Err("tensor extends past end of storage");
        }

        std::string bpath = outdir + "/tensors/" + binname(path) + ".bin";
        FILE* out = fopen(bpath.c_str(), "wb");
        if (!out) throw Err("cannot write " + bpath);
        if ((uint64_t)total > 0) {
            // innermost contiguous run
            size_t k = 0;
            unsigned __int128 expect = 1;
            for (size_t dd = nd; dd-- > 0;) {
                if (t->shape[dd] <= 1) { k++; continue; }
                if ((uint64_t)t->stride[dd] != (uint64_t)expect) break;
                expect *= (uint64_t)t->shape[dd];
                k++;
            }
            uint64_t run_elems = 1;
            for (size_t d = nd - k; d < nd; d++) run_elems *= (uint64_t)t->shape[d];
            uint64_t run_bytes = run_elems * (uint64_t)rs;
            size_t outer_nd = nd - k;
            uint64_t outer_total = 1;
            for (size_t d = 0; d < outer_nd; d++) outer_total *= (uint64_t)t->shape[d];
            std::vector<int64_t> idx(outer_nd, 0);
            std::vector<uint8_t> buf, obuf;
            const uint64_t CHUNK = 1u << 20;
            for (uint64_t cnt = 0; cnt < outer_total; cnt++) {
                uint64_t base = (uint64_t)t->off;
                for (size_t d = 0; d < outer_nd; d++) base += (uint64_t)idx[d] * (uint64_t)t->stride[d];
                uint64_t byte_off = base * (uint64_t)rs;
                uint64_t left = run_bytes, at = byte_off;
                while (left > 0) {
                    size_t chunk = (size_t)std::min<uint64_t>(left, CHUNK);
                    buf.resize(chunk);
                    src.read(at, buf.data(), chunk);
                    if (big && rs > 1) byteswap(buf.data(), chunk, rs);
                    const uint8_t* wsrc = buf.data();
                    size_t wlen = chunk;
                    if (upcast) {
                        size_t ne = chunk / 2;
                        obuf.resize(ne * 4);
                        for (size_t i = 0; i < ne; i++) {
                            uint32_t b32 = f16_to_f32_bits((uint16_t)(buf[2 * i] | (buf[2 * i + 1] << 8)));
                            obuf[4 * i] = (uint8_t)b32;
                            obuf[4 * i + 1] = (uint8_t)(b32 >> 8);
                            obuf[4 * i + 2] = (uint8_t)(b32 >> 16);
                            obuf[4 * i + 3] = (uint8_t)(b32 >> 24);
                        }
                        wsrc = obuf.data();
                        wlen = ne * 4;
                    }
                    if (fwrite(wsrc, 1, wlen, out) != wlen) {
                        fclose(out); throw Err("write failed: " + bpath);
                    }
                    left -= chunk; at += chunk;
                }
                for (size_t d = outer_nd; d-- > 0;) {
                    if (++idx[d] < t->shape[d]) break;
                    idx[d] = 0;
                }
            }
        }
        if (fclose(out) != 0) throw Err("write failed: " + bpath);
        recs[path] = std::move(r);
    }

    void walk(Value* root) {
        struct Ev { Value* v; std::string path; bool exit; };
        std::vector<Ev> stk;
        stk.push_back({root, "", false});
        uint64_t visits = 0;
        while (!stk.empty()) {
            Ev e = std::move(stk.back()); stk.pop_back();
            if (e.exit) { e.v->mark = 2; continue; }
            if (++visits > 50000000) throw Err("walk: visit limit exceeded");
            Value* v = e.v;
            switch (v->k) {
                case Value::TENSOR: emit(e.path, v); break;
                case Value::DICT: {
                    if (v->mark == 1) break;             // cycle: skip
                    if (v->mark != 2) { v->mark = 1; stk.push_back({v, "", true}); }
                    for (auto it = v->kv.rbegin(); it != v->kv.rend(); ++it) {
                        std::string k;
                        if (it->first->k == Value::STR) k = it->first->s;
                        else if (it->first->k == Value::INT) k = std::to_string(it->first->i);
                        else continue;
                        stk.push_back({it->second, e.path.empty() ? k : e.path + "/" + k, false});
                    }
                    break;
                }
                case Value::LIST: case Value::TUPLE: case Value::SET: {
                    if (v->mark == 1) break;
                    if (v->mark != 2) { v->mark = 1; stk.push_back({v, "", true}); }
                    for (size_t i = v->it.size(); i-- > 0;) {
                        std::string pi = e.path.empty() ? std::to_string(i) : e.path + "/" + std::to_string(i);
                        stk.push_back({v->it[i], std::move(pi), false});
                    }
                    break;
                }
                default: break;
            }
        }
    }

    static void json_escape(std::string& o, const std::string& s) {
        o += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\b': o += "\\b"; break;
                case '\f': o += "\\f"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char tmp[8]; snprintf(tmp, sizeof tmp, "\\u%04x", c); o += tmp;
                    } else o += (char)c;
            }
        }
        o += '"';
    }

    static void json_ints(std::string& o, const std::vector<int64_t>& v, const std::string& ind, const std::string& ind2) {
        if (v.empty()) { o += "[]"; return; }
        o += "[\n";
        for (size_t i = 0; i < v.size(); i++) {
            o += ind2 + std::to_string(v[i]);
            if (i + 1 < v.size()) o += ",";
            o += "\n";
        }
        o += ind + "]";
    }

    // Generic Value -> JSON (python json.dumps(indent=1, sort_keys=True) style).
    static void json_value(std::string& o, const Value* v, int depth);

    void write_manifest(const std::map<std::string, Value*>* config) {
        std::string o = "{\n \"byteorder\": \"little\",\n";
        if (config) {
            o += " \"config\": ";
            json_map(o, *config, 1);
            o += ",\n";
        }
        o += " \"nulltorch_manifest\": 1,\n \"tensors\": ";
        if (recs.empty()) { o += "{}"; }
        else {
            o += "{\n";
            size_t ti = 0;
            for (auto& [path, r] : recs) {
                o += "  "; json_escape(o, path); o += ": {\n";
                o += "   \"dtype\": \"" + r.dtype + "\",\n";
                o += "   \"nbytes\": " + std::to_string(r.nbytes) + ",\n";
                o += "   \"shape\": "; json_ints(o, r.shape, "   ", "    ");
                if (!rvc) {
                    o += ",\n";
                    o += "   \"storage_key\": "; json_escape(o, r.key); o += ",\n";
                    o += "   \"storage_offset\": " + std::to_string(r.off) + ",\n";
                    o += "   \"stride\": "; json_ints(o, r.stride, "   ", "    ");
                }
                o += "\n";
                o += "  }";
                if (++ti < recs.size()) o += ",";
                o += "\n";
            }
            o += " }";
        }
        o += "\n}\n";
        std::string mp = outdir + "/manifest.json";
        FILE* f = fopen(mp.c_str(), "wb");
        if (!f) throw Err("cannot write " + mp);
        if (fwrite(o.data(), 1, o.size(), f) != o.size() || fclose(f) != 0)
            throw Err("write failed: " + mp);
    }

    static void json_map(std::string& o, const std::map<std::string, Value*>& m, int depth) {
        if (m.empty()) { o += "{}"; return; }
        std::string ind(depth * 1, ' '), ind2((depth + 1) * 1, ' ');
        o += "{\n";
        size_t i = 0;
        for (auto& [k, v] : m) {
            o += ind2; json_escape(o, k); o += ": ";
            json_value(o, v, depth + 1);
            if (++i < m.size()) o += ",";
            o += "\n";
        }
            o += ind + "}";
    }
};

void Emitter::json_value(std::string& o, const Value* v, int depth) {
    if (depth > 1000) { o += "null"; return; }
    std::string ind(depth * 1, ' '), ind2((depth + 1) * 1, ' ');
    switch (v->k) {
        case Value::NONE: case Value::OPAQUE: case Value::BIGINT: o += "null"; return;
        case Value::TF: o += v->b ? "true" : "false"; return;
        case Value::INT: o += std::to_string(v->i); return;
        case Value::FLOAT: {
            if (!std::isfinite(v->f)) { o += "null"; return; }
            char tmp[40]; snprintf(tmp, sizeof tmp, "%.17g", v->f);
            o += tmp; return;
        }
        case Value::STR: case Value::BYTES: json_escape(o, v->s); return;
        case Value::LIST: case Value::TUPLE: case Value::SET: {
            if (v->it.empty()) { o += "[]"; return; }
            o += "[\n";
            for (size_t i = 0; i < v->it.size(); i++) {
                o += ind2;
                json_value(o, v->it[i], depth + 1);
                if (i + 1 < v->it.size()) o += ",";
                o += "\n";
            }
            o += ind + "]";
            return;
        }
        case Value::DICT: {
            std::map<std::string, Value*> m;
            for (auto& pr : v->kv) {
                if (pr.first->k == Value::STR) m[pr.first->s] = pr.second;
                else if (pr.first->k == Value::INT) m[std::to_string(pr.first->i)] = pr.second;
            }
            json_map(o, m, depth);
            return;
        }
        default: o += "null"; return;
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: convert <file.pth> <out_dir>\n");
        return 2;
    }
    try {
        Zip z;
        z.open(argv[1]);
        // locate <prefix>/data.pkl
        const Entry* pkl = nullptr;
        std::string prefix;
        for (auto& e : z.entries) {
            bool ispkl = e.name == "data.pkl" ||
                (e.name.size() > 8 && e.name.compare(e.name.size() - 9, 9, "/data.pkl") == 0);
            if (ispkl) {
                pkl = &e;
                prefix = e.name.substr(0, e.name.size() - 8);
                break;
            }
        }
        if (!pkl) throw Err("no data.pkl entry in archive");
        const std::vector<uint8_t>& pkb = z.load(*const_cast<Entry*>(pkl), (uint64_t)1 << 28);

        bool big = false;
        if (const Entry* bo = z.find(prefix + "byteorder")) {
            const std::vector<uint8_t>& b = z.load(*const_cast<Entry*>(bo), 64);
            big = b.size() >= 3 && memcmp(b.data(), "big", 3) == 0;
        }

        Unpickler up{pkb.data(), pkb.size(), 0, {}, {}, {}};
        Value* root = up.run();

        std::error_code ec;
        std::filesystem::create_directories(std::string(argv[2]) + "/tensors", ec);
        if (ec) throw Err("cannot create output directory");

        Emitter em{z, prefix, big, argv[2], false, {}};

        // RVC-style checkpoints: {"weight": {tensors...}, "config": [18 values]}.
        // Tensor paths drop the "weight" prefix; the config list maps to
        // canonical field names (v2 layout), plus phone_dim derived from the
        // phone-embedding tensor.
        Value* tensor_root = root;
        std::map<std::string, Value*> config;
        bool have_config = false;
        if (root->k == Value::DICT) {
            Value* w = nullptr;
            Value* c = nullptr;
            for (auto& pr : root->kv) {
                if (pr.first->k != Value::STR) continue;
                if (pr.first->s == "weight") w = pr.second;
                else if (pr.first->s == "config") c = pr.second;
            }
            if (w && w->k == Value::DICT && c && c->k == Value::LIST && c->it.size() == 18) {
                bool any_tensor = false;
                for (auto& pr : w->kv) if (pr.second->k == Value::TENSOR) { any_tensor = true; break; }
                if (any_tensor) {
                    static const char* NAMES[18] = {
                        "spec_channels", "segment_size", "inter_channels", "hidden_channels",
                        "filter_channels", "n_heads", "n_layers", "kernel_size", "p_dropout",
                        "resblock", "resblock_kernel_sizes", "resblock_dilation_sizes",
                        "upsample_rates", "upsample_initial_channel", "upsample_kernel_sizes",
                        "n_speakers", "gin_channels", "sr"};
                    tensor_root = w;
                    have_config = true;
                    em.rvc = true;
                    for (size_t i = 0; i < 18; i++) config[NAMES[i]] = c->it[i];
                    for (auto& pr : w->kv)
                        if (pr.first->k == Value::STR && pr.first->s == "enc_p.emb_phone.weight" &&
                            pr.second->k == Value::TENSOR && !pr.second->shape.empty()) {
                            Value* pd = up.mk(Value::INT);
                            pd->i = pr.second->shape.back();
                            config["phone_dim"] = pd;
                        }
                }
            }
        }

        em.walk(tensor_root);
        em.write_manifest(have_config ? &config : nullptr);
        return 0;
    } catch (const Err& e) {
        fprintf(stderr, "convert: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "convert: internal error: %s\n", e.what());
        return 1;
    }
}
