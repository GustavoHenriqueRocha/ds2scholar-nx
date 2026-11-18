// SA2 - native renderer on deko3d: the file-scope part of sa2_native_targets.cpp (lines 1-930) for the translated
// render target class (sa2_native_targets_dk.h). Owned by the "alvos-infra" region (ds2-deko3d
// docs/pesquisas/traducao/01-alvos-infra.md).
//
// What is here:
//   - the includes every sa2_native_targets_dk_*.cpp needs (the same as the Vulkan file, minus the Vulkan device,
//     presenter and deferred recording);
//   - declarations of every cvar and enhancement sa2_native_targets.cpp defines. That file stays in the same binary,
//     so nothing is defined twice: REXCVAR_DECLARE and SA2_ENHANCEMENT_EXTERN (the enhancements are at global scope
//     there, so they are declared at global scope here too);
//   - the file-scope constants, helpers and structs of the Vulkan file (lines 701-928) with the GPU handles swapped
//     (VkImage -> sa2::dk::ImageId, VkBuffer + VkDeviceMemory -> sa2::dk::Block, command pools/buffers ->
//     sa2::dk::CmdStream, fences -> submission orders, descriptor pools -> one transient descriptor ring per slot).
// They live in namespace sa2::native (not an anonymous namespace as in the Vulkan file): the class that uses them is
// shared by four translation units, and an anonymous type in its members would differ per unit. The Vulkan file's
// own copies are in its anonymous namespace (internal linkage), so the names do not clash at link time.

#pragma once

#include "sa2_dk.h"
#include "sa2_native_dk.h"

#if SA2_DK_AVAILABLE

#include "cpu/sa2_enhancements.h"
#include "sa2_waits_hitch.h"
#include "sa2_native_shaders.h"  // ShaderEntry (SubmissionDraw::vs / ps); the SPIR-V arrays are not used here

#include "me_formats_color.h"  // HDR render targets
#include "me_resolve.h"
#include "me_edram_layout.h"
#include "me_msaa_raster_grid.h"
#include "me_edram_ownership.h"
#include "me_depth.h"
#include "me_resolved_allocation.h"
#include "me_resolved_sampling.h"
#include "me_msaa_depth_resolve.h"
#include "me_resolution_scale.h"
#include "../me_shader_identity.h"
#include "../sa2_render_mode.h"
#include "sa2_native_dk_shaders.h"  // dkshaders::Id (region 07, generated)

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

extern "C" uint32_t g_banjo_last_reject_cause;  // banjo-nx: sa2_native_draws.cpp, for the frame trace
// Counter 0 of the console profiler (game FPS).
extern "C" void RexSwitchPerfCount(unsigned id);
// Interval of a long frame for stack sampling (the "during the hitches" section of the profiler report).
extern "C" void RexSwitchPerfHitch(uint64_t start, uint64_t end);

namespace gr = rex::graphics;

namespace sa2::native {
bool CompareActiveFast();  // sa2_native_draws.cpp: sa2_native_compare_fast
}

// --- Enhancements of sa2_native_targets.cpp (defined there, global scope) ----------------------------------------
SA2_ENHANCEMENT_EXTERN(g_lazy_trace_text);   // sa2_cpu_lazy_trace_text
SA2_ENHANCEMENT_EXTERN(g_target_memo);       // sa2_cpu_ring_target_memo
SA2_ENHANCEMENT_EXTERN(g_redirect_key_int);  // sa2_cpu_ring_redirect_key
SA2_ENHANCEMENT_EXTERN(g_resolve_draw);      // sa2_gpu_resolve_draw
SA2_ENHANCEMENT_EXTERN(g_resolve_alias);     // sa2_gpu_resolve_alias
SA2_ENHANCEMENT_EXTERN(g_partial_inside);    // sa2_native_partial_resolve_inside
SA2_ENHANCEMENT_EXTERN(g_resolved_retire);   // sa2_native_resolved_retire_idle
SA2_ENHANCEMENT_EXTERN(g_edram_memo);        // sa2_cpu_ring_edram_memo

// --- Cvars of sa2_native_targets.cpp (all 67 it defines, in its order) -------------------------------------------
REXCVAR_DECLARE(bool, banjo_native_park_resolved);
REXCVAR_DECLARE(bool, sa2_native_edram4_pair_report);
REXCVAR_DECLARE(std::string, sa2_diag_stale_resolved);
REXCVAR_DECLARE(int32_t, sa2_native_resolved_idle_frames);
REXCVAR_DECLARE(bool, sa2_native_zcull_1x_depth);
REXCVAR_DECLARE(std::string, ds2_zcull_depth_views);
REXCVAR_DECLARE(std::string, ds2_clear_depth_claim);
REXCVAR_DECLARE(bool, sa2_native_edram4_clear_color0);
REXCVAR_DECLARE(bool, banjo_native_edram4_skip_unsupported);
REXCVAR_DECLARE(bool, sa2_native_reuse_alloc_resolved);
REXCVAR_DECLARE(bool, sa2_native_logical_resolved_size);
REXCVAR_DECLARE(int32_t, sa2_native_edram_alias_mode);
REXCVAR_DECLARE(int32_t, sa2_diag_resolves_from);
REXCVAR_DECLARE(int32_t, sa2_diag_alias_frames);
REXCVAR_DECLARE(bool, sa2_diag_present_alias);
REXCVAR_DECLARE(int32_t, sa2_native_shadows_scale);
REXCVAR_DECLARE(bool, sa2_native_resolver_no_copy);
REXCVAR_DECLARE(bool, sa2_native_edram4_cache_sync);
REXCVAR_DECLARE(bool, sa2_native_edram4_area_rect);
REXCVAR_DECLARE(bool, sa2_native_edram4_clear_consumer);
REXCVAR_DECLARE(bool, sa2_native_edram4_clear_zero);
REXCVAR_DECLARE(bool, sa2_native_edram4_partial_clear);
REXCVAR_DECLARE(bool, sa2_native_edram4_clear_color);
REXCVAR_DECLARE(bool, sa2_native_edram4_clear_stencil);
REXCVAR_DECLARE(bool, sa2_native_edram4_redirected_clear);
REXCVAR_DECLARE(bool, sa2_native_edram4_import_area_tiles);
REXCVAR_DECLARE(bool, sa2_native_edram4_clear_alias);
REXCVAR_DECLARE(bool, sa2_native_edram4_fast);
REXCVAR_DECLARE(bool, sa2_native_edram4_stencil_lazy);
REXCVAR_DECLARE(bool, sa2_native_edram4_stencil_light);
REXCVAR_DECLARE(bool, sa2_native_edram4_bits_stencil);
REXCVAR_DECLARE(bool, sa2_native_edram4_stencil_copy);
REXCVAR_DECLARE(bool, ds2_resolved_small_native);
REXCVAR_DECLARE(bool, ds2_clear_color0_alias);
REXCVAR_DECLARE(bool, ds2_clear_color0_7e3);
REXCVAR_DECLARE(bool, ds2_clear_color0_partial);
REXCVAR_DECLARE(bool, ds2_clear_depth_cross);
REXCVAR_DECLARE(bool, ds2_redirect_regions_reuse);
REXCVAR_DECLARE(bool, ds2_dynamic_resolution_keep);
REXCVAR_DECLARE(bool, ds2_7e3_r11g11b10);
REXCVAR_DECLARE(bool, ds2_resolve_7e3_direct);
REXCVAR_DECLARE(bool, ds2_skip_repeated_depth_resolve);
REXCVAR_DECLARE(bool, ds2_resolve_bias_rgba8);
REXCVAR_DECLARE(bool, ds2_resolve_clip);
REXCVAR_DECLARE(int32_t, sa2_diag_trace_from_s);
REXCVAR_DECLARE(int32_t, sa2_diag_dump_marks_s);
REXCVAR_DECLARE(bool, sa2_native_edram4_batch);
REXCVAR_DECLARE(bool, sa2_native_conversion_frag);
REXCVAR_DECLARE(int32_t, sa2_native_edram4_stencil_copy_min_tiles);
REXCVAR_DECLARE(bool, sa2_native_depth_samples_x);
REXCVAR_DECLARE(bool, sa2_native_swap_no_clear);
REXCVAR_DECLARE(bool, sa2_native_resolver_valid_content);
REXCVAR_DECLARE(bool, sa2_native_restore_area_util);
REXCVAR_DECLARE(bool, ds2_output_fxaa);
REXCVAR_DECLARE(bool, ds2_output_fxaa_quality);
REXCVAR_DECLARE(bool, sa2_native_lazy_front);
REXCVAR_DECLARE(bool, sa2_native_skip_cleared_repeated);
REXCVAR_DECLARE(bool, sa2_native_stats_pipeline);
REXCVAR_DECLARE(int32_t, sa2_native_resolved_read_texels);
REXCVAR_DECLARE(bool, sa2_native_invalidate_textures_each_copy);
REXCVAR_DECLARE(int32_t, sa2_native_slots_work);
REXCVAR_DECLARE(bool, sa2_native_zcull);
REXCVAR_DECLARE(bool, sa2_native_output_no_wait);
REXCVAR_DECLARE(bool, sa2_native_precise_marks);
REXCVAR_DECLARE(bool, sa2_gpu_marks_categories);
REXCVAR_DECLARE(int32_t, sa2_native_reads_each);
REXCVAR_DECLARE(bool, sa2_native_ramp_gamma);
// The ones sa2_native_targets.cpp itself declares (defined in sa2_native_draws.cpp).
REXCVAR_DECLARE(bool, sa2_native_edram4_stencil_inert);
REXCVAR_DECLARE(std::string, sa2_diag_trace_tile);
REXCVAR_DECLARE(std::string, ds2_diag_fetch_base);
REXCVAR_DECLARE(int32_t, sa2_native_stats_per_draw_s);
REXCVAR_DECLARE(bool, sa2_native_pass_narrow_dependency);
// New, deko3d only (defined in sa2_native_targets_dk_infra.cpp). Conservative defaults: what NVK did.
REXCVAR_DECLARE(bool, sa2_dk_compress_color);
REXCVAR_DECLARE(bool, sa2_dk_compress_depth);

namespace sa2::native {

namespace xenos = rex::graphics::xenos;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kMaxTargetHeight = 2048;
// sa2-nx: 2048 (was 256). SA2's split-screen gameplay records ~440 EDRAM conversions per frame, so 256 ran out about
// once a frame and each rollover is a fenced SendWork (a full CPU <-> GPU drain plus an extra submission). The sets
// are tiny (one or two image descriptors); the commands and their order are unchanged.
// deko3d: the same budget, now also bounded by the slot's transient descriptor ring (EnsureCapacityConversionEDRAM4).
constexpr uint32_t kConversionsEDRAMPerSlot = 2048;
// kRangeColor / kRangeDepth (VkImageSubresourceRange) are gone: deko3d views carry no aspect; a depth view samples
// depth and a stencil view is View(id, format, DkDsSource_Stencil). FnCopyImage / FnClearDepth / FnBlit are gone: the
// deko3d calls are dkCmdBufCopyImage / dkCmdBufClearDepthStencil / dkCmdBufBlitImage.

inline uint8_t ClassEdramFORMAT(uint32_t format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (F(format)) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA: return 0;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10: return 2;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16: return 3;
    case F::k_16_16_16_16_FLOAT: return 7;
    default: return uint8_t(format);
  }
}

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
inline int32_t Fixed16p8(float value) {
  if (!(std::abs(value) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double scaled = std::clamp(double(value) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(scaled));
}

// Address of a texel in a 32x32-tiled texture (same layout as the SDK's texture util).
inline int32_t TileDisplacement2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// With 4-byte texels, TileDisplacement2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (WriteReads).
inline const std::array<uint16_t, 1024>& TableTile2DTexel4() {
  static const std::array<uint16_t, 1024> table = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(TileDisplacement2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return table;
}

inline int32_t ExtenderSign15(uint32_t value) {
  return int32_t(value << 17) >> 17;
}

using Image = ImageNativeDk;  // prepared = already cleared once (there are no layouts in deko3d)

struct Resolved {
  Image image;
  uint32_t guest_format = 0;
  bool swap_rb = false;
  // Times the draws have requested this address since the last report. It lives here and not in the report
  // map because ResolvedTexture is on the hot path (one call per texture and draw, ~8,000 per frame) and the
  // lookup of the resolved texture is done anyway.
  uint64_t reads = 0;
  uint64_t revision = 0;
  // sa2_gpu_resolve_alias: its bytes are still in another texture (ContextTargets::aliases_).
  bool alias_pending = false;
  // sa2_native_resolved_retire_idle: the presented frame of the last resolve into it and of the last read of it.
  uint64_t last_write = 0;
  uint64_t last_read = 0;
};

struct ResolvedClip {
  uint32_t address = 0;
  Image image;
  sa2::dk::ImageId source = sa2::dk::kNoImage;  // was VkImage
  uint64_t revision = 0;
  uint64_t bytes = 0;                           // was VkDeviceSize
};

// Allocation cache only, never an alternative authoritative texture at this address.
struct ResolvedSleeping {
  uint32_t base = 0;
  Image image;
  uint64_t bytes = 0;  // was VkDeviceSize
  uint64_t order_retirement = 0;
};

// report of readbacks per render target (base and size) and cadence of sa2_native_reads_each.
struct TargetRead {
  uint32_t base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;    // small copies seen in the interval
  uint64_t skipped = 0;  // of those, the ones not read because of sa2_native_reads_each
};

// Copy inventory (NoteCopy): every resolved texture, with its size and how many times each one is read. That answers whether a
// copy is needed: a resolved texture that is copied every frame and never requested is wasted bandwidth.
struct CopyTarget {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;
  uint64_t with_draws = 0;  // copies with some draw since the previous copy
  uint64_t draws = 0;
  uint64_t pixels = 0;
};

// What is known about a render target between game commands. It serves two purposes:
//  - skipping a clear that changes nothing (sa2_native_skip_cleared_repeated);
//  - restoring only the rows the game really uses (sa2_native_restore_area_util).
// It lives in a separate map and not in ImageNative because that structure belongs to another file.
struct TargetState {
  bool clean_clear = false;    // the content is exactly the last clear, with nothing on top
  uint64_t clear_value = 0;     // packed color, or depth+stencil
  uint64_t draws_on_clear = 0; // global draw counter at that moment
  uint32_t used_height = 0;        // the largest y1 the game has resolved from this render target
};

// Readback of a small resolved texture (sa2_native_resolved_read_texels):
// CPU-visible block it is copied to (dkCmdBufCopyImageToBuffer) and what is needed to write it to the guest.
// deko3d: block = the VkBuffer + VkDeviceMemory pair, data = its CPU address; coherent = the block is CPU-uncached
// (a CpuCached one needs its CPU cache invalidated before the read, which the Vulkan code did with
// vkInvalidateMappedMemoryRanges).
struct Read {
  sa2::dk::Block block;
  uint8_t* data = nullptr;
  uint64_t bytes = 0;  // was VkDeviceSize
  bool coherent = true;
};
struct PendingRead {
  Read* read;
  uint32_t base;          // RB_COPY_DEST_BASE
  int32_t x0;             // texel of the texture where the rectangle starts
  int32_t y0;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;         // RB_COPY_DEST_PITCH
  uint32_t target_height;
  uint32_t info;          // RB_COPY_DEST_INFO
};

// Output slots with sa2_native_output_no_wait (without it, only slot 0 is used).
constexpr uint32_t kSlotsOutput = 3;
// GPU timestamps per slot (the last one is kept for the final timestamp).
// ME: mode-4 EDRAM operations mark their own GPU time. Eden Prime records a whole frame in one submission
// with several hundred category changes: at 1024 the marks ran out and the rest of the frame was billed to
// the last category (a saturated, meaningless "edram_import 207 ms").
constexpr uint32_t kMarksPerSlot = 8192;
constexpr uint8_t kGpuEnd = 0xFF;
// Passes measured per work unit (~16 per frame including the resumed ones).
constexpr uint32_t kStatsPerSlot = 64;
constexpr uint32_t kCountersStat = 3;  // vertices, clipped primitives and fragments
// Draws measured in a diagnostic frame (the scene has ~1200).
constexpr uint32_t kStatsDrawPerSlot = 8192;  // ME frames exceed 2048 draws
constexpr uint32_t kLabelsShader = 512;
// deko3d: a statistics query is kCountersStat counter reports at its start and kCountersStat at its end (DkCounter
// values are running totals); its result is end - start. Report index of counter k of query q: q * 6 + k (start),
// q * 6 + 3 + k (end).
constexpr uint32_t kReportsStat = 2 * kCountersStat;
// The Vulkan order: VERTEX_SHADER_INVOCATIONS, CLIPPING_PRIMITIVES (primitives out of the clipper) and
// FRAGMENT_SHADER_INVOCATIONS.
constexpr DkCounter kCountersStatDk[kCountersStat] = {DkCounter_VertexShaderInvocations,
                                                      DkCounter_ClipperOutputPrimitives,
                                                      DkCounter_FragmentShaderInvocations};
// deko3d: initial command memory of each work slot (sa2::dk::CmdStream adds blocks on overflow, logs it and keeps
// them), and the slot's arena for the push constants of the renderer's own passes (76-80 bytes each,
// DK_UNIFORM_BUF_ALIGNMENT = 256 apart: 4096 of them per slot).
constexpr uint32_t kCommandBytesWork = 1u << 20;
constexpr uint32_t kCommandBytesUpload = 256u << 10;
constexpr uint32_t kCommandBytesOutput = 64u << 10;
constexpr uint32_t kConstantsBytesSlot = 1u << 20;
// deko3d: the most transient descriptors one EDRAM operation takes from the slot ring (the depth conversion:
// sampled depth, storage target, sampled stencil). EnsureCapacityConversionEDRAM4 keeps room for one more.
constexpr uint32_t kDescriptorsConversionEDRAM = 3;

// One of the work slots: while a frame is being recorded, the previous ones can still be
// on the GPU with the other one.
// deko3d: named WorkSlot because SlotWork() is a ContextTargetsDk method (the name would be hidden in the class).
struct WorkSlot {
  // Command memory of the work and upload lists (were pool_work/work and pool_upload/upload). Rewound in
  // BeginRecording, after the slot's final submission completed.
  sa2::dk::CmdStream work;
  sa2::dk::CmdStream upload;
  // ds2_native_partial_send: partial submissions of this recording. The Vulkan code allocated extra command buffers
  // for them (extra_work, extra_upload); a deko3d command buffer just finishes a list and goes on in its memory.
  uint32_t partials = 0;
  // Descriptors for typeless EDRAM conversions are reset only after this
  // slot's fence, so every dispatch keeps the views recorded with it.
  // deko3d: one transient descriptor ring per slot instead of the six descriptor pools (pool_conversion_edram,
  // pool_conversion_depth_edram, pool_import_depth_edram, pool_conv_color_frag, pool_depth_resolve_ms,
  // pool_stencil_copy); descriptors_capacity = its size (sa2::dk::TransientRing).
  sa2::dk::TransientDescriptors descriptors;
  uint32_t descriptors_capacity = 0;
  // deko3d: uniform buffer memory for the push constants of the renderer's own passes (Arena::Push), rewound with the
  // ring.
  sa2::dk::Arena constants;
  // Copy-engine stencil import (Mass Effect mode 4): bytes computed into this buffer, then copied.
  sa2::dk::Block buffer_stencil_copy;
  uint32_t conversions_edram = 0;
  bool pending = false;
  uint64_t order = 0;                        // submission number
  // deko3d: the sa2::dk order of the slot's final submission (its fence: WaitOrder / CompletedOrder).
  uint64_t dk_order = 0;
  // deko3d: images destroyed while this slot was recording: released after its final submission (Destroy).
  std::vector<sa2::dk::ImageId> destroy_after_send;
  // Wall-clock time of the work, from vkQueueSubmit to the signaled fence. The GPU timestamps give
  // 30.5 ms per frame while the hardware's own counter says 99.7 % load on a 55 ms frame: either there is
  // work we do not mark or the timestamp scale is wrong. This bounds the truth from above (the CPU sees
  // the fence a little late) and the timestamps from below.
  // ds2-deko3d: it was the scale (NVK timestamps are raw ticks of 625/384 ns, the x1.627 of the reports).
  std::chrono::steady_clock::time_point sent{};
  std::vector<PendingRead> reads;  // readbacks submitted with the work
  std::vector<uint8_t> categories;  // GPU category of each timestamp written
  // sa2-nx: timestamp queries of this slot written since its last reset (all of them before the first recording):
  // BeginRecording resets only those instead of all kMarksPerSlot (a copy-engine fill of 8192 queries per
  // submission on NVK).
  // deko3d: counter reports need no reset; the count is kept for the logic and the reports.
  uint32_t marks_written = kMarksPerSlot;
  std::vector<uint32_t> labels;  // first draw (VS << 16 | PS) of each mark's interval, 0 = none
  std::vector<uint64_t> descriptions;  // the pass of each mark (DescribeMarkGpu)
  std::vector<uint32_t> draws_mark;  // draws recorded in each mark's interval
  bool precise_marks = false;      // intermediate marks with BOTTOM_OF_PIPE (DkCounter_Timestamp)
  // Statistics queries recorded, with the category of each one's pass.
  std::vector<std::pair<uint32_t, uint8_t>> stats;
  // Per-draw queries, with the pixel shader number of each one.
  std::vector<std::pair<uint32_t, uint16_t>> stats_draw;
  // the full pixel shader number + 1 and category (<< 24) of each per-draw query (the
  // uint16 label above folds ME's ~30000 pixel shaders into 512).
  std::vector<uint32_t> draw_ps;
  std::vector<uint32_t> draw_vs;
};

}  // namespace sa2::native

#endif  // SA2_DK_AVAILABLE
