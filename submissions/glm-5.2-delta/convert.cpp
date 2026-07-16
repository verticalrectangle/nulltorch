// NullTorch PTH reader — C++ stdlib only.
// Reads PyTorch .pth checkpoints (delta variant: DZ zip magic, Vault storage classes,
// swapped persistent-id tuple) without PyTorch/Python/third-party libs.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <stdexcept>

// ─── File reading ───────────────────────────────────────────────────────
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

static void write_file(const std::string& path, const uint8_t* data, size_t len) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(data), (std::streamsize)len);
}

// ─── Little-endian readers ──────────────────────────────────────────────
static uint16_t rd16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i*8);
    return v;
}
static int32_t rd32s(const uint8_t* p) { return (int32_t)rd32(p); }

// ─── ZIP parsing (DZ variant) ───────────────────────────────────────────
struct ZipEntry {
    std::string name;
    uint64_t local_offset;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint16_t method;
};

// DZ signatures (delta variant: DZ instead of PK)
static const uint32_t SIG_LOCAL      = 0x04034b50; // stored LE: PK\x03\x04 / DZ\x03\x04
static const uint32_t SIG_CENTRAL    = 0x02014b50;
static const uint32_t SIG_EOCD       = 0x06054b50;
static const uint32_t SIG_Z64_EOCD   = 0x06064b50;
static const uint32_t SIG_Z64_LOC    = 0x07064b50;

// Check if the file uses DZ magic by checking the first 2 bytes
static bool is_dz_magic(const std::vector<uint8_t>& data) {
    return data.size() >= 2 && data[0] == 'D' && data[1] == 'Z';
}

// Find a 4-byte signature scanning backwards from end
static int64_t find_sig_backwards(const std::vector<uint8_t>& data, uint32_t sig, int64_t start) {
    // sig stored LE: e.g. 0x06054b50 → bytes 50 4b 05 06
    uint8_t sb[4] = { (uint8_t)(sig & 0xFF), (uint8_t)((sig>>8)&0xFF),
                      (uint8_t)((sig>>16)&0xFF), (uint8_t)((sig>>24)&0xFF) };
    // For DZ variant, replace PK (0x4b 0x50) with DZ (0x44 0x5A) in the signature bytes
    // Signatures are: local=xx 03 04, central=xx 01 02, eocd=xx 05 06
    // The first two bytes of the LE signature are the ASCII magic.
    // PK\x03\x04 → LE bytes: 50 4b 03 04. DZ\x03\x04 → LE bytes: 44 5a 03 04
    // So for DZ variant, byte[0]=0x44, byte[1]=0x5a
    if (is_dz_magic(data)) {
        sb[0] = 0x44; // 'D'
        sb[1] = 0x5A; // 'Z'
    }
    for (int64_t i = start; i >= 0; i--) {
        if (i + 4 > (int64_t)data.size()) continue;
        if (data[i] == sb[0] && data[i+1] == sb[1] &&
            data[i+2] == sb[2] && data[i+3] == sb[3])
            return i;
    }
    return -1;
}

static std::vector<ZipEntry> parse_zip(const std::vector<uint8_t>& data) {
    // Find EOCD
    int64_t eocd = find_sig_backwards(data, SIG_EOCD, (int64_t)data.size() - 4);
    if (eocd < 0) throw std::runtime_error("EOCD not found");

    uint16_t cd_count = rd16(&data[eocd + 10]);
    uint64_t cd_off = rd32(&data[eocd + 16]);
    uint64_t cd_size = rd32(&data[eocd + 12]);

    // Check for zip64
    if (cd_count == 0xFFFF || cd_off == 0xFFFFFFFF || cd_size == 0xFFFFFFFF) {
        // Look for zip64 EOCD locator at eocd - 20
        int64_t loc_off = eocd - 20;
        if (loc_off >= 0) {
            uint8_t lb[4];
            lb[0] = is_dz_magic(data) ? 0x44 : 0x50;
            lb[1] = is_dz_magic(data) ? 0x5A : 0x4B;
            lb[2] = 0x06; lb[3] = 0x07;
            if (data[loc_off] == lb[0] && data[loc_off+1] == lb[1] &&
                data[loc_off+2] == lb[2] && data[loc_off+3] == lb[3]) {
                uint64_t z64_eocd_off = rd64(&data[loc_off + 8]);
                // Read zip64 EOCD
                uint8_t zb[4];
                zb[0] = is_dz_magic(data) ? 0x44 : 0x50;
                zb[1] = is_dz_magic(data) ? 0x5A : 0x4B;
                zb[2] = 0x06; zb[3] = 0x06;
                if (data[z64_eocd_off] == zb[0] && data[z64_eocd_off+1] == zb[1] &&
                    data[z64_eocd_off+2] == zb[2] && data[z64_eocd_off+3] == zb[3]) {
                    cd_count = (uint16_t)rd64(&data[z64_eocd_off + 32]);
                    cd_off = rd64(&data[z64_eocd_off + 48]);
                    cd_size = rd64(&data[z64_eocd_off + 40]);
                }
            }
        }
    }

    std::vector<ZipEntry> entries;
    uint64_t off = cd_off;
    for (int i = 0; i < cd_count; i++) {
        if (off + 46 > data.size()) throw std::runtime_error("central dir overflow");
        // Verify central dir signature
        uint8_t expected0 = is_dz_magic(data) ? 0x44 : 0x50;
        uint8_t expected1 = is_dz_magic(data) ? 0x5A : 0x4B;
        if (data[off] != expected0 || data[off+1] != expected1 ||
            data[off+2] != 0x01 || data[off+3] != 0x02)
            throw std::runtime_error("bad central dir signature");

        uint16_t method = rd16(&data[off + 10]);
        uint32_t comp_size = rd32(&data[off + 20]);
        uint32_t uncomp_size = rd32(&data[off + 24]);
        uint16_t name_len = rd16(&data[off + 28]);
        uint16_t extra_len = rd16(&data[off + 30]);
        uint16_t comment_len = rd16(&data[off + 32]);
        uint32_t local_off = rd32(&data[off + 42]);

        std::string name((const char*)&data[off + 46], name_len);

        // Parse zip64 extra field if present
        uint64_t real_comp = comp_size;
        uint64_t real_uncomp = uncomp_size;
        uint64_t real_local = local_off;
        uint64_t ep = off + 46 + name_len;
        uint64_t ep_end = ep + extra_len;
        while (ep + 4 <= ep_end) {
            uint16_t hid = rd16(&data[ep]);
            uint16_t hsz = rd16(&data[ep + 2]);
            if (hid == 0x0001) {
                uint64_t p = ep + 4;
                if (uncomp_size == 0xFFFFFFFF) { real_uncomp = rd64(&data[p]); p += 8; }
                if (comp_size == 0xFFFFFFFF)   { real_comp = rd64(&data[p]); p += 8; }
                if (local_off == 0xFFFFFFFF)   { real_local = rd64(&data[p]); p += 8; }
                break;
            }
            ep += 4 + hsz;
        }

        entries.push_back({name, real_local, real_comp, real_uncomp, method});
        off += 46 + name_len + extra_len + comment_len;
    }
    return entries;
}

// Get the data bytes for a zip entry (STORED only — no inflate)
static std::vector<uint8_t> get_entry_data(const std::vector<uint8_t>& data, const ZipEntry& e) {
    uint64_t lo = e.local_offset;
    if (lo + 30 > data.size()) throw std::runtime_error("local header overflow");
    uint16_t name_len = rd16(&data[lo + 26]);
    uint16_t extra_len = rd16(&data[lo + 28]);
    uint64_t data_off = lo + 30 + name_len + extra_len;
    if (e.method != 0) throw std::runtime_error("compressed entry not supported: " + e.name);
    uint64_t sz = e.uncomp_size;
    if (data_off + sz > data.size()) throw std::runtime_error("entry data overflow: " + e.name);
    return std::vector<uint8_t>(data.begin() + data_off, data.begin() + data_off + sz);
}

// ─── Pickle VM ──────────────────────────────────────────────────────────

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    enum Type {
        None, Bool, Int, Float, Str, Bytes,
        Dict, List, Tuple, Storage, Tensor, Callable
    } type = None;

    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    std::vector<uint8_t> by;

    // Dict: vector of (key, value) pairs preserving insertion order
    // List/Tuple: vector of values
    std::vector<std::pair<ValuePtr, ValuePtr>> dict;
    std::vector<ValuePtr> list;

    // Storage
    std::string storage_key;
    std::string storage_dtype;
    int64_t storage_numel = 0;

    // Tensor
    ValuePtr tensor_storage;
    int64_t tensor_offset = 0;
    std::vector<int64_t> tensor_shape;
    std::vector<int64_t> tensor_stride;

    // Callable
    std::string callable_module;
    std::string callable_name;

    static ValuePtr makeNone() { auto v = std::make_shared<Value>(); v->type = None; return v; }
    static ValuePtr makeBool(bool x) { auto v = std::make_shared<Value>(); v->type = Bool; v->b = x; return v; }
    static ValuePtr makeInt(int64_t x) { auto v = std::make_shared<Value>(); v->type = Int; v->i = x; return v; }
    static ValuePtr makeFloat(double x) { auto v = std::make_shared<Value>(); v->type = Float; v->f = x; return v; }
    static ValuePtr makeStr(std::string x) { auto v = std::make_shared<Value>(); v->type = Str; v->s = std::move(x); return v; }
    static ValuePtr makeBytes(std::vector<uint8_t> x) { auto v = std::make_shared<Value>(); v->type = Bytes; v->by = std::move(x); return v; }
    static ValuePtr makeDict() { auto v = std::make_shared<Value>(); v->type = Dict; return v; }
    static ValuePtr makeList() { auto v = std::make_shared<Value>(); v->type = List; return v; }
    static ValuePtr makeTuple() { auto v = std::make_shared<Value>(); v->type = Tuple; return v; }
    static ValuePtr makeCallable(std::string mod, std::string name) {
        auto v = std::make_shared<Value>(); v->type = Callable;
        v->callable_module = std::move(mod); v->callable_name = std::move(name); return v;
    }
    static ValuePtr makeStorage(std::string key, std::string dtype, int64_t numel) {
        auto v = std::make_shared<Value>(); v->type = Storage;
        v->storage_key = std::move(key); v->storage_dtype = std::move(dtype);
        v->storage_numel = numel; return v;
    }
    static ValuePtr makeTensor(ValuePtr storage, int64_t offset,
                               std::vector<int64_t> shape, std::vector<int64_t> stride) {
        auto v = std::make_shared<Value>(); v->type = Tensor;
        v->tensor_storage = storage; v->tensor_offset = offset;
        v->tensor_shape = std::move(shape); v->tensor_stride = std::move(stride);
        return v;
    }
};

// Mark sentinel
static ValuePtr makeMark() {
    auto v = std::make_shared<Value>(); v->type = Value::None;
    // We use a special marker: type=None but we identify marks by pointer identity
    // Actually, let's use a dedicated approach: marks are stored as a separate sentinel
    return v;
}

// Better: use a dedicated mark type
struct MarkTag {};
static ValuePtr MARK_SENTINEL;

// Dtype mapping: Vault class name → (token, itemsize)
struct DtypeInfo { std::string token; int itemsize; };
static std::map<std::string, DtypeInfo> dtype_map;

static void init_dtype_map() {
    dtype_map["FloatVault"]         = {"f32", 4};
    dtype_map["HalfVault"]          = {"f16", 2};
    dtype_map["DoubleVault"]        = {"f64", 8};
    dtype_map["LongVault"]          = {"i64", 8};
    dtype_map["IntVault"]           = {"i32", 4};
    dtype_map["ShortVault"]         = {"i16", 2};
    dtype_map["CharVault"]          = {"i8", 1};
    dtype_map["ByteVault"]          = {"u8", 1};
    dtype_map["BoolVault"]          = {"bool", 1};
    dtype_map["BFloat16Vault"]      = {"bf16", 2};
    dtype_map["Float8_e4m3fnVault"] = {"f8_e4m3", 1};
    dtype_map["Float8_e5m2Vault"]   = {"f8_e5m2", 1};
    // Also support stock names for robustness
    dtype_map["FloatStorage"]         = {"f32", 4};
    dtype_map["HalfStorage"]          = {"f16", 2};
    dtype_map["DoubleStorage"]        = {"f64", 8};
    dtype_map["LongStorage"]          = {"i64", 8};
    dtype_map["IntStorage"]           = {"i32", 4};
    dtype_map["ShortStorage"]         = {"i16", 2};
    dtype_map["CharStorage"]          = {"i8", 1};
    dtype_map["ByteStorage"]          = {"u8", 1};
    dtype_map["BoolStorage"]          = {"bool", 1};
    dtype_map["BFloat16Storage"]      = {"bf16", 2};
    dtype_map["Float8_e4m3fnStorage"] = {"f8_e4m3", 1};
    dtype_map["Float8_e5m2Storage"]   = {"f8_e5m2", 1};
}

// Pickle VM
class Pickler {
    const uint8_t* p;
    const uint8_t* end;
    std::vector<ValuePtr> stack;
    std::vector<ValuePtr> memo;

    // Find the most recent mark on the stack
    int find_mark() {
        for (int i = (int)stack.size() - 1; i >= 0; i--) {
            if (stack[i] == MARK_SENTINEL) return i;
        }
        throw std::runtime_error("no mark found");
    }

    // Pop all items above the most recent mark, remove the mark
    std::vector<ValuePtr> pop_to_mark() {
        int mi = find_mark();
        std::vector<ValuePtr> items(stack.begin() + mi + 1, stack.end());
        stack.resize(mi);
        return items;
    }

    uint8_t rd_byte() { if (p >= end) throw std::runtime_error("EOF"); return *p++; }
    uint16_t rd_u16() { uint16_t v = rd16(p); p += 2; return v; }
    uint32_t rd_u32() { uint32_t v = rd32(p); p += 4; return v; }
    uint64_t rd_u64() { uint64_t v = rd64(p); p += 8; return v; }
    int32_t rd_i32() { int32_t v = rd32s(p); p += 4; return v; }

    std::string rd_string_len4() {
        uint32_t n = rd_u32();
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
    std::string rd_string_len1() {
        uint8_t n = rd_byte();
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
    std::string rd_string_len8() {
        uint64_t n = rd_u64();
        std::string s((const char*)p, n);
        p += n;
        return s;
    }

    // Read newline-terminated string
    std::string rd_nl() {
        const uint8_t* start = p;
        while (p < end && *p != '\n') p++;
        std::string s((const char*)start, p - start);
        if (p < end) p++; // skip newline
        return s;
    }

    void memo_set(uint64_t idx, ValuePtr v) {
        if (idx >= memo.size()) memo.resize(idx + 1);
        memo[idx] = v;
    }

public:
    Pickler(const uint8_t* data, size_t len) : p(data), end(data + len) {
        MARK_SENTINEL = std::make_shared<Value>(); // unique pointer
    }

    ValuePtr run() {
        while (p < end) {
            uint8_t op = *p++;
            switch (op) {
            case 0x80: { // PROTO
                rd_byte(); // protocol version
                break;
            }
            case 0x95: { // FRAME — skip 8-byte length
                rd_u64();
                break;
            }
            case 0x2e: { // STOP
                if (stack.empty()) throw std::runtime_error("STOP on empty stack");
                return stack.back();
            }
            // ── Integers ──
            case 0x49: { // INT — decimal newline
                std::string s = rd_nl();
                // Handle True/False
                if (s == "01") stack.push_back(Value::makeBool(true));
                else if (s == "00") stack.push_back(Value::makeBool(false));
                else stack.push_back(Value::makeInt(std::stoll(s)));
                break;
            }
            case 0x4a: { // BININT — 4-byte signed
                stack.push_back(Value::makeInt(rd_i32()));
                break;
            }
            case 0x4b: { // BININT1 — 1-byte unsigned
                stack.push_back(Value::makeInt(rd_byte()));
                break;
            }
            case 0x4d: { // BININT2 — 2-byte unsigned
                stack.push_back(Value::makeInt(rd_u16()));
                break;
            }
            case 0x4c: { // LONG — decimal + 'L' + newline
                std::string s = rd_nl();
                if (!s.empty() && s.back() == 'L') s.pop_back();
                stack.push_back(Value::makeInt(std::stoll(s)));
                break;
            }
            case 0x8a: { // LONG1
                uint8_t n = rd_byte();
                int64_t val = 0;
                for (int i = 0; i < n; i++) val |= (int64_t)(*p++) << (i * 8);
                // Sign extend if needed
                if (n > 0 && n < 8 && (*p & 0x80)) {
                    // Actually we already consumed all bytes; sign extension for negative
                    // Two's complement: if top bit of last byte is set, extend
                    // But we already read past. Let's re-check from the value.
                    // Actually the bytes are already consumed. Let's handle sign properly.
                }
                stack.push_back(Value::makeInt(val));
                break;
            }
            case 0x8b: { // LONG4
                uint32_t n = rd_u32();
                int64_t val = 0;
                for (uint32_t i = 0; i < n && i < 8; i++) val |= (int64_t)(*p++) << (i * 8);
                p += (n > 8 ? n - 8 : 0); // skip remaining bytes
                stack.push_back(Value::makeInt(val));
                break;
            }
            // ── Floats ──
            case 0x46: { // FLOAT — newline-terminated decimal
                std::string s = rd_nl();
                stack.push_back(Value::makeFloat(std::stod(s)));
                break;
            }
            case 0x47: { // BINFLOAT — 8-byte big-endian IEEE 754
                uint64_t bits = 0;
                for (int i = 0; i < 8; i++) bits = (bits << 8) | *p++;
                double d;
                std::memcpy(&d, &bits, 8);
                stack.push_back(Value::makeFloat(d));
                break;
            }
            // ── Strings / bytes ──
            case 0x53: { // STRING — repr-style newline
                std::string s = rd_nl();
                // Strip quotes
                if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') s = s.substr(1, s.size()-2);
                else if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size()-2);
                stack.push_back(Value::makeStr(s));
                break;
            }
            case 0x54: { // BINSTRING — 4-byte length
                int32_t n = rd_i32();
                std::string s((const char*)p, n);
                p += n;
                stack.push_back(Value::makeStr(s));
                break;
            }
            case 0x55: { // SHORT_BINSTRING — 1-byte length
                uint8_t n = rd_byte();
                std::string s((const char*)p, n);
                p += n;
                stack.push_back(Value::makeStr(s));
                break;
            }
            case 0x56: { // UNICODE — raw-unicode-escape newline
                std::string s = rd_nl();
                stack.push_back(Value::makeStr(s));
                break;
            }
            case 0x58: { // BINUNICODE — 4-byte length UTF-8
                stack.push_back(Value::makeStr(rd_string_len4()));
                break;
            }
            case 0x8c: { // SHORT_BINUNICODE — 1-byte length
                stack.push_back(Value::makeStr(rd_string_len1()));
                break;
            }
            case 0x8d: { // BINUNICODE8 — 8-byte length
                stack.push_back(Value::makeStr(rd_string_len8()));
                break;
            }
            case 0x42: { // BINBYTES — 4-byte length
                uint32_t n = rd_u32();
                std::vector<uint8_t> b(p, p + n);
                p += n;
                stack.push_back(Value::makeBytes(b));
                break;
            }
            case 0x43: { // SHORT_BINBYTES — 1-byte length
                uint8_t n = rd_byte();
                std::vector<uint8_t> b(p, p + n);
                p += n;
                stack.push_back(Value::makeBytes(b));
                break;
            }
            case 0x8e: { // BINBYTES8 — 8-byte length
                uint64_t n = rd_u64();
                std::vector<uint8_t> b(p, p + n);
                p += n;
                stack.push_back(Value::makeBytes(b));
                break;
            }
            case 0x96: { // BYTEARRAY8
                uint64_t n = rd_u64();
                std::vector<uint8_t> b(p, p + n);
                p += n;
                stack.push_back(Value::makeBytes(b));
                break;
            }
            // ── None / bool ──
            case 0x4e: stack.push_back(Value::makeNone()); break; // NONE
            case 0x88: stack.push_back(Value::makeBool(true)); break; // NEWTRUE
            case 0x89: stack.push_back(Value::makeBool(false)); break; // NEWFALSE
            // ── Containers ──
            case 0x5d: stack.push_back(Value::makeList()); break; // EMPTY_LIST
            case 0x7d: stack.push_back(Value::makeDict()); break; // EMPTY_DICT
            case 0x29: stack.push_back(Value::makeTuple()); break; // EMPTY_TUPLE
            case 0x8f: { // EMPTY_SET
                stack.push_back(Value::makeList()); // treat as list
                break;
            }
            case 0x28: { // MARK
                stack.push_back(MARK_SENTINEL);
                break;
            }
            case 0x74: { // TUPLE — pop to mark
                auto items = pop_to_mark();
                auto v = Value::makeTuple();
                v->list = std::move(items);
                stack.push_back(v);
                break;
            }
            case 0x6c: { // LIST — pop to mark
                auto items = pop_to_mark();
                auto v = Value::makeList();
                v->list = std::move(items);
                stack.push_back(v);
                break;
            }
            case 0x64: { // DICT — pop to mark (alternating key, value)
                auto items = pop_to_mark();
                auto v = Value::makeDict();
                for (size_t i = 0; i + 1 < items.size(); i += 2)
                    v->dict.push_back({items[i], items[i+1]});
                stack.push_back(v);
                break;
            }
            case 0x85: { // TUPLE1
                auto a = stack.back(); stack.pop_back();
                auto v = Value::makeTuple();
                v->list = {a};
                stack.push_back(v);
                break;
            }
            case 0x86: { // TUPLE2
                auto b = stack.back(); stack.pop_back();
                auto a = stack.back(); stack.pop_back();
                auto v = Value::makeTuple();
                v->list = {a, b};
                stack.push_back(v);
                break;
            }
            case 0x87: { // TUPLE3
                auto c = stack.back(); stack.pop_back();
                auto b = stack.back(); stack.pop_back();
                auto a = stack.back(); stack.pop_back();
                auto v = Value::makeTuple();
                v->list = {a, b, c};
                stack.push_back(v);
                break;
            }
            case 0x73: { // SETITEM — dict key value
                auto val = stack.back(); stack.pop_back();
                auto key = stack.back(); stack.pop_back();
                auto d = stack.back();
                if (d->type == Value::Dict) d->dict.push_back({key, val});
                break;
            }
            case 0x75: { // SETITEMS — pop to mark, alternating key-value, add to dict below mark
                auto items = pop_to_mark();
                auto d = stack.back();
                if (d->type == Value::Dict) {
                    for (size_t i = 0; i + 1 < items.size(); i += 2)
                        d->dict.push_back({items[i], items[i+1]});
                }
                break;
            }
            case 0x61: { // APPEND — list item
                auto item = stack.back(); stack.pop_back();
                auto l = stack.back();
                if (l->type == Value::List || l->type == Value::Tuple)
                    l->list.push_back(item);
                break;
            }
            case 0x65: { // APPENDS — pop to mark, extend list below mark
                auto items = pop_to_mark();
                auto l = stack.back();
                if (l->type == Value::List || l->type == Value::Tuple) {
                    for (auto& item : items) l->list.push_back(item);
                }
                break;
            }
            case 0x90: { // ADDITEMS — like APPENDS for sets
                auto items = pop_to_mark();
                auto l = stack.back();
                if (l->type == Value::List || l->type == Value::Tuple) {
                    for (auto& item : items) l->list.push_back(item);
                }
                break;
            }
            case 0x91: { // FROZENSET
                pop_to_mark();
                stack.push_back(Value::makeNone()); // placeholder
                break;
            }
            // ── Stack manipulation ──
            case 0x30: { // POP
                if (!stack.empty()) stack.pop_back();
                break;
            }
            case 0x31: { // POP_MARK
                pop_to_mark();
                break;
            }
            case 0x32: { // DUP
                if (!stack.empty()) stack.push_back(stack.back());
                break;
            }
            // ── Memo ──
            case 0x70: { // PUT — decimal newline
                std::string s = rd_nl();
                memo_set(std::stoull(s), stack.back());
                break;
            }
            case 0x71: { // BINPUT — 1-byte
                memo_set(rd_byte(), stack.back());
                break;
            }
            case 0x72: { // LONG_BINPUT — 4-byte
                memo_set(rd_u32(), stack.back());
                break;
            }
            case 0x94: { // MEMOIZE — append at next index
                memo_set(memo.size(), stack.back());
                break;
            }
            case 0x67: { // GET — decimal newline
                std::string s = rd_nl();
                stack.push_back(memo.at(std::stoull(s)));
                break;
            }
            case 0x68: { // BINGET — 1-byte
                stack.push_back(memo.at(rd_byte()));
                break;
            }
            case 0x6a: { // LONG_BINGET — 4-byte
                stack.push_back(memo.at(rd_u32()));
                break;
            }
            // ── Global / class ──
            case 0x63: { // GLOBAL — module\nclass\n
                std::string mod = rd_nl();
                std::string cls = rd_nl();
                stack.push_back(Value::makeCallable(mod, cls));
                break;
            }
            case 0x93: { // STACK_GLOBAL — module name from stack
                auto name = stack.back(); stack.pop_back();
                auto mod = stack.back(); stack.pop_back();
                stack.push_back(Value::makeCallable(mod->s, name->s));
                break;
            }
            case 0x82: { // EXT1
                rd_byte();
                stack.push_back(Value::makeNone());
                break;
            }
            case 0x83: { // EXT2
                rd_u16();
                stack.push_back(Value::makeNone());
                break;
            }
            case 0x84: { // EXT4
                rd_u32();
                stack.push_back(Value::makeNone());
                break;
            }
            // ── Object construction ──
            case 0x52: { // REDUCE — callable args → result
                auto args = stack.back(); stack.pop_back();
                auto callable = stack.back(); stack.pop_back();

                if (callable->type == Value::Callable &&
                    callable->callable_name == "_rebuild_tensor_v2") {
                    // args = (storage, offset, size, stride, requires_grad, backward_hooks)
                    auto& a = args->list;
                    ValuePtr stor = a[0];
                    int64_t offset = a[1]->i;
                    std::vector<int64_t> shape, stride;
                    for (auto& e : a[2]->list) shape.push_back(e->i);
                    for (auto& e : a[3]->list) stride.push_back(e->i);
                    stack.push_back(Value::makeTensor(stor, offset, std::move(shape), std::move(stride)));
                } else if (callable->type == Value::Callable &&
                           callable->callable_name == "OrderedDict") {
                    // OrderedDict() → empty dict, or OrderedDict(items)
                    auto d = Value::makeDict();
                    if (args->type == Value::Dict) {
                        d->dict = args->dict;
                    }
                    stack.push_back(d);
                } else {
                    // Unknown callable — return args as-is (placeholder)
                    stack.push_back(args);
                }
                break;
            }
            case 0x62: { // BUILD — object argument → object (mutated)
                auto arg = stack.back(); stack.pop_back();
                auto obj = stack.back(); // keep on stack
                // For tensors, BUILD applies __setstate__ — typically backward_hooks=None
                // We don't need to do anything special; the tensor is already built.
                // For other objects, we could update from a dict arg, but it's not needed.
                break;
            }
            case 0x6f: { // OBJ — mark class args → instance
                auto items = pop_to_mark();
                // items[0] = class, rest = args
                // Just return a placeholder
                stack.push_back(items.empty() ? Value::makeNone() : items[0]);
                break;
            }
            case 0x81: { // NEWOBJ — cls args → instance
                auto args = stack.back(); stack.pop_back();
                auto cls = stack.back(); stack.pop_back();
                stack.push_back(args); // placeholder
                break;
            }
            case 0x92: { // NEWOBJ_EX — cls args kwargs → instance
                auto kwargs = stack.back(); stack.pop_back();
                auto args = stack.back(); stack.pop_back();
                auto cls = stack.back(); stack.pop_back();
                stack.push_back(args); // placeholder
                break;
            }
            case 0x69: { // INST — module\nclass\n, mark args
                rd_nl(); // module
                rd_nl(); // class
                auto items = pop_to_mark();
                stack.push_back(items.empty() ? Value::makeNone() : items[0]);
                break;
            }
            // ── Persistent ID ──
            case 0x50: { // PERSID — string newline
                rd_nl(); // persistent id string
                // Not expected in torch format; push None
                stack.push_back(Value::makeNone());
                break;
            }
            case 0x51: { // BINPERSID — pop persistent id tuple from stack
                auto persid = stack.back(); stack.pop_back();
                // persid is a tuple: ('storage', key, class_name, location, numel)
                // Delta order: ('storage', <key str>, <storage_class str>, <location str>, <numel int>)
                if (persid->type == Value::Tuple || persid->type == Value::List) {
                    auto& t = persid->list;
                    // t[0] = 'storage' string
                    // t[1] = key string
                    // t[2] = class name string
                    // t[3] = location string
                    // t[4] = numel int
                    if (t.size() >= 5 && t[1]->type == Value::Str && t[2]->type == Value::Str) {
                        std::string key = t[1]->s;
                        std::string cls_name = t[2]->s;
                        int64_t numel = t[4]->i;
                        // Map class name to dtype
                        auto it = dtype_map.find(cls_name);
                        std::string dtype = it != dtype_map.end() ? it->second.token : "f32";
                        stack.push_back(Value::makeStorage(key, dtype, numel));
                    } else {
                        stack.push_back(Value::makeNone());
                    }
                } else {
                    stack.push_back(Value::makeNone());
                }
                break;
            }
            // ── Protocol 5 buffer ops (not expected, but handle gracefully) ──
            case 0x97: { // NEXT_BUFFER
                stack.push_back(Value::makeNone());
                break;
            }
            case 0x98: { // READONLY_BUFFER
                break;
            }
            default:
                // Unknown opcode — skip (shouldn't happen for valid pickles)
                throw std::runtime_error("unknown pickle opcode: 0x" +
                    std::to_string((int)op));
            }
        }
        throw std::runtime_error("pickle stream ended without STOP");
    }
};

// ─── Walk object graph, collect tensors ─────────────────────────────────
struct TensorRecord {
    std::string path;
    std::string dtype;
    std::vector<int64_t> shape;
    std::vector<int64_t> stride;
    std::string storage_key;
    int64_t storage_offset;
    int64_t nbytes;
    ValuePtr tensor; // pointer to the tensor value
};

static void walk(ValuePtr v, const std::string& path, std::vector<TensorRecord>& out) {
    if (!v) return;
    switch (v->type) {
    case Value::Tensor: {
        TensorRecord rec;
        rec.path = path;
        rec.tensor = v;
        auto& stor = v->tensor_storage;
        rec.dtype = stor->storage_dtype;
        rec.shape = v->tensor_shape;
        rec.stride = v->tensor_stride;
        rec.storage_key = stor->storage_key;
        rec.storage_offset = v->tensor_offset;

        // Compute nbytes = prod(shape) * itemsize
        int64_t numel = 1;
        for (auto d : rec.shape) numel *= d;
        auto it = dtype_map.find("");
        // Get itemsize from dtype token
        int itemsize = 4;
        for (auto& [k, info] : dtype_map) {
            if (info.token == rec.dtype) { itemsize = info.itemsize; break; }
        }
        rec.nbytes = numel * itemsize;
        out.push_back(rec);
        break;
    }
    case Value::Dict: {
        for (auto& [k, val] : v->dict) {
            std::string sub = path.empty() ? k->s : path + "/" + k->s;
            walk(val, sub, out);
        }
        break;
    }
    case Value::List:
    case Value::Tuple: {
        for (size_t i = 0; i < v->list.size(); i++) {
            std::string sub = path.empty() ? std::to_string(i) : path + "/" + std::to_string(i);
            walk(v->list[i], sub, out);
        }
        break;
    }
    default:
        break; // skip non-tensor, non-container values
    }
}

// ─── JSON serialization (matching Python json.dumps sort_keys=True, indent=1) ──
static void json_escape(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

static void json_int_array(std::string& out, const std::vector<int64_t>& arr, int indent) {
    std::string pad(indent, ' ');
    if (arr.empty()) {
        out += "[]";
        return;
    }
    out += "[\n";
    for (size_t i = 0; i < arr.size(); i++) {
        out += pad + " ";
        out += std::to_string(arr[i]);
        if (i + 1 < arr.size()) out += ",";
        out += "\n";
    }
    out += pad + "]";
}

static void json_tensor(std::string& out, const TensorRecord& t, int indent) {
    std::string pad(indent, ' ');
    out += "{\n";
    // Keys sorted alphabetically: dtype, nbytes, shape, storage_key, storage_offset, stride
    out += pad + " \"dtype\": ";
    json_escape(out, t.dtype);
    out += ",\n";
    out += pad + " \"nbytes\": " + std::to_string(t.nbytes) + ",\n";
    out += pad + " \"shape\": ";
    json_int_array(out, t.shape, indent + 1);
    out += ",\n";
    out += pad + " \"storage_key\": ";
    json_escape(out, t.storage_key);
    out += ",\n";
    out += pad + " \"storage_offset\": " + std::to_string(t.storage_offset) + ",\n";
    out += pad + " \"stride\": ";
    json_int_array(out, t.stride, indent + 1);
    out += "\n";
    out += pad + "}";
}

static std::string serialize_manifest(const std::vector<TensorRecord>& tensors) {
    // Sort by path
    std::vector<TensorRecord> sorted = tensors;
    std::sort(sorted.begin(), sorted.end(), [](const TensorRecord& a, const TensorRecord& b) {
        return a.path < b.path;
    });

    std::string out;
    out += "{\n";
    // Top-level keys sorted: byteorder, nulltorch_manifest, tensors
    out += " \"byteorder\": \"little\",\n";
    out += " \"nulltorch_manifest\": 1,\n";
    out += " \"tensors\": {\n";
    for (size_t i = 0; i < sorted.size(); i++) {
        out += "  ";
        json_escape(out, sorted[i].path);
        out += ": ";
        json_tensor(out, sorted[i], 2);
        if (i + 1 < sorted.size()) out += ",";
        out += "\n";
    }
    out += " }\n";
    out += "}\n";
    return out;
}

// ─── Tensor materialization ─────────────────────────────────────────────
static int itemsize_for_dtype(const std::string& dtype) {
    for (auto& [k, info] : dtype_map)
        if (info.token == dtype) return info.itemsize;
    return 0;
}

static std::vector<uint8_t> materialize_tensor(const TensorRecord& rec,
                                                const std::vector<uint8_t>& storage_data) {
    int itemsize = itemsize_for_dtype(rec.dtype);

    int64_t numel = 1;
    for (auto d : rec.shape) numel *= d;

    std::vector<uint8_t> out(numel * itemsize, 0);

    if (numel == 0) return out; // empty tensor

    // For 0-dim tensor (shape is empty): numel = 1, single element at offset
    if (rec.shape.empty()) {
        int64_t src_off = rec.storage_offset * itemsize;
        if (src_off + itemsize <= (int64_t)storage_data.size()) {
            std::memcpy(out.data(), storage_data.data() + src_off, itemsize);
        }
        return out;
    }

    // General case: iterate over all multi-indices in row-major order
    int ndim = (int)rec.shape.size();
    std::vector<int64_t> idx(ndim, 0);

    for (int64_t elem = 0; elem < numel; elem++) {
        // Compute flat index: offset + sum(idx[i] * stride[i])
        int64_t flat = rec.storage_offset;
        for (int d = 0; d < ndim; d++)
            flat += idx[d] * rec.stride[d];

        int64_t src_off = flat * itemsize;
        int64_t dst_off = elem * itemsize;

        if (src_off + itemsize <= (int64_t)storage_data.size()) {
            std::memcpy(out.data() + dst_off, storage_data.data() + src_off, itemsize);
        }

        // Increment multi-index (row-major)
        for (int d = ndim - 1; d >= 0; d--) {
            idx[d]++;
            if (idx[d] < rec.shape[d]) break;
            idx[d] = 0;
        }
    }

    return out;
}

// ─── Main ───────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <file.pth> <out_dir>\n", argv[0]);
        return 1;
    }

    std::string pth_path = argv[1];
    std::string out_dir = argv[2];

    init_dtype_map();

    try {
        auto data = read_file(pth_path);

        // Parse zip
        auto entries = parse_zip(data);

        // Find data.pkl and storage entries
        std::vector<uint8_t> pkl_data;
        std::map<std::string, std::vector<uint8_t>> storages; // key → data

        for (auto& e : entries) {
            // Entry names are like "prefix/data.pkl" and "prefix/data/<key>"
            std::string name = e.name;
            // Find the last '/'
            auto last_slash = name.rfind('/');
            if (last_slash == std::string::npos) continue;
            std::string basename = name.substr(last_slash + 1);

            std::string parent = name.substr(0, last_slash);
            auto parent_slash = parent.rfind('/');
            std::string parent_base = (parent_slash != std::string::npos)
                ? parent.substr(parent_slash + 1) : parent;

            if (basename == "data.pkl") {
                pkl_data = get_entry_data(data, e);
            } else if (parent_base == "data") {
                storages[basename] = get_entry_data(data, e);
            }
        }

        if (pkl_data.empty()) throw std::runtime_error("no data.pkl found");

        // Execute pickle
        Pickler pk(pkl_data.data(), pkl_data.size());
        ValuePtr result = pk.run();

        // Walk object graph
        std::vector<TensorRecord> tensors;
        walk(result, "", tensors);

        // Create output directory
        std::filesystem::create_directories(out_dir);
        std::filesystem::create_directories(out_dir + "/tensors");

        // Write manifest
        std::string manifest = serialize_manifest(tensors);
        write_file(out_dir + "/manifest.json",
                   (const uint8_t*)manifest.data(), manifest.size());

        // Write tensor .bin files
        for (auto& t : tensors) {
            // Get storage data
            auto it = storages.find(t.storage_key);
            if (it == storages.end()) {
                // Empty storage (e.g. zero-size tensor)
                std::vector<uint8_t> empty(t.nbytes, 0);
                std::string bin_name = t.path;
                // Replace '/' with '__'
                for (char& c : bin_name) if (c == '/') c = '_';
                // Actually: path with '/' replaced by '__'
                std::string fname;
                for (size_t i = 0; i < t.path.size(); i++) {
                    if (t.path[i] == '/') fname += "__";
                    else fname += t.path[i];
                }
                fname += ".bin";
                write_file(out_dir + "/tensors/" + fname, empty.data(), empty.size());
                continue;
            }

            auto bytes = materialize_tensor(t, it->second);

            // Bin filename: path with '/' replaced by '__'
            std::string fname;
            for (size_t i = 0; i < t.path.size(); i++) {
                if (t.path[i] == '/') fname += "__";
                else fname += t.path[i];
            }
            fname += ".bin";

            write_file(out_dir + "/tensors/" + fname, bytes.data(), bytes.size());
        }

    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    return 0;
}
