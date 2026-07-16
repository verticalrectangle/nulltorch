#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

static uint16_t le16(const Bytes &b, size_t p) {
  if (p + 2 > b.size())
    throw Error("truncated uint16");
  return uint16_t(b[p]) | (uint16_t(b[p + 1]) << 8);
}
static uint32_t le32(const Bytes &b, size_t p) {
  if (p + 4 > b.size())
    throw Error("truncated uint32");
  return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) |
         (uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24);
}
static uint64_t le64(const Bytes &b, size_t p) {
  if (p + 8 > b.size())
    throw Error("truncated uint64");
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= uint64_t(b[p + i]) << (8 * i);
  return v;
}
static void checked_range(uint64_t p, uint64_t n, uint64_t size,
                          const char *what) {
  if (p > size || n > size - p)
    throw Error(std::string("truncated ") + what);
}
static Bytes read_file(const fs::path &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw Error("cannot open input: " + path.string());
  f.seekg(0, std::ios::end);
  auto end = f.tellg();
  if (end < 0)
    throw Error("cannot determine input size");
  if (uint64_t(end) > size_t(-1))
    throw Error("input too large");
  Bytes out(static_cast<size_t>(end));
  f.seekg(0);
  if (!out.empty() && !f.read(reinterpret_cast<char *>(out.data()),
                              std::streamsize(out.size())))
    throw Error("cannot read input");
  return out;
}

struct ZipEntry {
  uint16_t method = 0;
  uint64_t compressed_size = 0;
  uint64_t size = 0;
  uint64_t local_offset = 0;
};

class DeltaZip {
  const Bytes &file_;
  std::map<std::string, ZipEntry> entries_;

  static void zip64_extra(const Bytes &b, size_t p, size_t n, bool need_size,
                          bool need_comp, bool need_off, uint64_t &size,
                          uint64_t &comp, uint64_t &off) {
    size_t end = p + n;
    checked_range(p, n, b.size(), "ZIP extra field");
    while (p + 4 <= end) {
      uint16_t id = le16(b, p), len = le16(b, p + 2);
      p += 4;
      if (len > end - p)
        throw Error("malformed ZIP extra field");
      if (id == 1) {
        size_t q = p;
        if (need_size) {
          checked_range(q, 8, p + len, "zip64 size");
          size = le64(b, q);
          q += 8;
        }
        if (need_comp) {
          checked_range(q, 8, p + len, "zip64 compressed size");
          comp = le64(b, q);
          q += 8;
        }
        if (need_off) {
          checked_range(q, 8, p + len, "zip64 offset");
          off = le64(b, q);
        }
        return;
      }
      p += len;
    }
    if (need_size || need_comp || need_off)
      throw Error("missing zip64 extra field");
  }

public:
  explicit DeltaZip(const Bytes &file) : file_(file) {
    constexpr uint32_t EOCD = 0x06055a44; // DZ\x05\x06, little-endian
    constexpr uint32_t CENTRAL = 0x02015a44;
    constexpr uint32_t ZIP64_LOC = 0x07065a44;
    constexpr uint32_t ZIP64_END = 0x06065a44;
    if (file.size() < 22)
      throw Error("not a delta ZIP archive");
    size_t minp = file.size() > 65557 ? file.size() - 65557 : 0;
    size_t eocd = size_t(-1);
    for (size_t p = file.size() - 22;; --p) {
      if (le32(file, p) == EOCD &&
          p + 22ull + le16(file, p + 20) == file.size()) {
        eocd = p;
        break;
      }
      if (p == minp)
        break;
    }
    if (eocd == size_t(-1))
      throw Error("delta ZIP end record not found");
    if (le16(file, eocd + 4) != 0 || le16(file, eocd + 6) != 0)
      throw Error("multi-disk ZIP is unsupported");
    uint64_t count = le16(file, eocd + 10);
    uint64_t cd_size = le32(file, eocd + 12);
    uint64_t cd_offset = le32(file, eocd + 16);
    if (count == 0xffff || cd_size == 0xffffffffu || cd_offset == 0xffffffffu) {
      if (eocd < 20 || le32(file, eocd - 20) != ZIP64_LOC)
        throw Error("missing delta zip64 locator");
      uint64_t zoff = le64(file, eocd - 12);
      checked_range(zoff, 56, file.size(), "zip64 end record");
      if (le32(file, size_t(zoff)) != ZIP64_END)
        throw Error("bad delta zip64 end signature");
      if (le32(file, size_t(zoff) + 16) != 0 ||
          le32(file, size_t(zoff) + 20) != 0)
        throw Error("multi-disk zip64 is unsupported");
      count = le64(file, size_t(zoff) + 32);
      cd_size = le64(file, size_t(zoff) + 40);
      cd_offset = le64(file, size_t(zoff) + 48);
    }
    checked_range(cd_offset, cd_size, file.size(), "central directory");
    size_t p = size_t(cd_offset);
    for (uint64_t i = 0; i < count; ++i) {
      checked_range(p, 46, file.size(), "central directory header");
      if (le32(file, p) != CENTRAL)
        throw Error("bad delta central directory signature");
      uint16_t flags = le16(file, p + 8), method = le16(file, p + 10);
      if (flags & ((1u << 0) | (1u << 13)))
        throw Error("encrypted ZIP entry is unsupported");
      uint64_t comp = le32(file, p + 20), size = le32(file, p + 24),
               off = le32(file, p + 42);
      uint16_t name_n = le16(file, p + 28), extra_n = le16(file, p + 30),
               comment_n = le16(file, p + 32);
      checked_range(p + 46ull, uint64_t(name_n) + extra_n + comment_n,
                    file.size(), "central directory entry");
      std::string name(reinterpret_cast<const char *>(file.data() + p + 46),
                       name_n);
      bool ns = size == 0xffffffffu, nc = comp == 0xffffffffu,
           no = off == 0xffffffffu;
      if (ns || nc || no)
        zip64_extra(file, p + 46 + name_n, extra_n, ns, nc, no, size, comp,
                    off);
      entries_[name] = ZipEntry{method, comp, size, off};
      p += 46ull + name_n + extra_n + comment_n;
    }
    if (uint64_t(p) > cd_offset + cd_size)
      throw Error("central directory exceeds declared size");
  }

  const std::map<std::string, ZipEntry> &entries() const { return entries_; }

  Bytes extract(const std::string &name) const {
    auto it = entries_.find(name);
    if (it == entries_.end())
      throw Error("missing ZIP entry: " + name);
    const ZipEntry &e = it->second;
    if (e.method != 0)
      throw Error("compressed ZIP entry is unsupported: " + name);
    if (e.compressed_size != e.size)
      throw Error("invalid STORED entry size: " + name);
    checked_range(e.local_offset, 30, file_.size(), "local file header");
    size_t p = size_t(e.local_offset);
    if (le32(file_, p) != 0x04035a44)
      throw Error("bad delta local file signature: " + name);
    uint16_t name_n = le16(file_, p + 26), extra_n = le16(file_, p + 28);
    uint64_t data = e.local_offset + 30ull + name_n + extra_n;
    checked_range(data, e.size, file_.size(), "ZIP entry data");
    return Bytes(file_.begin() + size_t(data),
                 file_.begin() + size_t(data + e.size));
  }
};

enum class Kind {
  Null,
  Bool,
  Int,
  String,
  List,
  Tuple,
  Dict,
  Global,
  Storage,
  Tensor,
  Opaque,
  Mark
};
struct Obj;
using Val = std::shared_ptr<Obj>;
struct TensorInfo {
  Val storage;
  int64_t offset = 0;
  std::vector<int64_t> shape, stride;
};
struct Obj {
  Kind kind = Kind::Null;
  int64_t integer = 0;
  std::string text;
  std::vector<Val> seq;
  std::vector<std::pair<Val, Val>> dict;
  TensorInfo tensor;
  int64_t storage_numel = 0;
};
static Val obj(Kind k) {
  auto v = std::make_shared<Obj>();
  v->kind = k;
  return v;
}
static Val integer(int64_t n) {
  auto v = obj(Kind::Int);
  v->integer = n;
  return v;
}
static Val stringv(std::string s) {
  auto v = obj(Kind::String);
  v->text = std::move(s);
  return v;
}
static bool is_seq(const Val &v) {
  return v && (v->kind == Kind::List || v->kind == Kind::Tuple);
}

class Pickle {
  const Bytes &b_;
  size_t p_ = 0;
  std::vector<Val> st_;
  std::unordered_map<uint64_t, Val> memo_;
  Val mark_ = obj(Kind::Mark);

  uint8_t u8() {
    if (p_ >= b_.size())
      throw Error("truncated pickle");
    return b_[p_++];
  }
  uint16_t u16() {
    auto v = le16(b_, p_);
    p_ += 2;
    return v;
  }
  uint32_t u32() {
    auto v = le32(b_, p_);
    p_ += 4;
    return v;
  }
  uint64_t u64() {
    auto v = le64(b_, p_);
    p_ += 8;
    return v;
  }
  std::string bytes(size_t n) {
    checked_range(p_, n, b_.size(), "pickle string");
    std::string s(reinterpret_cast<const char *>(b_.data() + p_), n);
    p_ += n;
    return s;
  }
  std::string line() {
    size_t q = p_;
    while (q < b_.size() && b_[q] != '\n')
      ++q;
    if (q == b_.size())
      throw Error("unterminated pickle line");
    std::string s(reinterpret_cast<const char *>(b_.data() + p_), q - p_);
    p_ = q + 1;
    return s;
  }
  Val pop() {
    if (st_.empty())
      throw Error("pickle stack underflow");
    Val v = st_.back();
    st_.pop_back();
    return v;
  }
  size_t mark_pos() const {
    for (size_t i = st_.size(); i > 0; --i)
      if (st_[i - 1] == mark_)
        return i - 1;
    throw Error("pickle mark not found");
  }
  std::vector<Val> pop_mark() {
    size_t m = mark_pos();
    std::vector<Val> out(st_.begin() + m + 1, st_.end());
    st_.resize(m);
    return out;
  }
  static int64_t as_int(const Val &v) {
    if (!v || (v->kind != Kind::Int && v->kind != Kind::Bool))
      throw Error("expected pickle integer");
    return v->integer;
  }
  static std::vector<int64_t> int_seq(const Val &v) {
    if (!is_seq(v))
      throw Error("expected integer tuple");
    std::vector<int64_t> out;
    out.reserve(v->seq.size());
    for (const auto &x : v->seq)
      out.push_back(as_int(x));
    return out;
  }
  Val persistent(Val id) {
    if (!is_seq(id) || id->seq.size() != 5)
      throw Error("unsupported persistent ID");
    const auto &x = id->seq;
    if (!x[0] || x[0]->kind != Kind::String || x[0]->text != "storage" ||
        !x[1] || x[1]->kind != Kind::String || !x[2] ||
        x[2]->kind != Kind::String)
      throw Error("invalid delta storage persistent ID");
    auto v = obj(Kind::Storage);
    v->text = x[1]->text;
    v->seq.push_back(x[2]);
    v->storage_numel = as_int(x[4]);
    return v;
  }
  Val reduce(Val callable, Val args) {
    if (!callable || callable->kind != Kind::Global || !is_seq(args))
      return obj(Kind::Opaque);
    const std::string &name = callable->text;
    if ((name == "collections.OrderedDict" ||
         name == "collections.defaultdict") &&
        args->seq.size() <= 1)
      return obj(Kind::Dict);
    if (name.find("_rebuild_tensor") != std::string::npos) {
      if (args->seq.size() < 4 || !args->seq[0] ||
          args->seq[0]->kind != Kind::Storage)
        throw Error("invalid tensor rebuild arguments");
      auto v = obj(Kind::Tensor);
      v->tensor.storage = args->seq[0];
      v->tensor.offset = as_int(args->seq[1]);
      v->tensor.shape = int_seq(args->seq[2]);
      v->tensor.stride = int_seq(args->seq[3]);
      if (v->tensor.shape.size() != v->tensor.stride.size())
        throw Error("tensor shape/stride rank mismatch");
      return v;
    }
    if (name.find("_rebuild_parameter") != std::string::npos &&
        !args->seq.empty() && args->seq[0]->kind == Kind::Tensor)
      return args->seq[0];
    auto v = obj(Kind::Opaque);
    v->text = name;
    return v;
  }
  static void setitem(const Val &d, Val k, Val v) {
    if (!d || d->kind != Kind::Dict)
      return;
    for (auto &kv : d->dict) {
      if (kv.first->kind == Kind::String && k->kind == Kind::String &&
          kv.first->text == k->text) {
        kv.second = std::move(v);
        return;
      }
    }
    d->dict.emplace_back(std::move(k), std::move(v));
  }
  static int64_t long_bytes(const std::string &s) {
    if (s.empty())
      return 0;
    if (s.size() > 8)
      throw Error("pickle integer exceeds int64");
    uint64_t u = 0;
    for (size_t i = 0; i < s.size(); ++i)
      u |= uint64_t(uint8_t(s[i])) << (8 * i);
    if ((uint8_t(s.back()) & 0x80) && s.size() < 8)
      u |= (~uint64_t(0)) << (8 * s.size());
    return int64_t(u);
  }

public:
  explicit Pickle(const Bytes &b) : b_(b) {}
  Val run() {
    for (;;) {
      uint8_t op = u8();
      switch (op) {
      case 0x80: {
        uint8_t protocol = u8();
        if (protocol > 5)
          throw Error("unsupported pickle protocol");
        break;
      }
      case 0x95:
        u64();
        break;
      case '.': {
        Val v = pop();
        return v;
      }
      case '(':
        st_.push_back(mark_);
        break;
      case '0':
        pop();
        break;
      case '1': {
        size_t m = mark_pos();
        st_.resize(m);
        break;
      }
      case '2':
        if (st_.empty())
          throw Error("pickle stack underflow");
        else
          st_.push_back(st_.back());
        break;
      case 'N':
        st_.push_back(obj(Kind::Null));
        break;
      case 0x88: {
        auto v = obj(Kind::Bool);
        v->integer = 1;
        st_.push_back(v);
        break;
      }
      case 0x89: {
        auto v = obj(Kind::Bool);
        v->integer = 0;
        st_.push_back(v);
        break;
      }
      case 'I': {
        std::string s = line();
        if (s == "00" || s == "False") {
          auto v = obj(Kind::Bool);
          st_.push_back(v);
        } else if (s == "01" || s == "True") {
          auto v = obj(Kind::Bool);
          v->integer = 1;
          st_.push_back(v);
        } else
          st_.push_back(integer(std::stoll(s)));
        break;
      }
      case 'J':
        st_.push_back(integer(int32_t(u32())));
        break;
      case 'K':
        st_.push_back(integer(u8()));
        break;
      case 'M':
        st_.push_back(integer(u16()));
        break;
      case 'L': {
        std::string s = line();
        if (!s.empty() && s.back() == 'L')
          s.pop_back();
        st_.push_back(integer(std::stoll(s)));
        break;
      }
      case 0x8a: {
        size_t n = u8();
        st_.push_back(integer(long_bytes(bytes(n))));
        break;
      }
      case 0x8b: {
        size_t n = u32();
        st_.push_back(integer(long_bytes(bytes(n))));
        break;
      }
      case 'F':
        line();
        st_.push_back(obj(Kind::Opaque));
        break;
      case 'G':
        bytes(8);
        st_.push_back(obj(Kind::Opaque));
        break;
      case 'S': {
        std::string s = line();
        if (s.size() >= 2 && (s.front() == '\'' || s.front() == '\"'))
          s = s.substr(1, s.size() - 2);
        st_.push_back(stringv(s));
        break;
      }
      case 'T':
        st_.push_back(stringv(bytes(u32())));
        break;
      case 'U':
        st_.push_back(stringv(bytes(u8())));
        break;
      case 'V':
        st_.push_back(stringv(line()));
        break;
      case 'X':
        st_.push_back(stringv(bytes(u32())));
        break;
      case 0x8c:
        st_.push_back(stringv(bytes(u8())));
        break;
      case 0x8d: {
        uint64_t n = u64();
        if (n > size_t(-1))
          throw Error("pickle string too large");
        st_.push_back(stringv(bytes(size_t(n))));
        break;
      }
      case 'B':
        st_.push_back(stringv(bytes(u32())));
        break;
      case 'C':
        st_.push_back(stringv(bytes(u8())));
        break;
      case 0x8e: {
        uint64_t n = u64();
        if (n > size_t(-1))
          throw Error("pickle bytes too large");
        st_.push_back(stringv(bytes(size_t(n))));
        break;
      }
      case ']':
        st_.push_back(obj(Kind::List));
        break;
      case 'l': {
        auto x = pop_mark();
        auto v = obj(Kind::List);
        v->seq = std::move(x);
        st_.push_back(v);
        break;
      }
      case 'a': {
        Val x = pop(), l = st_.back();
        if (l->kind == Kind::List)
          l->seq.push_back(x);
        break;
      }
      case 'e': {
        auto x = pop_mark();
        if (st_.empty())
          throw Error("APPENDS without list");
        Val l = st_.back();
        if (l->kind == Kind::List)
          l->seq.insert(l->seq.end(), x.begin(), x.end());
        break;
      }
      case ')':
        st_.push_back(obj(Kind::Tuple));
        break;
      case 't': {
        auto x = pop_mark();
        auto v = obj(Kind::Tuple);
        v->seq = std::move(x);
        st_.push_back(v);
        break;
      }
      case 0x85: {
        Val a = pop();
        auto v = obj(Kind::Tuple);
        v->seq = {a};
        st_.push_back(v);
        break;
      }
      case 0x86: {
        Val b = pop(), a = pop();
        auto v = obj(Kind::Tuple);
        v->seq = {a, b};
        st_.push_back(v);
        break;
      }
      case 0x87: {
        Val c = pop(), b = pop(), a = pop();
        auto v = obj(Kind::Tuple);
        v->seq = {a, b, c};
        st_.push_back(v);
        break;
      }
      case '}':
        st_.push_back(obj(Kind::Dict));
        break;
      case 'd': {
        auto x = pop_mark();
        if (x.size() % 2)
          throw Error("odd DICT item count");
        auto v = obj(Kind::Dict);
        for (size_t i = 0; i < x.size(); i += 2)
          setitem(v, x[i], x[i + 1]);
        st_.push_back(v);
        break;
      }
      case 's': {
        Val v = pop(), k = pop();
        if (st_.empty())
          throw Error("SETITEM without dict");
        setitem(st_.back(), k, v);
        break;
      }
      case 'u': {
        auto x = pop_mark();
        if (x.size() % 2 || st_.empty())
          throw Error("invalid SETITEMS");
        for (size_t i = 0; i < x.size(); i += 2)
          setitem(st_.back(), x[i], x[i + 1]);
        break;
      }
      case 0x8f:
        st_.push_back(obj(Kind::List));
        break;
      case 0x90: {
        auto x = pop_mark();
        if (st_.empty())
          throw Error("ADDITEMS without set");
        st_.back()->seq.insert(st_.back()->seq.end(), x.begin(), x.end());
        break;
      }
      case 0x91: {
        auto x = pop_mark();
        auto v = obj(Kind::Tuple);
        v->seq = std::move(x);
        st_.push_back(v);
        break;
      }
      case 'c': {
        std::string module = line(), name = line();
        auto v = obj(Kind::Global);
        v->text = module + "." + name;
        st_.push_back(v);
        break;
      }
      case 0x93: {
        Val name = pop(), module = pop();
        if (module->kind != Kind::String || name->kind != Kind::String)
          throw Error("invalid STACK_GLOBAL");
        auto v = obj(Kind::Global);
        v->text = module->text + "." + name->text;
        st_.push_back(v);
        break;
      }
      case 'R': {
        Val args = pop(), call = pop();
        st_.push_back(reduce(call, args));
        break;
      }
      case 'b': {
        Val state = pop();
        (void)state;
        if (st_.empty())
          throw Error("BUILD without instance");
        break;
      }
      case 0x81: {
        Val args = pop(), cls = pop();
        (void)args;
        (void)cls;
        st_.push_back(obj(Kind::Opaque));
        break;
      }
      case 0x92: {
        Val kwargs = pop(), args = pop(), cls = pop();
        (void)kwargs;
        (void)args;
        (void)cls;
        st_.push_back(obj(Kind::Opaque));
        break;
      }
      case 'o': {
        auto x = pop_mark();
        (void)x;
        st_.push_back(obj(Kind::Opaque));
        break;
      }
      case 'i': {
        line();
        line();
        auto x = pop_mark();
        (void)x;
        st_.push_back(obj(Kind::Opaque));
        break;
      }
      case 'Q':
        st_.push_back(persistent(pop()));
        break;
      case 'P':
        throw Error("text persistent IDs are unsupported");
      case 'p':
        memo_[std::stoull(line())] = st_.back();
        break;
      case 'q':
        memo_[u8()] = st_.back();
        break;
      case 'r':
        memo_[u32()] = st_.back();
        break;
      case 'g': {
        auto i = std::stoull(line());
        if (!memo_.count(i))
          throw Error("bad pickle memo reference");
        st_.push_back(memo_[i]);
        break;
      }
      case 'h': {
        auto i = u8();
        if (!memo_.count(i))
          throw Error("bad pickle memo reference");
        st_.push_back(memo_[i]);
        break;
      }
      case 'j': {
        auto i = u32();
        if (!memo_.count(i))
          throw Error("bad pickle memo reference");
        st_.push_back(memo_[i]);
        break;
      }
      case 0x94:
        memo_[memo_.size()] = st_.back();
        break;
      default: {
        std::ostringstream s;
        s << "unsupported pickle opcode 0x" << std::hex << unsigned(op);
        throw Error(s.str());
      }
      }
    }
  }
};

struct DType {
  std::string token;
  uint64_t size;
};
static DType dtype_for(const std::string &vault) {
  static const std::map<std::string, DType> m = {
      {"FloatVault", {"f32", 4}},     {"HalfVault", {"f16", 2}},
      {"BFloat16Vault", {"bf16", 2}}, {"DoubleVault", {"f64", 8}},
      {"LongVault", {"i64", 8}},      {"IntVault", {"i32", 4}},
      {"ShortVault", {"i16", 2}},     {"CharVault", {"i8", 1}},
      {"ByteVault", {"u8", 1}},       {"BoolVault", {"bool", 1}}};
  auto it = m.find(vault);
  if (it == m.end())
    throw Error("unsupported storage class: " + vault);
  return it->second;
}
struct NamedTensor {
  std::string path;
  Val value;
  DType dtype;
  uint64_t nbytes = 0;
};

static void collect(const Val &v, const std::string &path,
                    std::vector<NamedTensor> &out,
                    std::set<const Obj *> &ancestors) {
  if (!v)
    return;
  if (v->kind == Kind::Tensor) {
    if (path.empty())
      throw Error("root tensor has no output path");
    Val s = v->tensor.storage;
    if (!s || s->kind != Kind::Storage || s->seq.empty() ||
        s->seq[0]->kind != Kind::String)
      throw Error("tensor has invalid storage");
    DType dt = dtype_for(s->seq[0]->text);
    uint64_t elements = 1;
    for (int64_t d : v->tensor.shape) {
      if (d < 0)
        throw Error("negative tensor dimension");
      if (d == 0) {
        elements = 0;
        continue;
      }
      if (elements > std::numeric_limits<uint64_t>::max() / uint64_t(d))
        throw Error("tensor size overflow");
      elements *= uint64_t(d);
    }
    if (elements > std::numeric_limits<uint64_t>::max() / dt.size)
      throw Error("tensor byte size overflow");
    out.push_back({path, v, dt, elements * dt.size});
    return;
  }
  if (v->kind != Kind::Dict && v->kind != Kind::List && v->kind != Kind::Tuple)
    return;
  if (!ancestors.insert(v.get()).second)
    return;
  if (v->kind == Kind::Dict) {
    for (const auto &kv : v->dict) {
      if (!kv.first || kv.first->kind != Kind::String)
        continue;
      std::string p =
          path.empty() ? kv.first->text : path + "/" + kv.first->text;
      collect(kv.second, p, out, ancestors);
    }
  } else {
    for (size_t i = 0; i < v->seq.size(); ++i) {
      std::string p =
          path.empty() ? std::to_string(i) : path + "/" + std::to_string(i);
      collect(v->seq[i], p, out, ancestors);
    }
  }
  ancestors.erase(v.get());
}
static std::string json_string(const std::string &s) {
  std::ostringstream o;
  o << '\"';
  for (unsigned char c : s) {
    switch (c) {
    case '\"':
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
        const char *h = "0123456789abcdef";
        o << "\\u00" << h[c >> 4] << h[c & 15];
      } else
        o << char(c);
    }
  }
  o << '\"';
  return o.str();
}
static std::string json_ints(const std::vector<int64_t> &x) {
  std::ostringstream o;
  o << '[';
  for (size_t i = 0; i < x.size(); ++i) {
    if (i)
      o << ',';
    o << x[i];
  }
  o << ']';
  return o.str();
}
static void write_bytes(const fs::path &p, const Bytes &b) {
  std::ofstream f(p, std::ios::binary);
  if (!f)
    throw Error("cannot create output: " + p.string());
  if (!b.empty())
    f.write(reinterpret_cast<const char *>(b.data()),
            std::streamsize(b.size()));
  if (!f)
    throw Error("cannot write output: " + p.string());
}
static std::string output_name(std::string path) {
  size_t p = 0;
  while ((p = path.find('/', p)) != std::string::npos) {
    path.replace(p, 1, "__");
    p += 2;
  }
  return path + ".bin";
}

int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw Error("usage: convert <file.pth> <out_dir>");
    Bytes input = read_file(argv[1]);
    DeltaZip zip(input);
    std::string pickle_name;
    for (const auto &[name, e] : zip.entries())
      if (name.size() >= 8 && name.substr(name.size() - 8) == "data.pkl") {
        if (!pickle_name.empty())
          throw Error("multiple data.pkl entries");
        pickle_name = name;
      }
    if (pickle_name.empty())
      throw Error("data.pkl not found");
    std::string prefix = pickle_name.substr(0, pickle_name.size() - 8);
    Val root = Pickle(zip.extract(pickle_name)).run();
    std::vector<NamedTensor> tensors;
    std::set<const Obj *> ancestors;
    collect(root, "", tensors, ancestors);
    fs::path out = argv[2];
    fs::create_directories(out / "tensors");
    std::ostringstream manifest;
    manifest << "{\n  \"nulltorch_manifest\": 1,\n  \"byteorder\": "
                "\"little\",\n  \"tensors\": {";
    for (size_t ti = 0; ti < tensors.size(); ++ti) {
      auto &nt = tensors[ti];
      Val t = nt.value, s = t->tensor.storage;
      std::string key = s->text;
      Bytes storage = zip.extract(prefix + "data/" + key);
      uint64_t count = nt.nbytes / nt.dtype.size;
      Bytes materialized;
      materialized.resize(size_t(nt.nbytes));
      if (t->tensor.offset < 0)
        throw Error("negative storage offset");
      for (uint64_t linear = 0; linear < count; ++linear) {
        uint64_t rem = linear;
        __int128 elem = t->tensor.offset;
        for (size_t r = t->tensor.shape.size(); r > 0; --r) {
          uint64_t dim = uint64_t(t->tensor.shape[r - 1]);
          uint64_t coord = dim ? rem % dim : 0;
          if (dim)
            rem /= dim;
          elem += __int128(coord) * t->tensor.stride[r - 1];
        }
        if (elem < 0)
          throw Error("tensor view precedes storage");
        uint64_t e = uint64_t(elem);
        if (e > std::numeric_limits<uint64_t>::max() / nt.dtype.size)
          throw Error("storage offset overflow");
        uint64_t src = e * nt.dtype.size;
        checked_range(src, nt.dtype.size, storage.size(),
                      "tensor storage view");
        std::copy_n(storage.data() + size_t(src), size_t(nt.dtype.size),
                    materialized.data() + size_t(linear * nt.dtype.size));
      }
      write_bytes(out / "tensors" / output_name(nt.path), materialized);
      manifest << (ti ? ",\n" : "\n") << "    " << json_string(nt.path)
               << ": {\n      \"dtype\": " << json_string(nt.dtype.token)
               << ",\n      \"shape\": " << json_ints(t->tensor.shape)
               << ",\n      \"stride\": " << json_ints(t->tensor.stride)
               << ",\n      \"storage_key\": " << json_string(key)
               << ",\n      \"storage_offset\": " << t->tensor.offset
               << ",\n      \"nbytes\": " << nt.nbytes << "\n    }";
    }
    manifest << (tensors.empty() ? "" : "\n") << "  }\n}\n";
    std::string ms = manifest.str();
    write_bytes(out / "manifest.json", Bytes(ms.begin(), ms.end()));
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "convert: " << e.what() << '\n';
    return 1;
  }
}
