// NullTorch PTH reader — C++ stdlib only.
// Reads a PyTorch .pth (ZIP + pickle) without torch/python/3rd-party libs.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <cmath>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Byte reader (bounds-checked)
// ---------------------------------------------------------------------------
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t i = 0;
    Reader(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
    bool eof() const { return i >= n; }
    size_t left() const { return i < n ? n - i : 0; }
    void need(size_t k) const {
        if (i + k > n) throw std::runtime_error("unexpected end of data");
    }
    uint8_t u8() { need(1); return p[i++]; }
    uint16_t u16() { need(2); uint16_t v = (uint16_t)p[i] | ((uint16_t)p[i+1] << 8); i += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) | ((uint32_t)p[i+2] << 16) | ((uint32_t)p[i+3] << 24); i += 4; return v; }
    uint64_t u64() { need(8); uint64_t v = 0; for (int k = 0; k < 8; ++k) v |= (uint64_t)p[i+k] << (8*k); i += 8; return v; }
    int32_t i32() { return (int32_t)u32(); }
    std::string_view bytes(size_t k) { need(k); auto sv = std::string_view((const char*)p + i, k); i += k; return sv; }
};

// ---------------------------------------------------------------------------
// DEFLATE inflater (RFC 1951, raw stream, LSB-first)
// ---------------------------------------------------------------------------
struct Inflater {
    const uint8_t* in;
    size_t inLen;
    size_t bitPos = 0; // byte index in `in`
    int bitBuf = 0;
    int bitCnt = 0;
    std::vector<uint8_t> out;

    Inflater(const uint8_t* p, size_t n) : in(p), inLen(n) {}

    int getBit() {
        if (bitCnt == 0) {
            if (bitPos >= inLen) throw std::runtime_error("deflate: truncated");
            bitBuf = in[bitPos++];
            bitCnt = 8;
        }
        int b = bitBuf & 1;
        bitBuf >>= 1;
        --bitCnt;
        return b;
    }
    uint32_t getBits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= (uint32_t)getBit() << i;
        return v;
    }
    // Huffman: read MSB-first code
    struct Huff {
        std::vector<int> len;     // code length per symbol
        std::vector<int> count;   // count of codes of each length
        std::vector<int> symbol;  // symbols sorted by code
        bool built = false;
    };
    void buildHuff(Huff& h, const std::vector<int>& lengths) {
        h.len = lengths;
        int maxLen = 0;
        for (int l : lengths) if (l > maxLen) maxLen = l;
        h.count.assign(maxLen + 1, 0);
        for (int l : lengths) if (l > 0) h.count[l]++;
        std::vector<int> offs(maxLen + 2, 0);
        for (int l = 1; l <= maxLen; ++l) offs[l + 1] = offs[l] + h.count[l];
        h.symbol.resize(lengths.size());
        for (int s = 0; s < (int)lengths.size(); ++s) {
            int l = lengths[s];
            if (l > 0) h.symbol[offs[l]++] = s;
        }
        h.built = true;
    }
    int decodeSym(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l <= (int)h.count.size() - 1; ++l) {
            code |= getBit();
            int cnt = h.count[l];
            if (code - first < cnt) return h.symbol[index + (code - first)];
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        throw std::runtime_error("deflate: invalid huffman code");
    }
    void decodeBlock(Huff& lit, Huff& dist) {
        while (true) {
            int sym = decodeSym(lit);
            if (sym == 256) return;
            if (sym < 256) { out.push_back((uint8_t)sym); continue; }
            sym -= 257;
            static const int lenBase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
            static const int lenExtra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
            int length = lenBase[sym] + (int)getBits(lenExtra[sym]);
            int dsym = decodeSym(dist);
            static const int distBase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
            static const int distExtra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
            int distance = distBase[dsym] + (int)getBits(distExtra[dsym]);
            size_t pos = out.size() - distance;
            for (int k = 0; k < length; ++k) { out.push_back(out[pos + k]); }
        }
    }
    void inflate() {
        while (true) {
            int bfinal = getBit();
            int btype = getBits(2);
            if (btype == 0) {
                // stored: skip to byte boundary
                bitCnt = 0;
                if (bitPos + 4 > inLen) throw std::runtime_error("deflate: truncated stored");
                int len = in[bitPos] | (in[bitPos+1] << 8);
                bitPos += 4; // len + nlen
                if (bitPos + len > inLen) throw std::runtime_error("deflate: truncated stored data");
                for (int k = 0; k < len; ++k) out.push_back(in[bitPos + k]);
                bitPos += len;
            } else if (btype == 1) {
                Huff lit, dist;
                std::vector<int> ll(288, 0);
                for (int i = 0; i <= 143; ++i) ll[i] = 8;
                for (int i = 144; i <= 255; ++i) ll[i] = 9;
                for (int i = 256; i <= 279; ++i) ll[i] = 7;
                for (int i = 280; i <= 287; ++i) ll[i] = 8;
                buildHuff(lit, ll);
                std::vector<int> dl(30, 5);
                buildHuff(dist, dl);
                decodeBlock(lit, dist);
            } else if (btype == 2) {
                int hlit = getBits(5) + 257;
                int hdist = getBits(5) + 1;
                int hclen = getBits(4) + 4;
                static const int order[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                std::vector<int> cl(19, 0);
                for (int i = 0; i < hclen; ++i) cl[order[i]] = getBits(3);
                Huff clHuff;
                buildHuff(clHuff, cl);
                std::vector<int> lengths;
                while ((int)lengths.size() < hlit + hdist) {
                    int sym = decodeSym(clHuff);
                    if (sym < 16) lengths.push_back(sym);
                    else if (sym == 16) {
                        int rep = getBits(2) + 3;
                        int prev = lengths.back();
                        for (int k = 0; k < rep; ++k) lengths.push_back(prev);
                    } else if (sym == 17) {
                        int rep = getBits(3) + 3;
                        for (int k = 0; k < rep; ++k) lengths.push_back(0);
                    } else { // 18
                        int rep = getBits(7) + 11;
                        for (int k = 0; k < rep; ++k) lengths.push_back(0);
                    }
                }
                std::vector<int> ll(lengths.begin(), lengths.begin() + hlit);
                std::vector<int> dl(lengths.begin() + hlit, lengths.end());
                Huff lit, dist;
                buildHuff(lit, ll);
                buildHuff(dist, dl);
                decodeBlock(lit, dist);
            } else {
                throw std::runtime_error("deflate: invalid block type");
            }
            if (bfinal) break;
        }
    }
};

// ---------------------------------------------------------------------------
// ZIP reader (central-directory driven, STORED + DEFLATE, zip64)
// ---------------------------------------------------------------------------
struct ZipEntry {
    std::string name;
    uint16_t method;
    uint64_t compSize;
    uint64_t uncompSize;
    uint64_t localOffset;
};

struct ZipFile {
    std::vector<uint8_t> data;
    std::vector<ZipEntry> entries;
    std::unordered_map<std::string, size_t> byName;

    static uint32_t u32at(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
    static uint16_t u16at(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1]<<8); }
    static uint64_t u64at(const uint8_t* p) { uint64_t v=0; for (int k=0;k<8;++k) v |= (uint64_t)p[k]<<(8*k); return v; }

    void read(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) throw std::runtime_error("cannot open file");
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0) { fclose(f); throw std::runtime_error("ftell failed"); }
        data.resize((size_t)sz);
        if (sz > 0 && fread(data.data(), 1, data.size(), f) != data.size()) { fclose(f); throw std::runtime_error("read failed"); }
        fclose(f);
        parseCentral();
    }

    void parseCentral() {
        size_t n = data.size();
        if (n < 22) throw std::runtime_error("zip: too small");
        // find EOCD by scanning backwards
        long scanStart = (long)n - 22;
        if (scanStart < 0) scanStart = 0;
        long eocd = -1;
        for (long i = scanStart; i >= 0; --i) {
            if (data[i]==0x50 && data[i+1]==0x4b && data[i+2]==0x05 && data[i+3]==0x06) {
                // validate comment length
                uint16_t clen = u16at(&data[i+20]);
                if (i + 22 + clen == (long)n) { eocd = i; break; }
            }
        }
        if (eocd < 0) throw std::runtime_error("zip: EOCD not found");
        uint16_t numEntries = u16at(&data[eocd+10]);
        uint64_t cdSize = u32at(&data[eocd+12]);
        uint64_t cdOffset = u32at(&data[eocd+16]);
        // zip64?
        if (numEntries == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF) {
            // look for zip64 EOCD locator at eocd-20
            long loc = eocd - 20;
            if (loc >= 0 && data[loc]==0x50 && data[loc+1]==0x4b && data[loc+2]==0x06 && data[loc+3]==0x07) {
                uint64_t z64off = u64at(&data[loc+8]);
                if (z64off + 56 <= n && data[z64off]==0x50 && data[z64off+1]==0x4b && data[z64off+2]==0x06 && data[z64off+3]==0x06) {
                    uint64_t total = u64at(&data[z64off+32]);
                    cdSize = u64at(&data[z64off+40]);
                    cdOffset = u64at(&data[z64off+48]);
                    numEntries = (uint16_t)total;
                }
            }
        }
        if (cdOffset + cdSize > n) throw std::runtime_error("zip: central directory out of range");
        size_t p = (size_t)cdOffset;
        for (uint16_t e = 0; e < numEntries; ++e) {
            if (p + 46 > n) throw std::runtime_error("zip: central entry truncated");
            if (data[p]!=0x50 || data[p+1]!=0x4b || data[p+2]!=0x01 || data[p+3]!=0x02) throw std::runtime_error("zip: bad central sig");
            uint16_t method = u16at(&data[p+10]);
            uint32_t compSize32 = u32at(&data[p+20]);
            uint32_t uncompSize32 = u32at(&data[p+24]);
            uint16_t nameLen = u16at(&data[p+28]);
            uint16_t extraLen = u16at(&data[p+30]);
            uint16_t commentLen = u16at(&data[p+32]);
            uint32_t localOff32 = u32at(&data[p+42]);
            if (p + 46 + nameLen + extraLen + commentLen > n) throw std::runtime_error("zip: central entry fields truncated");
            ZipEntry ze;
            ze.name.assign((const char*)&data[p+46], nameLen);
            ze.method = method;
            ze.compSize = compSize32;
            ze.uncompSize = uncompSize32;
            ze.localOffset = localOff32;
            // parse zip64 extra
            size_t ep = p + 46 + nameLen;
            size_t end = ep + extraLen;
            while (ep + 4 <= end) {
                uint16_t hid = u16at(&data[ep]);
                uint16_t hsz = u16at(&data[ep+2]);
                ep += 4;
                if (ep + hsz > end) break;
                if (hid == 0x0001) {
                    size_t q = ep;
                    if (uncompSize32 == 0xFFFFFFFF) { ze.uncompSize = u64at(&data[q]); q += 8; }
                    if (compSize32 == 0xFFFFFFFF) { ze.compSize = u64at(&data[q]); q += 8; }
                    if (localOff32 == 0xFFFFFFFF) { ze.localOffset = u64at(&data[q]); q += 8; }
                }
                ep += hsz;
            }
            byName[ze.name] = entries.size();
            entries.push_back(std::move(ze));
            p += 46 + nameLen + extraLen + commentLen;
        }
    }

    // Get uncompressed bytes of an entry (with bounds checks).
    std::vector<uint8_t> readEntry(const ZipEntry& ze) const {
        size_t n = data.size();
        if (ze.localOffset + 30 > n) throw std::runtime_error("zip: local header out of range");
        size_t lp = (size_t)ze.localOffset;
        if (data[lp]!=0x50 || data[lp+1]!=0x4b || data[lp+2]!=0x03 || data[lp+3]!=0x04) throw std::runtime_error("zip: bad local sig");
        uint16_t nameLen = u16at(&data[lp+26]);
        uint16_t extraLen = u16at(&data[lp+28]);
        size_t dataStart = lp + 30 + nameLen + extraLen;
        if (dataStart + ze.compSize > n) throw std::runtime_error("zip: entry data out of range");
        if (ze.method == 0) {
            return std::vector<uint8_t>(data.begin() + dataStart, data.begin() + dataStart + ze.compSize);
        } else if (ze.method == 8) {
            Inflater inf(&data[dataStart], (size_t)ze.compSize);
            inf.inflate();
            return std::move(inf.out);
        } else {
            throw std::runtime_error("zip: unsupported method");
        }
    }

    bool has(const std::string& name) const { return byName.count(name) > 0; }
    const ZipEntry& entry(const std::string& name) const {
        auto it = byName.find(name);
        if (it == byName.end()) throw std::runtime_error("zip: entry not found: " + name);
        return entries[it->second];
    }
};

// ---------------------------------------------------------------------------
// Pickle value model
// ---------------------------------------------------------------------------
struct Storage {
    std::string key;
    std::string dtype;
    int itemsize = 0;
    int64_t numel = 0;
};
struct TensorObj {
    std::shared_ptr<Storage> storage;
    int64_t offset = 0;
    std::vector<int64_t> shape;
    std::vector<int64_t> stride;
};
struct Global {
    std::string module;
    std::string name;
};

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    enum Type { NoneT, BoolT, IntT, FloatT, StrT, BytesT, ListT, TupleT, DictT, MarkT, TensorT, StorageT, GlobalT, OpaqueT };
    Type type = NoneT;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<ValuePtr> list;
    std::vector<std::pair<ValuePtr, ValuePtr>> dict;
    std::shared_ptr<TensorObj> tensor;
    std::shared_ptr<Storage> storage;
    std::shared_ptr<Global> global;
};

static ValuePtr make(Value::Type t) { auto v = std::make_shared<Value>(); v->type = t; return v; }

// dtype token -> element size
static const std::unordered_map<std::string, int>& dtypeSizes() {
    static const std::unordered_map<std::string, int> m = {
        {"f64",8},{"f32",4},{"f16",2},{"bf16",2},
        {"f8_e4m3",1},{"f8_e5m2",1},
        {"i64",8},{"i32",4},{"i16",2},{"i8",1},{"u8",1},{"bool",1},
    };
    return m;
}
// storage class name -> dtype token
static std::string storageClassToDtype(const std::string& cls) {
    static const std::unordered_map<std::string, std::string> m = {
        {"DoubleStorage","f64"},{"FloatStorage","f32"},{"HalfStorage","f16"},
        {"BFloat16Storage","bf16"},{"Float8_e4m3fnStorage","f8_e4m3"},
        {"Float8_e5m2Storage","f8_e5m2"},{"LongStorage","i64"},{"IntStorage","i32"},
        {"ShortStorage","i16"},{"CharStorage","i8"},{"ByteStorage","u8"},{"BoolStorage","bool"},
    };
    auto it = m.find(cls);
    if (it == m.end()) return "";
    return it->second;
}

// ---------------------------------------------------------------------------
// Pickle VM
// ---------------------------------------------------------------------------
struct Pickler {
    Reader r;
    std::vector<ValuePtr> stack;
    std::vector<ValuePtr> memo;
    ValuePtr result;

    Pickler(const uint8_t* p, size_t n) : r(p, n) {}

    void push(ValuePtr v) { stack.push_back(v); }
    ValuePtr pop() {
        if (stack.empty()) throw std::runtime_error("pickle: stack underflow");
        ValuePtr v = stack.back(); stack.pop_back(); return v;
    }
    void memoSet(size_t idx, ValuePtr v) {
        if (memo.size() <= idx) memo.resize(idx + 1);
        memo[idx] = v;
    }
    ValuePtr memoGet(size_t idx) {
        if (idx >= memo.size() || !memo[idx]) throw std::runtime_error("pickle: bad memo get");
        return memo[idx];
    }

    // find index of topmost mark
    long topMark() const {
        for (long i = (long)stack.size() - 1; i >= 0; --i)
            if (stack[i] && stack[i]->type == Value::MarkT) return i;
        throw std::runtime_error("pickle: no mark");
    }

    std::string readLine() {
        std::string s;
        while (!r.eof()) {
            uint8_t c = r.u8();
            if (c == '\n') break;
            s.push_back((char)c);
        }
        return s;
    }

    int64_t parseDecimal(const std::string& s) {
        // handle bool literals "01"/"00"
        if (s == "01") return 1;
        if (s == "00") return 0;
        return std::stoll(s);
    }

    ValuePtr persistentLoad(ValuePtr pid) {
        // pid is a tuple: ("storage", Global(class), key, device, numel)
        if (!pid || pid->type != Value::TupleT) throw std::runtime_error("pickle: bad persistent id");
        auto& items = pid->list;
        if (items.size() < 3) throw std::runtime_error("pickle: short persistent id");
        std::string kind;
        if (items[0]->type == Value::StrT) kind = items[0]->s;
        if (kind != "storage") throw std::runtime_error("pickle: unknown persistent kind");
        std::string cls;
        if (items[1]->type == Value::GlobalT) cls = items[1]->global->name;
        std::string key;
        if (items[2]->type == Value::StrT) key = items[2]->s;
        int64_t numel = 0;
        if (items.size() >= 5 && items[4]->type == Value::IntT) numel = items[4]->i;
        std::string dtype = storageClassToDtype(cls);
        if (dtype.empty()) throw std::runtime_error("pickle: unknown storage class " + cls);
        auto st = std::make_shared<Storage>();
        st->key = key;
        st->dtype = dtype;
        st->itemsize = dtypeSizes().at(dtype);
        st->numel = numel;
        auto v = make(Value::StorageT);
        v->storage = st;
        return v;
    }

    // Build a tensor from _rebuild_tensor_v2 args
    ValuePtr buildTensorV2(ValuePtr args) {
        auto& a = args->list; // tuple
        if (a.size() < 4) throw std::runtime_error("rebuild_tensor_v2: too few args");
        auto t = std::make_shared<TensorObj>();
        if (a[0]->type == Value::StorageT) t->storage = a[0]->storage;
        else throw std::runtime_error("rebuild_tensor_v2: no storage");
        if (a[1]->type == Value::IntT) t->offset = a[1]->i;
        auto toInts = [](ValuePtr v, std::vector<int64_t>& out) {
            if (!v || (v->type != Value::TupleT && v->type != Value::ListT)) return;
            for (auto& e : v->list) {
                if (e->type == Value::IntT) out.push_back(e->i);
            }
        };
        toInts(a[2], t->shape);
        toInts(a[3], t->stride);
        auto v = make(Value::TensorT);
        v->tensor = t;
        return v;
    }

    ValuePtr doReduce(ValuePtr callable, ValuePtr args) {
        if (callable && callable->type == Value::GlobalT) {
            const std::string& name = callable->global->name;
            const std::string& mod = callable->global->module;
            if (name == "_rebuild_tensor_v2" || name == "_rebuild_tensor") {
                return buildTensorV2(args);
            }
            if (name == "_rebuild_parameter" || name == "_rebuild_parameter_with_state") {
                // args[0] is the tensor
                if (args && !args->list.empty()) return args->list[0];
            }
            if (name == "OrderedDict" || name == "dict") {
                auto d = make(Value::DictT);
                if (args && !args->list.empty()) {
                    ValuePtr arg0 = args->list[0];
                    if (arg0 && (arg0->type == Value::ListT || arg0->type == Value::TupleT)) {
                        for (auto& pair : arg0->list) {
                            if (pair && pair->type == Value::TupleT && pair->list.size() == 2)
                                d->dict.push_back({pair->list[0], pair->list[1]});
                        }
                    }
                }
                return d;
            }
        }
        // Unknown callable: opaque, never execute.
        return make(Value::OpaqueT);
    }

    void run() {
        while (!r.eof()) {
            uint8_t op = r.u8();
            switch (op) {
            case 0x80: r.u8(); break; // PROTO
            case 0x95: r.u64(); break; // FRAME (skip length)
            case 0x2e: result = pop(); return; // STOP
            case 0x28: push(make(Value::MarkT)); break; // MARK
            case 0x29: push(make(Value::TupleT)); break; // EMPTY_TUPLE
            case 0x7d: push(make(Value::DictT)); break; // EMPTY_DICT
            case 0x5d: push(make(Value::ListT)); break; // EMPTY_LIST
            case 0x74: { // TUPLE
                long m = topMark();
                auto t = make(Value::TupleT);
                for (long i = m + 1; i < (long)stack.size(); ++i) t->list.push_back(stack[i]);
                stack.resize(m); stack.push_back(t); break;
            }
            case 0x6c: { // LIST
                long m = topMark();
                auto t = make(Value::ListT);
                for (long i = m + 1; i < (long)stack.size(); ++i) t->list.push_back(stack[i]);
                stack.resize(m); stack.push_back(t); break;
            }
            case 0x64: { // DICT
                long m = topMark();
                auto d = make(Value::DictT);
                for (long i = m + 1; i + 1 < (long)stack.size(); i += 2)
                    d->dict.push_back({stack[i], stack[i+1]});
                stack.resize(m); stack.push_back(d); break;
            }
            case 0x85: { auto a = pop(); auto t = make(Value::TupleT); t->list.push_back(a); push(t); break; } // TUPLE1
            case 0x86: { auto b = pop(); auto a = pop(); auto t = make(Value::TupleT); t->list.push_back(a); t->list.push_back(b); push(t); break; } // TUPLE2
            case 0x87: { auto c = pop(); auto b = pop(); auto a = pop(); auto t = make(Value::TupleT); t->list.push_back(a); t->list.push_back(b); t->list.push_back(c); push(t); break; } // TUPLE3
            case 0x61: { auto a = pop(); auto l = pop(); if (l->type != Value::ListT) throw std::runtime_error("APPEND not list"); l->list.push_back(a); push(l); break; } // APPEND
            case 0x73: { // SETITEM
                auto v = pop(); auto k = pop(); auto d = pop();
                if (d->type != Value::DictT) throw std::runtime_error("SETITEM no dict");
                d->dict.push_back({k, v}); push(d); break;
            }
            case 0x75: { // SETITEMS
                long m = topMark();
                auto d = (m > 0) ? stack[m-1] : ValuePtr();
                if (!d || d->type != Value::DictT) throw std::runtime_error("SETITEMS no dict");
                for (long i = m + 1; i + 1 < (long)stack.size(); i += 2)
                    d->dict.push_back({stack[i], stack[i+1]});
                stack.resize(m); break;
            }
            case 0x30: pop(); break; // POP
            case 0x31: { long m = topMark(); stack.resize(m); break; } // POP_MARK
            case 0x32: push(stack.back()); break; // DUP
            case 0x4e: push(make(Value::NoneT)); break; // NONE
            case 0x88: { auto v = make(Value::BoolT); v->b = true; push(v); break; } // NEWTRUE
            case 0x89: { auto v = make(Value::BoolT); v->b = false; push(v); break; } // NEWFALSE
            case 0x49: { // INT
                std::string s = readLine();
                if (!s.empty() && s.back() == 'L') s.pop_back();
                auto v = make(Value::IntT); v->i = parseDecimal(s); push(v); break;
            }
            case 0x4a: { auto v = make(Value::IntT); v->i = r.i32(); push(v); break; } // BININT
            case 0x4b: { auto v = make(Value::IntT); v->i = r.u8(); push(v); break; } // BININT1
            case 0x4d: { auto v = make(Value::IntT); v->i = r.u16(); push(v); break; } // BININT2
            case 0x4c: { // LONG
                std::string s = readLine();
                if (!s.empty() && s.back() == 'L') s.pop_back();
                auto v = make(Value::IntT); v->i = std::stoll(s); push(v); break;
            }
            case 0x8a: { // LONG1
                uint8_t n = r.u8();
                int64_t val = 0;
                if (n > 0) {
                    r.need(n);
                    for (int k = 0; k < (int)n; ++k) val |= (int64_t)r.p[r.i + k] << (8*k);
                    if (n < 8 && (r.p[r.i + n - 1] & 0x80))
                        for (int k = n; k < 8; ++k) val |= ((int64_t)0xFF << (8*k));
                    r.i += n;
                }
                auto v = make(Value::IntT); v->i = val; push(v); break;
            }
            case 0x8b: { // LONG4
                uint32_t n = r.u32();
                int64_t val = 0;
                if (n > 8) { r.need(n); r.i += n; } // ignore huge
                else if (n > 0) {
                    r.need(n);
                    for (int k = 0; k < (int)n; ++k) val |= (int64_t)r.p[r.i + k] << (8*k);
                    if (r.p[r.i + n - 1] & 0x80) for (int k = n; k < 8; ++k) val |= ((int64_t)0xFF << (8*k));
                    r.i += n;
                }
                auto v = make(Value::IntT); v->i = val; push(v); break;
            }
            case 0x46: { // FLOAT
                std::string s = readLine();
                auto v = make(Value::FloatT); v->f = std::stod(s); push(v); break;
            }
            case 0x47: { // BINFLOAT (big-endian 8 bytes)
                r.need(8);
                uint64_t bits = 0;
                for (int k = 0; k < 8; ++k) bits = (bits << 8) | r.p[r.i + k];
                r.i += 8;
                double d; std::memcpy(&d, &bits, 8);
                auto v = make(Value::FloatT); v->f = d; push(v); break;
            }
            case 0x56: { // UNICODE (raw-unicode-escape, newline-terminated)
                std::string s = readLine();
                auto v = make(Value::StrT); v->s = s; push(v); break;
            }
            case 0x8c: { uint8_t n = r.u8(); auto sv = r.bytes(n); auto v = make(Value::StrT); v->s.assign(sv); push(v); break; } // SHORT_BINUNICODE
            case 0x58: { uint32_t n = r.u32(); auto sv = r.bytes(n); auto v = make(Value::StrT); v->s.assign(sv); push(v); break; } // BINUNICODE
            case 0x8d: { uint64_t n = r.u64(); auto sv = r.bytes(n); auto v = make(Value::StrT); v->s.assign(sv); push(v); break; } // BINUNICODE8
            case 0x43: { uint8_t n = r.u8(); auto sv = r.bytes(n); auto v = make(Value::BytesT); v->s.assign(sv); push(v); break; } // SHORT_BINBYTES
            case 0x42: { uint32_t n = r.u32(); auto sv = r.bytes(n); auto v = make(Value::BytesT); v->s.assign(sv); push(v); break; } // BINBYTES
            case 0x8e: { uint64_t n = r.u64(); auto sv = r.bytes(n); auto v = make(Value::BytesT); v->s.assign(sv); push(v); break; } // BINBYTES8
            case 0x96: { uint64_t n = r.u64(); auto sv = r.bytes(n); auto v = make(Value::BytesT); v->s.assign(sv); push(v); break; } // BYTEARRAY8
            case 0x55: { uint8_t n = r.u8(); auto sv = r.bytes(n); auto v = make(Value::StrT); v->s.assign(sv); push(v); break; } // SHORT_BINSTRING
            case 0x54: { int32_t n = r.i32(); auto sv = r.bytes(n < 0 ? 0 : n); auto v = make(Value::StrT); v->s.assign(sv); push(v); break; } // BINSTRING
            case 0x53: { // STRING (repr-style, newline-terminated)
                std::string s = readLine();
                if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'')) s = s.substr(1, s.size() - 2);
                auto v = make(Value::StrT); v->s = s; push(v); break;
            }
            case 0x63: { // GLOBAL (module\nname\n)
                std::string mod = readLine();
                std::string nm = readLine();
                auto v = make(Value::GlobalT);
                v->global = std::make_shared<Global>(Global{mod, nm});
                push(v); break;
            }
            case 0x93: { // STACK_GLOBAL
                auto nm = pop(); auto mod = pop();
                auto v = make(Value::GlobalT);
                v->global = std::make_shared<Global>();
                v->global->module = (mod && mod->type == Value::StrT) ? mod->s : "";
                v->global->name = (nm && nm->type == Value::StrT) ? nm->s : "";
                push(v); break;
            }
            case 0x52: { // REDUCE
                auto args = pop(); auto callable = pop();
                if (args && args->type == Value::MarkT) throw std::runtime_error("REDUCE: args is mark");
                push(doReduce(callable, args)); break;
            }
            case 0x62: { // BUILD
                auto state = pop(); auto obj = pop();
                if (state && state->type == Value::DictT && obj && obj->type == Value::OpaqueT) {
                    // module-like: become its state dict so walker can traverse
                    push(state);
                } else {
                    push(obj);
                }
                break;
            }
            case 0x65: { // APPENDS
                long m = topMark();
                auto l = (m > 0) ? stack[m-1] : ValuePtr();
                if (!l || l->type != Value::ListT) throw std::runtime_error("APPENDS no list");
                for (long i = m + 1; i < (long)stack.size(); ++i) l->list.push_back(stack[i]);
                stack.resize(m); break;
            }
            case 0x81: { // NEWOBJ
                auto args = pop(); auto cls = pop();
                push(make(Value::OpaqueT)); break;
            }
            case 0x92: { // NEWOBJ_EX
                auto kwargs = pop(); auto args = pop(); auto cls = pop();
                push(make(Value::OpaqueT)); break;
            }
            case 0x6f: { // OBJ
                long m = topMark();
                // cls is stack[m+1]? Actually: mark, cls, args...
                // stack[m] = mark, stack[m+1] = cls, rest = args
                stack.resize(m);
                push(make(Value::OpaqueT)); break;
            }
            case 0x69: { // INST (module\nname\n) then mark...args
                readLine(); readLine();
                long m = topMark();
                stack.resize(m);
                push(make(Value::OpaqueT)); break;
            }
            case 0x51: { // BINPERSID
                auto pid = pop();
                push(persistentLoad(pid)); break;
            }
            case 0x50: { // PERSID
                readLine();
                push(make(Value::OpaqueT)); break;
            }
            case 0x71: { uint8_t idx = r.u8(); memoSet(idx, stack.back()); break; } // BINPUT
            case 0x72: { uint32_t idx = r.u32(); memoSet(idx, stack.back()); break; } // LONG_BINPUT
            case 0x70: { std::string s = readLine(); size_t idx = (size_t)std::stoll(s); memoSet(idx, stack.back()); break; } // PUT
            case 0x94: { memo.push_back(stack.back()); break; } // MEMOIZE
            case 0x68: { uint8_t idx = r.u8(); push(memoGet(idx)); break; } // BINGET
            case 0x6a: { uint32_t idx = r.u32(); push(memoGet(idx)); break; } // LONG_BINGET
            case 0x67: { std::string s = readLine(); size_t idx = (size_t)std::stoll(s); push(memoGet(idx)); break; } // GET
            case 0x82: { r.u8(); push(make(Value::OpaqueT)); break; } // EXT1
            case 0x83: { r.u16(); push(make(Value::OpaqueT)); break; } // EXT2
            case 0x84: { r.u32(); push(make(Value::OpaqueT)); break; } // EXT4
            case 0x97: push(make(Value::OpaqueT)); break; // NEXT_BUFFER
            case 0x98: break; // READONLY_BUFFER
            case 0x8f: push(make(Value::OpaqueT)); break; // EMPTY_SET (unused)
            case 0x90: { long m = topMark(); stack.resize(m); break; } // ADDITEMS
            case 0x91: { long m = topMark(); auto s = make(Value::OpaqueT); stack.resize(m); push(s); break; } // FROZENSET
            default:
                throw std::runtime_error("pickle: unknown opcode " + std::to_string((int)op));
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Walker: extract path -> tensor
// ---------------------------------------------------------------------------
struct RecTensor {
    std::string path;
    std::shared_ptr<TensorObj> t;
};

static std::string keyToStr(ValuePtr k) {
    if (!k) return "";
    if (k->type == Value::StrT) return k->s;
    if (k->type == Value::IntT) return std::to_string(k->i);
    if (k->type == Value::BytesT) return k->s;
    return "";
}

static void walk(ValuePtr root, std::vector<RecTensor>& out) {
    std::vector<std::pair<ValuePtr, std::string>> stack;
    stack.push_back({root, ""});
    std::unordered_set<const Value*> visited;
    const size_t MAX_VISITS = 2000000;
    size_t visits = 0;
    while (!stack.empty()) {
        if (++visits > MAX_VISITS) break;
        auto [v, path] = stack.back();
        stack.pop_back();
        if (!v) continue;
        if (v->type == Value::TensorT) {
            if (v->tensor) out.push_back({path, v->tensor});
            continue;
        }
        if (v->type == Value::DictT) {
            if (!visited.insert(v.get()).second) continue;
            for (auto it = v->dict.rbegin(); it != v->dict.rend(); ++it) {
                std::string k = keyToStr(it->first);
                if (k.empty()) continue;
                std::string sub = path.empty() ? k : path + "/" + k;
                stack.push_back({it->second, sub});
            }
        } else if (v->type == Value::ListT || v->type == Value::TupleT) {
            if (!visited.insert(v.get()).second) continue;
            for (size_t i = v->list.size(); i-- > 0;) {
                std::string sub = path.empty() ? std::to_string(i) : path + "/" + std::to_string(i);
                stack.push_back({v->list[i], sub});
            }
        }
        // else: skip (None, int, opaque, storage, global, ...)
    }
}

// ---------------------------------------------------------------------------
// JSON output (sorted keys, indent=1)
// ---------------------------------------------------------------------------
static void jsonEscape(std::string& out, const std::string& s) {
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
            if ((uint8_t)c < 0x20) {
                char buf[8]; snprintf(buf, 8, "\\u%04x", (uint8_t)c);
                out += buf;
            } else out += c;
        }
    }
    out += '"';
}

static void jsonInts(std::string& out, const std::vector<int64_t>& v, int indent) {
    if (v.empty()) { out += "[]"; return; }
    std::string pad(indent, ' ');
    std::string padItem(indent + 1, ' ');
    out += "[\n";
    for (size_t i = 0; i < v.size(); ++i) {
        out += padItem;
        out += std::to_string(v[i]);
        if (i + 1 < v.size()) out += ',';
        out += '\n';
    }
    out += pad;
    out += ']';
}

static void jsonTensor(std::string& out, std::shared_ptr<TensorObj> t, int indent) {
    std::string pad(indent, ' ');
    std::string padItem(indent + 1, ' ');
    out += "{\n";
    int64_t n = 1;
    for (int64_t d : t->shape) n *= d;
    int itemsize = t->storage ? t->storage->itemsize : 0;
    std::string dtype = t->storage ? t->storage->dtype : "";
    int64_t nbytes = n * itemsize;
    // sorted keys: dtype, nbytes, shape, storage_key, storage_offset, stride
    out += padItem; jsonEscape(out, "dtype"); out += ": "; jsonEscape(out, dtype); out += ",\n";
    out += padItem; jsonEscape(out, "nbytes"); out += ": "; out += std::to_string(nbytes); out += ",\n";
    out += padItem; jsonEscape(out, "shape"); out += ": "; jsonInts(out, t->shape, indent + 1); out += ",\n";
    out += padItem; jsonEscape(out, "storage_key"); out += ": "; jsonEscape(out, t->storage ? t->storage->key : ""); out += ",\n";
    out += padItem; jsonEscape(out, "storage_offset"); out += ": "; out += std::to_string(t->offset); out += ",\n";
    out += padItem; jsonEscape(out, "stride"); out += ": "; jsonInts(out, t->stride, indent + 1); out += '\n';
    out += pad;
    out += '}';
}

// f16 (IEEE half) → f32 (IEEE single) bit conversion
static uint32_t f16ToF32Bits(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h & 0x7C00) >> 10;
    uint32_t mant = h & 0x03FF;
    if (exp == 0) {
        if (mant == 0) return sign; // ±0
        // subnormal: normalize
        exp = 1;
        while (!(mant & 0x0400)) { mant <<= 1; --exp; }
        mant &= 0x03FF;
        return sign | ((exp + 112) << 23) | (mant << 13);
    }
    if (exp == 31) return sign | 0x7F800000 | (mant << 13); // inf/nan
    return sign | ((exp + 112) << 23) | (mant << 13);
}

// RVC tensor JSON: only dtype, nbytes, shape (no source-layout fields)
static void jsonTensorRvc(std::string& out, std::shared_ptr<TensorObj> t, int indent) {
    std::string pad(indent, ' ');
    std::string padItem(indent + 1, ' ');
    out += "{\n";
    int64_t n = 1;
    for (int64_t d : t->shape) n *= d;
    int64_t nbytes = n * 4; // f32
    out += padItem; jsonEscape(out, "dtype"); out += ": "; jsonEscape(out, "f32"); out += ",\n";
    out += padItem; jsonEscape(out, "nbytes"); out += ": "; out += std::to_string(nbytes); out += ",\n";
    out += padItem; jsonEscape(out, "shape"); out += ": "; jsonInts(out, t->shape, indent + 1); out += '\n';
    out += pad;
    out += '}';
}

// Convert a Value to JSON string (for RVC config), with given indent level.
static void jsonValue(std::string& out, ValuePtr v, int indent);
static void jsonList(std::string& out, const std::vector<ValuePtr>& items, int indent) {
    if (items.empty()) { out += "[]"; return; }
    std::string pad(indent, ' ');
    std::string padItem(indent + 1, ' ');
    out += "[\n";
    for (size_t i = 0; i < items.size(); ++i) {
        out += padItem;
        jsonValue(out, items[i], indent + 1);
        if (i + 1 < items.size()) out += ',';
        out += '\n';
    }
    out += pad;
    out += ']';
}
static void jsonValue(std::string& out, ValuePtr v, int indent) {
    if (!v) { out += "null"; return; }
    switch (v->type) {
    case Value::NoneT: out += "null"; break;
    case Value::BoolT: out += v->b ? "true" : "false"; break;
    case Value::IntT: out += std::to_string(v->i); break;
    case Value::FloatT: {
        // Match Python repr: 0.0 stays 0.0
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", v->f);
        std::string s(buf);
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos
            && s.find('n') == std::string::npos && s.find('i') == std::string::npos)
            s += ".0";
        out += s;
        break;
    }
    case Value::StrT: jsonEscape(out, v->s); break;
    case Value::BytesT: jsonEscape(out, v->s); break;
    case Value::ListT:
    case Value::TupleT: jsonList(out, v->list, indent); break;
    default: out += "null"; break;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <file.pth> <out_dir>\n", argv[0]);
        return 2;
    }
    std::string inPath = argv[1];
    std::string outDir = argv[2];

    try {
        ZipFile zip;
        zip.read(inPath);

        // find data.pkl entry and derive prefix
        std::string pklName;
        for (auto& e : zip.entries) {
            if (e.name.size() >= 9 && e.name.compare(e.name.size() - 9, 9, "/data.pkl") == 0) {
                pklName = e.name; break;
            }
        }
        if (pklName.empty()) throw std::runtime_error("no data.pkl in archive");

        std::string prefix = pklName.substr(0, pklName.size() - std::string("/data.pkl").size());

        std::vector<uint8_t> pklBytes = zip.readEntry(zip.entry(pklName));

        Pickler pk(pklBytes.data(), pklBytes.size());
        pk.run();
        if (!pk.result) throw std::runtime_error("pickle: no result");

        std::vector<RecTensor> recs;
        // RVC detection: top-level dict with "config" (list) and "weight" (dict)
        bool isRvc = false;
        ValuePtr configList;
        ValuePtr weightDict;
        if (pk.result && pk.result->type == Value::DictT) {
            for (auto& kv : pk.result->dict) {
                std::string k = keyToStr(kv.first);
                if (k == "config" && kv.second && kv.second->type == Value::ListT
                    && kv.second->list.size() >= 18) {
                    isRvc = true; configList = kv.second;
                }
                if (k == "weight" && kv.second && kv.second->type == Value::DictT)
                    weightDict = kv.second;
            }
        }
        if (isRvc && weightDict) {
            walk(weightDict, recs);
        } else {
            walk(pk.result, recs);
        }

        // sort by path for determinism
        std::sort(recs.begin(), recs.end(), [](const RecTensor& a, const RecTensor& b) { return a.path < b.path; });

        // storage data cache
        std::unordered_map<std::string, std::vector<uint8_t>> storageCache;
        auto getStorage = [&](const std::string& key) -> const std::vector<uint8_t>* {
            auto it = storageCache.find(key);
            if (it != storageCache.end()) return &it->second;
            std::string entryName = prefix + "/data/" + key;
            if (!zip.has(entryName)) return nullptr;
            std::vector<uint8_t> b = zip.readEntry(zip.entry(entryName));
            auto ins = storageCache.emplace(key, std::move(b));
            return &ins.first->second;
        };

        // create output dirs
        fs::create_directories(outDir);
        fs::create_directories(outDir + "/tensors");

        // materialize tensors
        for (auto& rec : recs) {
            auto& t = rec.t;
            int srcItemsize = t->storage ? t->storage->itemsize : 0;
            if (srcItemsize <= 0) continue;
            int64_t n = 1;
            for (int64_t d : t->shape) n *= d;
            // RVC: convert f16 → f32 (2x size)
            int outItemsize = isRvc ? 4 : srcItemsize;
            int64_t nbytes = n * outItemsize;

            std::string binName;
            for (char c : rec.path) binName += (c == '/') ? "__" : std::string(1, c);
            binName += ".bin";

            std::string binPath = outDir + "/tensors/" + binName;
            FILE* f = fopen(binPath.c_str(), "wb");
            if (!f) throw std::runtime_error("cannot write " + binPath);

            const std::vector<uint8_t>* sdata = nullptr;
            if (n > 0) {
                sdata = getStorage(t->storage ? t->storage->key : "");
            }

            if (nbytes == 0) {
                // empty file
            } else if (!sdata) {
                std::vector<uint8_t> zeros(nbytes, 0);
                fwrite(zeros.data(), 1, nbytes, f);
            } else {
                int64_t storageBytes = (int64_t)sdata->size();
                std::vector<uint8_t> outBuf(nbytes, 0);
                int ndim = (int)t->shape.size();
                std::vector<int64_t> idx(ndim, 0);
                for (int64_t el = 0; el < n; ++el) {
                    int64_t src = t->offset;
                    for (int d = 0; d < ndim; ++d) src += idx[d] * t->stride[d];
                    int64_t byteOff = src * srcItemsize;
                    if (byteOff >= 0 && byteOff + srcItemsize <= storageBytes) {
                        if (isRvc && srcItemsize == 2) {
                            // f16 → f32
                            uint16_t h = (uint16_t)sdata->data()[byteOff]
                                       | ((uint16_t)sdata->data()[byteOff+1] << 8);
                            uint32_t f = f16ToF32Bits(h);
                            std::memcpy(&outBuf[el * 4], &f, 4);
                        } else {
                            std::memcpy(&outBuf[el * outItemsize],
                                        sdata->data() + byteOff, srcItemsize);
                        }
                    }
                    for (int d = ndim - 1; d >= 0; --d) {
                        if (++idx[d] < t->shape[d]) break;
                        idx[d] = 0;
                    }
                }
                fwrite(outBuf.data(), 1, nbytes, f);
            }
            fclose(f);
        }

        // write manifest.json (sorted keys, indent=1)
        // Top-level keys sorted: byteorder, config, nulltorch_manifest, tensors
        std::string j;
        j += "{\n";
        j += " \"byteorder\": \"little\",\n";

        // RVC config block
        if (isRvc && configList && configList->list.size() >= 18) {
            // Positional args to SynthesizerTrnMsNSFsid:
            // 0:spec_channels 1:segment_size 2:inter_channels 3:hidden_channels
            // 4:filter_channels 5:n_heads 6:n_layers 7:kernel_size
            // 8:p_dropout 9:resblock 10:resblock_kernel_sizes
            // 11:resblock_dilation_sizes 12:upsample_rates
            // 13:upsample_initial_channel 14:upsample_kernel_sizes
            // 15:n_speakers 16:gin_channels 17:sr
            // phone_dim = filter_channels
            auto& a = configList->list;
            std::vector<std::pair<std::string, ValuePtr>> cfg = {
                {"filter_channels", a[4]},
                {"gin_channels", a[16]},
                {"hidden_channels", a[3]},
                {"inter_channels", a[2]},
                {"kernel_size", a[7]},
                {"n_heads", a[5]},
                {"n_layers", a[6]},
                {"n_speakers", a[15]},
                {"p_dropout", a[8]},
                {"phone_dim", a[4]},  // = filter_channels
                {"resblock", a[9]},
                {"resblock_dilation_sizes", a[11]},
                {"resblock_kernel_sizes", a[10]},
                {"segment_size", a[1]},
                {"spec_channels", a[0]},
                {"sr", a[17]},
                {"upsample_initial_channel", a[13]},
                {"upsample_kernel_sizes", a[14]},
                {"upsample_rates", a[12]},
            };
            // already sorted alphabetically
            j += " \"config\": {\n";
            for (size_t i = 0; i < cfg.size(); ++i) {
                j += "  ";
                jsonEscape(j, cfg[i].first);
                j += ": ";
                jsonValue(j, cfg[i].second, 2);
                if (i + 1 < cfg.size()) j += ',';
                j += '\n';
            }
            j += " },\n";
        }

        j += " \"nulltorch_manifest\": 1,\n";
        j += " \"tensors\": {";
        if (recs.empty()) {
            j += "}\n";
        } else {
            j += "\n";
            for (size_t i = 0; i < recs.size(); ++i) {
                j += "  ";
                jsonEscape(j, recs[i].path);
                j += ": ";
                if (isRvc) jsonTensorRvc(j, recs[i].t, 2); else jsonTensor(j, recs[i].t, 2);
                if (i + 1 < recs.size()) j += ',';
                j += '\n';
            }
            j += " }\n";
        }
        j += "}\n";

        std::string manifestPath = outDir + "/manifest.json";
        FILE* mf = fopen(manifestPath.c_str(), "wb");
        if (!mf) throw std::runtime_error("cannot write manifest");
        fwrite(j.data(), 1, j.size(), mf);
        fclose(mf);

        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
