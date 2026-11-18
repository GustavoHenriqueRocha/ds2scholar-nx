// ds2-deko3d: the game shaders as DKSH (deko3d), from a package next to sa2_shaders.mesp.
//
// The Vulkan renderer gives the driver SPIR-V and lets it specialize (spec constant 0 = PipelineKey::specialization,
// 60 = the pixel shader's input mask on the vertex stage) and rewrites SPIR-V at run time (alpha-only, 7e3 output
// limit, early Z, rectangle expansion, FragCoord XY, half-range depth, float24 quantization). deko3d has no shader
// compiler on the console: everything is compiled offline (ds2-deko3d tools/shaders/gerar_dkp.py) into
// sa2_shaders.dkp, one DKSH per (shader, variant) the game was seen to use, plus a generic DKSH per shader that reads
// the specialization from uniform buffer 3 at run time (correct, slower: real branches). A variant not in the
// package falls back to the generic one and is recorded (RecordMissing) for the next offline pass, except when the
// key needs a SPIR-V transform the generic DKSH lacks (kSpecNeedsTransform, or the pixel stage's raster bits): the
// Vulkan renderer rejects the pipeline when such a transform fails, and so does Variant() then (nullptr).
//
// Format (little-endian; ds2-deko3d docs/pesquisas/traducao/07-auxiliares-e-shaders.md, section 2.3):
//   header (64 bytes): "MESDKP\0\0", u32 version (1), u32 count (= the .mesp's, same order), u64 .mesp size,
//     u64 .mesp checksum (the u64 at +16 of its header, as the .idx binds itself), u32 variants, u32 flags (0),
//     u64 offset of the variant table, u64 offset of the blobs, u64 XXH3 of bytes [64, blobs).
//   entries[count] (32 bytes), in .mesp order: u64 container fingerprint (checked against the library), u32 first
//     variant, u32 variants, u32 facts (kFact*), u32 pixel shader input mask (Location bits, 0xFFFFFFFF unknown),
//     u32 kills, u32 spec mask (the specialization bits this shader's variants are keyed by).
//   variants[variants] (40 bytes), grouped by entry, sorted by (kind, spec, mask60), unique: u32 spec, u32 mask60
//     (vertex stage: constant 60; pixel stage: PipelineKey::rasterization & kRasterKeyBits), u32 kind (0 generic, 1 specialized), u32 stage (DkStage:
//     0 vertex, 4 fragment), u64 blob offset (multiple of 256), u32 blob size, u32 reserved, u64 XXH3 of the blob.
//   blobs: whole DKSH files (control + code), each at a multiple of 256.
// The format part (namespace dkp) is plain C++ for the host test (tools/tests/sa2_dkp_format_test.cpp).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sa2::native::dkp {

constexpr char kMagic[8] = {'M', 'E', 'S', 'D', 'K', 'P', 0, 0};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 64;
constexpr size_t kEntryBytes = 32;
constexpr size_t kVariantBytes = 40;
constexpr uint32_t kBlobAlignment = 256;  // DK_SHADER_CODE_ALIGNMENT
constexpr uint32_t kDkshMagic = 0x48534B44;  // "DKSH"

// Entry facts: what the Vulkan renderer reads from the SPIR-V at run time (sa2_native_draws.cpp HasStructuredCf,
// VsIndexUnused, HasShaderLight; ShaderEntry::discards). kFactsValid = 0: the tool could not tell, read the SPIR-V.
constexpr uint32_t kFactTestsStructuredCf = 1u << 0;     // SpirvTestsSpecBit(kSpecStructuredCf)
constexpr uint32_t kFactTestsVertexIndexUnused = 1u << 1;  // SpirvTestsSpecBit(kSpecVertexIndexUnused)
constexpr uint32_t kFactShaderLight = 1u << 2;           // Ds2ShaderLightSupported
constexpr uint32_t kFactVertex = 1u << 3;
constexpr uint32_t kFactsValid = 1u << 31;

enum Kind : uint32_t { kGeneric = 0, kSpecialized = 1 };

// Specialization bits whose SPIR-V transform the Vulkan renderer requires (it rejects the pipeline if it fails):
// 21 float24 half depth, 22 raster grid X, 29 rectangle, 30-31 float24 quantize/round (sa2_native_draws.cpp
// kSpec*). The other transform bits (14 alpha only, 20 early Z, 24-28 7e3 limit) fall back to the regular module
// there too, so a missing variant with only those may use the generic DKSH.
constexpr uint32_t kSpecNeedsTransform = (1u << 21) | (1u << 22) | (1u << 29) | (1u << 30) | (1u << 31);
// The pixel stage's part of PipelineKey::rasterization in the variant key: kNativeMsaa2PhaseProbe (0x40) and
// kNativeMsaa2PhaseMode2 (0x80), which pick the guest FragCoord XY transform (me_raster_state.h).
constexpr uint32_t kRasterKeyBits = 0xC0;

struct Header {
  uint32_t version = 0, count = 0;
  uint64_t mesp_size = 0, mesp_checksum = 0;
  uint32_t variants = 0, flags = 0;
  uint64_t variants_offset = 0, blobs_offset = 0, tables_hash = 0;
};
struct Entry {
  uint64_t fingerprint = 0;
  uint32_t first_variant = 0, variants = 0, facts = 0, ps_inputs = 0xFFFFFFFFu, kills = 0, spec_mask = 0;
};
struct Variant {
  uint32_t spec = 0, mask60 = 0, kind = kGeneric, stage = 0;
  uint64_t offset = 0;
  uint32_t size = 0;
  uint64_t hash = 0;
};

inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t Le64(const uint8_t* p) { return Le32(p) | uint64_t(Le32(p + 4)) << 32; }

// The (kind, spec, mask60) order of the variant table.
inline bool VariantLess(const Variant& a, uint32_t kind, uint32_t spec, uint32_t mask60) {
  if (a.kind != kind) return a.kind < kind;
  if (a.spec != spec) return a.spec < spec;
  return a.mask60 < mask60;
}

// Parses and checks the header and both tables (not the blob contents: each blob is checked by its XXH3 when it is
// read). `hash` is the XXH3 the caller computes over [64, blobs_offset). `file_size` bounds every offset. Returns
// nullptr if correct, or the reason.
template <typename Hash>
const char* ParseTables(const uint8_t* data, size_t size, uint64_t file_size, Hash hash, Header& h,
                        std::vector<Entry>& entries, std::vector<Variant>& variants) {
  entries.clear();
  variants.clear();
  if (size < kHeaderBytes || std::memcmp(data, kMagic, 8) != 0) return "not a .dkp (signature)";
  h.version = Le32(data + 8);
  h.count = Le32(data + 12);
  h.mesp_size = Le64(data + 16);
  h.mesp_checksum = Le64(data + 24);
  h.variants = Le32(data + 32);
  h.flags = Le32(data + 36);
  h.variants_offset = Le64(data + 40);
  h.blobs_offset = Le64(data + 48);
  h.tables_hash = Le64(data + 56);
  if (h.version != kVersion) return "unsupported .dkp version";
  if (!h.count || h.count > 65536 || h.variants > (1u << 22)) return "entry or variant count out of bounds";
  if (h.variants_offset != kHeaderBytes + uint64_t(h.count) * kEntryBytes ||
      h.blobs_offset < h.variants_offset + uint64_t(h.variants) * kVariantBytes || h.blobs_offset % kBlobAlignment ||
      h.blobs_offset > size || h.blobs_offset > file_size)
    return "table offsets inconsistent";
  if (hash(data + kHeaderBytes, size_t(h.blobs_offset - kHeaderBytes)) != h.tables_hash) return ".dkp tables altered";
  entries.resize(h.count);
  uint32_t next_variant = 0;
  for (uint32_t i = 0; i < h.count; ++i) {
    const uint8_t* e = data + kHeaderBytes + size_t(i) * kEntryBytes;
    Entry& x = entries[i];
    x.fingerprint = Le64(e);
    x.first_variant = Le32(e + 8);
    x.variants = Le32(e + 12);
    x.facts = Le32(e + 16);
    x.ps_inputs = Le32(e + 20);
    x.kills = Le32(e + 24);
    x.spec_mask = Le32(e + 28);
    if (x.first_variant != next_variant || x.variants > h.variants - next_variant) return "entry variant range wrong";
    next_variant += x.variants;
  }
  if (next_variant != h.variants) return "variants not covered by the entries";
  variants.resize(h.variants);
  for (uint32_t i = 0; i < h.variants; ++i) {
    const uint8_t* v = data + h.variants_offset + size_t(i) * kVariantBytes;
    Variant& x = variants[i];
    x.spec = Le32(v);
    x.mask60 = Le32(v + 4);
    x.kind = Le32(v + 8);
    x.stage = Le32(v + 12);
    x.offset = Le64(v + 16);
    x.size = Le32(v + 24);
    x.hash = Le64(v + 32);
    if (x.kind > kSpecialized || (x.stage != 0 && x.stage != 4)) return "variant kind or stage unknown";
    if (x.offset % kBlobAlignment || x.offset < h.blobs_offset || x.size < 24 || x.offset + x.size > file_size)
      return "variant blob outside the package";
  }
  for (const Entry& e : entries) {
    for (uint32_t k = 1; k < e.variants; ++k) {
      const Variant& a = variants[e.first_variant + k - 1];
      if (!VariantLess(a, variants[e.first_variant + k].kind, variants[e.first_variant + k].spec,
                       variants[e.first_variant + k].mask60))
        return "variants of an entry not sorted or repeated";
    }
  }
  return nullptr;
}

// Index of the variant of entry `e` with that key in `variants`, or UINT32_MAX.
inline uint32_t FindVariant(const Entry& e, const std::vector<Variant>& variants, uint32_t kind, uint32_t spec,
                            uint32_t mask60) {
  uint32_t lo = e.first_variant, hi = e.first_variant + e.variants;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (VariantLess(variants[mid], kind, spec, mask60)) lo = mid + 1;
    else hi = mid;
  }
  if (lo < e.first_variant + e.variants) {
    const Variant& v = variants[lo];
    if (v.kind == kind && v.spec == spec && v.mask60 == mask60) return lo;
  }
  return UINT32_MAX;
}

}  // namespace sa2::native::dkp

#if defined(__SWITCH__) || REX_PLATFORM_SWITCH
#include "sa2_dk.h"
#include "sa2_native_shaders.h"
#include "sa2_shader_library.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>

namespace sa2::native {

struct StatsDksh {
  uint64_t specialized = 0;  // Variant() answered with the exact variant
  uint64_t generic = 0;      // ... with the generic one (missing variant)
  uint64_t loaded = 0;       // DKSH put into code memory
  uint64_t preloaded = 0;    // blobs read ahead by Preload
  uint64_t bytes_read = 0;
  uint64_t errors = 0;       // read, hash or DKSH failures (that entry then has nothing)
  uint64_t missing = 0;      // distinct missing variants recorded
};

class LibraryDksh {
 public:
  LibraryDksh();
  ~LibraryDksh();
  LibraryDksh(const LibraryDksh&) = delete;
  LibraryDksh& operator=(const LibraryDksh&) = delete;

  // Opens `dkp` and binds it to the shader library: the .mesp at `mesp` (size and header checksum) and its entries
  // (`fingerprints`: Shader::fingerprint of each, in library order). `missing_folder` (may be empty): where
  // FlushMissing writes. false, with a warning, if the package is missing, damaged or belongs to another .mesp.
  bool Open(const std::filesystem::path& dkp, const std::filesystem::path& mesp,
            const std::vector<uint64_t>& fingerprints, const std::filesystem::path& missing_folder);
  bool loaded() const { return file_ != nullptr; }

  // The shader of entry `entry` (ShaderEntry::number) for this specialization. `spec` is masked by the entry's
  // SpecMask here (the bits the offline tool keyed its variants by); `mask60` is the vertex stage's constant 60,
  // or for a pixel shader PipelineKey::rasterization (masked here by dkp::kRasterKeyBits). The exact variant if the
  // package has it, otherwise the generic one (the caller then binds uniform buffer 3 with {spec, mask60, 0, 0}:
  // *generic_out = true) and the key is recorded as missing. nullptr if the entry has neither, its blob is damaged,
  // or the key needs a transform the generic one lacks (see the top). Ring thread only (CodeArena is not thread
  // safe).
  const DkShader* Variant(uint32_t entry, uint32_t spec, uint32_t mask60, sa2::dk::CodeArena& code,
                          bool* generic_out);

  // What the Vulkan renderer reads from the SPIR-V, precomputed. Facts() without dkp::kFactsValid: unknown.
  uint32_t Facts(uint32_t entry) const;
  uint32_t InputsMaskPs(uint32_t entry) const;  // 0xFFFFFFFF unknown
  uint32_t SpecMask(uint32_t entry) const;
  uint32_t Kills(uint32_t entry) const;

  // Reads ahead the blobs of these (entry, spec, mask60) keys (the pipeline prewarm list), in file order, with a
  // file handle of its own; Variant() then only copies them into code memory. Any thread; `stop` is polled.
  uint32_t Preload(std::span<const std::tuple<uint32_t, uint32_t, uint32_t>> keys, const std::atomic<bool>* stop);

  // Records a variant the package lacks (once per key) and, every call, appends the new ones to
  // <missing_folder>/missing.txt ("fingerprint spec mask60" in hex, one per line). Call from a thread that may
  // block on the SD card (not the ring thread's hot path): the report or the prewarm thread.
  void FlushMissing();

  StatsDksh Stats() const;

 private:
  struct Loaded;
  bool ReadBlob(std::FILE* f, const dkp::Variant& v, std::vector<uint8_t>& out) const;
  void RecordMissing(uint32_t entry, uint32_t spec, uint32_t mask60);
  const DkShader* LoadVariant(uint32_t index, sa2::dk::CodeArena& code);

  std::filesystem::path path_, missing_folder_;
  std::FILE* file_ = nullptr;            // ring thread reads (under read_)
  mutable std::mutex read_;
  dkp::Header header_;
  std::vector<dkp::Entry> entries_;
  std::vector<dkp::Variant> variants_;
  std::vector<const DkShader*> shaders_;  // per variant, nullptr until loaded
  std::vector<uint8_t> failed_;           // per variant: 1 = its blob is damaged, do not retry
  mutable std::mutex preloaded_mutex_;
  std::unordered_map<uint32_t, std::vector<uint8_t>> preloaded_;  // variant -> blob read ahead
  std::mutex missing_mutex_;
  std::set<std::tuple<uint64_t, uint32_t, uint32_t>> missing_;
  std::vector<std::tuple<uint64_t, uint32_t, uint32_t>> missing_pending_;
  mutable std::mutex stats_mutex_;
  StatsDksh stats_;
};

// The process's DKSH package, as ActiveLibrary() is the SPIR-V one: nullptr until OpenActiveLibraryDksh succeeded.
// Any thread.
LibraryDksh* ActiveLibraryDksh();
// Opens <mesp without extension>.dkp next to the shader library `library` just loaded from `mesp` (me_native_system.cpp
// calls it right after ShadersNative::Load), with the missing variants going to <mesp folder>/cache/dk. Once.
bool OpenActiveLibraryDksh(const ShadersNative& library, const std::filesystem::path& mesp);

}  // namespace sa2::native
#endif
