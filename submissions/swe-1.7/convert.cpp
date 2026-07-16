// NullTorch PTH reader — C++ standard library only.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

[[noreturn]] static void fail(const std::string& msg) {
    throw std::runtime_error(msg);
}

static uint16_t le_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

static int32_t le_i32(const uint8_t* p) {
    return (int32_t)le_u32(p);
}

static double be_double(const uint8_t* p) {
    uint8_t rev[8];
    for (int i = 0; i < 8; ++i) rev[i] = p[7 - i];
    double d;
    std::memcpy(&d, rev, 8);
    return d;
}

static int64_t pyprod(const std::vector<int64_t>& shape) {
    int64_t n = 1;
    for (int64_t s : shape) n *= s;
    return n;
}

// ---------------------------------------------------------------------------
// Dtype / storage class mapping
// ---------------------------------------------------------------------------

static const std::unordered_map<std::string, std::pair<std::string, int>>& storage_map() {
    static const std::unordered_map<std::string, std::pair<std::string, int>> m = {
        {"DoubleStorage", {"f64", 8}},
        {"FloatStorage", {"f32", 4}},
        {"HalfStorage", {"f16", 2}},
        {"BFloat16Storage", {"bf16", 2}},
        {"Float8_e4m3fnStorage", {"f8_e4m3", 1}},
        {"Float8_e5m2Storage", {"f8_e5m2", 1}},
        {"LongStorage", {"i64", 8}},
        {"IntStorage", {"i32", 4}},
        {"ShortStorage", {"i16", 2}},
        {"CharStorage", {"i8", 1}},
        {"ByteStorage", {"u8", 1}},
        {"BoolStorage", {"bool", 1}},
    };
    return m;
}

static std::string dtype_from_storage(const std::string& cls_name) {
    auto it = storage_map().find(cls_name);
    if (it == storage_map().end()) fail("unknown storage class: " + cls_name);
    return it->second.first;
}

static int dtype_itemsize(const std::string& dtype) {
    for (const auto& kv : storage_map()) {
        if (kv.second.first == dtype) return kv.second.second;
    }
    fail("unknown dtype: " + dtype);
    return 0;
}

// ---------------------------------------------------------------------------
// Value model
// ---------------------------------------------------------------------------

struct GlobalInfo {
    std::string module;
    std::string name;
};

struct StorageInfo {
    std::string cls_name;     // e.g. "FloatStorage"
    std::string key;
    std::string device;
    int64_t numel = 0;
};

struct TensorInfo {
    std::string dtype;        // e.g. "f32"
    std::vector<int64_t> shape;
    std::vector<int64_t> stride;
    std::string storage_key;
    int64_t storage_offset = 0;
    int64_t nbytes() const {
        int64_t n = 1;
        for (int64_t s : shape) n *= s;
        return n * dtype_itemsize(dtype);
    }
};

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    enum Type { NONE, BOOL, INT, FLOAT, STRING, GLOBAL, STORAGE, TENSOR, TUPLE, LIST, DICT, MARK } type = NONE;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    GlobalInfo global;
    StorageInfo storage;
    TensorInfo tensor;
    std::shared_ptr<std::vector<Value>> seq;
    std::shared_ptr<std::map<std::string, Value>> dict;

    static Value make_none() { Value v; v.type = NONE; return v; }
    static Value make_bool(bool x) { Value v; v.type = BOOL; v.b = x; return v; }
    static Value make_int(int64_t x) { Value v; v.type = INT; v.i = x; return v; }
    static Value make_float(double x) { Value v; v.type = FLOAT; v.f = x; return v; }
    static Value make_string(const std::string& x) { Value v; v.type = STRING; v.s = x; return v; }
    static Value make_global(const std::string& m, const std::string& n) {
        Value v; v.type = GLOBAL; v.global = {m, n}; return v;
    }
    static Value make_storage(const StorageInfo& si) { Value v; v.type = STORAGE; v.storage = si; return v; }
    static Value make_tensor(const TensorInfo& ti) { Value v; v.type = TENSOR; v.tensor = ti; return v; }
    static Value make_tuple() { Value v; v.type = TUPLE; v.seq = std::make_shared<std::vector<Value>>(); return v; }
    static Value make_list() { Value v; v.type = LIST; v.seq = std::make_shared<std::vector<Value>>(); return v; }
    static Value make_dict() { Value v; v.type = DICT; v.dict = std::make_shared<std::map<std::string, Value>>(); return v; }
    static Value make_mark() { Value v; v.type = MARK; return v; }
};

static bool is_mark(const Value& v) { return v.type == Value::MARK; }

// ---------------------------------------------------------------------------
// ZIP archive reader
// ---------------------------------------------------------------------------

struct ZipEntry {
    std::string name;
    uint32_t method = 0;
    uint64_t usize = 0;
    uint64_t csize = 0;
    uint64_t local_header_offset = 0;
};

static std::vector<uint8_t> inflate_deflate(const std::vector<uint8_t>& in, uint64_t expected_size);

class ZipArchive {
    std::vector<uint8_t> data_;
    std::unordered_map<std::string, ZipEntry> entries_;

    const uint8_t* ptr(size_t off) const {
        if (off >= data_.size()) fail("zip read out of bounds");
        return data_.data() + off;
    }

public:
    explicit ZipArchive(std::vector<uint8_t> data) : data_(std::move(data)) {
        if (data_.size() < 22) fail("zip too small");
        find_eocd_and_cd();
    }

    void find_eocd_and_cd() {
        size_t eocd_pos = (size_t)-1;
        // Scan backwards for EOCD signature within last 65557 bytes.
        size_t start = data_.size() > 65557 ? data_.size() - 65557 : 0;
        for (size_t i = data_.size() - 22; i + 22 <= data_.size() && i >= start; --i) {
            if (le_u32(ptr(i)) == 0x06054b50) {
                uint16_t comment_len = le_u16(ptr(i + 20));
                if (i + 22 + comment_len == data_.size()) {
                    eocd_pos = i;
                    break;
                }
            }
            if (i == 0) break;
        }
        if (eocd_pos == (size_t)-1) fail("EOCD not found");

        uint16_t disk = le_u16(ptr(eocd_pos + 4));
        uint16_t cd_disk = le_u16(ptr(eocd_pos + 6));
        uint16_t cd_count_disk = le_u16(ptr(eocd_pos + 8));
        uint16_t cd_total = le_u16(ptr(eocd_pos + 10));
        uint32_t cd_size = le_u32(ptr(eocd_pos + 12));
        uint32_t cd_offset = le_u32(ptr(eocd_pos + 16));
        uint16_t comment_len = le_u16(ptr(eocd_pos + 20));

        uint64_t zip64_eocd_offset = 0;
        bool has_zip64_eocd = false;
        if (disk == 0xFFFF || cd_disk == 0xFFFF || cd_count_disk == 0xFFFF || cd_total == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF) {
            // Read zip64 EOCD locator at eocd_pos - 20.
            if (eocd_pos < 20) fail("zip64 locator missing");
            const uint8_t* loc = ptr(eocd_pos - 20);
            if (le_u32(loc) != 0x07064b50) fail("zip64 locator signature mismatch");
            zip64_eocd_offset = le_u64(loc + 8);
            has_zip64_eocd = true;
        }

        uint64_t num_entries = cd_total;
        uint64_t cd_start = cd_offset;
        uint64_t cd_len = cd_size;
        if (has_zip64_eocd) {
            if (zip64_eocd_offset + 56 > data_.size()) fail("zip64 EOCD out of bounds");
            const uint8_t* z64 = ptr(zip64_eocd_offset);
            if (le_u32(z64) != 0x06064b50) fail("zip64 EOCD signature mismatch");
            num_entries = le_u64(z64 + 32);
            cd_len = le_u64(z64 + 40);
            cd_start = le_u64(z64 + 48);
        }

        read_central_dir(cd_start, cd_len, num_entries);
    }

    void read_central_dir(uint64_t off, uint64_t len, uint64_t count) {
        if (count > 100000 || off > data_.size() || len > data_.size()) fail("zip central directory too large");
        size_t pos = (size_t)off;
        (void)len;
        for (uint64_t e = 0; e < count; ++e) {
            if (pos + 46 > data_.size()) fail("central directory truncated");
            const uint8_t* h = ptr(pos);
            if (le_u32(h) != 0x02014b50) fail("central directory signature mismatch");
            uint16_t flags = le_u16(h + 8);
            uint16_t method = le_u16(h + 10);
            uint32_t csize32 = le_u32(h + 20);
            uint32_t usize32 = le_u32(h + 24);
            uint16_t nlen = le_u16(h + 28);
            uint16_t elen = le_u16(h + 30);
            uint16_t clen = le_u16(h + 32);
            uint16_t disk = le_u16(h + 34);
            uint32_t lhoff32 = le_u32(h + 42);

            std::string name((const char*)ptr(pos + 46), nlen);
            uint64_t csize = csize32;
            uint64_t usize = usize32;
            uint64_t lhoff = lhoff32;

            // Parse zip64 extended information extra field if needed.
            if (elen > 0) {
                size_t ex = pos + 46 + nlen;
                size_t ex_end = ex + elen;
                while (ex + 4 <= ex_end) {
                    uint16_t id = le_u16(ptr(ex));
                    uint16_t sz = le_u16(ptr(ex + 2));
                    if (id == 0x0001) {
                        size_t p = ex + 4;
                        if (usize32 == 0xFFFFFFFF && p + 8 <= ex_end) { usize = le_u64(ptr(p)); p += 8; }
                        if (csize32 == 0xFFFFFFFF && p + 8 <= ex_end) { csize = le_u64(ptr(p)); p += 8; }
                        if (lhoff32 == 0xFFFFFFFF && p + 8 <= ex_end) { lhoff = le_u64(ptr(p)); p += 8; }
                        if (disk == 0xFFFF && p + 4 <= ex_end) { p += 4; }
                        break;
                    }
                    ex += 4 + sz;
                }
            }

            entries_[name] = ZipEntry{name, method, usize, csize, lhoff};
            pos += 46 + nlen + elen + clen;
        }
    }

    std::vector<uint8_t> read_entry(const std::string& name) const {
        static constexpr uint64_t MAX_ENTRY_BYTES = 512ULL * 1024 * 1024;
        auto it = entries_.find(name);
        if (it == entries_.end()) fail("zip entry not found: " + name);
        const ZipEntry& ent = it->second;
        if (ent.usize > MAX_ENTRY_BYTES) fail("zip entry too large: " + name);
        if (ent.local_header_offset + 30 > data_.size()) fail("local header out of bounds");
        const uint8_t* lh = ptr(ent.local_header_offset);
        if (le_u32(lh) != 0x04034b50) fail("local header signature mismatch");
        uint16_t nlen = le_u16(lh + 26);
        uint16_t elen = le_u16(lh + 28);
        size_t data_off = (size_t)ent.local_header_offset + 30 + nlen + elen;
        if (data_off + ent.csize > data_.size()) fail("entry data out of bounds");

        if (ent.method == 0) {
            return std::vector<uint8_t>(ptr(data_off), ptr(data_off + ent.usize));
        } else if (ent.method == 8) {
            std::vector<uint8_t> comp(ptr(data_off), ptr(data_off + ent.csize));
            return inflate_deflate(comp, ent.usize);
        } else {
            fail("unsupported compression method");
        }
    }

    const std::unordered_map<std::string, ZipEntry>& entries() const { return entries_; }
};

// ---------------------------------------------------------------------------
// DEFLATE decompressor
// ---------------------------------------------------------------------------

struct BitReader {
    const std::vector<uint8_t>& data;
    size_t byte_pos = 0;
    uint64_t cache = 0;
    int cache_bits = 0;

    BitReader(const std::vector<uint8_t>& d) : data(d) {}

    void ensure(int n) {
        while (cache_bits < n && byte_pos < data.size()) {
            cache |= ((uint64_t)data[byte_pos++]) << cache_bits;
            cache_bits += 8;
        }
    }
    int read_bit() {
        ensure(1);
        if (cache_bits == 0) fail("deflate bit stream exhausted");
        int bit = cache & 1;
        cache >>= 1;
        cache_bits--;
        return bit;
    }
    uint32_t read_bits_lsb(int n) {
        if (n == 0) return 0;
        ensure(n);
        uint32_t mask = (1ULL << n) - 1;
        uint32_t v = cache & mask;
        cache >>= n;
        cache_bits -= n;
        return v;
    }
    uint32_t peek_msb(int n) {
        ensure(n);
        uint64_t mask = (1ULL << n) - 1;
        uint64_t v = cache & mask;
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) {
            r = (r << 1) | (v & 1);
            v >>= 1;
        }
        return r;
    }
    void consume(int n) {
        cache >>= n;
        cache_bits -= n;
    }
    void align_to_byte() {
        consume(cache_bits % 8);
    }
};

struct HuffmanTable {
    int max_bits = 0;
    std::vector<std::pair<int,int>> table; // (symbol, length)

    void build(const std::vector<int>& lengths) {
        max_bits = 0;
        for (int l : lengths) if (l > max_bits) max_bits = l;
        if (max_bits == 0) return;
        int size = 1 << max_bits;
        table.assign(size, {-1, 0});
        std::vector<int> bl_count(max_bits + 1, 0);
        for (int l : lengths) if (l > 0) bl_count[l]++;
        std::vector<int> next_code(max_bits + 1, 0);
        int code = 0;
        bl_count[0] = 0;
        for (int bits = 1; bits <= max_bits; ++bits) {
            code = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
        }
        for (int sym = 0; sym < (int)lengths.size(); ++sym) {
            int l = lengths[sym];
            if (l == 0) continue;
            int c = next_code[l]++;
            int start = c << (max_bits - l);
            int end = start + (1 << (max_bits - l));
            for (int i = start; i < end; ++i) table[i] = {sym, l};
        }
    }
    int decode(BitReader& br) const {
        if (max_bits == 0) fail("empty huffman table");
        br.ensure(max_bits);
        uint32_t v = br.peek_msb(max_bits);
        if (v >= table.size()) fail("huffman peek out of range");
        auto [sym, len] = table[v];
        if (len <= 0) fail("invalid huffman code");
        br.consume(len);
        return sym;
    }
};

static std::vector<uint8_t> inflate_deflate(const std::vector<uint8_t>& in, uint64_t expected_size) {
    BitReader br(in);
    std::vector<uint8_t> out;
    out.reserve((size_t)expected_size);

    static const int length_base[] = {
        3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
        35,43,51,59, 67,83,99,115, 131,163,195,227, 258
    };
    static const int length_extra[] = {
        0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
        3,3,3,3, 4,4,4,4, 5,5,5,5, 0
    };
    static const int dist_base[] = {
        1,2,3,4, 5,7, 9,13, 17,25, 33,49, 65,97, 129,193,
        257,385, 513,769, 1025,1537, 2049,3073, 4097,6145, 8193,12289, 16385,24577
    };
    static const int dist_extra[] = {
        0,0,0,0, 1,1, 2,2, 3,3, 4,4, 5,5, 6,6,
        7,7, 8,8, 9,9, 10,10, 11,11, 12,12, 13,13
    };

    bool bfinal = false;
    while (!bfinal) {
        bfinal = br.read_bit() != 0;
        uint32_t btype = br.read_bits_lsb(2);
        if (btype == 0) {
            br.align_to_byte();
            uint32_t len = br.read_bits_lsb(16);
            uint32_t nlen = br.read_bits_lsb(16);
            if ((len ^ nlen) != 0xFFFFU) fail("deflate LEN/NLEN mismatch");
            int k = br.cache_bits / 8;
            for (int i = 0; i < k; ++i) out.push_back((br.cache >> (8*i)) & 0xFF);
            uint32_t remain = (len > (uint32_t)k) ? len - k : 0;
            if (br.byte_pos + remain > in.size()) fail("deflate stored data truncated");
            out.insert(out.end(), in.begin() + br.byte_pos, in.begin() + br.byte_pos + remain);
            br.byte_pos += remain;
            br.cache = 0;
            br.cache_bits = 0;
        } else if (btype == 1 || btype == 2) {
            std::vector<int> litlen_lengths, dist_lengths;
            if (btype == 1) {
                litlen_lengths.resize(288);
                for (int i=0;i<=143;++i) litlen_lengths[i]=8;
                for (int i=144;i<=255;++i) litlen_lengths[i]=9;
                for (int i=256;i<=279;++i) litlen_lengths[i]=7;
                for (int i=280;i<=287;++i) litlen_lengths[i]=8;
                dist_lengths.assign(30, 5);
            } else {
                uint32_t hlit = br.read_bits_lsb(5);
                uint32_t hdist = br.read_bits_lsb(5);
                uint32_t hclen = br.read_bits_lsb(4);
                std::vector<int> cl_lengths(19, 0);
                const int cl_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                for (uint32_t i = 0; i < hclen + 4; ++i) {
                    cl_lengths[cl_order[i]] = (int)br.read_bits_lsb(3);
                }
                HuffmanTable cl_table;
                cl_table.build(cl_lengths);
                uint32_t total = hlit + 257 + hdist + 1;
                while (litlen_lengths.size() < total) {
                    int sym = cl_table.decode(br);
                    if (sym < 16) {
                        litlen_lengths.push_back(sym);
                    } else if (sym == 16) {
                        if (litlen_lengths.empty()) fail("deflate repeat with no previous");
                        int prev = litlen_lengths.back();
                        int n = 3 + (int)br.read_bits_lsb(2);
                        for (int i = 0; i < n; ++i) litlen_lengths.push_back(prev);
                    } else if (sym == 17) {
                        int n = 3 + (int)br.read_bits_lsb(3);
                        for (int i = 0; i < n; ++i) litlen_lengths.push_back(0);
                    } else if (sym == 18) {
                        int n = 11 + (int)br.read_bits_lsb(7);
                        for (int i = 0; i < n; ++i) litlen_lengths.push_back(0);
                    }
                }
                if (litlen_lengths.size() < hlit + 257) fail("deflate litlen lengths too short");
                dist_lengths.assign(litlen_lengths.begin() + hlit + 257, litlen_lengths.end());
                litlen_lengths.resize(hlit + 257);
            }
            HuffmanTable litlen_table, dist_table;
            litlen_table.build(litlen_lengths);
            dist_table.build(dist_lengths);
            while (true) {
                int sym = litlen_table.decode(br);
                if (sym < 256) {
                    out.push_back((uint8_t)sym);
                } else if (sym == 256) {
                    break;
                } else {
                    int li = sym - 257;
                    if (li < 0 || li >= 29) fail("deflate bad length symbol");
                    int length = length_base[li] + (int)br.read_bits_lsb(length_extra[li]);
                    int dist_sym = dist_table.decode(br);
                    if (dist_sym < 0 || dist_sym >= 30) fail("deflate bad distance symbol");
                    int distance = dist_base[dist_sym] + (int)br.read_bits_lsb(dist_extra[dist_sym]);
                    if (distance > (int)out.size()) fail("deflate distance too far");
                    size_t start = out.size() - (size_t)distance;
                    for (int i = 0; i < length; ++i) {
                        out.push_back(out[start + i]);
                    }
                }
            }
        } else {
            fail("deflate invalid BTYPE");
        }
    }
    if (out.size() != (size_t)expected_size) {
        // Some valid streams may have extra bits; trust expected size if we have at least that much.
        if (out.size() < (size_t)expected_size) fail("deflate output size mismatch");
        out.resize((size_t)expected_size);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Pickle parser
// ---------------------------------------------------------------------------

class PickleParser {
    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
    std::vector<Value> stack_;
    std::unordered_map<int64_t, Value> memo_;
    int64_t memo_next_ = 0;

    static constexpr size_t MAX_STR_LEN = 16 * 1024 * 1024;

    const uint8_t* ptr(size_t n) {
        if (pos_ + n > data_.size()) fail("pickle truncated");
        const uint8_t* p = data_.data() + pos_;
        pos_ += n;
        return p;
    }

    uint8_t get() { return *ptr(1); }

    std::string read_nl_string() {
        size_t start = pos_;
        while (pos_ < data_.size() && data_[pos_] != '\n') ++pos_;
        if (pos_ >= data_.size()) fail("newline-terminated string not found");
        size_t len = pos_ - start;
        if (len > MAX_STR_LEN) fail("string too large");
        std::string s((const char*)data_.data() + start, len);
        ++pos_; // consume '\n'
        return s;
    }

    std::string read_utf8_string(uint64_t len) {
        if (len > MAX_STR_LEN) fail("string too large");
        if (pos_ + len > data_.size()) fail("string data truncated");
        std::string s((const char*)data_.data() + pos_, (size_t)len);
        pos_ += len;
        return s;
    }

    void push(const Value& v) { stack_.push_back(v); }
    Value pop() {
        if (stack_.empty()) fail("pickle stack underflow");
        Value v = stack_.back();
        stack_.pop_back();
        return v;
    }
    Value& top() {
        if (stack_.empty()) fail("pickle stack underflow");
        return stack_.back();
    }
    void memo_put(Value v, int64_t idx) {
        if (idx < 0) fail("negative memo index");
        if (idx > 1000000) fail("memo index too large");
        memo_[idx] = std::move(v);
        if (idx >= memo_next_) memo_next_ = idx + 1;
    }

    Value memo_get(int64_t idx) {
        if (idx < 0 || idx >= memo_next_) fail("memo index out of range");
        auto it = memo_.find(idx);
        if (it == memo_.end()) return Value::make_none();
        return it->second;
    }


    // Pop all values above the nearest mark; remove mark; return items in order.
    std::vector<Value> pop_to_mark() {
        std::vector<Value> out;
        while (!stack_.empty()) {
            Value v = pop();
            if (v.type == Value::MARK) break;
            out.push_back(std::move(v));
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    // Pop the mark without collecting items.
    void pop_mark() {
        while (!stack_.empty()) {
            if (pop().type == Value::MARK) return;
        }
        fail("mark not found");
    }

    Value apply_reduce(const Value& callable, const std::vector<Value>& args) {
        if (callable.type == Value::GLOBAL) {
            const std::string& m = callable.global.module;
            const std::string& n = callable.global.name;
            // torch._utils._rebuild_tensor_v2
            if (m == "torch._utils" && n == "_rebuild_tensor_v2") {
                if (args.size() < 6) fail("_rebuild_tensor_v2 needs 6 args");
                const Value& st = args[0];
                if (st.type != Value::STORAGE) fail("rebuild: first arg not storage");
                int64_t offset = args[1].i;
                std::vector<int64_t> shape, stride;
                if (args[2].type == Value::TUPLE) {
                    for (const Value& x : *args[2].seq) {
                        if (x.type != Value::INT) fail("shape element not int");
                        shape.push_back(x.i);
                    }
                } else if (args[2].type != Value::NONE) {
                    fail("rebuild: size not tuple");
                }
                if (args[3].type == Value::TUPLE) {
                    for (const Value& x : *args[3].seq) {
                        if (x.type != Value::INT) fail("stride element not int");
                        stride.push_back(x.i);
                    }
                } else if (args[3].type != Value::NONE) {
                    fail("rebuild: stride not tuple");
                }
                TensorInfo ti;
                ti.dtype = dtype_from_storage(st.storage.cls_name);
                ti.shape = shape;
                ti.stride = stride;
                ti.storage_key = st.storage.key;
                ti.storage_offset = offset;
                return Value::make_tensor(ti);
            }
            // collections.OrderedDict -> empty dict
            if (m == "collections" && n == "OrderedDict") {
                if (args.size() == 1 && args[0].type == Value::LIST) {
                    Value d = Value::make_dict();
                    for (const Value& pair : *args[0].seq) {
                        if (pair.type != Value::TUPLE || pair.seq->size() != 2) fail("OrderedDict bad pair");
                        const Value& k = (*pair.seq)[0];
                        const Value& v = (*pair.seq)[1];
                        if (k.type != Value::STRING) fail("OrderedDict key not string");
                        (*d.dict)[k.s] = v;
                    }
                    return d;
                }
                return Value::make_dict();
            }
            // Other callables -> skip as opaque.
        }
        return Value::make_none();
    }

    Value apply_binpersid(const Value& pid) {
        if (pid.type != Value::TUPLE || pid.seq->size() != 5) fail("persistent_id not 5-tuple");
        const Value& a0 = (*pid.seq)[0];
        const Value& a1 = (*pid.seq)[1];
        const Value& a2 = (*pid.seq)[2];
        const Value& a3 = (*pid.seq)[3];
        const Value& a4 = (*pid.seq)[4];
        if (a0.type != Value::STRING || a0.s != "storage") fail("persistent_id not storage");
        if (a1.type != Value::GLOBAL) fail("persistent_id storage class not global");
        if (a2.type != Value::STRING) fail("persistent_id key not string");
        if (a3.type != Value::STRING) fail("persistent_id device not string");
        if (a4.type != Value::INT) fail("persistent_id numel not int");
        StorageInfo si;
        si.cls_name = a1.global.name;
        si.key = a2.s;
        si.device = a3.s;
        si.numel = a4.i;
        return Value::make_storage(si);
    }

public:
    explicit PickleParser(const std::vector<uint8_t>& d) : data_(d) {}

    Value parse() {
        while (pos_ < data_.size()) {
            uint8_t op = get();
            switch (op) {
                case 0x28: // MARK
                    push(Value::make_mark());
                    break;
                case 0x29: // EMPTY_TUPLE
                    push(Value::make_tuple());
                    break;
                case 0x5d: // EMPTY_LIST
                    push(Value::make_list());
                    break;
                case 0x7d: // EMPTY_DICT
                    push(Value::make_dict());
                    break;
                case 0x4e: // NONE
                    push(Value::make_none());
                    break;
                case 0x88: // NEWTRUE
                    push(Value::make_bool(true));
                    break;
                case 0x89: // NEWFALSE
                    push(Value::make_bool(false));
                    break;
                case 0x4b: { // BININT1
                    push(Value::make_int((uint8_t)get()));
                    break;
                }
                case 0x4d: { // BININT2
                    uint16_t v = le_u16(ptr(2));
                    push(Value::make_int(v));
                    break;
                }
                case 0x4a: { // BININT
                    int32_t v = le_i32(ptr(4));
                    push(Value::make_int(v));
                    break;
                }
                case 0x47: { // BINFLOAT
                    double v = be_double(ptr(8));
                    push(Value::make_float(v));
                    break;
                }
                case 0x49: { // INT (decimal with newline)
                    std::string s = read_nl_string();
                    int64_t v = std::stoll(s);
                    push(Value::make_int(v));
                    break;
                }
                case 0x4c: { // LONG (decimal with L and newline)
                    std::string s = read_nl_string();
                    if (!s.empty() && s.back() == 'L') s.pop_back();
                    int64_t v = std::stoll(s);
                    push(Value::make_int(v));
                    break;
                }
                case 0x8a: { // LONG1
                    uint8_t n = get();
                    if (n == 0) { push(Value::make_int(0)); break; }
                    const uint8_t* p = ptr(n);
                    uint64_t v = 0;
                    for (int i = 0; i < n; ++i) v |= ((uint64_t)p[i]) << (8 * i);
                    if (v > (uint64_t)INT64_MAX) fail("LONG1 overflow");
                    push(Value::make_int((int64_t)v));
                    break;
                }
                case 0x8b: { // LONG4
                    uint32_t n = le_u32(ptr(4));
                    if (n == 0) { push(Value::make_int(0)); break; }
                    if (n > 8) fail("LONG4 too large");
                    const uint8_t* p = ptr(n);
                    uint64_t v = 0;
                    for (uint32_t i = 0; i < n; ++i) v |= ((uint64_t)p[i]) << (8 * i);
                    if (v > (uint64_t)INT64_MAX) fail("LONG4 overflow");
                    push(Value::make_int((int64_t)v));
                    break;
                }
                case 0x46: { // FLOAT (newline-terminated decimal)
                    std::string s = read_nl_string();
                    push(Value::make_float(std::stod(s)));
                    break;
                }
                case 0x58: { // BINUNICODE
                    uint32_t len = le_u32(ptr(4));
                    push(Value::make_string(read_utf8_string(len)));
                    break;
                }
                case 0x8c: { // SHORT_BINUNICODE
                    uint8_t len = get();
                    push(Value::make_string(read_utf8_string(len)));
                    break;
                }
                case 0x8d: { // BINUNICODE8
                    uint64_t len = le_u64(ptr(8));
                    push(Value::make_string(read_utf8_string(len)));
                    break;
                }
                case 0x56: { // UNICODE raw-unicode-escape up to newline
                    std::string s = read_nl_string();
                    push(Value::make_string(s));
                    break;
                }
                case 0x53: { // STRING repr-style up to newline
                    std::string s = read_nl_string();
                    push(Value::make_string(s));
                    break;
                }
                case 0x54: { // BINSTRING
                    int32_t len = (int32_t)le_u32(ptr(4));
                    if (len < 0) fail("BINSTRING negative length");
                    push(Value::make_string(read_utf8_string((uint32_t)len)));
                    break;
                }
                case 0x55: { // SHORT_BINSTRING
                    uint8_t len = get();
                    push(Value::make_string(read_utf8_string(len)));
                    break;
                }
                case 0x42: { // BINBYTES
                    uint32_t len = le_u32(ptr(4));
                    push(Value::make_string(read_utf8_string(len)));
                    break;
                }
                case 0x43: { // SHORT_BINBYTES
                    uint8_t len = get();
                    push(Value::make_string(read_utf8_string(len)));
                    break;
                }
                case 0x8e: { // BINBYTES8
                    uint64_t len = le_u64(ptr(8));
                    push(Value::make_string(read_utf8_string(len)));
                    break;
                }
                case 0x63: { // GLOBAL
                    std::string module = read_nl_string();
                    std::string name = read_nl_string();
                    push(Value::make_global(module, name));
                    break;
                }
                case 0x93: { // STACK_GLOBAL
                    Value name = pop();
                    Value module = pop();
                    if (module.type != Value::STRING || name.type != Value::STRING) fail("STACK_GLOBAL needs two strings");
                    push(Value::make_global(module.s, name.s));
                    break;
                }
                case 0x74: { // TUPLE (from mark)
                    auto items = pop_to_mark();
                    Value t = Value::make_tuple();
                    *t.seq = std::move(items);
                    push(std::move(t));
                    break;
                }
                case 0x85: { // TUPLE1
                    Value a = pop();
                    Value t = Value::make_tuple();
                    t.seq->push_back(std::move(a));
                    push(std::move(t));
                    break;
                }
                case 0x86: { // TUPLE2
                    Value b = pop(); // top
                    Value a = pop();
                    Value t = Value::make_tuple();
                    t.seq->reserve(2);
                    t.seq->push_back(std::move(a));
                    t.seq->push_back(std::move(b));
                    push(std::move(t));
                    break;
                }
                case 0x87: { // TUPLE3
                    Value c = pop();
                    Value b = pop();
                    Value a = pop();
                    Value t = Value::make_tuple();
                    t.seq->reserve(3);
                    t.seq->push_back(std::move(a));
                    t.seq->push_back(std::move(b));
                    t.seq->push_back(std::move(c));
                    push(std::move(t));
                    break;
                }
                case 0x6c: { // LIST (from mark)
                    auto items = pop_to_mark();
                    Value l = Value::make_list();
                    *l.seq = std::move(items);
                    push(std::move(l));
                    break;
                }
                case 0x64: { // DICT (from mark)
                    auto items = pop_to_mark();
                    Value d = Value::make_dict();
                    if (items.size() % 2 != 0) fail("DICT items not paired");
                    for (size_t i = 0; i < items.size(); i += 2) {
                        if (items[i].type != Value::STRING) fail("DICT key not string");
                        (*d.dict)[items[i].s] = std::move(items[i + 1]);
                    }
                    push(std::move(d));
                    break;
                }
                case 0x61: { // APPEND
                    Value v = pop();
                    Value l = pop();
                    if (l.type != Value::LIST) fail("APPEND target not list");
                    l.seq->push_back(std::move(v));
                    push(std::move(l));
                    break;
                }
                case 0x65: { // APPENDS
                    auto items = pop_to_mark();
                    Value l = pop();
                    if (l.type != Value::LIST) fail("APPENDS target not list");
                    for (auto& x : items) l.seq->push_back(std::move(x));
                    push(std::move(l));
                    break;
                }
                case 0x73: { // SETITEM
                    Value v = pop();
                    Value k = pop();
                    Value d = pop();
                    if (d.type != Value::DICT) fail("SETITEM target not dict");
                    if (k.type != Value::STRING) fail("SETITEM key not string");
                    (*d.dict)[k.s] = std::move(v);
                    push(std::move(d));
                    break;
                }
                case 0x75: { // SETITEMS
                    auto items = pop_to_mark();
                    Value d = pop();
                    if (d.type != Value::DICT) fail("SETITEMS target not dict");
                    if (items.size() % 2 != 0) fail("SETITEMS items not paired");
                    for (size_t i = 0; i < items.size(); i += 2) {
                        if (items[i].type != Value::STRING) fail("SETITEMS key not string");
                        (*d.dict)[items[i].s] = std::move(items[i + 1]);
                    }
                    push(std::move(d));
                    break;
                }
                case 0x52: { // REDUCE
                    Value args = pop();
                    Value callable = pop();
                    if (args.type != Value::TUPLE && args.type != Value::LIST) fail("REDUCE args not tuple/list");
                    std::vector<Value> av;
                    if (args.type == Value::TUPLE || args.type == Value::LIST) av = *args.seq;
                    push(apply_reduce(callable, av));
                    break;
                }
                case 0x51: { // BINPERSID
                    Value pid = pop();
                    push(apply_binpersid(pid));
                    break;
                }
                case 0x50: { // PERSID
                    std::string s = read_nl_string();
                    push(Value::make_string(s));
                    break;
                }
                case 0x30: // POP
                    pop();
                    break;
                case 0x32: { // DUP
                    Value v = top();
                    push(v);
                    break;
                }
                case 0x31: // POP_MARK
                    pop_mark();
                    break;
                case 0x62: { // BUILD
                    Value arg = pop();
                    Value obj = pop();
                    if (obj.type == Value::DICT && arg.type == Value::DICT) {
                        for (auto& kv : *arg.dict) (*obj.dict)[kv.first] = kv.second;
                    }
                    push(std::move(obj));
                    break;
                }
                case 0x80: // PROTO
                    get();
                    break;
                case 0x95: { // FRAME
                    uint64_t len = le_u64(ptr(8));
                    (void)len;
                    break;
                }
                case 0x94: { // MEMOIZE
                    if (stack_.empty()) fail("MEMOIZE empty stack");
                    if (memo_next_ > 1000000) fail("memo too large");
                    memo_[memo_next_++] = stack_.back();
                    break;
                }
                case 0x71: { // BINPUT
                    uint8_t idx = get();
                    if (stack_.empty()) fail("BINPUT empty stack");
                    memo_put(stack_.back(), idx);
                    break;
                }
                case 0x72: { // LONG_BINPUT
                    uint32_t idx = le_u32(ptr(4));
                    if (stack_.empty()) fail("LONG_BINPUT empty stack");
                    memo_put(stack_.back(), idx);
                    break;
                }
                case 0x70: { // PUT (decimal index)
                    std::string s = read_nl_string();
                    int64_t idx = std::stoll(s);
                    if (stack_.empty()) fail("PUT empty stack");
                    memo_put(stack_.back(), idx);
                    break;
                }
                case 0x68: { // BINGET
                    uint8_t idx = get();
                    push(memo_get(idx));
                    break;
                }
                case 0x6a: { // LONG_BINGET
                    uint32_t idx = le_u32(ptr(4));
                    push(memo_get(idx));
                    break;
                }
                case 0x67: { // GET
                    std::string s = read_nl_string();
                    int64_t idx = std::stoll(s);
                    push(memo_get(idx));
                    break;
                }
                case 0x81: { // NEWOBJ
                    Value args = pop();
                    Value cls = pop();
                    (void)args; (void)cls;
                    push(Value::make_none());
                    break;
                }
                case 0x92: { // NEWOBJ_EX
                    Value kwargs = pop();
                    Value args = pop();
                    Value cls = pop();
                    (void)kwargs; (void)args; (void)cls;
                    push(Value::make_none());
                    break;
                }
                case 0x6f: { // OBJ
                    auto items = pop_to_mark();
                    if (items.empty()) fail("OBJ no class");
                    push(Value::make_none());
                    break;
                }
                case 0x69: { // INST
                    std::string module = read_nl_string();
                    std::string name = read_nl_string();
                    auto items = pop_to_mark();
                    (void)module; (void)name; (void)items;
                    push(Value::make_none());
                    break;
                }
                case 0x82: case 0x83: case 0x84: { // EXT1/2/4
                    int n = (op == 0x82) ? 1 : (op == 0x83 ? 2 : 4);
                    ptr(n);
                    push(Value::make_none());
                    break;
                }
                case 0x2e: { // STOP
                    if (stack_.empty()) fail("STOP empty stack");
                    return stack_.back();
                }
                default:
                    fail(std::string("unsupported pickle opcode 0x") + std::to_string(op));
            }
        }
        fail("pickle ended without STOP");
    }
};

// ---------------------------------------------------------------------------
// Tensor traversal and materialization
// ---------------------------------------------------------------------------
static void collect_tensors(const Value& v, const std::string& path,
                           std::vector<std::pair<std::string, TensorInfo>>& out,
                           std::set<const void*>& visited, int depth) {
    if (depth > 1000) fail("tensor collection too deeply nested");
    if (v.type == Value::TENSOR) {
        out.emplace_back(path, v.tensor);
    } else if (v.type == Value::DICT) {
        if (!visited.insert(v.dict.get()).second) return;
        for (const auto& kv : *v.dict) {
            std::string np = path.empty() ? kv.first : path + "/" + kv.first;
            collect_tensors(kv.second, np, out, visited, depth + 1);
        }
    } else if (v.type == Value::LIST || v.type == Value::TUPLE) {
        if (!visited.insert(v.seq.get()).second) return;
        for (size_t i = 0; i < v.seq->size(); ++i) {
            std::string np = path.empty() ? std::to_string(i) : path + "/" + std::to_string(i);
            collect_tensors((*v.seq)[i], np, out, visited, depth + 1);
        }
    }
}

static std::string json_escape(const std::string& s);

static std::string value_to_json(const Value& v) {
    switch (v.type) {
        case Value::NONE: return "null";
        case Value::BOOL: return v.b ? "true" : "false";
        case Value::INT: return std::to_string(v.i);
        case Value::FLOAT: {
            std::string s = std::to_string(v.f);
            size_t end = s.find_last_not_of('0');
            if (end != std::string::npos) s.erase(end + 1);
            if (!s.empty() && s.back() == '.') s += '0';
            if (s.empty()) s = "0.0";
            return s;
        }
        case Value::STRING: return "\"" + json_escape(v.s) + "\"";
        case Value::TUPLE:
        case Value::LIST: {
            std::string s = "[";
            for (size_t i = 0; i < v.seq->size(); ++i) {
                if (i) s += ",";
                s += value_to_json((*v.seq)[i]);
            }
            s += "]";
            return s;
        }
        case Value::DICT: {
            std::string s = "{";
            size_t i = 0;
            for (const auto& kv : *v.dict) {
                if (i++) s += ",";
                s += "\"" + json_escape(kv.first) + "\":" + value_to_json(kv.second);
            }
            s += "}";
            return s;
        }
        default: return "null";
    }
}

static uint32_t half_to_float_bits(uint16_t h) {
    uint32_t s = (h >> 15) & 1;
    uint32_t e = (h >> 10) & 0x1F;
    uint32_t m = h & 0x3FF;
    if (e == 0) {
        if (m == 0) return s << 31;
        int shift = 0;
        while ((m & 0x400) == 0) {
            m <<= 1;
            ++shift;
        }
        m &= 0x3FF;
        return (s << 31) | ((113u - (uint32_t)shift) << 23) | (m << 13);
    } else if (e == 31) {
        return (s << 31) | (0xFFu << 23) | (m << 13);
    } else {
        return (s << 31) | ((e + 112u) << 23) | (m << 13);
    }
}

static std::vector<uint8_t> convert_to_f32(const std::vector<uint8_t>& in, const std::string& src_dtype) {
    if (src_dtype == "f32") return in;
    std::vector<uint8_t> out;
    if (src_dtype == "f16") {
        if (in.size() % 2 != 0) fail("f16 data size not multiple of 2");
        out.resize(in.size() * 2);
        for (size_t i = 0; i < in.size(); i += 2) {
            uint16_t h = (uint16_t)in[i] | ((uint16_t)in[i+1] << 8);
            uint32_t f = half_to_float_bits(h);
            out[i*2] = (uint8_t)(f & 0xFF);
            out[i*2+1] = (uint8_t)((f >> 8) & 0xFF);
            out[i*2+2] = (uint8_t)((f >> 16) & 0xFF);
            out[i*2+3] = (uint8_t)((f >> 24) & 0xFF);
        }
        return out;
    } else if (src_dtype == "bf16") {
        if (in.size() % 2 != 0) fail("bf16 data size not multiple of 2");
        out.resize(in.size() * 2);
        for (size_t i = 0; i < in.size(); i += 2) {
            uint16_t b = (uint16_t)in[i] | ((uint16_t)in[i+1] << 8);
            uint32_t f = ((uint32_t)b) << 16;
            out[i*2] = (uint8_t)(f & 0xFF);
            out[i*2+1] = (uint8_t)((f >> 8) & 0xFF);
            out[i*2+2] = (uint8_t)((f >> 16) & 0xFF);
            out[i*2+3] = (uint8_t)((f >> 24) & 0xFF);
        }
        return out;
    } else if (src_dtype == "f64") {
        if (in.size() % 8 != 0) fail("f64 data size not multiple of 8");
        out.resize(in.size() / 2);
        for (size_t i = 0; i < in.size(); i += 8) {
            double d;
            std::memcpy(&d, in.data() + i, 8);
            float f = (float)d;
            uint32_t u;
            std::memcpy(&u, &f, 4);
            out[i/2] = (uint8_t)(u & 0xFF);
            out[i/2+1] = (uint8_t)((u >> 8) & 0xFF);
            out[i/2+2] = (uint8_t)((u >> 16) & 0xFF);
            out[i/2+3] = (uint8_t)((u >> 24) & 0xFF);
        }
        return out;
    } else {
        fail("cannot convert dtype " + src_dtype + " to f32");
    }
    return out;
}

static std::string extract_rvc_config(const Value& config) {
    if (config.type == Value::DICT) {
        return value_to_json(config);
    }
    if (config.type != Value::LIST && config.type != Value::TUPLE) {
        fail("RVC config must be a list/tuple/dict");
    }
    const auto& seq = *config.seq;
    if (seq.size() < 18) fail("RVC config list too short");
    static const char* field_names[18] = {
        "spec_channels", "segment_size", "inter_channels", "hidden_channels",
        "filter_channels", "n_heads", "n_layers", "kernel_size",
        "p_dropout", "resblock", "resblock_kernel_sizes", "resblock_dilation_sizes",
        "upsample_rates", "upsample_initial_channel", "upsample_kernel_sizes",
        "n_speakers", "gin_channels", "sr"
    };
    std::string out = "{";
    for (int i = 0; i < 18; ++i) {
        if (i) out += ",";
        out += "\"" + json_escape(field_names[i]) + "\":" + value_to_json(seq[i]);
    }
    // phone_dim is conventionally equal to filter_channels in this model.
    out += ",\"phone_dim\":" + value_to_json(seq[4]);
    out += "}";
    return out;
}

static std::vector<uint8_t> materialize(const TensorInfo& t, const std::vector<uint8_t>& storage) {
    int item = dtype_itemsize(t.dtype);
    int64_t n = 1;
    for (int64_t s : t.shape) n *= s;
    std::vector<uint8_t> out(n * item);
    if (n == 0) return out;

    std::vector<int64_t> idx(t.shape.size(), 0);
    int64_t out_pos = 0;
    while (true) {
        int64_t src_idx = t.storage_offset;
        for (size_t i = 0; i < t.shape.size(); ++i) {
            src_idx += idx[i] * t.stride[i];
        }
        if (src_idx < 0 || (src_idx + 1) * item > (int64_t)storage.size()) {
            fail("materialize source index out of bounds: " + std::to_string(src_idx));
        }
        std::memcpy(out.data() + out_pos, storage.data() + src_idx * item, item);
        out_pos += item;

        int j = (int)t.shape.size() - 1;
        for (; j >= 0; --j) {
            idx[j]++;
            if (idx[j] < t.shape[j]) break;
            idx[j] = 0;
        }
        if (j < 0) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON writer
// ---------------------------------------------------------------------------

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
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
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static std::string json_int_array(const std::vector<int64_t>& a) {
    std::string s = "[";
    for (size_t i = 0; i < a.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(a[i]);
    }
    s += "]";
    return s;
}

static std::string tensor_bin_name(const std::string& path) {
    std::string s;
    s.reserve(path.size() * 2 + 4);
    for (char c : path) {
        if (c == '/') s += "__";
        else s += c;
    }
    return s + ".bin";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("cannot open " + path);
    f.seekg(0, std::ios::end);
    size_t sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(sz);
    if (sz && !f.read((char*)data.data(), sz)) fail("cannot read " + path);
    return data;
}

static void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) fail("cannot open for write: " + path);
    if (!data.empty() && !f.write((const char*)data.data(), data.size())) fail("cannot write " + path);
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " <file.pth> <out_dir>\n";
            return 1;
        }
        std::string pth_path = argv[1];
        std::string out_dir = argv[2];

        std::vector<uint8_t> raw = read_file(pth_path);
        ZipArchive zip(std::move(raw));

        // Find top-level prefix by locating data.pkl.
        std::string pkl_entry;
        for (const auto& kv : zip.entries()) {
            const std::string& name = kv.first;
            if (name.size() > 9 && name.substr(name.size() - 9) == "/data.pkl") {
                pkl_entry = name;
                break;
            }
        }
        if (pkl_entry.empty()) fail("data.pkl not found in zip");
        std::string prefix;
        if (pkl_entry != "data.pkl") {
            prefix = pkl_entry.substr(0, pkl_entry.size() - 9); // strip "/data.pkl"
        }
        if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

        std::vector<uint8_t> pkl = zip.read_entry(pkl_entry);
        PickleParser parser(pkl);
        Value root = parser.parse();

        // RVC fixtures: top dict has 'weight' (tensor dict) and 'config' (arg list).
        bool is_rvc = false;
        const Value* tensor_root = &root;
        std::string config_json;
        if (root.type == Value::DICT) {
            auto it_weight = root.dict->find("weight");
            auto it_config = root.dict->find("config");
            if (it_weight != root.dict->end() && it_config != root.dict->end() &&
                (it_config->second.type == Value::LIST || it_config->second.type == Value::TUPLE) &&
                it_config->second.seq->size() >= 18) {
                is_rvc = true;
                tensor_root = &it_weight->second;
                config_json = extract_rvc_config(it_config->second);
            }
        }

        std::vector<std::pair<std::string, TensorInfo>> tensors;
        std::set<const void*> visited;
        collect_tensors(*tensor_root, "", tensors, visited, 0);

        fs::create_directories(out_dir);
        fs::create_directories(out_dir + "/tensors");

        // Sort tensor paths for deterministic output.
        std::sort(tensors.begin(), tensors.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });

        std::string manifest = "{\n";
        manifest += "  \"nulltorch_manifest\": 1,\n";
        manifest += "  \"byteorder\": \"little\"";
        if (is_rvc) {
            manifest += ",\n  \"config\": " + config_json;
        }
        manifest += ",\n  \"tensors\": {\n";
        for (size_t i = 0; i < tensors.size(); ++i) {
            auto& [path, ti] = tensors[i];
            std::string binname = tensor_bin_name(path);
            std::string storage_path = prefix.empty() ? "data/" + ti.storage_key : prefix + "/data/" + ti.storage_key;
            std::vector<uint8_t> storage = zip.read_entry(storage_path);

            std::vector<uint8_t> bin = materialize(ti, storage);
            if (is_rvc && ti.dtype != "f32") {
                bin = convert_to_f32(bin, ti.dtype);
                ti.dtype = "f32";
            }

            std::string bin_path = out_dir + "/tensors/" + binname;
            write_file(bin_path, bin);

            if (i) manifest += ",\n";
            manifest += "    \"" + json_escape(path) + "\": {\n";
            manifest += "      \"dtype\": \"" + json_escape(ti.dtype) + "\",\n";
            manifest += "      \"shape\": " + json_int_array(ti.shape) + ",\n";
            if (!is_rvc) {
                manifest += "      \"stride\": " + json_int_array(ti.stride) + ",\n";
                manifest += "      \"storage_key\": \"" + json_escape(ti.storage_key) + "\",\n";
                manifest += "      \"storage_offset\": " + std::to_string(ti.storage_offset) + ",\n";
            }
            manifest += "      \"nbytes\": " + std::to_string(ti.nbytes()) + "\n";
            manifest += "    }";
        }
        manifest += "\n  }\n}\n";

        write_file(out_dir + "/manifest.json", std::vector<uint8_t>(manifest.begin(), manifest.end()));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
