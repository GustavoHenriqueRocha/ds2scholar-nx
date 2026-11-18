// ds2-deko3d: the .dkp format (app/src/me/native/sa2_shader_library_dk.h, namespace dkp) on synthetic packages.
//   g++ -std=c++23 -Iapp/src/me/native -Isdk/thirdparty/xxHash tools/tests/sa2_dkp_format_test.cpp -o /tmp/dkp_test
//   /tmp/dkp_test [package.dkp]   (with a file: parse one written by ds2-deko3d tools/shaders/gerar_dkp.py)
#include <cassert>
#include <cstdio>
#include <vector>

#include "sa2_shader_library_dk.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace dkp = sa2::native::dkp;

namespace {

uint64_t Hash(const uint8_t* data, size_t bytes) { return XXH3_64bits(data, bytes); }

void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}
void Put64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
  Put32(b, at, uint32_t(v));
  Put32(b, at + 4, uint32_t(v >> 32));
}

struct Spec {
  uint32_t spec, mask60, kind, stage;
};

// Two entries: entry 0 with a generic and two specialized variants, entry 1 with only a generic one.
std::vector<uint8_t> Build(const std::vector<std::vector<Spec>>& per_entry) {
  uint32_t n_variants = 0;
  for (const auto& e : per_entry) n_variants += uint32_t(e.size());
  const size_t variants_offset = dkp::kHeaderBytes + per_entry.size() * dkp::kEntryBytes;
  size_t blobs_offset = variants_offset + n_variants * dkp::kVariantBytes;
  blobs_offset = (blobs_offset + 255) & ~size_t(255);
  std::vector<uint8_t> b(blobs_offset + n_variants * 256, 0);
  std::memcpy(b.data(), dkp::kMagic, 8);
  Put32(b, 8, dkp::kVersion);
  Put32(b, 12, uint32_t(per_entry.size()));
  Put64(b, 16, 12345);
  Put64(b, 24, 0xABCDEF);
  Put32(b, 32, n_variants);
  Put64(b, 40, variants_offset);
  Put64(b, 48, blobs_offset);
  uint32_t v = 0;
  for (size_t i = 0; i < per_entry.size(); ++i) {
    const size_t e = dkp::kHeaderBytes + i * dkp::kEntryBytes;
    Put64(b, e, 0x1000 + i);
    Put32(b, e + 8, v);
    Put32(b, e + 12, uint32_t(per_entry[i].size()));
    Put32(b, e + 16, dkp::kFactsValid | uint32_t(i));
    Put32(b, e + 20, 0x3 << i);
    Put32(b, e + 24, 1);
    Put32(b, e + 28, 0x7FFF);
    for (const Spec& s : per_entry[i]) {
      const size_t at = variants_offset + v * dkp::kVariantBytes;
      const size_t blob = blobs_offset + v * 256;
      Put32(b, blob, dkp::kDkshMagic);
      b[blob + 4] = uint8_t(v);
      Put32(b, at, s.spec);
      Put32(b, at + 4, s.mask60);
      Put32(b, at + 8, s.kind);
      Put32(b, at + 12, s.stage);
      Put64(b, at + 16, blob);
      Put32(b, at + 24, 64);
      Put64(b, at + 32, Hash(b.data() + blob, 64));
      ++v;
    }
  }
  Put64(b, 56, Hash(b.data() + dkp::kHeaderBytes, blobs_offset - dkp::kHeaderBytes));
  return b;
}

const char* Parse(const std::vector<uint8_t>& b, dkp::Header& h, std::vector<dkp::Entry>& e,
                  std::vector<dkp::Variant>& v) {
  return dkp::ParseTables(b.data(), b.size(), b.size(), Hash, h, e, v);
}

void Rehash(std::vector<uint8_t>& b) {
  Put64(b, 56, Hash(b.data() + dkp::kHeaderBytes, size_t(dkp::Le64(b.data() + 48)) - dkp::kHeaderBytes));
}

int File(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return 2;
  std::vector<uint8_t> b;
  uint8_t chunk[65536];
  for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) b.insert(b.end(), chunk, chunk + n);
  std::fclose(f);
  dkp::Header h;
  std::vector<dkp::Entry> e;
  std::vector<dkp::Variant> v;
  if (const char* why = Parse(b, h, e, v)) {
    std::printf("dkp: %s: %s\n", path, why);
    return 1;
  }
  uint32_t bad = 0;
  for (const auto& x : v)
    if (Hash(b.data() + x.offset, x.size) != x.hash || dkp::Le32(b.data() + x.offset) != dkp::kDkshMagic) ++bad;
  std::printf("dkp: %s: %u entries, %u variants, %u bad blobs\n", path, h.count, h.variants, bad);
  return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) return File(argv[1]);
  const std::vector<std::vector<Spec>> good = {
      {{0, 0, dkp::kGeneric, 0}, {0x20, 0x3, dkp::kSpecialized, 0}, {0x20, 0x7, dkp::kSpecialized, 0}},
      {{0, 0, dkp::kGeneric, 4}}};
  dkp::Header h;
  std::vector<dkp::Entry> e;
  std::vector<dkp::Variant> v;

  auto b = Build(good);
  assert(Parse(b, h, e, v) == nullptr);
  assert(h.count == 2 && h.variants == 4 && h.mesp_size == 12345 && h.mesp_checksum == 0xABCDEF);
  assert(e[0].fingerprint == 0x1000 && e[1].first_variant == 3 && e[1].variants == 1);
  assert(e[1].facts == (dkp::kFactsValid | 1) && e[1].ps_inputs == 0x6 && e[0].spec_mask == 0x7FFF);
  assert(v[3].stage == 4 && v[1].offset % dkp::kBlobAlignment == 0);
  // Lookup: exact key, generic key, a missing key.
  assert(dkp::FindVariant(e[0], v, dkp::kSpecialized, 0x20, 0x7) == 2);
  assert(dkp::FindVariant(e[0], v, dkp::kSpecialized, 0x20, 0x3) == 1);
  assert(dkp::FindVariant(e[0], v, dkp::kGeneric, 0, 0) == 0);
  assert(dkp::FindVariant(e[0], v, dkp::kSpecialized, 0x21, 0x3) == UINT32_MAX);
  assert(dkp::FindVariant(e[1], v, dkp::kSpecialized, 0x20, 0x3) == UINT32_MAX);
  assert(dkp::FindVariant(e[1], v, dkp::kGeneric, 0, 0) == 3);

  // Damage: every one must be refused with a reason.
  auto bad = b;
  bad[0] = 'X';
  assert(Parse(bad, h, e, v) != nullptr);  // signature
  bad = b;
  Put32(bad, 8, 2);
  assert(Parse(bad, h, e, v) != nullptr);  // version
  bad = b;
  bad[dkp::kHeaderBytes + 20] ^= 1;
  assert(Parse(bad, h, e, v) != nullptr);  // tables altered, hash not updated
  bad = b;
  Put32(bad, dkp::kHeaderBytes + dkp::kEntryBytes + 8, 2);  // entry 1 overlaps entry 0
  Rehash(bad);
  assert(Parse(bad, h, e, v) != nullptr);
  bad = b;
  Put64(bad, dkp::Le64(b.data() + 40) + 16, b.size() - 32);  // blob past the end
  Rehash(bad);
  assert(Parse(bad, h, e, v) != nullptr);
  bad = b;
  Put64(bad, dkp::Le64(b.data() + 40) + 16, dkp::Le64(b.data() + 48) + 8);  // blob not aligned
  Rehash(bad);
  assert(Parse(bad, h, e, v) != nullptr);
  bad = b;
  Put32(bad, dkp::Le64(b.data() + 40) + 12, 1);  // stage neither vertex nor fragment
  Rehash(bad);
  assert(Parse(bad, h, e, v) != nullptr);
  assert(Parse(std::vector<uint8_t>(b.begin(), b.begin() + 63), h, e, v) != nullptr);  // short

  // Unsorted and repeated variants of an entry.
  assert(Parse(Build({{{0x20, 0x3, dkp::kSpecialized, 0}, {0, 0, dkp::kGeneric, 0}}}), h, e, v) != nullptr);
  assert(Parse(Build({{{0x20, 0x3, dkp::kSpecialized, 0}, {0x20, 0x3, dkp::kSpecialized, 0}}}), h, e, v) != nullptr);
  // An entry without variants is valid (Variant() then returns nullptr for it).
  assert(Parse(Build({{}, {{0, 0, dkp::kGeneric, 4}}}), h, e, v) == nullptr && e[0].variants == 0);
  std::puts("dkp: parse, lookup, and 11 kinds of damage passed");
}
