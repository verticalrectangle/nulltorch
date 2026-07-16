// NullTorch pth-prime reader (delta variant)
// C++ standard library only. Reads PyTorch .pth checkpoints produced by the
// delta/pth-prime generator and writes manifest.json + contiguous tensor bins.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

// ---------- helpers ----------

static std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void write_file(const std::string &path, const std::string &data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(data.data(), data.size());
}

static void write_file(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char *>(data.data()), data.size());
}

// ---------- JSON string escaping ----------

static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

// ---------- ZIP parser (delta magic: DZ) ----------

struct ZipEntry {
    std::string name;
    uint64_t lfh_offset;
    uint64_t data_offset;
    uint64_t usize;
    uint64_t csize;
    uint16_t method;
};

class ZipArchive {
public:
    explicit ZipArchive(const std::string &data) : data_(data) {}

    void parse() {
        // find EOCD by scanning backwards
        if (data_.size() < 22) throw std::runtime_error("file too small for zip");
        size_t eocd = std::string::npos;
        size_t max_scan = std::min<size_t>(data_.size(), 65557);
        for (size_t i = data_.size() - 22; i + 22 <= data_.size() && data_.size() - i <= max_scan; --i) {
            if (u32(i) == 0x06055a44) { // DZ\x05\x06 little-endian
                eocd = i;
                break;
            }
            if (i == 0) break;
        }
        if (eocd == std::string::npos) throw std::runtime_error("EOCD not found");

        uint64_t cd_offset = u32(eocd + 16);
        uint64_t cd_size = u32(eocd + 12);
        uint16_t cd_count16 = u16(eocd + 10);
        uint16_t disk_count16 = u16(eocd + 8);

        // simple zip64 EOCD handling
        bool is_zip64 = (cd_count16 == 0xFFFF) || (cd_size == 0xFFFFFFFFu) || (cd_offset == 0xFFFFFFFFu);
        if (is_zip64) {
            if (eocd < 20) throw std::runtime_error("zip64 locator missing");
            size_t locator = eocd - 20;
            if (u32(locator) != 0x07065a44) throw std::runtime_error("zip64 locator signature missing");
            uint64_t z64_offset = u64(locator + 8);
            if (z64_offset + 56 > data_.size()) throw std::runtime_error("zip64 EOCD out of range");
            if (u32(z64_offset) != 0x06065a44) throw std::runtime_error("zip64 EOCD signature missing");
            cd_offset = u64(z64_offset + 48);
            cd_size = u64(z64_offset + 40);
            uint64_t cd_count64 = u64(z64_offset + 32);
            if (cd_count64 > 0xFFFF) throw std::runtime_error("too many zip64 entries");
            cd_count16 = static_cast<uint16_t>(cd_count64);
        }

        if (cd_offset + cd_size > data_.size()) throw std::runtime_error("central directory out of range");
        uint64_t pos = cd_offset;
        for (uint16_t i = 0; i < cd_count16; ++i) {
            if (pos + 46 > data_.size()) throw std::runtime_error("central directory truncated");
            if (u32(pos) != 0x02015a44) throw std::runtime_error("central directory signature mismatch");
            uint16_t name_len = u16(pos + 28);
            uint16_t extra_len = u16(pos + 30);
            uint16_t comment_len = u16(pos + 32);
            uint64_t usize = u32(pos + 24);
            uint64_t csize = u32(pos + 20);
            uint64_t lfh_off = u32(pos + 42);
            uint16_t method = u16(pos + 10);

            size_t name_start = pos + 46;
            std::string name(data_.data() + name_start, name_len);
            size_t extra_start = name_start + name_len;

            // zip64 extra field
            if (usize == 0xFFFFFFFFu || csize == 0xFFFFFFFFu || lfh_off == 0xFFFFFFFFu) {
                size_t p = extra_start;
                size_t end = extra_start + extra_len;
                while (p + 4 <= end) {
                    uint16_t fid = u16(p);
                    uint16_t flen = u16(p + 2);
                    if (fid == 0x0001) {
                        size_t q = p + 4;
                        if (usize == 0xFFFFFFFFu) { usize = u64(q); q += 8; }
                        if (csize == 0xFFFFFFFFu) { csize = u64(q); q += 8; }
                        if (lfh_off == 0xFFFFFFFFu) { lfh_off = u64(q); q += 8; }
                        break;
                    }
                    p += 4 + flen;
                }
            }

            // read local file header to get data offset
            if (lfh_off + 30 > data_.size()) throw std::runtime_error("local file header out of range");
            if (u32(lfh_off) != 0x04035a44) throw std::runtime_error("local file header signature mismatch");
            uint16_t lname = u16(lfh_off + 26);
            uint16_t lextra = u16(lfh_off + 28);
            uint64_t data_off = lfh_off + 30 + lname + lextra;

            ZipEntry e;
            e.name = name;
            e.lfh_offset = lfh_off;
            e.data_offset = data_off;
            e.usize = usize;
            e.csize = csize;
            e.method = method;
            entries_[name] = e;

            pos += 46 + name_len + extra_len + comment_len;
        }
    }

    const std::unordered_map<std::string, ZipEntry> &entries() const { return entries_; }

    std::string read_entry(const std::string &name) const {
        auto it = entries_.find(name);
        if (it == entries_.end()) throw std::runtime_error("zip entry not found: " + name);
        const ZipEntry &e = it->second;
        if (e.method != 0) throw std::runtime_error("unsupported compression method for " + name);
        if (e.data_offset + e.usize > data_.size()) throw std::runtime_error("entry data out of range");
        return std::string(data_.data() + e.data_offset, e.usize);
    }

    const std::string &data() const { return data_; }

private:
    const std::string &data_;
    std::unordered_map<std::string, ZipEntry> entries_;

    uint16_t u16(size_t pos) const {
        return static_cast<uint16_t>(static_cast<unsigned char>(data_[pos])) |
               (static_cast<uint16_t>(static_cast<unsigned char>(data_[pos + 1])) << 8);
    }
    uint32_t u32(size_t pos) const {
        return static_cast<uint32_t>(u16(pos)) | (static_cast<uint32_t>(u16(pos + 2)) << 16);
    }
    uint64_t u64(size_t pos) const {
        return static_cast<uint64_t>(u32(pos)) | (static_cast<uint64_t>(u32(pos + 4)) << 32);
    }
};

// ---------- Python object model ----------

struct PyObj {
    virtual ~PyObj() = default;
};
using PObj = std::shared_ptr<PyObj>;

struct PyNone : PyObj {};
struct PyBool : PyObj { bool v; explicit PyBool(bool b) : v(b) {} };
struct PyInt : PyObj { long long v; explicit PyInt(long long x) : v(x) {} };
struct PyFloat : PyObj { double v; explicit PyFloat(double x) : v(x) {} };
struct PyStr : PyObj { std::string v; explicit PyStr(std::string s) : v(std::move(s)) {} };
struct PyList : PyObj { std::vector<PObj> v; };
struct PyTuple : PyObj { std::vector<PObj> v; };
struct PyDict : PyObj { std::unordered_map<std::string, PObj> v; };
struct PyStorage : PyObj {
    std::string cls;
    std::string key;
    std::string loc;
    long long numel;
};
struct PyTensor : PyObj {
    PObj storage;
    long long offset;
    std::vector<long long> shape;
    std::vector<long long> stride;
};
struct PyMark : PyObj {};

static PObj make_none() { return std::make_shared<PyNone>(); }
static PObj make_bool(bool b) { return std::make_shared<PyBool>(b); }
static PObj make_int(long long x) { return std::make_shared<PyInt>(x); }
static PObj make_float(double x) { return std::make_shared<PyFloat>(x); }
static PObj make_str(std::string s) { return std::make_shared<PyStr>(std::move(s)); }
static PObj make_list() { return std::make_shared<PyList>(); }
static PObj make_tuple() { return std::make_shared<PyTuple>(); }
static PObj make_tuple(std::vector<PObj> items) { auto t = std::make_shared<PyTuple>(); t->v = std::move(items); return t; }
static PObj make_dict() { return std::make_shared<PyDict>(); }
static PObj make_mark() { return std::make_shared<PyMark>(); }

static std::string as_str(const PObj &o) {
    auto s = std::dynamic_pointer_cast<PyStr>(o);
    if (!s) throw std::runtime_error("expected string");
    return s->v;
}

static std::string key_to_str(const PObj &o) {
    auto s = std::dynamic_pointer_cast<PyStr>(o);
    if (s) return s->v;
    auto i = std::dynamic_pointer_cast<PyInt>(o);
    if (i) return std::to_string(i->v);
    auto f = std::dynamic_pointer_cast<PyFloat>(o);
    if (f) return std::to_string(f->v);
    auto b = std::dynamic_pointer_cast<PyBool>(o);
    if (b) return b->v ? "True" : "False";
    return "?";
}

static long long as_int(const PObj &o) {
    auto i = std::dynamic_pointer_cast<PyInt>(o);
    if (i) return i->v;
    auto b = std::dynamic_pointer_cast<PyBool>(o);
    if (b) return b->v ? 1 : 0;
    auto f = std::dynamic_pointer_cast<PyFloat>(o);
    if (f) return static_cast<long long>(f->v);
    throw std::runtime_error("expected int");
}

static bool as_bool(const PObj &o) {
    auto b = std::dynamic_pointer_cast<PyBool>(o);
    if (b) return b->v;
    auto i = std::dynamic_pointer_cast<PyInt>(o);
    if (i) return i->v != 0;
    return false;
}

static std::vector<PObj> as_seq(const PObj &o) {
    auto t = std::dynamic_pointer_cast<PyTuple>(o);
    if (t) return t->v;
    auto l = std::dynamic_pointer_cast<PyList>(o);
    if (l) return l->v;
    throw std::runtime_error("expected tuple/list");
}

static bool is_mark(const PObj &o) { return std::dynamic_pointer_cast<PyMark>(o) != nullptr; }

// ---------- Pickle interpreter ----------

class PickleMachine {
public:
    explicit PickleMachine(const std::string &data) : data_(data), pos_(0) {}

    PObj run() {
        while (pos_ < data_.size()) {
            uint8_t op = read_u8();
            switch (op) {
                case 0x80: /* PROTO */ read_u8(); break;
                case 0x95: /* FRAME */ read_u64(); break;

                case 0x4e: /* NONE */ push(make_none()); break;
                case 0x88: /* NEWTRUE */ push(make_bool(true)); break;
                case 0x89: /* NEWFALSE */ push(make_bool(false)); break;

                case 0x4b: /* BININT1 */ push(make_int(read_u8())); break;
                case 0x4d: /* BININT2 */ push(make_int(read_u16())); break;
                case 0x4a: /* BININT */ push(make_int(read_i32())); break;
                case 0x49: /* INT */ {
                    std::string tok = read_line();
                    if (tok == "01") push(make_bool(true));
                    else if (tok == "00") push(make_bool(false));
                    else push(make_int(std::stoll(tok)));
                    break;
                }
                case 0x4c: /* LONG */ {
                    std::string tok = read_line();
                    if (!tok.empty() && tok.back() == 'L' || tok.back() == 'l') tok.pop_back();
                    push(make_int(std::stoll(tok)));
                    break;
                }
                case 0x8a: /* LONG1 */ {
                    uint8_t n = read_u8();
                    push(make_int(read_varint_signed(n)));
                    break;
                }
                case 0x8b: /* LONG4 */ {
                    uint32_t n = read_u32();
                    if (n > 1024) throw std::runtime_error("LONG4 too large");
                    push(make_int(read_varint_signed(n)));
                    break;
                }

                case 0x46: /* FLOAT */ push(make_float(std::stod(read_line()))); break;
                case 0x47: /* BINFLOAT */ push(make_float(read_f64_be())); break;

                case 0x58: /* BINUNICODE */ push(make_str(read_str(read_u32()))); break;
                case 0x8c: /* SHORT_BINUNICODE */ push(make_str(read_str(read_u8()))); break;
                case 0x8d: /* BINUNICODE8 */ {
                    uint64_t n = read_u64();
                    if (n > 0x10000000) throw std::runtime_error("BINUNICODE8 too large");
                    push(make_str(read_str(static_cast<size_t>(n))));
                    break;
                }
                case 0x56: /* UNICODE */ {
                    std::string raw = read_line();
                    push(make_str(decode_unicode_escape(raw)));
                    break;
                }
                case 0x53: /* STRING */ push(make_str(parse_string(read_line()))); break;
                case 0x54: /* BINSTRING */ {
                    int32_t n = read_i32();
                    if (n < 0) throw std::runtime_error("BINSTRING negative length");
                    push(make_str(read_str(static_cast<size_t>(n))));
                    break;
                }
                case 0x55: /* SHORT_BINSTRING */ push(make_str(read_str(read_u8()))); break;
                case 0x42: /* BINBYTES */ {
                    uint32_t n = read_u32();
                    push(make_str(read_str(n)));
                    break;
                }
                case 0x43: /* SHORT_BINBYTES */ push(make_str(read_str(read_u8()))); break;
                case 0x8e: /* BINBYTES8 */ {
                    uint64_t n = read_u64();
                    if (n > 0x10000000) throw std::runtime_error("BINBYTES8 too large");
                    push(make_str(read_str(static_cast<size_t>(n))));
                    break;
                }

                case 0x5d: /* EMPTY_LIST */ push(make_list()); break;
                case 0x61: { /* APPEND */
                    PObj val = pop();
                    auto lst = std::dynamic_pointer_cast<PyList>(top());
                    if (!lst) throw std::runtime_error("APPEND: not list");
                    lst->v.push_back(val);
                    break;
                }
                case 0x65: { /* APPENDS */
                    std::vector<PObj> items = pop_to_mark();
                    auto lst = std::dynamic_pointer_cast<PyList>(pop());
                    if (!lst) throw std::runtime_error("APPENDS: not list");
                    for (auto it = items.rbegin(); it != items.rend(); ++it) lst->v.push_back(*it);
                    push(lst);
                    break;
                }
                case 0x6c: { /* LIST */
                    std::vector<PObj> items = pop_to_mark();
                    std::reverse(items.begin(), items.end());
                    auto lst = std::make_shared<PyList>();
                    lst->v = std::move(items);
                    push(lst);
                    break;
                }

                case 0x29: /* EMPTY_TUPLE */ push(make_tuple()); break;
                case 0x74: { /* TUPLE */
                    std::vector<PObj> items = pop_to_mark();
                    std::reverse(items.begin(), items.end());
                    push(make_tuple(std::move(items)));
                    break;
                }
                case 0x85: /* TUPLE1 */ { auto a = pop(); push(make_tuple({a})); break; }
                case 0x86: /* TUPLE2 */ { auto b = pop(); auto a = pop(); push(make_tuple({a, b})); break; }
                case 0x87: /* TUPLE3 */ { auto c = pop(); auto b = pop(); auto a = pop(); push(make_tuple({a, b, c})); break; }

                case 0x7d: /* EMPTY_DICT */ push(make_dict()); break;
                case 0x64: { /* DICT */
                    std::vector<PObj> items = pop_to_mark();
                    std::reverse(items.begin(), items.end());
                    auto d = std::make_shared<PyDict>();
                    for (size_t i = 0; i + 1 < items.size(); i += 2) {
                        d->v[key_to_str(items[i])] = items[i + 1];
                    }
                    push(d);
                    break;
                }
                case 0x73: { /* SETITEM */
                    PObj val = pop();
                    PObj key = pop();
                    PObj dict = pop();
                    auto d = std::dynamic_pointer_cast<PyDict>(dict);
                    if (!d) throw std::runtime_error("SETITEM: not dict");
                    d->v[key_to_str(key)] = val;
                    push(dict);
                    break;
                }
                case 0x75: { /* SETITEMS */
                    std::vector<PObj> items = pop_to_mark();
                    PObj dict = pop();
                    auto d = std::dynamic_pointer_cast<PyDict>(dict);
                    if (!d) throw std::runtime_error("SETITEMS: not dict");
                    std::reverse(items.begin(), items.end());
                    for (size_t i = 0; i + 1 < items.size(); i += 2) {
                        d->v[key_to_str(items[i])] = items[i + 1];
                    }
                    push(dict);
                    break;
                }

                case 0x30: /* POP */ pop(); break;
                case 0x32: /* DUP */ push(stack_.back()); break;
                case 0x28: /* MARK */ push(make_mark()); break;
                case 0x31: /* POP_MARK */ pop_to_mark(); break;

                case 0x70: { /* PUT */
                    size_t idx = static_cast<size_t>(std::stoll(read_line()));
                    memo_put(idx);
                    break;
                }
                case 0x71: /* BINPUT */ memo_put(read_u8()); break;
                case 0x72: { /* LONG_BINPUT */
                    uint32_t idx = read_u32();
                    memo_put(idx);
                    break;
                }
                case 0x94: /* MEMOIZE */ memo_put(next_memo_++); break;

                case 0x67: { /* GET */
                    size_t idx = static_cast<size_t>(std::stoll(read_line()));
                    push(memo_get(idx));
                    break;
                }
                case 0x68: /* BINGET */ push(memo_get(read_u8())); break;
                case 0x6a: /* LONG_BINGET */ push(memo_get(read_u32())); break;

                case 0x63: { /* GLOBAL */
                    std::string mod = read_line();
                    std::string cls = read_line();
                    push(make_str(mod + "." + cls));
                    break;
                }
                case 0x93: { /* STACK_GLOBAL */
                    std::string cls = as_str(pop());
                    std::string mod = as_str(pop());
                    push(make_str(mod + "." + cls));
                    break;
                }

                case 0x52: /* REDUCE */ {
                    PObj args = pop();
                    PObj callable = pop();
                    push(reduce(callable, args));
                    break;
                }
                case 0x62: { /* BUILD */
                    PObj arg = pop();
                    PObj obj = pop();
                    push(build(obj, arg));
                    break;
                }
                case 0x81: { /* NEWOBJ */
                    PObj args = pop();
                    PObj cls = pop();
                    push(newobj(cls, args));
                    break;
                }
                case 0x92: { /* NEWOBJ_EX */
                    PObj kwargs = pop();
                    PObj args = pop();
                    PObj cls = pop();
                    (void)kwargs;
                    push(newobj(cls, args));
                    break;
                }
                case 0x69: { /* INST */
                    std::string mod = read_line();
                    std::string cls = read_line();
                    std::vector<PObj> args = pop_to_mark();
                    std::reverse(args.begin(), args.end());
                    push(newobj(make_str(mod + "." + cls), make_tuple(std::move(args))));
                    break;
                }
                case 0x6f: { /* OBJ */
                    std::vector<PObj> args = pop_to_mark();
                    PObj cls = pop();
                    std::reverse(args.begin(), args.end());
                    push(newobj(cls, make_tuple(std::move(args))));
                    break;
                }

                case 0x50: { /* PERSID */
                    std::string pid = read_line();
                    push(persistent_load(make_str(pid)));
                    break;
                }
                case 0x51: { /* BINPERSID */
                    PObj pid = pop();
                    push(persistent_load(pid));
                    break;
                }

                case 0x2e: { /* STOP */
                    if (stack_.empty()) throw std::runtime_error("empty stack at STOP");
                    return stack_.back();
                }


                default: {
                    char msg[64];
                    std::snprintf(msg, sizeof(msg), "unsupported pickle opcode 0x%02x at %zu", op, pos_ - 1);
                    throw std::runtime_error(msg);
                }
            }
        }
        if (stack_.empty()) throw std::runtime_error("empty stack after pickle stream");
        return stack_.back();
    }

private:
    const std::string &data_;
    size_t pos_;
    std::vector<PObj> stack_;
    std::unordered_map<size_t, PObj> memo_;
    size_t next_memo_ = 0;

    uint8_t u8_at(size_t p) const { return static_cast<uint8_t>(data_[p]); }
    uint8_t read_u8() { return u8_at(pos_++); }
    uint16_t read_u16() { uint16_t v = u8_at(pos_) | (u8_at(pos_ + 1) << 8); pos_ += 2; return v; }
    uint32_t read_u32() { uint32_t v = read_u16() | (static_cast<uint32_t>(read_u16()) << 16); return v; }
    int32_t read_i32() { return static_cast<int32_t>(read_u32()); }
    uint64_t read_u64() { uint64_t v = read_u32() | (static_cast<uint64_t>(read_u32()) << 32); return v; }
    double read_f64_be() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | u8_at(pos_++);
        double d;
        static_assert(sizeof(d) == sizeof(v), "double size");
        std::memcpy(&d, &v, sizeof(d));
        return d;
    }

    long long read_varint_signed(size_t n) {
        if (n == 0) return 0;
        if (n > 16) throw std::runtime_error("LONG integer too large");
        __int128 val = 0;
        for (size_t i = 0; i < n; ++i) {
            val |= (__int128)u8_at(pos_ + i) << (8 * i);
        }
        pos_ += n;
        int bits = static_cast<int>(8 * n);
        __int128 sign_bit = (__int128)1 << (bits - 1);
        if (bits < 128 && (val & sign_bit)) {
            val -= (__int128)1 << bits;
        }
        if (val < LLONG_MIN || val > LLONG_MAX)
            throw std::runtime_error("LONG integer out of int64 range");
        return static_cast<long long>(val);
    }

    std::string read_line() {
        size_t start = pos_;
        while (pos_ < data_.size() && data_[pos_] != '\n') ++pos_;
        if (pos_ >= data_.size()) throw std::runtime_error("newline not found");
        std::string s(data_.data() + start, pos_ - start);
        ++pos_; // consume '\n'
        return s;
    }

    std::string read_str(size_t n) {
        if (pos_ + n > data_.size()) throw std::runtime_error("string read past end");
        std::string s(data_.data() + pos_, n);
        pos_ += n;
        return s;
    }

    std::string parse_string(const std::string &raw) {
        // STRING has a repr-style argument with surrounding quotes and escapes.
        if (raw.size() < 2) return raw;
        char q = raw[0];
        if (q != '\'' && q != '"') return raw;
        if (raw.back() != q) return raw;
        std::string out;
        out.reserve(raw.size());
        for (size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() - 1) {
                char e = raw[i + 1];
                switch (e) {
                    case '\\': out.push_back('\\'); break;
                    case '\'': out.push_back('\''); break;
                    case '"': out.push_back('"'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    default: out.push_back(e); break;
                }
                ++i;
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }

    std::string decode_unicode_escape(const std::string &raw) {
        // raw-unicode-escape: ASCII plus \uXXXX and \UXXXXXXXX.
        std::string out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                char e = raw[i + 1];
                if (e == 'u' && i + 5 < raw.size()) {
                    std::string hx = raw.substr(i + 2, 4);
                    uint32_t cp = static_cast<uint32_t>(std::stoul(hx, nullptr, 16));
                    // encode UTF-8
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) { out.push_back(0xC0 | (cp >> 6)); out.push_back(0x80 | (cp & 0x3F)); }
                    else { out.push_back(0xE0 | (cp >> 12)); out.push_back(0x80 | ((cp >> 6) & 0x3F)); out.push_back(0x80 | (cp & 0x3F)); }
                    i += 5;
                } else if (e == 'U' && i + 9 < raw.size()) {
                    std::string hx = raw.substr(i + 2, 8);
                    uint32_t cp = static_cast<uint32_t>(std::stoul(hx, nullptr, 16));
                    // encode UTF-8 (up to 4 bytes)
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) { out.push_back(0xC0 | (cp >> 6)); out.push_back(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { out.push_back(0xE0 | (cp >> 12)); out.push_back(0x80 | ((cp >> 6) & 0x3F)); out.push_back(0x80 | (cp & 0x3F)); }
                    else { out.push_back(0xF0 | (cp >> 18)); out.push_back(0x80 | ((cp >> 12) & 0x3F)); out.push_back(0x80 | ((cp >> 6) & 0x3F)); out.push_back(0x80 | (cp & 0x3F)); }
                    i += 9;
                } else {
                    // pass through backslash and char
                    if (e == '\\') out.push_back('\\');
                    else if (e == 'n') out.push_back('\n');
                    else if (e == 't') out.push_back('\t');
                    else if (e == 'r') out.push_back('\r');
                    else out.push_back(e);
                    ++i;
                }
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }

    void push(const PObj &o) { stack_.push_back(o); }
    PObj pop() {
        if (stack_.empty()) throw std::runtime_error("pop from empty stack");
        PObj o = stack_.back();
        stack_.pop_back();
        return o;
    }
    PObj &top() {
        if (stack_.empty()) throw std::runtime_error("top of empty stack");
        return stack_.back();
    }

    std::vector<PObj> pop_to_mark() {
        std::vector<PObj> items;
        while (!stack_.empty() && !is_mark(stack_.back())) {
            items.push_back(stack_.back());
            stack_.pop_back();
        }
        if (stack_.empty()) throw std::runtime_error("mark not found");
        stack_.pop_back(); // remove mark
        return items;
    }


    void memo_put(size_t idx) {
        if (stack_.empty()) throw std::runtime_error("memo_put on empty stack");
        memo_[idx] = stack_.back();
    }
    PObj memo_get(size_t idx) {
        auto it = memo_.find(idx);
        if (it == memo_.end()) throw std::runtime_error("memo miss");
        return it->second;
    }

    PObj persistent_load(const PObj &pid) {
        if (auto s = std::dynamic_pointer_cast<PyStr>(pid)) {
            // PERSID gives a plain string; not expected for storage in delta.
            (void)s;
            return make_none();
        }
        auto items = as_seq(pid);
        if (items.empty()) throw std::runtime_error("persistent id empty");
        std::string tag = as_str(items[0]);
        if (tag == "storage") {
            if (items.size() != 5) throw std::runtime_error("persistent storage id must be 5-tuple");
            auto st = std::make_shared<PyStorage>();
            st->key = as_str(items[1]);
            st->cls = as_str(items[2]);
            st->loc = as_str(items[3]);
            st->numel = as_int(items[4]);
            return st;
        }
        throw std::runtime_error("unknown persistent id tag: " + tag);
    }

    std::string class_name_from_global(const PObj &o) {
        if (auto s = std::dynamic_pointer_cast<PyStr>(o)) {
            return s->v;
        }
        return "";
    }

    PObj newobj(const PObj &cls, const PObj &args) {
        std::string name = class_name_from_global(cls);
        if (name == "collections.OrderedDict") {
            auto d = std::make_shared<PyDict>();
            // if args is non-empty tuple of pairs, populate
            if (args) {
                auto t = std::dynamic_pointer_cast<PyTuple>(args);
                if (t) {
                    for (auto &it : t->v) {
                        // each item could be a (key, value) pair
                        try {
                            auto pair = as_seq(it);
                            if (pair.size() == 2) d->v[key_to_str(pair[0])] = pair[1];
                        } catch (...) {}
                    }
                }
            }
            return d;
        }
        // generic: return empty dict for dict-like classes, none otherwise
        if (name.find("Dict") != std::string::npos || name.find("dict") != std::string::npos)
            return make_dict();
        return make_none();
    }

    PObj build(const PObj &obj, const PObj &arg) {
        if (auto d = std::dynamic_pointer_cast<PyDict>(obj)) {
            auto ad = std::dynamic_pointer_cast<PyDict>(arg);
            if (ad) {
                for (auto &kv : ad->v) d->v[kv.first] = kv.second;
            }
            return obj;
        }
        return obj;
    }

    PObj reduce(const PObj &callable, const PObj &args) {
        std::string name = class_name_from_global(callable);
        std::vector<PObj> a;
        if (args) a = as_seq(args);

        if (name == "torch._utils._rebuild_tensor_v2") {
            // args: (storage, storage_offset, size, stride, requires_grad, backward_hooks)
            if (a.size() != 6) throw std::runtime_error("_rebuild_tensor_v2 needs 6 args");
            auto t = std::make_shared<PyTensor>();
            t->storage = a[0];
            t->offset = as_int(a[1]);
            t->shape = seq_to_ints(a[2]);
            t->stride = seq_to_ints(a[3]);
            return t;
        }

        if (name == "torch._utils._rebuild_tensor") {
            // older variant: (storage, storage_offset, size, stride)
            if (a.size() != 4) throw std::runtime_error("_rebuild_tensor needs 4 args");
            auto t = std::make_shared<PyTensor>();
            t->storage = a[0];
            t->offset = as_int(a[1]);
            t->shape = seq_to_ints(a[2]);
            t->stride = seq_to_ints(a[3]);
            return t;
        }

        if (name == "collections.OrderedDict" || name == "_collections.OrderedDict" ||
            name.find("OrderedDict") != std::string::npos) {
            // args is an iterable of (k,v) pairs or empty tuple
            auto d = std::make_shared<PyDict>();
            for (auto &it : a) {
                try {
                    auto pair = as_seq(it);
                    if (pair.size() == 2) d->v[key_to_str(pair[0])] = pair[1];
                } catch (...) {}
            }
            return d;
        }

        // other callables (e.g., numpy.core.multiarray._reconstruct) not expected
        return make_none();
    }

    std::vector<long long> seq_to_ints(const PObj &o) {
        auto t = std::dynamic_pointer_cast<PyTuple>(o);
        if (t) {
            std::vector<long long> r;
            for (auto &x : t->v) r.push_back(as_int(x));
            return r;
        }
        auto l = std::dynamic_pointer_cast<PyList>(o);
        if (l) {
            std::vector<long long> r;
            for (auto &x : l->v) r.push_back(as_int(x));
            return r;
        }
        return {};
    }
};

// ---------- dtype / storage helpers ----------

static std::string storage_to_dtype(const std::string &cls) {
    // strip trailing Storage or Vault if present
    std::string base = cls;
    if (base.size() > 7 && base.substr(base.size() - 7) == "Storage") base = base.substr(0, base.size() - 7);
    if (base.size() > 5 && base.substr(base.size() - 5) == "Vault") base = base.substr(0, base.size() - 5);

    if (base == "Float") return "f32";
    if (base == "Half") return "f16";
    if (base == "BFloat16") return "bf16";
    if (base == "Double") return "f64";
    if (base == "Long") return "i64";
    if (base == "Int") return "i32";
    if (base == "Short") return "i16";
    if (base == "Char") return "i8";
    if (base == "Byte") return "u8";
    if (base == "Bool") return "bool";
    if (base == "Float8_e4m3fn" || base == "Float8E4M3FN") return "f8_e4m3";
    if (base == "Float8_e5m2" || base == "Float8E5M2") return "f8_e5m2";
    throw std::runtime_error("unknown storage class: " + cls);
}

static int dtype_size(const std::string &dtype) {
    if (dtype == "f64") return 8;
    if (dtype == "f32") return 4;
    if (dtype == "f16" || dtype == "bf16") return 2;
    if (dtype == "i64") return 8;
    if (dtype == "i32") return 4;
    if (dtype == "i16") return 2;
    if (dtype == "i8" || dtype == "u8" || dtype == "bool") return 1;
    if (dtype == "f8_e4m3" || dtype == "f8_e5m2") return 1;
    throw std::runtime_error("unknown dtype: " + dtype);
}

// ---------- traversal and materialization ----------

struct TensorInfo {
    std::string path;
    std::string dtype;
    std::vector<long long> shape;
    std::vector<long long> stride;
    std::string storage_key;
    long long storage_offset;
    long long nbytes;
    std::vector<uint8_t> data;
};

static std::string tensor_bin_name(const std::string &path) {
    std::string s;
    for (char c : path) {
        if (c == '/') s += "__";
        else s.push_back(c);
    }
    return s + ".bin";
}

static void find_data_prefix(const std::unordered_map<std::string, ZipEntry> &entries,
                              std::string &pkl_name, std::string &prefix) {
    pkl_name.clear();
    for (const auto &kv : entries) {
        if (kv.first.size() >= 8 && kv.first.substr(kv.first.size() - 8) == "data.pkl") {
            if (kv.first == "data.pkl") {
                pkl_name = kv.first;
                prefix = "";
            } else if (kv.first.size() > 9 && kv.first[kv.first.size() - 9] == '/') {
                pkl_name = kv.first;
                prefix = kv.first.substr(0, kv.first.size() - 9);
            }
            break;
        }
    }
    if (pkl_name.empty()) throw std::runtime_error("data.pkl not found in archive");
}

static std::string storage_entry_name(const std::string &prefix, const std::string &key) {
    if (prefix.empty()) return "data/" + key;
    return prefix + "/data/" + key;
}

static std::vector<uint8_t> materialize(const std::string &storage_bytes,
                                         const std::string &dtype,
                                         long long offset,
                                         const std::vector<long long> &shape,
                                         const std::vector<long long> &stride) {
    int itemsize = dtype_size(dtype);
    long long nelem = 1;
    for (long long s : shape) nelem *= s;
    long long nbytes = nelem * itemsize;
    std::vector<uint8_t> out(nbytes);
    if (nelem <= 0) return out;

    int ndim = static_cast<int>(shape.size());
    std::vector<long long> idx(ndim, 0);
    for (long long lin = 0; lin < nelem; ++lin) {
        // compute row-major multi-index
        long long tmp = lin;
        for (int i = ndim - 1; i >= 0; --i) {
            idx[i] = tmp % shape[i];
            tmp /= shape[i];
        }
        long long src_elem = offset;
        for (int i = 0; i < ndim; ++i) src_elem += idx[i] * stride[i];
        if (src_elem < 0 || (src_elem + 1) * itemsize > static_cast<long long>(storage_bytes.size()))
            throw std::runtime_error("storage index out of bounds");
        std::memcpy(out.data() + lin * itemsize,
                    storage_bytes.data() + src_elem * itemsize,
                    itemsize);
    }
    return out;
}

static void collect_tensors(const PObj &obj, const std::string &prefix,
                            std::vector<TensorInfo> &out,
                            const std::string &storage_prefix,
                            const ZipArchive &zip) {
    if (auto t = std::dynamic_pointer_cast<PyTensor>(obj)) {
        auto st = std::dynamic_pointer_cast<PyStorage>(t->storage);
        if (!st) {
            auto ss = std::dynamic_pointer_cast<PyStr>(t->storage);
            if (ss) throw std::runtime_error("unexpected string storage");
            return;
        }
        TensorInfo info;
        info.path = prefix;
        info.dtype = storage_to_dtype(st->cls);
        info.shape = t->shape;
        info.stride = t->stride;
        info.storage_key = st->key;
        info.storage_offset = t->offset;
        long long nelem = 1;
        for (long long s : info.shape) nelem *= s;
        info.nbytes = nelem * dtype_size(info.dtype);
        std::string sname = storage_entry_name(storage_prefix, st->key);
        std::string storage_data = zip.read_entry(sname);
        info.data = materialize(storage_data, info.dtype, info.storage_offset,
                                info.shape, info.stride);
        if ((long long)info.data.size() != info.nbytes)
            throw std::runtime_error("materialized size mismatch for " + prefix);
        out.push_back(std::move(info));
    } else if (auto d = std::dynamic_pointer_cast<PyDict>(obj)) {
        std::vector<std::string> keys;
        for (const auto &kv : d->v) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (const std::string &k : keys) {
            std::string np = prefix.empty() ? k : prefix + "/" + k;
            collect_tensors(d->v.at(k), np, out, storage_prefix, zip);
        }
    } else if (auto l = std::dynamic_pointer_cast<PyList>(obj)) {
        for (size_t i = 0; i < l->v.size(); ++i) {
            std::string np = prefix.empty() ? std::to_string(i) : prefix + "/" + std::to_string(i);
            collect_tensors(l->v[i], np, out, storage_prefix, zip);
        }
    } else if (auto t = std::dynamic_pointer_cast<PyTuple>(obj)) {
        for (size_t i = 0; i < t->v.size(); ++i) {
            std::string np = prefix.empty() ? std::to_string(i) : prefix + "/" + std::to_string(i);
            collect_tensors(t->v[i], np, out, storage_prefix, zip);
        }
    }
}

// ---------- manifest and output ----------

static std::string manifest_json(const std::vector<TensorInfo> &tensors) {
    // sort by path
    std::vector<const TensorInfo *> sorted;
    for (const auto &t : tensors) sorted.push_back(&t);
    std::sort(sorted.begin(), sorted.end(), [](const TensorInfo *a, const TensorInfo *b) { return a->path < b->path; });

    std::ostringstream o;
    o << "{\n";
    o << " \"byteorder\": \"little\",\n";
    o << " \"nulltorch_manifest\": 1,\n";
    o << " \"tensors\": {\n";
    for (size_t i = 0; i < sorted.size(); ++i) {
        const TensorInfo &t = *sorted[i];
        o << "  " << json_escape(t.path) << ": {\n";
        o << "   \"dtype\": " << json_escape(t.dtype) << ",\n";
        o << "   \"nbytes\": " << t.nbytes << ",\n";
        o << "   \"shape\": [";
        for (size_t j = 0; j < t.shape.size(); ++j) {
            if (j) o << ", ";
            o << t.shape[j];
        }
        o << "],\n";
        o << "   \"storage_key\": " << json_escape(t.storage_key) << ",\n";
        o << "   \"storage_offset\": " << t.storage_offset << ",\n";
        o << "   \"stride\": [";
        for (size_t j = 0; j < t.stride.size(); ++j) {
            if (j) o << ", ";
            o << t.stride[j];
        }
        o << "]\n";
        o << "  }";
        if (i + 1 < sorted.size()) o << ",";
        o << "\n";
    }
    o << " }\n";
    o << "}\n";
    return o.str();
}

// ---------- main ----------

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "usage: " << argv[0] << " <file.pth> <out_dir>\n";
            return 1;
        }

        std::string in_path = argv[1];
        std::string out_dir = argv[2];

        std::string archive_data = read_file(in_path);
        ZipArchive zip(archive_data);
        zip.parse();

        std::string pkl_name, data_prefix;
        find_data_prefix(zip.entries(), pkl_name, data_prefix);

        std::string pkl_data = zip.read_entry(pkl_name);

        PickleMachine pm(pkl_data);
        PObj root = pm.run();

        std::vector<TensorInfo> tensors;
        collect_tensors(root, "", tensors, data_prefix, zip);

        fs::create_directories(fs::path(out_dir) / "tensors");

        for (const auto &t : tensors) {
            std::string bin_path = (fs::path(out_dir) / "tensors" / tensor_bin_name(t.path)).string();
            write_file(bin_path, t.data);
        }

        std::string manifest = manifest_json(tensors);
        write_file((fs::path(out_dir) / "manifest.json").string(), manifest);

        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
