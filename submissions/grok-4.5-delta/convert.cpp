// NullTorch PTH-Read converter (delta variant).
// Stdlib only. Invocation: ./convert <file.pth> <out_dir>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// little-endian readers
// ---------------------------------------------------------------------------
static uint16_t rdu16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
static uint32_t rdu32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
static uint64_t rdu64(const uint8_t* p) {
  return uint64_t(rdu32(p)) | (uint64_t(rdu32(p + 4)) << 32);
}
static int32_t rdi32(const uint8_t* p) { return int32_t(rdu32(p)); }

// ---------------------------------------------------------------------------
// dtype
// ---------------------------------------------------------------------------
struct DType {
  std::string token;
  int itemsize = 0;
};

static DType dtype_from_vault(const std::string& name) {
  static const std::unordered_map<std::string, DType> k = {
      {"FloatVault", {"f32", 4}},
      {"FloatStorage", {"f32", 4}},
      {"HalfVault", {"f16", 2}},
      {"HalfStorage", {"f16", 2}},
      {"DoubleVault", {"f64", 8}},
      {"DoubleStorage", {"f64", 8}},
      {"BFloat16Vault", {"bf16", 2}},
      {"BFloat16Storage", {"bf16", 2}},
      {"LongVault", {"i64", 8}},
      {"LongStorage", {"i64", 8}},
      {"IntVault", {"i32", 4}},
      {"IntStorage", {"i32", 4}},
      {"ShortVault", {"i16", 2}},
      {"ShortStorage", {"i16", 2}},
      {"CharVault", {"i8", 1}},
      {"CharStorage", {"i8", 1}},
      {"ByteVault", {"u8", 1}},
      {"ByteStorage", {"u8", 1}},
      {"BoolVault", {"bool", 1}},
      {"BoolStorage", {"bool", 1}},
      {"Float8_e4m3fnVault", {"f8_e4m3", 1}},
      {"Float8_e4m3fnStorage", {"f8_e4m3", 1}},
      {"Float8_e5m2Vault", {"f8_e5m2", 1}},
      {"Float8_e5m2Storage", {"f8_e5m2", 1}},
  };
  auto it = k.find(name);
  if (it == k.end()) throw std::runtime_error("unknown storage class: " + name);
  return it->second;
}

// ---------------------------------------------------------------------------
// ZIP (delta signatures: DZ instead of PK)
// ---------------------------------------------------------------------------
struct ZipEntry {
  std::string name;
  uint32_t method = 0;
  uint64_t comp_size = 0;
  uint64_t uncomp_size = 0;
  uint64_t local_offset = 0;
};

struct ZipArchive {
  std::vector<uint8_t> data;
  std::vector<ZipEntry> entries;
  std::unordered_map<std::string, size_t> by_name;

  static ZipArchive open(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    if (n < 22) throw std::runtime_error("file too small");
    f.seekg(0, std::ios::beg);
    ZipArchive z;
    z.data.resize(size_t(n));
    f.read(reinterpret_cast<char*>(z.data.data()), n);
    z.parse();
    return z;
  }

  void parse() {
    const uint8_t eocd_sig[4] = {'D', 'Z', 0x05, 0x06};
    const size_t max_scan = std::min(data.size(), size_t(22 + 65535));
    size_t eocd = size_t(-1);
    size_t start = data.size() >= max_scan ? data.size() - max_scan : 0;
    for (size_t i = data.size() - 22 + 1; i-- > start;) {
      if (data[i] == eocd_sig[0] && data[i + 1] == eocd_sig[1] &&
          data[i + 2] == eocd_sig[2] && data[i + 3] == eocd_sig[3]) {
        uint16_t comment = rdu16(data.data() + i + 20);
        if (i + 22 + comment == data.size()) {
          eocd = i;
          break;
        }
      }
      if (i == 0) break;
    }
    if (eocd == size_t(-1)) throw std::runtime_error("EOCD not found");

    uint64_t nrec = rdu16(data.data() + eocd + 10);
    uint64_t cd_size = rdu32(data.data() + eocd + 12);
    uint64_t cd_off = rdu32(data.data() + eocd + 16);

    if (nrec == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
      if (eocd < 20) throw std::runtime_error("zip64 locator missing");
      size_t loc = eocd - 20;
      bool ok = (data[loc] == 'D' && data[loc + 1] == 'Z' &&
                 data[loc + 2] == 0x06 && data[loc + 3] == 0x07) ||
                (data[loc] == 'P' && data[loc + 1] == 'K' &&
                 data[loc + 2] == 0x06 && data[loc + 3] == 0x07);
      if (!ok) throw std::runtime_error("zip64 locator bad");
      uint64_t z64_off = rdu64(data.data() + loc + 8);
      if (z64_off + 56 > data.size()) throw std::runtime_error("zip64 eocd oob");
      const uint8_t* z = data.data() + z64_off;
      nrec = rdu64(z + 32);
      cd_size = rdu64(z + 40);
      cd_off = rdu64(z + 48);
      (void)cd_size;
    }

    size_t p = size_t(cd_off);
    entries.reserve(size_t(nrec));
    for (uint64_t i = 0; i < nrec; ++i) {
      if (p + 46 > data.size()) throw std::runtime_error("cd oob");
      if (!(data[p] == 'D' && data[p + 1] == 'Z' && data[p + 2] == 0x01 &&
            data[p + 3] == 0x02))
        throw std::runtime_error("bad central dir sig");
      uint16_t method = rdu16(data.data() + p + 10);
      uint32_t csize32 = rdu32(data.data() + p + 20);
      uint32_t usize32 = rdu32(data.data() + p + 24);
      uint16_t nlen = rdu16(data.data() + p + 28);
      uint16_t xlen = rdu16(data.data() + p + 30);
      uint16_t clen = rdu16(data.data() + p + 32);
      uint32_t loff32 = rdu32(data.data() + p + 42);
      if (p + 46 + nlen + xlen + clen > data.size())
        throw std::runtime_error("cd name oob");
      std::string name(reinterpret_cast<const char*>(data.data() + p + 46),
                       nlen);

      uint64_t csize = csize32, usize = usize32, loff = loff32;
      if (csize32 == 0xFFFFFFFFu || usize32 == 0xFFFFFFFFu ||
          loff32 == 0xFFFFFFFFu) {
        size_t xp = p + 46 + nlen;
        size_t xend = xp + xlen;
        while (xp + 4 <= xend) {
          uint16_t hid = rdu16(data.data() + xp);
          uint16_t hsz = rdu16(data.data() + xp + 2);
          xp += 4;
          if (xp + hsz > xend) break;
          if (hid == 0x0001) {
            size_t q = xp;
            if (usize32 == 0xFFFFFFFFu) {
              usize = rdu64(data.data() + q);
              q += 8;
            }
            if (csize32 == 0xFFFFFFFFu) {
              csize = rdu64(data.data() + q);
              q += 8;
            }
            if (loff32 == 0xFFFFFFFFu) {
              loff = rdu64(data.data() + q);
              q += 8;
            }
          }
          xp += hsz;
        }
      }

      ZipEntry e;
      e.name = std::move(name);
      e.method = method;
      e.comp_size = csize;
      e.uncomp_size = usize;
      e.local_offset = loff;
      by_name[e.name] = entries.size();
      entries.push_back(std::move(e));
      p += 46 + nlen + xlen + clen;
    }
  }

  std::vector<uint8_t> read(const std::string& name) const {
    auto it = by_name.find(name);
    if (it == by_name.end())
      throw std::runtime_error("zip entry missing: " + name);
    const ZipEntry& e = entries[it->second];
    if (e.method != 0)
      throw std::runtime_error("unsupported compression method for " + name);
    size_t lo = size_t(e.local_offset);
    if (lo + 30 > data.size()) throw std::runtime_error("local oob");
    if (!(data[lo] == 'D' && data[lo + 1] == 'Z' && data[lo + 2] == 0x03 &&
          data[lo + 3] == 0x04))
      throw std::runtime_error("bad local sig");
    uint16_t nlen = rdu16(data.data() + lo + 26);
    uint16_t xlen = rdu16(data.data() + lo + 28);
    size_t start = lo + 30 + nlen + xlen;
    if (start + e.uncomp_size > data.size())
      throw std::runtime_error("data oob for " + name);
    return std::vector<uint8_t>(data.begin() + start,
                                data.begin() + start + e.uncomp_size);
  }

  std::pair<std::string, std::vector<uint8_t>> find_data_pkl() const {
    for (const auto& e : entries) {
      if (e.name.size() >= 8 &&
          e.name.compare(e.name.size() - 8, 8, "data.pkl") == 0) {
        std::string prefix = e.name.substr(0, e.name.size() - 8);
        return {prefix, read(e.name)};
      }
    }
    throw std::runtime_error("data.pkl not found");
  }

  std::vector<uint8_t> read_storage(const std::string& prefix,
                                    const std::string& key) const {
    return read(prefix + "data/" + key);
  }
};

// ---------------------------------------------------------------------------
// Pickle value model
// ---------------------------------------------------------------------------
struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct StorageRef {
  std::string key;
  std::string storage_class;
  std::string location;
  int64_t numel = 0;
  DType dtype;
};

struct TensorObj {
  StorageRef storage;
  int64_t storage_offset = 0;
  std::vector<int64_t> size;
  std::vector<int64_t> stride;
  bool requires_grad = false;
};

enum class VKind {
  None,
  Bool,
  Int,
  Float,
  String,
  Bytes,
  List,
  Dict,
  Tuple,
  Set,
  Global,
  Mark,
  Storage,
  Tensor,
};

struct Value {
  VKind kind = VKind::None;
  bool b = false;
  int64_t i = 0;
  double f = 0.0;
  std::string s;
  std::vector<uint8_t> bytes;
  std::vector<ValuePtr> list;
  std::vector<std::pair<ValuePtr, ValuePtr>> dict;
  std::string global_mod, global_name;
  StorageRef storage;
  TensorObj tensor;
  bool is_ordered_dict = false;

  static ValuePtr none() {
    auto v = std::make_shared<Value>();
    v->kind = VKind::None;
    return v;
  }
  static ValuePtr mark() {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Mark;
    return v;
  }
  static ValuePtr boolean(bool x) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Bool;
    v->b = x;
    return v;
  }
  static ValuePtr integer(int64_t x) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Int;
    v->i = x;
    return v;
  }
  static ValuePtr floating(double x) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Float;
    v->f = x;
    return v;
  }
  static ValuePtr string(std::string x) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::String;
    v->s = std::move(x);
    return v;
  }
  static ValuePtr make_bytes(std::vector<uint8_t> x) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Bytes;
    v->bytes = std::move(x);
    return v;
  }
  static ValuePtr make_list() {
    auto v = std::make_shared<Value>();
    v->kind = VKind::List;
    return v;
  }
  static ValuePtr make_dict() {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Dict;
    return v;
  }
  static ValuePtr make_tuple(std::vector<ValuePtr> xs) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Tuple;
    v->list = std::move(xs);
    return v;
  }
  static ValuePtr make_set() {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Set;
    return v;
  }
  static ValuePtr make_global(std::string m, std::string n) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Global;
    v->global_mod = std::move(m);
    v->global_name = std::move(n);
    return v;
  }
  static ValuePtr make_storage(StorageRef sr) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Storage;
    v->storage = std::move(sr);
    return v;
  }
  static ValuePtr make_tensor(TensorObj t) {
    auto v = std::make_shared<Value>();
    v->kind = VKind::Tensor;
    v->tensor = std::move(t);
    return v;
  }

  std::string key_string() const {
    if (kind == VKind::String) return s;
    if (kind == VKind::Int) return std::to_string(i);
    if (kind == VKind::Bool) return b ? "True" : "False";
    throw std::runtime_error("dict key not string/int");
  }
};

// ---------------------------------------------------------------------------
// Unpickler
// ---------------------------------------------------------------------------
class Unpickler {
 public:
  explicit Unpickler(const std::vector<uint8_t>& buf) : buf_(buf) {}

  ValuePtr run() {
    while (pos_ < buf_.size()) {
      uint8_t op = buf_[pos_++];
      switch (op) {
        case 0x80: {  // PROTO
          need(1);
          proto_ = buf_[pos_++];
          break;
        }
        case 0x95: {  // FRAME
          need(8);
          (void)rdu64(buf_.data() + pos_);
          pos_ += 8;
          break;
        }
        case 0x2e: {  // STOP
          if (stack_.empty()) throw std::runtime_error("STOP on empty stack");
          auto r = stack_.back();
          stack_.pop_back();
          return r;
        }
        case 0x4e:  // NONE
          push(Value::none());
          break;
        case 0x88:  // NEWTRUE
          push(Value::boolean(true));
          break;
        case 0x89:  // NEWFALSE
          push(Value::boolean(false));
          break;
        case 0x49: {  // INT
          auto s = read_line();
          if (s == "01")
            push(Value::boolean(true));
          else if (s == "00")
            push(Value::boolean(false));
          else
            push(Value::integer(std::stoll(s)));
          break;
        }
        case 0x4a: {  // BININT
          need(4);
          push(Value::integer(rdi32(buf_.data() + pos_)));
          pos_ += 4;
          break;
        }
        case 0x4b: {  // BININT1
          need(1);
          push(Value::integer(buf_[pos_++]));
          break;
        }
        case 0x4d: {  // BININT2
          need(2);
          push(Value::integer(rdu16(buf_.data() + pos_)));
          pos_ += 2;
          break;
        }
        case 0x4c: {  // LONG
          auto s = read_line();
          if (!s.empty() && (s.back() == 'L' || s.back() == 'l')) s.pop_back();
          push(Value::integer(s.empty() ? 0 : std::stoll(s)));
          break;
        }
        case 0x8a: {  // LONG1
          need(1);
          uint8_t n = buf_[pos_++];
          need(n);
          push(Value::integer(decode_long(buf_.data() + pos_, n)));
          pos_ += n;
          break;
        }
        case 0x8b: {  // LONG4
          need(4);
          uint32_t n = rdu32(buf_.data() + pos_);
          pos_ += 4;
          need(n);
          push(Value::integer(decode_long(buf_.data() + pos_, n)));
          pos_ += n;
          break;
        }
        case 0x47: {  // BINFLOAT (big-endian IEEE754 double)
          need(8);
          uint8_t le[8];
          for (int i = 0; i < 8; ++i) le[i] = buf_[pos_ + 7 - i];
          pos_ += 8;
          double d;
          std::memcpy(&d, le, 8);
          push(Value::floating(d));
          break;
        }
        case 0x46: {  // FLOAT
          auto s = read_line();
          push(Value::floating(std::stod(s)));
          break;
        }
        case 0x53: {  // STRING
          auto s = read_line();
          push(Value::string(decode_stringnl(s)));
          break;
        }
        case 0x54: {  // BINSTRING
          need(4);
          int32_t n = rdi32(buf_.data() + pos_);
          pos_ += 4;
          need(size_t(n));
          push(Value::string(std::string(
              reinterpret_cast<const char*>(buf_.data() + pos_), size_t(n))));
          pos_ += size_t(n);
          break;
        }
        case 0x55: {  // SHORT_BINSTRING
          need(1);
          uint8_t n = buf_[pos_++];
          need(n);
          push(Value::string(
              std::string(reinterpret_cast<const char*>(buf_.data() + pos_), n)));
          pos_ += n;
          break;
        }
        case 0x56: {  // UNICODE
          auto s = read_line();
          push(Value::string(s));
          break;
        }
        case 0x58: {  // BINUNICODE
          need(4);
          uint32_t n = rdu32(buf_.data() + pos_);
          pos_ += 4;
          need(n);
          push(Value::string(
              std::string(reinterpret_cast<const char*>(buf_.data() + pos_), n)));
          pos_ += n;
          break;
        }
        case 0x8c: {  // SHORT_BINUNICODE
          need(1);
          uint8_t n = buf_[pos_++];
          need(n);
          push(Value::string(
              std::string(reinterpret_cast<const char*>(buf_.data() + pos_), n)));
          pos_ += n;
          break;
        }
        case 0x8d: {  // BINUNICODE8
          need(8);
          uint64_t n = rdu64(buf_.data() + pos_);
          pos_ += 8;
          need(size_t(n));
          push(Value::string(std::string(
              reinterpret_cast<const char*>(buf_.data() + pos_), size_t(n))));
          pos_ += size_t(n);
          break;
        }
        case 0x42: {  // BINBYTES
          need(4);
          uint32_t n = rdu32(buf_.data() + pos_);
          pos_ += 4;
          need(n);
          push(Value::make_bytes(std::vector<uint8_t>(
              buf_.begin() + pos_, buf_.begin() + pos_ + n)));
          pos_ += n;
          break;
        }
        case 0x43: {  // SHORT_BINBYTES
          need(1);
          uint8_t n = buf_[pos_++];
          need(n);
          push(Value::make_bytes(std::vector<uint8_t>(
              buf_.begin() + pos_, buf_.begin() + pos_ + n)));
          pos_ += n;
          break;
        }
        case 0x8e: {  // BINBYTES8
          need(8);
          uint64_t n = rdu64(buf_.data() + pos_);
          pos_ += 8;
          need(size_t(n));
          push(Value::make_bytes(std::vector<uint8_t>(
              buf_.begin() + pos_, buf_.begin() + pos_ + size_t(n))));
          pos_ += size_t(n);
          break;
        }
        case 0x96: {  // BYTEARRAY8
          need(8);
          uint64_t n = rdu64(buf_.data() + pos_);
          pos_ += 8;
          need(size_t(n));
          push(Value::make_bytes(std::vector<uint8_t>(
              buf_.begin() + pos_, buf_.begin() + pos_ + size_t(n))));
          pos_ += size_t(n);
          break;
        }
        case 0x5d:  // EMPTY_LIST
          push(Value::make_list());
          break;
        case 0x29:  // EMPTY_TUPLE
          push(Value::make_tuple({}));
          break;
        case 0x7d:  // EMPTY_DICT
          push(Value::make_dict());
          break;
        case 0x8f:  // EMPTY_SET
          push(Value::make_set());
          break;
        case 0x28:  // MARK
          push(Value::mark());
          break;
        case 0x30:  // POP
          if (stack_.empty()) throw std::runtime_error("POP empty");
          stack_.pop_back();
          break;
        case 0x31: {  // POP_MARK
          (void)pop_mark();
          break;
        }
        case 0x32:  // DUP
          if (stack_.empty()) throw std::runtime_error("DUP empty");
          push(stack_.back());
          break;
        case 0x61: {  // APPEND
          auto x = pop();
          auto lst = pop();
          if (lst->kind != VKind::List)
            throw std::runtime_error("APPEND non-list");
          lst->list.push_back(x);
          push(lst);
          break;
        }
        case 0x65: {  // APPENDS
          auto xs = pop_mark();
          auto lst = pop();
          if (lst->kind != VKind::List)
            throw std::runtime_error("APPENDS non-list");
          for (auto& x : xs) lst->list.push_back(x);
          push(lst);
          break;
        }
        case 0x6c: {  // LIST
          auto xs = pop_mark();
          auto v = Value::make_list();
          v->list = std::move(xs);
          push(v);
          break;
        }
        case 0x74: {  // TUPLE
          auto xs = pop_mark();
          push(Value::make_tuple(std::move(xs)));
          break;
        }
        case 0x85: {  // TUPLE1
          auto a = pop();
          push(Value::make_tuple({a}));
          break;
        }
        case 0x86: {  // TUPLE2
          auto b = pop();
          auto a = pop();
          push(Value::make_tuple({a, b}));
          break;
        }
        case 0x87: {  // TUPLE3
          auto c = pop();
          auto b = pop();
          auto a = pop();
          push(Value::make_tuple({a, b, c}));
          break;
        }
        case 0x64: {  // DICT
          auto xs = pop_mark();
          if (xs.size() % 2) throw std::runtime_error("DICT odd");
          auto d = Value::make_dict();
          for (size_t i = 0; i < xs.size(); i += 2)
            d->dict.emplace_back(xs[i], xs[i + 1]);
          push(d);
          break;
        }
        case 0x73: {  // SETITEM
          auto val = pop();
          auto key = pop();
          auto d = pop();
          if (d->kind != VKind::Dict)
            throw std::runtime_error("SETITEM non-dict");
          d->dict.emplace_back(key, val);
          push(d);
          break;
        }
        case 0x75: {  // SETITEMS
          auto xs = pop_mark();
          if (xs.size() % 2) throw std::runtime_error("SETITEMS odd");
          auto d = pop();
          if (d->kind != VKind::Dict)
            throw std::runtime_error("SETITEMS non-dict");
          for (size_t i = 0; i < xs.size(); i += 2)
            d->dict.emplace_back(xs[i], xs[i + 1]);
          push(d);
          break;
        }
        case 0x90: {  // ADDITEMS
          auto xs = pop_mark();
          auto s = pop();
          if (s->kind != VKind::Set)
            throw std::runtime_error("ADDITEMS non-set");
          for (auto& x : xs) s->list.push_back(x);
          push(s);
          break;
        }
        case 0x91: {  // FROZENSET
          auto xs = pop_mark();
          auto s = Value::make_set();
          s->list = std::move(xs);
          push(s);
          break;
        }
        case 0x67: {  // GET
          auto s = read_line();
          size_t idx = size_t(std::stoull(s));
          push(memo_get(idx));
          break;
        }
        case 0x68: {  // BINGET
          need(1);
          push(memo_get(buf_[pos_++]));
          break;
        }
        case 0x6a: {  // LONG_BINGET
          need(4);
          uint32_t idx = rdu32(buf_.data() + pos_);
          pos_ += 4;
          push(memo_get(idx));
          break;
        }
        case 0x70: {  // PUT
          auto s = read_line();
          size_t idx = size_t(std::stoull(s));
          memo_put(idx, stack_.back());
          break;
        }
        case 0x71: {  // BINPUT
          need(1);
          memo_put(buf_[pos_++], stack_.back());
          break;
        }
        case 0x72: {  // LONG_BINPUT
          need(4);
          uint32_t idx = rdu32(buf_.data() + pos_);
          pos_ += 4;
          memo_put(idx, stack_.back());
          break;
        }
        case 0x94: {  // MEMOIZE
          memo_put(memo_.size(), stack_.back());
          break;
        }
        case 0x63: {  // GLOBAL
          auto mod = read_line();
          auto name = read_line();
          push(Value::make_global(mod, name));
          break;
        }
        case 0x93: {  // STACK_GLOBAL
          auto name = pop();
          auto mod = pop();
          if (mod->kind != VKind::String || name->kind != VKind::String)
            throw std::runtime_error("STACK_GLOBAL types");
          push(Value::make_global(mod->s, name->s));
          break;
        }
        case 0x51: {  // BINPERSID
          auto pid = pop();
          push(persistent_load(pid));
          break;
        }
        case 0x50: {  // PERSID
          auto s = read_line();
          push(persistent_load(Value::string(s)));
          break;
        }
        case 0x52: {  // REDUCE
          auto args = pop();
          auto callable = pop();
          push(do_reduce(callable, args));
          break;
        }
        case 0x81: {  // NEWOBJ
          auto args = pop();
          auto cls = pop();
          push(do_newobj(cls, args));
          break;
        }
        case 0x92: {  // NEWOBJ_EX
          auto kwargs = pop();
          auto args = pop();
          auto cls = pop();
          (void)kwargs;
          push(do_newobj(cls, args));
          break;
        }
        case 0x62: {  // BUILD
          auto state = pop();
          auto obj = pop();
          do_build(obj, state);
          push(obj);
          break;
        }
        case 0x6f: {  // OBJ
          auto xs = pop_mark();
          if (xs.empty()) throw std::runtime_error("OBJ empty");
          auto cls = xs.front();
          std::vector<ValuePtr> args(xs.begin() + 1, xs.end());
          push(do_newobj(cls, Value::make_tuple(std::move(args))));
          break;
        }
        case 0x82:  // EXT1
          need(1);
          pos_++;
          push(Value::none());
          break;
        case 0x83:  // EXT2
          need(2);
          pos_ += 2;
          push(Value::none());
          break;
        case 0x84:  // EXT4
          need(4);
          pos_ += 4;
          push(Value::none());
          break;
        default:
          throw std::runtime_error("unsupported pickle opcode 0x" + to_hex(op));
      }
    }
    throw std::runtime_error("pickle ended without STOP");
  }

 private:
  const std::vector<uint8_t>& buf_;
  size_t pos_ = 0;
  int proto_ = 0;
  std::vector<ValuePtr> stack_;
  std::unordered_map<size_t, ValuePtr> memo_;

  void need(size_t n) const {
    if (pos_ + n > buf_.size()) throw std::runtime_error("pickle truncated");
  }
  void push(ValuePtr v) { stack_.push_back(std::move(v)); }
  ValuePtr pop() {
    if (stack_.empty()) throw std::runtime_error("stack underflow");
    auto v = stack_.back();
    stack_.pop_back();
    return v;
  }
  std::vector<ValuePtr> pop_mark() {
    std::vector<ValuePtr> xs;
    while (!stack_.empty()) {
      auto v = stack_.back();
      stack_.pop_back();
      if (v->kind == VKind::Mark) {
        std::reverse(xs.begin(), xs.end());
        return xs;
      }
      xs.push_back(v);
    }
    throw std::runtime_error("mark not found");
  }
  ValuePtr memo_get(size_t idx) {
    auto it = memo_.find(idx);
    if (it == memo_.end()) throw std::runtime_error("bad memo get");
    return it->second;
  }
  void memo_put(size_t idx, ValuePtr v) { memo_[idx] = std::move(v); }

  std::string read_line() {
    std::string s;
    while (pos_ < buf_.size() && buf_[pos_] != '\n') {
      s.push_back(char(buf_[pos_++]));
    }
    if (pos_ >= buf_.size()) throw std::runtime_error("line truncated");
    pos_++;
    return s;
  }

  static int64_t decode_long(const uint8_t* p, size_t n) {
    if (n == 0) return 0;
    // two's complement little-endian; fits in 64 bits for our fixtures
    int64_t v = 0;
    for (size_t i = 0; i < n && i < 8; ++i)
      v |= int64_t(uint64_t(p[i]) << (8 * i));
    if (n < 8 && (p[n - 1] & 0x80)) {
      for (size_t i = n; i < 8; ++i) v |= int64_t(uint64_t(0xff) << (8 * i));
    }
    return v;
  }

  static std::string decode_stringnl(const std::string& s) {
    if (s.size() < 2) return s;
    char q = s.front();
    if ((q != '\'' && q != '"') || s.back() != q) return s;
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size() - 1) {
        char c = s[++i];
        switch (c) {
          case 'n':
            out.push_back('\n');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '\'':
            out.push_back('\'');
            break;
          case '"':
            out.push_back('"');
            break;
          case 'x':
            if (i + 2 < s.size()) {
              auto hex = s.substr(i + 1, 2);
              out.push_back(char(std::stoi(hex, nullptr, 16)));
              i += 2;
            }
            break;
          default:
            out.push_back(c);
            break;
        }
      } else {
        out.push_back(s[i]);
      }
    }
    return out;
  }

  static std::string to_hex(uint8_t b) {
    const char* h = "0123456789abcdef";
    return std::string{h[b >> 4], h[b & 0xf]};
  }

  ValuePtr persistent_load(const ValuePtr& pid) {
    // Delta: ('storage', key_str, storage_class_str, location_str, numel)
    // Stock: ('storage', storage_class GLOBAL, key_str, location_str, numel)
    if (pid->kind != VKind::Tuple || pid->list.size() != 5)
      throw std::runtime_error("persid not 5-tuple");
    const auto& t = pid->list;
    if (t[0]->kind != VKind::String || t[0]->s != "storage")
      throw std::runtime_error("persid not storage");

    StorageRef sr;
    if (t[1]->kind == VKind::String && t[2]->kind == VKind::String) {
      sr.key = t[1]->s;
      sr.storage_class = t[2]->s;
    } else if (t[1]->kind == VKind::Global && t[2]->kind == VKind::String) {
      sr.storage_class = t[1]->global_name;
      sr.key = t[2]->s;
    } else if (t[1]->kind == VKind::String && t[2]->kind == VKind::Global) {
      sr.key = t[1]->s;
      sr.storage_class = t[2]->global_name;
    } else {
      throw std::runtime_error("unrecognized storage persid layout");
    }
    if (t[3]->kind != VKind::String)
      throw std::runtime_error("storage location not str");
    sr.location = t[3]->s;
    if (t[4]->kind != VKind::Int)
      throw std::runtime_error("storage numel not int");
    sr.numel = t[4]->i;
    sr.dtype = dtype_from_vault(sr.storage_class);
    return Value::make_storage(std::move(sr));
  }

  static std::vector<int64_t> as_int_seq(const ValuePtr& v) {
    std::vector<int64_t> out;
    if (v->kind == VKind::Tuple || v->kind == VKind::List) {
      for (auto& x : v->list) {
        if (x->kind != VKind::Int)
          throw std::runtime_error("size/stride elem not int");
        out.push_back(x->i);
      }
      return out;
    }
    throw std::runtime_error("size/stride not sequence");
  }

  ValuePtr do_reduce(const ValuePtr& callable, const ValuePtr& args) {
    if (callable->kind != VKind::Global)
      throw std::runtime_error("REDUCE non-global");
    if (args->kind != VKind::Tuple)
      throw std::runtime_error("REDUCE args not tuple");
    const std::string& mod = callable->global_mod;
    const std::string& name = callable->global_name;

    if ((mod == "torch._utils" || mod == "torch._tensor" ||
         mod == "torch.storage") &&
        (name == "_rebuild_tensor_v2" || name == "_rebuild_tensor")) {
      const auto& a = args->list;
      if (a.size() < 6) throw std::runtime_error("rebuild_tensor argc");
      if (a[0]->kind != VKind::Storage)
        throw std::runtime_error("rebuild storage type");
      TensorObj t;
      t.storage = a[0]->storage;
      if (a[1]->kind != VKind::Int) throw std::runtime_error("offset type");
      t.storage_offset = a[1]->i;
      t.size = as_int_seq(a[2]);
      t.stride = as_int_seq(a[3]);
      if (a[4]->kind == VKind::Bool)
        t.requires_grad = a[4]->b;
      else if (a[4]->kind == VKind::Int)
        t.requires_grad = a[4]->i != 0;
      return Value::make_tensor(std::move(t));
    }

    if (mod == "collections" && name == "OrderedDict") {
      auto d = Value::make_dict();
      d->is_ordered_dict = true;
      if (!args->list.empty()) {
        auto seq = args->list[0];
        if (seq->kind == VKind::List || seq->kind == VKind::Tuple) {
          for (auto& item : seq->list) {
            if (item->kind == VKind::Tuple && item->list.size() == 2)
              d->dict.emplace_back(item->list[0], item->list[1]);
          }
        }
      }
      return d;
    }

    // Unknown callable: return empty dict shell (walk ignores non-tensors).
    return Value::make_dict();
  }

  ValuePtr do_newobj(const ValuePtr& cls, const ValuePtr& args) {
    return do_reduce(cls, args);
  }

  void do_build(ValuePtr& obj, const ValuePtr& state) {
    if (obj->kind != VKind::Dict) return;
    if (state->kind == VKind::Dict) {
      for (auto& kv : state->dict) obj->dict.push_back(kv);
      return;
    }
    if (state->kind == VKind::Tuple && !state->list.empty() &&
        state->list[0]->kind == VKind::Dict) {
      for (auto& kv : state->list[0]->dict) obj->dict.push_back(kv);
    }
  }
};

// ---------------------------------------------------------------------------
// Walk object graph → tensors
// ---------------------------------------------------------------------------
struct TensorEntry {
  std::string path;
  TensorObj tensor;
};

static void walk(const ValuePtr& v, const std::string& path,
                 std::vector<TensorEntry>& out) {
  if (!v) return;
  if (v->kind == VKind::Tensor) {
    out.push_back({path, v->tensor});
    return;
  }
  if (v->kind == VKind::Dict) {
    for (auto& kv : v->dict) {
      std::string key = kv.first->key_string();
      // Skip torch state_dict metadata
      if (key == "_metadata") continue;
      std::string child = path.empty() ? key : (path + "/" + key);
      walk(kv.second, child, out);
    }
    return;
  }
  if (v->kind == VKind::List || v->kind == VKind::Tuple) {
    for (size_t i = 0; i < v->list.size(); ++i) {
      std::string child =
          path.empty() ? std::to_string(i) : (path + "/" + std::to_string(i));
      walk(v->list[i], child, out);
    }
  }
}

// ---------------------------------------------------------------------------
// Materialize contiguous row-major
// ---------------------------------------------------------------------------
static int64_t numel_of(const std::vector<int64_t>& shape) {
  int64_t n = 1;
  for (auto d : shape) {
    if (d < 0) throw std::runtime_error("negative dim");
    if (d == 0) return 0;
    n *= d;
  }
  return n;
}

static std::vector<uint8_t> materialize(const TensorObj& t,
                                        const std::vector<uint8_t>& storage) {
  const int isz = t.storage.dtype.itemsize;
  const int64_t n = numel_of(t.size);
  std::vector<uint8_t> out(size_t(n) * size_t(isz));
  if (n == 0) return out;

  const int rank = int(t.size.size());
  std::vector<int64_t> index(rank, 0);
  for (int64_t linear = 0; linear < n; ++linear) {
    int64_t elem_index = t.storage_offset;
    for (int d = 0; d < rank; ++d) elem_index += index[d] * t.stride[d];
    int64_t byte_off = elem_index * isz;
    if (byte_off < 0 || size_t(byte_off) + size_t(isz) > storage.size())
      throw std::runtime_error("storage OOB key=" + t.storage.key +
                               " off=" + std::to_string(byte_off));
    std::memcpy(out.data() + size_t(linear) * size_t(isz),
                storage.data() + size_t(byte_off), size_t(isz));
    for (int d = rank - 1; d >= 0; --d) {
      if (++index[d] < t.size[d]) break;
      index[d] = 0;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// JSON emit
// ---------------------------------------------------------------------------
static void json_escape(std::ostream& o, const std::string& s) {
  o << '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        o << "\\\"";
        break;
      case '\\':
        o << "\\\\";
        break;
      case '\b':
        o << "\\b";
        break;
      case '\f':
        o << "\\f";
        break;
      case '\n':
        o << "\\n";
        break;
      case '\r':
        o << "\\r";
        break;
      case '\t':
        o << "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o << buf;
        } else {
          o << c;
        }
    }
  }
  o << '"';
}

static std::string write_manifest(const std::vector<TensorEntry>& tensors) {
  std::map<std::string, const TensorEntry*> m;
  for (const auto& t : tensors) m[t.path] = &t;

  std::ostringstream o;
  o << "{\n";
  o << " \"byteorder\": \"little\",\n";
  o << " \"nulltorch_manifest\": 1,\n";
  o << " \"tensors\": {\n";
  size_t i = 0;
  for (auto& kv : m) {
    const TensorEntry& te = *kv.second;
    const TensorObj& t = te.tensor;
    int64_t n = numel_of(t.size);
    int64_t nbytes = n * t.storage.dtype.itemsize;
    o << "  ";
    json_escape(o, te.path);
    o << ": {\n";
    o << "   \"dtype\": ";
    json_escape(o, t.storage.dtype.token);
    o << ",\n";
    o << "   \"nbytes\": " << nbytes << ",\n";
    o << "   \"shape\": [";
    for (size_t d = 0; d < t.size.size(); ++d) {
      if (d) o << ", ";
      o << t.size[d];
    }
    o << "],\n";
    o << "   \"storage_key\": ";
    json_escape(o, t.storage.key);
    o << ",\n";
    o << "   \"storage_offset\": " << t.storage_offset << ",\n";
    o << "   \"stride\": [";
    for (size_t d = 0; d < t.stride.size(); ++d) {
      if (d) o << ", ";
      o << t.stride[d];
    }
    o << "]\n";
    o << "  }";
    if (++i != m.size()) o << ",";
    o << "\n";
  }
  o << " }\n";
  o << "}\n";
  return o.str();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: " << argv[0] << " <file.pth> <out_dir>\n";
      return 2;
    }
    const std::string pth = argv[1];
    const fs::path out_dir = argv[2];

    auto zip = ZipArchive::open(pth);
    auto [prefix, pkl_bytes] = zip.find_data_pkl();

    Unpickler up(pkl_bytes);
    ValuePtr root = up.run();

    std::vector<TensorEntry> tensors;
    walk(root, "", tensors);

    std::unordered_map<std::string, std::vector<uint8_t>> storages;
    auto get_storage =
        [&](const std::string& key) -> const std::vector<uint8_t>& {
      auto it = storages.find(key);
      if (it != storages.end()) return it->second;
      auto blob = zip.read_storage(prefix, key);
      auto [ins, _] = storages.emplace(key, std::move(blob));
      return ins->second;
    };

    fs::create_directories(out_dir / "tensors");

    for (const auto& te : tensors) {
      const auto& blob = get_storage(te.tensor.storage.key);
      auto cont = materialize(te.tensor, blob);
      std::string bin_name;
      for (char c : te.path) {
        if (c == '/')
          bin_name += "__";
        else
          bin_name.push_back(c);
      }
      bin_name += ".bin";
      fs::path bp = out_dir / "tensors" / bin_name;
      std::ofstream bf(bp, std::ios::binary);
      if (!bf) throw std::runtime_error("cannot write " + bp.string());
      if (!cont.empty())
        bf.write(reinterpret_cast<const char*>(cont.data()),
                 std::streamsize(cont.size()));
    }

    {
      auto man = write_manifest(tensors);
      std::ofstream mf(out_dir / "manifest.json");
      if (!mf) throw std::runtime_error("cannot write manifest");
      mf << man;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
