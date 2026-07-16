// NullTorch delta-variant ("pth-prime") .pth converter.
// Reads a PyTorch checkpoint (delta ZIP container + pickle stream) with no
// third-party libraries, materializes every tensor contiguous row-major in
// its source dtype, and writes manifest.json + tensors/*.bin.
//
// Usage: ./convert <file.pth> <out_dir>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

struct Fail : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

static std::vector<uint8_t> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw Fail("cannot open input: " + path);
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  if (n > 0 && !f.read(reinterpret_cast<char *>(buf.data()), n))
    throw Fail("cannot read input: " + path);
  return buf;
}

static uint16_t le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
static uint64_t le64(const uint8_t *p) {
  return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

// ---------------------------------------------------------------------------
// Inflate (RFC 1951 raw DEFLATE) — needed only for method-8 entries.
// ---------------------------------------------------------------------------

namespace {

struct BitReader {
  const uint8_t *p;
  size_t len;   // bytes
  size_t pos;   // byte position
  uint32_t bit; // bit position within current byte (0..7)

  uint32_t bits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      if (pos >= len) throw Fail("inflate: out of input");
      v |= static_cast<uint32_t>((p[pos] >> bit) & 1u) << i;
      if (++bit == 8) { bit = 0; ++pos; }
    }
    return v;
  }
  void align_byte() {
    if (bit) { bit = 0; ++pos; }
  }
};

struct Huffman {
  // Canonical Huffman decoding from code lengths (RFC 1951 3.2.2).
  // counts[len] = number of symbols with that length; symbols sorted.
  std::vector<int> counts;   // index = bit length (0 unused)
  std::vector<int> symbols;  // sorted by (length, symbol)
  int max_len = 0;

  void build(const std::vector<int> &lengths) {
    counts.assign(16, 0);
    for (int l : lengths)
      if (l > 0) {
        if (l > 15) throw Fail("inflate: code length > 15");
        counts[l]++;
      }
    max_len = 0;
    for (int l = 15; l >= 1; --l)
      if (counts[l]) { max_len = l; break; }
    symbols.clear();
    for (int l = 1; l <= 15; ++l)
      for (size_t s = 0; s < lengths.size(); ++s)
        if (lengths[s] == l) symbols.push_back(static_cast<int>(s));
  }

  // Decode one symbol, reading code MSB-first.
  int decode(BitReader &br) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= max_len; ++len) {
      code |= static_cast<int>(br.bits(1));
      int count = counts[len];
      if (code - first < count) return symbols[index + (code - first)];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    throw Fail("inflate: invalid huffman code");
  }
};

const int kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                          15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                          67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                           1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                           4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,
                           17,   25,   33,   49,   65,   97,   129,  193,
                           257,  385,  513,  769,  1025, 1537, 2049, 3073,
                           4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,
                            4, 4, 5, 5, 6, 6, 7,  7,  8,  8,
                            9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

std::vector<uint8_t> inflate(const uint8_t *data, size_t len,
                             size_t expected_size) {
  BitReader br{data, len, 0, 0};
  std::vector<uint8_t> out;
  out.reserve(expected_size);
  bool last = false;
  while (!last) {
    last = br.bits(1) != 0;
    uint32_t btype = br.bits(2);
    if (btype == 0) {
      br.align_byte();
      if (br.pos + 4 > len) throw Fail("inflate: truncated stored block");
      uint16_t blen = le16(data + br.pos);
      uint16_t nlen = le16(data + br.pos + 2);
      if (blen != static_cast<uint16_t>(~nlen))
        throw Fail("inflate: LEN/NLEN mismatch");
      br.pos += 4;
      if (br.pos + blen > len) throw Fail("inflate: truncated stored data");
      out.insert(out.end(), data + br.pos, data + br.pos + blen);
      br.pos += blen;
    } else if (btype == 1 || btype == 2) {
      Huffman lit, dist;
      if (btype == 1) {
        std::vector<int> ll(288, 8);
        for (int i = 144; i < 256; ++i) ll[i] = 9;
        for (int i = 256; i < 280; ++i) ll[i] = 7;
        lit.build(ll);
        dist.build(std::vector<int>(30, 5));
      } else {
        int hlit = static_cast<int>(br.bits(5)) + 257;
        int hdist = static_cast<int>(br.bits(5)) + 1;
        int hclen = static_cast<int>(br.bits(4)) + 4;
        static const int kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
        std::vector<int> cl(19, 0);
        for (int i = 0; i < hclen; ++i) cl[kOrder[i]] = static_cast<int>(br.bits(3));
        Huffman code_len;
        code_len.build(cl);
        std::vector<int> lengths;
        lengths.reserve(static_cast<size_t>(hlit + hdist));
        while (static_cast<int>(lengths.size()) < hlit + hdist) {
          int sym = code_len.decode(br);
          if (sym < 16) {
            lengths.push_back(sym);
          } else if (sym == 16) {
            if (lengths.empty()) throw Fail("inflate: repeat with no previous");
            int rep = 3 + static_cast<int>(br.bits(2));
            int prev = lengths.back();
            for (int i = 0; i < rep; ++i) lengths.push_back(prev);
          } else if (sym == 17) {
            int rep = 3 + static_cast<int>(br.bits(3));
            for (int i = 0; i < rep; ++i) lengths.push_back(0);
          } else if (sym == 18) {
            int rep = 11 + static_cast<int>(br.bits(7));
            for (int i = 0; i < rep; ++i) lengths.push_back(0);
          } else {
            throw Fail("inflate: bad code-length symbol");
          }
        }
        if (static_cast<int>(lengths.size()) != hlit + hdist)
          throw Fail("inflate: code-length overrun");
        lit.build(std::vector<int>(lengths.begin(), lengths.begin() + hlit));
        dist.build(std::vector<int>(lengths.begin() + hlit, lengths.end()));
      }
      for (;;) {
        int sym = lit.decode(br);
        if (sym == 256) break;
        if (sym < 256) {
          out.push_back(static_cast<uint8_t>(sym));
        } else {
          int li = sym - 257;
          if (li >= 29) throw Fail("inflate: bad length symbol");
          int match_len = kLenBase[li] + static_cast<int>(br.bits(kLenExtra[li]));
          int dsym = dist.decode(br);
          if (dsym >= 30) throw Fail("inflate: bad distance symbol");
          int d = kDistBase[dsym] + static_cast<int>(br.bits(kDistExtra[dsym]));
          if (d > static_cast<int>(out.size()))
            throw Fail("inflate: distance too far back");
          size_t from = out.size() - static_cast<size_t>(d);
          for (int i = 0; i < match_len; ++i)
            out.push_back(out[from + static_cast<size_t>(i)]);
        }
      }
    } else {
      throw Fail("inflate: reserved block type");
    }
  }
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Delta ZIP reader ("DZ" signatures; layouts identical to stock ZIP).
// ---------------------------------------------------------------------------

struct ZipEntry {
  std::string name;
  uint16_t method = 0;
  uint64_t comp_size = 0;
  uint64_t uncomp_size = 0;
  uint64_t local_offset = 0;
};

class ZipArchive {
public:
  explicit ZipArchive(std::vector<uint8_t> bytes) : buf_(std::move(bytes)) {
    parse();
  }

  const std::vector<ZipEntry> &entries() const { return entries_; }

  std::vector<uint8_t> extract(const ZipEntry &e) const {
    const uint8_t *lh = at(e.local_offset, 30);
    if (std::memcmp(lh, "DZ\x03\x04", 4) != 0)
      throw Fail("bad local file header signature for " + e.name);
    uint16_t nlen = le16(lh + 26);
    uint16_t mlen = le16(lh + 28);
    uint64_t data_off = e.local_offset + 30 + nlen + mlen;
    const uint8_t *data = at(data_off, e.comp_size);
    if (e.method == 0) {
      return std::vector<uint8_t>(data, data + e.comp_size);
    } else if (e.method == 8) {
      return inflate(data, static_cast<size_t>(e.comp_size),
                     static_cast<size_t>(e.uncomp_size));
    }
    throw Fail("unsupported compression method " + std::to_string(e.method) +
               " for " + e.name);
  }

private:
  std::vector<uint8_t> buf_;
  std::vector<ZipEntry> entries_;

  const uint8_t *at(uint64_t off, uint64_t need) const {
    if (off + need > buf_.size()) throw Fail("zip: unexpected end of file");
    return buf_.data() + off;
  }

  void parse() {
    if (buf_.size() < 22) throw Fail("zip: file too small");
    // Locate EOCD by scanning backwards for the delta signature.
    size_t lo = buf_.size() > 66000 ? buf_.size() - 66000 : 0;
    size_t eocd = SIZE_MAX;
    for (size_t i = buf_.size() - 22 + 1; i-- > lo;) {
      if (std::memcmp(buf_.data() + i, "DZ\x05\x06", 4) == 0) {
        uint16_t clen = le16(buf_.data() + i + 20);
        if (i + 22 + clen == buf_.size()) { eocd = i; break; }
      }
    }
    if (eocd == SIZE_MAX) throw Fail("zip: end of central directory not found");

    const uint8_t *ep = buf_.data() + eocd;
    uint64_t nrec = le16(ep + 10);
    uint64_t cd_off = le32(ep + 16);
    uint64_t cd_size = le32(ep + 12);

    bool saturated = (nrec == 0xFFFF) || (cd_off == 0xFFFFFFFFull) ||
                     (cd_size == 0xFFFFFFFFull) || le16(ep + 8) == 0xFFFF;
    if (saturated) {
      // Zip64: locator immediately precedes the EOCD.
      const uint8_t *loc = at(eocd >= 20 ? eocd - 20 : 0, 20);
      if (std::memcmp(loc, "DZ\x06\x07", 4) != 0)
        throw Fail("zip: zip64 locator not found");
      uint64_t z64_off = le64(loc + 8);
      const uint8_t *z = at(z64_off, 56);
      if (std::memcmp(z, "DZ\x06\x06", 4) != 0)
        throw Fail("zip: bad zip64 EOCD signature");
      nrec = le64(z + 32);
      cd_size = le64(z + 40);
      cd_off = le64(z + 48);
    }
    (void)cd_size;

    uint64_t p = cd_off;
    for (uint64_t i = 0; i < nrec; ++i) {
      const uint8_t *h = at(p, 46);
      if (std::memcmp(h, "DZ\x01\x02", 4) != 0)
        throw Fail("zip: bad central directory signature");
      ZipEntry e;
      e.method = le16(h + 10);
      e.comp_size = le32(h + 20);
      e.uncomp_size = le32(h + 24);
      uint16_t nlen = le16(h + 28);
      uint16_t mlen = le16(h + 30);
      uint16_t klen = le16(h + 32);
      e.local_offset = le32(h + 42);
      e.name.assign(reinterpret_cast<const char *>(h + 46), nlen);
      // Zip64 extended-information extra field (id 0x0001).
      bool unc_sat = e.uncomp_size == 0xFFFFFFFFull;
      bool cmp_sat = e.comp_size == 0xFFFFFFFFull;
      bool off_sat = e.local_offset == 0xFFFFFFFFull;
      if (unc_sat || cmp_sat || off_sat) {
        const uint8_t *x = at(p + 46 + nlen, mlen);
        size_t q = 0;
        bool done = false;
        while (q + 4 <= mlen && !done) {
          uint16_t id = le16(x + q);
          uint16_t sz = le16(x + q + 2);
          if (q + 4 + sz > static_cast<size_t>(mlen)) break;
          if (id == 0x0001) {
            const uint8_t *v = x + q + 4;
            size_t left = sz;
            if (unc_sat) { e.uncomp_size = le64(v); v += 8; left -= 8; }
            if (cmp_sat) { e.comp_size = le64(v); v += 8; left -= 8; }
            if (off_sat) { e.local_offset = le64(v); v += 8; left -= 8; }
            (void)left;
            done = true;
          }
          q += 4 + sz;
        }
      }
      entries_.push_back(std::move(e));
      p += 46 + nlen + mlen + klen;
    }
  }
};

// ---------------------------------------------------------------------------
// Pickle virtual machine (protocols 0-4 opcodes).
// ---------------------------------------------------------------------------

struct Obj;
using ObjPtr = std::shared_ptr<Obj>;

struct Obj {
  enum Kind {
    kNone, kBool, kInt, kFloat, kStr, kBytes, kList, kTuple, kDict,
    kGlobal, kStorage, kTensor, kOpaque, kMark
  } kind = kNone;

  bool boolean = false;
  int64_t integer = 0;
  double real = 0.0;
  std::string str;                              // kStr, kBytes
  std::vector<ObjPtr> items;                    // kList, kTuple
  std::vector<std::pair<ObjPtr, ObjPtr>> kv;    // kDict (insertion order)
  std::string mod, name;                        // kGlobal
  // kStorage:
  std::string storage_key;
  std::string dtype;    // manifest token: f32 f16 bf16 f64 i64 i32 i16 i8 u8 bool
  int itemsize = 0;
  int64_t numel = 0;
  // kTensor:
  ObjPtr storage;
  int64_t storage_offset = 0;
  std::vector<int64_t> shape;
  std::vector<int64_t> stride;

  static ObjPtr make(Kind k) {
    auto o = std::make_shared<Obj>();
    o->kind = k;
    return o;
  }
  static ObjPtr make_int(int64_t v) {
    auto o = make(kInt);
    o->integer = v;
    return o;
  }
  static ObjPtr make_str(std::string v) {
    auto o = make(kStr);
    o->str = std::move(v);
    return o;
  }
};

// dtype token + itemsize from a (delta) storage class name.
static bool storage_class_to_dtype(const std::string &cls, std::string &dtype,
                                   int &itemsize) {
  // Strip the delta "Vault" / stock "Storage" suffix.
  std::string base = cls;
  for (const char *suf : {"Vault", "Storage"}) {
    size_t n = std::strlen(suf);
    if (base.size() > n && base.compare(base.size() - n, n, suf) == 0) {
      base.resize(base.size() - n);
      break;
    }
  }
  static const std::pair<const char *, std::pair<const char *, int>> kMap[] = {
      {"Float", {"f32", 4}},  {"Half", {"f16", 2}}, {"BFloat16", {"bf16", 2}},
      {"Double", {"f64", 8}}, {"Long", {"i64", 8}}, {"Int", {"i32", 4}},
      {"Short", {"i16", 2}},  {"Char", {"i8", 1}},  {"Byte", {"u8", 1}},
      {"Bool", {"bool", 1}},
  };
  for (const auto &m : kMap) {
    if (base == m.first) {
      dtype = m.second.first;
      itemsize = m.second.second;
      return true;
    }
  }
  return false;
}

class PickleVM {
public:
  explicit PickleVM(const std::vector<uint8_t> &data)
      : d_(data.data()), n_(data.size()) {}

  ObjPtr run() {
    while (pos_ < n_) {
      uint8_t op = d_[pos_++];
      switch (op) {
      case 0x80: // PROTO
        read_u1();
        break;
      case 0x95: // FRAME
        read_u8();
        break;
      case 0x2e: // STOP
        if (stack_.empty()) throw Fail("pickle: STOP with empty stack");
        return stack_.back();
      case 0x28: // MARK
        stack_.push_back(Obj::make(Obj::kMark));
        break;
      case 0x30: // POP
        pop();
        break;
      case 0x32: // DUP
        if (stack_.empty()) throw Fail("pickle: DUP underflow");
        stack_.push_back(stack_.back());
        break;
      case 0x31: // POP_MARK
        pop_mark();
        break;

      case 0x49: { // INT (also encodes bool as 01/00)
        std::string s = read_line();
        if (s == "01") push_bool(true);
        else if (s == "00") push_bool(false);
        else push_int(parse_dec(s));
        break;
      }
      case 0x4a: // BININT
        push_int(static_cast<int32_t>(read_u4()));
        break;
      case 0x4b: // BININT1
        push_int(read_u1());
        break;
      case 0x4d: // BININT2
        push_int(read_u2());
        break;
      case 0x4c: // LONG (decimal, optional trailing L)
        push_int(parse_dec(read_line()));
        break;
      case 0x8a: // LONG1
        push_int(read_long_n(read_u1()));
        break;
      case 0x8b: // LONG4
        push_int(read_long_n(read_u4()));
        break;

      case 0x53: // STRING (repr-style)
        push(Obj::make_str(parse_string_repr(read_line())));
        break;
      case 0x54: // BINSTRING
        push(Obj::make_str(read_bytes(static_cast<int32_t>(read_u4()))));
        break;
      case 0x55: // SHORT_BINSTRING
        push(Obj::make_str(read_bytes(read_u1())));
        break;
      case 0x56: // UNICODE (raw-unicode-escape line; pass through)
        push(Obj::make_str(read_line()));
        break;
      case 0x58: // BINUNICODE
        push(Obj::make_str(read_bytes_u(read_u4())));
        break;
      case 0x8c: // SHORT_BINUNICODE
        push(Obj::make_str(read_bytes_u(read_u1())));
        break;
      case 0x8d: // BINUNICODE8
        push(Obj::make_str(read_bytes_u(read_u8())));
        break;

      case 0x42: { // BINBYTES
        auto o = Obj::make(Obj::kBytes);
        o->str = read_bytes_u(read_u4());
        push(o);
        break;
      }
      case 0x43: { // SHORT_BINBYTES
        auto o = Obj::make(Obj::kBytes);
        o->str = read_bytes_u(read_u1());
        push(o);
        break;
      }
      case 0x8e: { // BINBYTES8
        auto o = Obj::make(Obj::kBytes);
        o->str = read_bytes_u(read_u8());
        push(o);
        break;
      }
      case 0x96: { // BYTEARRAY8 -> treat as bytes
        auto o = Obj::make(Obj::kBytes);
        o->str = read_bytes_u(read_u8());
        push(o);
        break;
      }

      case 0x4e: // NONE
        push(Obj::make(Obj::kNone));
        break;
      case 0x88: // NEWTRUE
        push_bool(true);
        break;
      case 0x89: // NEWFALSE
        push_bool(false);
        break;
      case 0x46: { // FLOAT (decimal line)
        auto o = Obj::make(Obj::kFloat);
        o->real = std::strtod(read_line().c_str(), nullptr);
        push(o);
        break;
      }
      case 0x47: { // BINFLOAT (8 bytes, big-endian)
        need(8);
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u = (u << 8) | d_[pos_ + i];
        pos_ += 8;
        auto o = Obj::make(Obj::kFloat);
        static_assert(sizeof(double) == 8);
        std::memcpy(&o->real, &u, 8);
        push(o);
        break;
      }

      case 0x5d: // EMPTY_LIST
        push(Obj::make(Obj::kList));
        break;
      case 0x61: { // APPEND
        ObjPtr v = pop();
        ObjPtr l = top();
        if (l->kind != Obj::kList) throw Fail("pickle: APPEND target not list");
        l->items.push_back(v);
        break;
      }
      case 0x65: { // APPENDS
        auto slice = pop_mark();
        ObjPtr l = top();
        if (l->kind != Obj::kList) throw Fail("pickle: APPENDS target not list");
        for (auto &v : slice) l->items.push_back(v);
        break;
      }
      case 0x6c: { // LIST
        auto slice = pop_mark();
        auto o = Obj::make(Obj::kList);
        o->items = std::move(slice);
        push(o);
        break;
      }
      case 0x29: // EMPTY_TUPLE
        push(Obj::make(Obj::kTuple));
        break;
      case 0x74: { // TUPLE
        auto slice = pop_mark();
        auto o = Obj::make(Obj::kTuple);
        o->items = std::move(slice);
        push(o);
        break;
      }
      case 0x85: { // TUPLE1
        ObjPtr a = pop();
        push_tuple({a});
        break;
      }
      case 0x86: { // TUPLE2
        ObjPtr b = pop();
        ObjPtr a = pop();
        push_tuple({a, b});
        break;
      }
      case 0x87: { // TUPLE3
        ObjPtr c = pop();
        ObjPtr b = pop();
        ObjPtr a = pop();
        push_tuple({a, b, c});
        break;
      }

      case 0x7d: // EMPTY_DICT
        push(Obj::make(Obj::kDict));
        break;
      case 0x64: { // DICT
        auto slice = pop_mark();
        auto o = Obj::make(Obj::kDict);
        dict_fill(o, slice);
        push(o);
        break;
      }
      case 0x73: { // SETITEM
        ObjPtr v = pop();
        ObjPtr k = pop();
        ObjPtr dct = top();
        ensure_dict(dct);
        dct->kv.emplace_back(k, v);
        break;
      }
      case 0x75: { // SETITEMS
        auto slice = pop_mark();
        ObjPtr dct = top();
        ensure_dict(dct);
        dict_fill(dct, slice);
        break;
      }
      case 0x8f: // EMPTY_SET -> list-like container
        push(Obj::make(Obj::kList));
        break;
      case 0x90: { // ADDITEMS
        auto slice = pop_mark();
        ObjPtr s = top();
        if (s->kind != Obj::kList) { s->kind = Obj::kList; s->items.clear(); }
        for (auto &v : slice) s->items.push_back(v);
        break;
      }
      case 0x91: { // FROZENSET -> tuple
        auto slice = pop_mark();
        auto o = Obj::make(Obj::kTuple);
        o->items = std::move(slice);
        push(o);
        break;
      }

      case 0x67: // GET
        push(memo_get(parse_dec(read_line())));
        break;
      case 0x68: // BINGET
        push(memo_get(read_u1()));
        break;
      case 0x6a: // LONG_BINGET
        push(memo_get(read_u4()));
        break;
      case 0x70: // PUT
        memo_put(parse_dec(read_line()));
        break;
      case 0x71: // BINPUT
        memo_put(read_u1());
        break;
      case 0x72: // LONG_BINPUT
        memo_put(read_u4());
        break;
      case 0x94: // MEMOIZE
        memo_.push_back(top());
        break;

      case 0x63: { // GLOBAL
        std::string mod = read_line();
        std::string name = read_line();
        auto o = Obj::make(Obj::kGlobal);
        o->mod = std::move(mod);
        o->name = std::move(name);
        push(o);
        break;
      }
      case 0x93: { // STACK_GLOBAL
        ObjPtr name = pop();
        ObjPtr mod = pop();
        auto o = Obj::make(Obj::kGlobal);
        if (mod->kind == Obj::kStr) o->mod = mod->str;
        if (name->kind == Obj::kStr) o->name = name->str;
        push(o);
        break;
      }
      case 0x82: // EXT1
        read_u1();
        push(Obj::make(Obj::kOpaque));
        break;
      case 0x83: // EXT2
        read_u2();
        push(Obj::make(Obj::kOpaque));
        break;
      case 0x84: // EXT4
        read_u4();
        push(Obj::make(Obj::kOpaque));
        break;

      case 0x52: { // REDUCE
        ObjPtr args = pop();
        ObjPtr callable = pop();
        push(do_reduce(callable, args));
        break;
      }
      case 0x62: { // BUILD
        ObjPtr arg = pop();
        ObjPtr obj = top();
        if (obj->kind == Obj::kDict && arg->kind == Obj::kDict) {
          for (auto &p : arg->kv) obj->kv.push_back(p);
        } else if (obj->kind == Obj::kOpaque && arg->kind == Obj::kDict) {
          // Module-style __setstate__ with a state dict: expose contents so
          // tensors inside remain discoverable (graceful module skip).
          obj->kind = Obj::kDict;
          obj->kv = arg->kv;
        }
        break;
      }
      case 0x69: { // INST
        read_line(); // module
        read_line(); // class
        pop_mark();
        push(Obj::make(Obj::kOpaque));
        break;
      }
      case 0x6f: { // OBJ
        auto slice = pop_mark_with_obj();
        (void)slice;
        push(Obj::make(Obj::kOpaque));
        break;
      }
      case 0x81: { // NEWOBJ
        pop(); // args
        pop(); // cls
        push(Obj::make(Obj::kOpaque));
        break;
      }
      case 0x92: { // NEWOBJ_EX
        pop(); // kwargs
        pop(); // args
        pop(); // cls
        push(Obj::make(Obj::kOpaque));
        break;
      }

      case 0x50: { // PERSID (string pid; not used for storages)
        read_line();
        push(Obj::make(Obj::kOpaque));
        break;
      }
      case 0x51: { // BINPERSID
        ObjPtr pid = pop();
        push(do_persid(pid));
        break;
      }

      default:
        throw Fail("pickle: unsupported opcode 0x" + hex2(op) + " at offset " +
                   std::to_string(pos_ - 1));
      }
    }
    throw Fail("pickle: stream ended without STOP");
  }

private:
  const uint8_t *d_;
  size_t n_;
  size_t pos_ = 0;
  std::vector<ObjPtr> stack_;
  std::vector<ObjPtr> memo_;

  static std::string hex2(uint8_t b) {
    const char *h = "0123456789abcdef";
    return {h[b >> 4], h[b & 15]};
  }

  void need(size_t k) const {
    if (pos_ + k > n_) throw Fail("pickle: unexpected end of stream");
  }
  uint8_t read_u1() {
    need(1);
    return d_[pos_++];
  }
  uint16_t read_u2() {
    need(2);
    uint16_t v = le16(d_ + pos_);
    pos_ += 2;
    return v;
  }
  uint32_t read_u4() {
    need(4);
    uint32_t v = le32(d_ + pos_);
    pos_ += 4;
    return v;
  }
  uint64_t read_u8() {
    need(8);
    uint64_t v = le64(d_ + pos_);
    pos_ += 8;
    return v;
  }
  std::string read_line() {
    size_t start = pos_;
    while (pos_ < n_ && d_[pos_] != '\n') ++pos_;
    if (pos_ >= n_) throw Fail("pickle: unterminated line argument");
    std::string s(reinterpret_cast<const char *>(d_ + start), pos_ - start);
    ++pos_; // consume '\n'
    return s;
  }
  std::string read_bytes(int64_t k) {
    if (k < 0) throw Fail("pickle: negative string length");
    need(static_cast<size_t>(k));
    std::string s(reinterpret_cast<const char *>(d_ + pos_), static_cast<size_t>(k));
    pos_ += static_cast<size_t>(k);
    return s;
  }
  std::string read_bytes_u(uint64_t k) {
    if (k > n_) throw Fail("pickle: string length too large");
    return read_bytes(static_cast<int64_t>(k));
  }
  int64_t read_long_n(uint64_t k) {
    // Two's-complement little-endian; accept up to 8 bytes.
    if (k == 0) return 0;
    if (k > 8) {
      // Bignum beyond int64: skip bytes, saturate (not expected in fixtures).
      need(static_cast<size_t>(k));
      bool neg = (d_[pos_ + k - 1] & 0x80) != 0;
      pos_ += k;
      return neg ? INT64_MIN : INT64_MAX;
    }
    need(static_cast<size_t>(k));
    uint64_t v = 0;
    for (uint64_t i = 0; i < k; ++i)
      v |= static_cast<uint64_t>(d_[pos_ + i]) << (8 * i);
    bool neg = (d_[pos_ + k - 1] & 0x80) != 0;
    pos_ += k;
    if (neg) v |= ~uint64_t(0) << (8 * k);
    return static_cast<int64_t>(v);
  }
  int64_t parse_dec(std::string s) {
    while (!s.empty() && (s.back() == 'L' || std::isspace(
                              static_cast<unsigned char>(s.back()))))
      s.pop_back();
    if (s.empty()) throw Fail("pickle: empty decimal literal");
    return std::stoll(s);
  }
  std::string parse_string_repr(const std::string &s) {
    // Minimal repr parsing: strip quotes, handle common escapes.
    if (s.size() < 2) return s;
    char q = s.front();
    if ((q != '\'' && q != '"') || s.back() != q) return s;
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
      char c = s[i];
      if (c == '\\' && i + 2 <= s.size()) {
        char e = s[++i];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '\\': out += '\\'; break;
        case '\'': out += '\''; break;
        case '"': out += '"'; break;
        case 'x': {
          if (i + 2 < s.size()) {
            auto hv = [&](char h) -> int {
              if (h >= '0' && h <= '9') return h - '0';
              if (h >= 'a' && h <= 'f') return h - 'a' + 10;
              if (h >= 'A' && h <= 'F') return h - 'A' + 10;
              return 0;
            };
            out += static_cast<char>(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
          }
          break;
        }
        default: out += e; break;
        }
      } else {
        out += c;
      }
    }
    return out;
  }

  void push(const ObjPtr &o) { stack_.push_back(o); }
  void push_int(int64_t v) { stack_.push_back(Obj::make_int(v)); }
  void push_bool(bool b) {
    auto o = Obj::make(Obj::kBool);
    o->boolean = b;
    stack_.push_back(o);
  }
  void push_tuple(std::vector<ObjPtr> items) {
    auto o = Obj::make(Obj::kTuple);
    o->items = std::move(items);
    stack_.push_back(o);
  }
  ObjPtr top() const {
    if (stack_.empty()) throw Fail("pickle: stack underflow");
    return stack_.back();
  }
  ObjPtr pop() {
    ObjPtr o = top();
    stack_.pop_back();
    return o;
  }
  // Pop everything at and above the most recent MARK; return items above it.
  std::vector<ObjPtr> pop_mark() {
    for (size_t i = stack_.size(); i-- > 0;) {
      if (stack_[i]->kind == Obj::kMark) {
        std::vector<ObjPtr> slice(stack_.begin() + static_cast<long>(i) + 1,
                                  stack_.end());
        stack_.resize(i);
        return slice;
      }
    }
    throw Fail("pickle: MARK not found");
  }
  // OBJ: mark, then class object, then args; drop all of it.
  std::vector<ObjPtr> pop_mark_with_obj() { return pop_mark(); }

  ObjPtr memo_get(int64_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= memo_.size())
      throw Fail("pickle: memo index out of range");
    return memo_[static_cast<size_t>(idx)];
  }
  void memo_put(int64_t idx) {
    if (idx < 0) throw Fail("pickle: negative memo index");
    if (static_cast<size_t>(idx) >= memo_.size())
      memo_.resize(static_cast<size_t>(idx) + 1);
    memo_[static_cast<size_t>(idx)] = top();
  }

  static void ensure_dict(ObjPtr &o) {
    if (o->kind == Obj::kDict) return;
    if (o->kind == Obj::kOpaque) { // graceful: expose as dict
      o->kind = Obj::kDict;
      o->kv.clear();
      return;
    }
    throw Fail("pickle: SETITEM target not a dict");
  }
  static void dict_fill(ObjPtr &d, const std::vector<ObjPtr> &slice) {
    if (slice.size() % 2 != 0)
      throw Fail("pickle: odd number of items for dict");
    for (size_t i = 0; i + 1 < slice.size(); i += 2)
      d->kv.emplace_back(slice[i], slice[i + 1]);
  }

  // persistent-id: delta layout is a 5-tuple of plain values:
  //   ('storage', <key str>, <storage_class str>, <location str>, <numel>)
  // Stock layout (tolerated): ('storage', <class GLOBAL>, <key>, <loc>, <n>)
  ObjPtr do_persid(const ObjPtr &pid) {
    if (pid->kind != Obj::kTuple || pid->items.size() != 5)
      return Obj::make(Obj::kOpaque);
    auto &it = pid->items;
    if (it[0]->kind != Obj::kStr || it[0]->str != "storage")
      return Obj::make(Obj::kOpaque);

    std::string key, cls_name;
    ObjPtr numel_obj;
    if (it[1]->kind == Obj::kStr) { // delta order
      key = it[1]->str;
      if (it[2]->kind == Obj::kStr) cls_name = it[2]->str;
      else if (it[2]->kind == Obj::kGlobal) cls_name = it[2]->name;
    } else { // stock order
      if (it[1]->kind == Obj::kGlobal) cls_name = it[1]->name;
      else if (it[1]->kind == Obj::kStr) cls_name = it[1]->str;
      if (it[2]->kind == Obj::kStr) key = it[2]->str;
    }
    numel_obj = it[4];
    if (cls_name.empty() || numel_obj->kind != Obj::kInt)
      return Obj::make(Obj::kOpaque);

    auto o = Obj::make(Obj::kStorage);
    o->storage_key = key;
    if (!storage_class_to_dtype(cls_name, o->dtype, o->itemsize))
      return Obj::make(Obj::kOpaque); // unknown storage class: skip gracefully
    o->numel = numel_obj->integer;
    return o;
  }

  ObjPtr do_reduce(const ObjPtr &callable, const ObjPtr &args) {
    std::string mod, name;
    if (callable->kind == Obj::kGlobal) {
      mod = callable->mod;
      name = callable->name;
    }
    std::vector<ObjPtr> a;
    if (args->kind == Obj::kTuple) a = args->items;

    if (name == "_rebuild_tensor_v2" || name == "_rebuild_tensor") {
      // (storage, storage_offset, size, stride, requires_grad, backward_hooks)
      if (a.size() >= 4 && a[0]->kind == Obj::kStorage) {
        auto t = Obj::make(Obj::kTensor);
        t->storage = a[0];
        t->storage_offset = as_int(a[1]);
        t->shape = as_int_list(a[2]);
        t->stride = as_int_list(a[3]);
        if (t->shape.size() != t->stride.size())
          throw Fail("pickle: tensor shape/stride rank mismatch");
        return t;
      }
      return Obj::make(Obj::kOpaque);
    }
    if (name == "_rebuild_parameter") {
      // (tensor, requires_grad, backward_hooks) -> the tensor itself
      if (!a.empty() && a[0]->kind == Obj::kTensor) return a[0];
      return Obj::make(Obj::kOpaque);
    }
    if (mod == "collections" && name == "OrderedDict") {
      // OrderedDict() or OrderedDict([(k, v), ...]) — populate from pairs.
      auto d = Obj::make(Obj::kDict);
      if (!a.empty() && (a[0]->kind == Obj::kList || a[0]->kind == Obj::kTuple)) {
        for (auto &pr : a[0]->items)
          if ((pr->kind == Obj::kTuple || pr->kind == Obj::kList) &&
              pr->items.size() == 2)
            d->kv.emplace_back(pr->items[0], pr->items[1]);
      }
      return d;
    }
    // Unknown callable: opaque object (graceful skip).
    return Obj::make(Obj::kOpaque);
  }

  static int64_t as_int(const ObjPtr &o) {
    if (o->kind == Obj::kInt) return o->integer;
    if (o->kind == Obj::kBool) return o->boolean ? 1 : 0;
    throw Fail("pickle: expected int");
  }
  static std::vector<int64_t> as_int_list(const ObjPtr &o) {
    std::vector<int64_t> out;
    if (o->kind == Obj::kTuple || o->kind == Obj::kList) {
      for (auto &v : o->items) out.push_back(as_int(v));
    } else if (o->kind == Obj::kInt) {
      out.push_back(o->integer);
    } else {
      throw Fail("pickle: expected int tuple");
    }
    return out;
  }
};

// ---------------------------------------------------------------------------
// Object-graph walker: collect tensors in path order.
// ---------------------------------------------------------------------------

struct TensorRef {
  std::string path;
  ObjPtr tensor;
};

static void walk(const ObjPtr &o, const std::string &path,
                 std::vector<TensorRef> &out,
                 std::unordered_set<const Obj *> &active, int depth) {
  if (depth > 10000) throw Fail("object graph too deep");
  switch (o->kind) {
  case Obj::kTensor:
    out.push_back({path, o});
    return;
  case Obj::kDict: {
    if (!active.insert(o.get()).second) return; // cycle
    for (auto &p : o->kv) {
      std::string key;
      const ObjPtr &k = p.first;
      if (k->kind == Obj::kStr || k->kind == Obj::kBytes) key = k->str;
      else if (k->kind == Obj::kInt) key = std::to_string(k->integer);
      else if (k->kind == Obj::kBool) key = k->boolean ? "True" : "False";
      else continue; // unrenderable key: skip subtree key
      walk(p.second, path.empty() ? key : path + "/" + key, out, active,
           depth + 1);
    }
    active.erase(o.get());
    return;
  }
  case Obj::kList:
  case Obj::kTuple: {
    if (!active.insert(o.get()).second) return;
    for (size_t i = 0; i < o->items.size(); ++i) {
      std::string child = path.empty() ? std::to_string(i)
                                       : path + "/" + std::to_string(i);
      walk(o->items[i], child, out, active, depth + 1);
    }
    active.erase(o.get());
    return;
  }
  default:
    return; // scalars, storages, globals, opaque objects: not tensors
  }
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static std::string json_escape(const std::string &s) {
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
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  return out;
}

static std::string join_ints(const std::vector<int64_t> &v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ", ";
    out += std::to_string(v[i]);
  }
  out += "]";
  return out;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <file.pth> <out_dir>\n", argv[0]);
    return 2;
  }
  try {
    ZipArchive zip(read_file(argv[1]));

    // Locate data.pkl; the storage files live under the same prefix.
    const ZipEntry *pkl = nullptr;
    for (auto &e : zip.entries()) {
      const std::string suf = "/data.pkl";
      if (e.name.size() >= suf.size() &&
          e.name.compare(e.name.size() - suf.size(), suf.size(), suf) == 0) {
        if (!pkl || e.name.size() < pkl->name.size()) pkl = &e;
      }
    }
    if (!pkl) {
      for (auto &e : zip.entries())
        if (e.name == "data.pkl") { pkl = &e; break; }
    }
    if (!pkl) throw Fail("archive contains no data.pkl");
    std::string prefix = // includes trailing '/', e.g. "fixture/"
        pkl->name.substr(0, pkl->name.size() - std::strlen("data.pkl"));

    std::vector<uint8_t> pkl_bytes = zip.extract(*pkl);
    PickleVM vm(pkl_bytes);
    ObjPtr root = vm.run();

    std::vector<TensorRef> tensors;
    {
      std::unordered_set<const Obj *> active;
      walk(root, "", tensors, active, 0);
    }
    std::sort(tensors.begin(), tensors.end(),
              [](const TensorRef &a, const TensorRef &b) { return a.path < b.path; });

    // Lazily extract storage blobs keyed by storage key.
    std::unordered_map<std::string, std::vector<uint8_t>> blobs;
    std::unordered_map<std::string, const ZipEntry *> by_name;
    for (auto &e : zip.entries()) by_name[e.name] = &e;

    fs::path out_dir = argv[2];
    fs::path tensors_dir = out_dir / "tensors";
    fs::create_directories(tensors_dir);

    std::string manifest;
    manifest += "{\n \"byteorder\": \"little\",\n \"nulltorch_manifest\": 1,\n \"tensors\": {\n";

    bool first = true;
    for (auto &tr : tensors) {
      const Obj &t = *tr.tensor;
      const Obj &st = *t.storage;

      int64_t numel = 1;
      for (int64_t s : t.shape) {
        if (s < 0) throw Fail("negative dimension in " + tr.path);
        numel *= s;
      }
      uint64_t nbytes = static_cast<uint64_t>(numel) * static_cast<uint64_t>(st.itemsize);

      // Materialize contiguous row-major.
      std::vector<uint8_t> buf;
      if (numel > 0) {
        auto it = blobs.find(st.storage_key);
        if (it == blobs.end()) {
          std::string ename = prefix + "data/" + st.storage_key;
          auto ze = by_name.find(ename);
          if (ze == by_name.end())
            throw Fail("missing storage entry " + ename + " for " + tr.path);
          it = blobs.emplace(st.storage_key, zip.extract(*ze->second)).first;
        }
        const std::vector<uint8_t> &blob = it->second;
        buf.resize(static_cast<size_t>(nbytes));
        int64_t nd = static_cast<int64_t>(t.shape.size());
        size_t dst = 0;
        for (int64_t lin = 0; lin < numel; ++lin) {
          int64_t rem = lin;
          int64_t elem = t.storage_offset;
          for (int64_t dim = nd - 1; dim >= 0; --dim) {
            int64_t sz = t.shape[static_cast<size_t>(dim)];
            int64_t c = sz > 0 ? rem % sz : 0;
            rem = sz > 0 ? rem / sz : 0;
            elem += c * t.stride[static_cast<size_t>(dim)];
          }
          if (elem < 0 ||
              static_cast<uint64_t>(elem) * st.itemsize + st.itemsize > blob.size())
            throw Fail("storage access out of range for " + tr.path);
          std::memcpy(buf.data() + dst,
                      blob.data() + static_cast<size_t>(elem) * st.itemsize,
                      static_cast<size_t>(st.itemsize));
          dst += static_cast<size_t>(st.itemsize);
        }
      }

      // Write tensor .bin (path with '/' -> "__").
      std::string fname;
      for (char c : tr.path) {
        if (c == '/') fname += "__";
        else fname += c;
      }
      fname += ".bin";
      {
        std::ofstream bf(tensors_dir / fname, std::ios::binary);
        if (!bf) throw Fail("cannot write " + (tensors_dir / fname).string());
        if (!buf.empty())
          bf.write(reinterpret_cast<const char *>(buf.data()),
                   static_cast<std::streamsize>(buf.size()));
        if (!bf) throw Fail("write failed for " + fname);
      }

      if (!first) manifest += ",\n";
      first = false;
      manifest += "  \"" + json_escape(tr.path) + "\": {\n";
      manifest += "   \"dtype\": \"" + st.dtype + "\",\n";
      manifest += "   \"nbytes\": " + std::to_string(nbytes) + ",\n";
      manifest += "   \"shape\": " + join_ints(t.shape) + ",\n";
      manifest += "   \"storage_key\": \"" + json_escape(st.storage_key) + "\",\n";
      manifest += "   \"storage_offset\": " + std::to_string(t.storage_offset) + ",\n";
      manifest += "   \"stride\": " + join_ints(t.stride) + "\n";
      manifest += "  }";
    }
    manifest += "\n }\n}\n";

    {
      std::ofstream mf(out_dir / "manifest.json");
      if (!mf) throw Fail("cannot write manifest.json");
      mf << manifest;
      if (!mf) throw Fail("write failed for manifest.json");
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "convert: error: %s\n", e.what());
    return 1;
  }
}
