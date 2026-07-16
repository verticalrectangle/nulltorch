#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };

static uint16_t u16(const Bytes& b, size_t p) {
    if (p > b.size() || b.size() - p < 2) throw Error("truncated uint16");
    return uint16_t(b[p]) | (uint16_t(b[p + 1]) << 8);
}
static uint32_t u32(const Bytes& b, size_t p) {
    if (p > b.size() || b.size() - p < 4) throw Error("truncated uint32");
    return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24);
}
static uint64_t u64(const Bytes& b, size_t p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) {
        if (p + i >= b.size()) throw Error("truncated uint64");
        x |= uint64_t(b[p + i]) << (8 * i);
    }
    return x;
}
static int32_t i32(const Bytes& b, size_t p) { return static_cast<int32_t>(u32(b, p)); }
static size_t checked_size(uint64_t x) {
    if (x > std::numeric_limits<size_t>::max()) throw Error("value too large for this platform");
    return static_cast<size_t>(x);
}
static void require_range(size_t p, size_t n, size_t size, const char* what) {
    if (p > size || n > size - p) throw Error(std::string("truncated ") + what);
}
static Bytes read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("cannot open input: " + path.string());
    f.seekg(0, std::ios::end);
    auto end = f.tellg();
    if (end < 0) throw Error("cannot size input");
    Bytes b(static_cast<size_t>(end));
    f.seekg(0);
    if (!b.empty() && !f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()))) throw Error("cannot read input");
    return b;
}

class BitReader {
    const uint8_t* p_;
    size_t n_;
    size_t byte_ = 0;
    unsigned bit_ = 0;
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint32_t bits(unsigned count) {
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (byte_ >= n_) throw Error("truncated deflate stream");
            value |= uint32_t((p_[byte_] >> bit_) & 1u) << i;
            if (++bit_ == 8) { bit_ = 0; ++byte_; }
        }
        return value;
    }
    void align() { if (bit_) { bit_ = 0; ++byte_; } }
};

struct Huffman {
    struct Code { uint32_t reversed; uint16_t symbol; uint8_t length; };
    std::vector<Code> codes;

    static uint32_t reverse_bits(uint32_t x, unsigned n) {
        uint32_t r = 0;
        while (n--) { r = (r << 1) | (x & 1u); x >>= 1; }
        return r;
    }
    explicit Huffman(const std::vector<uint8_t>& lengths) {
        std::array<uint32_t, 16> count{};
        for (uint8_t l : lengths) {
            if (l > 15) throw Error("invalid Huffman code length");
            if (l) ++count[l];
        }
        std::array<uint32_t, 16> next{};
        uint32_t code = 0;
        for (unsigned len = 1; len <= 15; ++len) {
            code = (code + count[len - 1]) << 1;
            next[len] = code;
        }
        for (size_t sym = 0; sym < lengths.size(); ++sym) {
            unsigned len = lengths[sym];
            if (len) codes.push_back({reverse_bits(next[len]++, len), static_cast<uint16_t>(sym), static_cast<uint8_t>(len)});
        }
    }
    unsigned decode(BitReader& r) const {
        uint32_t code = 0;
        for (unsigned len = 1; len <= 15; ++len) {
            code |= r.bits(1) << (len - 1);
            for (const auto& c : codes) if (c.length == len && c.reversed == code) return c.symbol;
        }
        throw Error("invalid Huffman code");
    }
};

static Bytes inflate_raw(const uint8_t* src, size_t size, size_t expected) {
    static constexpr int length_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static constexpr int length_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static constexpr int dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static constexpr int dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    BitReader r(src, size);
    Bytes out;
    out.reserve(expected);
    bool final = false;
    while (!final) {
        final = r.bits(1) != 0;
        unsigned type = r.bits(2);
        if (type == 0) {
            r.align();
            uint16_t len = static_cast<uint16_t>(r.bits(16));
            uint16_t nlen = static_cast<uint16_t>(r.bits(16));
            if (uint16_t(~len) != nlen) throw Error("invalid stored deflate block");
            for (unsigned i = 0; i < len; ++i) out.push_back(static_cast<uint8_t>(r.bits(8)));
            continue;
        }
        if (type == 3) throw Error("invalid deflate block type");
        std::vector<uint8_t> lit_lengths, dist_lengths;
        if (type == 1) {
            lit_lengths.resize(288);
            for (int i = 0; i <= 143; ++i) lit_lengths[i] = 8;
            for (int i = 144; i <= 255; ++i) lit_lengths[i] = 9;
            for (int i = 256; i <= 279; ++i) lit_lengths[i] = 7;
            for (int i = 280; i <= 287; ++i) lit_lengths[i] = 8;
            dist_lengths.assign(32, 5);
        } else {
            unsigned nlit = r.bits(5) + 257, ndist = r.bits(5) + 1, nclen = r.bits(4) + 4;
            static constexpr int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<uint8_t> cl(19);
            for (unsigned i = 0; i < nclen; ++i) cl[order[i]] = static_cast<uint8_t>(r.bits(3));
            Huffman ch(cl);
            std::vector<uint8_t> all;
            all.reserve(nlit + ndist);
            while (all.size() < nlit + ndist) {
                unsigned sym = ch.decode(r);
                if (sym <= 15) all.push_back(static_cast<uint8_t>(sym));
                else if (sym == 16) {
                    if (all.empty()) throw Error("deflate repeat without previous length");
                    unsigned count = r.bits(2) + 3;
                    uint8_t value = all.back();
                    while (count--) all.push_back(value);
                } else if (sym == 17) {
                    unsigned count = r.bits(3) + 3;
                    while (count--) all.push_back(0);
                } else if (sym == 18) {
                    unsigned count = r.bits(7) + 11;
                    while (count--) all.push_back(0);
                } else throw Error("invalid code-length symbol");
                if (all.size() > nlit + ndist) throw Error("too many deflate code lengths");
            }
            lit_lengths.assign(all.begin(), all.begin() + nlit);
            dist_lengths.assign(all.begin() + nlit, all.end());
        }
        Huffman lit(lit_lengths), dist(dist_lengths);
        for (;;) {
            unsigned sym = lit.decode(r);
            if (sym < 256) { out.push_back(static_cast<uint8_t>(sym)); continue; }
            if (sym == 256) break;
            if (sym < 257 || sym > 285) throw Error("invalid deflate length symbol");
            unsigned li = sym - 257;
            size_t length = static_cast<size_t>(length_base[li]) + r.bits(length_extra[li]);
            unsigned ds = dist.decode(r);
            if (ds >= 30) throw Error("invalid deflate distance symbol");
            size_t distance = static_cast<size_t>(dist_base[ds]) + r.bits(dist_extra[ds]);
            if (distance == 0 || distance > out.size()) throw Error("invalid deflate distance");
            if (length > std::numeric_limits<size_t>::max() - out.size()) throw Error("inflated data too large");
            for (size_t i = 0; i < length; ++i) out.push_back(out[out.size() - distance]);
        }
    }
    if (out.size() != expected) throw Error("deflate size mismatch");
    return out;
}

struct ZipEntry { uint16_t method; uint64_t compressed, uncompressed, local_offset; };

class ZipArchive {
    Bytes bytes_;
    std::map<std::string, ZipEntry> entries_;

    static void parse_zip64_extra(const Bytes& b, size_t p, size_t n, uint32_t raw_usize, uint32_t raw_csize, uint32_t raw_offset,
                                  uint64_t& usize, uint64_t& csize, uint64_t& offset) {
        size_t end = p + n;
        require_range(p, n, b.size(), "ZIP extra field");
        while (p + 4 <= end) {
            uint16_t id = u16(b, p), len = u16(b, p + 2); p += 4;
            if (len > end - p) throw Error("truncated ZIP extra block");
            if (id == 1) {
                size_t q = p;
                auto take8 = [&]() { if (q + 8 > p + len) throw Error("truncated ZIP64 extra field"); uint64_t x = u64(b, q); q += 8; return x; };
                if (raw_usize == 0xffffffffu) usize = take8();
                if (raw_csize == 0xffffffffu) csize = take8();
                if (raw_offset == 0xffffffffu) offset = take8();
                return;
            }
            p += len;
        }
    }
public:
    explicit ZipArchive(const fs::path& path) : bytes_(read_file(path)) {
        if (bytes_.size() < 22) throw Error("not a ZIP archive");
        size_t floor = bytes_.size() > 65557 ? bytes_.size() - 65557 : 0;
        size_t eocd = std::numeric_limits<size_t>::max();
        for (size_t p = bytes_.size() - 22;; --p) {
            if (u32(bytes_, p) == 0x06054b50u && p + 22 + u16(bytes_, p + 20) == bytes_.size()) { eocd = p; break; }
            if (p == floor) break;
        }
        if (eocd == std::numeric_limits<size_t>::max()) throw Error("ZIP end record not found");
        if (u16(bytes_, eocd + 4) || u16(bytes_, eocd + 6)) throw Error("multi-disk ZIP is unsupported");
        uint64_t count = u16(bytes_, eocd + 10), cd_size = u32(bytes_, eocd + 12), cd_offset = u32(bytes_, eocd + 16);
        if (count == 0xffffu || cd_size == 0xffffffffu || cd_offset == 0xffffffffu) {
            if (eocd < 20 || u32(bytes_, eocd - 20) != 0x07064b50u) throw Error("ZIP64 locator missing");
            size_t z = checked_size(u64(bytes_, eocd - 12));
            require_range(z, 56, bytes_.size(), "ZIP64 end record");
            if (u32(bytes_, z) != 0x06064b50u) throw Error("ZIP64 end record missing");
            if (u32(bytes_, z + 16) || u32(bytes_, z + 20)) throw Error("multi-disk ZIP64 is unsupported");
            count = u64(bytes_, z + 32); cd_size = u64(bytes_, z + 40); cd_offset = u64(bytes_, z + 48);
        }
        size_t p = checked_size(cd_offset), cd_end = p + checked_size(cd_size);
        if (cd_end < p || cd_end > bytes_.size()) throw Error("invalid central directory bounds");
        for (uint64_t index = 0; index < count; ++index) {
            require_range(p, 46, cd_end, "central directory header");
            if (u32(bytes_, p) != 0x02014b50u) throw Error("invalid central directory signature");
            uint16_t flags = u16(bytes_, p + 8), method = u16(bytes_, p + 10);
            if (flags & ((1u << 0) | (1u << 13))) throw Error("encrypted ZIP entry is unsupported");
            uint32_t rc = u32(bytes_, p + 20), ru = u32(bytes_, p + 24), ro = u32(bytes_, p + 42);
            uint64_t csize = rc, usize = ru, offset = ro;
            size_t nl = u16(bytes_, p + 28), xl = u16(bytes_, p + 30), cl = u16(bytes_, p + 32);
            require_range(p + 46, nl + xl + cl, cd_end, "central directory entry");
            std::string name(reinterpret_cast<const char*>(bytes_.data() + p + 46), nl);
            parse_zip64_extra(bytes_, p + 46 + nl, xl, ru, rc, ro, usize, csize, offset);
            entries_[name] = {method, csize, usize, offset};
            p += 46 + nl + xl + cl;
        }
    }
    std::vector<std::string> names() const {
        std::vector<std::string> r;
        for (const auto& [name, _] : entries_) r.push_back(name);
        return r;
    }
    Bytes extract(const std::string& name) const {
        auto it = entries_.find(name);
        if (it == entries_.end()) throw Error("ZIP entry not found: " + name);
        const auto& e = it->second;
        size_t p = checked_size(e.local_offset);
        require_range(p, 30, bytes_.size(), "local ZIP header");
        if (u32(bytes_, p) != 0x04034b50u) throw Error("invalid local ZIP header");
        size_t data = p + 30 + u16(bytes_, p + 26) + u16(bytes_, p + 28);
        size_t compressed = checked_size(e.compressed), uncompressed = checked_size(e.uncompressed);
        require_range(data, compressed, bytes_.size(), "ZIP entry data");
        if (e.method == 0) {
            if (compressed != uncompressed) throw Error("invalid STORED ZIP entry sizes");
            return Bytes(bytes_.begin() + data, bytes_.begin() + data + compressed);
        }
        if (e.method == 8) return inflate_raw(bytes_.data() + data, compressed, uncompressed);
        throw Error("unsupported ZIP compression method " + std::to_string(e.method));
    }
};

enum class Kind { Nil, Integer, Real, String, Sequence, Dictionary, Global, Storage, Tensor, Unknown, Mark };
struct Node;
using V = std::shared_ptr<Node>;
struct Node {
    Kind kind = Kind::Nil;
    int64_t integer = 0;
    double real = 0;
    std::string text;
    std::vector<V> seq;
    std::vector<std::pair<V,V>> dict;
    V storage;
    int64_t offset = 0;
    std::vector<int64_t> shape, stride;
};
static V node(Kind k) { auto v = std::make_shared<Node>(); v->kind = k; return v; }
static V integer(int64_t x) { auto v = node(Kind::Integer); v->integer = x; return v; }
static V stringv(std::string x) { auto v = node(Kind::String); v->text = std::move(x); return v; }
static V sequence(std::vector<V> x = {}) { auto v = node(Kind::Sequence); v->seq = std::move(x); return v; }
static V dictionary() { return node(Kind::Dictionary); }
static V globalv(std::string x) { auto v = node(Kind::Global); v->text = std::move(x); return v; }
static std::string as_string(const V& v, const char* what) {
    if (!v || (v->kind != Kind::String && v->kind != Kind::Global)) throw Error(std::string("expected string for ") + what);
    return v->text;
}
static int64_t as_int(const V& v, const char* what) {
    if (!v || v->kind != Kind::Integer) throw Error(std::string("expected integer for ") + what);
    return v->integer;
}
static std::vector<int64_t> as_ints(const V& v, const char* what) {
    if (!v || v->kind != Kind::Sequence) throw Error(std::string("expected tuple for ") + what);
    std::vector<int64_t> r; r.reserve(v->seq.size());
    for (const auto& x : v->seq) r.push_back(as_int(x, what));
    return r;
}

class PickleReader {
    const Bytes& b_;
    size_t p_ = 0;
    std::vector<V> stack_, memo_;
    V mark_ = node(Kind::Mark);

    uint8_t byte() { if (p_ >= b_.size()) throw Error("truncated pickle"); return b_[p_++]; }
    uint16_t read16() { uint16_t x = u16(b_, p_); p_ += 2; return x; }
    uint32_t read32() { uint32_t x = u32(b_, p_); p_ += 4; return x; }
    uint64_t read64() { uint64_t x = u64(b_, p_); p_ += 8; return x; }
    std::string bytes(size_t n) { require_range(p_, n, b_.size(), "pickle string"); std::string s(reinterpret_cast<const char*>(b_.data() + p_), n); p_ += n; return s; }
    std::string line() { size_t start = p_; while (p_ < b_.size() && b_[p_] != '\n') ++p_; if (p_ == b_.size()) throw Error("unterminated pickle line"); std::string s(reinterpret_cast<const char*>(b_.data() + start), p_ - start); ++p_; return s; }
    V pop() { if (stack_.empty()) throw Error("pickle stack underflow"); V v = stack_.back(); stack_.pop_back(); return v; }
    size_t mark_pos() const { for (size_t i = stack_.size(); i-- > 0;) if (stack_[i] == mark_) return i; throw Error("pickle MARK missing"); }
    std::vector<V> pop_mark() { size_t m = mark_pos(); std::vector<V> x(stack_.begin() + m + 1, stack_.end()); stack_.resize(m); return x; }
    void memo_put(size_t i) { if (stack_.empty()) throw Error("cannot memoize empty stack"); if (i >= memo_.size()) memo_.resize(i + 1); memo_[i] = stack_.back(); }
    void memo_get(size_t i) { if (i >= memo_.size() || !memo_[i]) throw Error("invalid pickle memo index"); stack_.push_back(memo_[i]); }
    static void dict_set(const V& d, V key, V value) {
        if (!d || d->kind != Kind::Dictionary) throw Error("pickle SETITEM target is not a dict");
        for (auto& kv : d->dict) {
            if (kv.first->kind == key->kind && ((key->kind == Kind::String && kv.first->text == key->text) || (key->kind == Kind::Integer && kv.first->integer == key->integer))) { kv.second = std::move(value); return; }
        }
        d->dict.emplace_back(std::move(key), std::move(value));
    }
    V persistent(V id) {
        if (!id || id->kind != Kind::Sequence || id->seq.size() < 5 || as_string(id->seq[0], "persistent id") != "storage") throw Error("unsupported pickle persistent id");
        auto s = node(Kind::Storage);
        s->text = as_string(id->seq[2], "storage key");
        std::string cls = as_string(id->seq[1], "storage class");
        auto pos = cls.rfind('.'); if (pos != std::string::npos) cls = cls.substr(pos + 1);
        static const std::map<std::string, std::pair<std::string,int>> types = {
            {"FloatStorage", {"f32",4}}, {"HalfStorage", {"f16",2}}, {"BFloat16Storage", {"bf16",2}}, {"DoubleStorage", {"f64",8}},
            {"LongStorage", {"i64",8}}, {"IntStorage", {"i32",4}}, {"ShortStorage", {"i16",2}}, {"CharStorage", {"i8",1}},
            {"ByteStorage", {"u8",1}}, {"BoolStorage", {"bool",1}}
        };
        auto it = types.find(cls);
        if (it == types.end()) throw Error("unsupported storage class: " + cls);
        s->shape = {it->second.second};
        s->stride = {as_int(id->seq[4], "storage size")};
        s->offset = 0;
        s->storage = stringv(it->second.first);
        return s;
    }
    V reduce(V callable, V args) {
        std::string name = callable && callable->kind == Kind::Global ? callable->text : "";
        if (!args || args->kind != Kind::Sequence) return node(Kind::Unknown);
        if (name.find("_rebuild_tensor") != std::string::npos) {
            if (args->seq.size() < 4 || !args->seq[0] || args->seq[0]->kind != Kind::Storage) throw Error("invalid tensor rebuild arguments");
            auto t = node(Kind::Tensor); t->storage = args->seq[0];
            t->offset = as_int(args->seq[1], "storage offset"); t->shape = as_ints(args->seq[2], "shape"); t->stride = as_ints(args->seq[3], "stride");
            if (t->shape.size() != t->stride.size()) throw Error("tensor shape/stride rank mismatch");
            return t;
        }
        if (name.find("_rebuild_parameter") != std::string::npos && !args->seq.empty()) return args->seq[0];
        if (name == "collections.OrderedDict" || name == "collections.defaultdict" || name == "builtins.dict") return dictionary();
        auto u = node(Kind::Unknown); u->seq = args->seq; return u;
    }
    static int64_t signed_long(const std::string& s) {
        if (s.empty()) return 0;
        if (s.size() > 8) throw Error("pickle integer exceeds int64");
        uint64_t x = 0; for (size_t i = 0; i < s.size(); ++i) x |= uint64_t(static_cast<uint8_t>(s[i])) << (8 * i);
        if ((static_cast<uint8_t>(s.back()) & 0x80u) && s.size() < 8) x |= (~uint64_t(0)) << (8 * s.size());
        return static_cast<int64_t>(x);
    }
public:
    explicit PickleReader(const Bytes& b) : b_(b) {}
    V run() {
        for (;;) {
            uint8_t op = byte();
            switch (op) {
            case 0x80: { unsigned proto = byte(); if (proto > 5) throw Error("unsupported pickle protocol"); break; }
            case 0x95: (void)read64(); break;
            case '.': { V result = pop(); return result; }
            case '(' : stack_.push_back(mark_); break;
            case '0': (void)pop(); break;
            case '1': (void)pop_mark(); break;
            case '2': if (stack_.empty()) throw Error("pickle DUP underflow"); else stack_.push_back(stack_.back()); break;
            case 'N': stack_.push_back(node(Kind::Nil)); break;
            case 0x88: stack_.push_back(integer(1)); break;
            case 0x89: stack_.push_back(integer(0)); break;
            case 'I': { std::string s = line(); stack_.push_back(integer(s == "01" ? 1 : s == "00" ? 0 : std::stoll(s))); break; }
            case 'J': stack_.push_back(integer(static_cast<int32_t>(read32()))); break;
            case 'K': stack_.push_back(integer(byte())); break;
            case 'M': stack_.push_back(integer(read16())); break;
            case 'L': { std::string s = line(); if (!s.empty() && s.back() == 'L') s.pop_back(); stack_.push_back(integer(std::stoll(s))); break; }
            case 0x8a: { size_t n = byte(); stack_.push_back(integer(signed_long(bytes(n)))); break; }
            case 0x8b: { size_t n = read32(); stack_.push_back(integer(signed_long(bytes(n)))); break; }
            case 'F': { auto v = node(Kind::Real); v->real = std::stod(line()); stack_.push_back(v); break; }
            case 'G': { require_range(p_, 8, b_.size(), "BINFLOAT"); uint64_t bits = 0; for (int i=0;i<8;++i) bits=(bits<<8)|b_[p_++]; auto v=node(Kind::Real); std::memcpy(&v->real,&bits,8); stack_.push_back(v); break; }
            case 'X': stack_.push_back(stringv(bytes(read32()))); break;
            case 0x8c: stack_.push_back(stringv(bytes(byte()))); break;
            case 0x8d: stack_.push_back(stringv(bytes(checked_size(read64())))); break;
            case 'T': stack_.push_back(stringv(bytes(checked_size(read32())))); break;
            case 'U': stack_.push_back(stringv(bytes(byte()))); break;
            case 'B': stack_.push_back(stringv(bytes(checked_size(read32())))); break;
            case 'C': stack_.push_back(stringv(bytes(byte()))); break;
            case 0x8e: stack_.push_back(stringv(bytes(checked_size(read64())))); break;
            case 'S': { std::string s=line(); if (s.size()>=2 && (s.front()=='\''||s.front()=='\"')) s=s.substr(1,s.size()-2); stack_.push_back(stringv(s)); break; }
            case 'V': stack_.push_back(stringv(line())); break;
            case ']': stack_.push_back(sequence()); break;
            case 'l': stack_.push_back(sequence(pop_mark())); break;
            case 'a': { V x=pop(); if(stack_.empty()||stack_.back()->kind!=Kind::Sequence) throw Error("pickle APPEND target is not a list"); stack_.back()->seq.push_back(x); break; }
            case 'e': { auto x=pop_mark(); if(stack_.empty()||stack_.back()->kind!=Kind::Sequence) throw Error("pickle APPENDS target is not a list"); stack_.back()->seq.insert(stack_.back()->seq.end(),x.begin(),x.end()); break; }
            case ')': stack_.push_back(sequence()); break;
            case 't': stack_.push_back(sequence(pop_mark())); break;
            case 0x85: { V a=pop(); stack_.push_back(sequence({a})); break; }
            case 0x86: { V b=pop(),a=pop(); stack_.push_back(sequence({a,b})); break; }
            case 0x87: { V c=pop(),b=pop(),a=pop(); stack_.push_back(sequence({a,b,c})); break; }
            case '}': stack_.push_back(dictionary()); break;
            case 'd': { auto x=pop_mark(); if(x.size()%2) throw Error("odd pickle DICT items"); V d=dictionary(); for(size_t i=0;i<x.size();i+=2) dict_set(d,x[i],x[i+1]); stack_.push_back(d); break; }
            case 's': { V value=pop(), key=pop(); if(stack_.empty()) throw Error("pickle SETITEM underflow"); dict_set(stack_.back(),key,value); break; }
            case 'u': { auto x=pop_mark(); if(x.size()%2||stack_.empty()) throw Error("invalid pickle SETITEMS"); for(size_t i=0;i<x.size();i+=2) dict_set(stack_.back(),x[i],x[i+1]); break; }
            case 'q': memo_put(byte()); break;
            case 'r': memo_put(read32()); break;
            case 'p': memo_put(static_cast<size_t>(std::stoull(line()))); break;
            case 'h': memo_get(byte()); break;
            case 'j': memo_get(read32()); break;
            case 'g': memo_get(static_cast<size_t>(std::stoull(line()))); break;
            case 0x94: memo_put(memo_.size()); break;
            case 'c': { std::string module=line(), name=line(); stack_.push_back(globalv(module+"."+name)); break; }
            case 0x93: { std::string name=as_string(pop(),"global name"), module=as_string(pop(),"global module"); stack_.push_back(globalv(module+"."+name)); break; }
            case 'Q': stack_.push_back(persistent(pop())); break;
            case 'P': stack_.push_back(stringv(line())); break;
            case 'R': { V args=pop(), callable=pop(); stack_.push_back(reduce(callable,args)); break; }
            case 'b': { (void)pop(); if(stack_.empty()) throw Error("pickle BUILD underflow"); break; }
            case 0x81: { V args=pop(), callable=pop(); stack_.push_back(reduce(callable,args)); break; }
            default: throw Error("unsupported pickle opcode 0x" + [] (uint8_t x) { static const char* h="0123456789abcdef"; std::string s; s+=h[x>>4]; s+=h[x&15]; return s; }(op));
            }
        }
    }
};

struct TensorInfo { std::string path, dtype, key; int itemsize; int64_t offset; std::vector<int64_t> shape, stride; };
static void collect(const V& v, const std::string& path, std::vector<TensorInfo>& out, std::vector<const Node*>& ancestors) {
    if (!v) return;
    if (v->kind == Kind::Tensor) {
        if (path.empty()) throw Error("root tensor has no output path");
        const V& s = v->storage;
        if (!s || s->kind != Kind::Storage || !s->storage || s->storage->kind != Kind::String || s->shape.empty()) throw Error("invalid tensor storage");
        out.push_back({path, s->storage->text, s->text, static_cast<int>(s->shape[0]), v->offset, v->shape, v->stride});
        return;
    }
    if (std::find(ancestors.begin(), ancestors.end(), v.get()) != ancestors.end()) return;
    ancestors.push_back(v.get());
    if (v->kind == Kind::Dictionary) {
        for (const auto& [k, value] : v->dict) if (k && (k->kind == Kind::String || k->kind == Kind::Integer)) {
            std::string part = k->kind == Kind::String ? k->text : std::to_string(k->integer);
            collect(value, path.empty() ? part : path + "/" + part, out, ancestors);
        }
    } else if (v->kind == Kind::Sequence) {
        for (size_t i=0;i<v->seq.size();++i) collect(v->seq[i], path.empty()?std::to_string(i):path+"/"+std::to_string(i), out, ancestors);
    }
    ancestors.pop_back();
}
static uint64_t numel(const TensorInfo& t) {
    uint64_t n = 1;
    for (int64_t d : t.shape) {
        if (d < 0) throw Error("negative tensor dimension");
        if (d == 0) return 0;
        if (n > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(d)) throw Error("tensor size overflow");
        n *= static_cast<uint64_t>(d);
    }
    return n;
}
static std::string json_escape(const std::string& s) {
    std::string r="\"";
    static const char* hex="0123456789abcdef";
    for(unsigned char c:s) {
        switch(c) { case '\"':r+="\\\"";break; case '\\':r+="\\\\";break; case '\b':r+="\\b";break; case '\f':r+="\\f";break; case '\n':r+="\\n";break; case '\r':r+="\\r";break; case '\t':r+="\\t";break;
        default: if(c<0x20){r+="\\u00";r+=hex[c>>4];r+=hex[c&15];}else r+=char(c); }
    }
    return r+'\"';
}
static std::string json_ints(const std::vector<int64_t>& xs) {
    std::string r="["; for(size_t i=0;i<xs.size();++i){if(i)r+=",";r+=std::to_string(xs[i]);} return r+"]";
}
static void write_binary(const fs::path& p, const Bytes& b) {
    std::ofstream f(p,std::ios::binary|std::ios::trunc); if(!f) throw Error("cannot create output: "+p.string());
    if(!b.empty()) f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));
    if(!f) throw Error("cannot write output: "+p.string());
}
static void convert(const fs::path& input, const fs::path& output) {
    ZipArchive zip(input);
    std::string pickle_name, prefix;
    for(const auto& name:zip.names()) if(name.size()>9 && name.ends_with("/data.pkl")) {
        if(!pickle_name.empty()) throw Error("archive contains multiple data.pkl entries");
        pickle_name=name; prefix=name.substr(0,name.size()-8);
    }
    if(pickle_name.empty()) throw Error("archive has no top-level data.pkl");
    V root=PickleReader(zip.extract(pickle_name)).run();
    std::vector<TensorInfo> tensors; std::vector<const Node*> ancestors; collect(root,"",tensors,ancestors);
    std::sort(tensors.begin(),tensors.end(),[](const auto&a,const auto&b){return a.path<b.path;});
    for(size_t i=1;i<tensors.size();++i) if(tensors[i-1].path==tensors[i].path) throw Error("duplicate tensor path: "+tensors[i].path);
    fs::create_directories(output/"tensors");
    for(const auto& t:tensors) {
        Bytes storage=zip.extract(prefix+"data/"+t.key);
        uint64_t count=numel(t), total=count*static_cast<uint64_t>(t.itemsize);
        if(t.itemsize<=0 || (count && total/count!=static_cast<uint64_t>(t.itemsize))) throw Error("tensor byte size overflow");
        Bytes materialized(checked_size(total));
        if(count) {
            std::vector<int64_t> index(t.shape.size(),0);
            for(uint64_t linear=0;linear<count;++linear) {
                int64_t element=t.offset;
                for(size_t d=0;d<index.size();++d) {
                    if(index[d] && (t.stride[d]>0 ? index[d]>std::numeric_limits<int64_t>::max()/t.stride[d] : t.stride[d]<0 && index[d]>std::numeric_limits<int64_t>::max()/-t.stride[d])) throw Error("tensor storage index overflow");
                    int64_t add=index[d]*t.stride[d];
                    if((add>0&&element>std::numeric_limits<int64_t>::max()-add)||(add<0&&element<std::numeric_limits<int64_t>::min()-add)) throw Error("tensor storage index overflow");
                    element+=add;
                }
                if(element<0 || static_cast<uint64_t>(element)>std::numeric_limits<uint64_t>::max()/t.itemsize) throw Error("tensor reads before storage");
                uint64_t source=static_cast<uint64_t>(element)*static_cast<uint64_t>(t.itemsize);
                if(source>storage.size() || static_cast<uint64_t>(t.itemsize)>storage.size()-source) throw Error("tensor reads beyond storage");
                std::copy_n(storage.data()+checked_size(source),t.itemsize,materialized.data()+checked_size(linear*static_cast<uint64_t>(t.itemsize)));
                for(size_t d=index.size();d-->0;) { if(++index[d]<t.shape[d]) break; index[d]=0; }
            }
        }
        std::string filename=t.path; for(size_t p=0;(p=filename.find('/',p))!=std::string::npos;) filename.replace(p,1,"__"),p+=2;
        write_binary(output/"tensors"/(filename+".bin"),materialized);
    }
    std::string manifest="{\n  \"nulltorch_manifest\": 1,\n  \"byteorder\": \"little\",\n  \"tensors\": {";
    for(size_t i=0;i<tensors.size();++i) {
        const auto&t=tensors[i]; uint64_t bytes_count=numel(t)*static_cast<uint64_t>(t.itemsize);
        manifest+=(i?",\n":"\n")+std::string("    ")+json_escape(t.path)+": {\n";
        manifest+="      \"dtype\": "+json_escape(t.dtype)+",\n";
        manifest+="      \"shape\": "+json_ints(t.shape)+",\n";
        manifest+="      \"stride\": "+json_ints(t.stride)+",\n";
        manifest+="      \"storage_key\": "+json_escape(t.key)+",\n";
        manifest+="      \"storage_offset\": "+std::to_string(t.offset)+",\n";
        manifest+="      \"nbytes\": "+std::to_string(bytes_count)+"\n    }";
    }
    manifest+=(tensors.empty()?"":"\n")+std::string("  }\n}\n");
    Bytes mb(manifest.begin(),manifest.end()); write_binary(output/"manifest.json",mb);
}
int main(int argc,char**argv) {
    if(argc!=3){std::cerr<<"usage: "<<argv[0]<<" <file.pth> <out_dir>\n";return 2;}
    try { convert(argv[1],argv[2]); return 0; }
    catch(const std::exception&e){std::cerr<<"convert: "<<e.what()<<"\n";return 1;}
}
