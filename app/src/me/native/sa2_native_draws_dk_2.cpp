// SA2 - native renderer on deko3d: the second half of the draws class (DrawsDkImpl, sa2_native_draws_dk.h), the
// translation of lines 8158-14763 of sa2_native_draws.cpp: the payload and constant audits, the upload buffers, the
// pipelines file (its prewarm list), the descriptors, the textures, the vertex input, BeginPass, the index
// conversions, the shader variants, the fixed state, PipelineFor, the shader preload and DrawsDk::Create.
// Same logic, order and comments as the Vulkan file; what changed meaning is marked [dk]. Members:
// sa2_native_draws_dk_2.inc. Contract: ds2-deko3d docs/pesquisas/traducao/08-contrato-sa2-dk.md; what was left
// out and why: 06-draws-2.md, section "Fase C".

// Headers the class members need (sa2_native_draws_dk_2.inc), before the class header. sa2_native_dk.h first: it
// brings the Vulkan plain types in the order the SDK expects (me_resolution_scale.h alone breaks vulkan_enums.hpp).
#include "sa2_native_dk.h"

#include <malloc.h>  // mallinfo: the heap in the pipelines file lines

#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>

#include <rex/filesystem.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/thread.h>

#include "../me_vertex_fetch_selection.h"
#include "../sa2_render_mode.h"
#include "me_ds2_shader_light.h"
#include "me_edram_ownership.h"
#include "me_raster_state.h"
#include "me_rectangle_spirv.h"
#include "me_resolution_scale.h"
#include "sa2_native_hooks.h"
#include "sa2_native_shaders.h"
#include "sa2_native_vertex_cache.h"
#include "sa2_native_vertices_dedupe.h"
#include "sa2_shader_library.h"
#include "sa2_dk_state_tables.h"
#include "sa2_shader_library_dk.h"
#include "sa2_waits_hitch.h"

#include "sa2_native_draws_dk.h"

#if SA2_DK_AVAILABLE

// The Vulkan file's cvars this half reads (it stays in the same binary: declared, never redefined), and the
// deko3d ones it defines.
REXCVAR_DEFINE_BOOL(sa2_dk_upload_uncached, false, "SA2 renderer",
                    "deko3d renderer: the upload buffers (vertices, indices, constants, texture data) in CPU-uncached "
                    "memory (CpuUncached | GpuCached, write-combined, no flush before submitting) instead of "
                    "CPU-cached memory flushed in BeforeSend. sa2_native_upload_memory 1 or 2 takes precedence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, sa2_cold_startup);
REXCVAR_DECLARE(bool, sa2_native_pipeline_cache_save_guard);
REXCVAR_DECLARE(int32_t, sa2_native_pipeline_cache_save_fail_test);
REXCVAR_DECLARE(int32_t, sa2_native_upload_memory);
REXCVAR_DECLARE(int32_t, ds2_vertex_cache_mb);
REXCVAR_DECLARE(int32_t, ds2_vertex_cache_hash);
REXCVAR_DECLARE(bool, ds2_vertex_cache_verify);
REXCVAR_DECLARE(bool, sa2_shared_native_cache);
REXCVAR_DECLARE(bool, sa2_native_diag_memory_upload);
REXCVAR_DECLARE(int32_t, sa2_diag_texture_slots);
REXCVAR_DECLARE(bool, sa2_native_constants_ubo);
REXCVAR_DECLARE(bool, sa2_native_cache_textures_between_frames);
REXCVAR_DECLARE(bool, sa2_native_mipmaps);
REXCVAR_DECLARE(int32_t, sa2_native_textures_mb_max);
REXCVAR_DECLARE(bool, sa2_native_normalized_remap);
REXCVAR_DECLARE(int32_t, sa2_native_texture_interval_max);
REXCVAR_DECLARE(bool, sa2_native_adaptive_texture);
REXCVAR_DECLARE(bool, ds2_resolved_not_compressed);
REXCVAR_DECLARE(std::string, ds2_diag_fetch_base);
REXCVAR_DECLARE(int32_t, sa2_native_fingerprints_sampling);
REXCVAR_DECLARE(int32_t, sa2_native_fingerprints_kb_frame);
REXCVAR_DECLARE(bool, ds2_texture_lazy_fingerprint);
REXCVAR_DECLARE(bool, ds2_texture_fused_untile);
REXCVAR_DECLARE(bool, sa2_native_diag_reuse);
REXCVAR_DECLARE(bool, sa2_native_tile_fast);
REXCVAR_DECLARE(int32_t, sa2_native_filter_aniso);
REXCVAR_DECLARE(int32_t, sa2_anisotropic_native);
REXCVAR_DECLARE(double, ds2_texture_lod_bias);
REXCVAR_DECLARE(bool, sa2_native_ps_alpha_only);
REXCVAR_DECLARE(bool, sa2_native_no_ps_no_color);
REXCVAR_DECLARE(bool, sa2_native_pass_shadows_no_load);
REXCVAR_DECLARE(bool, sa2_native_vs_pruned_outputs);
REXCVAR_DECLARE(bool, sa2_native_indices_neon);

namespace sa2::native {

// The DKSH package (region 07 / me_native_system defines it; nullptr until it is open).
LibraryDksh* ActiveLibraryDksh();

// ======================================== part A ========================================
// ---- Part A: lines 8158-9333 of sa2_native_draws.cpp ---------------------------------------------------------
// The integrator needs, at file scope: #include <malloc.h> (Switch, HeapState), <filesystem>, <fstream>, <thread>,
// <rex/filesystem.h>, <rex/thread.h>, "sa2_shader_library_dk.h", and extern "C" fake_heap_start / fake_heap_end
// (declared below, Switch only).



#if REX_PLATFORM_SWITCH
extern "C" char* fake_heap_start;  // newlib's heap bounds, set by libnx at start-up
extern "C" char* fake_heap_end;
#endif

// The DKSH package the draws take their shaders from (region 07 / me_native_system defines it; nullptr = none).

// [dk] The deko3d build keeps its own pipelines file next to the Vulkan one (same "NFPC" format, an empty cache
// part), so switching backends never overwrites the Vulkan driver cache.
static constexpr const char* kFilePipelinesDk = "sa2_dk_pipelines.bin";

bool DrawsDkImpl::ResolveFetchAuditEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("SA2_NATIVE_AUDIT_RESOLVE_FETCH");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

void DrawsDkImpl::AuditFetchResolved(uint32_t base, const uint32_t* fetch, const ImageNativeDk& image) {
  if (!ResolveFetchAuditEnabled()) return;
  if (me_resolve_fetch_contracts_.size() >= 256) {
    if (!me_resolve_fetch_cap_logged_) {
      me_resolve_fetch_cap_logged_ = true;
      REXLOG_INFO("[native] resolved fetch audit reached 256-contract cap at frame {}; "
                  "later absence of contracts is not evidence of matching layouts", frame_);
    }
    return;
  }
  const uint32_t guest_format = fetch[1] & 63u;
  const uint32_t width = (fetch[2] & 8191u) + 1;
  const uint32_t height = ((fetch[2] >> 13) & 8191u) + 1;
  const auto base_format = [](uint32_t value) {
    return uint32_t(rex::graphics::GetBaseFormat(rex::graphics::xenos::TextureFormat(value)));
  };
  const uint32_t contract[] = {base, guest_format, image.resolved_guest_format,
      uint32_t(image.format), width, height, image.width, image.height,
      me_resolve_fetch_vs_, uint32_t(me_resolve_fetch_ps_), me_resolve_fetch_sampler_,
      fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]};
  if (!me_resolve_fetch_contracts_.insert(XXH3_64bits(contract, sizeof(contract))).second) return;
  REXLOG_INFO("[native] resolved fetch contract frame {} VS n{} PS n{} sampler {}: "
              "base {:08X}, fetch guest {} {}x{} pitch {}, latest resolve guest {} "
              "host {} {}x{}, exact guest-format match {}, SDK base-format match {}, "
              "logical extent match {}, swizzle {:03X}, endian {}; "
              "diagnostic only, no reinterpretation or correction",
              frame_, me_resolve_fetch_vs_, me_resolve_fetch_ps_, me_resolve_fetch_sampler_,
              base, guest_format, width, height, ((fetch[0] >> 22) & 511u) << 5,
              image.resolved_guest_format, uint32_t(image.format), image.width, image.height,
              guest_format == image.resolved_guest_format,
              image.resolved_guest_format < 64 && base_format(guest_format) == base_format(image.resolved_guest_format),
              width == image.width && height == image.height,
              (fetch[3] >> 1) & 4095u, (fetch[1] >> 6) & 3u);
}

bool DrawsDkImpl::PayloadAuditEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("SA2_NATIVE_AUDIT_PAYLOAD_STABILITY");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

bool DrawsDkImpl::StartPayloadAudit(uint32_t kind, uint64_t bytes) {
  if (!PayloadAuditEnabled() || !me_payload_hdr_ || !bytes) return false;
  constexpr uint64_t kByteCap = 128ull << 20;
  if (me_payload_checked_[kind] >= 256 || bytes > kByteCap - me_payload_bytes_[kind]) {
    if (++me_payload_skipped_[kind] == 1) ReportPayloadAudit("limit/oversized");
    return false;
  }
  ++me_payload_checked_[kind];
  me_payload_bytes_[kind] += bytes;
  return true;
}

void DrawsDkImpl::ReportPayloadAudit(const char* reason) {
  if (!PayloadAuditEnabled()) return;
  REXLOG_INFO("[native] payload stability {}: texture levels {} / {} bytes, source changes {}, "
              "staged disagreements {}, cap skips {}; vertex copies {} / {} bytes, source changes {}, "
              "staged disagreements {}, cap skips {}; independent caps 256/128MiB, afterHDR {}, "
              "full USED bytes only, invalid audit ranges {}; no coverage of async vertices, texture cache hits, "
              "cache fingerprint-to-level gaps, worker snapshots, 3D textures or LOAD_ALU; "
              "zero discrepancies is not proof of no data races",
              reason, me_payload_checked_[0], me_payload_bytes_[0], me_payload_changed_[0],
              me_payload_staged_[0], me_payload_skipped_[0], me_payload_checked_[1], me_payload_bytes_[1],
              me_payload_changed_[1], me_payload_staged_[1], me_payload_skipped_[1], me_payload_hdr_,
              me_payload_invalid_);
}

void DrawsDkImpl::FinalizePayloadAudit(uint32_t kind, uint64_t address, uint64_t bytes, uint64_t before,
                                       uint64_t after, uint64_t staged) {
  if (before != after) ++me_payload_changed_[kind];
  if (staged != before || staged != after) ++me_payload_staged_[kind];
  if ((before != after || staged != before || staged != after) && ++me_payload_logged_[kind] <= 8) {
    REXLOG_WARN("[native] payload stability {} discrepancy: address {:08X}, {} full used bytes, "
                "frame {} VS {} PS {}, before {:016X} staged {:016X} after {:016X}; measurement only",
                kind ? "vertex" : "texture-level", address, bytes, frame_, me_payload_vs_,
                me_payload_ps_, before, staged, after);
  }
  if (me_payload_checked_[kind] == 1 || me_payload_checked_[kind] % 32 == 0)
    ReportPayloadAudit("coverage");
}

bool DrawsDkImpl::SameAsUploaded(const uint8_t* last, const uint8_t* copy, const uint32_t* current, uint32_t bytes,
                                 bool from_upload) {
  if (from_upload && sa2::enh::VerifyMode()) CheckUploadedCopy(last, copy, bytes, "constants");
  return std::memcmp(last, current, bytes) == 0;
}

bool DrawsDkImpl::SameSharedUploaded(const uint8_t* uploaded, const uint32_t* shared, size_t bytes) {
  if (sa2::enh::VerifyMode()) {
    CheckUploadedCopy(uploaded, reinterpret_cast<const uint8_t*>(shared_previous_), uint32_t(bytes), "shared block");
  }
  return Equal(shared, uploaded, bytes);
}

void DrawsDkImpl::CheckUploadedCopy(const uint8_t* uploaded, const uint8_t* copy, uint32_t bytes, const char* what) {
  sa2::enh::VerifyCheck();
  if (std::memcmp(uploaded, copy, bytes) == 0) return;
  sa2::enh::VerifyDifference();
  compare_uploaded_failed_ = true;
  REXLOG_ERROR("[native] DIFFERENCE: an uploaded {} ({} bytes) differs from its CPU-side copy; "
               "sa2_cpu_ring_compare_uploaded off for the session", what, bytes);
}

void DrawsDkImpl::AuditCacheConstants(bool vs, uint32_t shader, uint64_t generation, const uint32_t* current,
                                      VkDeviceSize offset, uint32_t bytes) {
  static const bool enabled = [] {
    const char* value = std::getenv("SA2_NATIVE_AUDIT_CONSTANT_CACHE");
    return value && std::strcmp(value, "1") == 0;
  }();
  constexpr uint64_t kLimit = 20000;
  if (!enabled || !me_constant_audit_hdr_ || !bytes ||
      me_constant_audit_checked_[0] + me_constant_audit_checked_[1] >= kLimit) return;
  const uint32_t bank = vs ? 0 : 1;
  if (offset > used_upload_ || bytes > used_upload_ - offset) {
    REXLOG_ERROR("[native] constant cache audit invalid {} offset {} bytes {} used {}",
                 vs ? "VS" : "PS", offset, bytes, used_upload_);
    return;
  }
  ++me_constant_audit_checked_[bank];
  me_constant_audit_bytes_[bank] += bytes;
  const uint8_t* uploaded = upload_data_ + offset;
  if (std::memcmp(uploaded, current, bytes) != 0) {
    if (++me_constant_audit_mismatches_[bank] <= 8) {
      uint32_t word = 0, previous = 0;
      for (; word < bytes / 4; ++word) {
        std::memcpy(&previous, uploaded + word * 4, 4);
        if (previous != current[word]) break;
      }
      REXLOG_WARN("[native] constant cache MISMATCH {} n{} generation {} epoch {} "
                  "c{}.{} uploaded {:08X} current {:08X} bytes {}",
                  vs ? "VS" : "PS", shader, generation, epoch_upload_, word / 4,
                  word % 4, previous, current[word], bytes);
    }
  }
  const uint64_t checked = me_constant_audit_checked_[0] + me_constant_audit_checked_[1];
  if (checked == 1 || (checked & 2047) == 0 || checked == kLimit) {
    REXLOG_INFO("[native] constant cache audit after HDR: VS {} comparisons / {} mismatches / {} bytes; "
                "PS {} comparisons / {} mismatches / {} bytes; limit {}{}",
                me_constant_audit_checked_[0], me_constant_audit_mismatches_[0], me_constant_audit_bytes_[0],
                me_constant_audit_checked_[1], me_constant_audit_mismatches_[1], me_constant_audit_bytes_[1],
                kLimit, checked == kLimit ? " reached" : "");
  }
}

bool DrawsDkImpl::Reserve(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& offset) {
  const VkDeviceSize start = (used_upload_ + alignment - 1) & ~(alignment - 1);
  if (start + bytes > kUploadSize) {
    offset = 0;
    return false;  // cannot happen: Draw checks the space first
  }
  offset = start;
  used_upload_ = start + bytes;
  megabytes_bytes_ += bytes;
  return true;
}

bool DrawsDkImpl::ReserveMultiple(VkDeviceSize bytes, VkDeviceSize multiple, VkDeviceSize& offset) {
  const VkDeviceSize start = (used_upload_ + multiple - 1) / multiple * multiple;
  if (start + bytes > kUploadSize) {
    offset = 0;
    return false;  // cannot happen: Draw checks the space first
  }
  offset = start;
  used_upload_ = start + bytes;
  megabytes_bytes_ += bytes;
  return true;
}

// The pipelines file (cache/sa2_native_pipelines.bin): the Vulkan cache and the prewarm list.
// [dk] cache/sa2_dk_pipelines.bin: the same format with only the prewarm list (the inventory of the DKSH variants
// the game uses, ds2-deko3d tools/variantes). Diagnostic and cold-start paths get their own deko3d names too.
std::filesystem::path DrawsDkImpl::PathFilePipelines() {
  // Isolate diagnostic runs from the shared driver cache without deleting it.
  // MoltenVK may compile every stored library while loading initial data.
  static const char* const diagnostic_path = std::getenv("SA2_NATIVE_PIPELINE_CACHE_PATH");
  if (diagnostic_path && *diagnostic_path) {
    std::filesystem::path path(diagnostic_path);
    path += ".dk";
    return path;
  }
  // Cold-start test (sa2_cold_startup): a separate file that the test cycle deletes first, so the
  // run starts with no pipeline cache and no prewarm list, without touching the normal cache.
  if (REXCVAR_GET(sa2_cold_startup))
    return rex::filesystem::GetExecutableFolder() / kFolderCache / "cold_dk.bin";
  return rex::filesystem::GetExecutableFolder() / kFolderCache / kFilePipelinesDk;
}

void DrawsDkImpl::ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const std::streamoff bytes = file ? std::streamoff(file.tellg()) : 0;
  if (bytes > 0 && bytes < (std::streamoff(256) << 20)) {
    data.resize(size_t(bytes));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(bytes))) {
      data.clear();
    }
  }
}

// Reads the pipelines file and splits it into the cache and the list. If it does not exist yet, the two
// older files (next to the NRO), which are deleted when the new one is written.
// [dk] If the deko3d file does not exist yet, the list (only) is seeded from the Vulkan file
// (cache/sa2_native_pipelines.bin), which is never deleted or rewritten here: old_files_ then means "the list came
// from the Vulkan file" and makes the first save write the deko3d file.
void DrawsDkImpl::ReadFilePipelines(std::vector<uint8_t>& cache, std::vector<uint8_t>& list) {
  std::vector<uint8_t> all;
  ReadWholeFile(PathFilePipelines(), all);
  bool from_vulkan = false;
  if (all.empty() && !REXCVAR_GET(sa2_cold_startup)) {
    const char* const diagnostic = std::getenv("SA2_NATIVE_PIPELINE_CACHE_PATH");
    if (!diagnostic || !*diagnostic) {
      ReadWholeFile(rex::filesystem::GetExecutableFolder() / kFolderCache / kFilePipelines, all);
      from_vulkan = !all.empty();
    }
  }
  if (!all.empty()) {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t bytes_list = 0;
    uint64_t bytes_cache = 0;
    if (all.size() >= kHeaderFilePipelines) {
      std::memcpy(&magic, all.data(), 4);
      std::memcpy(&version, all.data() + 4, 4);
      std::memcpy(&bytes_list, all.data() + 8, 8);
      std::memcpy(&bytes_cache, all.data() + 16, 8);
    }
    if (magic != kMagicFilePipelines || version != kVersionFilePipelines ||
        bytes_list > all.size() || bytes_cache > all.size() ||
        kHeaderFilePipelines + bytes_list + bytes_cache != all.size()) {
      REXLOG_WARN("[native] C6: pipeline file from another version or damaged: starting from scratch");
      return;
    }
    const auto start = all.begin() + kHeaderFilePipelines;
    list.assign(start, start + std::ptrdiff_t(bytes_list));
    cache.assign(start + std::ptrdiff_t(bytes_list), all.end());
    old_files_ = from_vulkan && !list.empty();
    return;
  }
}

// On-disk pipeline cache (derived from the game's shaders: not distributed). If the driver rejects it
// (another driver or corrupt data), it starts from scratch.
// [dk] No driver pipeline cache: only the prewarm list is read (and later written), and a cache part read from
// the Vulkan file is dropped at once.
void DrawsDkImpl::LoadCachePipelines() {
  const std::filesystem::path path = PathFilePipelines();
  std::vector<uint8_t> data;
  ReadFilePipelines(data, read_list_);
  bytes_cache_saved_ = 0;
  cache_saved_ = std::chrono::steady_clock::now();
  REXLOG_INFO("[native] C6: pipeline cache not available [dk] (no driver cache; prewarm list {} KB read from {}{})",
              read_list_.size() >> 10, path.string(),
              old_files_ ? ", seeded from the Vulkan pipelines file" : "");
  data = {};
  written_cache_ = {};
  if (old_files_) {
    // it came from the Vulkan file: moved to the deko3d one on the first save even if nothing changed
    pipelines_no_save_ = 1;
    list_no_save_ = 1;
  }
  LoadPipelinesList();  // the prewarm list
}

// Time each save costs the calling (ring) thread, for the ds2_pipeline_cache_copy_off_ring A/B.
void DrawsDkImpl::SaveCachePipelines() {
  const auto before = std::chrono::steady_clock::now();
  SaveCachePipelinesTimed();
  if (timed_saves_cache_++ < 16) {
    REXLOG_INFO("[native] C6: pipeline cache save took {:.1f} ms on the ring thread ({})",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count(),
                "[dk] prewarm list only");
  }
}

void DrawsDkImpl::SaveCachePipelinesTimed() {
  // [dk] SaveCachePipelinesOffRing (ds2_pipeline_cache_copy_off_ring) needs a driver cache: CopyOffRing() is false.
  if (!REXCVAR_GET(sa2_native_pipeline_cache_save_guard)) {
    SaveCachePipelinesNoGuard();
    return;
  }
  const uint32_t pipelines_before = pipelines_no_save_;
  const uint32_t list_before = list_no_save_;
  try {
    SaveCachePipelinesNoGuard();
  } catch (const std::bad_alloc&) {
    // Nothing reached the writer thread: the unsaved counts come back and the next attempt is the usual
    // one (64 new pipelines or a minute; cache_saved_ was already updated).
    pipelines_no_save_ = pipelines_before;
    list_no_save_ = list_before;
    if (failed_saves_cache_++ < 16) {
      REXLOG_WARN("[native] C6: pipeline cache save postponed, out of memory [dk] (prewarm list only; "
                  "failure {}; {})",
                  failed_saves_cache_, HeapState());
    }
  }
}

void DrawsDkImpl::SaveCachePipelinesNoGuard() {
  cache_saved_ = std::chrono::steady_clock::now();
  // The prewarm list is saved even if the cache does not grow (a new pipeline whose shaders were already
  // in it does not make it grow). [dk] There is no cache part: pipelines_no_save_ only paces the saves.
  std::vector<uint8_t> list;
  if (list_no_save_) {
    list_no_save_ = 0;
    list = SerializePipelinesList();
  }
  pipelines_no_save_ = 0;
  // sa2_native_pipeline_cache_save_fail_test: the guard's test hook, now on the list (the only allocation).
  if (!list.empty() && REXCVAR_GET(sa2_native_pipeline_cache_save_guard) &&
      failed_saves_cache_ < uint32_t(REXCVAR_GET(sa2_native_pipeline_cache_save_fail_test))) {
    throw std::bad_alloc();
  }
  if (list.empty()) {
    return;
  }
  // The file is written on its own thread: on the Switch, writing about 2 MB to the SD card from the ring
  // thread stalled it on every save. If the previous one has not been written yet, this one replaces it.
  std::lock_guard<std::mutex> latch(writer_mutex_);
  if (!writer_cache_.joinable()) {
    writer_cache_ = std::thread([this] {
      rex::thread::set_current_thread_name("SA2 cache de pipelines");
      WriterCacheMain();
    });
  }
  writer_list_ = std::move(list);
  pending_writer_ = true;
  writer_warning_.notify_one();
}

// Pipeline cache writer thread. Sleeping or continuing is decided with the lock held.
// [dk] It writes the prewarm list (writer_data_ and writer_take_cache_ stay empty).
void DrawsDkImpl::WriterCacheMain() {
  std::vector<uint8_t> list;  // the prewarm one
  for (;;) {
    {
      std::unique_lock<std::mutex> latch(writer_mutex_);
      writer_warning_.wait(latch, [this] { return pending_writer_ || writer_stop_; });
      if (!pending_writer_) {
        return;  // stop, nothing pending
      }
      writer_data_ = {};
      list = std::move(writer_list_);
      writer_list_ = {};
      writer_take_cache_ = false;
      pending_writer_ = false;
    }
    if (list.empty()) {
      continue;
    }
    written_list_ = std::move(list);
    WriteFilePipelines();
  }
}

// The heap for the pipeline cache lines: on the Switch newlib's (mallinfo walks it: save paths only).
std::string DrawsDkImpl::HeapState() {
#if REX_PLATFORM_SWITCH
  const struct mallinfo mi = mallinfo();
  const size_t total = size_t(fake_heap_end - fake_heap_start);
  char text[160];
  std::snprintf(text, sizeof(text), "heap %zu MB: arena %zu MB, in use %zu MB, free in arena %zu MB in %zu chunks",
                total >> 20, size_t(mi.arena) >> 20, size_t(mi.uordblks) >> 20, size_t(mi.fordblks) >> 20,
                size_t(mi.ordblks));
  return text;
#else
  return "heap not measured on this platform";
#endif
}

// cache/sa2_native_pipelines.bin with the list and the cache, written to a temporary file that is then
// renamed. Writer thread only. [dk] cache/sa2_dk_pipelines.bin, with an empty cache part; nothing of the Vulkan
// build's files is deleted.
void DrawsDkImpl::WriteFilePipelines() {
  const auto before = std::chrono::steady_clock::now();
  const std::filesystem::path path = PathFilePipelines();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::filesystem::path temporal = path;
  temporal += ".tmp";
  {
    const uint32_t magic = kMagicFilePipelines;
    const uint32_t version = kVersionFilePipelines;
    const uint64_t bytes_list = written_list_.size();
    const uint64_t bytes_cache = 0;
    std::ofstream file(temporal, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(&magic), 4) ||
        !file.write(reinterpret_cast<const char*>(&version), 4) ||
        !file.write(reinterpret_cast<const char*>(&bytes_list), 8) ||
        !file.write(reinterpret_cast<const char*>(&bytes_cache), 8) ||
        !file.write(reinterpret_cast<const char*>(written_list_.data()), std::streamsize(bytes_list))) {
      return;
    }
  }
  error.clear();
  std::filesystem::rename(temporal, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    std::filesystem::rename(temporal, path, error);
  }
  if (!error && old_files_) {
    old_files_ = false;  // [dk] the seeded list now lives in the deko3d file; the Vulkan file stays as it is
  }
  if (saved_cache_++ < 8) {
    REXLOG_INFO("[native] C6: pipelines saved ({} KB of cache and {} in the prewarm list, in {} ms, from "
                "its thread{})",
                written_cache_.size() >> 10,
                written_list_.size() >= kHeaderList
                    ? (written_list_.size() - kHeaderList) / sizeof(RegisterPipeline)
                    : 0,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                    .count(),
                error ? ", could not rename" : "");
  }
}

void DrawsDkImpl::StopWriterCache() {
  {
    std::lock_guard<std::mutex> latch(writer_mutex_);
    writer_stop_ = true;
    writer_warning_.notify_one();
  }
  if (writer_cache_.joinable()) {
    writer_cache_.join();
  }
}

// One upload buffer per work slot of the render target code: the previous frame's may still be in use
// on the GPU while the other is filled.
bool DrawsDkImpl::CreateUpload() {
  for (sa2::dk::Arena& s : uploads_) {
    const bool created = CreateBufferUpload(s);
    upload_memory_ = nullptr;
    upload_data_ = nullptr;
    if (!created) {
      return false;
    }
  }
  REXLOG_INFO("[native] C6: upload buffer in memory type {:#x} [dk] ({}, {}; sa2_native_upload_memory = {}, "
              "sa2_dk_upload_uncached = {})",
              upload_type_, cached_upload_ ? "with CPU cache" : "without CPU cache",
              coherent_upload_ ? "coherent, no publishing needed" : "published before submitting",
              REXCVAR_GET(sa2_native_upload_memory), REXCVAR_GET(sa2_dk_upload_uncached));
  MeasureMemoryUpload();
  CreateSharedSeparate();
  CreateVertexCache();
  UseSlot(0);
  return true;
}

// ds2_vertex_cache_mb: one buffer in the upload buffer's memory type (written by the CPU, read by the GPU). On a
// non-coherent type (the Switch: CPU-cached, sa2_native_upload_memory = 1) each new copy is published right after it
// is written; in steady play there are few. On any failure the cache stays off and the log says why.
// [dk] One DkMemBlock with the upload buffers' flags; "published" = sa2::dk::FlushCpu of the copied range.
void DrawsDkImpl::CreateVertexCache() {
  const int32_t mb = REXCVAR_GET(ds2_vertex_cache_mb);
  if (mb <= 0) return;
  const uint32_t size = uint32_t(mb) << 20;
  vcache_buf_ = sa2::dk::MakeBlock(size, upload_type_, "vertex cache");
  if (!vcache_buf_) {
    REXLOG_WARN("[native] vertex cache: could not allocate or map {} MB; off", mb);
    return;
  }
  vcache_.Init(vcache_buf_.size);
  vcache_active_ = true;
  vcache_hash_ = REXCVAR_GET(ds2_vertex_cache_hash);
  vcache_verify_ = REXCVAR_GET(ds2_vertex_cache_verify);
  vcache_report_ = std::chrono::steady_clock::now();
  REXLOG_INFO("[native] vertex cache: {} MB in memory type {:#x} ({}; {} chunks of {} MB), fingerprint {}{}", mb,
              upload_type_, coherent_upload_ ? "coherent" : "each copy published", VertexCache::kChunks,
              vcache_.chunk_size() >> 20, vcache_hash_ ? "XXH3 of every byte" : "sampled",
              vcache_verify_ ? ", verified on every hit" : "");
}

// Every 10 s: what the vertex cache saved (deltas).
void DrawsDkImpl::ReportVertexCache() {
  if (!vcache_active_) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - vcache_report_ < std::chrono::seconds(10)) return;
  vcache_report_ = now;
  const VertexCache::Figures f = vcache_.figures();
  const VertexCache::Figures& o = vcache_reported_;
  SA2_REPORT_RING("[native] vertex cache (10 s): {} lookups, {} hits ({:.1f} %; {} without a fingerprint), {:.1f} MB "
                  "not copied; {} copied in "
                  "({:.1f} MB); first sightings {}, changed {} (became dynamic {}, dynamic misses {}); full {}, "
                  "chunks recycled {} retired {}; table replacements {}; verified {} stale {}",
                  f.lookups - o.lookups, f.hits - o.hits,
                  f.lookups > o.lookups ? 100.0 * double(f.hits - o.hits) / double(f.lookups - o.lookups) : 0.0,
                  f.fast_hits - o.fast_hits,
                  double(f.bytes_saved - o.bytes_saved) / 1048576.0, f.copies - o.copies,
                  double(f.bytes_copied - o.bytes_copied) / 1048576.0, f.first_sightings - o.first_sightings,
                  f.changed - o.changed, f.became_dynamic - o.became_dynamic, f.dynamic_misses - o.dynamic_misses,
                  f.full - o.full, f.recycled - o.recycled, f.retired - o.retired, f.replaced - o.replaced,
                  f.verified - o.verified, f.stale - o.stale);
  vcache_reported_ = f;
}

// A small CPU-cached buffer per slot, only for the shared constants. If anything fails, it continues as
// before (in the upload buffer) and says so in the log.
// [dk] CpuCached | GpuCached blocks: never coherent, so they are flushed before submitting (BeforeSend).
void DrawsDkImpl::CreateSharedSeparate() {
  shared_separate_ = false;
  if (!REXCVAR_GET(sa2_shared_native_cache)) {
    REXLOG_INFO("[native] C6: shared constants in the upload buffer (sa2_shared_native_cache = false)");
    return;
  }
  for (sa2::dk::Arena& c : shared_bufs_) {
    if (!c.Initialize(uint32_t(kSharedSize), sa2::dk::kReadback, "shared constants")) {
      REXLOG_WARN("[native] C6: could not allocate the shared-constants buffer; they go in the upload buffer");
      return;
    }
    shared_type_ = sa2::dk::kReadback;
  }
  shared_coherent_ = false;
  shared_separate_ = true;
  REXLOG_INFO("[native] C6: shared constants separate, in memory type {:#x} (with CPU cache, {}), {} MB "
              "per slot",
              shared_type_, shared_coherent_ ? "coherent" : "published before submitting",
              kSharedSize >> 20);
}

// [dk] One slot's upload buffer (CreateUpload walks the slots). Vertex, index, uniform and copy source data are
// all read by address, so one DkMemBlock is the whole of VkBuffer + memory + mapping + device address.
// sa2_native_upload_memory: with 1 or 2, a type with or without CPU caching (as in Vulkan); otherwise
// sa2_dk_upload_uncached decides, CPU-cached (flushed in BeforeSend) by default.
bool DrawsDkImpl::CreateBufferUpload(sa2::dk::Arena& upload) {
  const int32_t preference = REXCVAR_GET(sa2_native_upload_memory);
  const bool uncached = preference == 2 || (preference != 1 && REXCVAR_GET(sa2_dk_upload_uncached));
  upload_type_ = uncached ? sa2::dk::kCpuToGpu : sa2::dk::kReadback;
  coherent_upload_ = uncached;
  cached_upload_ = !uncached;
  if (!upload.Initialize(uint32_t(kUploadSize), upload_type_, "upload buffer")) {
    return false;
  }
  upload_memory_ = &upload.block();
  upload_real_size_ = upload.Size();
  upload_data_ = upload.Cpu(0);
  upload_address_ = upload.Gpu(0);
  return upload_address_ != DK_GPU_ADDR_INVALID;
}

// For the sa2_native_upload_memory A/B test: CPU write MB/s into each host-visible memory type and the
// cost of publishing those 8 MB (vkFlushMappedMemoryRanges; in NVK for Tegra, armDCacheClean). Once.
// [dk] The two CPU-visible kinds deko3d has: CpuUncached (write-combined) and CpuCached (dkMemBlockFlushCpuCache).
void DrawsDkImpl::MeasureMemoryUpload() {
  if (!REXCVAR_GET(sa2_native_diag_memory_upload)) {
    return;
  }
  constexpr uint32_t kBytes = uint32_t(8) << 20;
  std::vector<uint8_t> source(static_cast<size_t>(kBytes));
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = uint8_t((i * 131) ^ (i >> 11));
  }
  std::string report;
  for (const uint32_t type : {sa2::dk::kCpuToGpu, sa2::dk::kReadback}) {
    sa2::dk::Block memory = sa2::dk::MakeBlock(kBytes, type, "memory measurement");
    if (!memory) {
      continue;
    }
    using Clock = std::chrono::steady_clock;
    std::memcpy(memory.cpu, source.data(), size_t(kBytes));  // first pass: pages and caches
    const auto before = Clock::now();
    std::memcpy(memory.cpu, source.data(), size_t(kBytes));
    const auto copied = Clock::now();
    const bool coherent = !(type & DkMemBlockFlags_CpuCached);
    if (!coherent) {
      sa2::dk::FlushCpu(memory, 0, kBytes);
    }
    const auto published = Clock::now();
    const double seconds = std::chrono::duration<double>(copied - before).count();
    report += fmt::format(
        "{}type {:#x} ({}{}): {:.0f} MB/s{}", report.empty() ? "" : "; ", type,
        coherent ? "without CPU cache" : "with CPU cache", coherent ? ", coherent" : "",
        seconds > 0 ? 8.0 / seconds : 0.0,
        coherent ? std::string()
                 : fmt::format(", publish {:.2f} ms",
                               std::chrono::duration<double, std::milli>(published - copied).count()));
    sa2::dk::FreeBlock(memory, 0);  // the GPU never saw it
  }
  REXLOG_INFO("[native] C6: CPU write speed per memory type (8 MB): {}",
              report.empty() ? std::string("no measurable types") : report);
}

// [dk] No descriptor set layouts, pools or pipeline layout: the heaps are ranges of the process's one image set
// (sa2::dk::kDescriptorsDraws2D/3D/Cube) and the first kSamplersDraws samplers, sized to kCapacityHeap as the
// Vulkan code requests (deko3d has no per-set limit to query). The constants are always UBOs. Also here: the
// draws' shader code memory, the specialization table (UBO 3) and the DKSH package.
bool DrawsDkImpl::CreateDescriptors() {
  static_assert(kCapacityHeap[3] <= sa2::dk::kSamplersDraws, "the samplers heap is the draws' sampler range");
  for (uint32_t i = 0; i < 4; ++i) {
    REXLOG_INFO("[native] descriptor heap {}: {} supported entries (requested {}) [dk] from descriptor {}",
                i, kCapacityHeap[i], kCapacityHeap[i], i < 3 ? DescriptorOfSlot(i, 0) : DescriptorOfSampler(0));
    heaps_[i].capacity = kCapacityHeap[i];
    if (i == 0) {  // sa2_diag_texture_slots: fewer slots are handed out (the heap itself keeps its size)
      const int32_t diag = REXCVAR_GET(sa2_diag_texture_slots);
      if (diag >= 64 && uint32_t(diag) < heaps_[0].capacity) {
        diag_slots_2d_ = uint32_t(diag);
        REXLOG_INFO("[native] sa2_diag_texture_slots: the 2D heap hands out {} slots", diag);
      }
    }
  }
  // --- Set 4, the constants through dynamic UBOs ------------------------------------------------------
  // [dk] Always UBOs (the deko3d shaders have no buffer pointers): UBO 0 VS, 1 PS and 2 shared, bound by
  // address per draw; sa2_native_constants_ubo is only reported.
  use_ubo_ = true;
  cache_between_frames_ = REXCVAR_GET(sa2_native_cache_textures_between_frames);
  REXLOG_INFO("[native] C6: texture caches across frames (sa2_native_cache_textures_between_frames) = {}",
              cache_between_frames_ ? "YES" : "no");
  mipmaps_ = REXCVAR_GET(sa2_native_mipmaps);
  REXLOG_INFO("[native] C3: texture mip levels (sa2_native_mipmaps) = {}", mipmaps_ ? "YES" : "no");
  textures_mb_max_ = REXCVAR_GET(sa2_native_textures_mb_max);
  REXLOG_INFO("[native] C3: texture cache limit (sa2_native_textures_mb_max) = {} MB{}", textures_mb_max_,
              textures_mb_max_ > 0 ? "" : " (no limit)");
  alignment_ubo_ = DK_UNIFORM_BUF_ALIGNMENT;
  REXLOG_INFO("[native] C6: constants through dynamic UBO (sa2_native_constants_ubo) = YES [dk] (always; the cvar "
              "says {}); alignment {} bytes",
              REXCVAR_GET(sa2_native_constants_ubo) ? "YES" : "no", alignment_ubo_);
  if (!code_arena_.Initialize()) {
    REXLOG_ERROR("[native] C6: [dk] could not allocate the shader code memory: nothing is drawn");
    return false;
  }
  if (!spec_table_.Initialize()) {
    REXLOG_ERROR("[native] C6: [dk] could not allocate the specialization table (UBO 3): nothing is drawn");
    return false;
  }
  dksh_ = ActiveLibraryDksh();
  if (!dksh_ || !dksh_->loaded()) {
    REXLOG_WARN("[native] C6: [dk] no DKSH shader package (sa2_shaders.dkp): draws are rejected until there is one");
  }
  return true;
}

// Slot 0 of each heap: a transparent black texture and a basic sampler, as the emulation does for an
// invalid fetch constant.
bool DrawsDkImpl::CreateEmpty() {
  const std::array<std::pair<DkImageType, uint32_t>, 3> types = {
      {{DkImageType_2D, 1}, {DkImageType_3D, 1}, {DkImageType_Cubemap, 6}}};
  for (uint32_t i = 0; i < 3; ++i) {
    sa2::dk::ImageDesc info;
    info.type = types[i].first;
    info.format = DkImageFormat_RGBA8_Unorm;
    info.width = info.height = 1;
    info.depth_or_layers = types[i].second;
    info.mips = 1;
    info.logical = VK_FORMAT_R8G8B8A8_UNORM;
    empty_[i].image = sa2::dk::CreateImage(info, "empty texture");
    if (empty_[i].image == sa2::dk::kNoImage) {
      return false;
    }
    empty_[i].width = empty_[i].height = 1;
    empty_[i].format = VK_FORMAT_R8G8B8A8_UNORM;
    WriteImage(i, 0, sa2::dk::View(empty_[i].image));
  }
  DkSampler sampler;
  dkSamplerDefaults(&sampler);  // nearest, no mips; lodClampMax 1000 = VK_LOD_CLAMP_NONE
  sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
  sampler_empty_ = DescriptorOfSampler(0);
  WriteSampler(0, sampler);
  return true;
}

// The empty ones are initialized on the first submission: GENERAL and cleared to zero.
// [dk] No layouts and no vkCmdClearColorImage for sampled images: zeros from the upload buffer are copied in
// (dkCmdBufCopyBufferToImage), one texel per layer, then the copy is made visible to the draws.
bool DrawsDkImpl::PrepareEmpty() {
  if (empty_prepared_) {
    return true;
  }
  const DkCmdBuf upload = context_->CommandsUpload();
  if (!upload) {
    return false;
  }
  VkDeviceSize zeros = 0;
  if (!Reserve(6 * 4, 16, zeros)) {
    return false;
  }
  std::memset(upload_data_ + zeros, 0, 6 * 4);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t layers = i == 2 ? 6 : 1;
    Barrier(upload, empty_[i].image, layers);
    const DkCopyBuf source{upload_address_ + zeros, 0, 0};
    const DkImageView view = sa2::dk::View(empty_[i].image);
    const DkImageRect rect{0, 0, 0, 1, 1, layers};
    dkCmdBufCopyBufferToImage(upload, &source, &view, &rect, 0);
    empty_[i].prepared = true;
  }
  sa2::dk::Barrier(upload, sa2::dk::Hazard::UploadToSample);
  empty_prepared_ = true;
  return true;
}

// [dk] The UNDEFINED -> GENERAL transition before a transfer write has nothing to translate: deko3d images have no
// layouts, and an image's memory is only reused after the GPU is done with the previous one (DestroyImage after an
// order). Kept so the callers read as in Vulkan; the copy -> sample dependency is Hazard::UploadToSample.
void DrawsDkImpl::Barrier(DkCmdBuf cmd, sa2::dk::ImageId image, uint32_t layers) {
  (void)cmd;
  (void)image;
  (void)layers;
}

void DrawsDkImpl::WriteImage(uint32_t heap, uint32_t slot, const DkImageView& view) {
  sa2::dk::WriteImageDescriptor(DescriptorOfSlot(heap, slot), view);
}

void DrawsDkImpl::WriteSampler(uint32_t slot, const DkSampler& sampler) {
  sa2::dk::WriteSamplerDescriptor(DescriptorOfSampler(slot), sampler);
}

uint32_t DrawsDkImpl::ReserveSlot(uint32_t heap) {
  auto& m = heaps_[heap];
  if (heap == 0 && diag_slots_2d_ && SlotsUsed(0) + 1 >= diag_slots_2d_) [[unlikely]] {
    return 0;  // sa2_diag_texture_slots: as if the heap ended here
  }
  if (!m.free.empty()) {
    const uint32_t slot = m.free.back();
    m.free.pop_back();
    return slot;
  }
  return m.next < m.capacity ? m.next++ : 0;
}

// ======================================== part B ========================================
// parte B (região 06): EntryFor, KeyEntry, ComputeEntry, CapInterval, PrepareTexture
// (sa2_native_draws.cpp 9333-10384).
// The draws' enhancements (anonymous namespace in the Vulkan file): the same cvars, reported as "<key> (dk)".

// The draw's vertex input, recomputed only when the patched VS changes.
const VerticesEntry* DrawsDkImpl::EntryFor(const SubmissionDraw& p) {
  if (generation_entry_ == p.generation_vs && vs_entry_ == p.vs) {
    if (valid_entry_) {
      ++reused_inputs_;
      return entry_current_;
    }
    // The same input that already failed: no recomputation and no warning.
    ++rejected_;
    ++causes_[cause_entry_];
    return nullptr;
  }
  generation_entry_ = p.generation_vs;
  vs_entry_ = p.vs;
  valid_entry_ = false;
  // Cache: the same VS with the same patched fetches gives the same input.
  const uint64_t key_entry = KeyEntry(p);
  if (key_entry) {
    if (const auto it = inputs_cache_.find(key_entry); it != inputs_cache_.end()) {
      entry_current_ = &it->second;
      valid_entry_ = true;
      cause_entry_ = 0;
      ++inputs_cache_hits_;
      return entry_current_;
    }
  }
  entry_ = VerticesEntry{};
  const uint64_t rejected_before = rejected_;
  const auto before_entry = std::chrono::steady_clock::now();
  const VerticesEntry* entry = ComputeEntry(p);
  ns_inputs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - before_entry)
                               .count());
  ++computed_inputs_;
  cause_entry_ = rejected_ != rejected_before ? last_cause_ : 0;
  entry_current_ = &entry_;
  if (entry && key_entry && inputs_cache_.size() < 4096) {
    inputs_cache_.emplace(key_entry, entry_);
  }
  return entry;
}

// Key of the input cache: declarations/fetches plus the FULL loaded program.
// ALU/CF changes can invalidate ordered identity and switch exact selection
// to the unique-candidate fallback even if every fetch word is unchanged.
// Seed with the selected VS; 0 = do not cache.
uint64_t DrawsDkImpl::KeyEntry(const SubmissionDraw& p) const {
  const ShaderEntry& vs = *p.vs;
  const auto& patched = p.vs_microcode;
  if (patched.size() != vs.microcode.size() || vs.elements.size() > 32) {
    return 0;
  }
  std::array<uint32_t, 32 * 6> words;
  size_t n = 0;
  for (const ElementVertex& element : vs.elements) {
    const size_t i = size_t(element.instruction) * 3;
    if (i + 2 >= patched.size()) {
      return 0;
    }
    words[n++] = (uint32_t(element.instruction) << 16) | (uint32_t(element.usage) << 8) |
                    element.usage_index;
    words[n++] = vs.microcode[i];
    words[n++] = vs.microcode[i + 1];
    words[n++] = patched[i];
    words[n++] = patched[i + 1];
    words[n++] = patched[i + 2];
  }
  // sa2_cpu_ring_entry_hash: the program's XXH3 was already computed by the ring when it identified this load
  // (the same function over the same words); the first uses, and every use in verify mode, recompute and compare.
  uint64_t program_hash;
  if (g_dk_entry_hash.active && entry_hash_ && p.vs_program_hash_set) {
    program_hash = p.vs_program_hash;
    g_dk_entry_hash.UseSampled();
    if (entry_hash_checks_ > 0 || sa2::enh::VerifyMode()) {
      if (entry_hash_checks_ > 0) --entry_hash_checks_;
      sa2::enh::VerifyCheck();
      const uint64_t direct = XXH3_64bits(patched.data(), patched.size_bytes());
      if (direct != program_hash) {
        sa2::enh::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: VS program hash from the ring {:016X} != recomputed {:016X} (VS n{}); "
                     "the old path from now on", program_hash, direct, vs.number);
        entry_hash_ = false;
        program_hash = direct;
      }
    }
  } else {
    program_hash = XXH3_64bits(patched.data(), patched.size_bytes());
  }
  const uint64_t key = XXH3_64bits_withSeed(
      words.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)) ^ program_hash);
  return key ? key : 1;
}

const VerticesEntry* DrawsDkImpl::ComputeEntry(const SubmissionDraw& p) {
  const ShaderEntry& vs = *p.vs;
  entry_.remaps.fill(kRemapIdentity);
  const auto& patched = p.vs_microcode;
  if (patched.size() != vs.microcode.size()) {
    Reject(20, "the ring's VS does not have the container length");
    return nullptr;
  }
  const bool ordered_identity = me::native::VertexShaderIdentityMatches(
      {me::native::ShaderIdentityStage::Vertex, patched},
      {me::native::ShaderIdentityStage::Vertex, vs.microcode}, vs.elements,
      [](const ElementVertex& e) { return e.instruction; });
  uint32_t used_locations = 0;
  for (const ElementVertex& element : vs.elements) {
    const uint32_t register_value = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
    const uint32_t original = vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF;
    const auto selection = me::native::SelectVertexFetch(
        vs.microcode, patched, element.instruction, ordered_identity, vs.elements,
        [](const ElementVertex& e) { return e.instruction; });
    if (!selection) {
      if (selection.failure == me::native::VertexFetchSelectionFailure::Ambiguous) {
        const uint64_t program_hash = XXH3_64bits(patched.data(), patched.size_bytes());
        const uint64_t diagnostic_key = program_hash ^ uint64_t(uintptr_t(p.vs));
        if (me_fetch_ambig_diagnosed_.size() < 16 &&
            me_fetch_ambig_diagnosed_.insert(diagnostic_key).second) {
          REXLOG_WARN("[native] ME ambiguous fetch VS n{} {}{} instr {} temp r{} loaded_hash {:016X} "
                      "words {} (ordered identity false)", vs.number, UsageName(element.usage),
                      element.usage_index, element.instruction, register_value, program_hash, patched.size());
          for (const ElementVertex& e : vs.elements) {
            const size_t i = size_t(e.instruction) * 3;
            if (i + 3 > patched.size()) continue;
            REXLOG_WARN("[native] ME ambiguous VS n{} declared {}{} instr {} selected {:08X} {:08X} {:08X} "
                        "loaded {:08X} {:08X} {:08X}", vs.number, UsageName(e.usage), e.usage_index, e.instruction,
                        vs.microcode[i], vs.microcode[i + 1], vs.microcode[i + 2],
                        patched[i], patched[i + 1], patched[i + 2]);
          }
          uint32_t differences = 0;
          for (size_t i = 0; i < patched.size(); ++i) {
            bool fetch = false;
            for (const ElementVertex& e : vs.elements) {
              const size_t q_decl = size_t(e.instruction) * 3;
              if (i >= q_decl && i - q_decl < 3) { fetch = true; break; }
            }
            if (!fetch && patched[i] != vs.microcode[i] && differences++ < 16)
              REXLOG_WARN("[native] ME ambiguous VS n{} nonfetch word {} loaded {:08X} selected {:08X}",
                          vs.number, i, patched[i], vs.microcode[i]);
          }
          REXLOG_WARN("[native] ME ambiguous VS n{} nonfetch difference count {}", vs.number, differences);
        }
        Reject(21, "ambiguous vertex fetch without ordered VS identity proof");
        return nullptr;
      }
      if (warned_vs_.size() < 16 && warned_vs_.insert(vs.number).second) {
        std::string detail;
        for (const ElementVertex& other : vs.elements) {
          const size_t i = size_t(other.instruction) * 3;
          detail += fmt::format(" {}{}@{}: original r{} op{}, patched r{} op{} {:08X} {:08X} {:08X};",
                                 UsageName(other.usage), other.usage_index, other.instruction,
                                 (vs.microcode[i] >> 12) & 0x3F, vs.microcode[i] & 0x1F,
                                 (patched[i] >> 12) & 0x3F, patched[i] & 0x1F, patched[i],
                                 patched[i + 1], patched[i + 2]);
        }
        REXLOG_WARN("[native] C6 diag: VS n{} without fetch for {}{} (register r{}):{}", vs.number,
                    UsageName(element.usage), element.usage_index, register_value, detail);
      }
      Reject(21, "vertex fetch not found in the patched VS");
      return nullptr;
    }
    const size_t q = selection.word_offset;
    const uint32_t d1 = patched[q + 1], d2 = patched[q + 2];
    const bool mini = ((d1 >> 30) & 0x1) != 0;
    size_t q_base = q;
    if (mini) {
      q_base = SIZE_MAX;
      for (const ElementVertex& other : vs.elements) {
        const size_t i = size_t(other.instruction) * 3;
        if (i < q && (patched[i] & 0x1F) == 0 && !((patched[i + 1] >> 30) & 0x1) &&
            (q_base == SIZE_MAX || i > q_base)) {
          q_base = i;
        }
      }
      if (q_base == SIZE_MAX) {
        Reject(26, "complete base fetch of vfetch_mini not found");
        return nullptr;
      }
    }
    const uint32_t d0_base = patched[q_base], d2_base = patched[q_base + 2];
    const uint32_t slot = ((d0_base >> 20) & 0x1F) * 3 + ((d0_base >> 25) & 0x3);
    const uint32_t format = (d1 >> 16) & 0x3F;
    const uint32_t stride = (d2_base & 0xFF) * 4;
    const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
    const int32_t location = LocationOfUsage(element.usage, element.usage_index);
    if (location < 0 || ((used_locations >> location) & 0x1)) {
      Warn(22, "vertex element without a free location in the shader: skipped");
      continue;
    }
    const uint32_t code = RemapCode(original, d1 & 0xFFF);
    if (code != kRemapIdentity && warnings_swizzle_ < 24) {
      ++warnings_swizzle_;
      REXLOG_INFO("[native] C6: VS n{} {}{} (format {}): original swizzle {:03X}, patched {:03X}, "
                  "remap {:03X}",
                  vs.number, UsageName(element.usage), element.usage_index, format, original,
                  d1 & 0xFFF, code);
    }
    if (((d1 >> 24) & 0x3F) && banjo_exp_logs_ < 16) {  // banjo-nx diagnostic: exp_adjust is not applied
      ++banjo_exp_logs_;
      REXLOG_INFO("[native] vertex fetch with exp_adjust {}: VS n{} location {} format {} signed {} integer {}",
                  int32_t(((d1 >> 24) & 0x3F) << 26) >> 26, vs.number, location, format, (d1 >> 12) & 0x1,
                  (d1 >> 13) & 0x1);
    }
    bool r11g11b10 = false;
    VkFormat vk = AttributeFormat(format, WholeEntry(element.usage), (d1 >> 12) & 0x1,
                                  (d1 >> 13) & 0x1, false, r11g11b10);
    bool repack = false;
    if (vk == VK_FORMAT_UNDEFINED && (format == 16 || format == 17) && !WholeEntry(element.usage) &&
        !((d1 >> 13) & 0x1)) {
      vk = ((d1 >> 12) & 0x1) ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      repack = true;
    }
    if (vk == VK_FORMAT_UNDEFINED) {
      Reject(300 + format, "vertex format not supported yet");
      return nullptr;
    }
    if (r11g11b10) {
      entry_.specialization |= 0x1;
    }
    uint32_t binding = 0;
    while (binding < entry_.bindings.size() && entry_.bindings[binding].slot != slot) {
      ++binding;
    }
    if (binding == entry_.bindings.size()) {
      entry_.bindings.push_back({slot, stride});
    } else if (!entry_.bindings[binding].stride) {
      entry_.bindings[binding].stride = stride;
    }
    if (offset < 0) {
      Reject(24, "negative vertex offset");
      return nullptr;
    }
    entry_.attributes.push_back({uint32_t(location), binding, vk, uint32_t(offset)});
    if (repack) entry_.repacks.push_back({binding, uint32_t(offset), format, bool((d1 >> 12) & 0x1)});
    entry_.remaps[location] = REXCVAR_GET(sa2_native_normalized_remap)
        ? NormalizeRemap(code, r11g11b10 ? 4u : ComponentsVertexFormat(vk)) : code;
    used_locations |= uint32_t(1) << location;
  }
  for (const BindingVertices& binding : entry_.bindings) {
    if (!binding.stride) {
      Reject(25, "vertex stream without stride");
      return nullptr;
    }
  }
  uint64_t fingerprint = XXH3_64bits(entry_.attributes.data(),
                                entry_.attributes.size() * sizeof(AttributeVertices));
  fingerprint = XXH3_64bits_withSeed(entry_.bindings.data(),
                                entry_.bindings.size() * sizeof(BindingVertices), fingerprint);
  // sa2_gpu_identity_remap: every location keeps its component order (unused ones stay at the identity code).
  if (g_dk_identity_remap.active &&
      std::all_of(entry_.remaps.begin(), entry_.remaps.end(), [](uint32_t c) { return c == kRemapIdentity; }))
    entry_.specialization |= kSpecIdentityRemap;
  entry_.fingerprint = fingerprint ^ entry_.specialization;
  valid_entry_ = true;
  return &entry_;
}

// Texture of a fetch constant: heap slot and, if it has to be uploaded, it is left in textures_to_upload_
// with its data already prepared. Base level of 2D textures and cubemaps.
uint32_t DrawsDkImpl::CapInterval(const Texture& t) {
  uint32_t cap = uint32_t(REXCVAR_GET(sa2_native_texture_interval_max));
  if (t.late_changes && REXCVAR_GET(sa2_native_adaptive_texture)) {
    cap = std::min<uint32_t>(cap, t.late_changes >= 3 ? 2u : 4u);
  }
  return cap;
}

void DrawsDkImpl::PrepareTexture(const uint32_t* f, uint32_t& slot, uint32_t& heap,
                                 VkDeviceSize& bytes_upload, bool& punctual_sampling, uint64_t& valid_until,
                                 uint32_t& host_width_out, uint32_t& host_height_out) {
  slot = 0;
  heap = 0;
  host_width_out = 0;  // 0 = unknown (1/size is not written)
  host_height_out = 0;
  punctual_sampling = false;
  valid_until = frame_;  // by default, this frame only
  if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
    return;
  }
  const uint32_t dimension = (f[5] >> 9) & 0x3;
  const bool cube = dimension == uint32_t(xenos::DataDimension::kCube);
  const bool volume = dimension == uint32_t(xenos::DataDimension::k3D);
  if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cube && !volume) {
    Warn(30, "1D textures not supported yet: an empty one is used");
    return;
  }
  heap = cube ? 2 : volume ? 1 : 0;
  const uint32_t layers = cube ? 6 : 1;
  const uint8_t signs = RemappedSigns(f);
  bool signed_value = false;
  for (uint32_t i = 0; i < 4; ++i) {
    signed_value |= ((signs >> (i * 2)) & 0x3) == uint32_t(xenos::TextureSign::kSigned);
  }
  if (signed_value) {
    Warn(31, "signed textures not supported yet: read as unsigned");
    // Mass Effect diagnostic: which formats and sign sets are read unsigned (normal maps?).
    const uint32_t sign_key = (f[1] & 0x3F) | (uint32_t(signs) << 8);
    if (seen_signs_.size() < 48 && seen_signs_.insert(sign_key).second) {
      REXLOG_INFO("[native] C3 sign: format {} signs {:02X} (x{} y{} z{} w{}) swizzle {:03X} base {:08X} {}x{}",
                  f[1] & 0x3F, signs, signs & 3, (signs >> 2) & 3, (signs >> 4) & 3, (signs >> 6) & 3,
                  (f[3] >> 1) & 0xFFF, (f[1] >> 12) << 12, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1);
    }
  }
  const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
  const uint32_t base = (f[1] >> 12) << 12;
  // sa2_native_linear_fetch_not_resolved: only a tiled fetch (fetch word 0, bit 31) can be a resolve's output.
  bool may_be_resolved = ((f[0] >> 31) & 1) || !g_dk_linear_not_resolved.active;
  if (may_be_resolved && REXCVAR_GET(ds2_resolved_not_compressed)) {
    // ds2-nx: Xenos resolves never write block-compressed formats, so a DXT/DXN/CTX fetch at a resolve's address is
    // CPU data that reused the memory (Dark Souls II streams a 128x512 DXT1 texture into 14AE9000, where a 256x152
    // RGBA8 was resolved once at load): read from guest memory instead of rejecting the draw (cause 109).
    switch (f[1] & 0x3F) {
      case 18: case 19: case 20: case 49: case 51: case 52: case 53: case 58: case 59: case 60: case 61:
        may_be_resolved = false;
        break;
      default:
        break;
    }
  }
  if (!may_be_resolved) g_dk_linear_not_resolved.UseSampled();  // (every linear 2D fetch: the use report only)
  static const uint32_t diag_base = uint32_t(std::strtoul(REXCVAR_GET(ds2_diag_fetch_base).c_str(), nullptr, 16));
  if (diag_base && (base & 0x1FFFFFFF) == (diag_base & 0x1FFFFFFF) && logged_diag_fetch_++ < 8) {
    const ImageNativeDk* r = context_->ResolvedTexture(base & 0x1FFFFFFF);
    REXLOG_INFO("[native] ds2_diag_fetch_base {:08X}: fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}; dimension {} "
                "{} format {} size {}x{}; resolved image {} ({}x{}, host {}x{})",
                base, f[0], f[1], f[2], f[3], f[4], f[5], dimension, ((f[0] >> 31) & 0x1) ? "tiled" : "linear",
                f[1] & 0x3F, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, r ? "found" : "none",
                r ? r->width : 0, r ? r->height : 0, r ? r->host_width : 0, r ? r->host_height : 0);
  }
  if (!cube && !volume && may_be_resolved) {
    if (const ImageNativeDk* resolved = context_->ResolvedTexture(base & 0x1FFFFFFF)) {
      AuditFetchResolved(base & 0x1FFFFFFF, f, *resolved);
      resolved = context_->ResolvedTextureForFetch(base & 0x1FFFFFFF, f, *resolved);
      if (!resolved) {
        me_resolved_fetch_failed_this_draw_ = true;
        Reject(109, "resolved logical-size fetch unsupported or GPU copy failed");
        return;
      }
      if (!me::native::IsSingleSample(uint32_t(resolved->sample_count))) {
        me_resolved_fetch_failed_this_draw_ = true;
        Reject(109, "ordinary resolved texture fetch cannot bind a multisample image");
        return;
      }
      // Without this the shadows flicker.
      // With sa2_native_inv_tex_size the shader takes 1/size from the shared constants, and this path
      // used to return without reporting the size: 0 was written and the tfetch offsets (the taps of the
      // shadow map's PCF filter) all landed on the same texel. The filtering disappeared and the edge
      // shimmered as the camera moved. Resolved textures report their size like any other.
      host_width_out = resolved->width;
      host_height_out = resolved->height;
      // One trace per size, to check in the log that the 1/size constant of resolved textures is no longer
      // 0 (that was the cause of the shadow flicker).
      if (warnings_inv_resolved_size_.insert(uint64_t(resolved->width) << 32 | resolved->height).second) {
        REXLOG_INFO("[native] C3: resolved texture {}x{}: 1/size = {:.6f}, {:.6f}",
                    resolved->width, resolved->height, 1.0f / float(resolved->width),
                    1.0f / float(resolved->height));
      }
      if (IsDepth(resolved->format) || resolved->resolved_depth_guestspace) {
        // Depth copy (k_24_8): depth comes out in R and the fetch constant's swizzle distributes it. No
        // filtering: it is read as is.
        slot = SlotView(resolved->image, resolved->format, swizzle, kSwizzleRRRR);
        punctual_sampling = true;
        // This used to be UINT64_MAX, and that is what forced the cross-frame cache off.
        // A resolved texture is a render target the game rewrites every frame. Saying its descriptor slot is
        // valid "until the generation changes" meant that, with the CPU two frames ahead (3 work slots), an
        // old view was reused: that was a smeared text overlay that kept
        // sa2_native_cache_textures_between_frames at false for a while. The other paths are already tied
        // to texture.next and relax nothing; these two were the only ones without a horizon.
        valid_until = frame_;
        return;
      }
      // The emulation writes the copy to memory with copy_dest_swap and loads it as a texture: the fetch
      // constant's swizzle (ZYXW for 8888) undoes that swap. Here the copy is image to image and does not
      // swap channels, so the swap goes into the host channels (without this, Mia came out blue).
      slot = SlotView(resolved->image, resolved->format, swizzle,
                           resolved->swap_rb ? kSwizzleBGRA : kSwizzleRGBA);
      valid_until = frame_;  // see the long comment on the depth copy
      return;
    }
  }
  const uint32_t format = f[1] & 0x3F;
  TextureFormat tf;
  if (!TextureFormatFor(format, tf)) {
    Warn(400 + format, "texture format not supported yet: an empty one is used");
    return;
  }
  // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h:1222-1233).
  const uint32_t width = volume ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
  const uint32_t height = volume ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t background = volume ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
  if (volume && tf.block > 1) {
    Warn(38, "compressed 3D texture: an empty one is used");
    return;
  }
  const uint32_t blocks_x = (width + tf.block - 1) / tf.block;
  const uint32_t blocks_y = (height + tf.block - 1) / tf.block;
  const uint32_t pitch_blocks = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.block, 1);
  // The faces of a cubemap are consecutive: each takes its base level with rows and columns aligned to
  // 32 blocks and the total to 4 KB (GetGuestTextureLayout).
  const uint64_t stride_face =
      (uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes *
           ((blocks_y + 31) & ~uint32_t(31)) +
       4095) &
      ~uint64_t(4095);
  const uint32_t keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
  const uint64_t key = XXH3_64bits(keys, sizeof(keys));

  // Mip levels with the rules of GetSubresourcesFromFetchConstant: no
  // mip address means no mips; the maximum is clamped to the size; if the base is missing or the minimum
  // is above 0, the base is not read. 3D textures still use only the base.
  const bool tile_texture = (f[0] >> 31) & 0x1;
  const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
  const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
  const uint32_t packed_level =
      mipmaps_ && !volume && ((f[5] >> 11) & 0x1) ? PackedLevel(width, height) : UINT32_MAX;
  uint32_t level_max = 0;
  bool read_base = true;
  if (mipmaps_ && !volume && dir_mips != 0) {
    const uint32_t size_max = Log2Floor(std::max(width, height));
    uint32_t level_min = std::min((f[4] >> 2) & 0xFu, size_max);
    level_max = std::max(std::min((f[4] >> 6) & 0xFu, size_max), level_min);
    if (level_max != 0) {
      if (dir_base == 0) {
        level_min = std::max(level_min, 1u);
      }
      read_base = level_min == 0;
    }
  }
  // Mip regions from dir_mips (GetGuestTextureLayout): each level stored with rows of 32 blocks computed
  // from the size rounded up to a power of 2 (and aligned to 256 bytes if the texture is linear) and
  // layers aligned to 4 KB; the levels of the packed tail share the region of the first of them.
  struct RegionMip {
    uint64_t displacement = 0;
    uint64_t row_bytes = 0;
    uint64_t stride = 0;
    uint32_t pitch_blocks = 0;
  };
  std::array<RegionMip, 16> regions{};
  uint64_t extension_mips = 0;
  if (level_max != 0) {
    const uint32_t last = packed_level == 0 ? 0 : std::min(level_max, packed_level);
    for (uint32_t s = packed_level == 0 ? 0 : 1; s <= last; ++s) {
      RegionMip& region = regions[s];
      const uint32_t row_texels = std::max(std::bit_ceil(width) >> s, 1u);
      const uint32_t rows_texels = std::max(std::bit_ceil(height) >> s, 1u);
      region.pitch_blocks = ((row_texels + tf.block - 1) / tf.block + 31) & ~31u;
      region.row_bytes = uint64_t(region.pitch_blocks) * tf.bytes;
      if (!tile_texture) {
        region.row_bytes = (region.row_bytes + 255) & ~uint64_t(255);
      }
      const uint64_t rows_blocks = uint64_t(((rows_texels + tf.block - 1) / tf.block + 31) & ~31u);
      region.stride = (region.row_bytes * rows_blocks + 4095) & ~uint64_t(4095);
      region.displacement = extension_mips;
      extension_mips += region.stride * layers;
    }
    if (dir_mips + extension_mips > kPhysicalMemory) {
      level_max = 0;  // the mips would go past memory: base only
      extension_mips = 0;
      read_base = true;
    }
  }
  // With the packed tail starting at level 0 (short side of 16 texels or less), the base does not start
  // at its address either (VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl applies the same
  // offset to level 0).
  uint32_t base_ox = 0;
  uint32_t base_oy = 0;
  if (packed_level == 0) {
    PackedDisplacement(width, height, tf.block, 0, base_ox, base_oy);
  }

  Texture& texture = textures_[key];
  if (texture.image.image == sa2::dk::kNoImage) {
    // BC in Vulkan: the size is in whole blocks. [dk] The same in deko3d (the layout is in whole blocks).
    const uint32_t host_width = tf.block > 1 ? (width + 3) & ~uint32_t(3) : width;
    const uint32_t host_height = tf.block > 1 ? (height + 3) & ~uint32_t(3) : height;
    // With its mip levels (those that fit in the host size).
    const uint32_t levels = std::min(level_max + 1, Log2Floor(std::max(host_width, host_height)) + 1);
    /*
     * The vkBindImageMemory goes to the bind thread if possible (sa2_native_textures_binding_thread) and,
     * otherwise, CreateTexture as usual. What the ring spends creating is timed, without its wait for the
     * thread (that is counted separately), for the [hitch] ring line and the 10 s report.
     * [dk] There is no bind thread (creating a deko3d image over block memory is CPU work only): always
     * CreateTexture, timed whole for the [hitch] ring line. The bind thread's 10 s report is gone with it.
     */
    const auto before_create = std::chrono::steady_clock::now();
    const bool created = CreateTexture(texture.image, tf.format, host_width, host_height, layers, background, levels);
    {
      const uint64_t own = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - before_create)
                                        .count());
      sa2::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
    }
    if (!created) {
      textures_.erase(key);
      Warn(32, "could not create a texture");
      return;
    }
    texture.layers = layers;
    texture.background = background;
    texture.levels = levels;
    // Measurement only: the shape without the address, to count, once its first hash is known, whether it
    // repeats the content of a live one (NoteContentTexture). If this image is being recreated, the
    // previous one stops counting. The words are the key's without the base and mip addresses and without
    // bit 11 of f[1], which the sampler sets: format, byte order, tiling, pitch, size, mip limits, dimension
    // and packed tail.
    if (DiagReuse()) {
      RemoveContentTexture(texture, key);
      const uint32_t shape[5] = {keys[0], f[1] & 0x7FF, keys[2], keys[3], (f[5] >> 9) & 0x7};
      texture.shape_content = XXH3_64bits(shape, sizeof(shape));
      texture.address = base;
      texture.content_per_measure = true;
    }
    for (uint32_t n = 0; n < levels; ++n) {
      texture.bytes += uint64_t((std::max(host_width >> n, 1u) + tf.block - 1) / tf.block) *
                       ((std::max(host_height >> n, 1u) + tf.block - 1) / tf.block) * tf.bytes * layers *
                       (background ? background : 1);
    }
    bytes_textures_ += texture.bytes;
    // Diagnostic: why the cache grows on the console. A new texture at an address that already had another
    // points to world zones reloaded at the same place; with the same address, format and size but a
    // different key, to key fields that change (mip limits, byte order...).
    {
      ++created_textures_;
      sa2::waits::g_created_textures.fetch_add(1, std::memory_order_relaxed);
      const uint64_t shape = XXH3_64bits_withSeed(&base, sizeof(base), (uint64_t(format) << 40) ^
                                                                            (uint64_t(width) << 20) ^ height);
      if (created_per_address_[base]++ > 0) {
        ++created_in_view_address_;
      }
      const auto [it_shape, new_shape] = key_per_shape_.try_emplace(shape, key);
      if (!new_shape && it_shape->second != key) {
        ++created_same_shape_other_key_;
        it_shape->second = key;
        /*
         * In the main menu, ~100 textures per frame once came back with the same address, format and size and
         * a different key, and were created again every frame. This shows which key words change (the first
         * 40 times).
         */
        const auto [it_words, new_entries] = words_per_shape_.try_emplace(shape);
        /*
         * Key guard. The same five words cannot produce a different key: if one does, the key was not computed
         * from what its words say (as when XXH3 read keys[4] before it was written).
         */
        if (!new_entries && std::equal(std::begin(keys), std::end(keys), it_words->second.begin())) {
          const uint64_t times = ++incoherent_keys_;
          if (times == 1 || times == 10 || times == 100 || times == 1000 || times % 10000 == 0) {
            REXLOG_ERROR("[native] C3 INCONSISTENT texture key ({} times): {:08X} {}x{} format {} with the "
                         "same five words as the previous time and a different key; the key is not derived from its words",
                         times, base, width, height, format);
          }
        }
        if (!new_entries && warnings_other_key_ < 40) {
          ++warnings_other_key_;
          const auto& before = it_words->second;
          REXLOG_INFO("[native] C3 same texture with another key: {:08X} {}x{} format {} | before {:08X} {:08X} "
                      "{:08X} {:02X} {:06X} | now {:08X} {:08X} {:08X} {:02X} {:06X} | changed {:08X} {:08X} "
                      "{:08X} {:02X} {:06X}",
                      base, width, height, format, before[0], before[1], before[2], before[3], before[4], keys[0],
                      keys[1], keys[2], keys[3], keys[4], before[0] ^ keys[0], before[1] ^ keys[1],
                      before[2] ^ keys[2], before[3] ^ keys[3], before[4] ^ keys[4]);
        }
        std::copy(std::begin(keys), std::end(keys), it_words->second.begin());
      } else if (new_shape) {
        auto& words = words_per_shape_[shape];
        std::copy(std::begin(keys), std::end(keys), words.begin());
      }
      // From 256 to 1024. This line and the GPU memory line after it were the two longest stutters of the
      // "diagnostic dump" group: 147.6 ms and 107.5 ms of frame time on their own. With 1024 they
      // still appear once or twice per session, which is enough to see whether the duplicate key counter
      // spikes.
      if (created_textures_ % 1024 == 0) {
        REXLOG_INFO("[native] C3 cache diag: {} textures created ({} in the cache, {} MB): {} at an address that already "
                    "had another texture; {} with the same address, format and size as another one but a different key",
                    created_textures_, textures_.size(), bytes_textures_ >> 20, created_in_view_address_,
                    created_same_shape_other_key_);
        NoteMemoryOfTheGpu();
      }
    }
    if (textures_.size() <= 48) {
      REXLOG_INFO("[native] C3: texture {:08X} {}x{} format {} {} order {} pitch {} "
                  "swizzle {:03X} signs {:02X}{}; levels {} (mips at {:08X}, packed from {})",
                  base, width, height, format, ((f[0] >> 31) & 0x1) ? "tiled" : "linear",
                  (f[1] >> 6) & 0x3, ((f[0] >> 22) & 0x1FF) << 5, swizzle, (f[0] >> 2) & 0xFF,
                  cube ? " (cube)" : volume ? " (3D)" : "", levels, dir_mips,
                  packed_level == UINT32_MAX ? std::string("none") : std::to_string(packed_level));
      if (format == 2 && !tile_texture && width >= 640 && height >= 360) {
        const uint8_t* flat = memory_->TranslatePhysical(base & 0x1FFFFFFF);
        uint8_t min = 255, max = 0;
        uint64_t sum = 0;
        for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* row = flat + uint64_t(y) * pitch_blocks;
          for (uint32_t x = 0; x < width; ++x) {
            min = std::min(min, row[x]);
            max = std::max(max, row[x]);
            sum += row[x];
          }
        }
        REXLOG_INFO("[native] ME video plane {:08X}: min {} max {} mean {:.1f}, first "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    base, min, max, double(sum) / double(uint64_t(width) * height), flat[0], flat[1],
                    flat[2], flat[3], flat[4], flat[5], flat[6], flat[7]);
      }
    }
  }
  slot = SlotView(texture.image.image, tf.format, swizzle, tf.swizzle_host, heap);
  host_width_out = texture.image.width;  // the host's, which is what the shader sees
  host_height_out = texture.image.height;
  if (texture.frame == frame_) {
    // Valid until the frame before the next check (if it changed now, only this frame)
    valid_until = std::max<uint64_t>(frame_, texture.next ? texture.next - 1 : 0);
    return;  // already checked this frame
  }
  // Textures that do not change are checked less and less often (down to every 32 frames); those that
  // change (videos) go back to being checked every frame.
  if (texture.image.prepared && frame_ < texture.next) {
    valid_until = texture.next - 1;
    return;
  }
  texture.frame = frame_;
  // What it costs from here to the end (check, untile, prepare the upload).
  struct StopwatchTexture {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~StopwatchTexture() {
      sa2::waits::g_ns_textures.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                       .count()),
          std::memory_order_relaxed);
    }
  } stopwatch_texture;
  // 2D and cubemaps: before untiling, XXH3 of the guest bytes. If they have not changed there is nothing
  // to do (untiling and comparing every texture every frame took 62 of the 66 us of each menu draw).
  if (!volume) {
    const uint32_t log2_block = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                              : tf.bytes >= 2 ? 1 : 0;
    // Extent of a layer: tiled, GetTiledAddressUpperBound2D; linear,
    // up to the last block. Includes the packed base offset (base_ox, base_oy; 0 for the rest).
    const uint32_t blocks_x_read = blocks_x + base_ox;
    const uint32_t blocks_and_read = blocks_y + base_oy;
    const uint64_t extension_layer =
        ((f[0] >> 31) & 0x1)
            ? uint64_t(std::max<int64_t>(
                  TileDisplacement2D(int32_t((blocks_x_read - 1) & ~31u),
                                          int32_t((blocks_and_read - 1) & ~31u), pitch_blocks,
                                          log2_block),
                  0)) +
                  (log2_block == 0   ? 0xA00u
                   : log2_block == 1 ? 0xC00u
                                      : (0x400u << log2_block))
            : uint64_t(std::max(pitch_blocks, blocks_x_read)) * tf.bytes * (blocks_and_read - 1) +
                  uint64_t(blocks_x_read) * tf.bytes;
    const uint64_t raw_start = uint64_t(base) & 0x1FFFFFFF;
    const uint64_t extension = (layers - 1) * stride_face + extension_layer;
    if (raw_start + extension <= kPhysicalMemory) {
      /*
       * Per-frame check budget.
       * During stutters both game threads wait for room in the ring (20-50 ms) while the ring neither
       * stops nor waits for the GPU: the ring thread itself is stuck. During the stutters, that thread was in
       * PrepareTexture and in XXH3. Textures that arrive together (one zone) double their interval at the
       * same time (1, 2, 4... 32), so all of them are rechecked in the same frame. Here a stable texture
       * (interval 8 or more) whose check does not fit in the frame's budget is deferred a few frames, at
       * most kPostponementMax in a row. New textures, changing ones (videos) and just-uploaded ones are
       * never deferred.
       */
      /*
       * Sampled recheck of stable textures (sa2_native_fingerprints_sampling).
       * A stable texture used to go through XXH3 in full every 32-39 frames only to conclude, almost always,
       * that it had not changed: in a busy scene that was 0.2-5.3 ms per frame of the ring thread. Now every
       * full check from interval 4 on also computes the hash of a sample of the same memory (SampleFingerprint:
       * first and last 4 KB block and 1 in 8, of the base and the mips) and, if the full hash matches, stores
       * it. Rechecks of stable textures compute only the sample: if it matches, the texture is taken as
       * unchanged; otherwise the usual full path follows. Textures where the sample would read more than half
       * the bytes (those with few blocks) work as before.
       * Self-checking guard: the first kSamplesToCheck rechecks of stable textures compute both hashes
       * and count disagreements (same sample, different full hash). After that, 1 in N rechecks of each
       * texture is still full (N = the cvar), so a change the sample misses is caught at most N rechecks
       * later. A single disagreement turns sampling off for the rest of the session, with a warning.
       */
      if (sampling_fingerprints_ < 0) {
        const int32_t each = REXCVAR_GET(sa2_native_fingerprints_sampling);
        sampling_fingerprints_ = each >= 2 ? std::min<int32_t>(each, 64) : 0;
        REXLOG_INFO("[native] C3: sampled recheck of stable textures (sa2_native_fingerprints_sampling) "
                    "= {}",
                    sampling_fingerprints_ ? fmt::format("YES, 1 in {} full; the first {} with both fingerprints",
                                                    sampling_fingerprints_, kSamplesToCheck)
                                      : std::string("no, always the full fingerprint"));
      }
      const uint64_t bytes_complete = extension + extension_mips;
      const uint64_t bytes_of_sample = BytesSample(extension) + BytesSample(extension_mips);
      const bool with_sample = sampling_fingerprints_ > 0 && texture.image.prepared && texture.interval >= 4 &&
                               bytes_of_sample * 2 <= bytes_complete;
      const bool sample_util = with_sample && texture.interval >= 8 && texture.valid_sample;
      const bool sample_only = sample_util && checked_samples_ >= kSamplesToCheck &&
                                texture.consecutive_samples + 1u < uint32_t(sampling_fingerprints_);
      const uint64_t bytes_sample = with_sample ? bytes_of_sample : 0;
      // What will actually be read, which is what counts against the budget.
      const uint64_t bytes_fingerprint = sample_only ? bytes_sample : bytes_complete + bytes_sample;
      if (frame_fingerprints_ != frame_) {
        frame_fingerprints_ = frame_;
        bytes_frame_fingerprint_ = 0;
      }
      if (budget_fingerprints_ < 0) {
        budget_fingerprints_ = std::max(REXCVAR_GET(sa2_native_fingerprints_kb_frame), 0);
        REXLOG_INFO("[native] C3: texture recheck budget = {} KB per frame{}",
                    budget_fingerprints_, budget_fingerprints_ ? "" : " (no limit)");
      }
      if (budget_fingerprints_ && texture.image.prepared && texture.interval >= 8 &&
          texture.postponements < kPostponementMax && bytes_frame_fingerprint_ > 0 &&
          bytes_frame_fingerprint_ + bytes_fingerprint > uint64_t(budget_fingerprints_) * 1024) {
        ++texture.postponements;
        texture.next = frame_ + 1 + (key & 1);
        valid_until = texture.next - 1;
        sa2::waits::g_postponed_fingerprints.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      texture.postponements = 0;
      // ds2_texture_lazy_fingerprint: a new texture is uploaded without its fingerprint (it is uploaded whatever the
      // hash says); its first recheck, a few frames later and spread out, takes the fingerprint as the baseline.
      static const bool lazy_new = REXCVAR_GET(ds2_texture_lazy_fingerprint);
      const bool skip_raw = lazy_new && !texture.image.prepared;
      if (!skip_raw) {
        bytes_frame_fingerprint_ += bytes_fingerprint;
        sa2::waits::g_bytes_fingerprint.fetch_add(bytes_fingerprint, std::memory_order_relaxed);
      }
      const auto start_raw_fingerprint = std::chrono::steady_clock::now();
      const uint8_t* const raw_base = memory_->TranslatePhysical(uint32_t(raw_start));
      const uint8_t* const raw_mips = extension_mips ? memory_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
      // The sample goes before the full hash. If the full one matches, the sample is of that same content,
      // and it is the one stored.
      uint64_t sample = 0;
      if (with_sample) {
        sample = SampleFingerprint(raw_base, extension, 0);
        if (extension_mips) {
          sample = SampleFingerprint(raw_mips, extension_mips, sample);
        }
        bytes_sample_ += bytes_sample;
        ReportSampling(start_raw_fingerprint);
      }
      const bool equal_sample = sample_util && sample == texture.sample_fingerprint;
      if (sample_only && equal_sample) {
        sa2::waits::g_ns_raw_fingerprint.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                          start_raw_fingerprint)
                         .count()),
            std::memory_order_relaxed);
        ++texture.consecutive_samples;
        ++samples_hits_;
        bytes_saved_sample_ += bytes_complete - bytes_sample;
        texture.interval = std::min<uint32_t>(texture.interval * 2, CapInterval(texture));
        texture.next = frame_ + texture.interval + (texture.interval >= 32 ? (key >> 7) & 7 : 0);
        valid_until = texture.next - 1;
        return;
      }
      if (sample_only) {
        // The sample changed: now the full hash, which was not in this frame's budget.
        ++distinct_samples_;
        bytes_frame_fingerprint_ += bytes_complete;
        sa2::waits::g_bytes_fingerprint.fetch_add(bytes_complete, std::memory_order_relaxed);
      }
      uint64_t raw_fingerprint = 0;
      if (!skip_raw) {
        raw_fingerprint = RawTextureFingerprint(raw_base, size_t(extension), 0, false);
        if (extension_mips) {  // and the bytes of all the mips
          raw_fingerprint = RawTextureFingerprint(raw_mips, size_t(extension_mips), raw_fingerprint, true);
        }
      } else {
        texture.raw_pending = true;
        ++lazy_skipped_;
        lazy_skipped_bytes_ += bytes_fingerprint;
      }
      sa2::waits::g_ns_raw_fingerprint.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                        start_raw_fingerprint)
                       .count()),
          std::memory_order_relaxed);
      // ds2_texture_lazy_fingerprint: the first recheck of a texture uploaded without a fingerprint takes this one as
      // its content (the upload read the same memory a few frames ago).
      const bool baseline = texture.raw_pending && texture.image.prepared && !skip_raw;
      if (baseline) {
        texture.raw_fingerprint = raw_fingerprint;
        texture.raw_pending = false;
        ++lazy_baselines_;
        const auto now_lazy = std::chrono::steady_clock::now();
        if (now_lazy - report_lazy_ >= std::chrono::seconds(30)) {
          SA2_REPORT_RING("[native] C3 lazy fingerprint (ds2_texture_lazy_fingerprint): {} new textures uploaded "
                          "without hashing ({:.1f} MB), {} baselines taken at their first recheck, last 30 s",
                          lazy_skipped_, double(lazy_skipped_bytes_) / 1048576.0, lazy_baselines_);
          report_lazy_ = now_lazy;
          lazy_skipped_ = 0;
          lazy_skipped_bytes_ = 0;
          lazy_baselines_ = 0;
        }
      }
      const bool complete_equal = texture.image.prepared && raw_fingerprint == texture.raw_fingerprint;
      if (sample_util && !sample_only) {
        // The guard. This stable recheck computed both hashes.
        ++checked_samples_;
        ++samples_with_the_two_;
        if (equal_sample && !complete_equal) {
          REXLOG_ERROR("[native] C3: the SAMPLE of texture {:08X} {}x{} format {} ({} KB base and {} KB of "
                       "mips) did not see a change that the full fingerprint did. Fingerprint sampling is SWITCHED OFF "
                       "(after {} rechecks with both).",
                       uint32_t(raw_start), width, height, format, extension >> 10, extension_mips >> 10,
                       checked_samples_);
          sampling_fingerprints_ = 0;
        } else if (checked_samples_ == kSamplesToCheck) {
          REXLOG_INFO("[native] C3: fingerprint sampling: {} rechecks with both fingerprints, no "
                      "disagreement. Sampling continues, with 1 in {} full.",
                      checked_samples_, sampling_fingerprints_);
        }
      }
      if (complete_equal) {
        // The sample of this content, for the stable rechecks.
        if (with_sample && sampling_fingerprints_ > 0) {
          texture.sample_fingerprint = sample;
          texture.valid_sample = true;
        }
        texture.consecutive_samples = 0;
        texture.interval = std::min<uint32_t>(texture.interval * 2, CapInterval(texture));
        // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
        texture.next = frame_ + texture.interval + (texture.interval >= 32 ? (key >> 7) & 7 : 0);
        valid_until = texture.next - 1;
        return;
      }
      texture.valid_sample = false;  // the content changed; the sample is no longer valid
      if (texture.image.prepared && texture.interval >= 2 && texture.late_changes < 255) {
        ++texture.late_changes;
      }
      if (texture.image.prepared && texture.interval >= 4) {
        // Diagnostic (texture popping): a stable texture changed at the same address; the old content was
        // shown for up to `interval` frames.
        static uint32_t late_changes = 0;
        ++late_changes;
        if (late_changes <= 64 || (late_changes & 1023) == 0)
          REXLOG_INFO("[native] ME stable texture changed at {:08X} {}x{} format {} after interval {} (maximum "
                      "delay {} frames; late changes {}, sample {})",
                      uint32_t(raw_start), width, height, format, texture.interval, texture.interval,
                      late_changes, sample_only ? "sample only" : "full");
      }
      texture.consecutive_samples = 0;
      texture.raw_fingerprint = raw_fingerprint;
      NoteContentTexture(texture, key);  // measurement only: sa2_native_diag_reuse
    }
  }
  // Base level of each layer, untiled and in host byte order.
  const uint32_t blocks_x_host = (texture.image.width + tf.block - 1) / tf.block;
  const uint32_t blocks_y_host = (texture.image.height + tf.block - 1) / tf.block;
  const size_t bytes_layer = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
  std::vector<uint8_t>& data = temporal_;
  // All levels back to back, each one layer after layer (UploadTexture copies them one by one).
  std::array<size_t, 16> bytes_layer_level{};
  size_t bytes_data = 0;
  for (uint32_t n = 0; n < texture.levels; ++n) {
    const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
    texture.level_displacement[n] = uint32_t(bytes_data);
    bytes_layer_level[n] = size_t(bx_host) * by_host * tf.bytes;
    bytes_data += bytes_layer_level[n] * layers * (background ? background : 1);
  }
  const auto start_zero = std::chrono::steady_clock::now();
  data.assign(bytes_data, 0);
  const auto start_detile = std::chrono::steady_clock::now();
  sa2::waits::g_ns_texture_zero.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(start_detile - start_zero).count()),
      std::memory_order_relaxed);
  struct StopwatchDetile {
    std::chrono::steady_clock::time_point start;
    ~StopwatchDetile() {
      sa2::waits::g_ns_detile.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                       .count()),
          std::memory_order_relaxed);
    }
  } stopwatch_detile{start_detile};
  const bool tile = (f[0] >> 31) & 0x1;
  if (volume && !ReadVolume(f, tf, blocks_x, blocks_y, background, blocks_x_host,
                              blocks_y_host, pitch_blocks, tile, data)) {
    Warn(33, "texture outside memory");
    if (!texture.image.prepared) {
      slot = 0;  // the image remains uninitialized
    }
    return;
  }
  const auto order = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
  // ds2_texture_fused_untile: the levels come out in host byte order; the order pass below is skipped.
  if (fused_untile_ < 0) {
    fused_untile_ = REXCVAR_GET(ds2_texture_fused_untile) ? 1 : 0;
    REXLOG_INFO("[native] C3: fused untiling and byte order (ds2_texture_fused_untile) = {}",
                fused_untile_ ? "YES, whole textures checked against the usual path" : "no");
  }
  const uint32_t unit = tf.unit_order;
  const bool fused = fused_untile_ > 0 && tile_fast_ != 0 && !volume &&
                     (FusedOrderMode(unit, uint32_t(order)) == 0 || tf.bytes % unit == 0);
  // Base level: not read if not needed (minimum level 1; it stays zeroed and the sampler never goes below
  // 1).
  const auto read_levels = [&](uint8_t* out, bool with_fused) {
  for (uint32_t c = 0; c < (volume || !read_base ? 0u : layers); ++c) {
    const uint64_t address = dir_base + c * stride_face;
    if (!ReadLevel(address, tile, pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                   base_ox, base_oy, blocks_x, blocks_y, blocks_x_host, out + bytes_layer * c, with_fused, unit,
                   uint32_t(order))) {
      return false;
    }
  }
  // Mip levels. Those in the packed tail share a region and are located with PackedDisplacement;
  // the rest, each in its own.
  for (uint32_t n = 1; n < texture.levels; ++n) {
    const uint32_t s = packed_level == 0 ? 0 : std::min(n, packed_level);
    uint32_t ox = 0;
    uint32_t oy = 0;
    if (n >= packed_level) {
      PackedDisplacement(width, height, tf.block, n, ox, oy);
    }
    const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
    for (uint32_t c = 0; c < layers; ++c) {
      const uint64_t address = dir_mips + regions[s].displacement + c * regions[s].stride;
      if (!ReadLevel(address, tile, regions[s].pitch_blocks, regions[s].row_bytes, tf, ox, oy,
                     std::min(bx_guest, bx_host), std::min(by_guest, by_host), bx_host,
                     out + texture.level_displacement[n] + bytes_layer_level[n] * c, with_fused, unit,
                     uint32_t(order))) {
        Warn(39, "mip level outside memory: left at zero");
        break;
      }
    }
  }
  return true;
  };
  if (!read_levels(data.data(), fused)) {
    Warn(33, "texture outside memory");
    if (!texture.image.prepared) {
      slot = 0;  // the image remains uninitialized
    }
    return;
  }
  if (fused) {
    // The guard: the first 200 textures, then 1 in 64, again on the usual path (untile, then OrderOfAlways).
    ++seen_fused_;
    if (seen_fused_ <= kLevelsToCheck || seen_fused_ % kCheckAOfEach == 0) {
      ++checked_fused_;
      check_fused_.assign(bytes_data, 0);
      read_levels(check_fused_.data(), false);
      OrderOfAlways(check_fused_, unit, order);
      if (check_fused_ != data) {
        size_t at = 0;
        while (at < data.size() && data[at] == check_fused_[at]) ++at;
        REXLOG_ERROR("[native] C3: fused untiling does NOT match the usual path ({}x{} format {}, {} levels, "
                     "block {} bytes, unit {} order {}; first difference at byte {} of {}). It is SWITCHED OFF.",
                     width, height, format, texture.levels, tf.bytes, unit, uint32_t(order), at, data.size());
        data.swap(check_fused_);
        fused_untile_ = 0;
      } else if (checked_fused_ == kLevelsToCheck) {
        REXLOG_INFO("[native] C3: fused untiling: {} textures checked against the usual path, all equal.",
                    checked_fused_);
      }
    }
  } else
  // In one go (ChangeOrderBytes), with the same guard as the fast untiling.
  if (tile_fast_ > 0 && order != xenos::Endian::kNone && (tf.unit_order == 2 || tf.unit_order == 4)) {
    // The first 200, then 1 in 64. Checking every texture, this guard never got to switch itself off (there
    // were only 1,024 textures) and cost 2.1 ms per MB on every new texture.
    ++seen_orders_;
    const bool check = seen_orders_ <= kLevelsToCheck || seen_orders_ % kCheckAOfEach == 0;
    if (check) {
      check_tile_ = data;
    }
    ChangeOrderBytes(data.data(), data.size(), tf.unit_order, uint32_t(order));
    if (check) {
      ++checked_orders_;
      OrderOfAlways(check_tile_, tf.unit_order, order);
      if (check_tile_ != data) {
        REXLOG_ERROR("[native] C3: the fast order change does NOT match (unit {}, order {}, {} bytes). The "
                     "fast order and untiling are SWITCHED OFF.",
                     tf.unit_order, uint32_t(order), data.size());
        data.swap(check_tile_);
        tile_fast_ = 0;
      } else if (checked_orders_ == kLevelsToCheck) {
        REXLOG_INFO("[native] C3: fast order change: {} textures checked against the regular path, all "
                    "equal.",
                    checked_orders_);
      }
    }
  } else {
    OrderOfAlways(data, tf.unit_order, order);
  }
  if (format == 60) TranscodeCtx1(data.data(), data.size());  // banjo-nx: k_CTX1 -> BC1
  // sa2-nx diagnostic: SA2_DUMP_TEXTURE=<guest base, hex> writes the bytes uploaded for that texture
  // (untiled, byte order changed, all levels) once, to check a texture a draw samples against the game's data.
  if (static const uint32_t dump_base = [] {
        const char* v = std::getenv("SA2_DUMP_TEXTURE");
        return v && *v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u;
      }();
      dump_base && (uint32_t(base) & 0x1FFFFFFFu) == (dump_base & 0x1FFFFFFFu)) {
    static std::atomic<bool> dumped{false};
    if (!dumped.exchange(true)) {
      const auto path = rex::filesystem::GetExecutableFolder() /
                        fmt::format("texture_{:08X}_{}x{}_f{}_l{}.bin", dump_base, width, height, format,
                                    texture.levels);
      if (std::FILE* out = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(data.data(), 1, data.size(), out);
        std::fclose(out);
      }
      REXLOG_INFO("[native] texture {:08X} dumped: {}x{} format {} {} levels, {} bytes -> {}", dump_base, width,
                  height, format, texture.levels, data.size(), path.string());
    }
  }
  /*
   * For a new (unprepared) texture this hash decides nothing: it is uploaded regardless. 0 is stored, and
   * the first time its memory changes it will be computed and uploaded (at most, one extra upload).
   */
  const auto start_data_fingerprint = std::chrono::steady_clock::now();
  const uint64_t fingerprint = texture.image.prepared ? XXH3_64bits(data.data(), data.size()) : 0;
  sa2::waits::g_ns_data_fingerprint.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                    start_data_fingerprint)
                   .count()),
      std::memory_order_relaxed);
  if (texture.image.prepared && fingerprint == texture.fingerprint) {
    texture.interval = std::min<uint32_t>(texture.interval * 2, CapInterval(texture));
    texture.next = frame_ + texture.interval;
    valid_until = texture.next - 1;
    return;
  }
  texture.interval = 1;
  texture.next = frame_ + 1;
  // ds2_texture_lazy_fingerprint: the baseline rechecks of a zone's new textures spread over 8 frames.
  if (texture.raw_pending) texture.next = frame_ + 2 + (key & 7);
  texture.fingerprint = fingerprint;
  texture.data.swap(data);
  texture.needs_upload = true;
  textures_to_upload_.push_back(&texture);
  sa2::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
  sa2::waits::g_bytes_uploaded.fetch_add(texture.data.size(), std::memory_order_relaxed);
  bytes_upload += (texture.data.size() + 3) & ~size_t(3);
}

// ======================================== part C ========================================
// ---- Part C (Vulkan lines 10151-11090; 10386-11325 at 98ce31b9) --------------------------------------------

// [dk] The DkImageFormat of a texture's logical VkFormat: sa2::dk::FromVk, plus what the base leaves to the
// draws' texture table (TextureFormatFor in sa2_native_draws_dk_types.h, which pairs each VkFormat with these):
// the three 16-bit packed formats and BC1 without alpha (k_CTX1 transcoded to BC1).
static DkImageFormat TextureDkFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return DkImageFormat_RGBA4_Unorm;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return DkImageFormat_RGB565_Unorm;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return DkImageFormat_RGB5A1_Unorm;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return DkImageFormat_RGB_BC1;
    default: return sa2::dk::FromVk(format);
  }
}

bool DrawsDkImpl::DiagReuse() {
  if (diag_reuse_ < 0) {
    diag_reuse_ = REXCVAR_GET(sa2_native_diag_reuse) ? 1 : 0;
    REXLOG_INFO("[native] C3: measurement of new textures with the content of another live one "
                "(sa2_native_diag_reuse) = {}",
                diag_reuse_ ? "YES (counting only: changes nothing)" : "no");
  }
  return diag_reuse_ > 0;
}

// The content key of a texture with a valid raw hash. Never 0 (0 = not recorded).
uint64_t DrawsDkImpl::ContentKey(const Texture& texture) {
  const uint64_t shape[4] = {texture.shape_content, (uint64_t(texture.image.format) << 32) | texture.levels,
                             (uint64_t(texture.image.width) << 32) | texture.image.height, texture.layers};
  return XXH3_64bits_withSeed(shape, sizeof(shape), texture.raw_fingerprint) | 1;
}

// Stops counting a texture (when it is released, when its image is recreated, or before recording it
// with another hash).
void DrawsDkImpl::RemoveContentTexture(Texture& texture, uint64_t key) {
  if (!texture.content_key) {
    return;
  }
  const auto [from, until] = live_per_content_.equal_range(texture.content_key);
  for (auto it = from; it != until; ++it) {
    if (it->second == key) {
      live_per_content_.erase(it);
      break;
    }
  }
  if (live_per_content_.find(texture.content_key) == live_per_content_.end() && distinct_contents_) {
    --distinct_contents_;
  }
  texture.content_key = 0;
}

// With the raw hash just set. If the texture is new, counts whether another live one has the same
// content and shape, and whether that other one is cold. Then records it under its content key. Ring
// thread only.
void DrawsDkImpl::NoteContentTexture(Texture& texture, uint64_t key) {
  if (!DiagReuse()) {
    return;
  }
  RemoveContentTexture(texture, key);
  const uint64_t content = ContentKey(texture);
  if (texture.content_per_measure) {
    texture.content_per_measure = false;
    ++reuse_new_;
    reuse_bytes_new_ += texture.bytes;
    const Texture* equal = nullptr;
    const Texture* cold = nullptr;
    uint32_t looked = 0;
    const auto [from, until] = live_per_content_.equal_range(content);
    for (auto it = from; it != until; ++it) {
      if (looked++ >= kReuseLookMax) {
        ++reuse_no_look_;
        break;
      }
      const auto other = textures_.find(it->second);
      if (other == textures_.end() || &other->second == &texture) {
        continue;
      }
      const Texture& t = other->second;
      if (t.content_key != content || t.image.image == sa2::dk::kNoImage || !t.image.prepared) {
        continue;
      }
      equal = &t;
      if (t.frame != UINT64_MAX && t.frame + kFramesNoUsageForDrop < frame_) {
        cold = &t;
        break;  // one cold one is enough
      }
    }
    if (equal) {
      ++reuse_match_;
      reuse_bytes_match_ += texture.bytes;
    }
    if (cold) {
      ++reuse_cold_;
      reuse_bytes_cold_ += texture.bytes;
    }
    if (equal && reuse_details_ < kReuseDetails) {
      ++reuse_details_;
      const Texture& t = cold ? *cold : *equal;
      SA2_REPORT_RING("[native] C3 reuse by content (measurement only): new texture {:08X} "
                           "({}x{}, VkFormat {}, {} levels, {} layers, {} KB) has the same content and shape as "
                           "{:08X}, {}",
                           texture.address, texture.image.width, texture.image.height,
                           uint32_t(texture.image.format), texture.levels, texture.layers, texture.bytes >> 10,
                           t.address,
                           cold ? fmt::format("unchecked for {} frames", frame_ - t.frame)
                                : std::string("still in use"));
    }
  }
  if (live_per_content_.find(content) == live_per_content_.end()) {
    ++distinct_contents_;
  }
  live_per_content_.emplace(content, key);
  texture.content_key = content;
}

// Every 10 s, if there were new textures with a hash. The cache snapshot (recorded, distinct contents)
// is O(1).
void DrawsDkImpl::ReportReuse() {
  if (diag_reuse_ <= 0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - report_reuse_ < std::chrono::seconds(10)) {
    return;
  }
  report_reuse_ = now;
  if (reuse_new_) {
    const uint64_t pointed = uint64_t(live_per_content_.size());
    SA2_REPORT_RING(
        "[native] C3 reuse by content (measurement only), last 10 s: {} new textures with fingerprint "
        "({:.1f} MB); {} with the same content and shape as another live one ({:.1f} MB), {} of them with one "
        "unchecked for more than {} frames ({:.1f} MB); {} with more than {} equal ones not fully inspected | in the "
        "cache: {} textures with fingerprint in {} distinct contents ({} repeated)",
        reuse_new_, double(reuse_bytes_new_) / 1048576.0, reuse_match_,
        double(reuse_bytes_match_) / 1048576.0, reuse_cold_, kFramesNoUsageForDrop,
        double(reuse_bytes_cold_) / 1048576.0, reuse_no_look_, kReuseLookMax, pointed,
        distinct_contents_, pointed - std::min(pointed, distinct_contents_));
  }
  reuse_new_ = 0;
  reuse_match_ = 0;
  reuse_cold_ = 0;
  reuse_no_look_ = 0;
  reuse_bytes_new_ = 0;
  reuse_bytes_match_ = 0;
  reuse_bytes_cold_ = 0;
}

// Every 30 s, one line with what hash sampling has done (see PrepareTexture). The ring thread writes
// it, but it is a single short line every 30 s.
void DrawsDkImpl::ReportSampling(std::chrono::steady_clock::time_point now) {
  if (now - report_sampling_ < std::chrono::seconds(30)) {
    return;
  }
  if (report_sampling_ != std::chrono::steady_clock::time_point{}) {
    SA2_REPORT_RING("[native] C3 fingerprint sampling: {} rechecks resolved with the sample ({:.1f} MB read and "
                "{:.1f} MB unread), {} with a changed sample and {} with both fingerprints since the previous line; "
                "{} with both since the start",
                samples_hits_, double(bytes_sample_) / 1048576.0,
                double(bytes_saved_sample_) / 1048576.0, distinct_samples_, samples_with_the_two_,
                checked_samples_);
  }
  report_sampling_ = now;
  samples_hits_ = 0;
  distinct_samples_ = 0;
  samples_with_the_two_ = 0;
  bytes_sample_ = 0;
  bytes_saved_sample_ = 0;
}

// The blocks of one level (one layer) of a 2D texture or cubemap, from block (ox, oy) of its guest
// region (the packed tail or the base of a small texture do not start at 0), to the host destination
// bx_host blocks wide. Linear: row_bytes per row; tiled: pitch in blocks (GetTiledOffset2D). false if
// it goes past memory.
// fused (ds2_texture_fused_untile): the level comes out already in host byte order (unit, order as for
// ChangeOrderBytes), and the per-level guard of the fast untiling is left to the caller's whole-texture one.
bool DrawsDkImpl::ReadLevel(uint64_t address, bool tile, uint32_t pitch_blocks, uint64_t row_bytes,
                            const TextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                            uint32_t bx_host, uint8_t* target, bool fused, uint32_t unit, uint32_t order) {
  if (!bx || !by) {
    return true;
  }
  const uint32_t fused_mode = fused ? FusedOrderMode(unit, order) : 0;
  const auto order_rows = [&] {
    if (!fused_mode) return;
    for (uint32_t y = 0; y < by; ++y) {
      ChangeOrderBytes(target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes, unit, order);
    }
  };
  const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
  const uint64_t end =
      tile ? uint64_t(std::max<int64_t>(TileDisplacement2D(int32_t((ox + bx + 31) & ~31u),
                                                                   int32_t((oy + by + 31) & ~31u),
                                                                   pitch_blocks, log2),
                                           0))
              : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
  if (address + end > kPhysicalMemory) {
    return false;
  }
  const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
  // Opt-in measurement only: gather every USED raw block before the real
  // untiler, then compare staged bytes and the same guest blocks afterwards.
  // Never hash padding or read outside physical memory for the audit.
  const uint64_t audit_bytes = uint64_t(bx) * by * tf.bytes;
  bool audit_level = StartPayloadAudit(0, audit_bytes);
  std::vector<uint64_t> audit_offsets;
  std::vector<uint8_t> audit_raw;
  uint64_t audit_before = 0;
  if (audit_level) {
    audit_offsets.reserve(size_t(bx) * by);
    for (uint32_t y = 0; y < by && audit_level; ++y) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int64_t offset = tile
            ? TileDisplacement2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2)
            : int64_t(row_bytes * (oy + y) + uint64_t(ox + x) * tf.bytes);
        if (offset < 0 || uint64_t(offset) > kPhysicalMemory ||
            address + uint64_t(offset) + tf.bytes > kPhysicalMemory) {
          audit_level = false;
          ++me_payload_invalid_;
          --me_payload_checked_[0];
          me_payload_bytes_[0] -= audit_bytes;
          break;
        }
        audit_offsets.push_back(uint64_t(offset));
      }
    }
    if (audit_level) {
      audit_raw.resize(size_t(audit_bytes));
      for (size_t i = 0; i < audit_offsets.size(); ++i)
        std::memcpy(audit_raw.data() + i * tf.bytes, source + audit_offsets[i], tf.bytes);
      audit_before = XXH3_64bits(audit_raw.data(), audit_raw.size());
    }
  }
  const auto finish_payload_level = [&] {
    if (!audit_level) return;
    for (uint32_t y = 0; y < by; ++y)
      std::memcpy(audit_raw.data() + size_t(y) * bx * tf.bytes,
                  target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
    const uint64_t staged = XXH3_64bits(audit_raw.data(), audit_raw.size());
    for (size_t i = 0; i < audit_offsets.size(); ++i)
      std::memcpy(audit_raw.data() + i * tf.bytes, source + audit_offsets[i], tf.bytes);
    FinalizePayloadAudit(0, address, audit_bytes, audit_before,
                          XXH3_64bits(audit_raw.data(), audit_raw.size()), staged);
  };
  if (!tile) {
    for (uint32_t y = 0; y < by; ++y) {
      std::memcpy(target + size_t(y) * bx_host * tf.bytes,
                  source + uint64_t(oy + y) * row_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
      if (fused_mode) {  // while the row is in the cache
        ChangeOrderBytes(target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes, unit, order);
      }
    }
    finish_payload_level();
    return true;
  }
  // The fast untiling (UntileLevel) when the block is 1, 2, 4, 8 or 16 bytes.
  if (tile_fast_ < 0) {
    tile_fast_ = REXCVAR_GET(sa2_native_tile_fast) ? 1 : 0;
    REXLOG_INFO("[native] C3: fast untiling (sa2_native_tile_fast) = {}",
                tile_fast_ ? "YES, checking the first levels against the regular path" : "no");
  }
  if (fused) {
    if (tile_fast_ && tf.bytes == (1u << log2) && (ox & 31) == 0) {
      switch (log2) {
        case 0: UntileLevelOrderMode<0>(fused_mode, source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 1: UntileLevelOrderMode<1>(fused_mode, source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 2: UntileLevelOrderMode<2>(fused_mode, source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 3: UntileLevelOrderMode<3>(fused_mode, source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        default: UntileLevelOrderMode<4>(fused_mode, source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      }
    } else if (tile_fast_ && tf.bytes == (1u << log2)) {  // the packed tail: UntileLevel, then the order
      switch (log2) {
        case 0: UntileLevel<0>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 1: UntileLevel<1>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 2: UntileLevel<2>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 3: UntileLevel<3>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        default: UntileLevel<4>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      }
      order_rows();
    } else {
      ReadLevelTileOfAlways(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
      order_rows();
    }
    finish_payload_level();  // (the opt-in payload audit sees swapped bytes here: use it with this off)
    return true;
  }
  if (tile_fast_ && tf.bytes == (1u << log2)) {
    switch (log2) {
      case 0: UntileLevel<0>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      case 1: UntileLevel<1>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      case 2: UntileLevel<2>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      case 3: UntileLevel<3>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      default: UntileLevel<4>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
    }
    /*
     * Guard: the first kLevelsToCheck levels are repeated on the usual path and compared byte by byte.
     * If one differs, the usual result is kept, a warning is logged and the fast path is off for the rest
     * of the session.
     */
    // The first 200, then 1 in 64 (checking every level, the guard cost 4.2 ms per MB).
    ++seen_levels_;
    if (seen_levels_ <= kLevelsToCheck || seen_levels_ % kCheckAOfEach == 0) {
      ++checked_levels_;
      check_tile_.assign(size_t(by) * bx_host * tf.bytes, 0);
      for (uint32_t y = 0; y < by; ++y) {
        std::memcpy(check_tile_.data() + size_t(y) * bx_host * tf.bytes,
                    target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
      }
      ReadLevelTileOfAlways(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
      bool equal = true;
      for (uint32_t y = 0; y < by && equal; ++y) {
        equal = std::memcmp(check_tile_.data() + size_t(y) * bx_host * tf.bytes,
                              target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes) == 0;
      }
      if (!equal) {
        REXLOG_ERROR("[native] C3: fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, "
                     "from {},{}). It is SWITCHED OFF and the regular path is used.",
                     bx, by, tf.bytes, pitch_blocks, ox, oy);
        tile_fast_ = 0;
      } else if (checked_levels_ == kLevelsToCheck) {
        REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the regular path, all "
                    "equal. The fast path continues.",
                    checked_levels_);
      }
    }
    finish_payload_level();
    return true;
  }
  ReadLevelTileOfAlways(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
  finish_payload_level();
  return true;
}

// The original byte swap, word by word.
void DrawsDkImpl::OrderOfAlways(std::vector<uint8_t>& data, uint32_t unit, xenos::Endian order) {
  if (unit == 2 && order != xenos::Endian::kNone) {
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
      uint16_t v;
      std::memcpy(&v, data.data() + i, 2);
      v = xenos::GpuSwap(v, order);
      std::memcpy(data.data() + i, &v, 2);
    }
  } else if (unit == 4 && order != xenos::Endian::kNone) {
    for (size_t i = 0; i + 3 < data.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, data.data() + i, 4);
      v = xenos::GpuSwap(v, order);
      std::memcpy(data.data() + i, &v, 4);
    }
  }
}

// The original untiling, block by block.
void DrawsDkImpl::ReadLevelTileOfAlways(const uint8_t* source, uint32_t pitch_blocks, uint32_t log2,
                                        uint32_t bytes, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                                        uint32_t bx_host, uint8_t* target) {
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      const int32_t displacement =
          TileDisplacement2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2);
      std::memcpy(target + (size_t(y) * bx_host + x) * bytes, source + displacement, bytes);
    }
  }
}

// Base level of a 3D texture, slice by slice (z, then y, then x), in the order vkCmdCopyBufferToImage
// uploads it. Tiled: GetTiledOffset3D; linear: consecutive slices with rows and columns aligned to 32
// blocks (GetGuestTextureLayout).
bool DrawsDkImpl::ReadVolume(const uint32_t* f, const TextureFormat& tf, uint32_t blocks_x,
                             uint32_t blocks_y, uint32_t background, uint32_t blocks_x_host,
                             uint32_t blocks_y_host, uint32_t pitch_blocks, bool tile,
                             std::vector<uint8_t>& data) {
  const uint64_t address = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
  const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                           : tf.bytes >= 2 ? 1 : 0;
  const uint64_t row = uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes;
  const uint64_t cut = row * ((blocks_y + 31) & ~uint32_t(31));
  const uint64_t end =
      tile ? uint64_t(std::max<int64_t>(
                    TileDisplacement3D(int32_t((blocks_x + 31) & ~31u),
                                            int32_t((blocks_y + 31) & ~31u),
                                            int32_t((background + 3) & ~3u), pitch_blocks,
                                            blocks_y, log2),
                    0))
              : cut * background;
  if (address + end > kPhysicalMemory) {
    return false;
  }
  const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
  const size_t bytes_cut = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
  for (uint32_t z = 0; z < background; ++z) {
    for (uint32_t y = 0; y < blocks_y; ++y) {
      for (uint32_t x = 0; x < blocks_x; ++x) {
        const uint64_t displacement =
            tile ? uint64_t(TileDisplacement3D(int32_t(x), int32_t(y), int32_t(z),
                                                       pitch_blocks, blocks_y, log2))
                    : z * cut + y * row + uint64_t(x) * tf.bytes;
        std::memcpy(data.data() + bytes_cut * z + (size_t(y) * blocks_x_host + x) * tf.bytes,
                    source + displacement, tf.bytes);
      }
    }
  }
  return true;
}

// A texture's VkImageCreateInfo, factored out so CreateTexture and CreateTextureInThread create exactly the
// same image. [dk] Its sa2::dk::ImageDesc (the DkImageLayoutMaker fields): sampling and being the destination of
// a copy need no usage flags in deko3d; the format is the DkImageFormat of the logical VkFormat, which stays in
// `logical` for keys and logs.
sa2::dk::ImageDesc DrawsDkImpl::InfoImageTexture(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                                                  uint32_t background, uint32_t levels) {
  sa2::dk::ImageDesc info;
  info.type = background    ? DkImageType_3D
              : layers == 6 ? DkImageType_Cubemap
              : layers > 1  ? DkImageType_2DArray
                            : DkImageType_2D;
  info.format = TextureDkFormat(format);
  info.width = width;
  info.height = height;
  info.depth_or_layers = background ? background : layers;
  info.mips = std::max(levels, 1u);
  info.flags = 0;  // block linear, sampled, copy destination
  info.ms = DkMsMode_1x;
  info.logical = format;
  return info;
}

// `emergency` = false only from CollectBindings. Half the cache cannot be released there: that waits
// for the GPU by submitting the work, and we are inside BeforeSend. Out of memory: false, and the
// texture is recreated later.
bool DrawsDkImpl::CreateTexture(ImageNativeDk& image, VkFormat format, uint32_t width, uint32_t height,
                                uint32_t layers, uint32_t background, uint32_t levels, bool emergency) {
  const sa2::dk::ImageDesc info = InfoImageTexture(format, width, height, layers, background, levels);
  /*
   * The pool first, and the usual path only if it does not fit.
   *
   * A dedicated allocation per texture costs 1.9 ms of CPU on Horizon, measured inside the ioctls:
   * nvMapCreate 422 us, plus two GPU address reservations (127 us each) and two mappings (626 us each).
   * There are two of each because the dedicated allocation makes its own address and mapping, and then
   * nvk_image_plane_bind makes others for the plane. And the only thing the dedicated allocation buys is
   * compression, which our textures never use (nvk_image.c:843-846 returns false with
   * SAMPLED|TRANSFER_DST). So we paid double and got nothing.
   *
   * Sub-allocating from large slabs removes the allocation's address and mapping: 1.9 -> 0.75 ms. For a
   * burst of 15 textures in one frame, from 28.5 to 11.3 ms. That is what shows when entering a new zone
   * of the map.
   *
   * It does not change a single pixel: same tiling, same format, same pte_kind. Binding at an offset
   * keeps the block-linear layout (nvk_image.c:1700-1710) and is the normal path of any sub-allocator in
   * NVK.
   *
   * [dk] sa2::dk::CreateImage is that pool: it sub-allocates from 32 MB image blocks (images of 8 MB or more
   * get a block of their own) and initializing a DkImage over block memory is CPU work only, no ioctl.
   * pool_block stays invalid: DestroyImage hands the memory back through sa2::dk::DestroyImage.
   */
  if (info.format == DkImageFormat_None) {
    if (warned_.insert(0xD0000000u | uint32_t(format)).second) {
      REXLOG_ERROR("[native] [dk] C3: VkFormat {} has no deko3d texture format; the texture is not created",
                   uint32_t(format));
    }
    return false;
  }
  image.pool_block = 0xFFFFFFFFu;
  image.image = sa2::dk::CreateImage(info, "texture");
  bool reserved = image.image != sa2::dk::kNoImage;
  if (!reserved) {
    // Out of GPU memory. Instead of giving up (which ends in a black screen), half the cache is released
    // with the GPU idle and the allocation is retried once.
    // [dk] The retry asks sa2::dk::CreateImage again: the released textures' memory comes back to its blocks
    // once the GPU has passed their last use (DropTexturesPerMissingOfMemory waits for it).
    if (!emergency || !DropTexturesPerMissingOfMemory() ||
        (image.image = sa2::dk::CreateImage(info, "texture")) == sa2::dk::kNoImage) {
      REXLOG_ERROR("[native] C3: out of GPU memory for a {}x{} texture ({} levels, {} layers) and "
                   "{}; {} textures and {} MB remain",
                   width, height, std::max(levels, 1u), layers,
                   emergency ? "releasing the cache was not enough"
                              : "without releasing the cache (from CollectBindings; it is recreated at its next check)",
                   textures_.size(), bytes_textures_ >> 20);
      return false;
    }
  }
  image.width = width;
  image.height = height;
  image.format = format;
  image.dk_flags = info.flags;
  image.prepared = false;
  return true;
}

bool DrawsDkImpl::UploadTexture(Texture& texture) {
  const DkCmdBuf upload = context_->CommandsUpload();
  if (!upload || !texture.needs_upload) {
    return upload != nullptr;
  }
  VkDeviceSize offset;
  Reserve(texture.data.size(), 16, offset);  // BC: offset multiple of the block
  std::memcpy(upload_data_ + offset, texture.data.data(), texture.data.size());
  /*
   * If its vkBindImageMemory is still on the bind thread, neither the barrier nor the copy can be
   * recorded for the image (it has no memory). The data is already in the upload buffer; the barrier and
   * the copy are recorded in CollectBindings, in this same upload buffer before it is closed
   * (BeforeSend).
   * [dk] There is no bind thread (texture.in_flight is always 0): PostponeCopy and CollectBindings are gone.
   */
  if (texture.image.image != sa2::dk::kNoImage) {
    if (!texture.image.prepared) {
      Barrier(upload, texture.image.image, texture.layers);
      texture.image.prepared = true;
    }
    RecordCopyTexture(upload, texture, offset);
  }
  texture.needs_upload = false;
  /*
   * The copy of the pixels is no longer needed. They are already in the upload buffer, and to know
   * whether the texture changes its hashes are kept, not the bytes. Keeping the copy made the texture
   * cache take the same space again in CPU RAM (150-680 MB). That once ended in std::bad_alloc in the
   * main menu, with the cache at 682 MB.
   */
  // The larger buffer is kept as temporal_ (so the next new texture does not ask the system for memory
  // again or touch fresh pages); the other is released.
  if (texture.data.capacity() > temporal_.capacity()) {
    temporal_.swap(texture.data);
  }
  std::vector<uint8_t>().swap(texture.data);
  ++uploads_texture_;
  return true;
}

// One range per mip level (with all its layers). Factored out for UploadTexture and for the deferred
// copies of CollectBindings.
void DrawsDkImpl::RecordCopyTexture(DkCmdBuf upload, const Texture& texture, VkDeviceSize offset) {
  if (!sa2::dk::Alive(texture.image.image)) {
    return;  // [dk] a view needs a live image
  }
  // A guest texture may be uploaded more than once within one submission.
  // Order overlapping transfer writes as well as writes from earlier work.
  // [dk] The named barrier of buffer-to-image copies (a superset of transfer -> transfer). The one before the
  // draws that sample the texture belongs to whoever closes the upload command list.
  sa2::dk::Barrier(upload, sa2::dk::Hazard::UploadToSample);
  for (uint32_t n = 0; n < texture.levels; ++n) {
    // The data is level after level and, in each, layer after layer, tightly packed: rowLength and imageHeight
    // 0 (deko3d counts compressed formats in blocks, as vkCmdCopyBufferToImage does with bufferRowLength 0).
    sa2::dk::ViewDesc level;
    level.first_mip = uint8_t(n);
    level.mips = 1;
    const DkImageView view = sa2::dk::View(texture.image.image, level);
    const DkCopyBuf source{upload_address_ + offset + texture.level_displacement[n], 0, 0};
    const DkImageRect rect{0, 0, 0, std::max(texture.image.width >> n, 1u), std::max(texture.image.height >> n, 1u),
                           texture.background ? texture.background : texture.layers};
    dkCmdBufCopyBufferToImage(upload, &source, &view, &rect, 0);
  }
}

// A texture's view, the same for SlotView and for the deferred views of CollectBindings.
// [dk] A DkImageView made on the spot (no object to create or destroy); false if the image is gone. The format
// is the image's own (the Vulkan view's format is always the image's), and a depth format samples its depth
// (DkDsSource_Depth, the default: the VK_IMAGE_ASPECT_DEPTH_BIT of the Vulkan view).
bool DrawsDkImpl::CreateViewTexture(sa2::dk::ImageId image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                                    uint32_t heap, DkImageView& view) {
  if (!sa2::dk::Alive(image)) {
    return false;
  }
  sa2::dk::ViewDesc info;
  info.type = heap == 2   ? DkImageType_Cubemap
              : heap == 1 ? DkImageType_3D
                          : DkImageType_2D;
  MappingComponents(swizzle, swizzle_host, info.swizzle);
  info.source = DkDsSource_Depth;
  (void)format;
  info.first_mip = 0;
  info.mips = 0;   // with its mips
  info.layers = 0;  // all: 6 for a cubemap, 1 for the rest
  view = sa2::dk::View(image, info);
  return true;
}

// The views_ key (image, heap and both swizzles), factored out for CollectBindings.
uint64_t DrawsDkImpl::ViewKey(sa2::dk::ImageId image, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap) {
  return XXH3_64bits_withSeed(&image, sizeof(image),
                              (uint64_t(heap) << 40) | (uint64_t(swizzle) << 16) | swizzle_host);
}

// Heap slot (0 for 2D textures, 2 for cubemaps) for a view of the image with that swizzle.
uint32_t DrawsDkImpl::SlotView(sa2::dk::ImageId image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                               uint32_t heap) {
  const uint64_t key = ViewKey(image, swizzle, swizzle_host, heap);  // The same key
  if (const auto it = views_.find(key); it != views_.end()) {
    return it->second.slot;
  }
  // An image whose vkBindImageMemory is still on the bind thread. The view carries the image's GPU
  // address, which comes from the bind: the slot is reserved now, and the view is created and written in
  // CollectBindings, before submission (UPDATE_AFTER_BIND descriptors).
  // [dk] No bind thread: images_in_flight_ and SlotViewInFlight are gone.
  View view;
  view.image = image;
  view.heap = heap;
  DkImageView view_dk;
  if (!CreateViewTexture(image, format, swizzle, swizzle_host, heap, view_dk)) {
    Warn(34, "could not create a texture view");
    return 0;
  }
  view.slot = ReserveSlot(heap);
  if (!view.slot) {
    NoteSlotsFull(heap);
    Warn(35, "texture pool full");
    return 0;
  }
  WriteImage(heap, view.slot, view_dk);
  views_.emplace(key, view);
  views_per_image_[image].push_back(key);
  return view.slot;
}

// point: no filtering (depth textures).
uint32_t DrawsDkImpl::SlotSampler(const uint32_t* f, bool punctual) {
  // Diagnostic: filters the game requests that this renderer does not apply (anisotropy, mip bias and
  // maximum mip level; the bias is not even part of the key). Each new value is logged once.
  {
    const uint32_t aniso = (f[3] >> 25) & 0x7;
    const int32_t bias = int32_t(f[4] << 10) >> 22;  // lod_bias: 10 bits signed, 5 fractional
    const uint32_t mip_max = (f[4] >> 6) & 0xF;
    const uint32_t bias_index = uint32_t(bias + 512);
    if (!((aniso_seen_ >> aniso) & 1u) || !seen_biases_[bias_index] || !((mip_max_seen_ >> mip_max) & 1u)) {
      aniso_seen_ |= 1u << aniso;
      seen_biases_[bias_index] = true;
      mip_max_seen_ |= 1u << mip_max;
      REXLOG_INFO("[native] C4 filters requested by the game (new value): anisotropic {}, mip bias {:.3f}, "
                  "max mip level {}, mag/min/mip filters {}/{}/{}{}",
                  aniso, float(bias) / 32.0f, mip_max, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                  (f[3] >> 23) & 0x3, punctual ? " (point sampling)" : "");
    }
  }
  const uint32_t field_filters = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
  const uint32_t field_mips = (f[4] >> 2) & 0xFF;       // minimum in 0-3, maximum in 4-7
  const uint32_t key = ((f[0] >> 10) & 0x1FF) | (field_filters << 9) |
                         (field_mips << 21) | ((f[5] & 0x3) << 29) |
                         (punctual ? 0x80000000u : 0u);
  if (const auto it = samplers_.find(key); it != samplers_.end()) {
    return it->second.second;
  }
  static constexpr DkWrapMode kModes[8] = {
      DkWrapMode_Repeat,        DkWrapMode_MirroredRepeat,
      DkWrapMode_ClampToEdge,   DkWrapMode_MirrorClampToEdge,
      DkWrapMode_ClampToEdge,   DkWrapMode_MirrorClampToEdge,
      DkWrapMode_ClampToBorder, DkWrapMode_MirrorClampToEdge};
  // [dk] Maxwell always has mirror clamp to edge (samplerMirrorClampToEdge). The table keeps the Vulkan
  // approximations: deko3d's exact Clamp (4, ClampHalfway), MirrorClamp (5) and MirrorClampToBorder (7) are a
  // change to measure on its own.
  const bool mirror = true;
  const auto mode = [&](uint32_t value) {
    const DkWrapMode m = kModes[value & 0x7];
    return m == DkWrapMode_MirrorClampToEdge && !mirror ? DkWrapMode_ClampToEdge : m;
  };
  // [dk] The limits NVK reports for this GPU (VkPhysicalDeviceLimits on the Switch): anisotropy up to 16.
  constexpr float kMaxSamplerAnisotropy = 16.0f;
  DkSampler info;
  dkSamplerDefaults(&info);
  info.magFilter =
      ((f[3] >> 19) & 0x3) == 1 && !punctual ? DkFilter_Linear : DkFilter_Nearest;
  info.minFilter =
      ((f[3] >> 21) & 0x3) == 1 && !punctual ? DkFilter_Linear : DkFilter_Nearest;
  const uint32_t mip = (field_filters >> 4) & 0x3;
  info.mipFilter =
      mip == 1 && !punctual ? DkMipFilter_Linear : DkMipFilter_Nearest;
  info.wrapMode[0] = mode(f[0] >> 10);
  info.wrapMode[1] = mode(f[0] >> 13);
  info.wrapMode[2] = mode(f[0] >> 16);
  info.lodClampMin = float(field_mips & 0xF);  // with the mip diagnostic if it is enabled
  // With real mips, the fetch constant's maximum level also limits (like the emulation's sampler);
  // single-level textures stay as before.
  info.lodClampMax = mip == 2 ? info.lodClampMin + 0.25f : std::max(info.lodClampMin, float((f[4] >> 6) & 0xF));
  // VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE or VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK.
  const float border = (f[5] & 0x3) == 1 ? 1.0f : 0.0f;
  for (auto& component : info.borderColor) {
    component.value_f = border;
  }
  // sa2_native_filter_aniso: the game's anisotropy (fetch bits 25-27, already in the key through the
  // filter field), as the SDK applies it. Only samplers with mips; point (depth) samplers stay as they are.
  {
    static const int32_t filter_aniso = REXCVAR_GET(sa2_native_filter_aniso);
    const uint32_t aniso = (f[3] >> 25) & 0x7;
    if (filter_aniso > 0 && !punctual && aniso >= 1 && aniso <= 5 && info.lodClampMax > info.lodClampMin) {
      info.magFilter = DkFilter_Linear;
      info.minFilter = DkFilter_Linear;
      info.mipFilter = DkMipFilter_Linear;
      if (filter_aniso > 1 && aniso > 1) {
        info.maxAnisotropy = std::min(float(1u << (aniso - 1)), kMaxSamplerAnisotropy);
      }
    }
  }
  // sa2_anisotropic_native. Only linear samplers with mips (maxLod above minLod); the value is read at
  // start-up, so it does not need to be in the key: every sampler is created with the same one.
  {
    static const uint32_t aniso_request = uint32_t(std::max(0, int32_t(REXCVAR_GET(sa2_anisotropic_native))));
    if (aniso_request > 1 && info.minFilter == DkFilter_Linear &&
        info.mipFilter == DkMipFilter_Linear && info.lodClampMax > info.lodClampMin) {
      info.maxAnisotropy = std::max(1.0f, std::min(float(aniso_request), kMaxSamplerAnisotropy));
      if (!aniso_recorded_) {
        aniso_recorded_ = true;
        REXLOG_INFO("[native] C4 anisotropic filtering {} on linear samplers with mips (requested {}, device "
                    "limit {}); single-level and point samplers unchanged",
                    info.maxAnisotropy, aniso_request, kMaxSamplerAnisotropy);
      }
    }
  }
  // ds2_texture_lod_bias. Read at start-up, so every sampler gets the same one and the key does not need it.
  {
    static const float lod_bias = float(REXCVAR_GET(ds2_texture_lod_bias));
    if (lod_bias != 0.0f && info.minFilter == DkFilter_Linear && info.lodClampMax > info.lodClampMin) {
      constexpr float limit = 2.0f;  // maxSamplerLodBias: Vulkan guarantees at least 2
      info.lodBias = std::clamp(lod_bias, -limit, limit);
      if (!lod_bias_recorded_) {
        lod_bias_recorded_ = true;
        REXLOG_INFO("[native] C4 mip LOD bias {:.2f} on linear samplers with mips (ds2_texture_lod_bias, device "
                    "limit {})", info.lodBias, limit);
      }
    }
  }
  // [dk] Nothing to create (vkCreateSampler and its warning 36 are gone): the descriptor is written below.
  const uint32_t slot = ReserveSlot(3);
  if (!slot) {
    Warn(37, "sampler pool full");
    return 0;
  }
  WriteSampler(slot, info);
  samplers_.emplace(key, std::make_pair(info, slot));
  return slot;
}

bool DrawsDkImpl::PsAlphaOnly() { return REXCVAR_GET(sa2_native_ps_alpha_only); }

bool DrawsDkImpl::NoPsNoColor() { return REXCVAR_GET(sa2_native_no_ps_no_color); }

// 1/size of a slot's host image, in the shared constants. With size 0 (a texture that could not be
// prepared) 0 is written: the shader does not use it because it does not sample either.
void DrawsDkImpl::WriteInvSize(uint32_t* shared, uint32_t register_value, uint32_t width, uint32_t height) {
  const float inv[2] = {width ? 1.0f / float(width) : 0.0f, height ? 1.0f / float(height) : 0.0f};
  std::memcpy(shared + kWordInvSize + register_value * 2, inv, sizeof(inv));
}

bool DrawsDkImpl::BeginPass(const uint32_t* r, const uint64_t keys[5], uint32_t pitch, uint64_t pass_key) {
  pass_start_ = time_ ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
  if (!PrepareEmpty()) {
    return false;
  }
  std::array<ImageNativeDk*, 5> images{};
  for (uint32_t i = 0; i < 4; ++i) {
    if (keys[i]) {
      images[i] = context_->TargetColor(uint32_t(keys[i] >> 24) & 0xFFF,
                                            uint32_t(keys[i] >> 16) & 0xF, pitch);
      if (!images[i]) {
        return Reject(40, "no color target");
      }
    }
  }
  if (keys[4]) {
    images[4] = context_->TargetDepth(uint32_t(keys[4] >> 24) & 0xFFF,
                                                uint32_t(keys[4] >> 16) & 0x1, pitch);
    if (!images[4]) {
      return Reject(41, "no depth target");
    }
  }
  (void)r;
  // The render targets are already resolved above; that segment is closed.
  const auto after_targets = time_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
  if (time_) {
    stages_ns_[9] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        after_targets - pass_start_).count());
  }
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  // sa2-nx render resolution: the attachments' host extent and their common scale (me_resolution_scale.h).
  uint32_t host_width = UINT32_MAX, host_height = UINT32_MAX, scale_q = 0;
  uint32_t raster_grid_x = UINT32_MAX;
  std::array<sa2::dk::ImageId, 5> views{};  // [dk] the targets' images (a view is made when binding)
  uint32_t formats[5] = {};
  std::array<uint32_t, 5> sample_counts{};
  for (uint32_t i = 0; i < 5; ++i) {
    if (images[i]) {
      const uint32_t grid = images[i]->raster_grid_x;
      if (grid > 1 || (grid && (images[i]->width & ((1u << grid) - 1))))
        return Reject(41, "target with unsupported raster grid X");
      if (raster_grid_x != UINT32_MAX && raster_grid_x != grid)
        return Reject(41, "targets with incompatible raster grids X");
      if (scale_q && scale_q != images[i]->scale_q)
        return Reject(41, "targets at different render scales");
      raster_grid_x = grid;
      scale_q = images[i]->scale_q;
      width = std::min(width, images[i]->width);
      height = std::min(height, images[i]->height);
      host_width = std::min(host_width, HostWidth(*images[i]));
      host_height = std::min(host_height, HostHeight(*images[i]));
      views[i] = images[i]->image;
      formats[i] = uint32_t(images[i]->format);
      sample_counts[i] = uint32_t(images[i]->sample_count);
    }
  }
  if (raster_grid_x == UINT32_MAX || !width || !height)
    return Reject(41, "pass without a valid raster extent");
  const uint32_t sample_count = me::native::CommonHardwareSampleCount(sample_counts);
  if (!sample_count)
    return Reject(41, "attachment hardware sample counts are invalid or incompatible");
  // Render-pass/pipeline and material shader variants are 1x, except the host multisampling of the game's
  // single-sample targets (sa2_msaa): passes and pipelines then carry that sample count.
  if (!me::native::IsSingleSample(sample_count) && sample_count != msaa_samples_)
    return Reject(41, "real multisample draw pass is not enabled in this backend");
  const uint32_t logical_width = width >> raster_grid_x;
  // Shadow map: a depth-only target matching shadow dimensions (880x880, 1280x1280, 1600x1600).
  const bool is_shadows = pitch >= 1600 && formats[4] && !formats[0] && !formats[1] &&
                          !formats[2] && !formats[3];
  // GPU time per pass type (C2 report), by render target width.
  category_pass_ = CategoryOfTarget(pitch, keys);
  context_->MarkGpu(category_pass_);
  context_->DescribeMarkGpu((uint64_t(width) << 48) | (uint64_t(height) << 32) | (uint64_t(formats[0] & 0xFFFF) << 16) |
                               (formats[4] & 0xFFFF));
  label_pass_ = true;
  const bool shadows_no_load =
      is_shadows && REXCVAR_GET(sa2_native_pass_shadows_no_load);
  /*
   * [dk] No render pass or framebuffer objects (PassFor, CreatePass, FramebufferFor): the targets are bound
   * below with dkCmdBufBindRenderTargets. Maxwell renders straight to memory (no tile load or store), so the
   * pass that does not load its previous content (kLoadIgnore, the shadow map) binds the same way, and the
   * subpass dependencies are the named barriers of whoever reads the targets later (render targets region).
   * What was "could not create the framebuffer" is now a target whose image is gone.
   */
  const uint32_t load_mode = shadows_no_load ? kLoadIgnore : kLoadRead;
  (void)load_mode;
  for (uint32_t i = 0; i < 5; ++i) {
    if (images[i] && !sa2::dk::Alive(views[i])) {
      return Reject(43, "[dk] a render target image is gone");
    }
  }
  if (time_) {
    stages_ns_[10] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - after_targets).count());
  }
  const auto before_open = time_ ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
  const DkCmdBuf cmd = context_->CommandsWork();
  if (!cmd) {
    return false;
  }
  // Only the rectangle the game uses (see sa2_native_pass_area_util). Host pixels: the heights are recorded
  // from the draws' host scissors (and forgotten when the render scale changes, NotifyRescale).
  uint32_t pass_height = host_height;
  if (area_util_) {
    const auto it = util_height_.find(pitch);
    if (it != util_height_.end() && it->second < host_height) {
      pass_height = it->second;
    }
  }
  // [dk] There is no render area: pass_height stays for the texel counts below (the draws' scissors limit
  // the work, as they do inside the Vulkan render area).
  // The statistics cover the whole pass, from here to after EndRenderPass.
  stats_pass_ = context_->BeginStats(category_pass_);
  const auto before_start = std::chrono::steady_clock::now();
  {
    // [dk] vkCmdBeginRenderPass: the color targets compacted in order, as CreatePass numbers its color
    // attachments (the pipelines' blend states follow the same order), and the depth target.
    std::array<DkImageView, 4> colors_dk;
    std::array<const DkImageView*, 4> colors_bound{};
    uint32_t n_colors = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (images[i]) {
        colors_dk[n_colors] = sa2::dk::View(views[i]);
        colors_bound[n_colors] = &colors_dk[n_colors];
        ++n_colors;
      }
    }
    DkImageView depth_dk;
    if (images[4]) {
      depth_dk = sa2::dk::View(views[4]);
    }
    dkCmdBufBindRenderTargets(cmd, colors_bound.data(), n_colors, images[4] ? &depth_dk : nullptr);
  }
  ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - before_start).count());
  if (time_) {
    stages_ns_[11] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_open).count());
  }
  active_pass_ = true;
  ++started_passes_;
  texels_passes_ += uint64_t(host_width) * pass_height;  // how much tile is loaded by loadOp = LOAD
  {
    // Same criterion as the per-category GPU time: the render target's pitch decides.
    const uint32_t category = CategoryOfTarget(pitch, keys);
    texels_per_category_[category] += uint64_t(host_width) * pass_height;
    ++passes_per_category_[category];
  }
  pass_commands_ = cmd;
  draws_in_pass_ = 0;  // position within the pass (deferred sky report)
  pass_key_ = pass_key;
  std::memcpy(pass_keys_, keys, sizeof(pass_keys_));  // the bytes that produce pass_key_
  pass_valid_keys_ = true;
  ++pass_series_;  // the cached framing depends on the pass size and scale
  pass_generation_ = context_->GenerationCommands();
  // The pass size in host pixels (ComputeFraming clamps the host scissor to it and builds the NDC from it).
  pass_width_ = host_width;
  pass_height_ = host_height;
  pass_res_q_ = scale_q ? scale_q : kScaleQuartersNative;
  pass_samples_ = sample_count;  // sa2_msaa: the pipelines' rasterization samples (PipelineKey::fill)
  // Grid expansion changes only physical X coverage, not the shadow scale
  // (which applies in both axes). Derive the latter from the logical width.
  pass_raster_scale_x_ = float(1u << raster_grid_x);
  pass_scale_ = (is_shadows && pitch && logical_width && logical_width != pitch)
                     ? float(logical_width) / float(pitch)
                     : 1.0f;
  std::copy(std::begin(formats), std::end(formats), std::begin(pass_formats_));
  pass_depth_float24_half_ = images[4] && images[4]->depth_float24_half;
  // Phase 0b (sa2_native_pipeline_between_passes). Vulkan keeps the bound pipeline across passes of the
  // same buffer: the pass's first draw only binds again if its key differs. A new buffer still starts
  // with nothing bound (Draw, recording_generation_).
  if (!pipeline_between_passes_) {
    pipeline_bound_ = nullptr;
  } else if (pipeline_bound_ != nullptr) {
    ++passes_with_pipeline_;
  }
  recorded_state_ = false;
  if (pipeline_bound_ == nullptr) {
    eds_valid_ = false;  // pass that forgets the pipeline (without 0b): set everything again
  }
  return true;
}

// ======================================== part D ========================================
// ==== Part D (Vulkan lines 11327-12150) ====================================================================

// The DKSH package of the active shader library (region 07 / me_native_system define it, as ActiveLibrary()).

// SpirvTestsSpecBit (Vulkan file, line 1438), for an entry whose package facts are not valid: whether a SPIR-V
// module tests bit `bit` of specialization constant 0. Read once per shader.
static bool SpirvTestsSpecBitDk(std::span<const uint32_t> spirv, uint32_t bit) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) return false;
  std::unordered_set<uint32_t> spec_ids, bit_constants;
  std::vector<std::pair<uint32_t, uint32_t>> ands;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16, op = spirv[i] & 0xFFFF;
    if (!words || i + words > spirv.size()) return false;
    if (op == 71 && words == 4 && spirv[i + 2] == 1 && spirv[i + 3] == 0) spec_ids.insert(spirv[i + 1]);  // SpecId 0
    if (op == 43 && words == 4 && spirv[i + 3] == bit) bit_constants.insert(spirv[i + 2]);  // OpConstant
    if (op == 52 && words == 6 && spirv[i + 3] == 199) ands.push_back({spirv[i + 4], spirv[i + 5]});  // BitwiseAnd
    i += words;
  }
  for (const auto& [a, b] : ands)
    if ((spec_ids.count(a) && bit_constants.count(b)) || (spec_ids.count(b) && bit_constants.count(a))) return true;
  return false;
}

// The Location walk of InputsMaskPS (Vulkan file, lines 11870-11887) over a fragment shader's SPIR-V.
static uint32_t InputsMaskOfSpirvDk(const std::vector<uint32_t>& code) {
  uint32_t mask = 0;
  bool valid = code.size() > 5 && code[0] == 0x07230203;
  std::unordered_map<uint32_t, uint32_t> locations;  // id -> Location
  std::unordered_set<uint32_t> inputs, builtins;
  for (size_t i = 5; valid && i < code.size();) {
    const uint32_t words = code[i] >> 16, op = code[i] & 0xFFFF;
    if (!words || i + words > code.size()) { valid = false; break; }
    if (op == 71 /*OpDecorate*/ && words >= 4 && code[i + 2] == 30 /*Location*/) locations[code[i + 1]] = code[i + 3];
    if (op == 71 && words >= 3 && code[i + 2] == 11 /*BuiltIn*/) builtins.insert(code[i + 1]);
    if (op == 59 /*OpVariable*/ && words >= 4 && code[i + 3] == 1 /*Input*/) inputs.insert(code[i + 2]);
    i += words;
  }
  for (const uint32_t id : inputs) {
    if (builtins.count(id)) continue;
    const auto it = locations.find(id);
    if (it == locations.end() || it->second >= 32) { valid = false; break; }
    mask |= 1u << it->second;
  }
  return valid ? mask : 0xFFFFFFFFu;
}

// [dk] A package fact (dkp::kFact*) of an entry: 1 / 0, or -1 when the package does not know it.
static int FactDk(const LibraryDksh* dksh, uint32_t number, uint32_t fact) {
  if (!dksh || !dksh->loaded()) return -1;
  const uint32_t facts = dksh->Facts(number);
  if (!(facts & dkp::kFactsValid)) return -1;
  return (facts & fact) ? 1 : 0;
}

bool DrawsDkImpl::HasShaderLight(const ShaderEntry& entry) {
  if (entry.number >= shader_light_by_number_.size()) shader_light_by_number_.resize(size_t(entry.number) + 1, -1);
  int8_t& known = shader_light_by_number_[entry.number];
  if (known < 0) {
    if (!dksh_) dksh_ = ActiveLibraryDksh();
    const int fact = FactDk(dksh_, entry.number, dkp::kFactShaderLight);
    known = fact >= 0 ? int8_t(fact) : (entry.shader && Ds2ShaderLightSupported(entry.shader->Spirv()) ? 1 : 0);
    if (known)
      REXLOG_INFO("[native] DS2 light shader {:016X} n{}: variant available (mask bit {})",
                  entry.shader->fingerprint, entry.number, Ds2ShaderLightMask(entry.shader->fingerprint));
    else
      REXLOG_WARN("[native] DS2 light shader n{}: package has no variant; using original", entry.number);
  }
  return known != 0;
}

bool DrawsDkImpl::HasStructuredCf(const ShaderEntry& entry) {
  if (entry.number >= structured_cf_by_number_.size()) structured_cf_by_number_.resize(size_t(entry.number) + 1, -1);
  int8_t& known = structured_cf_by_number_[entry.number];
  if (known < 0) {
    if (!dksh_) dksh_ = ActiveLibraryDksh();
    const int fact = FactDk(dksh_, entry.number, dkp::kFactTestsStructuredCf);
    known = fact >= 0 ? int8_t(fact)
                      : (entry.shader && SpirvTestsSpecBitDk(entry.shader->Spirv(), kSpecStructuredCf) ? 1 : 0);
  }
  return known != 0;
}

bool DrawsDkImpl::VsIndexUnused(const ShaderEntry& entry) {
  if (entry.number >= index_unused_by_number_.size()) index_unused_by_number_.resize(size_t(entry.number) + 1, -1);
  int8_t& known = index_unused_by_number_[entry.number];
  if (known < 0) {
    if (!dksh_) dksh_ = ActiveLibraryDksh();
    const int fact = FactDk(dksh_, entry.number, dkp::kFactTestsVertexIndexUnused);
    known = fact >= 0 ? int8_t(fact)
                      : (entry.shader && SpirvTestsSpecBitDk(entry.shader->Spirv(), kSpecVertexIndexUnused) ? 1 : 0);
  }
  return known != 0;
}

// The plain loop for 16-bit indices (byte-swapped or not), untouched.
// restart (ds2-nx, ds2_indices16_restart): 0xFFFF is the restart index, left out of the range (all restarts: min
// 0xFFFF > max 0, "resets only").
void DrawsDkImpl::IndicesFrom16Scale(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                                   uint32_t& max, bool restart) {
  min = 0xFFFF;
  max = 0;
  const uint16_t keep = restart ? 0xFFFF : 0;  // the value left out of max
  if (rotate) {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t raw;
      std::memcpy(&raw, data + size_t(i) * 2, 2);
      const uint16_t v = std::byteswap(raw);
      output[i] = v;
      min = std::min<uint32_t>(min, v);
      if (!restart || v != keep) max = std::max<uint32_t>(max, v);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, data + size_t(i) * 2, 2);
      output[i] = v;
      min = std::min<uint32_t>(min, v);
      if (!restart || v != keep) max = std::max<uint32_t>(max, v);
    }
  }
}

// The same loop with NEON, 16 indices per iteration (vrev16 swaps the bytes of each 16-bit index, the
// same as std::byteswap), and the remainder with the plain loop. NEON loads and stores need no
// alignment on AArch64.
void DrawsDkImpl::IndicesFrom16Neon(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                                  uint32_t& max, bool restart) {
  if (restart) {  // ds2-nx (ds2_indices16_restart)
    IndicesFrom16NeonRestart(data, count, output, rotate, min, max);
    return;
  }
  uint32_t i = 0;
  uint32_t mn = 0xFFFF;
  uint32_t mx = 0;
  if (count >= 16) {
    uint16x8_t min_a = vdupq_n_u16(0xFFFF);
    uint16x8_t min_b = min_a;
    uint16x8_t max_a = vdupq_n_u16(0);
    uint16x8_t max_b = max_a;
    // Two loops (swapping is fixed per draw): no decision inside the iteration.
    if (rotate) {
      for (; i + 16 <= count; i += 16) {
        const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2)));
        const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2 + 16)));
        vst1q_u16(output + i, a);
        vst1q_u16(output + i + 8, b);
        min_a = vminq_u16(min_a, a);
        min_b = vminq_u16(min_b, b);
        max_a = vmaxq_u16(max_a, a);
        max_b = vmaxq_u16(max_b, b);
      }
    } else {
      for (; i + 16 <= count; i += 16) {
        const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2));
        const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2 + 16));
        vst1q_u16(output + i, a);
        vst1q_u16(output + i + 8, b);
        min_a = vminq_u16(min_a, a);
        min_b = vminq_u16(min_b, b);
        max_a = vmaxq_u16(max_a, a);
        max_b = vmaxq_u16(max_b, b);
      }
    }
    mn = vminvq_u16(vminq_u16(min_a, min_b));
    mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
  }
  for (; i < count; ++i) {
    uint16_t v;
    std::memcpy(&v, data + size_t(i) * 2, 2);
    if (rotate) {
      v = std::byteswap(v);
    }
    output[i] = v;
    mn = std::min<uint32_t>(mn, v);
    mx = std::max<uint32_t>(mx, v);
  }
  min = mn;
  max = mx;
}

// ds2-nx (ds2_indices16_restart): IndicesFrom16Neon with 0xFFFF (the restart index) left out of max: those lanes
// are cleared before the max (vbic with the vceq mask); min needs nothing (0xFFFF is never below a real index).
void DrawsDkImpl::IndicesFrom16NeonRestart(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate,
                                         uint32_t& min, uint32_t& max) {
  uint32_t i = 0;
  uint32_t mn = 0xFFFF;
  uint32_t mx = 0;
  if (count >= 16) {
    const uint16x8_t ones = vdupq_n_u16(0xFFFF);
    uint16x8_t min_a = ones, min_b = ones;
    uint16x8_t max_a = vdupq_n_u16(0), max_b = max_a;
    for (; i + 16 <= count; i += 16) {
      uint8x16_t ra = vld1q_u8(data + size_t(i) * 2), rb = vld1q_u8(data + size_t(i) * 2 + 16);
      if (rotate) {
        ra = vrev16q_u8(ra);
        rb = vrev16q_u8(rb);
      }
      const uint16x8_t a = vreinterpretq_u16_u8(ra), b = vreinterpretq_u16_u8(rb);
      vst1q_u16(output + i, a);
      vst1q_u16(output + i + 8, b);
      min_a = vminq_u16(min_a, a);
      min_b = vminq_u16(min_b, b);
      max_a = vmaxq_u16(max_a, vbicq_u16(a, vceqq_u16(a, ones)));
      max_b = vmaxq_u16(max_b, vbicq_u16(b, vceqq_u16(b, ones)));
    }
    mn = vminvq_u16(vminq_u16(min_a, min_b));
    mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
  }
  for (; i < count; ++i) {
    uint16_t v;
    std::memcpy(&v, data + size_t(i) * 2, 2);
    if (rotate) v = std::byteswap(v);
    output[i] = v;
    mn = std::min<uint32_t>(mn, v);
    if (v != 0xFFFF) mx = std::max<uint32_t>(mx, v);
  }
  min = mn;
  max = mx;
}

// The 16-bit indices of a draw (see sa2_native_indices_neon). Ring only.
void DrawsDkImpl::IndicesFrom16(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                              uint32_t& max, bool restart) {
  if (indices_neon_ < 0) {
    indices_neon_ = REXCVAR_GET(sa2_native_indices_neon) ? 1 : 0;
    REXLOG_INFO("[native] C6 16-bit indices: {}",
                indices_neon_ ? "with NEON; the first 20,000 draws are checked, then 1 in 4,096"
                              : "with the plain loop (sa2_native_indices_neon = false)");
  }
  if (indices_neon_ == 0) {
    IndicesFrom16Scale(data, count, output, rotate, min, max, restart);
    return;
  }
  IndicesFrom16Neon(data, count, output, rotate, min, max, restart);
  const uint64_t n = ++indices_neon_draws_;
  if (n > 20000 && (n & 4095) != 0) {
    return;
  }
  indices_neon_test_.resize(count);
  uint32_t mn = 0;
  uint32_t mx = 0;
  IndicesFrom16Scale(data, count, indices_neon_test_.data(), rotate, mn, mx, restart);
  ++indices_neon_checked_;
  if (mn != min || mx != max ||
      (count && std::memcmp(indices_neon_test_.data(), output, size_t(count) * 2) != 0)) {
    // The plain path stays for this draw and for the rest of the session.
    if (count) {
      std::memcpy(output, indices_neon_test_.data(), size_t(count) * 2);
    }
    min = mn;
    max = mx;
    indices_neon_ = 0;
    sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
    REXLOG_ERROR("[native] C6 16-bit indices: MISMATCH between NEON and the plain loop ({} indices, {}; min "
                 "{} vs {}, max {} vs {}). Switched off for the rest of the session: plain loop",
                 count, rotate ? "rotated" : "not rotated", min, mn, max, mx);
    return;
  }
  if (indices_neon_checked_ == 20000) {
    REXLOG_INFO("[native] C6 16-bit indices: 20,000 draws checked against the plain loop, 0 "
                "mismatches; still checking 1 in 4,096");
  }
}

/*
 * The early Z and invisible draws report, every 10 s like the others.
 *
 * What matters is not what is saved but the second figure: "CANNOT: they write depth"
 * is, draw by draw, the exact size of what only a depth pre-pass could address, and that is expensive
 * to implement. A small figure means a pre-pass is not worth the risk.
 */
void DrawsDkImpl::ReportZEarly() {
  ++frames_z_;
  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report_z_).count() < 10) {
    return;
  }
  last_report_z_ = now;
  const uint64_t seen = frames_z_ - frames_z_previous_;
  const double frames = double(seen ? seen : 1);
  std::array<uint64_t, kZCounts> d{};
  uint64_t total = 0;
  for (uint32_t i = 0; i < kZCounts; ++i) {
    d[i] = counts_z_[i] - counts_z_previous_[i];
    counts_z_previous_[i] = counts_z_[i];
    total += d[i];
  }
  frames_z_previous_ = frames_z_;
  ReportSky(frames);  // goes before the !total cutoff, which has nothing to do with the sky
  if (!total) {
    return;
  }
  const uint64_t late = d[kZSet] + d[kZWritesZ] + d[kZStencil];
  SA2_REPORT_RING(
      "[native] C6 early Z per frame: {:.0f} draws benefit, {:.0f} CANNOT because "
      "they write depth (that is the size of a depth pre-pass), {:.0f} because of stencil; {:.0f} "
      "already tested depth first. Of those that forced shading "
      "first, {:.0f} % are fixed{}",
      double(d[kZSet]) / frames, double(d[kZWritesZ]) / frames,
      double(d[kZStencil]) / frames,
      double(d[kZAlreadyEarly]) / frames,
      late ? 100.0 * double(d[kZSet]) / double(late) : 0.0,
      z_early_no_module_ ? fmt::format(" ({} shaders without a patched module)", z_early_no_module_)
                             : std::string());
  if (d[kInvisibleBlend] || d[kInvisibleAlpha] || d[kInvisibleColorOnly]) {
    SA2_REPORT_RING(
        "[native] C6 invisible draws per frame: {:.1f} with a blend that copies the destination, {:.1f} "
        "with alpha test set to NEVER; {:.1f} are left with depth only",
        double(d[kInvisibleBlend]) / frames, double(d[kInvisibleAlpha]) / frames,
        double(d[kInvisibleColorOnly]) / frames);
  }
}

/*
 * The deferred sky report. Without it there is no way to measure whether it works.
 *
 * What to check: "deferred" must be ~0.9 per frame in play (the sky is a single draw and is
 * sometimes absent). "lost" must be 0: if not, some path closes the pass without going through
 * FinishPass and the sky is being lost. And "position" says how many draws of the pass were recorded
 * between the old place and the new one: exactly the amount of geometry that now covers the sky before
 * it is shaded. If it is small, so is the saving.
 */
void DrawsDkImpl::ReportSky(double frames) {
  const uint64_t postponed = postponed_sky_count_ - postponed_previous_sky_;
  const uint64_t seen = seen_sky_ - seen_previous_sky_;
  if (!seen) {
    return;
  }
  std::array<uint64_t, kSkyReasons> per_reason{};
  uint64_t emitted = 0;
  for (uint32_t i = 0; i < kSkyReasons; ++i) {
    per_reason[i] = emitted_sky_[i] - emitted_previous_sky_[i];
    emitted_previous_sky_[i] = emitted_sky_[i];
    emitted += per_reason[i];
  }
  const uint64_t position_sum = sky_position_sum_ - sky_position_previous_sum_;
  const uint64_t candidates = sky_candidates_ - sky_previous_candidates_;
  sky_previous_candidates_ = sky_candidates_;
  seen_previous_sky_ = seen_sky_;
  postponed_previous_sky_ = postponed_sky_count_;
  sky_position_previous_sum_ = sky_position_sum_;
  /*
   * The line that decides whether the criterion works. "with the dome geometry" is the number that
   * matters: it must be 1.00 per frame. The "detected" ones are by fingerprint only and once gave 7.00,
   * which is what broke the image.
   */
  SA2_REPORT_RING("[native] C6 sky: {:.2f} detected by fingerprint and {:.2f} with the dome geometry "
              "(480 indices and first in the pass) per frame. The second must be 1.00. "
              "GUARD: {} ({} frames with a dome examined, {} with exactly one, max {} in "
              "one frame)",
              double(seen) / frames, double(candidates) / frames,
              sky_guard_ == kSkyPostponing  ? "PASSED, deferring"
              : sky_guard_ == kDiscardedSky ? "DISCARDED, nothing is deferred"
                                                   : "still looking",
              sky_guard_frames_, sky_guard_with_one_, sky_guard_max_);
  SA2_REPORT_RING(
      "[native] C6 deferred sky ({}): {:.2f} detected per frame, {:.2f} really deferred "
      "({} not deferrable in total, {} with another sky already pending); emitted {} when a draw "
      "with blending arrived, {} when an opaque draw that does not write Z arrived, {} at pass close, {} by another sky; "
      "LOST {}; average position {:.0f} draws ahead of it (max {})",
      postponed_sky_ ? "on" : "OFF", double(seen) / frames,
      double(postponed) / frames, sky_no_postponable_, sky_two_in_pass_,
      per_reason[kSkyPerBlend], per_reason[kSkyPerNoZ], per_reason[kSkyPerPassEnd],
      per_reason[kSkyPerOtherSky], lost_sky_,
      emitted ? double(position_sum) / double(emitted) : 0.0, sky_position_max_);
}

/*
 * Closes the frame for the sky guard. See the block of fields.
 *
 * Runs once per frame and does nothing once the guard has decided, so its cost is one comparison. The
 * decision is taken only once per session and logged.
 */
void DrawsDkImpl::CloseFrameOfTheGuardOfTheSky() {
  const uint32_t in_this = sky_guard_in_frame_;
  sky_guard_in_frame_ = 0;
  if (sky_guard_ != kSkyLooking) {
    return;
  }
  /*
   * A frame without a sky says nothing about the criterion: menus, the logo and loading screens, which
   * come before gameplay. If they counted, the test would run out in the menu and the guard would switch
   * off for good without ever seeing the dome. Only frames in which the dome appears count.
   */
  if (in_this == 0) {
    return;
  }
  sky_guard_max_ = std::max(sky_guard_max_, in_this);
  if (in_this == 1) {
    ++sky_guard_with_one_;
  }
  if (++sky_guard_frames_ < kSkyFramesTest) {
    return;
  }
  /*
   * The test is over. It only switches on if it came out clean: never more than one, and enough frames
   * with exactly one. In any other case it stays off and the image is identical to the non-deferred one,
   * which is known to work.
   */
  if (sky_guard_max_ == 1 && sky_guard_with_one_ >= kSkyFramesWithOne) {
    sky_guard_ = kSkyPostponing;
    REXLOG_INFO("[native] C6 sky: guard PASSED ({} of {} frames with exactly one dome, "
                "never two). The sky is deferred from now on: that is 3.4-4.3 ms of GPU without "
                "changing a pixel",
                sky_guard_with_one_, sky_guard_frames_);
  } else {
    sky_guard_ = kDiscardedSky;
    REXLOG_WARN("[native] C6 sky: guard NOT passed (max {} domes in one frame, {} of {} "
                "frames with exactly one; {} were needed). NOTHING is deferred: the image "
                "stays exactly as it was",
                sky_guard_max_, sky_guard_with_one_, sky_guard_frames_,
                kSkyFramesWithOne);
  }
}


// bit L set = the fragment module reads input Location L. Vertex shaders (package v30+) drop
// every output the pixel shader does not read through specialization constant 60 (NVK links no
// varyings: unread outputs and their math were computed for every vertex). Unknown code: all bits.
// [dk] The mask is the DKSH package's (the key its vertex variants were built with) and, when the package does not
// know it, the library SPIR-V's. The Vulkan code read the final module's code (an alpha-only PS may read fewer
// inputs): the library's gives a superset, so the image is the same and only some pruning can be lost.
uint32_t DrawsDkImpl::InputsMaskPS(const ShaderEntry* ps) {
  if (!ps || !REXCVAR_GET(sa2_native_vs_pruned_outputs)) return 0xFFFFFFFFu;
  std::lock_guard<std::mutex> lock(masks_inputs_mutex_);
  if (const auto it = masks_inputs_ps_.find(ps); it != masks_inputs_ps_.end()) return it->second;
  const LibraryDksh* dksh = dksh_;  // set by the ring (ModuleFor) before any pipeline exists
  const uint32_t package = dksh && dksh->loaded() ? dksh->InputsMaskPs(ps->number) : 0xFFFFFFFFu;
  const uint32_t spirv = ps->shader ? InputsMaskOfSpirvDk(ps->shader->Spirv()) : 0xFFFFFFFFu;
  const uint32_t mask = package != 0xFFFFFFFFu ? package : spirv;
  if (package != 0xFFFFFFFFu && spirv != 0xFFFFFFFFu && package != spirv)
    REXLOG_WARN("[native] [dk] PS n{} input mask: package {:08X}, SPIR-V {:08X}; the package's is used (its "
                "vertex variants were built with it)", ps->number, package, spirv);
  masks_inputs_ps_.emplace(ps, mask);
  return mask;
}

const DkShader* DrawsDkImpl::ModuleFor(const ShaderEntry* entry, uint32_t spec, uint32_t mask60, bool* generic) {
  if (generic) *generic = false;
  if (!entry) return nullptr;
  if (!dksh_) dksh_ = ActiveLibraryDksh();
  if (!dksh_ || !dksh_->loaded()) {
    Warn(52, "[dk] no DKSH package (sa2_shaders.dkp): no shader module can be made");
    return nullptr;
  }
  // The Vulkan rewrites and the bit (or key) each one is chosen by. The generic DKSH is the shader without any
  // of them: for the image-preserving ones (alpha only, 7e3 clamp, early Z) the Vulkan code also falls back to
  // the regular module when the rewrite is not possible; the others it rejects.
  enum : uint32_t { kLogRectangle = 1, kLogGrid, kLogHalf, kLogQuantize, kLogAlphaOnly, kLog7e3, kLogZEarly };
  const auto first_log = [&](uint32_t kind) {
    return variants_logged_.insert((uint64_t(entry->number) << 8) | kind).second;
  };
  const bool vertex = entry->vertices;
  // Guest XY raster mode (ModuleGuestFragCoordXY): kSpecRasterGridX, or the phase bits of key.rasterization that
  // the pixel stage carries in mask60 (PsVariantMask60).
  const bool grid = !vertex && (spec & kSpecRasterGridX);
  const bool phase = !vertex && (mask60 & me::native::kNativeMsaa2PhaseProbe);
  const bool phase2 = !vertex && (mask60 & me::native::kNativeMsaa2PhaseMode2);
  if ((grid || phase) && (grid == phase || (phase2 && !phase))) {
    const uint64_t n = ++raster_grid_ps_rejected_;
    if (n <= 16 || (n & 255) == 0)
      REXLOG_ERROR("[native] expanded raster grid rejected final PS (reject {}): {}; PS n{} container {:016X}, "
                   "gridX={} phase={} raster={:08X} specialization={:08X}",
                   n, "unsupported combined or absent guest XY raster mode", entry->number, entry->fingerprint,
                   grid, phase, mask60, spec);
    return nullptr;
  }
  bool is_generic = false;
  const DkShader* shader = dksh_->Variant(entry->number, spec, mask60, code_arena_, &is_generic);
  if (!shader) {
    if (first_log(0))
      REXLOG_ERROR("[native] [dk] {} n{}: the DKSH package has neither the variant {:08X}/{:08X} nor the generic "
                   "shader", vertex ? "VS" : "PS", entry->number, spec, mask60);
    return nullptr;
  }
  if (is_generic) {
    // Rewrites the image depends on: the Vulkan code rejects the draw rather than use the plain shader.
    if (vertex && (spec & kSpecRectangle)) {
      if (first_log(kLogRectangle))
        REXLOG_ERROR("[native] [dk] rectangle VS n{}: no expanded variant in the DKSH package; rejecting, NOT "
                     "drawing the unexpanded VS", entry->number);
      return nullptr;
    }
    if (grid || phase) {
      const uint64_t n = ++raster_grid_ps_rejected_;
      if (n <= 16 || (n & 255) == 0)
        REXLOG_ERROR("[native] expanded raster grid rejected final PS (reject {}): {}; PS n{} container {:016X}, "
                     "gridX={} phase={} raster={:08X} specialization={:08X}",
                     n, "[dk] no FragCoord XY variant in the DKSH package", entry->number, entry->fingerprint,
                     grid, phase, mask60, spec);
      return nullptr;
    }
    if (!vertex && (spec & kSpecDepthFloat24Quantize)) {
      if (first_log(kLogQuantize))
        REXLOG_ERROR("[native] FLOAT24 final PS transform failed n{} round={}: {}; rejecting, NOT using an "
                     "unquantized fallback", entry->number, bool(spec & kSpecDepthFloat24Round),
                     "[dk] no quantized variant in the DKSH package");
      return nullptr;
    }
    if (!vertex && (spec & kSpecDepthFloat24Half)) {
      if (first_log(kLogHalf))
        REXLOG_ERROR("[native] final PS depth half transform failed (n{}): {}; rejecting, NOT using unhalved PS",
                     entry->number, "[dk] no depth half variant in the DKSH package");
      return nullptr;
    }
  }
  // The image-preserving rewrites, in the Vulkan order of precedence (7e3, else alpha only, else early Z): logged
  // once per shader as the Vulkan code logs each module it creates.
  if (!vertex && (spec & kSpecTarget7e3)) {
    if (first_log(kLog7e3))
      REXLOG_INFO("[native] C6: PS n{} 7e3 output mask {:X}: {}", entry->number,
                  (spec & kSpecMask7e3) >> kSpecMask7e3Displacement,
                  is_generic ? "the regular module is used" : "limited");
  } else if (!vertex && (spec & kSpecAlphaOnly)) {
    if (first_log(kLogAlphaOnly))
      REXLOG_INFO("[native] C5a: PS n{} without color writes: {}", entry->number,
                  is_generic ? "[dk] no variant, the regular one is used" : "[dk] variant from the package");
  } else if (!vertex && (spec & kSpecZEarly)) {
    if (first_log(kLogZEarly)) {
      REXLOG_INFO("[native] C6 early Z: PS n{} {} ({} kills)", entry->number,
                  is_generic ? std::string("stays as it was: [dk] no early Z variant in the DKSH package")
                             : std::string("tests depth before shading"),
                  entry->kills);
      if (is_generic) ++z_early_no_module_;
    }
  }
  if (generic) *generic = is_generic;
  return shader;
}

const DkShader* DrawsDkImpl::ModuleDepthOnlyQuantize(bool round_float24) {
  // [dk] me::native::MakeDepthOnlyFragmentForQuantization + TransformDepthHalf + TransformDepthQuantizeIncoming
  // made this stage from nothing in SPIR-V; it needs an embedded DKSH (region 07) that does not exist yet.
  if (variants_logged_.insert(round_float24 ? 0xFFFFFFFF01ull : 0xFFFFFFFF00ull).second)
    REXLOG_ERROR("[native] FLOAT24 selected PS creation failed; rejecting, NOT substituting a depth-only PS "
                 "([dk] no DKSH for the synthetic depth-only stage, round={})", round_float24);
  return nullptr;
}

// ======================================== part E ========================================
// ---- parte E: fixed state, canonical key, dynamic state, PipelineFor (Vulkan 12147-13221) ----
// Needs (integrator): #include "sa2_dk_state_tables.h" with the other includes of sa2_native_draws_dk_2.cpp.


// The numeric tables of sa2_dk_state_tables.h are deko3d's enums.
static_assert(dk_tables::kFactorZero == DkBlendFactor_Zero && dk_tables::kFactorOne == DkBlendFactor_One &&
              dk_tables::kFactorSrcColor == DkBlendFactor_SrcColor &&
              dk_tables::kFactorInvSrcColor == DkBlendFactor_InvSrcColor &&
              dk_tables::kFactorSrcAlpha == DkBlendFactor_SrcAlpha &&
              dk_tables::kFactorInvSrcAlpha == DkBlendFactor_InvSrcAlpha &&
              dk_tables::kFactorDstAlpha == DkBlendFactor_DstAlpha &&
              dk_tables::kFactorInvDstAlpha == DkBlendFactor_InvDstAlpha &&
              dk_tables::kFactorDstColor == DkBlendFactor_DstColor &&
              dk_tables::kFactorInvDstColor == DkBlendFactor_InvDstColor &&
              dk_tables::kFactorSrcAlphaSaturate == DkBlendFactor_SrcAlphaSaturate &&
              dk_tables::kFactorConstColor == DkBlendFactor_ConstColor &&
              dk_tables::kFactorInvConstColor == DkBlendFactor_InvConstColor &&
              dk_tables::kFactorConstAlpha == DkBlendFactor_ConstAlpha &&
              dk_tables::kFactorInvConstAlpha == DkBlendFactor_InvConstAlpha,
              "sa2_dk_state_tables.h: DkBlendFactor values");
static_assert(dk_tables::kOpAdd == DkBlendOp_Add && dk_tables::kOpSub == DkBlendOp_Sub &&
              dk_tables::kOpRevSub == DkBlendOp_RevSub && dk_tables::kOpMin == DkBlendOp_Min &&
              dk_tables::kOpMax == DkBlendOp_Max,
              "sa2_dk_state_tables.h: DkBlendOp values");
static_assert(uint32_t(DkFace_Front) == uint32_t(VK_CULL_MODE_FRONT_BIT) &&
              uint32_t(DkFace_Back) == uint32_t(VK_CULL_MODE_BACK_BIT),
              "the cull bits of the key are DkFace as they are VkCullModeFlags");

// warn = false from the prewarm thread (warnings belong to the ring only).
void DrawsDkImpl::FillFixedState(const PipelineKey& key, FixedPipelineState& fixed, bool warn) {
  // [dk] FixedState() already holds the dk*StateDefaults; every field the Vulkan code sets (or leaves at the
  // zero of its Vk*CreateInfo) is set here, so two keys with the same Vulkan state give the same bytes (the
  // state tracker and EffectiveDifference compare them).
  fixed.primitive = PrimitiveDk(key.topology);
  fixed.restart = (key.rasterization & 0x8) != 0;

  // [dk] VkPipelineViewportStateCreateInfo (one viewport, one scissor) has no deko3d state: the draw sets
  // viewport 0 and scissor 0 with dkCmdBufSetViewports / dkCmdBufSetScissors.

  DkRasterizerState& rasterization = fixed.raster;
  rasterization.rasterizerEnable = 1;  // rasterizerDiscardEnable = VK_FALSE
  rasterization.fillRectangleEnable = 0;
  rasterization.polygonModeFront = DkPolygonMode_Fill;
  rasterization.polygonModeBack = DkPolygonMode_Fill;
  rasterization.cullMode = DkFace(((key.rasterization & 0x1) ? DkFace_Front : 0) |
                                  ((key.rasterization & 0x2) ? DkFace_Back : 0));
  // PA_SU_SC_MODE_CNTL.face: 1 = the front face is clockwise (not confirmed on screen).
  // [dk] DkFrontFace numbers CW 0 / CCW 1 (VkFrontFace CCW 0 / CW 1). The winding is seen after the viewport;
  // with sa2::dk::SetViewport (NegativeY swizzle or plain viewport for Vulkan's negative height) the same face
  // name keeps every cull case as in Vulkan (bench-semantica test 6, 2026-11-18).
  rasterization.frontFace = (key.rasterization & 0x4) ? DkFrontFace_CW : DkFrontFace_CCW;
  // [dk] Vulkan's provoking vertex is the first one (no VK_EXT_provoking_vertex here); deko3d's default is the last.
  rasterization.provokingVertex = DkProvokingVertex_First;
  rasterization.polygonSmoothEnableMask = 0;
  // sa2-nx render resolution: a line pipeline's scale (PipelineKey::fill bits 8-15); 1 otherwise, as before.
  const uint32_t line_q = (key.fill >> 8) & 0xFF;
  fixed.line_width = line_q ? std::clamp(float(line_q) * 0.25f, 1.0f, max_line_width_) : 1.0f;
  rasterization.depthBiasEnableMask = (key.rasterization & 0x10) ? DkPolygonFlag_All : 0;
  rasterization.depthClampEnable = (key.rasterization & me::native::kNativeDepthClamp) ? 1 : 0;

  DkMultisampleState& sampling = fixed.ms;
  // sa2_msaa: key.fill bits 0-7 are the pass's host sample count (0 = single sample, as before).
  const uint32_t samples = (key.fill & 0xFF) > 1 ? (key.fill & 0xFF) : 1;
  sampling.mode = sa2::dk::FromSamples(samples);
  sampling.rasterizerMode = sampling.mode;

  const uint32_t d = key.depth;
  DkDepthStencilState& depth = fixed.depth;
  // [dk] The zero of VkPipelineDepthStencilStateCreateInfo (no test, no write, no stencil, NEVER, KEEP): deko3d's
  // defaults enable the depth test, so without a depth target it is cleared explicitly.
  depth.depthTestEnable = 0;
  depth.depthWriteEnable = 0;
  depth.stencilTestEnable = 0;
  depth.depthCompareOp = CompareDk(0);
  depth.stencilFrontFailOp = depth.stencilFrontPassOp = depth.stencilFrontDepthFailOp = StencilDk(0);
  depth.stencilFrontCompareOp = CompareDk(0);
  depth.stencilBackFailOp = depth.stencilBackPassOp = depth.stencilBackDepthFailOp = StencilDk(0);
  depth.stencilBackCompareOp = CompareDk(0);
  if (key.formats[4]) {
    depth.depthTestEnable = (d >> 1) & 0x1;
    depth.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
    depth.depthCompareOp = CompareDk((d >> 4) & 0x7);
    depth.stencilTestEnable = d & 0x1;
    depth.stencilFrontFailOp = StencilDk((d >> 11) & 0x7);
    depth.stencilFrontPassOp = StencilDk((d >> 14) & 0x7);
    depth.stencilFrontDepthFailOp = StencilDk((d >> 17) & 0x7);
    depth.stencilFrontCompareOp = CompareDk((d >> 8) & 0x7);
    if ((d >> 7) & 0x1) {
      depth.stencilBackCompareOp = CompareDk((d >> 20) & 0x7);
      depth.stencilBackFailOp = StencilDk((d >> 23) & 0x7);
      depth.stencilBackPassOp = StencilDk((d >> 26) & 0x7);
      depth.stencilBackDepthFailOp = StencilDk((d >> 29) & 0x7);
    } else {
      depth.stencilBackFailOp = depth.stencilFrontFailOp;
      depth.stencilBackPassOp = depth.stencilFrontPassOp;
      depth.stencilBackDepthFailOp = depth.stencilFrontDepthFailOp;
      depth.stencilBackCompareOp = depth.stencilFrontCompareOp;
    }
  }

  // [dk] kFactors and the `operation` lambda are dk_tables::BlendFactorXenos / BlendOpXenos
  // (sa2_dk_state_tables.h, same mapping, with a host test): deko3d numbers both enums from 1.
  const auto operation = [&](uint32_t op) {
    bool reverse = false;
    const DkBlendOp result = DkBlendOp(dk_tables::BlendOpXenos(op, &reverse));
    if (reverse && warn) {
      Warn(52, "blend with reverse subtract: subtract is used");
    }
    return result;
  };
  const auto factor = [](uint32_t field) { return DkBlendFactor(dk_tables::BlendFactorXenos(field)); };
  std::array<DkBlendState, 4>& blends = fixed.blend;
  uint32_t& n_colors = fixed.n_blend;
  // [dk] Per compacted target id (the targets BeginPass binds, in this same order): the write masks
  // (DkColorWriteState, 4 bits each) and the blend enable bits (DkColorState). Targets not bound: no writes.
  fixed.write.masks = 0;
  fixed.color.blendEnableMask = 0;
  fixed.color.logicOp = DkLogicOp_Copy;          // logicOpEnable = VK_FALSE
  fixed.color.alphaCompareOp = DkCompareOp_Always;  // no fixed-function alpha test (the shader does it)
  for (uint32_t i = 0; i < 4; ++i) {
    if (!key.formats[i]) {
      continue;
    }
    const uint32_t m = key.blend[i];
    const uint32_t a = n_colors++;
    DkBlendState& s = blends[a];
    const uint32_t color_write_mask = (key.masks >> (i * 4)) & 0xF;
    dkColorWriteStateSetMask(&fixed.write, a, color_write_mask);
    const uint32_t src_data = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
    const uint32_t source_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7,
                   target_a = (m >> 24) & 0x1F;
    /*
     * Blending is only enabled if it is needed on the channels that are written.
     *
     * It used to require all six fields to be "1 x source + 0 x destination, ADD". But if the mask does
     * not write RGB, the three color factors do not matter, and the same goes for alpha. With blendEnable
     * false the ROP does not have to read the destination or go through the blend unit. The result is the
     * same.
     */
    const bool writes_rgb = (color_write_mask & 0x7) != 0;
    const bool writes_alpha = (color_write_mask & 0x8) != 0;
    const bool color_direct = src_data == 1 && target == 0 && op == 0;
    const bool direct_alpha = source_a == 1 && target_a == 0 && op_a == 0;
    dkColorStateSetBlendEnable(&fixed.color, a, (writes_rgb && !color_direct) || (writes_alpha && !direct_alpha));
    s.srcColorBlendFactor = factor(src_data);
    s.dstColorBlendFactor = factor(target);
    s.colorBlendOp = operation(op);
    s.srcAlphaBlendFactor = factor(source_a);
    s.dstAlphaBlendFactor = factor(target_a);
    s.alphaBlendOp = operation(op_a);
  }
  // [dk] blend.attachmentCount / pAttachments: n_blend states, bound from id 0 (dkCmdBufBindBlendStates).
}

// One stencil face, in what deko3d looks at in the state (masks and reference are dynamic).
bool DrawsDkImpl::SameFaceStencil(const DkDepthStencilState& a, const DkDepthStencilState& b, bool back) {
  if (back) {
    return a.stencilBackFailOp == b.stencilBackFailOp && a.stencilBackPassOp == b.stencilBackPassOp &&
           a.stencilBackDepthFailOp == b.stencilBackDepthFailOp && a.stencilBackCompareOp == b.stencilBackCompareOp;
  }
  return a.stencilFrontFailOp == b.stencilFrontFailOp && a.stencilFrontPassOp == b.stencilFrontPassOp &&
         a.stencilFrontDepthFailOp == b.stencilFrontDepthFailOp && a.stencilFrontCompareOp == b.stencilFrontCompareOp;
}

/*
 * Two fixed states, field by field, in what the GPU looks at: the blend equation only with blendEnable,
 * each half only if the mask writes its channels, the Z function only with a Z test, stencil operations
 * only with stencil. Returns the first differing field, or nullptr if there is none.
 */
const char* DrawsDkImpl::EffectiveDifference(const FixedPipelineState& a, const FixedPipelineState& b) {
  if (a.primitive != b.primitive) {
    return "topology";
  }
  if (a.restart != b.restart) {
    return "primitiveRestartEnable";
  }
  // [dk] viewportCount / scissorCount: no such state (always viewport 0 and scissor 0).
  const DkRasterizerState& ra = a.raster;
  const DkRasterizerState& rb = b.raster;
  if (ra.polygonModeFront != rb.polygonModeFront || ra.polygonModeBack != rb.polygonModeBack ||
      a.line_width != b.line_width || ra.rasterizerEnable != rb.rasterizerEnable ||
      ra.depthClampEnable != rb.depthClampEnable) {
    return "polygonMode, lineWidth, rasterizerDiscardEnable or depthClampEnable";
  }
  if (ra.fillRectangleEnable != rb.fillRectangleEnable || ra.provokingVertex != rb.provokingVertex ||
      ra.polygonSmoothEnableMask != rb.polygonSmoothEnableMask) {
    return "[dk] fillRectangleEnable, provokingVertex or polygonSmoothEnableMask";
  }
  if (ra.cullMode != rb.cullMode) {
    return "cullMode";
  }
  if (ra.frontFace != rb.frontFace) {
    return "frontFace";
  }
  if (ra.depthBiasEnableMask != rb.depthBiasEnableMask) {
    return "depthBiasEnable";
  }
  if (a.ms.mode != b.ms.mode || a.ms.rasterizerMode != b.ms.rasterizerMode) {
    return "rasterizationSamples";
  }
  const DkDepthStencilState& pa = a.depth;
  const DkDepthStencilState& pb = b.depth;
  if (pa.depthTestEnable != pb.depthTestEnable) {
    return "depthTestEnable";
  }
  if (pa.depthWriteEnable != pb.depthWriteEnable) {
    return "depthWriteEnable";
  }
  if (pa.depthTestEnable && pa.depthCompareOp != pb.depthCompareOp) {
    return "depthCompareOp";
  }
  // [dk] depthBoundsTestEnable: not in deko3d's state (depth bounds are dkCmdBufSetDepthBounds, never used).
  if (pa.stencilTestEnable != pb.stencilTestEnable) {
    return "stencilTestEnable";
  }
  if (pa.stencilTestEnable && !SameFaceStencil(pa, pb, false)) {
    return "front (stencil)";
  }
  if (pa.stencilTestEnable && !SameFaceStencil(pa, pb, true)) {
    return "back (stencil)";
  }
  if (a.n_blend != b.n_blend || a.color.logicOp != b.color.logicOp ||
      a.color.alphaCompareOp != b.color.alphaCompareOp) {
    return "attachmentCount or logicOpEnable";
  }
  for (uint32_t i = 0; i < a.n_blend && i < 4; ++i) {
    const DkBlendState& x = a.blend[i];
    const DkBlendState& y = b.blend[i];
    const uint32_t mask_x = (a.write.masks >> (i * 4)) & 0xF;
    const uint32_t mask_y = (b.write.masks >> (i * 4)) & 0xF;
    const bool enable_x = (a.color.blendEnableMask >> i) & 1;
    const bool enable_y = (b.color.blendEnableMask >> i) & 1;
    if (mask_x != mask_y) {
      return "colorWriteMask";
    }
    if (enable_x != enable_y) {
      return "blendEnable";
    }
    if (enable_x && (mask_x & 0x7) &&
        (x.srcColorBlendFactor != y.srcColorBlendFactor || x.dstColorBlendFactor != y.dstColorBlendFactor ||
         x.colorBlendOp != y.colorBlendOp)) {
      return "color equation";
    }
    if (enable_x && (mask_x & 0x8) &&
        (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor || x.dstAlphaBlendFactor != y.dstAlphaBlendFactor ||
         x.alphaBlendOp != y.alphaBlendOp)) {
      return "alpha equation";
    }
  }
  return nullptr;
}

/*
 * Phase 0a (sa2_native_canonical_key). A draw's key in canonical form. Same criterion as
 * CanonicalState (the counter) with two differences: disabled blending becomes 1 x source + 0 x
 * destination, ADD (0x00010001; with 0, FillFixedState would enable it with zero factors), and with
 * blending enabled the half of the equation (color or alpha) whose channels the mask does not write also
 * becomes that.
 */
void DrawsDkImpl::Canonicalize(PipelineKey& c) {
  constexpr uint32_t kDirect = 0x00010001u;  // 1 x source + 0 x destination, ADD, for color and alpha
  uint32_t masks = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!c.formats[i]) {
      c.blend[i] = 0;  // FillFixedState skips targets not in the pass (Draw leaves them at 0)
      continue;
    }
    const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
    masks |= mask << (i * 4);
    const uint32_t m = c.blend[i];
    const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
    const bool direct_alpha = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
    const bool writes_rgb = (mask & 0x7) != 0;
    const bool writes_alpha = (mask & 0x8) != 0;
    if (!((writes_rgb && !color_direct) || (writes_alpha && !direct_alpha))) {
      c.blend[i] = kDirect;  // without blendEnable the GPU ignores the equation
      continue;
    }
    uint32_t canonical = m;
    if (!writes_rgb) {
      canonical = (canonical & 0xFFFF0000u) | 0x00000001u;  // the color equation reaches no channel
    }
    if (!writes_alpha) {
      canonical = (canonical & 0x0000FFFFu) | 0x00010000u;  // neither does the alpha one
    }
    c.blend[i] = canonical;
  }
  c.masks = masks;
  uint32_t d = c.formats[4] ? c.depth : 0;  // ignored without a depth target
  d &= ~0x8u;                                      // bit 3 is not read
  if (!((d >> 1) & 0x1)) {
    d &= ~0x74u;  // without a Z test, write and function do not count
  }
  if (!(d & 0x1)) {
    d &= 0x77u;  // without stencil, its functions and operations do not count
  } else if (!((d >> 7) & 0x1)) {
    d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
  }
  c.depth = d;
}

/*
 * The phase 0a guard. Canonicalize only touches blending, masks and depth: the rest of the key must come
 * out the same, and the fixed state of both keys must match in everything the GPU looks at
 * (EffectiveDifference on FillFixedState). On a difference the phase switches off for the session and
 * false is returned.
 */
bool DrawsDkImpl::CheckCanonicalKey(const PipelineKey& raw, const PipelineKey& canonical, uint64_t n) {
  ++canonical_checked_;
  const char* field = nullptr;
  if (raw.vs != canonical.vs || raw.ps != canonical.ps || raw.entry != canonical.entry ||
      raw.topology != canonical.topology || raw.specialization != canonical.specialization ||
      raw.rasterization != canonical.rasterization || raw.fill != canonical.fill ||
      raw.fill2 != canonical.fill2 ||
      !Equal(raw.formats, canonical.formats, sizeof(raw.formats))) {
    field = "shaders, input, topology, specialization, rasterization or formats";
  } else {
    FixedPipelineState a;
    FixedPipelineState b;
    FillFixedState(raw, a);
    FillFixedState(canonical, b);
    field = EffectiveDifference(a, b);
  }
  if (field) {
    canonical_off_key_ = true;
    canonical_key_ = false;
    canonical_valid_ = false;
    sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
    REXLOG_ERROR("[native] C6 canonical key: MISMATCH in {} (check {}: VS {} PS {}, blend {:08X} -> "
                 "{:08X}, masks {:04X} -> {:04X}, depth {:08X} -> {:08X}). Switched off for the rest of the "
                 "session: lookup uses the usual key",
                 field, n, raw.vs, raw.ps, raw.blend[0], canonical.blend[0], raw.masks,
                 canonical.masks, raw.depth, canonical.depth);
    return false;
  }
  if (n == kCanonicalToCheck) {
    SA2_REPORT_RING("[native] C6 canonical key: {} changed keys checked field by field against the "
                         "usual one, 0 mismatches; still checking 1 in 4096",
                         n);
  }
  return true;
}

/*
 * The key used to look up a draw's pipeline. The raw one (key) is still used for everything else: the
 * deferred sky reads its blending (opaque_in_all) and the counter compares it. Phase 0a: canonical
 * form, remembering the previous draw's (the same raw key gives the same canonical one).
 */
PipelineKey DrawsDkImpl::SearchKey(const PipelineKey& key) {
  PipelineKey c = key;
  if (canonical_key_) {
    if (canonical_valid_ && Equal(&key, &canonical_raw_, sizeof(key))) {
      c = canonical_result_;
    } else {
      Canonicalize(c);
      if (!Equal(&c, &key, sizeof(c))) {
        const uint64_t n = ++canonical_changed_;
        if ((n <= kCanonicalToCheck || (n & 4095) == 0) && !CheckCanonicalKey(key, c, n)) {
          c = key;  // the guard saw a difference: this draw, and the rest of the session, use the usual one
        }
      }
      canonical_raw_ = key;
      canonical_result_ = c;
      canonical_valid_ = canonical_key_;
    }
  }
  // Phases 1 and 2. What goes through vkCmdSet* leaves the key, and the flag (fill2) keeps these
  // pipelines apart from the usual ones in the map, in the direct-mapped cache and in PipelineFor's
  // shortcut. [dk] eds_mode_ is always 0 (LoadDynamicState): the key keeps all its state.
  const uint32_t eds_mode = eds_mode_;
  if (eds_mode & kEds12) {
    c.depth = 0;
    c.rasterization = me::native::StaticRasterKey(c.rasterization);
    c.topology = RepresentativeTopology(c.topology);
  }
  if (eds_mode & kEds3) {  // phase 2: blending and masks go through vkCmdSet*; targets stay in the formats part of the key
    std::fill(std::begin(c.blend), std::end(c.blend), 0u);
    c.masks = 0;
  }
  c.fill2 = eds_mode;
  return c;
}

// Every 20 s, phase 0a (sa2_native_canonical_key).
void DrawsDkImpl::ReportCanonicalKey() {
  const auto now = std::chrono::steady_clock::now();
  if (now - canonical_report_ < std::chrono::seconds(20)) {
    return;
  }
  const bool first = canonical_report_ == std::chrono::steady_clock::time_point{};
  canonical_report_ = now;
  const uint64_t changed = canonical_changed_ - canonical_changed_previous_;
  canonical_changed_previous_ = canonical_changed_;
  const uint64_t pipelines = pipelines_.size();
  const uint64_t new_values = pipelines - std::min<uint64_t>(pipelines, canonical_pipelines_previous_);
  canonical_pipelines_previous_ = pipelines;
  if (first || (!changed && !new_values)) {
    return;
  }
  SA2_REPORT_RING("[native] C6 canonical key: {}; {} keys changed in 20 s ({} checked "
                       "since the start, {}); {} pipelines in the map, {} new in 20 s",
                       canonical_off_key_ ? "SWITCHED OFF by the guard"
                       : canonical_key_       ? "on"
                                               : "off",
                       changed, canonical_checked_,
                       canonical_checked_ >= kCanonicalToCheck ? "guard passed" : "checking", pipelines,
                       new_values);
}

// Every 20 s, phase 0b (sa2_native_pipeline_between_passes).
void DrawsDkImpl::ReportPipelineBetweenPasses() {
  const auto now = std::chrono::steady_clock::now();
  if (now - between_passes_report_ < std::chrono::seconds(20)) {
    return;
  }
  const bool first = between_passes_report_ == std::chrono::steady_clock::time_point{};
  between_passes_report_ = now;
  const uint64_t passes = started_passes_ - between_passes_previous_passes_;
  const uint64_t kept = passes_with_pipeline_ - between_kept_previous_passes_;
  between_passes_previous_passes_ = started_passes_;
  between_kept_previous_passes_ = passes_with_pipeline_;
  if (first || !passes) {
    return;
  }
  SA2_REPORT_RING("[native] C6 pipeline between passes: {}; {} passes started in 20 s, {} with a "
                       "bound pipeline that is kept (its first draw does not bind again if the key is the "
                       "same)",
                       pipeline_between_passes_ ? "kept" : "forgotten, as before", passes, kept);
}

// Without EDS3 the dynamic topology must be of the same class as the pipeline's: the lookup key carries
// one per class. Classes the ring does not draw stay as they are.
uint32_t DrawsDkImpl::RepresentativeTopology(uint32_t topology) {
  switch (ClassTopology(topology)) {
    case 1:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 2:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default:
      return topology;
  }
}

const char* DrawsDkImpl::NameEdsMode(uint32_t mode) {
  switch (mode & (kEds12 | kEds3)) {
    case kEds12:
      return "EDS1/EDS2 (phase 1)";
    case kEds3:
      return "EDS3 (phase 2)";
    case kEds12 | kEds3:
      return "EDS1/EDS2 and EDS3 (phases 1 and 2)";
    default:
      return "off: all state in the pipeline, as before";
  }
}

// The dynamic state of a raw key in the given mode (kEds12 | kEds3).
void DrawsDkImpl::EdsStateFor(const PipelineKey& c, EdsState& e, uint32_t mode) {
  e = EdsState{};
  if (mode & kEds12) {
    e.face = ((c.rasterization & 0x1) ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) |
             ((c.rasterization & 0x2) ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
    e.front = (c.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    e.reset = (c.rasterization & 0x8) ? 1u : 0u;
    e.bias = (c.rasterization & 0x10) ? 1u : 0u;
    e.topology = c.topology;
    if (c.formats[4]) {  // without a depth target everything stays zero, as in FillFixedState
      const uint32_t d = c.depth;
      e.test_z = (d >> 1) & 0x1;
      e.writes_z = ((d >> 1) & 0x1) & ((d >> 2) & 0x1);
      e.function_z = (d >> 4) & 0x7;
      e.stencil = d & 0x1;
      e.ahead[0] = (d >> 11) & 0x7;
      e.ahead[1] = (d >> 14) & 0x7;
      e.ahead[2] = (d >> 17) & 0x7;
      e.ahead[3] = (d >> 8) & 0x7;
      if ((d >> 7) & 0x1) {
        e.behind[0] = (d >> 23) & 0x7;
        e.behind[1] = (d >> 26) & 0x7;
        e.behind[2] = (d >> 29) & 0x7;
        e.behind[3] = (d >> 20) & 0x7;
      } else {
        std::memcpy(e.behind, e.ahead, sizeof(e.behind));
      }
    }
  }
  if (mode & kEds3) {  // phase 2: like FillFixedState, target by target and compacted
    for (uint32_t i = 0; i < 4; ++i) {
      if (!c.formats[i]) {
        continue;
      }
      const uint32_t a = e.n_colors++;
      const uint32_t m = c.blend[i];
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      const uint32_t src_data = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
      const uint32_t source_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, target_a = (m >> 24) & 0x1F;
      const bool color_direct = src_data == 1 && target == 0 && op == 0;
      const bool direct_alpha = source_a == 1 && target_a == 0 && op_a == 0;
      e.mask[a] = mask;
      e.active_blend[a] =
          (((mask & 0x7) != 0 && !color_direct) || ((mask & 0x8) != 0 && !direct_alpha)) ? VK_TRUE : VK_FALSE;
      VkColorBlendEquationEXT& q = e.equation[a];
      q.srcColorBlendFactor = kFactorsBlend[src_data];
      q.dstColorBlendFactor = kFactorsBlend[target];
      q.colorBlendOp = OperationBlend(op);
      q.srcAlphaBlendFactor = kFactorsBlend[source_a];
      q.dstAlphaBlendFactor = kFactorsBlend[target_a];
      q.alphaBlendOp = OperationBlend(op_a);
    }
  }
}

/*
 * [dk] The Vulkan code records here the vkCmdSet* calls of phases 1 and 2 (EDS1/EDS2 and EDS3) for what changed
 * since the last state set in the buffer. deko3d has no pipeline: every group (rasterizer, depth/stencil, color,
 * blend) is already set per draw by the state tracker from the pipeline's FixedState, so eds_mode_ stays 0 and
 * the sky replay and Draw never reach this with a mode. Kept so their calls read as before; it records nothing.
 */
void DrawsDkImpl::EmitDynamicState(DkCmdBuf cmd, const EdsState& d, bool all, uint32_t mode) {
  (void)cmd;
  (void)d;
  (void)mode;
  eds_calls_[kEdsAll] += all ? 1 : 0;
}

/*
 * Phases 1 and 2, before every draw with dynamic state. [dk] Never active (eds_mode_ = 0, see
 * EmitDynamicState): nothing to set or check, so the guard (CheckDynamicState, which compared against the
 * Vulkan fixed-state structures) is not translated.
 */
bool DrawsDkImpl::PinDynamicState(DkCmdBuf cmd, const PipelineKey& key) {
  (void)cmd;
  (void)key;
  ++eds_draws_;
  return true;
}

/*
 * Dynamic state phases 1 and 2. [dk] The Vulkan code asks the driver for the EDS1/EDS2 (core 1.3) and EDS3
 * vkCmdSet* functions. In deko3d all of that state is dynamic already, so neither phase is used.
 */
void DrawsDkImpl::LoadDynamicState() {
  eds12_available_ = false;
  eds3_available_ = false;
  eds_mode_ = 0;
  REXLOG_INFO("[native] C6 dynamic state: [dk] deko3d binds every state group per draw: phases 1 and 2 "
              "(EDS1/EDS2, EDS3) are not used");
}

// Every 20 s, dynamic state phases 1 and 2.
void DrawsDkImpl::ReportDynamicState() {
  const auto now = std::chrono::steady_clock::now();
  if (now - eds_report_ < std::chrono::seconds(20)) {
    return;
  }
  const bool first = eds_report_ == std::chrono::steady_clock::time_point{};
  eds_report_ = now;
  const std::array<uint64_t, kEdsN> n = eds_calls_;
  eds_calls_.fill(0);
  const uint64_t draws = eds_draws_ - eds_previous_draws_;
  const uint64_t repeated = eds_repeated_ - eds_repeated_previous_;
  const uint64_t frames = frame_ - eds_previous_frames_;
  eds_previous_draws_ = eds_draws_;
  eds_repeated_previous_ = eds_repeated_;
  eds_previous_frames_ = frame_;
  if (first || !draws) {
    return;
  }
  uint64_t calls = 0;
  for (size_t k = 0; k < kEdsN; ++k) {
    calls += k == kEdsAll ? 0 : n[k];
  }
  SA2_REPORT_RING(
      "[native] C6 dynamic state: {}; {} draws in 20 s ({:.0f} per frame; {} repeat the previous key) "
      "and {} vkCmdSet* ({:.3f} per draw; {} times everything: new buffer, sky or pass that forgets the "
      "pipeline) | face {}, front {}, topology {}, restart {}, bias {}, Z test {}, Z write {}, Z function "
      "{}, stencil {}, stencil operations {} | blend {}, equation {}, masks {} | pipelines created: {} "
      "regular, {} EDS1/EDS2, {} EDS3, {} with both | guard: {} checked ({})",
      eds_off_ ? "SWITCHED OFF by the guard" : NameEdsMode(eds_mode_), draws,
      frames ? double(draws) / double(frames) : 0.0, repeated, calls,
      double(calls) / double(draws), n[kEdsAll], n[kEdsFace], n[kEdsFront], n[kEdsTopology],
      n[kEdsReset], n[kEdsBias], n[kEdsTestZ], n[kEdsWritesZ], n[kEdsFunctionZ], n[kEdsStencil],
      n[kEdsStencilOps], n[kEdsActiveBlend], n[kEdsEquation], n[kEdsMask], pipelines_per_mode_[0],
      pipelines_per_mode_[kEds12], pipelines_per_mode_[kEds3], pipelines_per_mode_[kEds12 | kEds3],
      eds_checked_, eds_checked_ >= kEdsToCheck ? "guard passed" : "checking");
}

// Phase 2. The blend operation of a register field, like the lambda in FillFixedState.
VkBlendOp DrawsDkImpl::OperationBlend(uint32_t op) {
  switch (op) {
    case 1:
      return VK_BLEND_OP_SUBTRACT;
    case 2:
      return VK_BLEND_OP_MIN;
    case 3:
      return VK_BLEND_OP_MAX;
    case 4:
      Warn(52, "blend with reverse subtract: subtract is used");
      return VK_BLEND_OP_SUBTRACT;
    default:
      return VK_BLEND_OP_ADD;
  }
}

const PipelineDk* DrawsDkImpl::PipelineFor(const PipelineKey& key, const VerticesEntry& entry,
                                           const SubmissionDraw& p) {
  /*
   * One-entry shortcut.
   *
   * There are 105 pipelines in a whole session and 2,345 draws per frame, so consecutive draws almost always
   * repeat the key. The full lookup is an XXH3 of 80 bytes + an integer division by libstdc++'s prime
   * bucket count + two pointer hops (bucket and node, ~96 B) = 2-3 cache misses, all to end up comparing
   * the same 80 bytes this compares. Here there is just one memcmp on memory that is already hot.
   */
  if (last_valid_key_ && Equal(&last_key_, &key, sizeof(key))) {
    return last_pipeline_;
  }
  const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
  // The direct-mapped cache (sa2_native_pipelines_direct) before the map.
  CellPipeline& cell = cells_pipeline_[fingerprint & (kCellsPipeline - 1)];
  if (pipelines_direct_ && cell.pipeline != nullptr &&
      Equal(&cell.key, &key, sizeof(key))) {
    const uint64_t n = ++pipelines_direct_hits_;
    if (!(n <= kPipelinesToCheck || (n & 4095) == 0) ||
        CheckCellPipeline(fingerprint, key, cell.pipeline, n)) {
      last_key_ = key;
      last_pipeline_ = cell.pipeline;
      last_valid_key_ = true;
      return cell.pipeline;
    }
    // the guard saw a difference: this draw goes through the map, as before
  }
  if (pipelines_direct_) {
    ++pipelines_direct_failures_;
  }
  if (const auto it = pipelines_.find(fingerprint); it != pipelines_.end()) {
    if (Equal(&it->second.first, &key, sizeof(key))) {
      const PipelineDk* found = PipelineOf(it->second.second);
      last_key_ = key;
      last_pipeline_ = found;
      last_valid_key_ = true;
      if (pipelines_direct_) {  // the slot keeps the map's pair
        cell.key = key;
        cell.pipeline = found;
      }
      return found;
    }
    Warn(50, "pipeline fingerprint collision");
    return nullptr;
  }
  /*
   * [dk] The shaders. The Vulkan code builds each stage's module here: the rectangle VS (kSpecRectangle), the
   * PS limited to 7e3 (kSpecTarget7e3), without color writes (kSpecAlphaOnly) or with early tests
   * (kSpecZEarly), then the guest FragCoord XY remap (kSpecRasterGridX or the MSAA phase probe) and the
   * float24 depth quantize / half-range depth, all SPIR-V rewrites at run time. In deko3d those are DKSH variants
   * of the package, compiled offline and chosen by the same specialization bits: one ModuleFor per stage.
   * A missing variant falls back to the generic DKSH (correct for the specialization it reads from UBO 3, but it
   * has none of the rewrites). The Vulkan code draws without the 7e3 limit, the alpha-only pruning and the early
   * tests when they cannot be made (the image is the same, or the regular module is accepted), but rejects the
   * draw when the rectangle VS, the FragCoord XY remap or the depth rewrites cannot be made: so does this.
   */
  const bool quantize = (key.specialization & kSpecDepthFloat24Quantize) != 0;
  if (!key.ps && quantize) {
    // The Vulkan code synthesizes a depth-only fragment stage that writes the quantized depth
    // (ModuleDepthQuantize with no PS). There is no such DKSH yet.
    Reject(51, "[dk] no synthetic depth-only fragment stage for the float24 quantize");
    return nullptr;
  }
  // Vertex stage: also constant 60 = the outputs the pixel shader reads (ignored by older packages).
  // No fragment shader (depth-only draws): no output but the position is read.
  const uint32_t mask60 =
      !key.ps && REXCVAR_GET(sa2_native_vs_pruned_outputs) ? 0u : InputsMaskPS(key.ps ? p.ps : nullptr);
  bool generic_vs = false;
  bool generic_ps = false;
  const DkShader* vs = ModuleFor(p.vs, key.specialization, mask60, &generic_vs);
  // [dk] The MSAA phase probe's FragCoord XY modes (kNativeMsaa2PhaseProbe / kNativeMsaa2PhaseMode2) live in
  // key.rasterization, not in the specialization: the pixel stage's variant carries them as its constant 60
  // (PsVariantMask60); ModuleFor rejects the generic shader for them, as the Vulkan code rejects the draw.
  const DkShader* ps = key.ps ? ModuleFor(p.ps, key.specialization, PsVariantMask60(key), &generic_ps) : nullptr;
  if (vs == nullptr || (key.ps && ps == nullptr)) {
    Reject(51, "could not create a shader module");
    return nullptr;
  }
  constexpr uint32_t kRewritesPs = kSpecRasterGridX | kSpecDepthFloat24Half | kSpecDepthFloat24Quantize;
  if ((generic_vs && (key.specialization & kSpecRectangle)) ||
      (generic_ps && (key.specialization & kRewritesPs))) {
    Reject(51, "[dk] the DKSH package lacks the variant of a shader rewrite (rectangle VS, guest FragCoord XY, "
               "half-range or float24 depth): the generic shader would draw without it");
    return nullptr;
  }
  // The deko3d pipeline lives in CreatePipelineDk, the same function the prewarm would use from its thread;
  // what surrounds it stays here, as it was.
  uint32_t n_colors = 0;
  PipelineDk pipeline;
  const auto creation_start = std::chrono::steady_clock::now();
  const bool created =
      CreatePipelineDk(key, entry, vs, ps, mask60, generic_vs || generic_ps, true, pipeline, n_colors);
  if (!created) {
    Reject(53, "could not create a pipeline");
    pipeline = PipelineDk{};  // kept in the map without shaders: PipelineOf gives nullptr, as VK_NULL_HANDLE
  } else {
    ++pipelines_no_save_;
  }
  const uint64_t ns_creation = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - creation_start)
                                            .count());
  ns_pipelines_ += ns_creation;
  NotePipelineCreated(key, entry, p, created, ns_creation);  // List and measurement
  const auto inserted = pipelines_.emplace(fingerprint, std::make_pair(key, pipeline)).first;
  const PipelineDk* result = PipelineOf(inserted->second.second);  // the node never moves
  ++pipelines_per_mode_[key.fill2 & 3];  // Dynamic state report
  if (pipelines_direct_ && result != nullptr) {  // the same pair as the map
    cell.key = key;
    cell.pipeline = result;
  }
  if (pipelines_.size() <= 64) {
    REXLOG_INFO("[native] C6: pipeline {} (VS n{} fingerprint {:016X} PS n{} fingerprint {:016X}, topology {}, {} colors, "
                "depth {:08X}, blend {:08X}, specialization {}){}",
                pipelines_.size(), p.vs->number, p.vs->fingerprint, key.ps ? int(p.ps->number) : -1,
                key.ps ? p.ps->fingerprint : 0, key.topology, n_colors,
                key.depth, key.blend[0], key.specialization,
                generic_vs || generic_ps ? " [dk] generic DKSH (variant missing from the package)" : "");
  }
  return result;
}

/*
 * The deko3d pipeline for a key. It is CreatePipelineVulkan's usual code: the specialization constants and the
 * fixed state (FillFixedState). [dk] It only reads its arguments and spec_table_ (ring only: the table is not
 * locked); warn = false outside the ring.
 */
bool DrawsDkImpl::CreatePipelineDk(const PipelineKey& key, const VerticesEntry& entry, const DkShader* vs,
                                   const DkShader* ps, uint32_t mask60, bool generic, bool warn,
                                   PipelineDk& pipeline, uint32_t& n_colors_output) {
  pipeline = PipelineDk{};
  pipeline.vs = nullptr;  // set last: a PipelineDk with shaders is a created one (PipelineOf)
  // Specialization constant 0 = key.specialization on both stages; the vertex stage also gets constant 60 = the
  // outputs the pixel shader reads (mask60, InputsMaskPS). [dk] A specialized DKSH has them folded in; the
  // generic one reads them from UBO 3, so both stages always get their entry of the table (written once).
  pipeline.spec_vs = spec_table_.For(key.specialization, mask60);
  pipeline.spec_ps = spec_table_.For(key.specialization, 0);
  if (pipeline.spec_vs == DK_GPU_ADDR_INVALID || pipeline.spec_ps == DK_GPU_ADDR_INVALID) {
    if (warn) {
      Warn(53, "[dk] the UBO 3 specialization table is full");
    }
    return false;
  }
  // [dk] The vertex input (VkPipelineVertexInputStateCreateInfo) is not pipeline state in deko3d: the draw binds
  // VerticesLayoutFor(entry) when the entry changes. Only what vkCreateGraphicsPipelines would refuse is checked
  // here: a format with no deko3d equivalent, a location past 15 or an offset past 14 bits.
  if (!VerticesLayoutFor(entry).valid) {
    if (warn) {
      Warn(53, "[dk] vertex input with no deko3d equivalent (format, location or offset)");
    }
    return false;
  }
  // The fixed state is filled in by FillFixedState: it is the usual code, moved there without
  // changing any computation, so the guards of the canonical key and of dynamic state compare with what
  // really goes into the pipeline.
  FillFixedState(key, pipeline.state, warn);  // notify
  // [dk] VkPipelineDynamicStateCreateInfo: viewport, scissor, blend constants, stencil masks and reference and
  // depth bias are set per draw in deko3d too (dkCmdBufSetViewports/Scissors/BlendConst/Stencil/DepthBias);
  // fill2 (the EDS phases) is always 0. layout_pipeline_ and the render pass have no deko3d counterpart.
  pipeline.ps = ps;
  pipeline.generic = generic;
  pipeline.vs = vs;
  n_colors_output = pipeline.state.n_blend;
  return true;
}

// ======================================== part F ========================================
// ---- Part F: lines 13205-13749 and 14747-14761 of sa2_native_draws.cpp (the prewarm list, the shader preload,
// the prewarm thread, DestroyImage, the factory) ----------------------------------------------------------------
// Uses, all declared elsewhere: sa2_shaders_preload, sa2_native_pipelines_prewarm, sa2_native_vs_pruned_outputs
// (REXCVAR_DECLARE in sa2_native_draws_dk_types.h); g_dk_prewarm_current, g_dk_texcoords_unswapped (extern in
// sa2_native_draws_dk_types.h, defined in sa2_native_draws_dk_1.cpp); ActiveLibraryDksh() (region 07 /
// me_native_system, declared once at the top of sa2_native_draws_dk_2.cpp).

// [dk] The constant 60 the ring passes to LibraryDksh::Variant for a vertex shader, from the package's fact (the
// prewarm thread cannot read SPIR-V through the ring's InputsMaskPS). The same rule as the Vulkan code at 13117:
// without a pixel shader, 0 when sa2_native_vs_pruned_outputs prunes the outputs; otherwise the pixel shader's
// input mask, or every bit when the cvar is off or the mask is unknown.
static uint32_t Mask60ForPreloadDk(const LibraryDksh& dksh, const ShaderEntry* ps) {
  const bool pruned = REXCVAR_GET(sa2_native_vs_pruned_outputs);
  if (!ps) return pruned ? 0u : 0xFFFFFFFFu;
  return pruned ? dksh.InputsMaskPs(ps->number) : 0xFFFFFFFFu;
}

// [dk] The (entry, spec, mask60) keys of one record: the vertex shader with the pixel shader's mask, and the pixel
// shader (mask60 = 0). The spec is the whole key.specialization: LibraryDksh masks it per entry.
static void PreloadKeysOfRecordDk(const LibraryDksh& dksh, const RegisterPipeline& r, const ShaderEntry& vs,
                                  const ShaderEntry* ps,
                                  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>& keys) {
  keys.emplace_back(vs.number, r.key.specialization, Mask60ForPreloadDk(dksh, r.key.ps ? ps : nullptr));
  if (r.key.ps && ps) keys.emplace_back(ps->number, r.key.specialization, 0u);
}

// At start-up (Initialize): the previous session's list, before the thread exists. It is not a
// separate file: it is the part of the pipelines file that LoadCachePipelines read.
void DrawsDkImpl::LoadPipelinesList() {
  const std::filesystem::path path = PathFilePipelines();
  std::vector<uint8_t> data = std::move(read_list_);
  read_list_ = {};
  if (data.size() < kHeaderList ||
      data.size() > kHeaderList + kMaxRegistersList * sizeof(RegisterPipeline)) {
    data.clear();
  }
  const char* reason = nullptr;
  uint32_t header[4] = {};
  if (data.empty()) {
    reason = "no list: starting fresh in this session";
  } else {
    std::memcpy(header, data.data(), sizeof(header));
    if (header[0] != kMagicPipelinesList || header[1] != kVersionPipelinesList ||
        header[2] != sizeof(RegisterPipeline) ||
        data.size() != kHeaderList + size_t(header[3]) * sizeof(RegisterPipeline)) {
      reason = "list from another version or damaged: starting from scratch";
    }
  }
  uint32_t other_settings = 0;
  if (!reason) {
    // sa2_gpu_prewarm_current_keys: what this session's PipelineFor can put in a key. PipelineKey::fill bits 0-7 =
    // the sa2_msaa samples (0 without; read at start), bits 8-15 = the render scale of a line pipeline (0 at the
    // game's scale; either console mode's), nothing above; specialization bit 5 = sa2_gpu_texcoords_unswapped.
    const bool current_only = g_dk_prewarm_current.active;
    const uint32_t samples_code = msaa_samples_ > 1 ? msaa_samples_ : 0u;
    const auto line_code = [&](uint32_t q) {
      return q != kScaleQuartersNative && max_line_width_ > 1.0f ? q : 0u;
    };
    const uint32_t line_handheld = line_code(sa2::render::ScaleQuartersOf(false));
    const uint32_t line_docked = line_code(sa2::render::ScaleQuartersOf(true));
    const bool unswapped = g_dk_texcoords_unswapped.active;
    file_list_.reserve(header[3]);
    for (uint32_t i = 0; i < header[3]; ++i) {
      RegisterPipeline r;
      std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(RegisterPipeline), sizeof(r));
      if (r.n_attributes > RegisterPipeline::kMaxAttributes || r.n_bindings > RegisterPipeline::kMaxBindings) {
        continue;
      }
      bool other = false;
      if (current_only) {
        const uint32_t line = (r.key.fill >> 8) & 0xFFu;
        // With MSAA on, the passes it does not apply to (other formats, Direct3D's 4x Clear aliases) stay
        // single-sample: fill 0 is producible in every session.
        const bool producible =
            ((r.key.fill & 0xFFu) == samples_code || (r.key.fill & 0xFFu) == 0) && (r.key.fill >> 16) == 0 &&
            (ClassTopology(r.key.topology) == 1 ? (line == line_handheld || line == line_docked) : line == 0) &&
            ((r.key.specialization & kSpecTexcoordsUnswapped) != 0) == unswapped;
        other = !producible;
      }
      if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), file_list_.size()).second) {
        file_list_.push_back(r);
        other_settings_list_.push_back(other ? 1 : 0);
        other_settings += other ? 1 : 0;
      }
    }
    written_list_ = std::move(data);  // the writer thread rewrites it unchanged if only the cache changes
    if (other_settings) g_dk_prewarm_current.Use(other_settings);
  }
  state_list_.assign(file_list_.size(), kPendingList);
  REXLOG_INFO("[native] C6 prewarm: {} pipelines in the list from {}{}{}", file_list_.size(),
              path.string(), reason ? ": " : "", reason ? reason : "");
  if (other_settings) {
    REXLOG_INFO("[native] C6 prewarm: {} records of the list are for other settings (another sa2_msaa or render "
                "scale, or the other sa2_gpu_texcoords_unswapped state): kept in the list, not prewarmed "
                "(sa2_gpu_prewarm_current_keys)",
                other_settings);
  }
}

// Ring only (SaveCachePipelines): the file's list without the records the thread found missing their
// shaders, followed by this session's new ones. Above kMaxRegistersList the oldest are dropped.
std::vector<uint8_t> DrawsDkImpl::SerializePipelinesList() const {
  const size_t until = prewarmed_until_.load(std::memory_order_acquire);
  std::vector<const RegisterPipeline*> registers;
  registers.reserve(file_list_.size() + session_list_.size());
  for (size_t i = 0; i < file_list_.size(); ++i) {
    if (i < until && state_list_[i] == kListNoShader) {
      continue;
    }
    registers.push_back(&file_list_[i]);
  }
  for (const RegisterPipeline& r : session_list_) {
    registers.push_back(&r);
  }
  if (registers.size() > kMaxRegistersList) {
    registers.erase(registers.begin(), registers.begin() + std::ptrdiff_t(registers.size() - kMaxRegistersList));
  }
  std::vector<uint8_t> data(kHeaderList + registers.size() * sizeof(RegisterPipeline));
  const uint32_t header[4] = {kMagicPipelinesList, kVersionPipelinesList, uint32_t(sizeof(RegisterPipeline)),
                              uint32_t(registers.size())};
  std::memcpy(data.data(), header, sizeof(header));
  for (size_t i = 0; i < registers.size(); ++i) {
    std::memcpy(data.data() + kHeaderList + i * sizeof(RegisterPipeline), registers[i], sizeof(RegisterPipeline));
  }
  return data;
}

// Ring only (PipelineFor), with every pipeline it creates: the measurement for the report and the guard,
// and new ones go to the list. [dk] The list is the inventory of the DKSH variants the game uses (ds2-deko3d
// tools/variantes/lista_pipelines.py reads it): every record is exactly (VS, PS, specialization) of a pipeline.
void DrawsDkImpl::NotePipelineCreated(const PipelineKey& key, const VerticesEntry& entry, const SubmissionDraw& p,
                                      bool created, uint64_t ns) {
  const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
  if (const auto it = index_list_.find(fingerprint); it != index_list_.end()) {
    const size_t j = it->second;
    if (j < file_list_.size() && j < prewarmed_until_.load(std::memory_order_acquire) &&
        state_list_[j] == kPrewarmedList) {
      ++prewarmed_ring_;
      ns_prewarmed_ring_ += ns;
      if (ns >= kNsSlow) {
        ++prewarmed_slow_ring_;
      }
    } else {
      ++ring_of_list_;
      ns_ring_of_list_ += ns;
    }
    return;
  }
  ++new_ring_;
  ns_new_ring_ += ns;
  if (!created || !p.vs || !p.vs->shader || (key.ps && (!p.ps || !p.ps->shader)) ||
      entry.attributes.size() > RegisterPipeline::kMaxAttributes ||
      entry.bindings.size() > RegisterPipeline::kMaxBindings ||
      file_list_.size() + session_list_.size() >= kMaxRegistersList) {
    return;
  }
  RegisterPipeline r;
  r.key = key;
  r.vs_fingerprint = p.vs->shader->fingerprint;
  r.ps_fingerprint = key.ps ? p.ps->shader->fingerprint : 0;
  r.n_attributes = uint32_t(entry.attributes.size());
  for (uint32_t k = 0; k < r.n_attributes; ++k) {
    const AttributeVertices& a = entry.attributes[k];
    r.attributes[k] = {a.location, a.binding, uint32_t(a.format), a.offset};
  }
  r.n_bindings = uint32_t(entry.bindings.size());
  for (uint32_t k = 0; k < r.n_bindings; ++k) {
    r.strides[k] = entry.bindings[k].stride;
  }
  index_list_.emplace(fingerprint, SIZE_MAX);  // from this session: not recorded again
  session_list_.push_back(r);
  ++list_no_save_;
  ++ring_to_list_;
}

// Shader prefetch (sa2_shaders_preload). Ring only, on each submission until decided: with an indexed
// library (SPIR-V read on first use), a background thread reads now the SPIR-V of every shader in the prewarm
// list, so the ring does not read the SD card when it first needs them. It uses a file handle of its own;
// other threads only wait for it while it publishes one entry.
// [dk] The same thread then reads ahead the DKSH blobs of the list (LibraryDksh::Preload: one variant per stage
// and record, the generic one where the package lacks it), so the ring's LibraryDksh::Variant only copies them
// into code memory. The SPIR-V is still read in phase 1 of region 07 (the facts: HasShaderLight,
// HasStructuredCf, VsIndexUnused, InputsMaskPS), so its prefetch stays as it was.
void DrawsDkImpl::TryPreloadShaders() {
  if (decided_preload_) {
    return;
  }
  if (!REXCVAR_GET(sa2_shaders_preload)) {
    decided_preload_ = true;
    return;
  }
  const ShadersNative* library = ActiveLibrary();
  if (!library || !library->loaded()) {
    return;  // not yet
  }
  if (!dksh_) dksh_ = ActiveLibraryDksh();
  if (!dksh_) {
    return;  // [dk] the DKSH package is not opened yet (a package that failed to open is loaded() == false)
  }
  decided_preload_ = true;
  if (file_list_.empty()) {
    return;  // nothing to prefetch
  }
  std::vector<uint64_t> fingerprints;
  if (library->is_indexed()) {  // [dk] otherwise the whole package is resident: only the DKSH part below
    fingerprints.reserve(file_list_.size() * 2);
    for (const RegisterPipeline& r : file_list_) {
      for (const auto& [fingerprint, number] :
           {std::pair{r.vs_fingerprint, r.key.vs}, std::pair{r.ps_fingerprint, r.key.ps}}) {
        if (fingerprint) {
          fingerprints.push_back(fingerprint);
        } else if (number) {  // records written before the fingerprints (PrewarmedLoop does the same)
          if (const ShaderEntry* e = library->PerNumber(number - 1); e && e->shader) {
            fingerprints.push_back(e->shader->fingerprint);
          }
        }
      }
    }
    std::sort(fingerprints.begin(), fingerprints.end());
    fingerprints.erase(std::unique(fingerprints.begin(), fingerprints.end()), fingerprints.end());
  }
  // [dk] The DKSH keys, built here on the ring (file_list_ and the library do not change afterwards).
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> keys;
  LibraryDksh* const dksh = (dksh_ && dksh_->loaded()) ? dksh_ : nullptr;
  if (dksh) {
    keys.reserve(file_list_.size() * 2);
    for (size_t i = 0; i < file_list_.size(); ++i) {
      if (other_settings_list_[i]) continue;  // not producible in this session (sa2_gpu_prewarm_current_keys)
      const RegisterPipeline& r = file_list_[i];
      const ShaderEntry* vs = r.vs_fingerprint ? library->PerFingerprint(r.vs_fingerprint)
                                               : (r.key.vs ? library->PerNumber(r.key.vs - 1) : nullptr);
      const ShaderEntry* ps = r.ps_fingerprint ? library->PerFingerprint(r.ps_fingerprint)
                                               : (r.key.ps ? library->PerNumber(r.key.ps - 1) : nullptr);
      if (!vs || !vs->vertices || (r.key.ps && (!ps || ps->vertices))) continue;
      PreloadKeysOfRecordDk(*dksh, r, *vs, ps, keys);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  }
  if (fingerprints.empty() && keys.empty()) {
    return;
  }
  try {
    preload_thread_ = std::thread([this, library, dksh, fingerprints = std::move(fingerprints),
                                   keys = std::move(keys)] {
      rex::thread::set_current_thread_name("ME shader preload");
      // Low priority, never above the guest's (0x3B), like the prewarm thread: it only reads the SD card.
      for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
        if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
          break;
        }
      }
      const auto start = std::chrono::steady_clock::now();
      if (!fingerprints.empty()) {
        const sa2::native::StatsPreload r = library->PreloadSpirv(fingerprints, &preload_stop_);
        REXLOG_INFO("[native] U1 shader preload: {} entries read ({:.1f} MB of SPIR-V) and {} already resident out of "
                    "{} fingerprints from the pipeline list, in {} ms{}{}",
                    r.inputs, double(r.bytes) / (1024.0 * 1024.0), r.already_resident, fingerprints.size(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count(),
                    r.error ? "; read or verification ERROR, stopped" : "",
                    preload_stop_.load(std::memory_order_relaxed) ? "; stopped" : "");
      }
      if (dksh && !keys.empty() && !preload_stop_.load(std::memory_order_relaxed)) {
        const auto start_dksh = std::chrono::steady_clock::now();
        const uint32_t read = dksh->Preload(keys, &preload_stop_);
        REXLOG_INFO("[native] U1 [dk] DKSH preload: {} variants read ahead for {} (shader, specialization) keys of "
                    "the pipeline list, in {} ms{}",
                    read, keys.size(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                          start_dksh)
                        .count(),
                    preload_stop_.load(std::memory_order_relaxed) ? "; stopped" : "");
      }
    });
  } catch (const std::system_error& error) {
    REXLOG_WARN("[native] U1 shader preload: could not create the thread ({})", error.what());
  }
}

void DrawsDkImpl::StopPreloadShaders() {
  preload_stop_.store(true, std::memory_order_relaxed);
  if (preload_thread_.joinable()) {
    preload_thread_.join();  // at most one entry read
  }
}

// Ring only, on each submission: starts the thread once, as soon as the library is loaded.
// [dk] There is no pipeline to compile and no driver cache to fill: what the thread can still do ahead of the
// ring is check every record against the library (records whose shaders are gone are dropped from the list when
// it is written, as in Vulkan) and read ahead each record's DKSH variants. The guard keeps its meaning: a ring
// that still takes 20 ms or more to create a record the thread already handled means the thread prepares other
// variants than the ring asks for.
void DrawsDkImpl::TryPrewarm() {
  TryPreloadShaders();
  if (prewarmed_decided_) {
    return;
  }
  if (!dksh_) dksh_ = ActiveLibraryDksh();
  const bool has_package = dksh_ && dksh_->loaded();
  if (file_list_.empty() || !REXCVAR_GET(sa2_native_pipelines_prewarm)) {
    prewarmed_decided_ = true;
    if (!file_list_.empty()) {
      REXLOG_INFO("[native] C6 prewarm: not prewarming (sa2_native_pipelines_prewarm = false)");
    }
    return;
  }
  const ShadersNative* library = ActiveLibrary();
  if (!library || !library->loaded() || !has_package) {
    return;  // not yet: check again on the next submission ([dk] the DKSH package instead of the layout)
  }
  prewarmed_decided_ = true;
  prewarmed_library_ = library;
  prewarmed_eds_ = eds_mode_;  // the ring would not request those of another dynamic state mode
  prewarmed_start_ = std::chrono::steady_clock::now();
  try {
    prewarmed_thread_ = std::thread([this] { PrewarmedLoop(); });
    REXLOG_INFO("[native] C6 prewarm: thread created for {} pipelines from the list", file_list_.size());
  } catch (const std::system_error& error) {
    REXLOG_WARN("[native] C6 prewarm: could not create the thread ({}); not prewarming", error.what());
  }
}

// The thread. It only reads file_list_, the library, the layout and the cache; it writes
// state_list_[i] before publishing prewarmed_until_ = i + 1, and its atomic counters. Its modules
// and render pass are its own and it destroys them when done.
// [dk] It reads file_list_, the shader library and the DKSH package (LibraryDksh::Preload has a file handle of its
// own and touches no code memory: thread safe). "Prewarmed" = both stages' variants read ahead; "really compiled"
// becomes "read from the SD card now" (Preload returned blobs it did not have yet), with its time.
void DrawsDkImpl::PrewarmedLoop() {
  rex::thread::set_current_thread_name("SA2 pipeline prewarm");
  int32_t priority = -1;
  // The lowest priority the system accepts, and never above the guest's (0x3B): compiling at the priority
  // threads are born with would take the core from the game and the ring. If none is accepted, nothing is
  // compiled.
  for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
    if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
      priority = candidate;
      break;
    }
  }
  if (priority < 0) {
    REXLOG_WARN("[native] C6 prewarm: the system accepts no priority from 0x3B to 0x3F: not "
                "prewarming");
    prewarmed_finished_.store(true, std::memory_order_release);
    return;
  }
  prewarmed_priority_.store(priority, std::memory_order_relaxed);
  const ShadersNative& library = *prewarmed_library_;
  LibraryDksh& dksh = *dksh_;  // TryPrewarm checked it is loaded; it is never replaced while the draws live
  // [dk] The Vulkan thread built shader modules (normal, 7e3, alpha only, early Z, FragCoord XY, depth half,
  // depth quantize, rectangle) and a render pass per format set here, then CreatePipelineVulkan. In deko3d every
  // one of those is a package variant selected by key.specialization, so one Preload per record replaces them.
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> keys;
  uint32_t done = 0, compiled = 0, no_shader = 0, other_mode = 0, failed = 0;
  uint64_t ns_compiled = 0;
  const size_t n = file_list_.size();
  for (size_t i = 0; i < n; ++i) {
    if (prewarmed_stop_.load(std::memory_order_relaxed)) {
      break;
    }
    const RegisterPipeline& r = file_list_[i];
    uint8_t state = kFailedList;
    // Package order changes whenever discovery adds a shader. Prefer the stable container fingerprint;
    // the numeric key is retained only for compatibility with records written before fingerprints existed.
    const ShaderEntry* vs = r.vs_fingerprint ? library.PerFingerprint(r.vs_fingerprint)
                                             : (r.key.vs ? library.PerNumber(r.key.vs - 1) : nullptr);
    const ShaderEntry* ps = r.ps_fingerprint ? library.PerFingerprint(r.ps_fingerprint)
                                             : (r.key.ps ? library.PerNumber(r.key.ps - 1) : nullptr);
    if (!vs || !vs->vertices || !vs->shader || vs->shader->fingerprint != r.vs_fingerprint ||
        (r.key.ps && (!ps || ps->vertices || !ps->shader || ps->shader->fingerprint != r.ps_fingerprint))) {
      state = kListNoShader;
      ++no_shader;
    } else if (r.key.fill2 != prewarmed_eds_ || other_settings_list_[i]) {
      state = kOtherListMode;  // another dynamic-state mode, or another setting (sa2_gpu_prewarm_current_keys)
      ++other_mode;
    } else {
      keys.clear();
      PreloadKeysOfRecordDk(dksh, r, *vs, ps, keys);
      // A record without a fragment stage but with kSpecDepthFloat24Quantize had a synthetic depth-only fragment
      // module in Vulkan; in deko3d that is one of the renderer's own shaders (embedded), nothing to read.
      const auto t0 = std::chrono::steady_clock::now();
      const uint32_t read = dksh.Preload(keys, &prewarmed_stop_);
      const uint64_t ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
      if (!prewarmed_stop_.load(std::memory_order_relaxed)) {
        state = kPrewarmedList;
        ++done;
        if (read) {
          ++compiled;
          ns_compiled += ns;
        }
      } else {
        ++failed;
      }
    }
    state_list_[i] = state;
    prewarmed_done_.store(done, std::memory_order_relaxed);
    prewarmed_compiled_.store(compiled, std::memory_order_relaxed);
    prewarmed_ns_compiled_.store(ns_compiled, std::memory_order_relaxed);
    prewarmed_no_shader_.store(no_shader, std::memory_order_relaxed);
    prewarmed_other_mode_.store(other_mode, std::memory_order_relaxed);
    prewarmed_failed_.store(failed, std::memory_order_relaxed);
    prewarmed_until_.store(i + 1, std::memory_order_release);
  }
  // [dk] The missing variants the ring recorded so far go to the SD card here (FlushMissing may block on it;
  // this thread is the one allowed to).
  dksh.FlushMissing();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarmed_start_).count();
  REXLOG_INFO("[native] C6 prewarm: [dk] {} in {:.1f} s, priority {:#x}: {} of {} pipelines "
              "prewarmed (their DKSH variants read ahead), {} read from the SD card now ({:.0f} ms, {:.1f} ms each; "
              "the rest was already read), {} skipped because the library no longer has their shaders, {} from "
              "another dynamic-state mode and {} failed",
              prewarmed_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished", seconds, priority,
              done, n, compiled, double(ns_compiled) / 1e6,
              compiled ? double(ns_compiled) / 1e6 / double(compiled) : 0.0, no_shader, other_mode, failed);
  prewarmed_finished_.store(true, std::memory_order_release);
}

// Ring only, every 10 s if anything changed: the thread's progress, what the ring had to create, and
// the guard (see above).
void DrawsDkImpl::PrewarmedReport(std::chrono::steady_clock::time_point now) {
  if (now - prewarmed_report_ < std::chrono::seconds(10)) {
    return;
  }
  prewarmed_report_ = now;
  if (!prewarmed_difference_ && prewarmed_ring_ >= 8 &&
      prewarmed_slow_ring_ * 2 >= prewarmed_ring_) {
    prewarmed_difference_ = true;
    prewarmed_stop_.store(true, std::memory_order_relaxed);
    sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
    REXLOG_ERROR("[native] C6 prewarm: MISMATCH: of {} pipelines the ring requested that were already "
                 "prewarmed, {} took as long as a compiled one (20 ms or more): what the thread prewarms is not what "
                 "the ring requests. The thread is stopped; nothing else changes",
                 prewarmed_ring_, prewarmed_slow_ring_);
  }
  const size_t until = prewarmed_until_.load(std::memory_order_acquire);
  const bool finished = prewarmed_finished_.load(std::memory_order_acquire);
  const uint64_t changes = uint64_t(until) + prewarmed_ring_ + ring_of_list_ + new_ring_ +
                           (finished ? 1 : 0) + (prewarmed_decided_ ? 1 : 0);
  if (changes == prewarmed_previous_changes_) {
    return;
  }
  prewarmed_previous_changes_ = changes;
  const auto media = [](uint64_t ns, uint64_t k) { return k ? double(ns) / double(k) / 1e6 : 0.0; };
  const uint32_t compiled = prewarmed_compiled_.load(std::memory_order_relaxed);
  SA2_REPORT_RING(
      "[native] C6 pipeline prewarm: [dk] list of {}; thread {} (priority {:#x}): {} visited, {} "
      "prewarmed, {} read from the SD card ({:.0f} ms), {} without their shaders, {} of another mode, {} failed | "
      "the ring created {} from the list already prewarmed ({:.1f} ms average, {} slow), {} from the list not "
      "prewarmed ({:.1f} ms average) and {} new ({:.1f} ms average; {} added to the list)",
      file_list_.size(),
      !prewarmed_thread_.joinable() ? (prewarmed_decided_ ? "not launched" : "waiting for the library")
      : finished                    ? (prewarmed_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished")
                                     : "running",
      uint32_t(prewarmed_priority_.load(std::memory_order_relaxed)), until,
      prewarmed_done_.load(std::memory_order_relaxed), compiled,
      double(prewarmed_ns_compiled_.load(std::memory_order_relaxed)) / 1e6,
      prewarmed_no_shader_.load(std::memory_order_relaxed), prewarmed_other_mode_.load(std::memory_order_relaxed),
      prewarmed_failed_.load(std::memory_order_relaxed), prewarmed_ring_,
      media(ns_prewarmed_ring_, prewarmed_ring_), prewarmed_slow_ring_, ring_of_list_,
      media(ns_ring_of_list_, ring_of_list_), new_ring_, media(ns_new_ring_, new_ring_),
      ring_to_list_);
}

void DrawsDkImpl::StopPrewarmed() {
  prewarmed_stop_.store(true, std::memory_order_relaxed);
  if (prewarmed_thread_.joinable()) {
    prewarmed_thread_.join();  // at most, as long as the variant being read takes
  }
}

// [dk] The image's memory and id go after everything recorded so far (sa2::dk::DestroyImage without an order):
// no view to destroy, and no pool chunk to return (sa2::dk::CreateImage's sub-allocator takes the memory back with
// the id; pool_block stays 0xFFFFFFFF in deko3d).
void DrawsDkImpl::DestroyImage(ImageNativeDk& image) {
  if (image.image != sa2::dk::kNoImage) sa2::dk::DestroyImage(image.image);
  image = ImageNativeDk{};
}

std::unique_ptr<DrawsDk> DrawsDk::Create(rex::memory::Memory* memory, ContextTargetsDk* context) {
  if (!memory || !context) {
    return nullptr;
  }
  auto draws = std::make_unique<DrawsDkImpl>(memory, context);
  draws->SetHostMsaaSamples(context->HostMsaaSamples());  // sa2_msaa (1 = none)
  if (!draws->Initialize()) {
    REXLOG_ERROR("[native] C6: could not prepare the native draws");
    return nullptr;
  }
  REXLOG_INFO("[native] C6: native draws prepared");
  return draws;
}

}  // namespace sa2::native
#endif  // SA2_DK_AVAILABLE
