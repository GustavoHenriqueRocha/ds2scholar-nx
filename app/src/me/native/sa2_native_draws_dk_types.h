// sa2_native_draws_dk_types.h: file-scope helpers of the translated region (see the class header).
//
// SA2 - native renderer on deko3d: what sa2_native_draws.cpp keeps at file scope before DrawsVulkanImpl (lines
// 1309-2804: constants, the guest copy and detile helpers, the vertex and texture format tables, PipelineKey,
// Texture, View...), translated for the two halves of DrawsDkImpl (sa2_native_draws_dk_1.cpp / _2.cpp), plus the
// deko3d pieces that replace Vulkan objects (descriptor ranges, vertex layout, FixedState / PipelineDk /
// StateTracker, the UBO 3 specialization table).
//
// The Vulkan file keeps these in an anonymous namespace; here they are in sa2::native::draws_dk (an inline
// namespace: the class and both .cpp files use the names as they are, and nothing clashes with the Vulkan file's
// copies, which another translation unit owns). Everything is inline: the header is included by two .cpp files.
//
// Contract: ds2-deko3d docs/pesquisas/traducao/08-contrato-sa2-dk.md; decisions: 05-draws-1.md.
#pragma once

#include "sa2_native_dk.h"

#if SA2_DK_AVAILABLE
#include "cpu/sa2_enhancements.h"
#include "me_formats_color.h"
#include "../me_platform_compat.h"  // banjo-nx: NEON on aarch64, SIMDe elsewhere
// What the class members of both regions need (the Vulkan file's includes, without the Vulkan-only ones:
// sa2_deferred_recording.h, sa2_native_textures_pool.h, sa2_shader_library.h, rex/ui/vulkan/util.h).
#include "me_ds2_shader_light.h"
#include "sa2_waits_hitch.h"
#include "sa2_native_vertices_dedupe.h"
#include "sa2_native_vertex_cache.h"
#include "me_primitives.h"
#include "me_depth.h"
#include "me_depth_quantize_spirv.h"  // QuantizeDepthHalfCpu (CPU only)
#include "me_rectangle_spirv.h"
#include "me_edram_ownership.h"
#include "me_raster_state.h"
#include "me_resolution_scale.h"
#include "../sa2_render_mode.h"
#include "../me_vertex_fetch_selection.h"
#include "sa2_native_shaders.h"
#include "sa2_native_hooks.h"
#include "sa2_shader_library_dk.h"  // LibraryDksh (region 07)

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// xxHash with memcpy reads, as in the Vulkan file (see its comment: strict aliasing moved a read ahead of the
// write of the data being hashed). XXH_INLINE_ALL makes every function static, so a second include elsewhere in
// the same translation unit with other settings would be a different copy: include this header first.
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would take effect too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "sa2_crc_fingerprint.h"

// --- The Vulkan file's cvars (it stays in the same binary: declared here, never redefined) ----------------------
REXCVAR_DECLARE(std::string, banjo_native_skip_vs);
REXCVAR_DECLARE(bool, banjo_native_alpha_to_mask_test);
REXCVAR_DECLARE(bool, banjo_native_skip_indexed);
REXCVAR_DECLARE(int32_t, sa2_native_float24_ps_mode);
REXCVAR_DECLARE(int32_t, sa2_native_diag_msaa2_phase);
REXCVAR_DECLARE(bool, sa2_native_constants_dedupe);
REXCVAR_DECLARE(bool, sa2_diag_shared_words);
REXCVAR_DECLARE(bool, sa2_native_pass_narrow_end);
REXCVAR_DECLARE(bool, sa2_native_gpu_labels);
REXCVAR_DECLARE(bool, sa2_native_sort_updates_texture);
REXCVAR_DECLARE(bool, sa2_native_pipeline_cache_save_guard);
REXCVAR_DECLARE(int32_t, sa2_native_pipeline_cache_save_fail_test);
REXCVAR_DECLARE(bool, sa2_native_pipeline_cache_drop_copy);
REXCVAR_DECLARE(bool, ds2_pipeline_cache_copy_off_ring);
REXCVAR_DECLARE(int32_t, ds2_pipeline_cache_write_chunk_kb);
REXCVAR_DECLARE(int32_t, ds2_pipeline_cache_write_pause_ms);
REXCVAR_DECLARE(bool, sa2_native_tile_fast);
REXCVAR_DECLARE(bool, sa2_native_framebuffers_forget_views);
REXCVAR_DECLARE(int32_t, sa2_native_fingerprints_kb_frame);
REXCVAR_DECLARE(int32_t, sa2_native_texture_interval_max);
REXCVAR_DECLARE(bool, sa2_native_adaptive_texture);
REXCVAR_DECLARE(int32_t, sa2_native_fingerprints_sampling);
REXCVAR_DECLARE(bool, sa2_shadows_no_vegetation);
REXCVAR_DECLARE(bool, sa2_native_constants_ubo);
REXCVAR_DECLARE(bool, sa2_native_set4_differences);
REXCVAR_DECLARE(int32_t, sa2_native_nvk_measure);
REXCVAR_DECLARE(bool, sa2_native_nvk_measure_failures);
REXCVAR_DECLARE(bool, sa2_native_nvk_emission);
REXCVAR_DECLARE(bool, sa2_native_nvk_cbufs);
REXCVAR_DECLARE(bool, sa2_native_nvk_dynamic);
REXCVAR_DECLARE(bool, sa2_native_nvk_preload);
REXCVAR_DECLARE(bool, sa2_native_cache_textures_between_frames);
REXCVAR_DECLARE(bool, sa2_native_mipmaps);
REXCVAR_DECLARE(int32_t, sa2_native_textures_mb_max);
REXCVAR_DECLARE(bool, sa2_native_textures_binding_thread);
REXCVAR_DECLARE(int32_t, sa2_native_textures_binding_thread_priority);
REXCVAR_DECLARE(bool, sa2_native_diag_reuse);
REXCVAR_DECLARE(bool, sa2_shaders_preload);
REXCVAR_DECLARE(bool, sa2_native_pipelines_prewarm);
REXCVAR_DECLARE(bool, sa2_native_pass_narrow_dependency);
REXCVAR_DECLARE(bool, sa2_native_vs_pruned_outputs);
REXCVAR_DECLARE(bool, sa2_nvk_cache_per_stage);
REXCVAR_DECLARE(std::string, sa2_diag_trace_tile);
REXCVAR_DECLARE(bool, sa2_native_normalized_remap);
REXCVAR_DECLARE(bool, sa2_cold_startup);
REXCVAR_DECLARE(bool, sa2_native_pass_shadows_no_load);
REXCVAR_DECLARE(bool, sa2_native_diag_vertices_repeated);
REXCVAR_DECLARE(bool, sa2_native_uploads_thread);
REXCVAR_DECLARE(int32_t, sa2_native_uploads_thread_priority);
REXCVAR_DECLARE(int32_t, sa2_native_uploads_thread_core);
REXCVAR_DECLARE(bool, ds2_indices16_restart);
REXCVAR_DECLARE(bool, sa2_native_indices_neon);
REXCVAR_DECLARE(bool, sa2_native_uploads_help);
REXCVAR_DECLARE(bool, ds2_resolved_not_compressed);
REXCVAR_DECLARE(bool, ds2_vpos_render_scale);
REXCVAR_DECLARE(bool, ds2_texture_fused_untile);
REXCVAR_DECLARE(bool, ds2_texture_lazy_fingerprint);
REXCVAR_DECLARE(bool, ds2_diag_vs_constants);
REXCVAR_DECLARE(bool, ds2_diag_7e3_no_alpha);
REXCVAR_DECLARE(bool, ds2_scaled_min_viewport);
REXCVAR_DECLARE(std::string, ds2_diag_fetch_base);
REXCVAR_DECLARE(bool, ds2_diag_ps_census);
REXCVAR_DECLARE(std::string, ds2_diag_skip_ps);
REXCVAR_DECLARE(std::string, ds2_diag_ps_constants);
REXCVAR_DECLARE(bool, ds2_texture_exp_adjust);
REXCVAR_DECLARE(bool, sa2_native_inv_tex_size);
REXCVAR_DECLARE(int32_t, ds2_shader_light_mask);
REXCVAR_DECLARE(bool, sa2_native_pcf_cheap);
REXCVAR_DECLARE(int32_t, sa2_anisotropic_native);
REXCVAR_DECLARE(int32_t, sa2_native_shadows_pending_bias);
REXCVAR_DECLARE(double, ds2_texture_lod_bias);
REXCVAR_DECLARE(int32_t, sa2_native_filter_aniso);
REXCVAR_DECLARE(bool, sa2_native_skip_prepass);
REXCVAR_DECLARE(bool, sa2_native_send_after_shadows);
REXCVAR_DECLARE(int32_t, ds2_native_partial_send);
REXCVAR_DECLARE(int32_t, sa2_native_stats_per_draw_s);
REXCVAR_DECLARE(bool, sa2_native_indices_cache);
REXCVAR_DECLARE(int32_t, ds2_indices_cache_fingerprint);
REXCVAR_DECLARE(bool, ds2_indices_cache_verify);
REXCVAR_DECLARE(int32_t, sa2_native_dedupe_max_fingerprint);
REXCVAR_DECLARE(bool, sa2_native_compare_fast);
REXCVAR_DECLARE(bool, sa2_native_fast_copy);
REXCVAR_DECLARE(int32_t, sa2_native_fast_copy_verify);
REXCVAR_DECLARE(int32_t, sa2_native_fingerprint_mode);
REXCVAR_DECLARE(bool, sa2_native_dedupe_sample);
REXCVAR_DECLARE(int32_t, sa2_diag_texture_slots);
REXCVAR_DECLARE(bool, sa2_native_dedupe_vertices);
REXCVAR_DECLARE(bool, sa2_native_vertices_base_zero);
REXCVAR_DECLARE(int32_t, ds2_vertex_cache_mb);
REXCVAR_DECLARE(int32_t, ds2_vertex_cache_hash);
REXCVAR_DECLARE(bool, ds2_vertex_cache_verify);
REXCVAR_DECLARE(bool, sa2_native_ps_alpha_only);
REXCVAR_DECLARE(bool, sa2_native_z_early);
REXCVAR_DECLARE(bool, sa2_native_postponed_sky);
REXCVAR_DECLARE(bool, sa2_native_skip_invisibles);
REXCVAR_DECLARE(bool, sa2_native_framing_cache);
REXCVAR_DECLARE(bool, sa2_native_util_height_memo);
REXCVAR_DECLARE(bool, sa2_native_key_fast_pass);
REXCVAR_DECLARE(bool, sa2_native_cvars_per_frame);
REXCVAR_DECLARE(bool, sa2_native_no_ps_no_color);
REXCVAR_DECLARE(bool, sa2_native_fix_tonemap_7e3);
REXCVAR_DECLARE(double, sa2_native_tonemap_7e3_scale);
REXCVAR_DECLARE(int32_t, sa2_native_upload_memory);
REXCVAR_DECLARE(bool, sa2_shared_native_cache);
REXCVAR_DECLARE(bool, sa2_native_pipelines_direct);
REXCVAR_DECLARE(bool, sa2_native_count_changes_pipeline);
REXCVAR_DECLARE(bool, sa2_native_canonical_key);
REXCVAR_DECLARE(bool, sa2_native_pipeline_between_passes);
REXCVAR_DECLARE(bool, sa2_native_dynamic_state);
REXCVAR_DECLARE(bool, sa2_native_dynamic_state3);
REXCVAR_DECLARE(bool, sa2_native_diag_memory_upload);

// The last reject cause (defined by the Vulkan file at global scope; the crash reporter reads it).
extern uint32_t g_banjo_last_reject_cause;

// --- The draws' enhancements, mirrored for deko3d ---------------------------------------------------------------
// The Vulkan file defines them at global scope (SA2_ENHANCEMENT before namespace sa2::native), so the deko3d copies
// must be global too: SA2_ENHANCEMENT_DK declares the cvar's storage function where it is expanded, and only the
// global declaration links. Different names (g_dk_*) keep both objects; the deko3d ones report as "<key> (dk)".
// Defined in sa2_native_draws_dk_1.cpp; sa2_gpu_push_once has no deko3d copy (no push constants: the constants
// are always uniform buffers, see 05-draws-1.md, "Fase C").
extern ::sa2::enh::Enhancement g_dk_bias_seen;
extern ::sa2::enh::Enhancement g_dk_entry_hash;
extern ::sa2::enh::Enhancement g_dk_linear_not_resolved;
extern ::sa2::enh::Enhancement g_dk_slots_evict;
extern ::sa2::enh::Enhancement g_dk_structured_cf;
extern ::sa2::enh::Enhancement g_dk_texcoords_unswapped;
extern ::sa2::enh::Enhancement g_dk_no_texture_signs;
extern ::sa2::enh::Enhancement g_dk_identity_remap;
extern ::sa2::enh::Enhancement g_dk_prewarm_current;
extern ::sa2::enh::Enhancement g_dk_index_base_held;
extern ::sa2::enh::Enhancement g_dk_blend_skip;
extern ::sa2::enh::Enhancement g_dk_base_zero_r0;
extern ::sa2::enh::Enhancement g_dk_bound_plan_memo;
extern ::sa2::enh::Enhancement g_dk_compare_uploaded;

namespace sa2::native {
inline namespace draws_dk {

/*
 * A pass's category from its render target, in one place.
 *
 * It used to be decided by width: "1600 or more" meant shadows. With the scene at 1920
 * (sa2_1080p_test) that no longer tells them apart and the scene was counted as shadows, so its
 * category vanished from the report. The shadow map is recognized for what it is: a depth-only target, with
 * no color at all.
 */
inline uint32_t CategoryOfTarget(uint32_t pitch, const uint64_t* keys) {
  const bool color = keys[0] || keys[1] || keys[2] || keys[3];
  if (!color && keys[4] && pitch >= 1600) {
    return kGpuShadows;
  }
  if (pitch >= 1280) {
    return keys[4] ? kGpuScene : kGpuSceneNoDepth;
  }
  if (pitch >= 640) {
    return kGpuMirror;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuSmaller;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kRegConstantsVs = 0x4000;
constexpr uint32_t kRegConstantsPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;
constexpr uint32_t kRegBooleans = 0x4900;
constexpr uint32_t kRegistersConstants = 0x400;  // 256 constants x 4

constexpr uint32_t kCapacityHeap[4] = {4096, 16, 64, 512};  // 2D, 3D, cube, samplers
// Work slots. The ones actually used are chosen by sa2_native_slots_work; this is the room reserved
// for them, and it has to match the array in sa2_native_targets.cpp.
constexpr size_t kSlotsOfWork = 3;
constexpr VkDeviceSize kUploadSize = VkDeviceSize(64) << 20;
// Separate buffer for the shared constants, per slot. ~1,400 draws x 512 bytes = 0.7 MB per frame; if it
// fills up, the work is submitted just as with the upload buffer.
constexpr VkDeviceSize kSharedSize = VkDeviceSize(4) << 20;  // per work slot (there are three)
// The pipeline cache and the prewarm list go in a single file in <NRO folder>/cache/ (nothing loose next to
// the NRO, and a single pipelines .bin). Header "NFPC", version, list bytes and cache bytes (two uint32
// and two uint64), then both parts: the list as is (its "NFPL" header and the records) and the
// vkGetPipelineCacheData data.
constexpr const char* kFolderCache = "cache";
constexpr const char* kFilePipelines = "sa2_native_pipelines.bin";
// The same file under its name before the 2026-10-08 rename (cache/): read once when the new one does not exist yet,
// and deleted when the new one is written, so the renamed build keeps its prewarm list and Vulkan cache.
constexpr const char* kFilePipelinesRenamed = "masseffect_native_pipelines.bin";
constexpr uint32_t kMagicFilePipelines = 0x4350464Eu;  // "NFPC" in little-endian
constexpr uint32_t kVersionFilePipelines = 1;
constexpr size_t kHeaderFilePipelines = 2 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
// The two files used by earlier versions, next to the NRO: if the new one does not exist yet they are read
// once (so the existing cache is not lost) and deleted when the new one is written.
constexpr const char* kFileCacheOld = "masseffect_native_pipelines.bin";
constexpr const char* kFileOldList = "masseffect_native_pipelines_list.bin";
// The pipeline prewarm list. It holds no game data: state keys, formats and fingerprints. A header of four
// uint32 ("NFPL", version, record size and record count) followed by the records (RegisterPipeline).
constexpr uint32_t kMagicPipelinesList = 0x4C50464Eu;  // "NFPL" in little-endian
constexpr uint32_t kVersionPipelinesList = 2;
constexpr size_t kHeaderList = 4 * sizeof(uint32_t);
constexpr size_t kMaxRegistersList = 4096;
// 296 bytes: MASSEFFECT's shader_common.h (g_NdcScale at +280 and g_NdcOffset at +288).
// Shared constants: texture and sampler indices (0-63), booleans, texcoords, half pixel, alpha threshold
// (68) and function (69), NDC (64-73) and g_InputRemap for the 16 locations (74-89).
// 90 words up to g_InputRemap (bytes 296..359) and 32 more for 1/size of the 16 texture slots (bytes
// 360..487), which avoid querying the texture size on every sample.
constexpr uint32_t kSharedWords = 138;  // banjo-nx: words 122..129 = the 8 SHADER_CONSTANT_BOOL words; 130..135 =
                                         // indexed vertex fetch buffers (device address 130 + 2K, words 134 + K);
                                         // 136 = the vertex index base (g_VertexIndexBase)
                                         // ds2scholar-nx: 137 = g_VposScale (ds2_vpos_render_scale)
constexpr uint32_t kWordInvSize = 90;
// Constants through a dynamic UBO (sa2_native_constants_ubo). The bit is SPEC_CONSTANT_CONSTANTS_UBO
// from shader_common.h, and the sizes are the blocks the shaders declare: 256 and 224 float4, and 23 shared
// float4.
constexpr uint32_t kSpecConstantsUbo = uint32_t(1) << 8;
// The shaders take 1/texture size from the shared constants (SPEC_CONSTANT_INV_TEX_SIZE in XenosRecomp)
// and do not query the texture size on every sample.
constexpr uint32_t kSpecInvTexSize = uint32_t(1) << 9;
// The pixel shader is compiled without its color writes (pass without a color target).
constexpr uint32_t kSpecAlphaOnly = uint32_t(1) << 14;
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets (nine samples
// per pixel; eleven in p_000101, the smoke, which is full-screen quads and 21 % of the scene). With this bit
// the eight outer offsets are set to zero, the nine samples become identical and the compiler merges them
// into one. Shadow edges lose some smoothness. Needs a shader library built with the PCF variant.
constexpr uint32_t kSpecPcfCheap = uint32_t(1) << 15;
// Bits 16-18 = the alpha test function (0-6). See SPEC_CONSTANT_ALPHA_FUNC_SHIFT in shader_common.h: it
// removes the 7-case switch from every pixel shader.
constexpr uint32_t kSpecFunctionAlphaDisplacement = 16;
// Internal bit of the pipeline key that no shader reads (shader_common.h goes up to bit 19). It marks that
// the pixel shader module is the copy with OpExecutionMode EarlyFragmentTests.
constexpr uint32_t kSpecZEarly = uint32_t(1) << 20;
// Renderer-only pipeline/module variant: clamp fragment outputs to the numeric range of Xenos 7e3 RGB
// render targets. The shader sources don't inspect this bit; it only keeps the pipeline key distinct.
constexpr uint32_t kSpecTarget7e3 = uint32_t(1) << 24;
constexpr uint32_t kSpecRectangle = uint32_t(1) << 29;
// The actual depth attachment representation, not merely RB_DEPTH_INFO.
// Kept in canonical / prewarm keys even when fixed state becomes dynamic.
constexpr uint32_t kSpecDepthFloat24Half = uint32_t(1) << 21;
// Renderer-only variant bits; shader_common does not inspect them.
constexpr uint32_t kSpecDepthFloat24Quantize = uint32_t(1) << 30;
constexpr uint32_t kSpecDepthFloat24Round = uint32_t(1) << 31;
// Host raster-grid expansion is independent of shadow-map resolution scaling.
// Keep its safety contract in pipeline/prewarm keys as well as dynamic framing.
constexpr uint32_t kSpecRasterGridX = uint32_t(1) << 22;
// sa2_gpu_blend_skip: SPEC_CONSTANT_BLEND_SKIP_ALPHA / _ZERO (shader_common.h): the shader discards a pixel whose
// blend would leave the target exactly as it was (source alpha, or the whole source, 0 or less).
constexpr uint32_t kSpecBlendSkipAlpha = uint32_t(1) << 10;
constexpr uint32_t kSpecBlendSkipZero = uint32_t(1) << 11;
// sa2_gpu_index_base_held: SPEC_CONSTANT_VERTEX_INDEX_UNUSED (shader_common.h).
constexpr uint32_t kSpecVertexIndexUnused = uint32_t(1) << 19;
// GPU round 2: SPEC_CONSTANT_TEXCOORDS_UNSWAPPED, _NO_TEXTURE_SIGNS and _IDENTITY_REMAP
// (shader_common.h).
constexpr uint32_t kSpecTexcoordsUnswapped = uint32_t(1) << 5;
constexpr uint32_t kSpecNoTextureSigns = uint32_t(1) << 6;
constexpr uint32_t kSpecIdentityRemap = uint32_t(1) << 13;
// sa2_gpu_structured_cf: SPEC_CONSTANT_STRUCTURED_CF (shader_common.h): the translator's nested if/else form of a
// shader's forward-only control flow, emitted next to the skipTo form when it proved them equivalent.
constexpr uint32_t kSpecStructuredCf = uint32_t(1) << 12;

// SpirvTestsSpecBit (Vulkan file, line 1438) is not here: the deko3d shaders are DKSH, and which spec bits a
// shader tests comes from the package (sa2_shader_library_dk.h, region 07), not from walking SPIR-V.
/*
 * sa2_gpu_blend_skip: the SPEC_CONSTANT_BLEND_SKIP_* bit for a draw into one color target, or 0 when a discarded pixel
 * could differ from blending it.
 *  - The target: k_8_8_8_8 into an RGBA8 UNORM image, without exponent bias. The blend clamps the source and the factors
 *    to [0, 1] there, so an output of 0 or less contributes exactly 0, and destination x 1 is exact. Not the gamma,
 *    10-bit or float targets (-0 + 0 = +0, 0 x inf = NaN).
 *  - Nothing else the pixel would do: no alpha to coverage, no depth write, no stencil write of any kind (a discarded
 *    pixel skips the stencil fail, depth-fail and pass ops). SA2's occlusion queries are answered on the CPU
 *    (EVENT_WRITE_ZPD), so no host query counts these pixels.
 *  - The blend keeps the destination: op ADD (the renderer records the guest's REVERSE_SUBTRACT as SUBTRACT, which would
 *    not keep it) and a destination factor of exactly 1 once the source is 0: ONE, ONE_MINUS_SRC_ALPHA, and
 *    ONE_MINUS_SRC_COLOR when the whole source is 0.
 *  - _ALPHA (source alpha 0 or less): the color source term is 0 through a SRC_ALPHA or ZERO factor; the alpha source
 *    term is 0 whatever its factor. _ZERO (all four outputs 0 or less): every source term is 0.
 * A NaN output is never discarded (the shader tests <= 0), so it blends as before.
 */
inline uint32_t BlendSkipBits(const uint32_t* r, uint32_t control_depth, bool depth_target, uint32_t mask) {
  const uint32_t info = r[gr::XE_GPU_REG_RB_COLOR_INFO];
  if (((info >> 16) & 0xF) != 0 || (int32_t(info << 6) >> 26) != 0 ||
      HostFormatTargetColor(0) != VK_FORMAT_R8G8B8A8_UNORM)
    return 0;
  if ((r[gr::XE_GPU_REG_RB_COLORCONTROL] >> 4) & 0x1) return 0;  // alpha to coverage
  if (depth_target) {
    if (((control_depth >> 1) & 0x1) && ((control_depth >> 2) & 0x1)) return 0;  // depth test with depth write
    if (control_depth & 0x1) {  // stencil on: every op KEEP, or a write mask of 0, on both faces
      const bool front_keeps = ((control_depth >> 11) & 0x1FF) == 0 ||
                               ((r[gr::XE_GPU_REG_RB_STENCILREFMASK] >> 16) & 0xFF) == 0;
      const bool back_keeps = !((control_depth >> 7) & 0x1)
          ? front_keeps
          : ((control_depth >> 23) & 0x1FF) == 0 || ((r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF] >> 16) & 0xFF) == 0;
      if (!front_keeps || !back_keeps) return 0;
    }
  }
  const uint32_t blend = r[gr::XE_GPU_REG_RB_BLENDCONTROL0];
  const uint32_t source_color = blend & 0x1F, op_color = (blend >> 5) & 0x7, destination_color = (blend >> 8) & 0x1F;
  const uint32_t op_alpha = (blend >> 21) & 0x7, destination_alpha = (blend >> 24) & 0x1F;
  const auto one = [](uint32_t factor, bool source_zero) {
    return factor == 1 || factor == 7 || (source_zero && factor == 5);  // ONE, ONE_MINUS_SRC_ALPHA, ONE_MINUS_SRC_COLOR
  };
  const bool color = (mask & 0x7) != 0, alpha = (mask & 0x8) != 0;
  if (!color && !alpha) return 0;
  if ((!color || ((source_color == 6 || source_color == 0) && one(destination_color, false) && op_color == 0)) &&
      (!alpha || (one(destination_alpha, true) && op_alpha == 0)))
    return kSpecBlendSkipAlpha;
  if ((!color || (one(destination_color, true) && op_color == 0)) &&
      (!alpha || (one(destination_alpha, true) && op_alpha == 0)))
    return kSpecBlendSkipZero;
  return 0;
}


// Bits 25-28 carry the exact color-target mask for kSpecTarget7e3. The normal path used to recover it
// from the live Xenos registers, but a prewarm record only has PipelineKey. Keeping the mask in the key
// lets the prewarm thread build the exact same transformed SPIR-V module as the ring.
constexpr uint32_t kSpecMask7e3Displacement = 25;
constexpr uint32_t kSpecMask7e3 = uint32_t(0xF) << kSpecMask7e3Displacement;
// The UE3 tone map pixel shader (Vulkan file line 1200, next to sa2_native_tonemap_7e3_scale).
constexpr uint64_t kMeFingerprintTonemap = 0x44EC14CFD16EF8F8ull;

/*
 * The render target height comes from the pitch, not from what the game uses.
 *
 * sa2_native_targets.cpp creates every render target with `height = max(720, pitch)`, because the real
 * height is not known when it is created. Measured result:
 *
 *   scene    pitch 1280 -> image 1280x1280, the game uses 1280x720
 *   cubemap  pitch  320 -> image  320x720,  the game uses  320x256
 *   bloom    pitch  320 -> image  320x720,  the game uses  320x180
 *   bloom    pitch  160 -> image  160x720,  the game uses  160x90
 *
 * And since the passes are opened with loadOp = LOAD and closed with storeOp = STORE, that whole area is
 * read and written in each of the ~27 pass openings per frame: 18.81 Mtexels opened when the game uses
 * ~8.5. That is ~130-165 MB per frame moved for nothing, over a 21.3 GB/s bus that the three cores also
 * share.
 *
 * The Xbox 360 paid none of this: its framebuffer lived in 10 MB of on-chip EDRAM at 256 GB/s, and the
 * resolve was a hardware operation. We move it back and forth.
 *
 * It is fixed through the renderArea, not the image size. Vulkan only loads and stores the renderArea;
 * everything outside is preserved. So the height does not have to be guessed when the image is created:
 * opening the pass over the rectangle the game really uses is enough.
 *
 * How that height is known without guessing: from the draws' scissor. The maximum seen per pitch is kept,
 * only grows, and is rounded up to a multiple of 64. Until there is data the whole pass is opened, so the
 * first frame never clips too much.
 */
// The cvar is defined inside the Vulkan file's anonymous namespace, so it cannot be REXCVAR_DECLAREd from here:
// it is read through the registry (once per UseSlot, as the Vulkan code reads it). In deko3d there is no render
// area (load and store follow what the draws touch), so it only keeps UtilHeightFor's measurement as it was.
inline bool PassAreaUtilCvar() { return REXCVAR_QUERY(bool, sa2_native_pass_area_util); }
/*
 * The pixel shader of the sky dome (sa2_native_postponed_sky).
 *
 * The fingerprint is the XXH3 of the shader's original container, not of the translated SPIR-V: it
 * identifies the game's shader and does not change when the library is regenerated (checked: the same
 * 28AA3CDAC6C19705 in sa2_validated_predicates and in sa2_fusion3, with SPIR-V of 1,859 and 3,306
 * words). It is the same mechanism as the other fingerprint checks.
 *
 * What it is, beyond doubt: its constant table declares CloudIntensity, SkyAlphaTag and Brightness, and
 * four samplers (DIFFUSEMAP, MISCMAP1, MISCMAP2, MISCMAP3) with 5 samples. It is the only shader in the
 * library (152 containers) that names the sky: it cannot be confused with any other. In the log it shows
 * up as PS n29.
 */
constexpr uint64_t kSkyFingerprint = 0x28AA3CDAC6C19705ull;
// The sky dome is 480 indices = 160 triangles, measured in the log (the C6 diag line of the draw with
// PS n29). That is the mark that tells the dome draw apart from the other six that share its pixel
// shader, which were the ones that broke the image in an earlier version.
constexpr uint32_t kIndicesDomeSky = 480;
/*
 * How long the sky guard's test lasts, counting only the frames in which the dome appears (menus and
 * loading screens do not count, see CloseFrameOfTheGuardOfTheSky). 90 frames are about 3 seconds
 * of play: enough not to decide on four samples, and short enough for the saving to start almost as
 * the scene begins. All 90 must have exactly one dome: as soon as two appear in the same frame (the
 * earlier failure) it switches off for good.
 */
constexpr uint32_t kSkyFramesTest = 90;
constexpr uint32_t kSkyFramesWithOne = 90;
constexpr VkDeviceSize kUboBytesVs = 256 * 16;
constexpr VkDeviceSize kUboBytesPs = 256 * 16;  // banjo-nx: pixel shaders read up to c255
constexpr VkDeviceSize kUboBytesShared = 35 * 16;  // banjo-nx: + the 8 bool words and the indexed fetch buffers (shader_common.h)
constexpr uint32_t kRemapIdentity = 0xFFF;
constexpr uint32_t kMaxVerticesPerDraw = uint32_t(1) << 20;
constexpr uint32_t kPhysicalMemory = 0x20000000;
// banjo-nx: the most of an indexed vertex fetch buffer (bone palette) copied per draw; the shader reads 0 beyond.
constexpr uint64_t kIndexedFetchMaxBytes = uint64_t(4) << 20;
// Times a statement in the timed draws (C6 substages).
#define SA2_SUB(k, ...) \
  do { \
    if (time_) { \
      const auto t0_sub_ = std::chrono::steady_clock::now(); \
      __VA_ARGS__; \
      sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                 std::chrono::steady_clock::now() - t0_sub_) \
                                 .count()); \
      ++sub_n_[k]; \
    } else { \
      __VA_ARGS__; \
    } \
  } while (0)

/*
 * From 8 to 64. Each timed draw reads the clock about 20 times (Stage, SA2_SUB, samplers and
 * vertices). At 1 in 8 that averaged 0.15-0.4 us on every draw of the ring, and a busy scene has
 * 2,500-4,000 per frame. "C6 stages", "C6 substages" and "ms copying vertices" divide (or rescale) by
 * the draws that actually carried a stopwatch, so their scale does not change: they just have 8 times
 * fewer samples (~600-1,400 per second in play).
 */
constexpr uint32_t kStopwatchEach = 64;  // draws per stage-timed draw (power of 2)
constexpr size_t kCopiesPerWarning = 64;   // copies queued between wake-ups of the copy thread (power of 2)
// Copies taken in one batch (sa2_native_uploads_help). In play they are ~2 KB each: a batch is
// ~30 KB, about 20 us. That is how long the ring may have to wait for the thread, already with its
// priority lent.
constexpr size_t kCopiesPerChunk = 16;
// Waits with pending copies in the observing phase (the ring waits as before and checks the marks)
// before it starts helping. In the menus that is a few seconds.
constexpr uint64_t kWaitsCopiesLooking = 512;

// One guest vertex binding into the upload buffer, as host words with the fetch constant's byte order.
// Done by the ring thread or by the copy thread (sa2_native_uploads_thread).
struct WorkCopy {
  const uint8_t* source = nullptr;
  uint8_t* target = nullptr;
  uint32_t words = 0;
  xenos::Endian order = xenos::Endian::kNone;
};

inline void CopyVerticesBase(const WorkCopy& t) {
  // The fields go into local variables: the destination is a byte pointer and, as far as the compiler
  // knows, writing through it could change the structure itself. With t.* inside the loop it was not
  // vectorized (935 ms per 5.3 GB of vertices in play, against 490 ms with the loop inside Draw).
  const uint8_t* const source = t.source;
  uint8_t* const target = t.target;
  const uint32_t words = t.words;
  const xenos::Endian order = t.order;
  if (order == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, order);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  }
}

// sa2_native_fast_copy: the same swap and copy, 64 bytes per iteration (four 16-byte loads, then four stores
// that the compiler pairs into stp q) with the source prefetched 640 bytes ahead. The guest's vertex data was written
// frames ago by another core, so its lines are cold; the plain loop waits for each one. The first copies are compared
// with the plain loop (MISMATCH, plain loop from then on and for that copy).
template <int kOrder>
inline uint8x16_t PermuteBytes(uint8x16_t v) {
  if constexpr (kOrder == 1) return vrev16q_u8(v);                                                       // 8in16
  else if constexpr (kOrder == 2) return vrev32q_u8(v);                                                  // 8in32
  else if constexpr (kOrder == 3) return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));     // 16in32
  else return v;
}
template <int kOrder>
inline void CopyBlocks64(const uint8_t* source, uint8_t* target, size_t blocks) {
  for (size_t b = 0; b < blocks; ++b) {
    __builtin_prefetch(source + 640);
    const uint8x16_t a = vld1q_u8(source), c = vld1q_u8(source + 16), d = vld1q_u8(source + 32), e = vld1q_u8(source + 48);
    vst1q_u8(target, PermuteBytes<kOrder>(a));
    vst1q_u8(target + 16, PermuteBytes<kOrder>(c));
    vst1q_u8(target + 32, PermuteBytes<kOrder>(d));
    vst1q_u8(target + 48, PermuteBytes<kOrder>(e));
    source += 64;
    target += 64;
  }
}

inline std::atomic<int32_t> g_fast_copy_verify{-1};  // -1 = not initialised, then the uses still to check
inline std::atomic<bool> g_fast_off_copy{false};

inline void CopyVertices(const WorkCopy& t) {
  static const bool fast = REXCVAR_GET(sa2_native_fast_copy);
  if (fast && t.words >= 32 && !g_fast_off_copy.load(std::memory_order_relaxed)) {
    const size_t blocks = size_t(t.words) / 16;
    switch (t.order) {
      case xenos::Endian::k8in16: CopyBlocks64<1>(t.source, t.target, blocks); break;
      case xenos::Endian::k8in32: CopyBlocks64<2>(t.source, t.target, blocks); break;
      case xenos::Endian::k16in32: CopyBlocks64<3>(t.source, t.target, blocks); break;
      default: CopyBlocks64<0>(t.source, t.target, blocks); break;
    }
    const uint32_t done = uint32_t(blocks * 16);
    if (done < t.words) {
      CopyVerticesBase({t.source + size_t(done) * 4, t.target + size_t(done) * 4, t.words - done, t.order});
    }
    int32_t remain = g_fast_copy_verify.load(std::memory_order_relaxed);
    if (remain < 0) {
      int32_t initial = REXCVAR_GET(sa2_native_fast_copy_verify);
      g_fast_copy_verify.compare_exchange_strong(remain, initial);
      remain = g_fast_copy_verify.load(std::memory_order_relaxed);
    }
    if (remain > 0 && g_fast_copy_verify.fetch_sub(1, std::memory_order_relaxed) > 0) {
      std::vector<uint8_t> expected(size_t(t.words) * 4);
      CopyVerticesBase({t.source, expected.data(), t.words, t.order});
      if (std::memcmp(expected.data(), t.target, expected.size()) != 0) {
        g_fast_off_copy.store(true, std::memory_order_relaxed);
        sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
        REXLOG_ERROR("[native] MISMATCH: the fast vertex copy ({} words, order {}) does not give the same bytes as "
                     "the plain loop; using the plain loop from now on", t.words, uint32_t(t.order));
        std::memcpy(t.target, expected.data(), expected.size());
      }
    }
    return;
  }
  CopyVerticesBase(t);
}

// Allocator that leaves whatever resize adds uninitialized: indices_, converted_ and indices16_ are
// fully rewritten right after, and std::vector's zero fill was 1.4 % of the ring thread on PC.
template <class T>
struct NoInitialize : std::allocator<T> {
  using value_type = T;
  NoInitialize() = default;
  template <class U>
  NoInitialize(const NoInitialize<U>&) noexcept {}
  template <class U>
  struct rebind {
    using other = NoInitialize<U>;
  };
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

// FnBufferAddress (vkGetBufferDeviceAddress) is gone: a DkMemBlock has its GPU address (dkMemBlockGetGpuAddr).

// Inline equality of two small blocks (the per-draw state caches compare 8-130 bytes several times per draw; a libc memcmp
// call each cost ~0.6 ms/frame on the Switch ring thread). Same answer as std::memcmp(a, b, n) == 0.
inline bool EqualInLine(const void* a, const void* b, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(a);
  const uint8_t* q = static_cast<const uint8_t*>(b);
  uint64_t rest = 0;
  uint8x16_t accumulated = vdupq_n_u8(0);
  while (n >= 16) {
    accumulated = vorrq_u8(accumulated, veorq_u8(vld1q_u8(p), vld1q_u8(q)));
    p += 16;
    q += 16;
    n -= 16;
  }
  rest = vmaxvq_u8(accumulated);
  while (n >= 8) {
    uint64_t x, y;
    std::memcpy(&x, p, 8);
    std::memcpy(&y, q, 8);
    rest |= x ^ y;
    p += 8;
    q += 8;
    n -= 8;
  }
  if (n >= 4) {
    uint32_t x, y;
    std::memcpy(&x, p, 4);
    std::memcpy(&y, q, 4);
    rest |= x ^ y;
    p += 4;
    q += 4;
    n -= 4;
  }
  while (n) {
    rest |= uint8_t(*p ^ *q);
    ++p;
    ++q;
    --n;
  }
  return rest == 0;
}

// sa2_native_compare_fast selects the inline version; off = the libc memcmp as before.
inline bool Equal(const void* a, const void* b, size_t n) {
  static const bool in_line = REXCVAR_GET(sa2_native_compare_fast);
  return in_line ? EqualInLine(a, b, n) : std::memcmp(a, b, n) == 0;
}

inline float Float(uint32_t value) {
  return std::bit_cast<float>(value);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
inline uint32_t Log2Ceiling(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

inline uint32_t Log2Floor(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

/*
 * The sample used to recheck a stable texture (sa2_native_fingerprints_sampling). From a region of guest
 * memory, its first 4 KB block, its last one and one in every kSampleEach are read, counted from the
 * start of the region. The base and the mips of a texture start at 4 KB-aligned addresses, so each block
 * is a whole page. That reads ~13-19 % of the bytes of textures of 64 KB or more; for textures of few
 * blocks the sample is almost everything, and PrepareTexture does not use it if it exceeds half. Tested
 * on PC: BytesSample matches what is read for every size from 1 byte to 300 KB, and a one-byte change is
 * seen if and only if it falls in a sampled block.
 */
constexpr uint64_t kBlockSample = 4096;
constexpr uint64_t kSampleEach = 8;

// Bytes SampleFingerprint reads in a region of that size.
inline uint64_t BytesSample(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  return (last + kSampleEach - 1) / kSampleEach * kBlockSample + (bytes - last * kBlockSample);
}

// sa2_native_fingerprint_mode: how a block's bytes are folded into the running fingerprint.
inline int ModeFingerprint() {
  static const int mode = [] {
    int m = REXCVAR_GET(sa2_native_fingerprint_mode);
    if (m == 2 && !crc_fingerprint::kAvailable) m = 1;
    return m;
  }();
  return mode;
}

// Mode 1: chains an unseeded block hash into the previous fingerprint. Bijective in the block hash for a fixed previous
// fingerprint and in the previous fingerprint for a fixed block hash, so two inputs differing in one block always get
// different fingerprints unless the 64-bit block hashes collide; several differing blocks collide with probability 2^-64.
inline uint64_t ChainFingerprint(uint64_t previous, uint64_t block) {
  uint64_t m = previous * 0x9E3779B97F4A7C15ull;
  m ^= m >> 32;
  m *= 0xC2B2AE3D27D4EB4Full;
  m ^= m >> 29;
  return m + block;
}

inline uint64_t BlockFingerprint(const uint8_t* data, size_t bytes, uint64_t previous) {
  switch (ModeFingerprint()) {
#if SA2_CRC_FINGERPRINT
    case 2:
      return crc_fingerprint::Fingerprint64(data, bytes, previous);
#endif
    case 1:
      return ChainFingerprint(previous, XXH3_64bits(data, bytes));
    default:
      return XXH3_64bits_withSeed(data, bytes, previous);
  }
}

// Unseeded fingerprint of a whole range (the 16-bit index cache).
inline uint64_t RangeFingerprint(const uint8_t* data, size_t bytes) {
#if SA2_CRC_FINGERPRINT
  if (ModeFingerprint() == 2) return crc_fingerprint::Fingerprint64(data, bytes, 0);
#endif
  return XXH3_64bits(data, bytes);
}

// ds2scholar-nx (2026-10-23): the full fingerprint of a texture's guest bytes (base, then mips seeded with it). Mode 2
// (sa2_native_fingerprint_mode, the hardware CRC32 pair) takes it too; 0 and 1 keep XXH3 exactly as before. Console stack
// profile (pgo2 stacks): these full rechecks were ~9 % of the ring thread's running samples, with sampling off.
inline uint64_t RawTextureFingerprint(const uint8_t* data, size_t bytes, uint64_t seed, bool seeded) {
#if SA2_CRC_FINGERPRINT
  if (ModeFingerprint() == 2) return crc_fingerprint::Fingerprint64(data, bytes, seed);
#endif
  return seeded ? XXH3_64bits_withSeed(data, bytes, seed) : XXH3_64bits(data, bytes);
}

// Chained fingerprint of blocks 0, 8, 16... below the last one, and of the last one (which may be partial).
inline uint64_t SampleFingerprint(const uint8_t* data, uint64_t bytes, uint64_t seed) {
  if (!bytes) {
    return seed;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  uint64_t fingerprint = seed;
  for (uint64_t b = 0; b < last; b += kSampleEach) {
    fingerprint = BlockFingerprint(data + b * kBlockSample, size_t(kBlockSample), fingerprint);
  }
  return BlockFingerprint(data + last * kBlockSample, size_t(bytes - last * kBlockSample), fingerprint);
}

// First level of the packed tail: once the short side is 16 texels or less.
inline uint32_t PackedLevel(uint32_t width, uint32_t height) {
  const uint32_t l = Log2Ceiling(std::min(width, height));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
inline void PackedDisplacement(uint32_t width, uint32_t height, uint32_t block, uint32_t level, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_width = Log2Ceiling(width);
  const uint32_t l2_height = Log2Ceiling(height);
  const uint32_t l2 = std::min(l2_width, l2_height);
  x = 0;
  y = 0;
  if (l2 > 4 + level) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = level - base;
  if (m < 3) {
    if (l2_width > l2_height) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_width > l2_height) {
    x = (1u << (l2_width - base)) >> (m - 2);
  } else {
    y = (1u << (l2_height - base)) >> (m - 2);
  }
  x /= block;
  y /= block;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
inline int32_t TileDisplacement2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): ReadLevel called TileDisplacement2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as TileDisplacement2D (checked block by block on PC).
template <uint32_t kLog2>
inline void UntileLevel(const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* target) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_row = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * macros_row;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* output = target + size_t(row) * bx_host * kBytes;
    uint32_t column = 0;
    while (column < bx) {
      const int32_t x = int32_t(ox + column);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t displacement = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGroup > 1 && (uint32_t(x) % kGroup) == 0 && column + kGroup <= bx) {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kGroup * kBytes);
        column += kGroup;
      } else {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kBytes);
        ++column;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void ChangeOrderBytes(uint8_t* data, size_t bytes, uint32_t unit, uint32_t order) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unit == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unit == 2 && order != k8in16) || (unit != 2 && unit != 4) || order == 0) {
    return;
  }
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(data + i);
    uint8x16_t r;
    if (unit == 2 || order == k8in16) {
      r = vrev16q_u8(v);
    } else if (order == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(data + i, r);
  }
  if (unit == 2 || order == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(data[i], data[i + 1]);
    }
  } else if (order == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (order == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}

// ds2_texture_fused_untile: the byte swap ChangeOrderBytes does for (unit, order), as one of three vector
// reversals (0 = nothing to do).
inline uint32_t FusedOrderMode(uint32_t unit, uint32_t order) {
  if (order == 0) return 0;
  if (unit == 2) return order == 1 ? 1 : 0;
  if (unit == 4) return order <= 3 ? order : 0;
  return 0;
}

template <uint32_t kMode>
inline void CopyGroupOrder16(uint8_t* destination, const uint8_t* source) {
  uint8x16_t v = vld1q_u8(source);
  if constexpr (kMode == 1) v = vrev16q_u8(v);
  if constexpr (kMode == 2) v = vrev32q_u8(v);
  if constexpr (kMode == 3) v = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
  vst1q_u8(destination, v);
}

template <uint32_t kMode>
inline void CopyGroupOrder8(uint8_t* destination, const uint8_t* source) {
  uint8x8_t v = vld1_u8(source);
  if constexpr (kMode == 1) v = vrev16_u8(v);
  if constexpr (kMode == 2) v = vrev32_u8(v);
  if constexpr (kMode == 3) v = vreinterpret_u8_u16(vrev32_u16(vreinterpret_u16_u8(v)));
  vst1_u8(destination, v);
}

// ds2scholar-nx (ds2_texture_fused_untile). UntileLevel still worked out the address of every 16-byte group
// (~20 integer operations each) and the byte order was a second pass over the whole texture: console zone
// streaming, ~1.8 ms per MB of new textures, 40-70 ms in one frame. Here:
//   - TileDisplacement2D splits into a part from the macro tile column and a part from the column inside it:
//     the offset from the macro tile is a multiple of 2^(log2 + 7) and the rest is below that, so their bits
//     do not overlap and the bit shuffle of the offset applies to each part separately; the bank term
//     ((y8 + (x >> 3)) & 3) only sees x & 31. So each row has a table of the 32 / kGroup group addresses inside
//     a macro tile, and each macro tile adds one base;
//   - each group is copied as one 16-byte (8 for 1-byte blocks) vector, its bytes reversed in the register.
// Only for ox a multiple of 32 (the packed tail keeps UntileLevel). Same bytes as UntileLevel + ChangeOrderBytes;
// the caller compares whole textures against that path (the first 200, then 1 in 64).
template <uint32_t kLog2, uint32_t kMode>
inline void UntileLevelOrder(const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                      uint32_t bx_host, uint8_t* target) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  constexpr uint32_t kGroupBytes = kGroup * kBytes;                       // 16, or 8 for 1-byte blocks
  constexpr uint32_t kGroups = 32u / kGroup;                              // groups in a macro tile row
  const auto shuffle = [](int32_t offset) {
    return ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F);
  };
  const int32_t macros_row = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  const uint32_t macro_first = ox >> 5;
  int32_t inner[kGroups];
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * macros_row;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    for (uint32_t g = 0; g < kGroups; ++g) {
      const int32_t x = int32_t(g * kGroup);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      inner[g] = shuffle(offset) + y16 + (((y8 + (x >> 3)) & 3) << 6);
    }
    uint8_t* output = target + size_t(row) * bx_host * kBytes;
    for (uint32_t column = 0; column < bx; column += 32) {
      const uint8_t* base =
          source + shuffle((int32_t(macro_first + (column >> 5)) + macro_y) << (kLog2 + 7));
      uint8_t* out = output + size_t(column) * kBytes;
      const uint32_t n = std::min(32u, bx - column);
      const uint32_t whole = n / kGroup;
      for (uint32_t g = 0; g < whole; ++g) {
        if constexpr (kGroupBytes == 16) {
          CopyGroupOrder16<kMode>(out + g * kGroupBytes, base + inner[g]);
        } else {
          CopyGroupOrder8<kMode>(out + g * kGroupBytes, base + inner[g]);
        }
      }
      if (whole * kGroup < n) {  // the last blocks of a row narrower than a group: copied, then their order
        const uint32_t rest = n - whole * kGroup;
        uint8_t* tail = out + whole * kGroupBytes;
        std::memcpy(tail, base + inner[whole], size_t(rest) * kBytes);
        if constexpr (kMode == 1) ChangeOrderBytes(tail, size_t(rest) * kBytes, 2, 1);  // 8in16 of any unit
        if constexpr (kMode == 2) ChangeOrderBytes(tail, size_t(rest) * kBytes, 4, 2);
        if constexpr (kMode == 3) ChangeOrderBytes(tail, size_t(rest) * kBytes, 4, 3);
      }
    }
  }
}

template <uint32_t kLog2>
inline void UntileLevelOrderMode(uint32_t mode, const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx,
                          uint32_t by, uint32_t bx_host, uint8_t* target) {
  switch (mode) {
    case 1: UntileLevelOrder<kLog2, 1>(source, pitch, ox, oy, bx, by, bx_host, target); break;
    case 2: UntileLevelOrder<kLog2, 2>(source, pitch, ox, oy, bx, by, bx_host, target); break;
    case 3: UntileLevelOrder<kLog2, 3>(source, pitch, ox, oy, bx, by, bx_host, target); break;
    default: UntileLevelOrder<kLog2, 0>(source, pitch, ox, oy, bx, by, bx_host, target); break;
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// pitch and height in blocks.
inline int32_t TileDisplacement3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height = (height + 31) & ~uint32_t(31);
  const int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = outer + ((((x >> 3) + (outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// USAGE_LOCATIONS from XenosRecomp (shader_recompiler.cpp).
inline int32_t LocationOfUsage(uint8_t usage, uint8_t index) {
  switch (usage) {
    case 0:  // position
      return index == 0 ? 0 : (index == 1 ? 15 : -1);
    case 3:  // normal
      return index == 0 ? 1 : -1;
    case 6:  // tangent; ds2-nx: TANGENT1 on binormal's 3 (DS2 never declares BINORMAL0), as in XenosRecomp USAGE_LOCATIONS
      return index == 0 ? 2 : (index == 1 ? 3 : -1);
    case 7:  // binormal
      return index == 0 ? 3 : -1;
    case 5:  // texcoord; banjo-nx: 8..15 for register-linked shaders (banjoSlotLocation in XenosRecomp)
      return index < 4 ? 4 + index : index < 8 ? 12 + (index - 4) : index < 12 ? index - 8 : index < 16 ? 8 + (index - 12) : -1;
    case 10:  // color
      // ds2-nx: COLOR2 on TEXCOORD6's 14 (DS2's COLOR2 shaders declare TEXCOORD0..2 only), as in USAGE_LOCATIONS
      return index == 0 ? 8 : (index == 1 ? 11 : (index == 2 ? 14 : -1));
    case 2:  // blend indices
      return index == 0 ? 9 : -1;
    case 1:  // blend weights
      return index == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
inline bool WholeEntry(uint8_t usage) {
  // Only BLENDINDICES. Since the sa2_validated_normal library, normals, tangents and binormals are
  // float4: MASSEFFECT stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen on reflective surfaces).
  return usage == 2;
}

// g_InputRemap code: the SPIR-V writes r[i] = input[orig[i]] and the fetch patched by D3D writes
// r[i] = data[patched[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from patched[i]. 7 = the same component.
inline uint32_t RemapCode(uint32_t original, uint32_t patched) {
  uint32_t code = kRemapIdentity;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

// Components a host vertex format delivers; Vulkan fills the missing ones with (0, 0, 1).
inline uint32_t ComponentsVertexFormat(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_UINT:
      return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_SSCALED: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_USCALED: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_UINT:
      return 2;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT:
      return 3;
    default:
      return 4;  // also anything unknown: no normalization of missing components
  }
}

// The same remap code with every "no change" slot written as 7, so the shader's 0xFFF fast path is taken
// (remapInput's per-vertex select chain was ~70 % of the ALU of typical vertex shaders): a slot reading its
// own component, or the constant a missing component already has (0 for y/z, 1 for w).
inline uint32_t NormalizeRemap(uint32_t code, uint32_t components) {
  for (uint32_t o = 0; o < 4; ++o) {
    const uint32_t d = (code >> (o * 3)) & 0x7;
    const bool equal = d == o || (o >= components && ((o == 3 && d == 5) || (o != 3 && d == 4)));
    if (equal) code |= uint32_t(0x7) << (o * 3);
  }
  return code;
}

inline VkFormat AttributeFormat(uint32_t format, bool whole_entry, bool with_sign, bool whole,
                         bool red_blue, bool& r11g11b10) {
  r11g11b10 = false;
  const auto choose = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                          VkFormat uint_, VkFormat sint) {
    if (whole_entry) {
      return with_sign ? sint : uint_;
    }
    if (whole) {
      return with_sign ? sscaled : uscaled;
    }
    return with_sign ? snorm : unorm;
  };
  switch (format) {
    case 6:  // k_8_8_8_8
      return red_blue ? choose(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
                                VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED,
                                VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT)
                       : choose(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                                VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      if (red_blue) break;
      return choose(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                    VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                    VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:  // k_10_11_11: packed normal decoded by the shader itself
      if (!whole_entry || red_blue) break;
      r11g11b10 = true;
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                    VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                    VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                    VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return whole_entry || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return whole_entry || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return whole_entry && !red_blue ? (with_sign ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                                         : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return whole_entry && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                 : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return whole_entry && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                 : VK_FORMAT_UNDEFINED;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT);
    case 37:  // k_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT);
    case 57:  // k_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT);
    case 38:  // k_32_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32G32B32A32_UINT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT);
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

struct TextureFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;  // the logical format (keys, logs, format checks), as in the Vulkan code
  DkImageFormat dk = DkImageFormat_None;  // what the image is created with
  uint8_t block = 1;       // texels per block side
  uint8_t bytes = 1;        // bytes per block
  uint8_t unit_order = 0;  // 0 = no byte swap, else 2 or 4 bytes
  uint16_t swizzle_host = kSwizzleRGBA;
};

// banjo-nx: k_CTX1 has no Vulkan format. Its 8-byte blocks hold two 8:8 (X, Y) endpoints and sixteen 2-bit codes
// with the DXT1 four-value palette (end0, end1, 2/3 end0 + 1/3 end1, 1/3 end0 + 2/3 end1), so each block becomes
// a BC1 block with the same codes: X in red (5 bits), Y in green (6 bits). BC1 needs color0 > color1 for that
// palette: otherwise the endpoints swap and the codes follow (0<->1, 2<->3); equal endpoints use code 0 only.
inline void TranscodeCtx1(uint8_t* data, size_t bytes) {
  for (size_t at = 0; at + 8 <= bytes; at += 8) {
    uint32_t ends, codes;
    std::memcpy(&ends, data + at, 4);
    std::memcpy(&codes, data + at + 4, 4);
    const auto rgb565 = [](uint32_t x, uint32_t y) { return uint16_t(((x * 31 + 127) / 255) << 11 | ((y * 63 + 127) / 255) << 5); };
    uint16_t c0 = rgb565(ends & 0xFF, (ends >> 8) & 0xFF);
    uint16_t c1 = rgb565((ends >> 16) & 0xFF, ends >> 24);
    if (c0 < c1) {
      std::swap(c0, c1);
      codes ^= 0x55555555u;  // 0<->1, 2<->3
    } else if (c0 == c1) {
      codes = 0;
    }
    const uint32_t bc1 = uint32_t(c0) | (uint32_t(c1) << 16);
    std::memcpy(data + at, &bc1, 4);
    std::memcpy(data + at + 4, &codes, 4);
  }
}

// Same host formats as the emulation.
// deko3d: the same table with the DkImageFormat next to the logical VkFormat. The 16-bit packed formats are not the
// Vulkan ones: deko3d has the layouts with X in the low bits (maxwell/format_traits.inc: RGBA4_Unorm = A4B4G4R4 with
// R in bits 0-3, RGB565_Unorm = B5G6R5 with R in bits 0-4, RGB5A1_Unorm = A1B5G5R5 with R in bits 0-4), so the
// guest components land in R, G, B, A in order and the host swizzle is the identity (k_5_6_5's W still reads Z).
// The logical VkFormat stays the Vulkan one so the keys and the logs do not change. Bit order to be confirmed on
// the console (ds2-deko3d bench-semantica, test 10: one texture of each packed format read back).
inline bool TextureFormatFor(uint32_t format, TextureFormat& f) {
  switch (format) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {VK_FORMAT_R8_UNORM, DkImageFormat_R8_Unorm, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {VK_FORMAT_R8G8_UNORM, DkImageFormat_RG8_Unorm, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {VK_FORMAT_R8G8B8A8_UNORM, DkImageFormat_RGBA8_Unorm, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, DkImageFormat_RGB10A2_Unorm, 1, 4, 4, kSwizzleRGBA};
      return true;
    // banjo-nx: the 16-bit packed formats, copied as they are into a mandatory Vulkan packed format of the same layout
    // (Xenos components from the low bits up: X, Y, Z, W); the host swizzle maps each guest component to the field
    // that holds it, so no conversion pass is needed (the emulation converts in its load shaders instead).
    case 60:  // k_CTX1 (two-channel normal maps): transcoded in place to BC1, same 8-byte 4x4 blocks (TranscodeCtx1)
      f = {VK_FORMAT_BC1_RGB_UNORM_BLOCK, DkImageFormat_RGB_BC1, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 15:  // k_4_4_4_4: X 0-3, Y 4-7, Z 8-11, W 12-15; B4G4R4A4 has A 0-3, R 4-7, G 8-11, B 12-15
              // (deko3d RGBA4_Unorm: R 0-3, G 4-7, B 8-11, A 12-15)
      f = {VK_FORMAT_B4G4R4A4_UNORM_PACK16, DkImageFormat_RGBA4_Unorm, 1, 2, 2, kSwizzleRGBA};
      return true;
    case 4:   // k_5_6_5: X 0-4, Y 5-10, Z 11-15; R5G6B5 has B 0-4, G 5-10, R 11-15 (W reads Z, as in the emulation)
              // (deko3d RGB565_Unorm: R 0-4, G 5-10, B 11-15)
      f = {VK_FORMAT_R5G6B5_UNORM_PACK16, DkImageFormat_RGB565_Unorm, 1, 2, 2, uint16_t(0 | (1 << 3) | (2 << 6) | (2 << 9))};
      return true;
    case 3:   // k_1_5_5_5: X 0-4, Y 5-9, Z 10-14, W 15; A1R5G5B5 has B 0-4, G 5-9, R 10-14, A 15
              // (deko3d RGB5A1_Unorm: R 0-4, G 5-9, B 10-14, A 15)
      f = {VK_FORMAT_A1R5G5B5_UNORM_PACK16, DkImageFormat_RGB5A1_Unorm, 1, 2, 2, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, DkImageFormat_RGBA_BC1, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {VK_FORMAT_BC2_UNORM_BLOCK, DkImageFormat_RGBA_BC2, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {VK_FORMAT_BC3_UNORM_BLOCK, DkImageFormat_RGBA_BC3, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {VK_FORMAT_BC5_UNORM_BLOCK, DkImageFormat_RG_BC5_Unorm, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {VK_FORMAT_BC4_UNORM_BLOCK, DkImageFormat_R_BC4_Unorm, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:  // k_16
      f = {VK_FORMAT_R16_UNORM, DkImageFormat_R16_Unorm, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {VK_FORMAT_R16G16_UNORM, DkImageFormat_RG16_Unorm, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {VK_FORMAT_R16G16B16A16_UNORM, DkImageFormat_RGBA16_Unorm, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {VK_FORMAT_R16_SFLOAT, DkImageFormat_R16_Float, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {VK_FORMAT_R16G16_SFLOAT, DkImageFormat_RG16_Float, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {VK_FORMAT_R16G16B16A16_SFLOAT, DkImageFormat_RGBA16_Float, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {VK_FORMAT_R32_SFLOAT, DkImageFormat_R32_Float, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {VK_FORMAT_R32G32_SFLOAT, DkImageFormat_RG32_Float, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {VK_FORMAT_R32G32B32A32_SFLOAT, DkImageFormat_RGBA32_Float, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

inline bool IsDepth(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// GuestToHostSwizzle: the fetch constant's swizzle, mapped through the channels the host format has.
// deko3d: the view's DkImageSwizzle (Zero = 0, One = 1, Red = 2 ... Alpha = 5; not VkComponentSwizzle's values),
// written straight into sa2::dk::ViewDesc::swizzle.
inline void MappingComponents(uint32_t swizzle_guest, uint16_t swizzle_host, DkImageSwizzle (&output)[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t value = (swizzle_guest >> (i * 3)) & 0x7;
    if (value <= 3) {
      const uint32_t host = (swizzle_host >> (value * 3)) & 0x7;
      output[i] = DkImageSwizzle(DkImageSwizzle_Red + host);
    } else if (value == 4) {
      output[i] = DkImageSwizzle_Zero;
    } else {
      output[i] = DkImageSwizzle_One;
    }
  }
}

// Four post-swizzle TextureSign values, two bits per sampled component. This is the same mapping as
// pipeline::texture::SwizzleSigns in the stock renderer. The native shader ABI packs it into bits 24-31
// of the descriptor index; the descriptor heap itself uses only the low 16 bits (ds2-nx: bits 16-22 carry
// the fetch constant's exp_adjust, ds2_texture_exp_adjust).
inline uint8_t RemappedSigns(const uint32_t* fetch) {
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  uint8_t signs = 0;
  bool signed_any = false, any_no_signed = false;
  uint8_t constant_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t component = (swizzle >> (i * 3)) & 0x7;
    if (component & 0x4) {
      constant_mask |= uint8_t(1) << (i * 2);
      continue;
    }
    const uint32_t sign = (fetch[0] >> (2 + component * 2)) & 0x3;
    signs |= uint8_t(sign << (i * 2));
    signed_any |= sign == uint32_t(xenos::TextureSign::kSigned);
    any_no_signed |= sign != uint32_t(xenos::TextureSign::kSigned);
  }
  uint32_t sign_constants = uint32_t(xenos::TextureSign::kUnsigned);
  if ((constant_mask == 0x55 && ((fetch[0] >> 2) & 0xFF) == 0x55) ||
      (constant_mask != 0x55 && signed_any && !any_no_signed)) {
    sign_constants = uint32_t(xenos::TextureSign::kSigned);
  }
  signs |= uint8_t(sign_constants) * constant_mask;
  return signs;
}

struct AttributeVertices {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
};

struct BindingVertices {
  uint32_t slot;   // fetch constant of the vertices (0-95)
  uint32_t stride;  // bytes
};

// banjo-nx: a k_10_11_11 / k_11_11_10 attribute read as normalized floats. Vulkan has no such vertex format: the copy
// repacks each one in place into 10:10:10:2 (the 11-bit components lose their lowest bit), read as A2B10G10R10.
struct RepackVertices {
  uint32_t binding;
  uint32_t offset;  // bytes into the vertex
  uint32_t format;  // 16 = k_10_11_11 (11, 11, 10 bits from the low end), 17 = k_11_11_10 (10, 11, 11)
  bool with_sign;
};

inline void RepackWord1011(uint8_t* p, uint32_t format, bool with_sign) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  const uint32_t widths[3] = {format == 16 ? 11u : 10u, 11u, format == 16 ? 10u : 11u};
  uint32_t out = 0, shift = 0;
  for (uint32_t c = 0; c < 3; ++c) {
    const uint32_t width = widths[c];
    uint32_t value = (v >> shift) & ((1u << width) - 1u);
    shift += width;
    if (width == 11) {
      value = with_sign ? uint32_t(int32_t(value << 21) >> 22) & 0x3FFu : value >> 1;
    }
    out |= (value & 0x3FFu) << (c * 10);
  }
  out |= (with_sign ? 1u : 3u) << 30;  // W = 1.0
  std::memcpy(p, &out, 4);
}

// The vertex input of a VerticesEntry as deko3d state: attributes indexed by LOCATION (deko3d has no location
// field: attribute i feeds shader input i), the holes marked isFixed (a constant 0, as an unbound Vulkan
// location). Built once per entry (VerticesLayoutFor), bound only when the entry changes.
struct VerticesLayout {
  std::array<DkVtxAttribState, 16> attributes{};
  std::array<DkVtxBufferState, 16> buffers{};
  uint32_t n_attributes = 0;  // highest location + 1
  uint32_t n_buffers = 0;
  bool valid = false;  // false: a format with no deko3d equivalent (logged by the caller)
};
struct VerticesEntry {
  std::vector<AttributeVertices> attributes;
  std::vector<BindingVertices> bindings;
  std::vector<RepackVertices> repacks;  // banjo-nx
  std::array<uint32_t, 16> remaps{};  // g_InputRemap by location
  uint32_t specialization = 0;
  uint64_t fingerprint = 0;
  // deko3d: what CreatePipelineVulkan put in the pipeline's vertex input state, built on first use by the draw
  // (VerticesLayoutFor) and bound with dkCmdBufBindVtxAttribState / BindVtxBufferState. Whoever changes
  // attributes or bindings after that clears layout_built (the rectangle-list entry does, Draw).
  mutable VerticesLayout layout;
  mutable bool layout_built = false;
};

// Copy of the SPIR-V without the writes to the color targets (output variables with Location 0..3).

// PruneWritesOfColor, LimitOutputs7e3 and WithEarlyTests (Vulkan file, lines 2498-2712) rewrite SPIR-V at run time.
// In deko3d those variants (alpha-only, 7e3 clamp, early fragment tests) are built offline into the DKSH package
// and looked up with LibraryDksh::Variant (region 07); nothing rewrites shader code on the console.


struct PipelineKey {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t entry = 0;
  uint32_t topology = 0;
  uint32_t specialization = 0;
  uint32_t formats[5] = {};
  uint32_t blend[4] = {};
  uint32_t masks = 0;
  uint32_t depth = 0;
  uint32_t rasterization = 0;
  // The key is hashed and compared byte by byte (PipelineFor). With 76 bytes of fields and 8-byte alignment
  // it had 4 bytes of uninitialized implicit padding: stack garbage that made identical keys differ and
  // created duplicate pipelines (125-134, and 193-203 in a later version, with no change in the image).
  uint32_t fill = 0;
  uint32_t fill2 = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineKey>,
              "PipelineKey cannot have implicit padding: it is hashed and compared byte by byte");

/*
 * One pipeline of the prewarm list (sa2_native_pipelines_prewarm).
 *
 * What is needed to recreate it in another session exactly as the ring created it: its key as PipelineFor
 * looks it up; the fingerprint of its two shaders in the library (if the library has changed, key.vs
 * and key.ps are no longer the same shaders and the record is skipped); and its vertex input, which the
 * key only carries as a fingerprint. It is stored on disk byte for byte, so it cannot have implicit
 * padding.
 */
struct AttributeRegister {
  uint32_t location = 0;
  uint32_t binding = 0;
  uint32_t format = 0;  // VkFormat (logical; AttributeFormatDk at the GPU boundary)
  uint32_t offset = 0;
};

struct RegisterPipeline {
  static constexpr uint32_t kMaxAttributes = 16;
  static constexpr uint32_t kMaxBindings = 16;
  PipelineKey key;
  uint64_t vs_fingerprint = 0;  // sa2::native::Shader::fingerprint
  uint64_t ps_fingerprint = 0;  // 0 without a fragment stage (key.ps == 0)
  uint32_t n_attributes = 0;
  uint32_t n_bindings = 0;
  AttributeRegister attributes[kMaxAttributes] = {};
  uint32_t strides[kMaxBindings] = {};
};
static_assert(std::has_unique_object_representations_v<RegisterPipeline>,
              "RegisterPipeline is saved to disk byte by byte: no implicit padding");

struct Texture {
  ImageNativeDk image;
  uint32_t layers = 1;  // 6 for cubemaps
  uint32_t background = 0;  // slices of 3D textures; 0 for the rest
  uint64_t fingerprint = 0;
  uint64_t frame = UINT64_MAX;
  uint64_t raw_fingerprint = 0;  // XXH3 of the guest bytes (2D and cubemaps)
  bool raw_pending = false;  // ds2_texture_lazy_fingerprint: uploaded without it; the first recheck takes it
  uint64_t next = 0;     // frame of the next check
  uint32_t interval = 1;     // frames between checks: 1 to sa2_native_texture_interval_max
  uint8_t late_changes = 0;  // times a change was found after the texture had been stable (sa2_native_adaptive_texture)
  uint32_t postponements = 0;  // consecutive checks deferred by the budget
  // Sample fingerprint (SampleFingerprint) of the same content as raw_fingerprint if valid_sample, and how many
  // consecutive rechecks have been accepted on it alone (sa2_native_fingerprints_sampling).
  uint64_t sample_fingerprint = 0;
  bool valid_sample = false;
  uint8_t consecutive_samples = 0;
  bool needs_upload = false;
  std::vector<uint8_t> data;  // levels already laid out for the host: level after level and, in each, layer after layer
  uint32_t levels = 1;        // mip levels of the host image
  std::array<uint32_t, 16> level_displacement{};  // data bytes up to each level
  uint64_t bytes = 0;          // what it counts in bytes_textures_ (for eviction)
  // 1 + its index in in_flight_ while the bind thread runs its vkBindImageMemory; 0 = no. While set, the
  // image exists but has no memory: no command may reference it until CollectBindings.
  // deko3d: always 0 (an image gets its memory in sa2::dk::CreateImage, no bind thread); kept so the tests
  // that read it stay as they are.
  uint32_t in_flight = 0;
  // Measurement only (sa2_native_diag_reuse): its shape without the address (when the image is
  // created), its address (only for the log), the content key it is filed under in live_per_content_
  // (0 = none) and whether it is new and its first fingerprint has not been measured yet.
  uint64_t shape_content = 0;
  uint64_t content_key = 0;
  uint32_t address = 0;
  bool content_per_measure = false;
};

// deko3d: no view object. What stays is the descriptor slot the view was written to (sa2::dk::View +
// WriteImageDescriptor at DescriptorOfSlot(heap, slot)) and the image it shows.
struct View {
  sa2::dk::ImageId image = sa2::dk::kNoImage;
  uint32_t slot = 0;
  uint32_t heap = 0;  // 0 2D textures, 2 cubemaps
};


// ===============================================================================================================
// deko3d: what replaces the Vulkan objects the class used to create (descriptor heaps, vertex input state, the
// pipeline and its specialization). Shared by both halves of the class (sa2_native_draws_dk_1.cpp and _2.cpp).
// ===============================================================================================================

// --- Texture heaps -> ranges of the one image descriptor set ---------------------------------------------------
// The game shaders' handle is image | sampler << 20 (ds2-deko3d tools/shaders/reescrever.py): the 2D, 3D and cube
// heaps of the Vulkan code share one image index space, so each heap is a fixed range (sa2_dk.h) and what the
// shared block carries in the low 16 bits of a texture word is the GLOBAL index (base of its heap + slot). The heaps
// keep their own slot numbering and capacities (kCapacityHeap) exactly as in Vulkan; only the descriptor written
// and the index the shader receives are offset.
constexpr uint32_t kDescriptorsBaseHeap[3] = {sa2::dk::kDescriptorsDraws2D, sa2::dk::kDescriptorsDraws3D,
                                              sa2::dk::kDescriptorsDrawsCube};
static_assert(kCapacityHeap[0] <= sa2::dk::kDescriptorsDraws3D - sa2::dk::kDescriptorsDraws2D &&
                  kCapacityHeap[1] <= sa2::dk::kDescriptorsDrawsCube - sa2::dk::kDescriptorsDraws3D &&
                  kCapacityHeap[2] <= sa2::dk::kDescriptorsDraws - sa2::dk::kDescriptorsDrawsCube &&
                  kCapacityHeap[3] <= sa2::dk::kSamplersDraws,
              "a draws heap does not fit its range of the descriptor sets (sa2_dk.h)");
static_assert(sa2::dk::kDescriptorsDraws <= 0xFFFF, "the shaders mask the texture index with 0xFFFF");
// Image descriptor of slot `slot` of heap `heap` (0 2D, 1 3D, 2 cube).
inline uint32_t DescriptorOfSlot(uint32_t heap, uint32_t slot) { return kDescriptorsBaseHeap[heap] + slot; }
// Sampler descriptor of a sampler heap slot (the draws' samplers are the first kSamplersDraws of the set).
inline uint32_t DescriptorOfSampler(uint32_t slot) { return slot; }

// --- Vertex attributes ----------------------------------------------------------------------------------------
// The host formats AttributeFormat (and ComponentsVertexFormat) can give, to a DkVtxAttribState's size, type and
// BGRA swap. false = no deko3d equivalent (the caller rejects as with VK_FORMAT_UNDEFINED).
inline bool AttributeFormatDk(VkFormat format, DkVtxAttribSize& size, DkVtxAttribType& type, bool& bgra) {
  bgra = false;
  const auto set = [&](DkVtxAttribSize s, DkVtxAttribType t) {
    size = s;
    type = t;
    return true;
  };
  switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_B8G8R8A8_SNORM: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Snorm);
    case VK_FORMAT_B8G8R8A8_USCALED: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled);
    case VK_FORMAT_B8G8R8A8_SSCALED: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sscaled);
    case VK_FORMAT_B8G8R8A8_UINT: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uint);
    case VK_FORMAT_B8G8R8A8_SINT: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sint);
    case VK_FORMAT_R8G8B8A8_UNORM: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_R8G8B8A8_SNORM: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Snorm);
    case VK_FORMAT_R8G8B8A8_USCALED: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R8G8B8A8_SSCALED: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R8G8B8A8_UINT: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uint);
    case VK_FORMAT_R8G8B8A8_SINT: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sint);
    // A2B10G10R10 (Vulkan) = R in the low 10 bits, as deko3d's 10_10_10_2.
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Unorm);
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Snorm);
    case VK_FORMAT_A2B10G10R10_USCALED_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Uscaled);
    case VK_FORMAT_A2B10G10R10_SSCALED_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Sscaled);
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Uint);
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Sint);
    case VK_FORMAT_R8_UNORM: return set(DkVtxAttribSize_1x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_R8G8_UNORM: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_R16_UNORM: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Unorm);
    case VK_FORMAT_R16_SFLOAT: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Float);
    case VK_FORMAT_R16G16_UNORM: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Unorm);
    case VK_FORMAT_R16G16_SNORM: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Snorm);
    case VK_FORMAT_R16G16_USCALED: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R16G16_SSCALED: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R16G16_UINT: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Uint);
    case VK_FORMAT_R16G16_SINT: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Sint);
    case VK_FORMAT_R16G16_SFLOAT: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Float);
    case VK_FORMAT_R16G16B16A16_UNORM: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Unorm);
    case VK_FORMAT_R16G16B16A16_SNORM: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Snorm);
    case VK_FORMAT_R16G16B16A16_USCALED: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R16G16B16A16_SSCALED: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R16G16B16A16_UINT: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Uint);
    case VK_FORMAT_R16G16B16A16_SINT: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Sint);
    case VK_FORMAT_R16G16B16A16_SFLOAT: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Float);
    case VK_FORMAT_R32_UINT: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32_SINT: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Sint);
    case VK_FORMAT_R32_SFLOAT: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Float);
    case VK_FORMAT_R32G32_UINT: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32G32_SINT: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Sint);
    case VK_FORMAT_R32G32_SFLOAT: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Float);
    case VK_FORMAT_R32G32B32_UINT: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32G32B32_SFLOAT: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Float);
    case VK_FORMAT_R32G32B32A32_UINT: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32G32B32A32_SINT: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Sint);
    case VK_FORMAT_R32G32B32A32_SFLOAT: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Float);
    default: return false;
  }
}

// (VerticesLayout is declared before VerticesEntry, which carries one.)
inline VerticesLayout VerticesLayoutFor(const VerticesEntry& entry) {
  VerticesLayout layout;
  layout.valid = true;
  for (DkVtxAttribState& a : layout.attributes) {
    a = {};
    a.isFixed = 1;
    a.size = DkVtxAttribSize_1x32;
    a.type = DkVtxAttribType_Float;
  }
  for (const AttributeVertices& a : entry.attributes) {
    if (a.location >= layout.attributes.size() || a.offset >= (1u << 14)) {
      layout.valid = false;
      continue;
    }
    DkVtxAttribSize size{};
    DkVtxAttribType type{};
    bool bgra = false;
    if (!AttributeFormatDk(a.format, size, type, bgra)) {
      layout.valid = false;
      continue;
    }
    DkVtxAttribState& s = layout.attributes[a.location];
    s = {};
    s.bufferId = a.binding;
    s.isFixed = 0;
    s.offset = a.offset;
    s.size = size;
    s.type = type;
    s.isBgra = bgra ? 1 : 0;
    layout.n_attributes = std::max(layout.n_attributes, a.location + 1);
  }
  layout.n_buffers = uint32_t(std::min<size_t>(entry.bindings.size(), layout.buffers.size()));
  for (uint32_t i = 0; i < layout.n_buffers; ++i) {
    layout.buffers[i] = DkVtxBufferState{entry.bindings[i].stride, 0};
  }
  return layout;
}

// --- Topology, compare and stencil ops (the Vulkan enums the key keeps, to deko3d) -------------------------------
// VkPrimitiveTopology 0..10 -> DkPrimitive. Not the same numbers: deko3d's 2 is LineLoop.
inline DkPrimitive PrimitiveDk(uint32_t topology) {
  switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return DkPrimitive_Points;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return DkPrimitive_Lines;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return DkPrimitive_LineStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return DkPrimitive_Triangles;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return DkPrimitive_TriangleFan;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: return DkPrimitive_LinesAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return DkPrimitive_LineStripAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY: return DkPrimitive_TrianglesAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: return DkPrimitive_TriangleStripAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return DkPrimitive_Patches;
    default: return DkPrimitive_Triangles;
  }
}
// VkCompareOp NEVER..ALWAYS (0..7) -> DkCompareOp Never..Always (1..8); VkStencilOp KEEP..DECREMENT_AND_WRAP
// (0..7) -> DkStencilOp Keep..DecrWrap (1..8): the same order, one more.
inline DkCompareOp CompareDk(uint32_t op) { return DkCompareOp((op & 0x7) + 1); }
inline DkStencilOp StencilDk(uint32_t op) { return DkStencilOp((op & 0x7) + 1); }
// Both shifts rest on the enums keeping Vulkan's (and the Xenos register's) order one value up.
static_assert(DkCompareOp_Never == VK_COMPARE_OP_NEVER + 1 && DkCompareOp_Lequal == VK_COMPARE_OP_LESS_OR_EQUAL + 1 &&
              DkCompareOp_NotEqual == VK_COMPARE_OP_NOT_EQUAL + 1 && DkCompareOp_Always == VK_COMPARE_OP_ALWAYS + 1);
static_assert(DkStencilOp_Keep == VK_STENCIL_OP_KEEP + 1 && DkStencilOp_Incr == VK_STENCIL_OP_INCREMENT_AND_CLAMP + 1 &&
              DkStencilOp_Invert == VK_STENCIL_OP_INVERT + 1 &&
              DkStencilOp_DecrWrap == VK_STENCIL_OP_DECREMENT_AND_WRAP + 1);

// --- The "pipeline" ---------------------------------------------------------------------------------------------
// deko3d has no pipeline object: what a VkPipeline baked (FillFixedState, Vulkan file line 12170) is plain state
// bound by groups. PipelineFor keeps its key (PipelineKey), its one-entry shortcut, its 256 cells and its map; the
// value is a PipelineDk, filled by FillStateDk (region 06, from FillFixedState with the same arithmetic).
// Widest line deko3d draws (Maxwell always has wide lines; nouveau nvc0 reports 10.0 for aliased lines,
// nvc0_screen.c PIPE_CAPF_MAX_LINE_WIDTH). To confirm on the console with a wide-line draw.
inline constexpr float kMaxLineWidthDk = 10.0f;

struct FixedState {
  DkRasterizerState raster{};
  DkColorState color{};
  DkColorWriteState write{};
  std::array<DkBlendState, 4> blend{};  // compacted as the render targets are (only the formats present)
  uint32_t n_blend = 0;
  DkDepthStencilState depth{};
  DkMultisampleState ms{};
  DkPrimitive primitive = DkPrimitive_Triangles;
  bool restart = false;  // primitive restart; the index (0xFFFF / 0xFFFFFFFF) follows the draw's index format
  float line_width = 1.0f;
  FixedState() {
    dkRasterizerStateDefaults(&raster);
    dkColorStateDefaults(&color);
    dkColorWriteStateDefaults(&write);
    for (DkBlendState& b : blend) dkBlendStateDefaults(&b);
    dkDepthStencilStateDefaults(&depth);
    dkMultisampleStateDefaults(&ms);
  }
};

// UBO 3: the shaders' specialization constants (reescrever.py turns every constant_id into a field of a 4th
// uniform block, read at run time). Vertex shaders declare constant 0 (PipelineKey::specialization) and 60 (the
// inputs the pixel shader reads, InputsMaskPS); pixel shaders only 0. The fields are uints in std140, in the order
// the GLSL declares them, so a block is {constant 0, constant 60} (to be confirmed per shader against the
// package's metadata: region 07 records the field order).
// One 256-byte block per distinct pair, written once and never rewritten (so no submission in flight ever reads a
// block that changes): a persistent table, not per-slot memory.
class SpecTable {
 public:
  static constexpr uint32_t kBlockBytes = DK_UNIFORM_BUF_ALIGNMENT;
  static constexpr uint32_t kBoundBytes = 16;  // what is bound: one vec4's worth (two uint fields)
  bool Initialize(uint32_t blocks = 4096) {
    block_ = sa2::dk::MakeBlock(blocks * kBlockBytes, sa2::dk::kCpuToGpu, "draws spec table");
    capacity_ = block_ ? blocks : 0;
    used_ = 0;
    map_.clear();
    return bool(block_);
  }
  void Release() { sa2::dk::FreeBlock(block_, sa2::dk::CurrentOrder()); }
  // DK_GPU_ADDR_INVALID when full (logged once by the caller).
  DkGpuAddr For(uint32_t constant0, uint32_t constant60) {
    const uint64_t key = uint64_t(constant0) | (uint64_t(constant60) << 32);
    const auto it = map_.find(key);
    if (it != map_.end()) return it->second;
    if (used_ >= capacity_) return DK_GPU_ADDR_INVALID;
    const uint32_t offset = used_++ * kBlockBytes;
    const uint32_t words[4] = {constant0, constant60, 0, 0};
    std::memcpy(block_.cpu + offset, words, sizeof(words));  // CpuUncached: visible without a flush
    const DkGpuAddr address = block_.gpu + offset;
    map_.emplace(key, address);
    return address;
  }
  uint32_t Used() const { return used_; }

 private:
  sa2::dk::Block block_;
  std::unordered_map<uint64_t, DkGpuAddr> map_;
  uint32_t used_ = 0, capacity_ = 0;
};

// What PipelineFor gives instead of a VkPipeline. Its address is its identity (the "same pipeline" tests and
// pipeline_bound_ compare pointers, as they compared handles); the cache never moves or frees one.
struct PipelineDk {
  const DkShader* vs = nullptr;
  const DkShader* ps = nullptr;  // nullptr without a fragment stage (key.ps == 0)
  FixedState state;
  DkGpuAddr spec_vs = DK_GPU_ADDR_INVALID;  // UBO 3 of each stage (SpecTable)
  DkGpuAddr spec_ps = DK_GPU_ADDR_INVALID;
  bool generic = false;  // the package had no exact variant for the key (LibraryDksh::Variant)
};

// Replaces vkCmdBindPipeline: binds the groups of a PipelineDk that differ from what this command list has bound.
// Every new command list (and anything that binds state behind the draws' back: the renderer's own passes, the
// clears) starts with Reset(), the deko3d form of "a new command buffer has no pipeline bound".
class StateTracker {
 public:
  enum Group : uint32_t { kShaders, kRaster, kColor, kBlend, kDepth, kMultisample, kRestart, kLineWidth, kSpec,
                          kGroups };
  void Reset() { valid_ = 0; }
  void Bind(DkCmdBuf cmd, const PipelineDk& p) {
    if (!Has(kShaders) || p.vs != vs_ || p.ps != ps_) {
      const DkShader* shaders[2] = {p.vs, p.ps};
      dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, p.ps ? 2 : 1);
      vs_ = p.vs;
      ps_ = p.ps;
      Mark(kShaders);
    }
    const FixedState& s = p.state;
    if (!Has(kRaster) || !Same(s.raster, state_.raster)) {
      dkCmdBufBindRasterizerState(cmd, &s.raster);
      state_.raster = s.raster;
      Mark(kRaster);
    }
    if (!Has(kColor) || !Same(s.color, state_.color) || !Same(s.write, state_.write)) {
      dkCmdBufBindColorState(cmd, &s.color);
      dkCmdBufBindColorWriteState(cmd, &s.write);
      state_.color = s.color;
      state_.write = s.write;
      Mark(kColor);
    }
    if (s.n_blend && (!Has(kBlend) || s.n_blend != state_.n_blend ||
                      std::memcmp(s.blend.data(), state_.blend.data(), sizeof(DkBlendState) * s.n_blend) != 0)) {
      dkCmdBufBindBlendStates(cmd, 0, s.blend.data(), s.n_blend);
      state_.blend = s.blend;
      state_.n_blend = s.n_blend;
      Mark(kBlend);
    }
    if (!Has(kDepth) || !Same(s.depth, state_.depth)) {
      dkCmdBufBindDepthStencilState(cmd, &s.depth);
      state_.depth = s.depth;
      Mark(kDepth);
    }
    if (!Has(kMultisample) || !Same(s.ms, state_.ms)) {
      dkCmdBufBindMultisampleState(cmd, &s.ms);
      state_.ms = s.ms;
      Mark(kMultisample);
    }
    if (!Has(kLineWidth) || s.line_width != state_.line_width) {
      dkCmdBufSetLineWidth(cmd, s.line_width);
      state_.line_width = s.line_width;
      Mark(kLineWidth);
    }
    if (!Has(kSpec) || p.spec_vs != spec_vs_ || p.spec_ps != spec_ps_) {
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 3, p.spec_vs, SpecTable::kBoundBytes);
      if (p.ps) dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 3, p.spec_ps, SpecTable::kBoundBytes);
      spec_vs_ = p.spec_vs;
      spec_ps_ = p.spec_ps;
      Mark(kSpec);
    }
    ++binds_;
  }
  // Primitive restart is per draw in deko3d (the index depends on the index format): the draw calls this.
  void Restart(DkCmdBuf cmd, bool enable, bool indices16) {
    const uint32_t index = indices16 ? 0xFFFFu : 0xFFFFFFFFu;
    if (!Has(kRestart) || enable != restart_ || (enable && index != restart_index_)) {
      dkCmdBufSetPrimitiveRestart(cmd, enable, index);
      restart_ = enable;
      restart_index_ = index;
      Mark(kRestart);
    }
  }
  uint64_t Binds() const { return binds_; }

 private:
  template <class T>
  static bool Same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
  bool Has(Group g) const { return (valid_ >> g) & 1; }
  void Mark(Group g) { valid_ |= 1u << g; }
  uint32_t valid_ = 0;
  const DkShader* vs_ = nullptr;
  const DkShader* ps_ = nullptr;
  FixedState state_;
  DkGpuAddr spec_vs_ = DK_GPU_ADDR_INVALID, spec_ps_ = DK_GPU_ADDR_INVALID;
  bool restart_ = false;
  uint32_t restart_index_ = 0;
  uint64_t binds_ = 0;
};

// (BufferUploadDk and PostponedSkyDk, proposed for region 06, are gone: it keeps uploads_ / shared_bufs_ as
// sa2::dk::Arena, vcache_buf_ as sa2::dk::Block and its own PostponedSky; the sky's vertex input is sky_layout_.)

// The Vulkan structs the translated logic keeps as data, to deko3d at the call (viewports: sa2::dk::SetViewport).
inline DkScissor ScissorDk(const VkRect2D& r) {
  return DkScissor{uint32_t(std::max(0, r.offset.x)), uint32_t(std::max(0, r.offset.y)), r.extent.width,
                   r.extent.height};
}

}  // namespace draws_dk
}  // namespace sa2::native
#endif  // SA2_DK_AVAILABLE
