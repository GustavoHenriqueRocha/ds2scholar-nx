// ds2-deko3d: the game shaders as DKSH. See sa2_shader_library_dk.h (format, variants, fallback).

#include "sa2_shader_library_dk.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>

#include <rex/logging.h>

// The same xxHash setup as sa2_shader_library.cpp (memcpy reads: see the comment there).
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace sa2::native {
namespace {

bool ExactRead(std::FILE* f, uint64_t displacement, uint8_t* target, size_t bytes) {
  if (std::fseek(f, long(displacement), SEEK_SET) != 0) return false;
  return std::fread(target, 1, bytes, f) == bytes;
}

uint64_t Hash(const uint8_t* data, size_t bytes) { return XXH3_64bits(data, bytes); }

}  // namespace

LibraryDksh::LibraryDksh() = default;
LibraryDksh::~LibraryDksh() {
  FlushMissing();
  if (file_) std::fclose(file_);
}

bool LibraryDksh::Open(const std::filesystem::path& dkp, const std::filesystem::path& mesp,
                       const std::vector<uint64_t>& fingerprints, const std::filesystem::path& missing_folder) {
  const auto start = std::chrono::steady_clock::now();
  std::FILE* f = std::fopen(dkp.string().c_str(), "rb");
  if (!f) {
    REXLOG_WARN("[dk] shaders: no DKSH package at {}: every draw will be skipped", dkp.string());
    return false;
  }
  const auto fail = [&](const char* why) {
    REXLOG_WARN("[dk] shaders: DKSH package {} not used: {}", dkp.string(), why);
    std::fclose(f);
    entries_.clear();
    variants_.clear();
    return false;
  };
  if (std::fseek(f, 0, SEEK_END) != 0) return fail("could not measure it");
  const long size = std::ftell(f);
  if (size < long(dkp::kHeaderBytes)) return fail("shorter than its header");
  std::array<uint8_t, dkp::kHeaderBytes> head{};
  if (!ExactRead(f, 0, head.data(), head.size())) return fail("header read failed");
  const uint64_t blobs = dkp::Le64(head.data() + 48);
  if (blobs < dkp::kHeaderBytes || blobs > uint64_t(size) || blobs > (uint64_t(256) << 20))
    return fail("table offsets inconsistent");
  std::vector<uint8_t> tables(static_cast<size_t>(blobs));
  if (!ExactRead(f, 0, tables.data(), tables.size())) return fail("table read failed");
  if (const char* why = dkp::ParseTables(tables.data(), tables.size(), uint64_t(size), Hash, header_, entries_,
                                         variants_))
    return fail(why);

  // Bound to this .mesp, as the .idx is (sa2_shader_library.cpp LoadIndexed).
  std::FILE* m = std::fopen(mesp.string().c_str(), "rb");
  if (!m) return fail("the shader package (.mesp) cannot be opened to check it");
  std::array<uint8_t, 24> mesp_head{};
  const bool read = std::fseek(m, 0, SEEK_END) == 0;
  const long mesp_size = read ? std::ftell(m) : -1;
  const bool head_read = ExactRead(m, 0, mesp_head.data(), mesp_head.size());
  std::fclose(m);
  if (!read || !head_read) return fail("the shader package (.mesp) cannot be read to check it");
  if (header_.mesp_size != uint64_t(mesp_size) || header_.mesp_checksum != dkp::Le64(mesp_head.data() + 16))
    return fail("it belongs to another shader package (size or checksum)");
  if (header_.count != fingerprints.size()) return fail("it belongs to another shader package (count)");
  for (uint32_t i = 0; i < header_.count; ++i)
    if (entries_[i].fingerprint != fingerprints[i]) return fail("entry fingerprint differs from the library");

  path_ = dkp;
  missing_folder_ = missing_folder;
  file_ = f;
  shaders_.assign(variants_.size(), nullptr);
  failed_.assign(variants_.size(), 0);
  uint32_t specialized = 0, generic = 0;
  for (const dkp::Variant& v : variants_) (v.kind == dkp::kGeneric ? generic : specialized) += 1;
  REXLOG_INFO("[dk] shaders: DKSH package {}: {} shaders, {} generic and {} specialized variants, {:.1f} MB, "
              "tables in {} ms",
              dkp.string(), header_.count, generic, specialized, double(size) / (1024.0 * 1024.0),
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
  return true;
}

bool LibraryDksh::ReadBlob(std::FILE* f, const dkp::Variant& v, std::vector<uint8_t>& out) const {
  out.resize(v.size);
  if (!ExactRead(f, v.offset, out.data(), out.size())) return false;
  return Hash(out.data(), out.size()) == v.hash && dkp::Le32(out.data()) == dkp::kDkshMagic;
}

const DkShader* LibraryDksh::LoadVariant(uint32_t index, sa2::dk::CodeArena& code) {
  if (shaders_[index]) return shaders_[index];
  if (failed_[index]) return nullptr;
  const dkp::Variant& v = variants_[index];
  std::vector<uint8_t> blob;
  {
    std::lock_guard<std::mutex> lock(preloaded_mutex_);
    if (auto it = preloaded_.find(index); it != preloaded_.end()) {
      blob = std::move(it->second);
      preloaded_.erase(it);
    }
  }
  bool ok = !blob.empty();
  if (!ok) {
    std::lock_guard<std::mutex> lock(read_);
    ok = ReadBlob(file_, v, blob);
    std::lock_guard<std::mutex> s(stats_mutex_);
    stats_.bytes_read += v.size;
  }
  const DkShader* shader = ok ? code.Load(blob.data(), blob.size(), "game shader") : nullptr;
  std::lock_guard<std::mutex> s(stats_mutex_);
  if (!shader) {
    failed_[index] = 1;
    if (++stats_.errors <= 8)
      REXLOG_ERROR("[dk] shaders: variant {} (spec {:08X}, mask60 {:08X}, {}) of the DKSH package {}: {}", index,
                   v.spec, v.mask60, v.kind == dkp::kGeneric ? "generic" : "specialized", path_.string(),
                   ok ? "code memory refused it" : "read or hash failed");
    return nullptr;
  }
  ++stats_.loaded;
  shaders_[index] = shader;
  return shader;
}

const DkShader* LibraryDksh::Variant(uint32_t entry, uint32_t spec, uint32_t mask60, sa2::dk::CodeArena& code,
                                     bool* generic_out) {
  if (generic_out) *generic_out = false;
  if (!file_ || entry >= entries_.size()) return nullptr;
  const dkp::Entry& e = entries_[entry];
  const bool vertex = e.facts & dkp::kFactVertex;
  const uint32_t key_spec = spec & e.spec_mask;
  const uint32_t key_60 = vertex ? mask60 : mask60 & dkp::kRasterKeyBits;
  const uint32_t found = dkp::FindVariant(e, variants_, dkp::kSpecialized, key_spec, key_60);
  if (found != UINT32_MAX) {
    if (const DkShader* s = LoadVariant(found, code)) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.specialized;
      return s;
    }
  } else {
    RecordMissing(entry, key_spec, key_60);
  }
  if ((key_spec & dkp::kSpecNeedsTransform) || (!vertex && key_60)) return nullptr;
  const uint32_t generic = dkp::FindVariant(e, variants_, dkp::kGeneric, 0, 0);
  if (generic == UINT32_MAX) return nullptr;
  const DkShader* s = LoadVariant(generic, code);
  if (s) {
    if (generic_out) *generic_out = true;
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.generic;
  }
  return s;
}

uint32_t LibraryDksh::Facts(uint32_t entry) const { return entry < entries_.size() ? entries_[entry].facts : 0; }
uint32_t LibraryDksh::InputsMaskPs(uint32_t entry) const {
  return entry < entries_.size() ? entries_[entry].ps_inputs : 0xFFFFFFFFu;
}
uint32_t LibraryDksh::SpecMask(uint32_t entry) const {
  return entry < entries_.size() ? entries_[entry].spec_mask : 0xFFFFFFFFu;
}
uint32_t LibraryDksh::Kills(uint32_t entry) const { return entry < entries_.size() ? entries_[entry].kills : 2; }

uint32_t LibraryDksh::Preload(std::span<const std::tuple<uint32_t, uint32_t, uint32_t>> keys,
                              const std::atomic<bool>* stop) {
  if (!file_) return 0;
  std::vector<uint32_t> wanted;
  for (const auto& [entry, spec, mask60] : keys) {
    if (entry >= entries_.size()) continue;
    const dkp::Entry& e = entries_[entry];
    const uint32_t key_60 = (e.facts & dkp::kFactVertex) ? mask60 : mask60 & dkp::kRasterKeyBits;
    uint32_t index = dkp::FindVariant(e, variants_, dkp::kSpecialized, spec & e.spec_mask, key_60);
    if (index == UINT32_MAX) index = dkp::FindVariant(e, variants_, dkp::kGeneric, 0, 0);
    if (index != UINT32_MAX) wanted.push_back(index);
  }
  std::sort(wanted.begin(), wanted.end(),
            [this](uint32_t a, uint32_t b) { return variants_[a].offset < variants_[b].offset; });
  wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
  std::FILE* f = std::fopen(path_.string().c_str(), "rb");  // its own handle: never takes read_
  if (!f) return 0;
  uint32_t n = 0;
  for (const uint32_t index : wanted) {
    if (stop && stop->load(std::memory_order_relaxed)) break;
    {
      std::lock_guard<std::mutex> lock(preloaded_mutex_);
      if (preloaded_.count(index)) continue;
    }
    std::vector<uint8_t> blob;
    if (!ReadBlob(f, variants_[index], blob)) continue;  // Variant() reports it when it is used
    std::lock_guard<std::mutex> lock(preloaded_mutex_);
    preloaded_.emplace(index, std::move(blob));
    ++n;
  }
  std::fclose(f);
  std::lock_guard<std::mutex> s(stats_mutex_);
  stats_.preloaded += n;
  return n;
}

void LibraryDksh::RecordMissing(uint32_t entry, uint32_t spec, uint32_t mask60) {
  const auto key = std::make_tuple(entries_[entry].fingerprint, spec, mask60);
  std::lock_guard<std::mutex> lock(missing_mutex_);
  if (!missing_.insert(key).second) return;
  missing_pending_.push_back(key);
  std::lock_guard<std::mutex> s(stats_mutex_);
  if (++stats_.missing <= 16)
    REXLOG_INFO("[dk] shaders: no DKSH variant for shader n{} {:016X} spec {:08X} mask60 {:08X}: generic one used",
                entry, std::get<0>(key), spec, mask60);
}

void LibraryDksh::FlushMissing() {
  std::vector<std::tuple<uint64_t, uint32_t, uint32_t>> pending;
  {
    std::lock_guard<std::mutex> lock(missing_mutex_);
    pending.swap(missing_pending_);
  }
  if (pending.empty() || missing_folder_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(missing_folder_, ec);
  const std::filesystem::path out = missing_folder_ / "missing.txt";
  std::FILE* f = std::fopen(out.string().c_str(), "a");
  if (!f) {
    REXLOG_WARN("[dk] shaders: could not write {}", out.string());
    return;
  }
  for (const auto& [fingerprint, spec, mask60] : pending)
    std::fprintf(f, "%016llX %08X %08X\n", static_cast<unsigned long long>(fingerprint), spec, mask60);
  std::fclose(f);
}

namespace {
std::atomic<LibraryDksh*> g_active_dksh{nullptr};
}  // namespace

LibraryDksh* ActiveLibraryDksh() { return g_active_dksh.load(std::memory_order_acquire); }

bool OpenActiveLibraryDksh(const ShadersNative& library, const std::filesystem::path& mesp) {
  static LibraryDksh dksh;  // lives to the end of the process: its destructor flushes the missing variants
  if (dksh.loaded()) return true;
  std::vector<uint64_t> fingerprints;
  for (uint32_t i = 0;; ++i) {
    const ShaderEntry* entry = library.PerNumber(i);
    if (!entry) break;
    fingerprints.push_back(entry->shader ? entry->shader->fingerprint : 0);
  }
  std::filesystem::path dkp = mesp;
  dkp.replace_extension(".dkp");
  if (!dksh.Open(dkp, mesp, fingerprints, mesp.parent_path() / "cache" / "dk")) return false;
  g_active_dksh.store(&dksh, std::memory_order_release);
  return true;
}

StatsDksh LibraryDksh::Stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

}  // namespace sa2::native
