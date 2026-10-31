// NBA JAM: On Fire Edition - a PS3 mod, applied on the console itself
//
// The published mods - johnz1's 1990s, 2020s and Legends editions - ship as
// PlayStation 3 packages. On a PC, tools/dlc.py opens one, converts what
// needs converting and lays it over the game. The Xbox apps can do the same
// themselves, so a mod's .pkg can go straight into the app's LocalState
// beside the game: this is that, the same steps in the same order.
//
//   1. Open the package (tools/ps3_pkg_extract.py): AES-128-CTR with the
//      published PS3 package key, over a table of named items.
//   2. Import it (tools/dlc.py import): data/ps3/... becomes data/xenon/...,
//      the PS3-only files are left behind, and every .ast archive has its
//      textures rewritten from the PS3's container to the 360's XPR2
//      (tools/ast_repack.py). Everything else in those archives - models,
//      meshes, animation - is already byte-identical between the consoles.
//   3. Instance it (tools/dlc.py instance): the mod's files replace the
//      game's, its roster update goes beside the data, and the tree is also
//      reachable as data/ps3, because the mod's own config files ask for it
//      by that name.
//
// The Python is the reference: the comments there say why each rule is what
// it is, and were each found by watching the game fail without it. What
// differs here is only that the front-end menu is left as the disc has it,
// since on the console each mod is an app of its own and there is no mod
// list to put in it.

#include "ps3_mod.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <regex>
#include <string>
#include <tuple>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Bytes

uint32_t BE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t BE64(const uint8_t* p) { return uint64_t(BE32(p)) << 32 | BE32(p + 4); }
uint32_t LE32(const uint8_t* p) {
  return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
void PutBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
uint64_t GetLE(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; --i) v = v << 8 | p[i];
  return v;
}
void PutLE(std::vector<uint8_t>& out, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n < 0) return false;
  in.seekg(0);
  out.resize(size_t(n));
  return n == 0 || bool(in.read(reinterpret_cast<char*>(out.data()), n));
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---------------------------------------------------------------------------
// AES-128, encryption only: CTR mode needs nothing else.

class Aes128 {
 public:
  explicit Aes128(const uint8_t key[16]) {
    std::memcpy(w_.data(), key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {w_[i - 4], w_[i - 3], w_[i - 2], w_[i - 1]};
      if (i % 16 == 0) {
        const uint8_t r = t[0];
        t[0] = uint8_t(Sbox(t[1]) ^ rcon);
        t[1] = Sbox(t[2]);
        t[2] = Sbox(t[3]);
        t[3] = Sbox(r);
        rcon = Xt(rcon);
      }
      for (int j = 0; j < 4; ++j) w_[i + j] = uint8_t(w_[i - 16 + j] ^ t[j]);
    }
  }

  void Encrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = uint8_t(in[i] ^ w_[i]);
    for (int round = 1; round <= 10; ++round) {
      uint8_t t[16];
      // SubBytes and ShiftRows together; s is column major.
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) t[c * 4 + r] = Sbox(s[((c + r) % 4) * 4 + r]);
      }
      if (round != 10) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* col = t + c * 4;
          const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
          const uint8_t x = uint8_t(a0 ^ a1 ^ a2 ^ a3);
          col[0] = uint8_t(a0 ^ x ^ Xt(uint8_t(a0 ^ a1)));
          col[1] = uint8_t(a1 ^ x ^ Xt(uint8_t(a1 ^ a2)));
          col[2] = uint8_t(a2 ^ x ^ Xt(uint8_t(a2 ^ a3)));
          col[3] = uint8_t(a3 ^ x ^ Xt(uint8_t(a3 ^ a0)));
        }
      }
      for (int i = 0; i < 16; ++i) s[i] = uint8_t(t[i] ^ w_[round * 16 + i]);
    }
    std::memcpy(out, s, 16);
  }

 private:
  static uint8_t Xt(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
  static uint8_t Sbox(uint8_t x) {
    static const std::array<uint8_t, 256> box = [] {
      std::array<uint8_t, 256> b{};
      // The S-box from its definition: the multiplicative inverse in GF(2^8),
      // then the affine map.
      uint8_t p = 1, q = 1;
      do {
        p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q = uint8_t(q ^ (q << 1));
        q = uint8_t(q ^ (q << 2));
        q = uint8_t(q ^ (q << 4));
        if (q & 0x80) q ^= 0x09;
        const uint8_t r = uint8_t(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^
                                  ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        b[p] = uint8_t(r ^ 0x63);
      } while (p != 1);
      b[0] = 0x63;
      return b;
    }();
    return box[x];
  }

  std::array<uint8_t, 176> w_{};
};

// The PS3 package key: published in the PS3 file format documentation and in
// every open source package tool.
constexpr uint8_t kPkgKey[16] = {0x2E, 0x7B, 0x71, 0xD7, 0xC9, 0xC9, 0xA1, 0x4E,
                                 0xA3, 0x22, 0x1F, 0x18, 0x88, 0x28, 0xB8, 0xF8};

// AES-CTR over `data`, which starts `block` 16-byte blocks into the stream.
void Ctr(const Aes128& aes, const uint8_t riv[16], uint64_t block, uint8_t* data, size_t n) {
  uint8_t counter[16], pad[16];
  std::memcpy(counter, riv, 16);
  // counter += block, as one 128-bit big-endian number.
  uint64_t carry = block;
  for (int i = 15; i >= 0 && carry; --i) {
    const uint64_t sum = uint64_t(counter[i]) + (carry & 0xFF);
    counter[i] = uint8_t(sum);
    carry = (carry >> 8) + (sum >> 8);
  }
  for (size_t at = 0; at < n; at += 16) {
    aes.Encrypt(counter, pad);
    const size_t m = std::min<size_t>(16, n - at);
    for (size_t i = 0; i < m; ++i) data[at + i] ^= pad[i];
    for (int i = 15; i >= 0; --i) {
      if (++counter[i] != 0) break;
    }
  }
}

// ---------------------------------------------------------------------------
// zlib, through miniz

bool Inflate(const uint8_t* src, size_t n, size_t expected, std::vector<uint8_t>& out) {
  for (const int flags : {int(TINFL_FLAG_PARSE_ZLIB_HEADER), 0}) {
    size_t got = 0;
    void* p = tinfl_decompress_mem_to_heap(src, n, &got, flags);
    if (p) {
      out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + got);
      mz_free(p);
      return true;
    }
  }
  (void)expected;
  return false;
}

std::vector<uint8_t> Deflate(const std::vector<uint8_t>& src) {
  mz_ulong n = mz_compressBound(mz_ulong(src.size()));
  std::vector<uint8_t> out(n);
  if (mz_compress2(out.data(), &n, src.data(), mz_ulong(src.size()), 9) != MZ_OK) return {};
  out.resize(n);
  return out;
}

// ---------------------------------------------------------------------------
// The EA BGFA archive (tools/ast_repack.py, Archive). The header is
// little-endian even though everything inside it is big-endian.

struct Entry {
  size_t index;
  std::vector<uint8_t> head;
  std::string name;
  std::vector<uint8_t> name_bytes;
  uint64_t offset, zsize, usize;
  bool packed;
};

class Archive {
 public:
  bool Open(const fs::path& path) {
    if (!ReadFile(path, raw_) || raw_.size() < 0x30 || std::memcmp(raw_.data(), "BGFA", 4) != 0) {
      return false;
    }
    const uint8_t* h = raw_.data();
    files_ = LE32(h + 12);
    info_off_ = LE32(h + 16);
    info2_ = h[0x21];
    info3_ = h[0x22];
    offset_bytes_ = h[0x23];
    zsize_bytes_ = h[0x24];
    size_bytes_ = h[0x25];
    offset_shift_ = h[0x26];
    num_ = LE32(h + 0x28);
    namesz_ = LE32(h + 0x2C);
    flag_bytes_ = info2_ == 0 ? 1 : 2;
    rec_ = flag_bytes_ + info3_ + offset_bytes_ + zsize_bytes_ + size_bytes_ + namesz_;
    prefix_off_ = info_off_;
    table_off_ = info_off_ + size_t(num_) * 4;
    if (offset_bytes_ > 8 || zsize_bytes_ > 8 || size_bytes_ > 8 ||
        table_off_ + size_t(files_) * rec_ > raw_.size()) {
      return false;
    }
    for (size_t i = 0; i < files_; ++i) {
      const uint8_t* rec = raw_.data() + table_off_ + i * rec_;
      size_t q = flag_bytes_ + info3_;
      Entry e;
      e.index = i;
      e.head.assign(rec, rec + q);
      e.offset = GetLE(rec + q, offset_bytes_) << offset_shift_;
      q += offset_bytes_;
      e.zsize = GetLE(rec + q, zsize_bytes_);
      q += zsize_bytes_;
      const uint64_t size = GetLE(rec + q, size_bytes_);
      q += size_bytes_;
      e.name_bytes.assign(rec + q, rec + q + namesz_);
      e.name.assign(reinterpret_cast<const char*>(rec + q),
                    strnlen(reinterpret_cast<const char*>(rec + q), namesz_));
      // The stored field is how much the entry grows unpacked; 0 means stored.
      e.usize = size ? size + e.zsize : e.zsize;
      e.packed = size != 0;
      if (e.offset + e.zsize > raw_.size()) return false;
      entries_.push_back(std::move(e));
    }
    return true;
  }

  const std::vector<Entry>& entries() const { return entries_; }

  bool Read(const Entry& e, std::vector<uint8_t>& out) const {
    const uint8_t* p = raw_.data() + e.offset;
    if (!e.packed) {
      out.assign(p, p + e.zsize);
      return true;
    }
    return Inflate(p, size_t(e.zsize), size_t(e.usize), out);
  }

  // `.XPR` where the PS3 archive said `.GTF`, in the resource tags in front
  // of the entry table: what the game dispatches its texture loader on.
  bool RetagTextures() {
    bool any = false;
    for (size_t p = prefix_off_; p + 4 <= table_off_; ++p) {
      if (std::memcmp(raw_.data() + p, "FTG.", 4) == 0) {
        std::memcpy(raw_.data() + p, "RPX.", 4);
        any = true;
        p += 3;
      }
    }
    return any;
  }

  bool Write(const fs::path& path, const std::map<size_t, std::vector<uint8_t>>& payloads) const {
    const uint64_t align = uint64_t(1) << offset_shift_;
    const uint64_t max_offset =
        (offset_bytes_ >= 8 ? ~uint64_t(0) : ((uint64_t(1) << (offset_bytes_ * 8)) - 1))
        << offset_shift_;
    std::vector<size_t> order(entries_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return entries_[a].offset < entries_[b].offset;
    });
    uint64_t start = ~uint64_t(0);
    for (const Entry& e : entries_) start = std::min(start, e.offset);
    if (entries_.empty()) return false;

    struct Placed {
      uint64_t offset, zsize, usize;
      bool packed;
      std::vector<uint8_t> data;
    };
    std::vector<Placed> placed(entries_.size());
    uint64_t cursor = start;
    for (size_t i : order) {
      const Entry& e = entries_[i];
      Placed& pl = placed[i];
      auto it = payloads.find(i);
      if (it == payloads.end()) {
        pl.data.assign(raw_.begin() + e.offset, raw_.begin() + e.offset + e.zsize);
        pl.usize = e.usize;
        pl.packed = e.packed;
      } else {
        std::vector<uint8_t> z = Deflate(it->second);
        if (!z.empty() && z.size() < it->second.size()) {
          pl.data = std::move(z);
          pl.packed = true;
        } else {
          pl.data = it->second;
          pl.packed = false;
        }
        pl.usize = it->second.size();
      }
      if (cursor > max_offset) return false;
      pl.offset = cursor;
      pl.zsize = pl.data.size();
      cursor += pl.zsize;
      cursor = (cursor + align - 1) / align * align;
    }

    std::vector<uint8_t> out(raw_.begin(), raw_.begin() + start);
    for (size_t i = 0; i < entries_.size(); ++i) {
      const Entry& e = entries_[i];
      const Placed& pl = placed[i];
      std::vector<uint8_t> rec = e.head;
      PutLE(rec, pl.offset >> offset_shift_, offset_bytes_);
      PutLE(rec, pl.zsize, zsize_bytes_);
      PutLE(rec, pl.packed ? pl.usize - pl.zsize : 0, size_bytes_);
      rec.insert(rec.end(), e.name_bytes.begin(), e.name_bytes.end());
      if (rec.size() != rec_ || table_off_ + (i + 1) * rec_ > out.size()) return false;
      std::memcpy(out.data() + table_off_ + i * rec_, rec.data(), rec_);
    }
    for (size_t i : order) {
      const Placed& pl = placed[i];
      if (out.size() < pl.offset + pl.zsize) out.resize(size_t(pl.offset + pl.zsize), 0);
      std::memcpy(out.data() + pl.offset, pl.data.data(), pl.data.size());
    }
    return WriteFile(path, out);
  }

 private:
  std::vector<uint8_t> raw_;
  std::vector<Entry> entries_;
  uint32_t files_ = 0, info_off_ = 0, num_ = 0, namesz_ = 0;
  uint8_t info2_ = 0, info3_ = 0, offset_bytes_ = 0, zsize_bytes_ = 0, size_bytes_ = 0,
          offset_shift_ = 0;
  size_t flag_bytes_ = 1, rec_ = 0, prefix_off_ = 0, table_off_ = 0;
};

// ---------------------------------------------------------------------------
// Textures (tools/ast_repack.py)

struct Format {
  uint32_t fmt, unit, texels, endian, swizzle;
};

std::optional<Format> RsxFormat(uint8_t code) {
  switch (code) {
    case 0x86: return Format{18, 8, 4, 0, 0x00000D10};   // DXT1
    case 0x87: return Format{19, 16, 4, 0, 0x00000D10};  // DXT3
    case 0x88: return Format{20, 16, 4, 0, 0x00000D10};  // DXT5
    case 0x85:                                            // A8R8G8B8
    case 0xA5: return Format{6, 4, 1, 2, 0x00000C14};
    default: return std::nullopt;
  }
}

// A PS3 archive pads its uncompressed levels out to this.
constexpr size_t kLinearRowAlign = 256;

// The disc's own row stride for each texture it ships, by name and shape;
// -1 where the same key gave two answers.
using Pitches = std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t>, int64_t>;

bool IsPs3Texture(const std::vector<uint8_t>& b) {
  if (b.size() < 0x80 || b[0x1A] != 2 || !RsxFormat(b[0x18])) return false;
  if (BE32(&b[0x10]) != 0x80) return false;
  const uint32_t w = BE16(&b[0x20]), h = BE16(&b[0x22]);
  return w > 0 && w <= 4096 && h > 0 && h <= 4096;
}

bool IsDds(const std::vector<uint8_t>& b) {
  return b.size() > 0x80 && std::memcmp(b.data(), "DDS ", 4) == 0 && LE32(&b[4]) == 124;
}

// Every XPR2 texture in the stock archives carries these 28 bytes in front of
// its fetch constant.
constexpr uint8_t kXpr2RecordPrefix[28] = {0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0};

// An XPR2 resource over linear, untiled pixel data. See build_xpr2 in
// tools/ast_repack.py for why each field is what it is.
std::optional<std::vector<uint8_t>> BuildXpr2(const std::string& name, uint32_t w, uint32_t h,
                                              const Format& f, const uint8_t* src,
                                              size_t src_len, size_t src_pitch,
                                              std::optional<int64_t> pitch_texels_in) {
  const size_t rows = std::max<size_t>(1, (h + f.texels - 1) / f.texels);
  const size_t row = std::max<size_t>(1, (w + f.texels - 1) / f.texels) * f.unit;
  const uint32_t pitch_texels = pitch_texels_in && *pitch_texels_in >= 0
                                    ? uint32_t(*pitch_texels_in)
                                    : std::max<uint32_t>(128, (w + 127) / 128 * 128);
  const size_t pitch = (pitch_texels / f.texels) * f.unit;
  if (src_len < src_pitch * (rows - 1) + row) return std::nullopt;
  std::vector<uint8_t> pixels(pitch * rows, 0);
  for (size_t y = 0; y < rows; ++y) {
    // A pitch narrower than the row would overrun; Python's slice assignment
    // grows the buffer there, so do the same.
    const size_t at = y * pitch;
    if (at + row > pixels.size()) pixels.resize(at + row, 0);
    std::memcpy(pixels.data() + at, src + y * src_pitch, row);
  }

  const std::string label = Lower(name);
  const size_t needed = 0x24 + label.size() + 1 - 12;
  const size_t record = 12 + std::max<size_t>(48, (needed + 15) / 16 * 16);
  const size_t desc = record + 28;
  const size_t end = record + 52;
  std::vector<uint8_t> out(end + 12, 0);
  std::memcpy(out.data(), "XPR2", 4);
  PutBE32(&out[0x04], uint32_t(end));
  PutBE32(&out[0x08], uint32_t(pixels.size()));
  PutBE32(&out[0x0C], 1);
  std::memcpy(&out[0x10], "TX2D", 4);
  PutBE32(&out[0x14], uint32_t(record - 12));
  PutBE32(&out[0x18], 52);
  PutBE32(&out[0x1C], 24);
  std::memcpy(&out[0x24], label.data(), label.size());
  std::memcpy(&out[record], kXpr2RecordPrefix, 28);
  PutBE32(&out[desc + 0], 2u | ((pitch_texels >> 5) & 0x1FFu) << 22);
  PutBE32(&out[desc + 4], (f.fmt & 0x3F) | ((f.endian & 3) << 6));
  PutBE32(&out[desc + 8], ((w - 1) & 0x1FFF) | (((h - 1) & 0x1FFF) << 13));
  PutBE32(&out[desc + 12], f.swizzle);
  PutBE32(&out[desc + 16], 0);                     // base mip only
  PutBE32(&out[desc + 20], (1u << 9) | (1u << 11));  // 2D, packed mips
  out.insert(out.end(), pixels.begin(), pixels.end());
  return out;
}

std::optional<int64_t> Lookup(const Pitches& p, const std::string& name, uint32_t w, uint32_t h,
                              uint32_t fmt) {
  auto it = p.find({name, w, h, fmt});
  if (it == p.end()) return std::nullopt;
  return it->second;  // -1 (ambiguous) falls back to the rule, as None does
}

std::optional<std::vector<uint8_t>> Ps3ToXpr2(const std::vector<uint8_t>& b,
                                              const std::string& name, const Pitches& pitches) {
  const Format f = *RsxFormat(b[0x18]);
  const uint32_t w = BE16(&b[0x20]), h = BE16(&b[0x22]);
  const size_t row = std::max<size_t>(1, (w + f.texels - 1) / f.texels) * f.unit;
  const size_t src_pitch =
      f.texels == 1 ? (row + kLinearRowAlign - 1) / kLinearRowAlign * kLinearRowAlign : row;
  return BuildXpr2(name, w, h, f, b.data() + 0x80, b.size() - 0x80, src_pitch,
                   Lookup(pitches, Lower(name), w, h, f.fmt));
}

std::optional<std::vector<uint8_t>> DdsToXpr2(const std::vector<uint8_t>& b,
                                              const Pitches& pitches) {
  const uint32_t h = LE32(&b[0x0C]), w = LE32(&b[0x10]);
  Format f;
  if (std::memcmp(&b[0x54], "DXT1", 4) == 0) f = {18, 8, 4, 0, 0x00000D10};
  else if (std::memcmp(&b[0x54], "DXT3", 4) == 0) f = {19, 16, 4, 0, 0x00000D10};
  else if (std::memcmp(&b[0x54], "DXT5", 4) == 0) f = {20, 16, 4, 0, 0x00000D10};
  else if (LE32(&b[0x58]) == 32) f = {6, 4, 1, 2, 0x00000C14};
  else return std::nullopt;
  if (!(w > 0 && w <= 4096 && h > 0 && h <= 4096)) return std::nullopt;
  const size_t row = std::max<size_t>(1, (w + f.texels - 1) / f.texels) * f.unit;
  return BuildXpr2("strName", w, h, f, b.data() + 0x80, b.size() - 0x80, row,
                   Lookup(pitches, "strname", w, h, f.fmt));
}

Pitches StockPitches(const fs::path& path) {
  Pitches out;
  Archive a;
  if (path.empty() || !a.Open(path)) return out;
  std::vector<uint8_t> blob;
  for (const Entry& e : a.entries()) {
    if (!a.Read(e, blob) || blob.size() < 0x28 || std::memcmp(blob.data(), "XPR2", 4) != 0) {
      continue;
    }
    const size_t desc = size_t(BE32(&blob[0x14])) + 40;
    if (desc + 12 > blob.size()) continue;
    const uint32_t w0 = BE32(&blob[desc]), w1 = BE32(&blob[desc + 4]),
                   w2 = BE32(&blob[desc + 8]);
    const char* n = reinterpret_cast<const char*>(&blob[0x24]);
    const std::string name(n, strnlen(n, blob.size() - 0x24));
    const auto key = std::make_tuple(name, (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1, w1 & 0x3F);
    const int64_t pitch = int64_t((w0 >> 22) & 0x1FF) * 32;
    auto it = out.find(key);
    if (it != out.end() && it->second != pitch) it->second = -1;
    else out[key] = pitch;
  }
  return out;
}

bool HoldsDds(const fs::path& path) {
  Archive a;
  if (!a.Open(path)) return false;
  std::vector<uint8_t> blob;
  for (const Entry& e : a.entries()) {
    if (a.Read(e, blob) && blob.size() >= 4 && std::memcmp(blob.data(), "DDS ", 4) == 0) {
      return true;
    }
  }
  return false;
}

// Whether the DDS in this archive is the game's own business: where the disc
// keeps plain DDS in the same archive, whatever reads it wants plain DDS.
bool DdsIsTheGamesOwn(const fs::path& src, const fs::path& stock) {
  std::error_code ec;
  if (!stock.empty() && fs::is_regular_file(stock, ec)) return HoldsDds(stock);
  for (const auto& part : src) {
    if (part == "bigs") return true;
  }
  return false;
}

// The roster update: only the PS3 textures are converted; its DDS portraits
// are the front end's and stay as they are.
int ConvertRosterUpdate(const fs::path& src, const fs::path& dst) {
  Archive a;
  if (!a.Open(src)) return 0;
  std::map<size_t, std::vector<uint8_t>> payloads;
  std::vector<uint8_t> blob;
  for (const Entry& e : a.entries()) {
    if (!a.Read(e, blob) || !IsPs3Texture(blob)) continue;
    if (auto out = Ps3ToXpr2(blob, e.name, {})) payloads[e.index] = std::move(*out);
  }
  if (payloads.empty()) return 0;
  a.RetagTextures();
  return a.Write(dst, payloads) ? int(payloads.size()) : 0;
}

// ---------------------------------------------------------------------------
// Import (tools/dlc.py)

constexpr const char* kRosterUpdate = "rend_roster_update_big.ast";

std::string MapPath(const std::string& rel) {
  return rel.rfind("ps3/", 0) == 0 ? "xenon/" + rel.substr(4) : rel;
}

// PS3-only files, and the two archives that are platform code rather than
// data. See SKIP_PATTERNS in tools/dlc.py.
bool Skip(const std::string& mapped) {
  const std::string l = Lower(mapped);
  if (l.size() >= 4 && l.compare(l.size() - 4, 4, ".gtf") == 0) return true;
  static const std::regex patterns[] = {
      std::regex("^xenon/saveload/"),
      std::regex("^xenon/genbigs/rend_misc_big\\.ast$"),
      std::regex("^xenon/bigs/audio/aemsdata\\.ast$"),
  };
  for (const auto& p : patterns) {
    if (std::regex_search(mapped, p)) return true;
  }
  return false;
}

bool CopyOver(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

// Where the mod's data/ is: in the folder, in its USRDIR, or one level down.
fs::path FindModData(const fs::path& mod, fs::path* usrdir) {
  std::error_code ec;
  std::vector<fs::path> tries = {mod, mod / "USRDIR"};
  for (const auto& e : fs::directory_iterator(mod, ec)) {
    if (e.is_directory(ec)) {
      tries.push_back(e.path());
      tries.push_back(e.path() / "USRDIR");
    }
  }
  for (const auto& t : tries) {
    if (fs::is_directory(t / "data", ec)) {
      *usrdir = t;
      return t / "data";
    }
  }
  return {};
}

}  // namespace

bool NbaRepackAst(const fs::path& src, const fs::path& dst, const fs::path& stock) {
  Archive a;
  if (!a.Open(src)) return false;
  const bool convert_dds = !DdsIsTheGamesOwn(src, stock);
  const Pitches pitches = StockPitches(stock);
  std::map<size_t, std::vector<uint8_t>> payloads;
  std::vector<uint8_t> blob;
  for (const Entry& e : a.entries()) {
    if (!a.Read(e, blob)) continue;
    std::optional<std::vector<uint8_t>> out;
    if (IsPs3Texture(blob)) {
      out = Ps3ToXpr2(blob, e.name, pitches);
    } else if (IsDds(blob) && convert_dds) {
      out = DdsToXpr2(blob, pitches);
    } else {
      continue;
    }
    if (out) payloads[e.index] = std::move(*out);
  }
  if (payloads.empty()) return false;
  a.RetagTextures();
  return a.Write(dst, payloads);
}

int NbaExtractPs3Pkg(const fs::path& pkg, const fs::path& out, std::ostream& status) {
  std::ifstream f(pkg, std::ios::binary);
  uint8_t head[0x80]{};
  if (!f.read(reinterpret_cast<char*>(head), sizeof(head)) ||
      std::memcmp(head, "\x7FPKG", 4) != 0) {
    status << pkg.filename().string() << " is not a PS3 package\n";
    return -1;
  }
  const uint32_t item_count = BE32(head + 0x14);
  const uint64_t data_offset = BE64(head + 0x20), data_size = BE64(head + 0x28);
  uint8_t riv[16];
  std::memcpy(riv, head + 0x70, 16);
  const Aes128 aes(kPkgKey);

  // The item table and the names after it, at the start of the data region.
  size_t head_len = size_t(std::min<uint64_t>(data_size, uint64_t(item_count) * 0x20 + 256 * 1024));
  head_len = (head_len + 15) & ~size_t(15);
  std::vector<uint8_t> table(head_len);
  f.seekg(std::streamoff(data_offset));
  f.read(reinterpret_cast<char*>(table.data()), std::streamsize(head_len));
  table.resize(size_t(f.gcount()));
  Ctr(aes, riv, 0, table.data(), table.size());

  std::error_code ec;
  fs::create_directories(out, ec);
  std::vector<uint8_t> buf;
  int written = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    if (size_t(i) * 0x20 + 0x20 > table.size()) return -1;
    const uint8_t* r = table.data() + size_t(i) * 0x20;
    const uint32_t name_off = BE32(r), name_len = BE32(r + 4);
    const uint64_t file_off = BE64(r + 8), file_size = BE64(r + 16);
    const uint32_t kind = BE32(r + 24) & 0xFF;
    if (uint64_t(name_off) + name_len > table.size()) {
      status << "a name in " << pkg.filename().string() << " is out of reach\n";
      return -1;
    }
    std::string name(reinterpret_cast<const char*>(table.data() + name_off), name_len);
    name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
    fs::path target = out;
    std::string part;
    for (char c : name + "/") {
      if (c == '/' || c == '\\') {
        if (!part.empty() && part != "." && part != "..") target /= part;
        part.clear();
      } else {
        part += c;
      }
    }
    if (kind == 0x04) {
      fs::create_directories(target, ec);
      continue;
    }
    if (file_off % 16 != 0) return -1;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream o(target, std::ios::binary | std::ios::trunc);
    if (!o) {
      status << "could not write " << target.string() << "\n";
      return -1;
    }
    f.clear();
    f.seekg(std::streamoff(data_offset + file_off));
    uint64_t remaining = file_size, block = file_off / 16;
    constexpr size_t kChunk = 8u << 20;
    while (remaining > 0) {
      const size_t n = size_t(std::min<uint64_t>(kChunk, remaining));
      const size_t padded = (n + 15) & ~size_t(15);
      buf.resize(padded);
      f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(padded));
      if (size_t(f.gcount()) < n) return -1;
      Ctr(aes, riv, block, buf.data(), padded);
      o.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(n));
      remaining -= n;
      block += padded / 16;
    }
    ++written;
  }
  status << written << " file(s) out of " << pkg.filename().string() << "\n";
  return written;
}

std::vector<fs::path> NbaFindPs3Pkgs(const fs::path& folder) {
  std::vector<std::pair<uintmax_t, fs::path>> found;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(folder, ec)) {
    if (e.is_regular_file(ec) && Lower(e.path().extension().string()) == ".pkg") {
      found.emplace_back(e.file_size(ec), e.path());
    }
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<fs::path> out;
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

int NbaApplyPs3Mod(const fs::path& mod, const fs::path& game, std::ostream& status) {
  fs::path usrdir;
  const fs::path data = FindModData(mod, &usrdir);
  if (data.empty()) {
    status << "no data folder in " << mod.string() << "\n";
    return -1;
  }
  std::error_code ec;
  int copied = 0, skipped = 0, repacked = 0;
  for (auto it = fs::recursive_directory_iterator(data, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    // Worked out from the names alone: fs::relative asks the drive for each
    // path's final form, which D:\\DevelopmentFiles does not answer.
    const std::string rel = it->path().lexically_relative(data).generic_string();
    const std::string mapped = MapPath(rel);
    if (Skip(mapped)) {
      ++skipped;
      continue;
    }
    const fs::path target = game / "data" / fs::path(mapped);
    const std::string l = Lower(mapped);
    bool done = false;
    if (l.size() > 4 && l.compare(l.size() - 4, 4, ".ast") == 0) {
      // The game's own copy decides how the textures are laid out, and is
      // about to be replaced, so the result goes beside it first.
      const fs::path tmp = target.string() + ".openjam";
      if (NbaRepackAst(it->path(), tmp, target)) {
        fs::rename(tmp, target, ec);
        if (ec) fs::remove(tmp, ec);
        done = !ec;
        if (done) ++repacked;
      }
    }
    if (!done && !CopyOver(it->path(), target)) {
      status << "could not write " << target.string() << "\n";
      return -1;
    }
    if (++copied % 250 == 0) {
      status << "  " << copied << " file(s)\n";
      status.flush();
    }
  }

  // EA's last roster update sits beside the data, not in it.
  int players = 0;
  if (fs::is_regular_file(usrdir / kRosterUpdate, ec)) {
    players = ConvertRosterUpdate(usrdir / kRosterUpdate, game / kRosterUpdate);
  }

  // The mod's own config files ask for data/ps3/... by name. A hard link per
  // file where the console allows it, a copy where it does not.
  const fs::path xenon = game / "data" / "xenon", ps3 = game / "data" / "ps3";
  int aliased = 0;
  if (fs::is_directory(xenon, ec) && !fs::exists(ps3, ec)) {
    for (auto it = fs::recursive_directory_iterator(xenon, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const fs::path t = ps3 / it->path().lexically_relative(xenon);
      fs::create_directories(t.parent_path(), ec);
      std::error_code lec;
      fs::create_hard_link(it->path(), t, lec);
      if (lec) CopyOver(it->path(), t);
      ++aliased;
    }
  }

  status << copied << " mod file(s) applied, " << skipped << " left out as PS3-only, "
         << repacked << " archive(s) converted";
  if (players) status << ", roster update: " << players << " texture(s)";
  if (aliased) status << ", " << aliased << " file(s) also as data/ps3";
  status << "\n";
  return copied;
}
