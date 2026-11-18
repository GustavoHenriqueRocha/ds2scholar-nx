// sa2_native_draws_dk_1.cpp: bodies of region draws-1 of the deko3d draws class (sa2_native_draws_dk_1.inc), the
// translation of lines 1-8158 of sa2_native_draws.cpp, in the same order. The Vulkan comments are kept; what deko3d
// changes is said in "deko3d:" notes next to it, and log lines whose meaning changed carry [dk].
// Contract: ds2-deko3d docs/pesquisas/traducao/08-contrato-sa2-dk.md; decisions: 05-draws-1.md ("Fase C").

// sa2_native_draws_dk_types.h first (through the class header): it sets the xxHash options before anything else
// can include xxhash.h.
#include "sa2_native_draws_dk.h"

#if SA2_DK_AVAILABLE
#include <switch.h>

// --- The draws' enhancements, deko3d copies (the Vulkan ones are at global scope in sa2_native_draws.cpp, lines
// 898-1006; SA2_ENHANCEMENT_DK declares the cvar, so it has to be at global scope too). g_push_once has no copy: the
// deko3d shaders have no push constants. ---------------------------------------------------------------------------
SA2_ENHANCEMENT_DK(g_dk_bias_seen, sa2_cpu_ring_bias_seen, sa2::enh::kCpu,
                   "Ring thread: render-target exponent biases already logged are remembered in a small table instead of an "
                   "unordered_set insert on every draw into such a target");
SA2_ENHANCEMENT_DK(g_dk_entry_hash, sa2_cpu_ring_entry_hash, sa2::enh::kCpu,
                   "Ring thread: the vertex-input cache key reuses the ring's XXH3 of the loaded VS instead of hashing "
                   "the program again on every VS change (self-checked)");
SA2_ENHANCEMENT_DK(g_dk_linear_not_resolved, sa2_native_linear_fetch_not_resolved, sa2::enh::kNonCpu,
                   "Renderer: a linear (untiled) texture fetch is never taken for a resolved texture (resolves write tiled "
                   "layouts only): CPU data at a freed resolve's address is read from guest memory instead of being "
                   "rejected (Wild Canyon's mission box centre)");
SA2_ENHANCEMENT_DK(g_dk_slots_evict, sa2_native_texture_slots_evict, sa2::enh::kNonCpu,
                   "Renderer: the texture cache also releases its coldest textures when a descriptor heap is three "
                   "quarters full, and an eighth of the heap at once when a view finds no slot. Without it only the "
                   "cache's texel bytes count, and with small textures the 4,096 slots run out first: after hours of "
                   "play new textures are drawn black or not at all");
SA2_ENHANCEMENT_DK(g_dk_structured_cf, sa2_gpu_structured_cf, sa2::enh::kNonCpu,
                   "GPU: shaders whose forward-only control flow the translator proved equivalent as nested if/else run "
                   "that form (SPEC_CONSTANT_STRUCTURED_CF) instead of an `if (skipTo <= pc)` test per CF instruction "
                   "(needs the shader package that has it)");
SA2_ENHANCEMENT_DK(g_dk_texcoords_unswapped, sa2_gpu_texcoords_unswapped, sa2::enh::kNonCpu,
                   "GPU: vertex shaders skip the swapped-texcoord test of every texcoord input (a 64-bit mask test and a "
                   "swizzle select per vertex): the native renderer never swaps them (g_SwappedTexcoords is always 0)");
SA2_ENHANCEMENT_DK(g_dk_no_texture_signs, sa2_gpu_no_texture_signs, sa2::enh::kNonCpu,
                   "GPU: when no texture of a draw has a signed, biased or gamma component, its pixel shader skips the "
                   "per-fetch sign test (a shift, a compare and a branch per texture fetch)");
SA2_ENHANCEMENT_DK(g_dk_identity_remap, sa2_gpu_identity_remap, sa2::enh::kNonCpu,
                   "GPU: a pipeline whose vertex inputs all keep their component order (every g_InputRemap is 0xFFF) skips "
                   "the per-input remap test in its vertex shader (a constant load, a compare and a branch per input)");
SA2_ENHANCEMENT_DK(g_dk_prewarm_current, sa2_gpu_prewarm_current_keys, sa2::enh::kNonCpu,
                   "C6 prewarm: pipeline-list records whose keys no draw of this session can produce (another sa2_msaa, "
                   "the line width of an unused render scale, the other sa2_gpu_texcoords_unswapped state) are not "
                   "compiled at start; they stay in the list for the sessions that use those settings");
SA2_ENHANCEMENT_DK(g_dk_index_base_held, sa2_gpu_index_base_held, sa2::enh::kNonCpu,
                   "GPU: vertex shaders the translator proved never read the vertex index run with r0 starting at 0 "
                   "(SPEC_CONSTANT_VERTEX_INDEX_UNUSED) and the per-draw vertex index base is held at 0, so draws keep the "
                   "same shared constant block and set-4 binding (needs the shader package that has it)");
SA2_ENHANCEMENT_DK(g_dk_blend_skip, sa2_gpu_blend_skip, sa2::enh::kNonCpu,
                   "GPU: transparent pixels (alpha 0 or less with SRC_ALPHA / ONE_MINUS_SRC_ALPHA or additive blending, or "
                   "an all-zero source with ONE / ONE_MINUS_SRC_ALPHA) are discarded before the blend when the blend "
                   "would leave the RGBA8 target unchanged and nothing writes depth or stencil");
SA2_ENHANCEMENT_DK(g_dk_base_zero_r0, sa2_cpu_vertices_base_zero_r0, sa2::enh::kCpu,
                   "Ring and recording threads: draws whose vertex shader reads r0 also take the base-zero vertex binding "
                   "(the upload buffer bound once, the draw shifted by firstVertex/vertexOffset), with the vertex index "
                   "base compensated so r0 is unchanged");
SA2_ENHANCEMENT_DK(g_dk_bound_plan_memo, sa2_cpu_ring_bound_plan_memo, sa2::enh::kCpu,
                   "Ring thread: the draw's EDRAM bound-range plan (PlanEdramBoundRanges, a pure function of its request) is "
                   "reused when the request equals the previous draw's");
SA2_ENHANCEMENT_DK(g_dk_compare_uploaded, sa2_cpu_ring_compare_uploaded, sa2::enh::kCpu,
                   "Ring thread: constants and shared-block change tests read the uploaded block (CPU-cached memory) "
                   "instead of keeping a second copy of every upload");

// New for deko3d: a barrier after every closed pass (what vkCmdEndRenderPass's external dependency gave the next
// reader of the targets). Conservative until the semantics bench says which readers need which.
REXCVAR_DEFINE_BOOL(sa2_dk_barrier_pass_end, true, "SA2 renderer",
                    "deko3d: barrier (render to sample) at the end of every draws pass. Turning it off is only for "
                    "measuring");

namespace sa2::native {

namespace {
// The process's memory as the kernel reports it (the GPU shares it on the Switch): total and used, in bytes.
bool ProcessMemoryDk(uint64_t& total, uint64_t& used) {
  return R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
         R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0));
}
}  // namespace

bool DrawsDkImpl::Initialize() {
  // sa2-nx render resolution: guest lines are drawn render-scale wide when the device has wide lines.
  // deko3d: Maxwell always has wide lines; no properties to query, the range is the hardware's (nouveau nvc0
  // reports a 10.0 maximum for aliased lines, nvc0_screen.c PIPE_CAPF_MAX_LINE_WIDTH).
  max_line_width_ = kMaxLineWidthDk;
  // (The Vulkan device requirements, shaderInt64 / bufferDeviceAddress / descriptor indexing: deko3d has them all
  // by construction. vkGetBufferDeviceAddress: blocks carry their GPU address. sa2_native_gpu_labels: no debug
  // labels in deko3d. LoadDynamicState: everything is dynamic state.)
  LoadCachePipelines();
  if (!CreateUpload() || !CreateDescriptors()) {
    return false;
  }
  // (pool_textures_.Start: sa2::dk::CreateImage is the pool; region 06 has no pool member.)
  return CreateEmpty();
}

bool DrawsDkImpl::Draw(const SubmissionDraw& p) {
  // Stage stopwatch on 1 in kStopwatchEach draws: reading the clock 12 times per draw took almost a
  // quarter of the ring thread's busy CPU on PC, and on the Switch each read costs more. The C6 averages
  // come from the sample.
  time_ = (++stopwatch_counter_ & (kStopwatchEach - 1)) == 0;
  if (time_) ++stage_draws_;  // the "C6 draw stages" report's divisor
  auto mark = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const uint32_t* r = p.registers;
  if (!r || !p.vs) {
    return false;
  }
  if (ResolveFetchAuditEnabled()) {
    me_resolve_fetch_vs_ = p.vs->number;
    me_resolve_fetch_ps_ = p.ps ? int(p.ps->number) : -1;
  }
  if (PayloadAuditEnabled()) {
    me_payload_vs_ = p.vs->number;
    me_payload_ps_ = p.ps ? int(p.ps->number) : -1;
    for (uint32_t info : {gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
                          gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO}) {
      const uint32_t format = (r[info] >> 16) & 15;
      if (format == 3 || format == 7 || format == 12) me_payload_hdr_ = true;
    }
  }
  // --- Primitive type ---------------------------------------------------
  const uint32_t edram_mode = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
  if (edram_mode != uint32_t(xenos::EdramMode::kColorDepth) && edram_mode != 5) {
    return Reject(1, "EDRAM mode without color or depth");
  }
  // Mode 5 (depth only): the Xenos does not run the pixel shader (IsPixelShaderNeededWithRasterization). Without a PS there are no textures, pixel constants or alpha test,
  // and the pipeline has no fragment stage. The shadow passes work this way, and some arrive with the PS
  // object set to 0.
  const ShaderEntry* ps =  // can be removed if it writes no color and does not discard
      edram_mode == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
  if (!ps && edram_mode == uint32_t(xenos::EdramMode::kColorDepth)) {
    return false;
  }
  const uint32_t starter = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
  const uint32_t type = starter & 0x3F;
  const uint32_t src_data = (starter >> 6) & 0x3;
  const bool indices32 = (starter >> 11) & 0x1;
  uint32_t count = starter >> 16;
  if (!count) {
    return true;
  }
  VkPrimitiveTopology topology;
  bool quads = false;
  bool fan = false;
  bool accepts_reset = false;
  switch (type) {
    case 1:
      // Mass Effect uses point lists in its D3D utility passes. Vulkan accepts the base topology;
      // point-sprite expansion (when enabled by raster state) is handled separately from topology.
      topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      break;
    case 2:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case 3:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      accepts_reset = true;
      break;
    case 4:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case 5:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      fan = true;
      accepts_reset = true;
      break;
    case 6:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      accepts_reset = true;
      break;
    case 8:
      // Expand the three real post-VS corners into six host vertices below.
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case 13:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      break;
    default:
      return Reject(100 + type, "primitive type not supported yet");
  }
  if (src_data == uint32_t(xenos::SourceSelect::kImmediate)) {
    return Reject(2, "immediate indices");
  }
  // Clipping disabled on the Xenos: the position may come in pixels (videos). It is drawn with a viewport
  // the size of the render target and the transform in the VS, like the emulation.
  const bool no_clip = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
  const uint32_t sc_mode = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  if (((sc_mode >> 3) & 0x3) == 2 && ((sc_mode >> 5) & 0x7) != 2) {
    return Reject(4, "polygons drawn as points or lines");
  }

  const VerticesEntry* entry = EntryFor(p);
  if (!entry) {
    return false;
  }
  Stage(0, mark);
  std::chrono::steady_clock::time_point t_indices_185 = mark;  // C6 substages 15-18

  // --- Targets --------------------------------------------------------------
  const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
  // B1 (black shards): the scene's depth prepass is drawn into the 2x view and its depth, brought back to
  // the 1x view, differs from the material pass's own depth on slopes; the material pass's depth test then
  // drops whole triangles. The material pass writes its own depth, so the prepass (depth-only draws at the
  // screen width, not rectangle clears) is skipped.
  static const uint32_t screen_width = uint32_t(rex::cvar::HasNonDefaultValue("video_mode_width") ?
      std::max<int32_t>(1, std::atoi(rex::cvar::GetFlagByName("video_mode_width").c_str())) : 1280);
  // (A smaller guest video mode keeps the 1280 surface pitch and narrows only the viewport.)
  if (edram_mode == 5 && (pitch == 1280 || pitch == screen_width) && type != 8 &&
      REXCVAR_GET(sa2_native_skip_prepass)) {
    return true;
  }
  const uint32_t register_mask =
      edram_mode == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
  static constexpr uint32_t kInfoColor[4] = {
      gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
      gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
  uint64_t keys[5] = {};
  uint32_t masks = 0;
  bool has_target = false;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t mask = (register_mask >> (i * 4)) & 0xF;
    if (!mask || !ps || !((ps->outputs >> i) & 0x1)) {
      continue;
    }
    const uint32_t info = r[kInfoColor[i]];
    const uint32_t format = (info >> 16) & 0xF;
    if (HostFormatTargetColor(format) == VK_FORMAT_UNDEFINED) {  // HDR targets
      return Reject(200 + format, "color target format not supported yet");
    }
    keys[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(format) << 16) |
                (uint64_t((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3) << 40) | pitch;
    masks |= mask << (i * 4);
    has_target = true;
  }
  /*
   * Draws that cannot change a single pixel (sa2_native_skip_invisibles).
   *
   * Two cases, read from this draw's own registers without any heuristics:
   *
   *  - Blending is "0 x source + 1 x destination" (ADD) on the written channels: the result is the
   *    destination as is. It is checked per channel group because the mask may write only RGB or only
   *    alpha, and then the other group's factors do not matter.
   *  - The alpha test function is 0 = NEVER: alphaTestValue (shader_common.h) returns -1 and clip() kills
   *    every fragment, before depth is written.
   *
   * If there is also no depth or stencil left to write, the whole draw is unnecessary: it is dropped. If
   * it writes depth it is drawn anyway and only counted, because setting the color mask to 0 would send it
   * down the "no color" path (kSpecAlphaOnly), where sa2_shadows_no_vegetation drops the whole draw and
   * its depth would be lost. Removing the color write saves too little to be worth it.
   */
  if (skip_invisibles_ && masks) {
    static constexpr uint32_t kBlendFor[4] = {
        gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
        gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    const uint32_t control_color_inv = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    const bool alpha_never =
        ((control_color_inv >> 3) & 0x1) && (control_color_inv & 0x7) == 0;  // function NEVER
    uint32_t useful_masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t i_mask = (masks >> (i * 4)) & 0xF;
      if (!i_mask) {
        continue;
      }
      const uint32_t m = r[kBlendFor[i]] & 0x1FFF1FFF;
      const bool copy_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
      const bool copy_alpha =
          ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
      const bool writes_rgb = (i_mask & 0x7) != 0 && !copy_color;
      const bool writes_alpha = (i_mask & 0x8) != 0 && !copy_alpha;
      if (writes_rgb || writes_alpha) {
        useful_masks |= i_mask << (i * 4);
      }
    }
    if ((!useful_masks || alpha_never)) {
      // With the NEVER function even the depth write dies (clip() runs in the shader, before the merger),
      // so the draw leaves no trace of any kind. With the blending that copies the destination, the draw can
      // only be dropped if it writes neither Z nor stencil either.
      const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
      const bool leaves_trail =
          !alpha_never && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
      ++counts_z_[alpha_never ? kInvisibleAlpha : kInvisibleBlend];
      if (!leaves_trail) {
        return true;
      }
      ++counts_z_[kInvisibleColorOnly];  // drawn anyway: only counts what is left to gain
    }
  }
  // With no color to write, the pixel shader can only have an effect by discarding pixels (alpha test or
  // its own kill) or by writing depth. If it does neither, the pipeline has no fragment stage and the image
  // is identical: the GPU saves shading the whole shadow map.
  // Of the draws that do write color, how many can use early depth rejection and how many force shading
  // before testing. That is what decides whether a depth pre-pass would help at all.
  if (masks && ps && has_target) {
    const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    const bool test_alpha =
        ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
    const bool discards = test_alpha || ps->discards || (ps->outputs & 0x10);
    if (pitch >= 1600) {
      // shadow map: already counted separately
    } else if (discards) {
      ++scene_with_discard_;
    } else {
      ++scene_no_discard_;
    }
  }
  // Shadow map detection: depth-only targets matching shadow dimensions (880x880, 1280x1280, 1600x1600).
  const bool target_shadows =  // also used for its depth bias (verified rule: pitch >= 1600)
      pitch >= 1600 && keys[4] && !keys[0] && !keys[1] && !keys[2] && !keys[3];
  bool alpha_only = false;
  if (!masks && ps) {
    const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    const bool test_alpha =
        ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
    const bool ps_writes_z = (ps->outputs & 0x10) != 0;
    if (!test_alpha && !ps->discards && !ps_writes_z) {
      ++draws_ps_useless_;
      if (cvars_per_frame_ ? no_ps_no_color_frame_ : NoPsNoColor()) {
        ps = nullptr;
      }
    } else {
      ++draws_ps_required_;
      // Diagnostic: in the shadow map the pixel shader reads constant c1 (g_bShadowMapAlphaEnabled). With
      // that constant at 0 it neither samples nor discards: the stage would be unnecessary.
      if (pitch >= 1600) {
        (Float(r[kRegConstantsPs + 4]) != 0.0f ? ++shadows_active_alpha_ : ++shadows_off_alpha_);
      }
      // Needed for the alpha test or a kill, but its color goes nowhere: it is compiled without those
      // writes.
      if (cvars_per_frame_ ? ps_alpha_only_frame_ : PsAlphaOnly()) {
        alpha_only = true;
      }
    }
  }
  /*
   * Shadow map vegetation discard, as early as possible.
   *
   * This same discard used to live 640 lines further down, right before the pipeline lookup. So these
   * draws were paid in full and then thrown away: index conversion, vertex sources, PrepareTexture for all
   * their samplers, upload buffer allocation, pass change, vertex copy, index and constant memcpy,
   * viewport, scissor and pipeline lookup.
   *
   * And it is not a handful of draws: 604 per frame are dropped, 31 % of those that come in (the
   * `draws_ps_required` counter matches the lost ones exactly across twelve intervals). At
   * ~8.5 us per incoming draw, that is 3-5 ms.
   *
   * Careful when measuring this: the divisor of `C6 stages` is the recorded draws (~1,370), not the
   * incoming ones (~1,970), even though the label says otherwise. Multiplying by the incoming ones inflates
   * the budget by 45 %.
   */
  const bool early_vegetation = alpha_only && no_vegetation_;
  if (early_vegetation) {
    ++draws_vegetation_soon_;
    return true;
  }
  const uint32_t control_depth = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
  if (control_depth & 0x3) {  // stencil or z
    const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
    keys[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) |
                (uint64_t((info >> 16) & 0x1) << 16) |
                (uint64_t((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3) << 40) | pitch;
    has_target = true;
  }
  // Match the SDK's same-base binding priority. ME1's lighting shaders may
  // declare two outputs at one physical base; binding the same sa2::dk::ImageId twice
  // exposes undefined unwritten-output data. Lower color slot wins, color
  // wins over depth (RenderTargetCache::Update, ME1 4D5307E6 comment).
  me::native::EdramBoundRequest bound_request;
  bound_request.pitch_pixels = pitch;
  bound_request.msaa = (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3;
  bound_request.depth_used = keys[4] != 0;
  bound_request.normalized_color_mask = uint16_t(masks);
  bound_request.views[0].base = uint32_t(keys[4] >> 24) & 0xFFF;
  for (uint32_t i = 0; i < 4; ++i)
    bound_request.views[i + 1].base = uint32_t(keys[i] >> 24) & 0xFFF;
  // sa2_cpu_ring_bound_plan_memo: the same request as the previous draw's, the same plan.
  if (!(g_dk_bound_plan_memo.active && bound_memo_valid_ && me::native::SameBoundRequest(bound_request, bound_memo_request_))) {
    bound_memo_plan_ = me::native::PlanEdramBoundRanges(bound_request);
    bound_memo_request_ = bound_request;
    bound_memo_valid_ = true;
  } else {
    g_dk_bound_plan_memo.UseSampled();
    if (sa2::enh::VerifyMode()) [[unlikely]] {
      sa2::enh::VerifyCheck();
      if (!me::native::SameBoundPlan(bound_memo_plan_, me::native::PlanEdramBoundRanges(bound_request))) {
        sa2::enh::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: EDRAM bound-plan memo differs from the plan for the same request");
        bound_memo_plan_ = me::native::PlanEdramBoundRanges(bound_request);
      }
    }
  }
  const auto& bound_plan = bound_memo_plan_;
  if (bound_plan.valid && bound_plan.suppressed_mask) {
    if (bound_plan.suppressed_mask & 1) keys[4] = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(bound_plan.suppressed_mask & (1u << (i + 1)))) continue;
      keys[i] = 0;
      masks &= ~(0xFu << (4 * i));
    }
    if (++me_bound_collisions_ <= 8)
      REXLOG_INFO("[native] EDRAM same-base binding: requested {:02X}, active {:02X}, VS n{} PS n{}",
                  bound_plan.requested_mask, bound_plan.active_mask, p.vs->number,
                  ps ? int(ps->number) : -1);
    has_target = bound_plan.active_mask != 0;
  }
  if (!has_target || !pitch) {
    return true;  // writes nothing visible
  }
  CutSubstage(15, t_indices_185);  // render targets and discards
  // --- Vertex and index range --------------------------------------------
  const uint32_t displacement = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
  const bool reset = accepts_reset && ((sc_mode >> 21) & 0x1);
  // Depth bias as in the emulation with host render targets (GetPreferredFacePolygonOffset).
  float scale_bias = 0.0f, bias_displacement = 0.0f;
  if (type >= 4 && type != 8) {  // rectangles use the parallelogram state, not polygon faces
    if (((sc_mode >> 11) & 0x1) && !(sc_mode & 0x1)) {
      scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      bias_displacement = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    if (((sc_mode >> 12) & 0x1) && !((sc_mode >> 1) & 0x1) && scale_bias == 0.0f &&
        bias_displacement == 0.0f) {
      scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
      bias_displacement = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
    }
  } else if ((sc_mode >> 13) & 0x1) {
    scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
    bias_displacement = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
  }
  // Constant scaled by the guest format's minimum value (2^24-1 for D24S8, 2^24 for D24FS8) and slope
  // from subpixels to pixels.
  float bias[2] = {
      bias_displacement * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                  ? float(uint32_t(1) << 24)
                                  : float((uint32_t(1) << 24) - 1)),
      scale_bias * (1.0f / 16.0f)};
  // sa2_native_shadows_pending_bias, only in the shadow map and only if the game does not set its own.
  if (target_shadows && type >= 4 && bias[0] == 0.0f && bias[1] == 0.0f && shadows_pending_bias_ != 0) {
    bias[1] = float(shadows_pending_bias_) * 0.1f;
  }
  const bool with_bias = bias[0] != 0.0f || bias[1] != 0.0f;
  if (with_bias && warnings_bias_ < 8 &&
      !Equal(bias, warned_bias_, sizeof(bias))) {
    ++warnings_bias_;
    std::memcpy(warned_bias_, bias, sizeof(bias));
    REXLOG_INFO("[native] C6: depth bias: scale {} offset {} "
                "(mode {:08X}) -> constant {} slope {}",
                scale_bias, bias_displacement, sc_mode, bias[0], bias[1]);
  }
  CutSubstage(16, t_indices_185);  // depth bias
  uint32_t vmin = UINT32_MAX;
  uint32_t vmax = 0;
  bool with_indices = false;
  bool indices_from_16 = false;  // in indices16_ instead of indices_
  // sa2_native_indices_cache (see the fast path below).
  bool indices_cache_hit = false, indices_cache_registrar = false;
  uint64_t indices_cache_offset = 0, indices_cache_frame_hit = 0, indices_cache_key = 0;
  uint64_t indices_cache_fingerprint = 0;
  const uint8_t* indices_cache_data = nullptr;
  bool indices_cache_rotate = false;
  indices_.clear();
  if (src_data == uint32_t(xenos::SourceSelect::kDMA)) {
    const uint32_t size = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
    const auto order = static_cast<xenos::Endian>(size >> 30);
    const uint32_t bytes = indices32 ? 4 : 2;
    const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
    if (count > (size & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(count) * bytes >
                                             kPhysicalMemory) {
      if (warnings_indices_ < 8) {
        ++warnings_indices_;
        REXLOG_WARN("[native] C6 diag: indices: count {} words {} VGT_DMA_SIZE {:08X} "
                    "VGT_DMA_BASE {:08X} 32-bit {} type {} VS n{} PS n{}",
                    count, size & 0xFFFFFF, size, r[gr::XE_GPU_REG_VGT_DMA_BASE], indices32,
                    type, p.vs->number, ps ? int(ps->number) : -1);
      }
      return Reject(7, "indices outside their buffer");
    }
    const uint8_t* data = memory_->TranslatePhysical(base & 0x1FFFFFFF);
    const uint32_t reset_index = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
    if (banjo_index_layouts_.insert(uint32_t(order) | (indices32 ? 4u : 0u) | (reset ? 8u : 0u)).second)
      REXLOG_INFO("[native] index layout seen: {} bit, endian {}, restart {}, type {}", indices32 ? 32 : 16,
                  uint32_t(order), reset, type);
    // ds2-nx (ds2_indices16_restart): with a restart index of 0xFFFF the 16-bit indices go up as they are too:
    // Vulkan's restart value for UINT16 is 0xFFFF, and the range below leaves those out.
    const bool restart16 = reset && reset_index == 0xFFFF && REXCVAR_GET(ds2_indices16_restart);
    if (!indices32 && (!reset || restart16) && !displacement && !quads && !fan &&
        (order == xenos::Endian::k8in16 || order == xenos::Endian::kNone)) {
      // The normal case in MASSEFFECT: 16 bits without restart or offset. They are uploaded as 16 bits, with a
      // branch-free loop the compiler can vectorize.
      indices16_.resize(count);
      uint16_t* output = indices16_.data();
      uint32_t min = 0xFFFF;
      uint32_t max = 0;
      if (indices_cache_active_ && type != 8) {
        const uint32_t sinc = g_synchronizations_ring.load(std::memory_order_relaxed);
        if (sinc != indices_cache_sinc_view_) {  // the guest may have rewritten a range it already drew
          indices_cache_sinc_view_ = sinc;
          ++indices_cache_generation_;
        }
        indices_cache_data = data;
        indices_cache_rotate = order == xenos::Endian::k8in16;
        indices_cache_key = (uint64_t(base & 0x1FFFFFFF) << 2) | (indices_cache_rotate ? 1u : 0u) |
                            (restart16 ? uint64_t(1) << 40 : 0u);  // ds2-nx: another range without 0xFFFF
        const uint64_t bytes_i = uint64_t(count) * 2;
        // ds2_indices_cache_fingerprint (with ds2_indices_cache_verify the full XXH3 is stored to check the hits).
        static const int32_t mode_fp = REXCVAR_GET(ds2_indices_cache_fingerprint);
        static const bool verify_fp = REXCVAR_GET(ds2_indices_cache_verify) && mode_fp < 2;
        const bool small_i = bytes_i <= 16384;
        indices_cache_fingerprint =
            !small_i ? 0
            : mode_fp == 2 || verify_fp ? std::max<uint64_t>(1, RangeFingerprint(data, size_t(bytes_i)))
            : mode_fp == 1 ? std::max<uint64_t>(1, SampleFingerprint(data, bytes_i, bytes_i))
                           : 0;
        IndicesEntryCache& e = indices_cache_[(indices_cache_key * 0x9E3779B97F4A7C15ull >> 40) &
                                                (indices_cache_.size() - 1)];
        bool same_content = e.fingerprint == indices_cache_fingerprint;
        if (verify_fp && small_i && e.frame == frame_ && e.generation == indices_cache_generation_ &&
            e.key == indices_cache_key && e.count == count && !same_content) {
          ++indices_cache_stale_;  // the sync epoch alone would have taken it
          if (indices_cache_stale_ <= 8)
            REXLOG_WARN("[native] index cache: STALE range at {:08X}, {} indices (ds2_indices_cache_verify)",
                        base & 0x1FFFFFFF, count);
        }
        if (e.frame == frame_ && e.generation == indices_cache_generation_ &&
            e.key == indices_cache_key && e.count == count && same_content) {
          min = e.vmin;
          max = e.vmax;
          indices_cache_hit = true;
          indices_cache_offset = e.offset;
          indices_cache_frame_hit = frame_;
          ++indices_cache_hits_;
          indices_cache_bytes_saved_ += bytes_i;
          if (indices_cache_hits_ % 50000 == 0) {
            REXLOG_INFO("[native] ME index cache: {} hits, {:.0f} MB neither converted nor copied, {} lost "
                        "due to buffer changes; stale (ds2_indices_cache_verify) {}",
                        indices_cache_hits_, double(indices_cache_bytes_saved_) / 1048576.0,
                        indices_cache_lost_, indices_cache_stale_);
          }
        } else {
          indices_cache_registrar = true;
        }
      }
      if (!indices_cache_hit) {
        // With hand-written NEON and its guard (IndicesFrom16, sa2_native_indices_neon).
        IndicesFrom16(data, count, output, order == xenos::Endian::k8in16, min, max, restart16);
      }
      vmin = min;
      vmax = max;
      indices_from_16 = true;
    } else {
      indices_.resize(count);
      uint32_t* output = indices_.data();
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t v;
        if (indices32) {
          uint32_t raw_bits;
          std::memcpy(&raw_bits, data + size_t(i) * 4, 4);
          v = xenos::GpuSwap(raw_bits, order) & 0xFFFFFF;
        } else {
          // banjo-nx: 16-bit indices swapped as 32-bit words (k8in32 / k16in32) also trade places in pairs,
          // as in the emulation; GpuSwap(uint16_t) leaves those modes unswapped, which joined the wrong
          // vertices (long stretched triangles across the screen).
          const bool in32 = order == xenos::Endian::k8in32 || order == xenos::Endian::k16in32;
          uint16_t raw_bits;
          std::memcpy(&raw_bits, data + size_t(in32 ? (i ^ 1u) : i) * 2, 2);
          v = order == xenos::Endian::k8in32 ? uint16_t((raw_bits >> 8) | (raw_bits << 8))
              : in32 ? raw_bits : xenos::GpuSwap(raw_bits, order);
        }
        if (reset && v == reset_index) {
          output[i] = UINT32_MAX;
          continue;
        }
        v = (v + displacement) & 0xFFFFFF;
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
        output[i] = v;
      }
    }
    if (vmin > vmax) {
      return true;  // resets only
    }
    with_indices = true;
  } else {
    vmin = displacement;
    vmax = displacement + count - 1;
    if (quads || fan) {
      indices_.resize(count);
      for (uint32_t i = 0; i < count; ++i) {
        indices_[i] = displacement + i;
      }
      with_indices = true;
    }
  }
  if (vmax - vmin >= kMaxVerticesPerDraw) {
    return Reject(8, "vertex range too large");
  }
  if (quads) {
    const size_t n = indices_.size() / 4;
    converted_.resize(n * 6);
    for (size_t q = 0; q < n; ++q) {
      const uint32_t* i = &indices_[q * 4];
      uint32_t* o = &converted_[q * 6];
      o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
      o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
    }
    indices_.swap(converted_);
  }
  if (fan) {
    me::native::ConvertFan(indices_, converted_);
    indices_.swap(converted_);
    if (indices_.empty()) return true;
  }
  CutSubstage(17, t_indices_185);  // indices
  // The indices stay as they come: vkCmdDrawIndexed subtracts vmin through vertexOffset.
  const uint32_t vertices = vmax - vmin + 1;

  // Bytes of each vertex binding in guest memory.
  struct Source {
    const uint8_t* data;
    xenos::Endian order;
    uint32_t bytes;
    uint64_t address;  // physical, of the first byte used
  };
  std::array<Source, 16> sources{};
  if (entry->bindings.size() > sources.size()) {
    return Reject(9, "too many vertex streams");
  }
  VkDeviceSize bytes_vertices = 0;
  for (size_t b = 0; b < entry->bindings.size(); ++b) {
    const BindingVertices& binding = entry->bindings[b];
    const uint32_t d0 = r[kRegFetch + binding.slot * 2];
    const uint32_t d1 = r[kRegFetch + binding.slot * 2 + 1];
    if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
      return Reject(10, "invalid vertex fetch constant");
    }
    const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
    const uint64_t available = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
    const uint64_t begin = uint64_t(vmin) * binding.stride;
    uint64_t required = uint64_t(vertices) * binding.stride;
    if (begin + required > available) {
      Warn(11, "vertices beyond the end of their buffer: clipped");
      required = available > begin ? (available - begin) / binding.stride * binding.stride : 0;
    }
    if (!required || address + begin + required > kPhysicalMemory) {
      if (banjo_vertex_range_logs_ < 12) {  // banjo-nx diagnostic
        ++banjo_vertex_range_logs_;
        REXLOG_INFO("[native] vertices outside their buffer: VS n{} binding {} slot {} stride {} address {:08X} "
                    "available {} vmin {} vmax {} indexed {} count {} type {} offset {}", p.vs->number, b,
                    binding.slot, binding.stride, address, available, vmin, vmax, with_indices, count, type,
                    displacement);
      }
      return Reject(12, "vertices outside memory");
    }
    sources[b] = {memory_->TranslatePhysical(uint32_t(address + begin)),
                   static_cast<xenos::Endian>(d1 & 0x3), uint32_t(required),
                   address + begin};
    bytes_vertices += (required + 3) & ~VkDeviceSize(3);
  }

  // ds2scholar-nx (2026-10-23): members, not locals. As locals every rectangle-list draw (type 8) allocated and freed
  // its expanded streams and a copy of the entry's vectors; console stack profile (pgo2 stacks): ~2 % of the ring
  // thread's running samples in operator delete from here. resize and copy-assignment keep the capacity.
  VerticesEntry& rectangle_entry = rectangle_entry_;
  std::array<std::vector<uint8_t>, 16>& data_rectangle = data_rectangle_;
  static const bool audit_depth_rectangle = [] {
    const char* value = std::getenv("SA2_NATIVE_AUDIT_DEPTH_RECTANGLE");
    return value && *value == '1';
  }();
  if (audit_depth_rectangle && me_constant_audit_hdr_ && type == 8 &&
      !masks && (control_depth & 0x76) == 0x76 &&
      me_depth_rectangle_audits_ < 16) {
    ++me_depth_rectangle_audits_;
    REXLOG_INFO("[native] depth rectangle audit {} VS n{} PS n{} pitch {} MSAA {} depthinfo {:08X} "
                "stencil {:08X} clip {:08X} VTE {:08X} viewportZ {:08X}/{:08X}",
                me_depth_rectangle_audits_, p.vs->number, ps ? int(ps->number) : -1,
                pitch, (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3,
                r[gr::XE_GPU_REG_RB_DEPTH_INFO], r[gr::XE_GPU_REG_RB_STENCILREFMASK],
                r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], r[gr::XE_GPU_REG_PA_CL_VTE_CNTL],
                r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE], r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]);
    for (const AttributeVertices& a : entry->attributes) {
      const Source& origin = sources[a.binding];
      const uint32_t stride = entry->bindings[a.binding].stride;
      for (uint32_t v = 0; v < std::min(vertices, 3u); ++v) {
        const uint64_t start = uint64_t(v) * stride + a.offset;
        uint32_t words[4]{};
        if (start < origin.bytes) {
          std::memcpy(words, origin.data + start,
                      std::min<uint64_t>(sizeof(words), origin.bytes - start));
          for (auto& word : words) word = xenos::GpuSwap(word, origin.order);
        }
        REXLOG_INFO("[native] depth rectangle attr {} format {} stride {} offset {} v{} "
                    "words {:08X} {:08X} {:08X} {:08X} asfloat {} {} {} {}",
                    a.location, uint32_t(a.format), stride, a.offset, v,
                    words[0], words[1], words[2], words[3], Float(words[0]),
                    Float(words[1]), Float(words[2]), Float(words[3]));
      }
    }
  }
  if (type == 8) {
    auto [it, inserted] = shaders_rectangle_.try_emplace(p.vs);
    if (inserted) it->second = me::native::ExpandRectangleShader(p.vs->shader->Spirv());
    const auto& shader = it->second;
    if (shader.words.empty()) return Reject(108, "rectangle VS interface unsupported");
    rectangle_entry = *entry;
    rectangle_entry.attributes.clear();
    for (const auto& mapping : shader.inputs) {
      const auto attr = std::find_if(entry->attributes.begin(), entry->attributes.end(),
          [&](const AttributeVertices& a) { return a.location == mapping[0]; });
      if (attr == entry->attributes.end()) return Reject(108, "rectangle missing VS input");
      for (uint32_t c = 0; c < 3; ++c) {
        auto a = *attr; a.location = mapping[c+1];
        a.offset += c * entry->bindings[a.binding].stride;
        rectangle_entry.attributes.push_back(a);
      }
    }
    const uint32_t rectangles = count / 3;
    if (!rectangles) {
      return true;
    }
    bytes_vertices = 0;
    for (size_t b = 0; b < entry->bindings.size(); ++b) {
      const uint32_t stride = entry->bindings[b].stride;
      const uint64_t bytes = uint64_t(rectangles) * 6 * 3 * stride;
      if (bytes > kUploadSize || stride > 2048 / 3)
        return Reject(108, "rectangle expanded stream exceeds limits");
      auto& data = data_rectangle[b]; data.resize(size_t(bytes));
      for (uint32_t q = 0; q < rectangles; ++q) {
        for (uint32_t c = 0; c < 3; ++c) {
          const uint32_t guest_index = with_indices
              ? (indices_from_16 ? indices16_[q*3+c] : indices_[q*3+c]) : displacement + q*3+c;
          const uint64_t offset = uint64_t(guest_index - vmin) * stride;
          if (guest_index < vmin || offset + stride > sources[b].bytes)
            return Reject(108, "rectangle corner outside stream");
          for (uint32_t v = 0; v < 6; ++v)
            std::memcpy(data.data() + (size_t(q)*18 + v*3 + c)*stride,
                        sources[b].data + offset, stride);
        }
      }
      sources[b].data = data.data(); sources[b].bytes = uint32_t(bytes);
      rectangle_entry.bindings[b].stride = stride * 3;
      bytes_vertices += bytes;
    }
    rectangle_entry.fingerprint = XXH3_64bits(rectangle_entry.attributes.data(),
        rectangle_entry.attributes.size()*sizeof(AttributeVertices));
    rectangle_entry.layout_built = false;  // deko3d: its attributes and strides changed in place
    entry = &rectangle_entry;
    count = rectangles * 6; with_indices = false; indices_from_16 = false;
    indices_.clear(); indices16_.clear(); vmin = 0; vmax = count - 1;
    if (recorded_rectangles_.insert(p.vs->number).second)
      REXLOG_INFO("[native] rectangle post-VS expansion: VS n{}, {} attributes, {} rectangles",
                  p.vs->number, shader.inputs.size(), rectangles);
    if (rectangles_recorded_constants_.insert(p.vs->number).second) {
      for (uint32_t reg = 252; reg < 256; ++reg)
        REXLOG_INFO("[native] rectangle VS n{} guest c{} = {:08X} {:08X} {:08X} {:08X}",
                    p.vs->number, reg, r[kRegConstantsVs+reg*4], r[kRegConstantsVs+reg*4+1],
                    r[kRegConstantsVs+reg*4+2], r[kRegConstantsVs+reg*4+3]);
    }
  }
  CutSubstage(18, t_indices_185);  // vertices and diagnostics
  Stage(1, mark);
  // --- Textures and samplers ----------------------------------------------------
  me_resolved_fetch_failed_this_draw_ = false;
  uint32_t shared[kSharedWords] = {};
  uint32_t draw_signs = 0;  // sa2_gpu_no_texture_signs: packed signs of every texture of the draw, ORed
  VkDeviceSize bytes_textures = 0;
  textures_to_upload_.clear();
  const auto t_samplers =
      time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  for (const SamplerShader& sampler :
       ps ? std::span<const SamplerShader>(ps->samplers) : std::span<const SamplerShader>()) {
    if (sampler.register_value >= 16) {
      continue;
    }
    const uint32_t* fetch = r + kRegFetch + uint32_t(sampler.register_value) * 6;
    ++samplers_prepared_;
    // Per-register cache: same fetch constant, same frame and no render target copies in between.
    CacheSampler& cache = cache_samplers_[sampler.register_value];
    // Valid until cache.valid_until (with the cross-frame cache disabled, only its own frame)
    if ((cache_between_frames_ ? frame_ <= cache.valid_until : cache.frame == frame_) &&
        cache.generation == generation_textures_ &&
        Equal(cache.fetch.data(), fetch, sizeof(cache.fetch))) {
      shared[cache.heap * 16 + sampler.register_value] = cache.slot;
      draw_signs |= cache.slot >> 24;
      shared[48 + sampler.register_value] = cache.sampler;
      WriteInvSize(shared, sampler.register_value, cache.width, cache.height);
      ++samplers_cache_;
      continue;
    }
    uint32_t slot_texture = 0;
    uint32_t heap = 0;
    uint32_t slot_sampler = 0;
    uint32_t host_width = 0, host_height = 0;
    // Second cache, keyed by the whole fetch constant within the same frame and generation: a register
    // changes texture between draws, but textures repeat a lot within a frame, and PrepareTexture already
    // queued their upload the first time.
    CacheSampler& per_fetch =
        cache_fetch_[XXH3_64bits(fetch, sizeof(uint32_t) * 6) & (cache_fetch_.size() - 1)];
    uint64_t valid_until = frame_;
    if ((cache_between_frames_ ? frame_ <= per_fetch.valid_until : per_fetch.frame == frame_) &&
        per_fetch.generation == generation_textures_ &&
        Equal(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch))) {
      slot_texture = per_fetch.slot;
      heap = per_fetch.heap;
      slot_sampler = per_fetch.sampler;
      valid_until = per_fetch.valid_until;
      host_width = per_fetch.width;
      host_height = per_fetch.height;
      ++samplers_cache_fetch_;
    } else {
      // Why the table misses ("C6 cache per fetch" report, every 10 s).
      if (!Equal(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch))) {
        ++(per_fetch.frame == UINT64_MAX ? fetch_empty_failures_ : fetch_failures_clash_);
      } else if (per_fetch.generation != generation_textures_) {
        ++fetch_failures_generation_;
      } else {
        ++fetch_expired_failures_;
      }
      bool punctual_sampling = false;
      me_resolve_fetch_sampler_ = sampler.register_value;
      PrepareTexture(fetch, slot_texture, heap, bytes_textures, punctual_sampling, valid_until,
                      host_width, host_height);
      if (me_resolved_fetch_failed_this_draw_) {
        return false;  // No cache entry and no stale-RAM / empty-texture fallback.
      }
      // deko3d: the three heaps share one image descriptor set (sa2_dk.h); what the shader receives is the
      // index in that set. Converted here, before the caches store it, so every copy of the word is the same.
      slot_texture = DescriptorOfSlot(heap, slot_texture);
      slot_texture |= uint32_t(RemappedSigns(fetch)) << 24;
      if (REXCVAR_GET(ds2_texture_exp_adjust)) {
        // ds2-nx: exp_adjust, dword 3 bits 13-18 (xenos::xe_gpu_texture_fetch_t); bit 22 marks it as present.
        const uint32_t exp_adjust = (fetch[3] >> 13) & 0x3F;
        if (exp_adjust) slot_texture |= (exp_adjust | 0x40u) << 16;
      }
      slot_sampler = SlotSampler(fetch, punctual_sampling);
      per_fetch.frame = frame_;
      per_fetch.generation = generation_textures_;
      std::memcpy(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch));
      per_fetch.slot = slot_texture;
      per_fetch.heap = heap;
      per_fetch.sampler = slot_sampler;
      per_fetch.valid_until = valid_until;
      per_fetch.width = host_width;
      per_fetch.height = host_height;
    }
    shared[heap * 16 + sampler.register_value] = slot_texture;
    draw_signs |= slot_texture >> 24;
    shared[48 + sampler.register_value] = slot_sampler;
    WriteInvSize(shared, sampler.register_value, host_width, host_height);
    cache.frame = frame_;
    cache.generation = generation_textures_;
    std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
    cache.slot = slot_texture;
    cache.heap = heap;
    cache.sampler = slot_sampler;
    cache.valid_until = valid_until;
    cache.width = host_width;
    cache.height = host_height;
  }
  if (time_) {  // C6 substages: the sampler loop inside the textures stage
    sub_ns_[14] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t_samplers)
                                .count());
    ++sub_n_[14];
  }
  Stage(2, mark);
  // Upload commands execute BEFORE all work commands in the submission. Updating
  // an existing image here would retroactively change earlier draws sampling it:
  // a transfer->transfer barrier cannot preserve that older texture version.
  // Split before acquiring the render pass / command buffer or allocating this
  // draw's constants. New images can still be uploaded in the leading buffer.
  if (used_upload_ && REXCVAR_GET(sa2_native_sort_updates_texture)) {
    for (const Texture* texture : textures_to_upload_) {
      if (!texture->needs_upload || !texture->image.prepared) continue;
      if (++me_ordered_updates_ <= 16 || (me_ordered_updates_ & 255) == 0) {
        REXLOG_INFO("[native] ME texture update ordering: split {} before replacing {:08X} "
                    "{}x{} (command generation {})",
                    me_ordered_updates_, texture->address, texture->image.width,
                    texture->image.height, context_->GenerationCommands());
      }
      if (!context_->SendAndWait()) return false;
      break;
    }
  }
  // banjo-nx: the buffers of the vertex fetches indexed by a computed register (bone palettes; ClassifyVertexFetches
  // in sa2_native_shaders.cpp). Each is copied whole, in host word order like the vertex streams, and the
  // shader reads it through its device address (banjoVfetch in shader_common.h).
  struct IndexedSource {
    const uint8_t* data = nullptr;
    xenos::Endian order = xenos::Endian::kNone;
    uint32_t bytes = 0;
    uint64_t address = 0;
  };
  std::array<IndexedSource, 2> indexed_sources{};
  VkDeviceSize bytes_indexed = 0;
  static const bool skip_indexed = REXCVAR_GET(banjo_native_skip_indexed);
  if (skip_indexed && !p.vs->indexed_constants.empty()) return true;  // diagnostic: draw nothing
  static const std::unordered_set<uint32_t> skip_vs = [] {  // diagnostic: banjo_native_skip_vs = "n,n,..."
    std::unordered_set<uint32_t> numbers;
    const std::string list = REXCVAR_GET(banjo_native_skip_vs);
    size_t start = 0;
    while (start < list.size()) {
      const size_t comma = list.find(',', start);
      const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!item.empty()) numbers.insert(uint32_t(std::strtoul(item.c_str(), nullptr, 10)));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    return numbers;
  }();
  if (!skip_vs.empty() && skip_vs.count(p.vs->number)) return true;
  for (size_t k = 0; k < p.vs->indexed_constants.size() && k < indexed_sources.size(); ++k) {
    const uint32_t slot = p.vs->indexed_constants[k];
    const uint32_t d0 = r[kRegFetch + slot * 2];
    const uint32_t d1 = r[kRegFetch + slot * 2 + 1];
    if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) continue;
    const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
    const uint64_t bytes = std::min<uint64_t>(uint64_t((d1 >> 2) & 0xFFFFFF) * 4, kIndexedFetchMaxBytes);
    if (!bytes || address + bytes > kPhysicalMemory) continue;
    indexed_sources[k] = {memory_->TranslatePhysical(uint32_t(address)), static_cast<xenos::Endian>(d1 & 0x3),
                          uint32_t(bytes), address};
    bytes_indexed += bytes + 16;
  }
  // --- Upload buffer space ----------------------------------------------------------
  const VkDeviceSize bytes_indices =
      indices_from_16 ? VkDeviceSize(indices16_.size()) * 2 : VkDeviceSize(indices_.size()) * 4;
  // With a single binding the copy goes to a multiple of the stride: up to stride - 4 bytes of padding.
  const VkDeviceSize gap_base_zero =
      vertices_base_zero_ && entry->bindings.size() == 1 ? VkDeviceSize(entry->bindings[0].stride) : 0;
  const VkDeviceSize required = gap_base_zero + bytes_vertices + bytes_indices + bytes_textures + bytes_indexed +
                                  2 * VkDeviceSize(kRegistersConstants) * 4 +
                                  kSharedWords * 4 + 64 * 8 +
                                  // With UBOs the blocks go whole and aligned to alignment_ubo_
                                  (use_ubo_ ? kUboBytesVs + kUboBytesPs + kUboBytesShared +
                                                   3 * alignment_ubo_
                                             : 0);
  if (required > kUploadSize) {
    return Reject(13, "draw larger than the upload buffer");
  }
  const bool shared_full =
      shared_separate_ && use_ubo_ &&
      shared_used_ + kUboBytesShared + alignment_ubo_ > kSharedSize;
  if (used_upload_ + required > kUploadSize || shared_full) {
    const auto before_send = std::chrono::steady_clock::now();
    const bool sent = context_->SendAndWait();
    ++full_sends_;
    ns_full_sends_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - before_send)
                                      .count());
    if (!sent) {
      return false;
    }
  }

  // --- Render pass ----------------------------------------------------------------
  /*
   * No XXH3 on every draw (sa2_native_key_fast_pass). If the 40 bytes of render targets are the
   * same ones that produced pass_key_ (BeginPass stores them next to it), their XXH3 is pass_key_.
   * Otherwise it is computed as usual: the pass change decision is the same as before in every case,
   * collisions included.
   */
  uint64_t pass_key;
  if (key_fast_pass_ && pass_valid_keys_ && Equal(keys, pass_keys_, sizeof(keys))) {
    const uint64_t n = ++keys_fast_pass_;
    pass_key = (n <= kKeysPassToCheck || (n & 4095) == 0) ? CheckPassKey(keys, n) : pass_key_;
  } else {
    pass_key = XXH3_64bits(keys, sizeof(keys));
  }
  if (!active_pass_ || pass_key != pass_key_ ||
      pass_generation_ != context_->GenerationCommands()) {
    // ds2_native_partial_send: only where a pass closes anyway; SendPartial closes it and bumps the generation.
    if (partial_send_ < 0) {
      partial_send_ = REXCVAR_GET(ds2_native_partial_send);
    }
    if (partial_send_ > 0 && active_pass_ && drawn_ - drawn_at_send_ >= uint64_t(partial_send_) &&
        context_->SendPartial()) {
      drawn_at_send_ = drawn_;
    }
    const auto before_pass = std::chrono::steady_clock::now();
    if (pass_generation_ != context_->GenerationCommands()) {
      ++passes_per_generation_;
    } else if (!active_pass_ && pass_key == pass_key_) {
      ++resumed_passes_;
    } else {
      ++passes_per_target_;
    }
    FinishPass();
    // The pass change is split. FinishPass closes the previous pass; BeginPass finds or creates the
    // render targets (it may create images, record barriers and clear them), builds the render pass and
    // the framebuffer, and opens the pass.
    const auto after_finish = std::chrono::steady_clock::now();
    if (time_) {
      stages_ns_[8] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          after_finish - before_pass).count());
    }
    const bool started = BeginPass(r, keys, pitch, pass_key);
    const auto after_pass = std::chrono::steady_clock::now();
    ns_passes_ += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
    // The pass change, separate from what the stage costs on each draw.
    if (time_) {
      stages_ns_[7] += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
    }
    if (!started) {
      return false;
    }
  }
  const DkCmdBuf cmd = pass_commands_;
  if (recording_generation_ != context_->GenerationCommands()) {
    recording_generation_ = context_->GenerationCommands();
    pipeline_bound_ = nullptr;
    // deko3d: nothing is bound in the new command list (the EDS phases' eds_valid_ is gone: no fixed state).
    state_tracker_.Reset();
    layout_bound_valid_ = false;
    bound_valid_key_ = false;  // New command buffer (CountChangePipeline)
    sets_bound_ = false;
    ubo_bound_ = false;
    recorded_state_ = false;
    recorded_bindings_ = 0;  // different command buffer: nothing is bound
  }

  Stage(3, mark);
  // --- Uploads: textures, vertices, indices and constants ------------------------------
  for (Texture* texture : textures_to_upload_) {
    if (!UploadTexture(*texture)) {
      return false;
    }
  }
  std::array<VkDeviceSize, 16> offsets_vertices{};
  // per binding: the upload buffer or the vertex cache's (deko3d: their GPU base addresses, no buffer objects)
  std::array<DkGpuAddr, 16> buffers_draw{};
  bool vertices_from_cache = false;
  // If the guest has waited for the GPU since the last draw, it may legally have rewritten an already
  // referenced range: what was recorded is no longer valid.
  if (dedupe_active_) {
    const uint32_t sinc = g_synchronizations_ring.load(std::memory_order_relaxed);
    if (sinc != dedupe_sinc_view_) {
      dedupe_sinc_view_ = sinc;
      dedupe_.Forget();
    }
  }
  const auto before_vertices =
      time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  for (size_t b = 0; b < entry->bindings.size(); ++b) {
    const Source& source = sources[b];
    if (diag_vertices_repeated_) {
      NoteVerticesRepeated(source.address, source.bytes, uint32_t(source.order), source.data);
    }
    buffers_draw[b] = upload_address_;
    uint64_t known_fingerprint = 0;  // the vertex cache's, reused by the in-frame dedupe on a miss
    // ds2_vertex_cache_mb: a range with the same content as when it was cached is bound from the cache.
    if (vcache_active_ && type != 8 && source.bytes >= 64 && source.bytes <= vcache_.chunk_size() / 4 &&
        !(ps && ps->shader && ps->shader->fingerprint == kSkyFingerprint) &&
        std::none_of(entry->repacks.begin(), entry->repacks.end(),
                     [b](const RepackVertices& rp) { return rp.binding == b; })) {
      const uint32_t sync = g_synchronizations_ring.load(std::memory_order_relaxed);
      uint64_t at = 0;
      // Already validated in this work slot and synchronization epoch: no fingerprint (sa2_native_vertex_cache.h).
      if (!vcache_verify_ && vcache_.Fast(source.address, uint32_t(source.order), source.bytes, epoch_upload_, sync,
                                          at)) {
        offsets_vertices[b] = at;
        buffers_draw[b] = vcache_buf_.gpu;
        vertices_from_cache = true;
        continue;
      }
      const uint64_t fingerprint = std::max<uint64_t>(
          1, vcache_hash_ ? XXH3_64bits(source.data, source.bytes)
                          : SampleFingerprint(source.data, source.bytes, source.bytes));
      const uint32_t stride = entry->bindings.size() == 1 ? entry->bindings[0].stride : 0;
      const VertexCache::Result result = vcache_.Lookup(source.address, uint32_t(source.order), source.bytes,
                                                        fingerprint, stride ? stride : 4, epoch_upload_, sync, at);
      // A miss goes on to the in-frame dedupe, which takes the same fingerprint when it is the same kind.
      if (result == VertexCache::Result::kMiss && (vcache_hash_ == 0) == REXCVAR_GET(sa2_native_dedupe_sample))
        known_fingerprint = fingerprint;
      bool use = result == VertexCache::Result::kCopy;
      if (result == VertexCache::Result::kHit) {
        use = !vcache_verify_ ||
              vcache_.CheckFull(source.address, uint32_t(source.order), source.bytes,
                                XXH3_64bits(source.data, source.bytes));
        if (!use) {
          vcache_.Drop(source.address, uint32_t(source.order), source.bytes);
          if (vcache_.figures().stale <= 8)
            REXLOG_WARN("[native] vertex cache: STALE hit at {:08X}, {} bytes (ds2_vertex_cache_verify): copied "
                        "again", source.address, source.bytes);
        }
      }
      if (result == VertexCache::Result::kCopy) {
        CopyVertices(WorkCopy{source.data, vcache_buf_.cpu + at, source.bytes / 4, source.order});
        if (!coherent_upload_) sa2::dk::FlushCpu(vcache_buf_, uint32_t(at), source.bytes);
        if (vcache_verify_)
          vcache_.NoteFull(source.address, uint32_t(source.order), source.bytes,
                           XXH3_64bits(source.data, source.bytes));
      }
      if (use) {
        offsets_vertices[b] = at;
        buffers_draw[b] = vcache_buf_.gpu;
        vertices_from_cache = true;
        continue;
      }
    }
    VkDeviceSize offset;
    // Mass Effect's native PM4 frontend does not publish the MASSEFFECT wait
    // generation. Address equality therefore cannot prove that a transient
    // vertex range still contains the copy recorded earlier in this slot.
    // Verify content before reusing it, and expose rejected stale hits.
    // Mass Effect on Switch: the full XXH3 of every range was ~18 % of the ring thread. With
    // sa2_native_dedupe_sample the check hashes every 8th block (SampleFingerprint) instead.
    static const uint32_t max_fingerprint = uint32_t(REXCVAR_GET(sa2_native_dedupe_max_fingerprint));
    const bool fingerprinted = dedupe_active_ && type != 8 && (!max_fingerprint || source.bytes <= max_fingerprint);
    const uint64_t vertices_fingerprint = fingerprinted
        ? known_fingerprint ? known_fingerprint
        : std::max<uint64_t>(1, REXCVAR_GET(sa2_native_dedupe_sample)
                                    ? SampleFingerprint(source.data, source.bytes, source.bytes)
                                    : XXH3_64bits(source.data, source.bytes)) : 0;
    if (fingerprinted) {
      ++dedupe_fingerprints_;
      dedupe_bytes_fingerprinted_ +=
          REXCVAR_GET(sa2_native_dedupe_sample) ? BytesSample(source.bytes) : source.bytes;
    }
    const uint64_t discrepancies_before = dedupe_.discrepancies();
    // If this same range was already copied in this frame, its place in the upload buffer is reused and
    // nothing is copied. See sa2_native_vertices_dedupe.h.
    const bool repacked = std::any_of(entry->repacks.begin(), entry->repacks.end(),
                                      [b](const RepackVertices& rp) { return rp.binding == b; });
    if (dedupe_active_ && type != 8 && !repacked &&
        dedupe_.Search(source.address, source.bytes, uint32_t(source.order), offset,
                       vertices_fingerprint)) {
      offsets_vertices[b] = offset;
      continue;
    }
    if (dedupe_.discrepancies() != discrepancies_before && dedupe_.discrepancies() <= 8) {
      REXLOG_WARN("[native] stale vertex reuse prevented: address {:08X}, {} bytes, "
                  "frame {}, discrepancy {}", source.address, source.bytes,
                  frame_, dedupe_.discrepancies());
    }
    // With a single binding, at a multiple of its stride (sa2_native_vertices_base_zero).
    if (vertices_base_zero_ && entry->bindings.size() == 1) {
      ReserveMultiple(source.bytes, entry->bindings[0].stride, offset);
    } else {
      Reserve(source.bytes, 4, offset);
    }
    const WorkCopy work{source.data, upload_data_ + offset, source.bytes / 4, source.order};
    const uint64_t audit_bytes = uint64_t(work.words) * 4;
    const bool audit_vertex = (type == 8 || !active_copies_) &&
        source.address + audit_bytes <= kPhysicalMemory && StartPayloadAudit(1, audit_bytes);
    const uint64_t audit_before = audit_vertex ? XXH3_64bits(source.data, size_t(audit_bytes)) : 0;
    if (repacked || type == 8 || !active_copies_ || !EnqueueCopy(work)) {
      CopyVertices(work);
    }
    if (repacked) {
      const uint32_t stride = entry->bindings[b].stride;
      for (const RepackVertices& rp : entry->repacks) {
        if (rp.binding != b || !stride) continue;
        for (uint64_t at = rp.offset; at + 4 <= source.bytes; at += stride)
          RepackWord1011(work.target + at, rp.format, rp.with_sign);
      }
    }
    if (audit_vertex) {
      // GpuSwap is involutive. Compare the complete staged copy in guest byte
      // order, without modifying the upload allocation or the actual draw.
      std::vector<uint8_t> raw(size_t(audit_bytes), 0);
      for (uint32_t i = 0; i < work.words; ++i) {
        uint32_t word;
        std::memcpy(&word, work.target + size_t(i) * 4, 4);
        word = xenos::GpuSwap(word, work.order);
        std::memcpy(raw.data() + size_t(i) * 4, &word, 4);
      }
      FinalizePayloadAudit(1, source.address, audit_bytes, audit_before,
                            XXH3_64bits(source.data, size_t(audit_bytes)),
                            XXH3_64bits(raw.data(), raw.size()));
    }
    bytes_vertices_ += source.bytes;
    offsets_vertices[b] = offset;
    if (dedupe_active_ && type != 8 && !repacked) {
      dedupe_.Note(source.address, source.bytes, uint32_t(source.order), offset,
                     vertices_fingerprint);
    }
  }
  if (time_) {
    ns_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - before_vertices)
                                 .count());
  }
  VkDeviceSize offset_indices = 0;
  if (with_indices && indices_cache_hit && indices_cache_frame_hit == frame_) {
    offset_indices = indices_cache_offset;  // already uploaded by an earlier draw of this upload buffer
  } else if (with_indices) {
    if (indices_cache_hit) {
      // The upload buffer changed since the lookup (a Reserve forced a submission): convert after all.
      uint32_t mn = 0xFFFF, mx = 0;
      IndicesFrom16(indices_cache_data, uint32_t(indices16_.size()), indices16_.data(), indices_cache_rotate, mn, mx);
      ++indices_cache_lost_;
    }
    Reserve(bytes_indices, 4, offset_indices);
    bytes_indices_uploaded_ += bytes_indices;
    std::memcpy(upload_data_ + offset_indices,
                indices_from_16 ? static_cast<const void*>(indices16_.data())
                              : static_cast<const void*>(indices_.data()),
                size_t(bytes_indices));
    if (indices_cache_registrar || indices_cache_hit) {
      IndicesEntryCache& e = indices_cache_[(indices_cache_key * 0x9E3779B97F4A7C15ull >> 40) &
                                              (indices_cache_.size() - 1)];
      e.key = indices_cache_key;
      e.count = uint32_t(indices16_.size());
      e.vmin = vmin;
      e.vmax = vmax;
      e.offset = offset_indices;
      e.fingerprint = indices_cache_fingerprint;
      e.frame = frame_;
      e.generation = indices_cache_generation_;
    }
  }
  // Only the registers each shader reads (ShaderEntry::constants_bytes). With the same generation, the
  // copy is reused if it already covers what this shader needs.
  // Start the bounded audit at the first HDR target so menu cache hits cannot
  // exhaust its coverage before the scene. This only compares host memory.
  for (uint32_t info : {gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
                        gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO}) {
    const uint32_t format = (r[info] >> 16) & 15;
    if (format == 3 || format == 7 || format == 12) me_constant_audit_hdr_ = true;
  }
  const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constants_bytes, kRegistersConstants * 4);
  // sa2-nx (sa2_native_constants_dedupe): a new constant generation whose bytes (the part this shader reads) equal
  // the block uploaded last in this upload epoch keeps that block: no new reservation, no new set-4 dynamic offset
  // (a constant-buffer rebind at the next draw on NVK). The comparison uses a CPU-side copy of what was uploaded,
  // never the mapped upload memory. ~77 % of SA2's draws re-uploaded their VS constants.
  static const bool dedupe = REXCVAR_GET(sa2_native_constants_dedupe);
  // sa2_cpu_ring_compare_uploaded: the last uploaded block (CPU-cached) instead of a second copy of it.
  const bool from_upload = dedupe && cached_upload_ && g_dk_compare_uploaded.active && !compare_uploaded_failed_;
  const bool keep_copies = dedupe && (!from_upload || sa2::enh::VerifyMode());
  if (dedupe && constants_vs_generation_ != p.generation_constants_vs &&
      constants_vs_epoch_ == epoch_upload_ && bytes_vs <= constants_vs_bytes_ &&
      SameAsUploaded(from_upload ? upload_data_ + constants_vs_offset_ : shadow_constants_vs_.data(),
                     shadow_constants_vs_.data(), r + kRegConstantsVs, bytes_vs, from_upload)) {
    constants_vs_generation_ = p.generation_constants_vs;
    ++deduped_constants_[0];
  } else if (constants_vs_generation_ != p.generation_constants_vs ||
      constants_vs_epoch_ != epoch_upload_ || bytes_vs > constants_vs_bytes_) {
    // Why they are uploaded again (measurement only; each upload is another set 4 offset).
    ++reuploaded_vs_[constants_vs_generation_ != p.generation_constants_vs ? 0
                    : constants_vs_epoch_ != epoch_upload_                 ? 1
                                                                            : 2];
    // With UBOs the block goes whole (the driver may read the full range) and aligned to the device minimum
    Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_vs, kUboBytesVs) : bytes_vs, use_ubo_ ? alignment_ubo_ : 16,
             constants_vs_offset_);
    if (diag_vs_constants_) {  // ds2_diag_vs_constants: bytes copied and how many float4 registers changed
      ++diag_vs_[0];
      diag_vs_[1] += bytes_vs;
      const uint8_t* now = reinterpret_cast<const uint8_t*>(r + kRegConstantsVs);
      for (uint32_t at = 0; at < bytes_vs; at += 16)
        if (std::memcmp(now + at, diag_vs_last_.data() + at, 16) != 0) ++diag_vs_[2];
      std::memcpy(diag_vs_last_.data(), now, bytes_vs);
    }
    std::memcpy(upload_data_ + constants_vs_offset_, r + kRegConstantsVs, bytes_vs);
    if (keep_copies) std::memcpy(shadow_constants_vs_.data(), r + kRegConstantsVs, bytes_vs);
    constants_vs_generation_ = p.generation_constants_vs;
    constants_vs_epoch_ = epoch_upload_;
    constants_vs_bytes_ = bytes_vs;
  } else {
    AuditCacheConstants(true, p.vs->number, p.generation_constants_vs,
                           r + kRegConstantsVs, constants_vs_offset_, bytes_vs);
  }
  const uint32_t bytes_ps =
      ps ? std::min<uint32_t>(ps->constants_bytes, kRegistersConstants * 4) : 0;
  {
    // ds2-nx diagnostic (ds2_diag_ps_constants = "<PS number>:<c>,<c>,..."): the float constants a pixel shader
    // reads from the live bank, for its first 4 draws.
    static const std::string diag = REXCVAR_GET(ds2_diag_ps_constants);
    static const uint32_t diag_ps = diag.empty() ? UINT32_MAX : uint32_t(std::strtoul(diag.c_str(), nullptr, 10));
    if (ps && ps->number == diag_ps && logged_diag_constants_ < 4) {
      ++logged_diag_constants_;
      std::string text;
      for (size_t at = diag.find(':'); at != std::string::npos; at = diag.find(',', at + 1)) {
        const uint32_t c = uint32_t(std::strtoul(diag.c_str() + at + 1, nullptr, 10)) & 0xFF;
        text += fmt::format(" c{}=({}, {}, {}, {})", c, Float(r[kRegConstantsPs + c * 4]),
                            Float(r[kRegConstantsPs + c * 4 + 1]), Float(r[kRegConstantsPs + c * 4 + 2]),
                            Float(r[kRegConstantsPs + c * 4 + 3]));
      }
      std::string fetches;
      for (const SamplerShader& sampler : ps->samplers) {
        const uint32_t* f = r + kRegFetch + uint32_t(sampler.register_value) * 6;
        fetches += fmt::format(" s{}[fmt {} {}x{} dim {} signs {:02X} swz {:03X} exp {} base {:08X}]",
                               sampler.register_value, f[1] & 0x3F, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1,
                               (f[5] >> 9) & 3, (f[0] >> 2) & 0xFF, (f[3] >> 1) & 0xFFF, int32_t(f[3] << 13) >> 26,
                               (f[1] >> 12) << 12);
      }
      REXLOG_INFO("[native] ds2_diag_ps_constants PS n{} ({} bytes uploaded, samplers{}):{}", ps->number, bytes_ps,
                  fetches, text);
    }
  }
  uint8_t constants_mode_ps = 0;
  uint32_t tonemap_scale_bits = 0;
  float tonemap_scale = 0.0f;
  if (ps && ps->shader->fingerprint == kMeFingerprintTonemap && bytes_ps >= 4) {
    if (REXCVAR_GET(sa2_native_fix_tonemap_7e3)) {
      constants_mode_ps = 1;
      tonemap_scale = static_cast<float>(
          std::clamp(REXCVAR_GET(sa2_native_tonemap_7e3_scale), 0.0, 8.0));
      std::memcpy(&tonemap_scale_bits, &tonemap_scale, sizeof(tonemap_scale_bits));
    }
  }
  if (dedupe && ps && !constants_mode_ps && !constants_ps_mode_ &&
      constants_ps_generation_ != p.generation_constants_ps && constants_ps_epoch_ == epoch_upload_ &&
      bytes_ps <= constants_ps_bytes_ &&
      SameAsUploaded(from_upload ? upload_data_ + constants_ps_offset_ : shadow_constants_ps_.data(),
                     shadow_constants_ps_.data(), r + kRegConstantsPs, bytes_ps, from_upload)) {
    constants_ps_generation_ = p.generation_constants_ps;
    ++deduped_constants_[1];
  } else if (ps && (constants_ps_generation_ != p.generation_constants_ps ||
             constants_ps_epoch_ != epoch_upload_ || bytes_ps > constants_ps_bytes_ ||
             constants_mode_ps != constants_ps_mode_ ||
             tonemap_scale_bits != constants_ps_tonemap_scale_bits_)) {
    ++reuploaded_ps_[constants_ps_generation_ != p.generation_constants_ps ? 0
                    : constants_ps_epoch_ != epoch_upload_                 ? 1
                                                                            : 2];
    Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps, use_ubo_ ? alignment_ubo_ : 16,
             constants_ps_offset_);
    std::memcpy(upload_data_ + constants_ps_offset_, r + kRegConstantsPs, bytes_ps);
    if (keep_copies) std::memcpy(shadow_constants_ps_.data(), r + kRegConstantsPs, bytes_ps);
    if (constants_mode_ps) {
      // UE3 writes this composition with SCENE_COLOR_BIAS_FACTOR=8 for Xenos' packed 7e3
      // output path. In the native renderer the EDRAM view is canonical FP16, so carrying that
      // packed-domain scale to the host attachment saturates the later UNORM presentation. The
      // measured equivalent at this boundary is 8 / 256 = 1 / 32.
      std::memcpy(upload_data_ + constants_ps_offset_, &tonemap_scale, sizeof(tonemap_scale));
    }
    constants_ps_generation_ = p.generation_constants_ps;
    constants_ps_epoch_ = epoch_upload_;
    constants_ps_bytes_ = bytes_ps;
    constants_ps_mode_ = constants_mode_ps;
    constants_ps_tonemap_scale_bits_ = tonemap_scale_bits;
  } else if (ps && !constants_mode_ps) {
    // The tonemap override intentionally differs from the guest bank.
    AuditCacheConstants(false, ps->number, p.generation_constants_ps,
                           r + kRegConstantsPs, constants_ps_offset_, bytes_ps);
  }

  Stage(4, mark);
  // --- Viewport, scissor and shared constants ---------------------------------------
  /*
   * The framing (viewport, ndc and scissor), cached (sa2_native_framing_cache).
   *
   * The viewport, the ndc and the scissor are a function of the framing registers (PA_CL_VTE_CNTL,
   * PA_CL_VPORT_*, PA_SC_WINDOW_OFFSET/SCISSOR, PA_CL_CLIP_CNTL and PA_SU_SC_MODE_CNTL:
   * IsRegisterOfFraming in the native graphics system, which bumps generation_framing on every path that
   * writes them) and of the pass size and scale (pass_series_). In play the framing really changes
   * 0.07-0.14 times per draw ("C6 generations"). The depth bias is not included: it depends on
   * PA_SU_POLY_OFFSET_*, RB_DEPTH_INFO and the primitive type, which do not bump that generation.
   */
  const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
  const auto phase_probe = me::native::GetMsaa2PhaseProbe(
      uint32_t(REXCVAR_GET(sa2_native_diag_msaa2_phase)),
      (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3u, no_clip, vte);
  if (!phase_probe.valid) return Reject(53, "unsupported collapsed-2x phase diagnostic raster contract");
  if (phase_probe.active && (pass_scale_ != 1.0f || pass_raster_scale_x_ != 1.0f))
    return Reject(53, "collapsed-2x phase diagnostic requires unscaled raster pixels");
  if (pass_raster_scale_x_ != 1.0f && pass_scale_ != 1.0f)
    return Reject(53, "guest FragCoord XY grid remapping does not support shadow resolution scaling");
  const uint32_t phase_probe_identity = phase_probe.active ?
      uint32_t(REXCVAR_GET(sa2_native_diag_msaa2_phase)) : 0;
  VkViewport viewport{};
  float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
  VkRect2D scissor{};
  uint32_t empty_framing = 0;
  const bool framing_in_cache = framing_cache_ && valid_framing_ &&
                                 p.generation_framing == framing_generation_ && pass_series_ == framing_pass_series_ &&
                                 framing_depth_half_ == pass_depth_float24_half_ &&
                                 framing_phase_probe_ == phase_probe_identity;
  bool check_framing = false;
  if (framing_in_cache) {
    const uint64_t n = ++framing_hits_;
    check_framing = n <= kFramingsToCheck || (n & 4095) == 0;
    ++framing_hits_report_;
  }
  if (framing_in_cache && !check_framing) {
    viewport = framing_viewport_;
    std::memcpy(ndc, framing_ndc_, sizeof(ndc));
    scissor = framing_scissor_;
    empty_framing = empty_framing_;
  } else {
    empty_framing = ComputeFraming(r, vte, no_clip, sc_mode, viewport, ndc, scissor, phase_probe);
    ++framing_computations_report_;
    if (check_framing) {
      CompareFraming(viewport, ndc, scissor, empty_framing);  // with the stored values, before overwriting them
    }
    framing_viewport_ = viewport;
    std::memcpy(framing_ndc_, ndc, sizeof(ndc));
    framing_scissor_ = scissor;
    empty_framing_ = empty_framing;
    framing_generation_ = p.generation_framing;
    framing_pass_series_ = pass_series_;
    framing_depth_half_ = pass_depth_float24_half_;
    framing_phase_probe_ = phase_probe_identity;
    valid_framing_ = true;
  }
  if (ps) {
    // ds2-nx diagnostics: ds2_diag_ps_census (draws per pixel shader, every 10 s) and ds2_diag_skip_ps (draws with
    // those pixel shaders are not drawn), to find which shaders draw a part of the image.
    static const bool census = REXCVAR_GET(ds2_diag_ps_census);
    static const std::unordered_set<uint32_t> skip = [] {
      std::unordered_set<uint32_t> set;
      const std::string text = REXCVAR_GET(ds2_diag_skip_ps);
      for (const char* c = text.c_str(); *c;) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(c, &end, 10);
        if (end == c) { ++c; continue; }
        set.insert(uint32_t(v));
        c = end;
      }
      return set;
    }();
    if (census) {
      ++diag_ps_census_[ps->number];
      const auto now = std::chrono::steady_clock::now();
      if (now - diag_ps_census_at_ >= std::chrono::seconds(10)) {
        diag_ps_census_at_ = now;
        std::vector<std::pair<uint32_t, uint64_t>> list(diag_ps_census_.begin(), diag_ps_census_.end());
        std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
        std::string text;
        for (const auto& [n, c] : list) text += fmt::format(" n{}:{}", n, c);
        REXLOG_INFO("[native] ds2_diag_ps_census (draws in 10 s):{}", text);
        diag_ps_census_.clear();
      }
    }
    if (!skip.empty() && skip.count(ps->number)) return true;
  }
  {
    static const std::string diag = REXCVAR_GET(ds2_diag_ps_constants);  // ds2-nx diagnostic: framing too
    static const uint32_t diag_ps = diag.empty() ? UINT32_MAX : uint32_t(std::strtoul(diag.c_str(), nullptr, 10));
    if (ps && ps->number == diag_ps && logged_diag_framing_ < 4) {
      ++logged_diag_framing_;
      REXLOG_INFO("[native] ds2_diag_ps_constants PS n{} framing: empty {} viewport {},{} {}x{} scissor {},{} {}x{} "
                  "pass {}x{} no_clip {} ndc {} {} {} {} xscale {} xoffset {} yscale {} yoffset {}",
                  ps->number, empty_framing, viewport.x, viewport.y, viewport.width, viewport.height,
                  scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height, pass_width_,
                  pass_height_, no_clip, ndc[0], ndc[1], ndc[2], ndc[3],
                  Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]), Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]),
                  Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]), Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]));
    }
  }
  if (empty_framing != 0) {
    return true;  // empty viewport or scissor: as before
  }

  uint32_t specialization = entry->specialization;
  if (pass_depth_float24_half_) specialization |= kSpecDepthFloat24Half;
  const int float24_mode = REXCVAR_GET(sa2_native_float24_ps_mode);
  if (float24_mode < 0 || float24_mode > 2)
    return Reject(51, "FLOAT24 quantization mode must be 0 (off), 1 (truncate), or 2 (round)");
  if (pass_depth_float24_half_ && float24_mode && (r[gr::XE_GPU_REG_RB_DEPTHCONTROL] & 2)) {
    // Final depth clipping must not undo quantization at an unrepresentable
    // viewport endpoint. Reject that unsupported experiment explicitly.
    if (me::native::QuantizeDepthHalfCpu(viewport.minDepth, float24_mode == 2) != viewport.minDepth ||
        me::native::QuantizeDepthHalfCpu(viewport.maxDepth, float24_mode == 2) != viewport.maxDepth)
      return Reject(51, "FLOAT24 quantization needs exactly representable viewport depth bounds");
    specialization |= kSpecDepthFloat24Quantize;
    if (float24_mode == 2) specialization |= kSpecDepthFloat24Round;
  }
  if (pass_raster_scale_x_ != 1.0f) specialization |= kSpecRasterGridX;
  if (type == 8) specialization |= kSpecRectangle;
  const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
  float threshold_alpha = 0.0f;
  uint32_t function_alpha = 7;  // always
  // The 8 Xenos functions (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always) with alphaTestValue
  // (sa2_validated_normal library). Without a PS (mode 5) there is no test.
  if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
    threshold_alpha = Float(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
    function_alpha = control_color & 0x7;
    specialization |= 0x2;
    // The function goes in the pipeline, not in the constants. RB_COLORCONTROL was already part of the key,
    // so this creates no pipelines that did not already exist.
    specialization |= (function_alpha & 0x7u) << kSpecFunctionAlphaDisplacement;
  } else if (ps && ((control_color >> 4) & 0x1) && REXCVAR_GET(banjo_native_alpha_to_mask_test)) {
    // banjo-nx: alpha to mask (alpha to coverage) on a single-sample target is an alpha test at 0.5, as the
    // emulation does without MSAA: Nuts & Bolts' grass and foliage cards otherwise draw their cut-out parts.
    threshold_alpha = 0.5f;
    function_alpha = 6;  // >=
    specialization |= 0x2;
    specialization |= (function_alpha & 0x7u) << kSpecFunctionAlphaDisplacement;
  }
  shared[64] = (r[kRegBooleans] & 0xFFFF) | ((r[kRegBooleans + 4] & 0xFFFF) << 16);
  for (uint32_t i = 0; i < 8; ++i) shared[122 + i] = r[kRegBooleans + i];  // banjo-nx: BANJO_BOOL(A)
  // banjo-nx: the guest vertex index is SV_VertexID + vmin: the indices keep their values and vertexOffset
  // subtracts vmin (or the auto-index draw starts at 0), and shaders that read r0 never take the zero-base path.
  /*
   * sa2_native_vertices_base_zero, decided here (it used to be at the recording, after the shared block): a
   * single binding whose copy starts at a multiple of its stride binds the upload buffer at 0 and shifts the draw.
   * sa2_cpu_vertices_base_zero_r0: a vertex shader that reads r0 takes it too, with the index base compensated
   * (vmin - first_vertex: the same r0 for every vertex). Not for Mass Effect's postponed sky, which is drawn again
   * later with its own binding.
   */
  bool base_zero = false;
  uint32_t first_vertex = 0;  // offset / stride: where the copy starts, in vertices
  const bool r0_compensated = p.vs->reads_r0 && g_dk_base_zero_r0.active &&
                              !(ps && ps->shader && ps->shader->fingerprint == kSkyFingerprint);
  if (vertices_base_zero_ && type != 8 && entry->bindings.size() == 1 && (!p.vs->reads_r0 || r0_compensated)) {
    const VkDeviceSize stride = entry->bindings[0].stride;
    if (stride && offsets_vertices[0] % stride == 0) {
      base_zero = CheckBaseZero(offsets_vertices[0], stride, sources[0].bytes, vmin, first_vertex,
                                buffers_draw[0] == vcache_buf_.gpu && vertices_from_cache
                                    ? vcache_buf_.size
                                    : std::min<VkDeviceSize>(kUploadSize, upload_real_size_));
    } else {
      ++base_misaligned_zero_;
    }
  } else if (entry->bindings.size() > 1) {
    ++base_zero_several_bindings_;
  }
  if (base_zero && p.vs->reads_r0) g_dk_base_zero_r0.UseSampled();
  // sa2_gpu_index_base_held: a vertex shader proven not to read the vertex index runs with r0 starting at 0 and
  // ignores g_VertexIndexBase: it is held at 0 (not for rectangle lists, whose expanded shader is built separately).
  const bool index_unused = g_dk_index_base_held.active && type != 8 && VsIndexUnused(*p.vs);
  if (index_unused) {
    specialization |= kSpecVertexIndexUnused;
    g_dk_index_base_held.UseSampled();
  }
  shared[136] = index_unused ? 0 : base_zero && p.vs->reads_r0 ? vmin - first_vertex : vmin;
  for (size_t k = 0; k < indexed_sources.size(); ++k) {
    const IndexedSource& source = indexed_sources[k];
    if (!source.bytes) continue;
    VkDeviceSize offset = 0;
    const uint64_t fingerprint = dedupe_active_ ? std::max<uint64_t>(1, REXCVAR_GET(sa2_native_dedupe_sample)
                                     ? SampleFingerprint(source.data, source.bytes, source.bytes)
                                     : XXH3_64bits(source.data, source.bytes)) : 0;
    if (!(dedupe_active_ &&
          dedupe_.Search(source.address, source.bytes, uint32_t(source.order), offset, fingerprint))) {
      Reserve(source.bytes, 16, offset);
      const WorkCopy work{source.data, upload_data_ + offset, source.bytes / 4, source.order};
      if (!active_copies_ || !EnqueueCopy(work)) CopyVertices(work);
      if (dedupe_active_) dedupe_.Note(source.address, source.bytes, uint32_t(source.order), offset, fingerprint);
    }
    const VkDeviceAddress device_address = upload_address_ + offset;
    shared[130 + k * 2] = uint32_t(device_address);
    shared[131 + k * 2] = uint32_t(device_address >> 32);
    shared[134 + k] = source.bytes / 4;
    ++indexed_fetch_uploads_;
  }
  shared[65] = 0;  // g_SwappedTexcoords
  if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero
    // D3D's half pixel is a GUEST half pixel. One guest pixel spans the
    // expanded X grid, whereas the viewport extent below is in HOST pixels.
    // sa2-nx render resolution: a guest pixel is q / 4 host pixels (hp = 1 at the game's scale: the same floats).
    const float hp = pass_res_q_ == kScaleQuartersNative ? 1.0f : float(pass_res_q_) * 0.25f;
    const float mid[2] = {pass_raster_scale_x_ * hp / viewport.width,
                            -hp / std::abs(viewport.height)};
    std::memcpy(&shared[66], mid, sizeof(mid));
  }
  std::memcpy(&shared[68], &threshold_alpha, sizeof(threshold_alpha));
  shared[69] = function_alpha;  // g_AlphaFunction
  // ds2_vpos_render_scale: the pixel shaders' VPOS in guest pixels (4 / q). 1.0 at the game's scale or with the key off.
  {
    const float vpos_scale = REXCVAR_GET(ds2_vpos_render_scale) && pass_res_q_ != kScaleQuartersNative
                                 ? 4.0f / float(pass_res_q_) : 1.0f;
    std::memcpy(&shared[137], &vpos_scale, sizeof(vpos_scale));
  }
  std::memcpy(&shared[70], ndc, sizeof(ndc));
  std::copy(entry->remaps.begin(), entry->remaps.end(), shared + 74);
  VkDeviceSize offset_shared;
  /*
   * How many draws really change the shared constants.
   *
   * This block is 488 bytes that are zeroed, filled, compared and, if they changed, copied twice, once
   * into memory without CPU caching (2654 MB/s). With ~2,400 draws per frame that is ~4.7 MB of traffic
   * per frame on the thread that is already at 96 % of a core.
   *
   * Which fix applies depends on the count: if almost no draw changes the block, the comparison is the
   * waste; if almost all do, the double memcpy is. They are two different fixes, and counting costs one
   * increment.
   */
  ++shared_looked_;
  const auto t_shared =
      time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  // sa2_cpu_ring_compare_uploaded: the separate shared buffer is always CPU-cached; the upload buffer when its type is.
  const bool shared_from_upload = g_dk_compare_uploaded.active && !compare_uploaded_failed_ &&
                                  ((shared_separate_ && use_ubo_) || cached_upload_);
  const uint8_t* const shared_uploaded =
      (shared_separate_ && use_ubo_ ? shared_data_ : upload_data_) + shared_offset_;
  static const bool diag_shared = REXCVAR_GET(sa2_diag_shared_words);
  if (diag_shared && shared_epoch_ == epoch_upload_) {
    const uint32_t* before = shared_from_upload ? reinterpret_cast<const uint32_t*>(shared_uploaded) : shared_previous_;
    uint32_t changed = 0, first_changed = kSharedWords, groups = 0;
    for (uint32_t i = 0; i < kSharedWords; ++i) {
      if (shared[i] == before[i]) continue;
      ++diag_shared_word_[i];
      ++changed;
      if (first_changed == kSharedWords) first_changed = i;
      groups |= 1u << (i < 64 ? 0 : i == 64 ? 1 : i < 74 ? 2 : i < 90 ? 3 : i < 122 ? 4 : i < 130 ? 5 : i < 136 ? 6 : 7);
    }
    ++diag_shared_draws_;
    if (changed) ++diag_shared_changed_;
    for (uint32_t g = 0; g < 8; ++g) {
      if (groups == (1u << g)) ++diag_shared_only_group_[g];
    }
    if (groups == (1u << 7) && !p.vs->reads_r0) ++diag_shared_only_base_no_r0_;
    if (groups == ((1u << 7) | (1u << 0))) ++diag_shared_base_and_slots_;
  }
  if (shared_epoch_ == epoch_upload_ &&
      (shared_from_upload ? SameSharedUploaded(shared_uploaded, shared, sizeof(shared))
                          : Equal(shared, shared_previous_, sizeof(shared)))) {
    offset_shared = shared_offset_;  // the same as the previous draw
  } else {
    ++shared_changed_;
    if (shared_separate_ && use_ubo_) {
      // To the separate CPU-cached buffer; binding 2 of set 4 points to it.
      offset_shared = (shared_used_ + alignment_ubo_ - 1) & ~(alignment_ubo_ - 1);
      shared_used_ = offset_shared + kUboBytesShared;
      std::memcpy(shared_data_ + offset_shared, shared, sizeof(shared));
    } else {
      Reserve(use_ubo_ ? kUboBytesShared : kSharedWords * 4, use_ubo_ ? alignment_ubo_ : 16,
               offset_shared);
      std::memcpy(upload_data_ + offset_shared, shared, sizeof(shared));
    }
    if (!shared_from_upload || sa2::enh::VerifyMode()) std::memcpy(shared_previous_, shared, sizeof(shared));
    shared_offset_ = offset_shared;
    shared_epoch_ = epoch_upload_;
  }
  if (time_) {
    sub_ns_[12] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t_shared)
                                .count());
    ++sub_n_[12];
  }

  // deko3d: where UBO 2 reads the shared block (the separate CPU-cached buffer of the slot, or the upload buffer).
  const DkGpuAddr shared_address =
      (shared_separate_ && use_ubo_ ? shared_bufs_[slot_current_].Gpu(0) : upload_address_) + offset_shared;

  // --- Pipeline ---------------------------------------------------------------------------
  PipelineKey key;
  key.vs = p.vs->number + 1;
  key.ps = ps ? ps->number + 1 : 0;  // 0: mode 5, no fragment stage
  key.entry = entry->fingerprint;
  key.topology = uint32_t(topology);
  if (use_ubo_) {
    specialization |= kSpecConstantsUbo;  // the shaders read from set 4
  }
  if (inv_tex_size_) {
    specialization |= kSpecInvTexSize;
  }
  if (pcf_cheap_) {
    specialization |= kSpecPcfCheap;
  }
  if (ps && ps->shader &&
      (shader_light_mask_ & Ds2ShaderLightMask(ps->shader->fingerprint)) && HasShaderLight(*ps)) {
    specialization |= kDs2ShaderLightSpec;
  }
  // GPU round 2 specializations (see their keys).
  if (g_dk_texcoords_unswapped.active) {
    specialization |= kSpecTexcoordsUnswapped;  // shared[65] (g_SwappedTexcoords) is always written 0
    g_dk_texcoords_unswapped.UseSampled();
    if (sa2::enh::VerifyMode()) [[unlikely]] {
      sa2::enh::VerifyCheck();
      if (shared[65] != 0) {
        sa2::enh::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: sa2_gpu_texcoords_unswapped with g_SwappedTexcoords {:08X}", shared[65]);
      }
    }
  }
  if (g_dk_no_texture_signs.active && draw_signs == 0) {
    specialization |= kSpecNoTextureSigns;
    g_dk_no_texture_signs.UseSampled();
    if (sa2::enh::VerifyMode()) [[unlikely]] {
      // The slots the shader reads (all three heaps) must have no packed signs.
      sa2::enh::VerifyCheck();
      for (uint32_t i = 0; i < 48; ++i)
        if (shared[i] >> 24) {
          sa2::enh::VerifyDifference();
          REXLOG_ERROR("[native] DIFFERENCE: sa2_gpu_no_texture_signs with signs {:02X} in slot {}", shared[i] >> 24, i);
          break;
        }
    }
  }
  if (specialization & kSpecIdentityRemap) {
    g_dk_identity_remap.UseSampled();
    if (sa2::enh::VerifyMode()) [[unlikely]] {
      sa2::enh::VerifyCheck();
      for (uint32_t i = 0; i < 16; ++i)
        if (shared[74 + i] != kRemapIdentity) {
          sa2::enh::VerifyDifference();
          REXLOG_ERROR("[native] DIFFERENCE: sa2_gpu_identity_remap with remap {:03X} at location {}", shared[74 + i], i);
          break;
        }
    }
  }
  // sa2_gpu_structured_cf: the structured control flow of shaders that have it (the bit is set only for them, so
  // the other pipelines keep their keys and their cache entries).
  if (g_dk_structured_cf.active && (HasStructuredCf(*p.vs) || (ps && HasStructuredCf(*ps)))) {
    specialization |= kSpecStructuredCf;
    g_dk_structured_cf.UseSampled();
  }
  if (alpha_only && ps) {
    // If vegetation is excluded from the shadow map, this whole draw is unnecessary.
    if (no_vegetation_) {
      return true;
    }
    specialization |= kSpecAlphaOnly;
  }
  /*
   * Test depth before shading where it costs nothing.
   *
   * A pixel shader that can discard (alpha test or its own kill) forces the hardware to shade first and
   * test Z afterwards: otherwise a discarded fragment would already have written its depth. But that only
   * matters if the draw writes depth. If it only tests it (smoke, particles, glass, decals, lights),
   * testing earlier is exact: there is no write to move, the shader still discards the color, and the GPU
   * stops shading what is hidden.
   *
   * The counters are kept even with the setting off, so the report says the same in both halves of an A/B
   * test, and the "CANNOT: they write depth" line measures exactly what a depth pre-pass
   * would have to fix.
   */
  if (ps && keys[4] && masks) {
    const bool test_z = ((control_depth >> 1) & 0x1) != 0;
    const bool writes_z = test_z && ((control_depth >> 2) & 0x1) != 0;
    const bool stencil = (control_depth & 0x1) != 0;
    const bool ps_writes_z = (ps->outputs & 0x10) != 0;
    const bool late = (specialization & 0x2) || ps->discards || ps_writes_z;
    if (!test_z) {
      // Without a depth test there is nothing to move earlier.
    } else if (!late) {
      ++counts_z_[kZAlreadyEarly];
    } else if (writes_z || ps_writes_z) {
      ++counts_z_[kZWritesZ];
    } else if (stencil) {
      ++counts_z_[kZStencil];
    } else if (ps->shader) {
      ++counts_z_[kZSet];
      if (z_early_) {
        specialization |= kSpecZEarly;
      }
    }
  }
  uint32_t outputs_mask_7e3 = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!keys[i]) continue;
    const uint32_t format = (r[kInfoColor[i]] >> 16) & 0xF;
    const int32_t bias_color = int32_t(r[kInfoColor[i]] << 6) >> 26;
    // sa2_cpu_ring_bias_seen: a target info already recorded is remembered in a 4-slot table, so draws into a
    // biased target skip the unordered_set insert (it could only find it again: same log lines).
    bool bias_known = false;
    if (bias_color && g_dk_bias_seen.active) {
      for (const uint32_t seen : bias_seen_) bias_known |= seen == r[kInfoColor[i]];
      if (bias_known) g_dk_bias_seen.UseSampled();
    }
    if (bias_color && !bias_known && g_dk_bias_seen.active) {
      bias_seen_[bias_seen_next_++ & 3] = r[kInfoColor[i]];  // inserted below or already in the set
    }
    if (bias_color && !bias_known && biases_color_recorded_.insert(r[kInfoColor[i]]).second) {
      REXLOG_WARN("[native] ME output-merger RT{} info {:08X}: format {} exponent bias {} (factor {})",
                  i, r[kInfoColor[i]], format, bias_color, std::ldexp(1.0f, bias_color));
    }
    if (format == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) ||
        format == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
      outputs_mask_7e3 |= 1u << i;
    }
  }
  // ds2scholar-nx (2026-10-23), measurement for a 32-bit host format of the 7e3 targets (R11G11B10 has no alpha):
  // do draws into them write alpha, and does their color blending read the destination's alpha?
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(outputs_mask_7e3 & (1u << i)) || !keys[i]) continue;
    ++diag7e3_[0];
    if ((masks >> (4 * i + 3)) & 1) ++diag7e3_[1];
    static constexpr uint32_t kBlendDiag[4] = {gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
                                               gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    const uint32_t blend = r[kBlendDiag[i]];
    const uint32_t src = blend & 0x1F, dst = (blend >> 8) & 0x1F;
    const auto reads_dst_alpha = [](uint32_t f) { return f == 10 || f == 11 || f == 16; };
    if (((masks >> (4 * i)) & 7) && (reads_dst_alpha(src) || reads_dst_alpha(dst))) ++diag7e3_[2];
    if (!(src == 1 && dst == 0)) ++diag7e3_[3];  // blending at all
  }
  if (outputs_mask_7e3) {
    specialization |= kSpecTarget7e3 |
                        (outputs_mask_7e3 << kSpecMask7e3Displacement);
  }
  // sa2_gpu_blend_skip: one color target only (SA2 draws with one).
  if (g_dk_blend_skip.active && ps && keys[0] && !keys[1] && !keys[2] && !keys[3]) {
    if (const uint32_t bits = BlendSkipBits(r, control_depth, keys[4] != 0, masks & 0xF)) {
      specialization |= bits;
      g_dk_blend_skip.UseSampled();
    }
  }
  key.specialization = specialization;
  std::copy(std::begin(pass_formats_), std::end(pass_formats_), std::begin(key.formats));
  // PipelineKey::fill (0 for every pipeline at the game's resolution without MSAA, as before): bits 0-7 the sa2_msaa
  // rasterization samples (0 = 1), bits 8-15 the render scale of a line pipeline (lines keep their width relative
  // to the image: lineWidth = q / 4).
  key.fill = (pass_samples_ > 1 ? pass_samples_ : 0) |
             (ClassTopology(key.topology) == 1 && pass_res_q_ != kScaleQuartersNative && max_line_width_ > 1.0f
                  ? (pass_res_q_ << 8) : 0u);
  static constexpr uint32_t kBlend[4] = {
      gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
      gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
  for (uint32_t i = 0; i < 4; ++i) {
    if (keys[i]) {
      key.blend[i] = r[kBlend[i]] & 0x1FFF1FFF;
    }
  }
  key.masks = masks;
  // Diagnostic (ds2_diag_7e3_no_alpha): the 7e3 targets keep their alpha (draws do not write it), to see whether
  // anything later reads it; with an unchanged image a 32-bit host format without alpha would do.
  if (static const bool no_alpha = REXCVAR_GET(ds2_diag_7e3_no_alpha); no_alpha && outputs_mask_7e3) {
    for (uint32_t i = 0; i < 4; ++i)
      if (outputs_mask_7e3 & (1u << i)) key.masks &= ~(1u << (4 * i + 3));
  }
  key.depth = keys[4] ? control_depth : 0;
  key.rasterization = (sc_mode & 0x7) | (reset && !fan ? 0x8 : 0) | (with_bias ? 0x10 : 0);
  if (phase_probe.active) {
    key.rasterization |= me::native::kNativeMsaa2PhaseProbe;
    if (phase_probe_identity == 2) key.rasterization |= me::native::kNativeMsaa2PhaseMode2;
  }
  // PA_CL_CLIP_CNTL.clip_disable must also disable host Z clipping. ME1's
  // physical EDRAM clear rectangles deliberately use a slightly negative Z;
  // clipping them leaves previous-frame color/stencil words behind.
  // deko3d: DkRasterizerState::depthClampEnable is always available (Vulkan: the depthClamp feature).
  if (me::native::XenosDepthClamp(r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], true))
    key.rasterization |= me::native::kNativeDepthClamp;
  // Same as rexglue draw_util::IsPrimitivePolygonal: rectangle lists have
  // no front/back culling even though the host expansion uses triangles.
  if (type == 8) key.rasterization &= ~0x3u;
  const PipelineDk* pipeline = nullptr;
  // Looked up with SearchKey (phase 0a: canonical form; phases 1 and 2: without the state set through
  // vkCmdSet*). the key stays raw: the deferred sky (opaque_in_all), the counter and the dynamic state read
  // it.
  SA2_SUB(11, pipeline = PipelineFor(SearchKey(key), *entry, p));
  if (pipeline == nullptr) {
    return false;
  }
  // (The NVK prefetch of the pipeline about to be bound, vk_switch_preload_pipeline, has no deko3d counterpart:
  // there is no pipeline object to prefetch.)
  // deko3d: the vertex input the pipeline used to carry. Built once per entry; the rectangle-list entry is rebuilt
  // with every draw (its attributes change in place).
  if (!entry->layout_built) {
    entry->layout = VerticesLayoutFor(*entry);
    entry->layout_built = true;
  }
  if (!entry->layout.valid) {
    return Reject(14, "[dk] vertex format without a deko3d equivalent");
  }

  Stage(5, mark);
  /*
   * The three dynamic state values that used to be computed inside the recording block are computed here,
   * without recording anything. They are pure computations on the draw's registers; they are needed
   * earlier so they can be saved if this draw is the sky and gets deferred (see below).
   */
  /*
   * Record how far down the game draws in this render target. It only grows, so the pass is never opened
   * smaller than what has already been seen drawn.
   */
  if (area_util_) {
    const uint32_t until = uint32_t(std::max(0, scissor.offset.y + int32_t(scissor.extent.height)));
    const uint32_t rounded = (until + 63u) & ~63u;
    uint32_t& pointed = UtilHeightFor(pitch);  // without a map lookup on every draw
    if (rounded > pointed) {
      pointed = rounded;
    }
  }
  const float blend_constant[4] = {Float(r[gr::XE_GPU_REG_RB_BLEND_RED]),
                                     Float(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                     Float(r[gr::XE_GPU_REG_RB_BLEND_BLUE]),
                                     Float(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
  const uint32_t stencil_front = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
  const uint32_t stencil_back = ((control_depth >> 7) & 0x1)
                                     ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                     : stencil_front;
  /*
   * The sky, deferred until after the opaque draws (sa2_native_postponed_sky).
   *
   * How it is recognized, and why it cannot be mistaken for another draw. Three conditions at once, none of
   * them a magic position or draw count:
   *
   *  1. The pixel shader is the sky's, by the fingerprint of its original container (kSkyFingerprint). It is
   *     the only one of the library's 152 shaders whose constant table names CloudIntensity and
   *     SkyAlphaTag. The fingerprint does not depend on the SPIR-V translation or on the library order.
   *  2. The draw writes color and is opaque on all its targets: blending is "1 x source + 0 x destination"
   *     (ADD) on the channels the mask writes, exactly the criterion PipelineFor uses to decide
   *     blendEnable. Opaque = it does not read the existing color, so whatever was painted before it does
   *     not matter.
   *  3. It tests depth and does not write it, and there is no stencil. Without a Z write, deferring it
   *     cannot change what later draws see; with the test on, in its new place the Z test discards it
   *     where the world used to overwrite it.
   *
   * If any of the three fails (another version of the game, another state) the draw takes the usual path
   * and nothing happens.
   */
  // A draw with no color to write does not read the target: it does not count as transparent.
  bool opaque_in_all = true;
  for (uint32_t i = 0; i < 4 && opaque_in_all; ++i) {
    const uint32_t i_mask = (masks >> (i * 4)) & 0xF;
    if (!i_mask) {
      continue;
    }
    const uint32_t m = key.blend[i];
    const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
    const bool direct_alpha =
        ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
    const bool on_blend =
        (((i_mask & 0x7) != 0) && !color_direct) || (((i_mask & 0x8) != 0) && !direct_alpha);
    if (on_blend) {
      opaque_in_all = false;
    }
  }
  const bool test_z_draw = ((control_depth >> 1) & 0x1) != 0;
  const bool writes_z_draw = test_z_draw && ((control_depth >> 2) & 0x1) != 0 &&
                                keys[4] != 0;
  bool is_sky = false;
  if (ps && ps->shader && ps->shader->fingerprint == kSkyFingerprint && masks) {
    ++seen_sky_;
    const bool stencil = (control_depth & 0x1) != 0;
    /*
     * Scene pass only. The same dome is also drawn in cubemaps and reflections, and there it
     * is not touched, and the measured saving (4.8 of the scene's 19.2 ms) is the scene's. This line is
     * what keeps those out of it.
     */
    const bool in_the_scene = CategoryOfTarget(pitch, keys) == kGpuScene;
    /*
     * The fingerprint is not enough, and that broke an earlier version.
     *
     * The fingerprint identifies the shader, not the draw. The counter said so: "7.00 detected per
     * frame" when the analysis expected 0.9. Seven draws use that pixel shader, and deferring one broke
     * the order of the other six: flickering and odd colors.
     *
     * Now it must also be the dome:
     *   - the index count measured by the analysis of the SPIR-V and the log: 480 = 160 triangles;
     *   - and it must be the first draw of the pass (the dome is painted on the empty Z-buffer, which is
     *     exactly why it shades the whole screen and is then covered).
     *
     * Both are cheap to check and both belong to the dome, not to the shader. If the counter still said
     * more than one per frame, nothing is deferred: there is a guard below.
     */
    const uint32_t indices_draw =
        uint32_t(indices_from_16 ? indices16_.size() : indices_.size());
    const bool geometry_of_dome = indices_draw == kIndicesDomeSky;
    const bool first_of_the_pass = draws_in_pass_ == 0;
    /* The full criterion, without the cvar or the guard: what is measured during the test. */
    const bool is_the_dome = in_the_scene && opaque_in_all && test_z_draw &&
                            !writes_z_draw && !stencil && !(ps->outputs & 0x10) &&
                            geometry_of_dome && first_of_the_pass;
    if (is_the_dome) {
      ++sky_candidates_;  // the ones matching the dome's marks: this must be 1
      /*
       * The guard counts here, whether it defers or not. Two in the same frame is exactly what broke the
       * image before, so as soon as it happens it switches off and stays off for the whole session.
       */
      if (++sky_guard_in_frame_ > 1 && sky_guard_ != kDiscardedSky) {
        sky_guard_ = kDiscardedSky;
        REXLOG_WARN("[native] C6 sky: the guard sees {} domes in the SAME frame; nothing is deferred "
                    "for the whole session (build 137 failure). The image is left untouched",
                    sky_guard_in_frame_);
      }
    }
    is_sky = postponed_sky_ && is_the_dome && sky_guard_ == kSkyPostponing && !vertices_from_cache;
    if (!is_sky && postponed_sky_) {
      ++sky_no_postponable_;  // only with the setting on: measures criterion failures, not the cvar
    }
  }
  /*
   * Which draws the sky can be moved past, and why that does not change a single pixel.
   *
   * The sky can only skip past a draw D if both hold:
   *
   *  - D is opaque: it does not read the existing color, so wherever D paints, the final color is its own
   *    whether or not the sky was underneath. A draw with blending does read the background: the sky goes
   *    first.
   *  - D writes depth: after D the Z-buffer holds D's z, which is closer than the dome, so the sky's LEQUAL
   *    test discards it exactly where D painted. If D did not write Z, the Z-buffer would stay as it was
   *    and the sky would be painted over D: that would change the image.
   *
   * So the sky is emitted as soon as the first draw that fails either condition arrives. In practice that
   * is the first transparent draw, because what follows the sky is the opaque world; the counter separates
   * the two reasons so this can be checked in the log.
   */
  if (pending_sky_) {
    if (is_sky) {
      ++sky_two_in_pass_;  // another sky in the same pass: emit the earlier one now, keeping the order
      EmitPostponedSky(cmd, kSkyPerOtherSky);
    } else if (!(opaque_in_all && writes_z_draw)) {
      EmitPostponedSky(cmd, opaque_in_all ? kSkyPerNoZ : kSkyPerBlend);
    }
  }
  // --- Record ---------------------------------------------------------------------------------
  if (is_sky) {
    // The arguments of the vkCmd* calls this draw would have emitted are saved; nothing is recorded. The
    // upload buffer is an allocator that only moves forward (Reserve) and is not reset until UseSlot,
    // which always comes after BeforeSend -> FinishPass: when the sky is emitted, its offsets still
    // point to the same data.
    PostponedSky& c = sky_;
    c.pipeline = pipeline;
    c.buffer = upload_address_;
    c.usa_ubo = use_ubo_;
    // (deko3d: no push constants, c.push stays zero; the three blocks are found again from offsets_ubo and slot_ubo.)
    c.offsets_ubo = use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_),
                                                        uint32_t(constants_ps_offset_),
                                                        uint32_t(offset_shared)}
                              : std::array<uint32_t, 3>{0, 0, 0};
    c.slot_ubo = slot_current_;
    sky_layout_ = entry->layout;  // deko3d: the vertex input its pipeline carried
    c.viewport = viewport;
    c.scissor = scissor;
    std::memcpy(c.blend, blend_constant, sizeof(c.blend));
    std::memcpy(c.bias, bias, sizeof(c.bias));
    c.with_stencil = keys[4] != 0;
    c.stencil[0] = stencil_front;
    c.stencil[1] = stencil_back;
    c.n_bindings = uint32_t(entry->bindings.size());
    c.offsets_vertices = offsets_vertices;
    c.with_indices = with_indices;
    c.indices_from_16 = indices_from_16;
    c.indices = uint32_t(indices_from_16 ? indices16_.size() : indices_.size());
    c.first_index = uint32_t(offset_indices / (indices_from_16 ? 2 : 4));
    c.vmin = vmin;
    c.count = count;
    c.ps_more_one = ps ? ps->number + 1 : 0;
    c.vs_more_one = p.vs ? p.vs->number + 1 : 0;
    c.category = CategoryOfTarget(pitch, keys);
    c.draws_on_postpone = draws_in_pass_;
    // (eds_mode / EdsStateFor: the EDS phases do not exist in deko3d, every value above is already all of it.)
    pending_sky_ = true;
    ++postponed_sky_count_;
    // Counted as if it had been recorded: it will be recorded before the pass closes.
    ++draws_per_category_[c.category];
    triangles_per_category_[c.category] += (with_indices ? c.indices : count) / 3;
    Stage(6, mark);
    ++drawn_;
    // draws_in_pass_ is not incremented here: this draw is not recorded yet. It is incremented in
    // EmitPostponedSky, and the difference with draws_on_postpone gives the draws that went ahead of
    // it, which is exactly the geometry that now covers it.
    drawn_timed_ += time_ ? 1 : 0;
    return true;
  }
  context_->CountDrawMarkGpu();
  if (label_pass_) {
    label_pass_ = false;
    context_->LabelMarkGpu((uint32_t(p.vs ? p.vs->number : 0xFFFF) << 16) | (p.ps ? p.ps->number & 0xFFFF : 0xFFFF));
  }
  if (pipeline != pipeline_bound_) {
    if (count_changes_pipeline_) {  // Measurement only (sa2_native_count_changes_pipeline)
      CountChangePipeline(key, pipeline_bound_ == nullptr);
    }
    // deko3d: no pipeline object. The tracker binds the shaders and the state groups that differ from what this
    // command list has (StateTracker, sa2_native_draws_dk_types.h); substage 0 keeps its name.
    SA2_SUB(0, state_tracker_.Bind(cmd, *pipeline));
    pipeline_bound_ = pipeline;
  }
  // (Phases 1 and 2 of the dynamic state, PinDynamicState: in deko3d every piece of state is set by command, so
  // there is no pipeline without state to complete and no guard to compare it with.)
  if (!sets_bound_) {
    // The one image set and the one sampler set (sa2_dk.h), once per command list.
    SA2_SUB(1, sa2::dk::BindDescriptors(cmd));
    sets_bound_ = true;
  }
  // deko3d: descriptors written while this list is recorded (new textures, a slot pointed at the empty texture)
  // reach the GPU through its descriptor cache, which only a barrier invalidates (what UPDATE_AFTER_BIND gave the
  // Vulkan code). The CPU writes all happen before the submission, so one barrier ahead of the first draw that can
  // read them is enough.
  if (sa2::dk::DescriptorsWrittenSince(descriptors_seen_)) {
    sa2::dk::Barrier(cmd, sa2::dk::Hazard::Descriptors);
  }
  // Dynamic state and push constants repeat a lot between consecutive draws: they are only recorded if
  // they change (recording: 0.4 us per draw in play).
  const bool recorded = recorded_state_;
  // (Push constants: the deko3d shaders read their constants from uniform buffers only. uam has no buffer
  // pointers, so kSpecConstantsUbo is always on (use_ubo_) and there are no three addresses to push;
  // sa2_gpu_push_once has nothing to save.)
  // Set 4 (dynamic UBOs) -> UBO 0 (vertex constants, vertex stage), 1 (pixel constants, fragment stage) and 2
  // (shared, both stages): one dkCmdBufBindUniformBuffer per block whose offset changed, which is what NVK did by
  // differences inside its set-4 bind. Only recorded when something changes.
  {
    const std::array<uint32_t, 3> offsets_ubo =
        use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_), uint32_t(constants_ps_offset_),
                                            uint32_t(offset_shared)}
                  : std::array<uint32_t, 3>{0, 0, 0};
    ++set4_draws_;  // C6 set 4 report
    if (!ubo_bound_ || slot_ubo_bound_ != slot_current_ || offsets_ubo != offsets_ubo_bound_) {
      // Which offsets change at each bind (measurement only). What NVK saves by differences depends on it:
      // every cbuf whose offset does not change is one rebind less in the Draw.
      const bool all = !ubo_bound_ || slot_ubo_bound_ != slot_current_;
      if (all) {
        ++set4_first_;
      } else {
        ++set4_changes_[(offsets_ubo[0] != offsets_ubo_bound_[0] ? 1u : 0u) |
                        (offsets_ubo[1] != offsets_ubo_bound_[1] ? 2u : 0u) |
                        (offsets_ubo[2] != offsets_ubo_bound_[2] ? 4u : 0u)];
      }
      SA2_SUB(2, {
        if (all || offsets_ubo[0] != offsets_ubo_bound_[0])
          dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, upload_address_ + constants_vs_offset_,
                                    uint32_t(kUboBytesVs));
        if (all || offsets_ubo[1] != offsets_ubo_bound_[1])  // also without a fragment stage: harmless
          dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, upload_address_ + constants_ps_offset_,
                                    uint32_t(kUboBytesPs));
        if (all || offsets_ubo[2] != offsets_ubo_bound_[2]) {
          dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, shared_address, uint32_t(kUboBytesShared));
          dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 2, shared_address, uint32_t(kUboBytesShared));
        }
      });
      offsets_ubo_bound_ = offsets_ubo;
      slot_ubo_bound_ = slot_current_;
      ubo_bound_ = true;
    }
  }
  if (!recorded || !Equal(&viewport, &viewport_recorded_, sizeof(viewport))) {
    SA2_SUB(3, sa2::dk::SetViewport(cmd, viewport));
    viewport_recorded_ = viewport;
  }
  if (!recorded || !Equal(&scissor, &recorded_scissor_, sizeof(scissor))) {
    const DkScissor scissor_dk = ScissorDk(scissor);
    SA2_SUB(4, dkCmdBufSetScissors(cmd, 0, &scissor_dk, 1));
    recorded_scissor_ = scissor;
  }
  if (!recorded ||
      !Equal(blend_constant, recorded_blend_, sizeof(blend_constant))) {
    SA2_SUB(5, dkCmdBufSetBlendConst(cmd, blend_constant[0], blend_constant[1], blend_constant[2],
                                     blend_constant[3]));
    std::memcpy(recorded_blend_, blend_constant, sizeof(blend_constant));
  }
  // Also without bias: the state is dynamic in every pipeline and has to be recorded.
  // deko3d doubles the constant factor, NVK does not: SetDepthBias halves it (sa2_dk_raster.h, bench test 9).
  if (!recorded || !Equal(bias, recorded_bias_, sizeof(bias))) {
    SA2_SUB(6, sa2::dk::SetDepthBias(cmd, bias[0], 0.0f, bias[1]));
    std::memcpy(recorded_bias_, bias, sizeof(bias));
  }
  if (!recorded) {
    stencil_recorded_valid_ = false;
    indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  }
  if (keys[4]) {
    const uint32_t front = stencil_front;  // computed before the recording block
    const uint32_t back = stencil_back;
    if (!stencil_recorded_valid_ || front != stencil_recorded_[0] ||
        back != stencil_recorded_[1]) {
      // One call per face sets write mask, reference and compare mask (Vulkan: six calls).
      SA2_SUB(7, {
        dkCmdBufSetStencil(cmd, DkFace_Front, uint8_t(front >> 16), uint8_t(front), uint8_t(front >> 8));
        dkCmdBufSetStencil(cmd, DkFace_Back, uint8_t(back >> 16), uint8_t(back), uint8_t(back >> 8));
      });
      stencil_recorded_[0] = front;
      stencil_recorded_[1] = back;
      stencil_recorded_valid_ = true;
    }
  }
  recorded_state_ = true;
  // deko3d: the vertex input state (a VkPipeline carried it). Compared with what this command list has bound, not
  // by entry pointer: the rectangle-list entry is one object whose content changes.
  if (!layout_bound_valid_ || entry->layout.n_attributes != layout_bound_.n_attributes ||
      entry->layout.n_buffers != layout_bound_.n_buffers ||
      std::memcmp(entry->layout.attributes.data(), layout_bound_.attributes.data(),
                  sizeof(DkVtxAttribState) * entry->layout.n_attributes) != 0 ||
      std::memcmp(entry->layout.buffers.data(), layout_bound_.buffers.data(),
                  sizeof(DkVtxBufferState) * entry->layout.n_buffers) != 0) {
    if (entry->layout.n_attributes) {
      dkCmdBufBindVtxAttribState(cmd, entry->layout.attributes.data(), entry->layout.n_attributes);
    }
    if (entry->layout.n_buffers) {
      dkCmdBufBindVtxBufferState(cmd, entry->layout.buffers.data(), entry->layout.n_buffers);
    }
    layout_bound_ = entry->layout;
    layout_bound_valid_ = true;
  }
  /*
   * sa2_native_vertices_base_zero. With a single binding whose copy starts at a multiple of its stride,
   * the upload buffer is bound at 0 (once per command buffer, or after the deferred sky) and the draw is
   * shifted with first_vertex. The others bind as usual.
   */
  // (base_zero and first_vertex: decided before the shared block, where g_VertexIndexBase is written.)
  if (!entry->bindings.empty()) {
    // 16 identical pointers were filled in on every draw to bind one or two. It is filled when the upload
    // buffer changes (once per work slot), not 2,345 times per frame.
    if (buffers_vertices_[0] != upload_address_) {
      buffers_vertices_.fill(upload_address_);
      recorded_bindings_ = 0;  // the buffer changed: earlier bindings are stale
    }
    /*
     * And do not bind again if it is exactly what is already bound.
     *
     * In NVK each binding is 5 dwords plus an invocation of the MME macro NVK_MME_BIND_VB
     * (nvk_cmd_draw.c:4658), with no redundancy check inside: ~2,800 MME macros per frame. With vertex
     * deduplication 28-33 % of the bindings reuse the same offset, so two consecutive draws of the same
     * mesh give exactly the same offsets.
     */
    // On the base-zero path what gets bound is the whole buffer, from 0.
    static constexpr VkDeviceSize kBindingInZero = 0;
    const VkDeviceSize* a_bind = base_zero ? &kBindingInZero : offsets_vertices.data();
    const uint32_t n_bindings = uint32_t(entry->bindings.size());
    // ds2_vertex_cache_mb: a binding may come from the vertex cache instead of the upload buffer.
    const DkGpuAddr* b_bind = vcache_active_ ? buffers_draw.data() : buffers_vertices_.data();
    bool equal = n_bindings == recorded_bindings_;
    for (uint32_t i = 0; equal && i < n_bindings; ++i) {
      equal = a_bind[i] == offsets_recorded_[i] && b_bind[i] == buffers_recorded_[i];
    }
    if (!equal) {
      // deko3d: a binding is an address and a size (what the GPU may read from it: to the end of its buffer).
      // dkCmdBufBindVtxBuffers writes 6 words per buffer and reserves 5 (the CmdStream guard covers it).
      std::array<DkBufExtents, 16> extents{};
      const uint64_t upload_bytes = std::min<uint64_t>(kUploadSize, upload_real_size_);
      for (uint32_t i = 0; i < n_bindings; ++i) {
        const uint64_t limit = vcache_active_ && b_bind[i] == vcache_buf_.gpu ? vcache_buf_.size : upload_bytes;
        extents[i] = DkBufExtents{b_bind[i] + a_bind[i], uint32_t(limit > a_bind[i] ? limit - a_bind[i] : 0)};
      }
      SA2_SUB(8, dkCmdBufBindVtxBuffers(cmd, 0, extents.data(), n_bindings));
      ++base_zero_recorded_bindings_;
      recorded_bindings_ = n_bindings;
      for (uint32_t i = 0; i < n_bindings; ++i) {
        offsets_recorded_[i] = a_bind[i];
        buffers_recorded_[i] = b_bind[i];
      }
    }
    ++base_zero_total_;
    base_zero_draws_ += base_zero ? 1 : 0;
  }
  // Draws and triangles per render target type (same classification as the GPU time).
  {
    const uint32_t category = CategoryOfTarget(pitch, keys);
    const uint32_t vertices_draw = with_indices ? uint32_t(indices_from_16 ? indices16_.size() : indices_.size())
                                                 : count;
    ++draws_per_category_[category];
    triangles_per_category_[category] += vertices_draw / 3;
  }
  // In the diagnostic frame, one query per draw with its pixel shader.
  // The diagnostic window is decided by the context, at the Swap. With the diagnostic off (the normal
  // case) this does not even reach the call.
  const uint32_t query_draw =
      !diag_stats_draw_
          ? UINT32_MAX
          : context_->BeginStatsDraw(
                ps ? ps->number + 1 : 0,
                CategoryOfTarget(pitch, keys), p.vs ? p.vs->number + 1 : 0);
  // (GPU draw labels, sa2_native_gpu_labels / vkCmdInsertDebugUtilsLabelEXT: deko3d has no debug labels.)
  const DkPrimitive primitive = pipeline->state.primitive;
  if (with_indices) {
    // The upload buffer is bound once per index type and each draw uses firstIndex.
    // indices_type_recorded_ keeps the Vulkan enum as data (region 06's member).
    const VkIndexType indices_type = indices_from_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (indices_type != indices_type_recorded_) {
      SA2_SUB(9, dkCmdBufBindIdxBuffer(cmd, indices_from_16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32,
                                       upload_address_));
      indices_type_recorded_ = indices_type;
    }
    // deko3d: primitive restart is not pipeline state; its index follows the index format.
    state_tracker_.Restart(cmd, pipeline->state.restart, indices_from_16);
    // On the base-zero path, vertexOffset also carries where the copy starts (in vertices).
    const int32_t vertices_displacement =
        base_zero ? int32_t(int64_t(first_vertex) - int64_t(vmin)) : -int32_t(vmin);
    const uint32_t indices_draw = uint32_t(indices_from_16 ? indices16_.size() : indices_.size());
    SA2_SUB(10, dkCmdBufDrawIndexed(cmd, primitive, indices_draw,
                                    1, uint32_t(offset_indices / (indices_from_16 ? 2 : 4)),
                                    vertices_displacement, 0));
  } else {
    SA2_SUB(10, dkCmdBufDraw(cmd, primitive, count, 1,
                             base_zero ? first_vertex : 0, 0));  // firstVertex
  }
  if (query_draw != UINT32_MAX) {
    context_->FinishStatsDraw(query_draw);
  }
  Stage(6, mark);
  if (time_) {
    ++sub_samples_;
    ReportSubstages();
  }
  ++drawn_;
  ++draws_in_pass_;  // to know at which position of the pass the sky ends up emitted
  drawn_timed_ += time_ ? 1 : 0;
  return true;
}

/*
 * Emits the deferred sky draw (sa2_native_postponed_sky).
 *
 * It records exactly the same vkCmd* calls it would have recorded in its original place, with the
 * values saved then. It relies on nothing recorded afterwards: it sends all of this draw's dynamic
 * state, and when done it invalidates the tracked state so the next draw records its own again. That
 * way the order cannot slip through an "already set" comparison.
 *
 * The upload buffer offsets are still valid because Reserve only moves forward and the buffer is not
 * reset until UseSlot, which always comes after BeforeSend -> FinishPass.
 */
void DrawsDkImpl::EmitPostponedSky(DkCmdBuf cmd, uint32_t reason) {
  if (!pending_sky_ || cmd == nullptr) {
    return;
  }
  pending_sky_ = false;
  const PostponedSky& c = sky_;
  ++emitted_sky_[reason < kSkyReasons ? reason : kSkyReasons - 1];
  const uint64_t position = draws_in_pass_ - c.draws_on_postpone;
  sky_position_sum_ += position;
  sky_position_max_ = std::max(sky_position_max_, position);
  state_tracker_.Bind(cmd, *c.pipeline);
  pipeline_bound_ = c.pipeline;
  // (c.eds_mode / EmitDynamicState: no EDS phases in deko3d.)
  bound_valid_key_ = false;  // The sky does not keep its key (CountChangePipeline)
  if (!sets_bound_) {
    sa2::dk::BindDescriptors(cmd);
    sets_bound_ = true;
  }
  // (No push constants in deko3d.) The three constant blocks, UBO 0, 1 and 2, from the offsets and the slot that
  // were saved (what the Vulkan code passed to vkCmdBindDescriptorSets with set 4 of slot_ubo).
  const DkGpuAddr shared_base =
      shared_separate_ ? shared_bufs_[c.slot_ubo % shared_bufs_.size()].Gpu(0) : c.buffer;
  dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, c.buffer + c.offsets_ubo[0], uint32_t(kUboBytesVs));
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, c.buffer + c.offsets_ubo[1], uint32_t(kUboBytesPs));
  dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, shared_base + c.offsets_ubo[2], uint32_t(kUboBytesShared));
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 2, shared_base + c.offsets_ubo[2], uint32_t(kUboBytesShared));
  sa2::dk::SetViewport(cmd, c.viewport);
  const DkScissor scissor = ScissorDk(c.scissor);
  dkCmdBufSetScissors(cmd, 0, &scissor, 1);
  dkCmdBufSetBlendConst(cmd, c.blend[0], c.blend[1], c.blend[2], c.blend[3]);
  sa2::dk::SetDepthBias(cmd, c.bias[0], 0.0f, c.bias[1]);
  if (c.with_stencil) {
    dkCmdBufSetStencil(cmd, DkFace_Front, uint8_t(c.stencil[0] >> 16), uint8_t(c.stencil[0]),
                       uint8_t(c.stencil[0] >> 8));
    dkCmdBufSetStencil(cmd, DkFace_Back, uint8_t(c.stencil[1] >> 16), uint8_t(c.stencil[1]),
                       uint8_t(c.stencil[1] >> 8));
  }
  // deko3d: its vertex input (the pipeline carried it).
  const VerticesLayout& layout = sky_layout_;
  if (layout.n_attributes) dkCmdBufBindVtxAttribState(cmd, layout.attributes.data(), layout.n_attributes);
  if (layout.n_buffers) dkCmdBufBindVtxBufferState(cmd, layout.buffers.data(), layout.n_buffers);
  if (c.n_bindings) {
    // Still this slot's upload buffer: the sky is emitted before the pass closes, before UseSlot.
    const uint64_t buffer_size = std::min<uint64_t>(kUploadSize, upload_real_size_);
    std::array<DkBufExtents, 16> buffers{};
    for (uint32_t i = 0; i < c.n_bindings; ++i) {
      const uint64_t offset = c.offsets_vertices[i];
      buffers[i] = DkBufExtents{c.buffer + offset, uint32_t(buffer_size > offset ? buffer_size - offset : 0)};
    }
    dkCmdBufBindVtxBuffers(cmd, 0, buffers.data(), c.n_bindings);
  }
  const uint32_t query = !diag_stats_draw_
                                ? UINT32_MAX
                                : context_->BeginStatsDraw(c.ps_more_one, c.category, c.vs_more_one);
  if (c.with_indices) {
    const DkIdxFormat type = c.indices_from_16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32;
    dkCmdBufBindIdxBuffer(cmd, type, c.buffer);
    state_tracker_.Restart(cmd, c.pipeline->state.restart, c.indices_from_16);
    dkCmdBufDrawIndexed(cmd, c.pipeline->state.primitive, c.indices, 1, c.first_index, -int32_t(c.vmin), 0);
  } else {
    dkCmdBufDraw(cmd, c.pipeline->state.primitive, c.count, 1, 0, 0);
  }
  if (query != UINT32_MAX) {
    context_->FinishStatsDraw(query);
  }
  ++draws_in_pass_;
  // Nothing tracked is valid any more: the dynamic state, the bindings and the index type are the sky's.
  // recorded_state_ = false makes the next draw also re-record stencil and indices.
  recorded_state_ = false;
  ubo_bound_ = false;
  recorded_bindings_ = 0;
  stencil_recorded_valid_ = false;
  indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  layout_bound_valid_ = false;
}

// Adds the time since the mark to the stage and moves the mark to now.
/*
 * Measurement only. What takes the time inside the ring's recording and pipeline stages (4.1 and 2.5 us per
 * recorded draw): each Vulkan command, PipelineFor and the shared constants block, on the same timed
 * draws (1 in 64). "C6 substages" line every 10 s: us per draw, us per call and calls per timed draw.
 */
// Its lines, like those of the other ring reports, go to the report thread (SA2_REPORT_RING).
void DrawsDkImpl::ReportSubstages() {
  const auto now = std::chrono::steady_clock::now();
  if (now < sub_next_) {
    return;
  }
  const bool first = sub_next_ == std::chrono::steady_clock::time_point{};
  sub_next_ = now + std::chrono::seconds(10);
  if (first || !sub_samples_) {
    sub_ns_.fill(0);
    sub_n_.fill(0);
    sub_samples_ = 0;
    return;
  }
  static constexpr const char* kNames[19] = {
      "BindPipeline", "sets 0-3", "set 4 (UBO)", "Viewport", "Scissor", "BlendConstants", "DepthBias",
      "Stencil (6)", "BindVertexBuffers", "BindIndexBuffer", "Draw", "PipelineFor", "shared",
      "(unused)", "samplers (textures stage)",
      // The four segments of the "indices" stage (CutSubstage)
      "indices stage: targets and discards", "indices stage: depth bias",
      "indices stage: indices", "indices stage: vertices and diagnostics"};
  std::string line;
  double total = 0.0;
  for (size_t k = 0; k < 19; ++k) {
    if (!sub_n_[k]) {
      continue;
    }
    const double per_draw = double(sub_ns_[k]) / 1e3 / double(sub_samples_);
    total += per_draw;
    line += fmt::format(" | {} {:.2f} us/draw ({:.2f} us x {:.2f} per draw)", kNames[k], per_draw,
                         double(sub_ns_[k]) / 1e3 / double(sub_n_[k]),
                         double(sub_n_[k]) / double(sub_samples_));
  }
  SA2_REPORT_RING("[native] C6 substages ({} timed draws; sum {:.2f} us per draw){}", sub_samples_,
              total, line);
  sub_ns_.fill(0);
  sub_n_.fill(0);
  sub_samples_ = 0;
}

/*
 * The base-zero path of a draw (sa2_native_vertices_base_zero). Returns false if it has to bind as
 * usual. Self-checking guard: the first kBaseZeroToCheck draws, then 1 in 4,096, redo the computation
 * backwards in 64 bits (the first vertex the GPU reads falls on the first byte of the copy, with and
 * without indices), and check that vertexOffset fits in a signed 32-bit value and that the copy fits in
 * the buffer. On a single difference: that draw binds as usual, MISMATCH in the log and the path is off
 * for the session.
 */
bool DrawsDkImpl::CheckBaseZero(VkDeviceSize offset, VkDeviceSize stride, uint64_t bytes, uint32_t vmin, uint32_t& first_vertex, VkDeviceSize limit) {
  const VkDeviceSize first = offset / stride;
  if (first > VkDeviceSize(INT32_MAX)) {
    return false;  // does not fit in firstVertex/vertexOffset (cannot happen with a 64 MB buffer)
  }
  const uint64_t n = ++base_checked_zero_;
  if (n <= kBaseZeroToCheck || (n & 4095) == 0) {
    const int64_t displacement = int64_t(first) - int64_t(vmin);  // the vertexOffset of indexed draws
    const bool ok = first * stride == offset &&
                      (displacement + int64_t(vmin)) * int64_t(stride) == int64_t(offset) &&
                      displacement >= int64_t(INT32_MIN) && displacement <= int64_t(INT32_MAX) &&
                      offset + bytes <= limit;
    if (!ok) {
      vertices_base_off_zero_ = true;
      vertices_base_zero_ = false;
      sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
      REXLOG_ERROR("[native] C6 zero-base vertices: MISMATCH (offset {} stride {} bytes {} vmin {} first {}): "
                   "this draw and the rest of the session bind as usual",
                   offset, stride, bytes, vmin, first);
      return false;
    }
    if (n == kBaseZeroToCheck) {
      SA2_REPORT_RING("[native] C6 zero-base vertices: {} draws checked, 0 mismatches; still checking 1 in "
                  "every 4096", n);
    }
  }
  first_vertex = uint32_t(first);
  return true;
}

// Fica de fora (deko3d): ControlSet4 (Vulkan file line 5271) drove the patch_nvk_set4 contract
// (nvk_switch_set4); there is no NVK: the uniform buffers are bound by differences in Draw itself.

// Fica de fora (deko3d): ControlDrawNvk (Vulkan file line 5301) drove the nvk_switch_draw contract
// (measurement and per-draw improvements inside NVK).

// Fica de fora (deko3d): ReportDrawNvk (Vulkan file line 5353) printed NVK's per-draw figures.

/*
 * Every 10 s, set 4. What changes at each bind (measured here, with or without the NVK patch) and, if
 * NVK has it, what is saved: root table writes per bind (4 without it), cbufs not rebound and the state
 * of its guard.
 */
void DrawsDkImpl::ReportSet4() {
  const auto now = std::chrono::steady_clock::now();
  if (now - set4_report_ < std::chrono::seconds(10)) {
    return;
  }
  const bool first = set4_report_ == std::chrono::steady_clock::time_point{};
  set4_report_ = now;
  uint64_t bindings = set4_first_;
  for (const uint64_t n : set4_changes_) {
    bindings += n;
  }
  if (!first && set4_draws_ && bindings) {
    const auto pct = [bindings](uint64_t n) { return 100.0 * double(n) / double(bindings); };
    SA2_REPORT_RING(
        "[native] C6 set 4: {:.2f} binds per draw ({} in {} draws); changes only VS {:.1f} %, only PS "
        "{:.1f} %, only shared {:.1f} %, VS+PS {:.1f} %, VS+shared {:.1f} %, PS+shared {:.1f} %, "
        "all three {:.1f} %, after buffer, slot or sky {:.1f} % | constants uploaded again: VS {} by "
        "generation, {} by epoch and {} because the shader reads more; PS {}, {} and {}; same bytes reused: VS {}, "
        "PS {}",
        double(bindings) / double(set4_draws_), bindings, set4_draws_, pct(set4_changes_[1]),
        pct(set4_changes_[2]), pct(set4_changes_[4]), pct(set4_changes_[3]), pct(set4_changes_[5]),
        pct(set4_changes_[6]), pct(set4_changes_[7]), pct(set4_first_), reuploaded_vs_[0], reuploaded_vs_[1],
        reuploaded_vs_[2], reuploaded_ps_[0], reuploaded_ps_[1], reuploaded_ps_[2], deduped_constants_[0],
        deduped_constants_[1]);
    // (The second line, NVK's set 4 by differences, has no deko3d counterpart: no NVK.)
  }
  set4_changes_.fill(0);
  set4_first_ = 0;
  set4_draws_ = 0;
  reuploaded_vs_.fill(0);
  reuploaded_ps_.fill(0);
}

// Every 10 s, how many draws go without binding their own offset (base zero).
void DrawsDkImpl::ReportBaseZero() {
  const auto now = std::chrono::steady_clock::now();
  if (now - base_zero_report_ < std::chrono::seconds(10)) {
    return;
  }
  const bool first = base_zero_report_ == std::chrono::steady_clock::time_point{};
  base_zero_report_ = now;
  if (!first && base_zero_total_) {
    SA2_REPORT_RING("[native] C6 zero-base vertices ({}): {} of {} draws with vertices not bound at their own "
                "offset ({:.1f} %); {} with several binds and {} with the dedupe copy outside a "
                "multiple of the stride; {:.2f} vkCmdBindVertexBuffers per draw",
                vertices_base_off_zero_ ? "SWITCHED OFF by the guard"
                : vertices_base_zero_       ? "on"
                                            : "off",
                base_zero_draws_, base_zero_total_, 100.0 * double(base_zero_draws_) / double(base_zero_total_),
                base_zero_several_bindings_, base_misaligned_zero_,
                double(base_zero_recorded_bindings_) / double(base_zero_total_));
  }
  // sa2-nx round 6: the coarse stage stopwatch of the timed draws (collected since masseffect-nx, never printed): per
  // timed draw, in order, the front checks, targets + depth bias + indices + vertices, textures and samplers, upload
  // space + render pass, uploads, framing + shared constants + pipeline, and recording; then the pass change
  // (inside the upload-space stage) and its parts.
  if (!first && stage_draws_ > report_stage_draws_) {
    const double n = double(stage_draws_ - report_stage_draws_);
    auto us = [&](size_t i) { return double(stages_ns_[i] - report_stages_ns_[i]) / 1000.0 / n; };
    SA2_REPORT_RING("[native] C6 draw stages ({} timed draws, us per draw): front {:.2f} | targets, bias, "
                           "indices, vertices {:.2f} | textures {:.2f} | upload space and pass {:.2f} (pass change "
                           "{:.2f}: finish {:.2f}, targets {:.2f}, framebuffer {:.2f}, begin {:.2f}) | uploads {:.2f} "
                           "| framing, shared, pipeline {:.2f} | record {:.2f}",
                           uint64_t(n), us(0), us(1), us(2), us(3), us(7), us(8), us(9), us(10), us(11), us(4), us(5),
                           us(6));
  }
  report_stage_draws_ = stage_draws_;
  report_stages_ns_ = stages_ns_;
  // sa2_diag_shared_words: what changes in the shared block from one draw to the next.
  if (!first && diag_shared_draws_) {
    std::array<uint32_t, kSharedWords> order{};
    for (uint32_t i = 0; i < kSharedWords; ++i) order[i] = i;
    std::partial_sort(order.begin(), order.begin() + 8, order.end(),
                      [&](uint32_t a, uint32_t b) { return diag_shared_word_[a] > diag_shared_word_[b]; });
    const double n = double(diag_shared_draws_);
    SA2_REPORT_RING("[native] C6 shared words ({} draws): {:.1f} % change the block; only the texture/sampler "
                           "slots {:.1f} %, only booleans {:.1f} %, only texcoord/half pixel/alpha/ndc {:.1f} %, only "
                           "remaps {:.1f} %, only 1/size {:.1f} %, only bool words {:.1f} %, only indexed fetch "
                           "{:.1f} %, only the vertex index base {:.1f} % ({:.1f} % with a VS that does not read r0), "
                           "the base and the slots only {:.1f} % | most changed words: {} {} {} {} {} {} {} {} "
                           "({:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} %)",
                           diag_shared_draws_, 100.0 * double(diag_shared_changed_) / n,
                           100.0 * double(diag_shared_only_group_[0]) / n, 100.0 * double(diag_shared_only_group_[1]) / n,
                           100.0 * double(diag_shared_only_group_[2]) / n, 100.0 * double(diag_shared_only_group_[3]) / n,
                           100.0 * double(diag_shared_only_group_[4]) / n, 100.0 * double(diag_shared_only_group_[5]) / n,
                           100.0 * double(diag_shared_only_group_[6]) / n, 100.0 * double(diag_shared_only_group_[7]) / n,
                           100.0 * double(diag_shared_only_base_no_r0_) / n,
                           100.0 * double(diag_shared_base_and_slots_) / n, order[0], order[1], order[2], order[3],
                           order[4], order[5], order[6], order[7], 100.0 * double(diag_shared_word_[order[0]]) / n,
                           100.0 * double(diag_shared_word_[order[1]]) / n, 100.0 * double(diag_shared_word_[order[2]]) / n,
                           100.0 * double(diag_shared_word_[order[3]]) / n, 100.0 * double(diag_shared_word_[order[4]]) / n,
                           100.0 * double(diag_shared_word_[order[5]]) / n, 100.0 * double(diag_shared_word_[order[6]]) / n,
                           100.0 * double(diag_shared_word_[order[7]]) / n);
    diag_shared_draws_ = diag_shared_changed_ = diag_shared_only_base_no_r0_ = diag_shared_base_and_slots_ = 0;
    diag_shared_only_group_.fill(0);
    diag_shared_word_.fill(0);
  }
  // sa2-nx round 6: what the vertex dedupe costs and saves (its counters existed; nothing printed them).
  if (!first && dedupe_active_) {
    const uint64_t queries = dedupe_.queries() - report_dedupe_queries_, hits = dedupe_.hits() - report_dedupe_hits_;
    SA2_REPORT_RING("[native] C6 vertex dedupe (10 s): {} lookups, {} hits ({:.1f} %), {:.1f} MB not copied; "
                           "{} fingerprints over {:.1f} MB; {} stale reuses prevented, {} slot collisions",
                           queries, hits, queries ? 100.0 * double(hits) / double(queries) : 0.0,
                           double(dedupe_.bytes_saved() - report_dedupe_saved_) / 1048576.0,
                           dedupe_fingerprints_ - report_dedupe_fingerprints_,
                           double(dedupe_bytes_fingerprinted_ - report_dedupe_fingerprinted_) / 1048576.0,
                           dedupe_.discrepancies() - report_dedupe_discrepancies_,
                           dedupe_.collisions() - report_dedupe_collisions_);
  }
  report_dedupe_queries_ = dedupe_.queries();
  report_dedupe_hits_ = dedupe_.hits();
  report_dedupe_saved_ = dedupe_.bytes_saved();
  report_dedupe_discrepancies_ = dedupe_.discrepancies();
  report_dedupe_collisions_ = dedupe_.collisions();
  report_dedupe_fingerprints_ = dedupe_fingerprints_;
  report_dedupe_fingerprinted_ = dedupe_bytes_fingerprinted_;
  base_zero_draws_ = 0;
  base_zero_total_ = 0;
  base_zero_several_bindings_ = 0;
  base_misaligned_zero_ = 0;
  base_zero_recorded_bindings_ = 0;
}

/*
 * The framing of a draw, moved out of Draw unchanged (sa2_native_framing_cache). Returns 0 if it
 * has to draw, 1 if the viewport is empty and 2 if the scissor is empty (in both cases Draw returns
 * without drawing, as before; the scissor is left at zero). Same operations in the same order. The only
 * multiplications GCC could fuse with an add or subtract (width * 0.5 and height * 0.5) are exact, so
 * fused or not they give the same bits.
 */
uint32_t DrawsDkImpl::ComputeFraming(const uint32_t* r, uint32_t vte, bool no_clip, uint32_t sc_mode, VkViewport& viewport, float ndc[4], VkRect2D& scissor, me::native::Msaa2PhaseProbe phase_probe) {
  float scale_x = (vte & 0x1) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
  float center_x = (vte & 0x2) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
  float scale_y = (vte & 0x4) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
  float center_y = (vte & 0x8) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
  const float scale_z = (vte & 0x10) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
  const float center_z = (vte & 0x20) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
  const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  const int32_t window_x = int32_t((window & 0x7FFF) << 17) >> 17;
  const int32_t window_y = int32_t(((window >> 16) & 0x7FFF) << 17) >> 17;
  if ((sc_mode >> 16) & 0x1) {
    center_x += float(window_x);
    center_y += float(window_y);
  }
  if (phase_probe.active) {
    center_x += phase_probe.geometry_x;
    center_y += phase_probe.geometry_y;
  }
  // With the shadow map drawn smaller (sa2_native_shadows_scale), the guest still speaks in
  // 1600-pixel units. The viewport and the scissor are multiplied by the pass scale, and with that the
  // geometry lands where it should; the rest (ndc, half pixel) already comes from the pass size.
  if (pass_scale_ != 1.0f) {
    scale_x *= pass_scale_;
    center_x *= pass_scale_;
    scale_y *= pass_scale_;
    center_y *= pass_scale_;
  }
  scale_x *= pass_raster_scale_x_;
  center_x *= pass_raster_scale_x_;
  // sa2-nx render resolution: the guest viewport in host pixels (q / 4 in both axes; nothing at the game's scale).
  if (pass_res_q_ != kScaleQuartersNative) {
    const float s = float(pass_res_q_) * 0.25f;
    scale_x *= s;
    center_x *= s;
    scale_y *= s;
    center_y *= s;
  }
  viewport = VkViewport{};
  viewport.x = center_x - std::abs(scale_x);
  viewport.width = 2.0f * std::abs(scale_x);
  // A negative YSCALE (the D3D norm) gives a positive height; a positive one, an inverted height.
  viewport.y = center_y + scale_y;
  viewport.height = -2.0f * scale_y;
  if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
    Warn(14, "OpenGL clip space (dx_clip_space_def = 0): Z not adjusted");
  }
  // Scale BEFORE clamping: guest 1.5 must become host .75, not .5.
  viewport.minDepth = std::clamp(me::native::GuestDepthToNative(center_z, pass_depth_float24_half_), 0.0f, 1.0f);
  viewport.maxDepth = std::clamp(me::native::GuestDepthToNative(center_z + scale_z, pass_depth_float24_half_), 0.0f, 1.0f);
  // g_NdcScale (x, y) and g_NdcOffset (x, y): identity when clipping is on.
  ndc[0] = 1.0f;
  ndc[1] = 1.0f;
  ndc[2] = 0.0f;
  ndc[3] = 0.0f;
  if (no_clip) {
    const float width = float(pass_width_);
    const float height = float(pass_height_);
    ndc[0] = scale_x * 2.0f / width;
    ndc[2] = (center_x - width * 0.5f) * 2.0f / width;
    // DXC flips Y at the end of the VS (-fvk-invert-y): the D3D Y goes here.
    ndc[1] = -scale_y * 2.0f / height;
    ndc[3] = -(center_y - height * 0.5f) * 2.0f / height;
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = width;
    viewport.height = height;
  }
  scissor = VkRect2D{};
  // ds2-nx (ds2_scaled_min_viewport): under a render scale a guest viewport of one pixel (Dark Souls II draws its
  // 256x1 tone-map table as a line) shrank below a host pixel and the draw was dropped as empty; it keeps one.
  if (pass_res_q_ != kScaleQuartersNative && REXCVAR_GET(ds2_scaled_min_viewport) && !no_clip) {
    if (viewport.width > 0.0f && viewport.width < 1.0f) viewport.width = 1.0f;
    if (viewport.height != 0.0f && std::abs(viewport.height) < 1.0f)
      viewport.height = viewport.height < 0.0f ? -1.0f : 1.0f;
  }
  if (viewport.width < 1.0f || std::abs(viewport.height) < 1.0f) {
    return 1;
  }
  const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
  int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
  if (!((tl >> 31) & 0x1)) {
    x0 += window_x;
    y0 += window_y;
    x1 += window_x;
    y1 += window_y;
  }
  const float scale_scissor_x = pass_scale_ * pass_raster_scale_x_;
  if (scale_scissor_x != 1.0f) {
    x0 = int32_t(std::floor(float(x0) * scale_scissor_x));
    x1 = int32_t(std::ceil(float(x1) * scale_scissor_x));
  }
  if (pass_scale_ != 1.0f) {  // Y is shadow resolution only, never grid expansion.
    y0 = int32_t(std::floor(float(y0) * pass_scale_));
    y1 = int32_t(std::ceil(float(y1) * pass_scale_));
  }
  // sa2-nx render resolution: the scissor's guest pixel edges in host pixels (the pixel-centre rule of
  // me_resolution_scale.h: neighbouring scissors never overlap and leave no gap).
  if (pass_res_q_ != kScaleQuartersNative) {
    const bool guest_x = x1 > x0, guest_y = y1 > y0;
    x0 = ScaleEdge(x0, pass_res_q_);
    x1 = ScaleEdge(x1, pass_res_q_);
    y0 = ScaleEdge(y0, pass_res_q_);
    y1 = ScaleEdge(y1, pass_res_q_);
    if (REXCVAR_GET(ds2_scaled_min_viewport)) {  // ds2-nx: a one-pixel guest scissor keeps one host pixel
      if (guest_x && x1 <= x0) x1 = x0 + 1;
      if (guest_y && y1 <= y0) y1 = y0 + 1;
    }
  }
  x0 = std::clamp(x0, 0, int32_t(pass_width_));
  x1 = std::clamp(x1, 0, int32_t(pass_width_));
  y0 = std::clamp(y0, 0, int32_t(pass_height_));
  y1 = std::clamp(y1, 0, int32_t(pass_height_));
  if (x1 <= x0 || y1 <= y0) {
    return 2;
  }
  scissor = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
  return 0;
}

// Guard of sa2_native_framing_cache. The current computation against the saved one, byte by byte.
void DrawsDkImpl::CompareFraming(const VkViewport& viewport, const float ndc[4], const VkRect2D& scissor, uint32_t empty) {
  ++checked_framing_;
  const bool equal = empty == empty_framing_ &&
                     Equal(&viewport, &framing_viewport_, sizeof(viewport)) &&
                     Equal(ndc, framing_ndc_, sizeof(framing_ndc_)) &&
                     Equal(&scissor, &framing_scissor_, sizeof(scissor));
  if (!equal) {
    framing_cache_off_ = true;
    framing_cache_ = false;
    sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
    REXLOG_ERROR("[native] C6 cached framing: MISMATCH at check {} (empty {} / {}; viewport {},{} "
                 "{}x{} / {},{} {}x{}; scissor {},{} {}x{} / {},{} {}x{}). Switched off for the rest of the session: this "
                 "draw uses the recomputed one",
                 checked_framing_, empty, empty_framing_, viewport.x, viewport.y, viewport.width,
                 viewport.height, framing_viewport_.x, framing_viewport_.y, framing_viewport_.width,
                 framing_viewport_.height, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                 scissor.extent.height, framing_scissor_.offset.x, framing_scissor_.offset.y,
                 framing_scissor_.extent.width, framing_scissor_.extent.height);
  } else if (checked_framing_ == kFramingsToCheck) {
    SA2_REPORT_RING("[native] C6 cached framing: {} hits checked against the computation, 0 mismatches; still "
                "checking 1 in 4096", checked_framing_);
  }
}

/*
 * The util_height_ element for that pitch (sa2_native_util_height_memo). The pitch is the pass's and only
 * changes with the pass, so the last one is remembered. References to unordered_map elements are not
 * invalidated by insertion (only by erasure, and util_height_ never erases). Guard: the first
 * kUtilHeightToCheck reuses, then 1 in 4,096, also look it up in the map and must get the same element.
 */
uint32_t& DrawsDkImpl::UtilHeightFor(uint32_t pitch) {
  if (!util_height_memo_active_) {
    return util_height_[pitch];
  }
  if (util_height_memo_ == nullptr || pitch != util_height_memo_pitch_) {
    util_height_memo_ = &util_height_[pitch];
    util_height_memo_pitch_ = pitch;
    return *util_height_memo_;
  }
  const uint64_t n = ++util_height_memo_hits_;
  if (n <= kUtilHeightToCheck || (n & 4095) == 0) {
    uint32_t* const of_the_map = &util_height_[pitch];
    if (of_the_map != util_height_memo_) {
      util_height_memo_off_ = true;
      util_height_memo_active_ = false;
      util_height_memo_ = nullptr;
      sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
      REXLOG_ERROR("[native] C6 remembered usable height: MISMATCH (pitch {}, check {}): not the map's "
                   "element. Switched off for the rest of the session", pitch, n);
      return *of_the_map;
    }
    if (n == kUtilHeightToCheck) {
      SA2_REPORT_RING("[native] C6 remembered usable height: {} checks against the map, 0 mismatches; still "
                  "checking 1 in 4096", n);
    }
  }
  return *util_height_memo_;
}

/*
 * Guard of sa2_native_key_fast_pass. With the same render target bytes, the XXH3 must be pass_key_. Returns the computed
 * one, which is the one that decides for this draw.
 */
uint64_t DrawsDkImpl::CheckPassKey(const uint64_t keys[5], uint64_t n) {
  const uint64_t computed = XXH3_64bits(keys, sizeof(uint64_t) * 5);
  if (computed != pass_key_) {
    key_fast_off_pass_ = true;
    key_fast_pass_ = false;
    sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
    REXLOG_ERROR("[native] C6 pass key: MISMATCH (check {}: {:016X} computed, {:016X} stored). "
                 "Switched off for the rest of the session", n, computed, pass_key_);
  } else if (n == kKeysPassToCheck) {
    SA2_REPORT_RING("[native] C6 pass key: {} reused keys checked with XXH3, 0 mismatches; still "
                "checking 1 in 4096", n);
  }
  return computed;
}

// Every 10 s, how much the small per-draw savings save.
void DrawsDkImpl::ReportMinutiae() {
  const auto now = std::chrono::steady_clock::now();
  if (now - minutiae_report_ < std::chrono::seconds(10)) {
    return;
  }
  const bool first = minutiae_report_ == std::chrono::steady_clock::time_point{};
  minutiae_report_ = now;
  const uint64_t framings = framing_hits_report_ + framing_computations_report_;
  if (!first && framings) {
    SA2_REPORT_RING("[native] C6 small savings: cached framing in {} of {} draws ({:.1f} %; {}{}); "
                "pass key without XXH3 {} times ({}); remembered usable height {} times ({}); settings of "
                "color-less draws {}",
                framing_hits_report_, framings,
                100.0 * double(framing_hits_report_) / double(framings),
                framing_cache_off_ ? "SWITCHED OFF by the guard" : framing_cache_ ? "on" : "off",
                checked_framing_ >= kFramingsToCheck ? ", guard passed" : ", checking",
                keys_fast_pass_ - keys_fast_previous_pass_,
                key_fast_off_pass_ ? "SWITCHED OFF by the guard" : key_fast_pass_ ? "on" : "off",
                util_height_memo_hits_ - util_height_memo_previous_hits_,
                util_height_memo_off_ ? "SWITCHED OFF by the guard" : util_height_memo_active_ ? "on" : "off",
                cvars_per_frame_ ? "once per frame" : "on every draw");
  }
  framing_hits_report_ = 0;
  framing_computations_report_ = 0;
  keys_fast_previous_pass_ = keys_fast_pass_;
  util_height_memo_previous_hits_ = util_height_memo_hits_;
}

// The guard of the direct-mapped pipeline cache. The same key in the map (the usual path) must give the
// same VkPipeline as the slot. Otherwise it switches off and returns false. (deko3d: the same PipelineDk.)
bool DrawsDkImpl::CheckCellPipeline(uint64_t fingerprint, const PipelineKey& key, const PipelineDk* in_cell, uint64_t n) {
  ++pipelines_direct_checked_;
  const PipelineDk* of_the_map = nullptr;
  if (const auto it = pipelines_.find(fingerprint);
      it != pipelines_.end() && Equal(&it->second.first, &key, sizeof(key))) {
    // deko3d: the map holds the PipelineDk itself; one kept without shaders (a failure) is nullptr, as VK_NULL_HANDLE.
    of_the_map = PipelineOf(it->second.second);
  }
  if (of_the_map != in_cell) {
    pipelines_direct_off_ = true;
    pipelines_direct_ = false;
    sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
    REXLOG_ERROR("[native] C6 pipelines: MISMATCH between the direct cache and the map (check {}, VS {} PS {} "
                 "specialization {:08X}). Switched off for the rest of the session: the map rules",
                 n, key.vs, key.ps, key.specialization);
    return false;
  }
  if (n == kPipelinesToCheck) {
    SA2_REPORT_RING("[native] C6 pipelines: {} direct-cache hits checked against the map, 0 "
                "mismatches; still checking 1 in 4096", n);
  }
  return true;
}

/*
 * Measurement only (sa2_native_count_changes_pipeline). What changes at each vkCmdBindPipeline.
 *
 * BindPipeline costs 3.3-4.5 us per call in play (0.3-0.4 per draw), and on each one NVK copies all
 * the pipeline's fixed state (vk_dynamic_graphics_state_copy, ~80 groups). If the change is state only,
 * with dynamic state (vkCmdSet*) no new pipeline would be needed. Before touching anything, the counts
 * and kinds have to be known:
 *  - cull mode, topology within the same class, Z test/write/function, stencil, bias and restart are
 *    EDS1/EDS2, core in Vulkan 1.3: the console (API 1.3.354) already provides them without touching
 *    the SDK;
 *  - blending, color masks and topology of another class need VK_EXT_extended_dynamic_state3, which NVK
 *    exposes on Maxwell and the SDK enables.
 * The new key is compared with the last one bound in the same command buffer, field by field and
 * without XXH3. Blending, masks and depth are compared in canonical form (CanonicalState: only what
 * PipelineFor really reads). If only bits PipelineFor does not use differ, the pipeline is effectively the
 * same: no effect. Cost: an 80-byte memcmp and about 60 operations per bind (0.3-0.4 binds per draw). It
 * changes nothing.
 */
uint32_t DrawsDkImpl::ClassTopology(uint32_t topology) {
  switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
      return 1;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
      return 3;
    default:
      return 2;  // triangles: list, strip and fan
  }
}

// What PipelineFor really reads from a key's blending, masks and depth, with its same criterion
// (blendEnable per target; depthWriteEnable = test and write; the back face copies the front).
void DrawsDkImpl::CanonicalState(const PipelineKey& c, uint32_t blend[4], uint32_t& masks, uint32_t& depth) {
  masks = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    blend[i] = 0;
    if (!c.formats[i]) {
      continue;  // PipelineFor skips color targets not in the pass
    }
    const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
    masks |= mask << (i * 4);
    const uint32_t m = c.blend[i];
    const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
    const bool direct_alpha = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
    if (((mask & 0x7) && !color_direct) || ((mask & 0x8) && !direct_alpha)) {
      blend[i] = m;  // blendEnable: factors count (without blending Vulkan ignores them)
    }
  }
  uint32_t d = c.formats[4] ? c.depth : 0;  // without a depth target PipelineFor ignores it
  d &= ~0x8u;                                      // PipelineFor does not read bit 3
  if (!((d >> 1) & 0x1)) {
    d &= ~0x74u;  // without a Z test, write and function do not count
  }
  if (!(d & 0x1)) {
    d &= 0x77u;  // without stencil, its functions and operations do not count
  } else if (!((d >> 7) & 0x1)) {
    d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
  }
  depth = d;
}

// Classifies a vkCmdBindPipeline from Draw against the last key bound in this buffer.
// after_begin_pass: BeginPass had forgotten the bound pipeline (pipeline_bound_ set to
// VK_NULL_HANDLE).
void DrawsDkImpl::CountChangePipeline(const PipelineKey& new_entry, bool after_begin_pass) {
  uint64_t* const n = changes_pipeline_.data();
  ++n[kChangeBindings];
  n[kChangeAfterPass] += after_begin_pass ? 1 : 0;
  const PipelineKey& a = bound_key_;
  constexpr uint32_t kBitsTestAlpha = 0x2u | (0x7u << kSpecFunctionAlphaDisplacement);
  if (!bound_valid_key_) {
    ++n[kFirstChange];
  } else if (Equal(&a, &new_entry, sizeof(new_entry))) {
    ++n[kIdenticalChange];  // only after BeginPass: Vulkan kept the bound pipeline across passes
  } else if (a.vs != new_entry.vs || a.ps != new_entry.ps) {
    ++n[kChangeShaders];
  } else if (a.entry != new_entry.entry) {
    ++n[kChangeEntry];
  } else if (!Equal(a.formats, new_entry.formats, sizeof(a.formats))) {
    ++n[kChangeFormats];
  } else if (const uint32_t spec = a.specialization ^ new_entry.specialization; spec != 0) {
    ++n[(spec & ~kBitsTestAlpha) == 0                      ? kChangeSpecAlpha
        : (spec & ~(kBitsTestAlpha | kSpecZEarly)) == 0 ? kChangeSpecZEarly
                                                            : kChangeSpecOther];
  } else {
    // Same shaders, input, formats and specialization: only fixed pipeline state changes.
    uint32_t blend_a[4], blend_n[4], masks_a, masks_n, prof_a, prof_n;
    CanonicalState(a, blend_a, masks_a, prof_a);
    CanonicalState(new_entry, blend_n, masks_n, prof_n);
    const bool topology = a.topology != new_entry.topology;
    const bool class_value = ClassTopology(a.topology) != ClassTopology(new_entry.topology);
    const bool blend = !Equal(blend_a, blend_n, sizeof(blend_a));
    const bool masks = masks_a != masks_n;
    const uint32_t prof = prof_a ^ prof_n;
    const uint32_t rast = a.rasterization ^ new_entry.rasterization;
    if (!topology && !blend && !masks && !prof && !rast) {
      ++n[kChangeNoEffect];  // only bits PipelineFor does not read: effectively the same pipeline
      n[kChangeNoEffectAfterPass] += after_begin_pass ? 1 : 0;
    } else {
      const bool eds12 = !blend && !masks && !class_value;
      ++n[kChangeOnlyState];
      n[kChangeAfterStatePass] += after_begin_pass ? 1 : 0;
      n[kChangeEds12] += eds12 ? 1 : 0;
      n[kChangeEds12AfterPass] += (eds12 && after_begin_pass) ? 1 : 0;
      n[kFieldTopology] += topology ? 1 : 0;
      n[kFieldClassTopology] += class_value ? 1 : 0;
      n[kFieldBlend] += blend ? 1 : 0;
      n[kFieldMasks] += masks ? 1 : 0;
      n[kFieldZ] += (prof & 0x76u) ? 1 : 0;
      n[kFieldStencil] += (prof & ~0x76u) ? 1 : 0;
      n[kFieldFace] += (rast & 0x7u) ? 1 : 0;
      n[kFieldReset] += (rast & 0x8u) ? 1 : 0;
      n[kFieldBias] += (rast & 0x10u) ? 1 : 0;
    }
  }
  bound_key_ = new_entry;
  bound_valid_key_ = true;
}

// Every 20 s, what changes at each vkCmdBindPipeline of the ring (measurement only). Reads its cvar on
// every call: ReportPipelinesDirect calls it, once per frame.
void DrawsDkImpl::ReportChangesPipeline(std::chrono::steady_clock::time_point now) {
  const bool count = REXCVAR_GET(sa2_native_count_changes_pipeline);
  if (count != count_changes_pipeline_) {
    count_changes_pipeline_ = count;
    bound_valid_key_ = false;  // what was tracked while not counting cannot be compared
  }
  if (now - changes_report_ < std::chrono::seconds(20)) {
    return;
  }
  const bool first = changes_report_ == std::chrono::steady_clock::time_point{};
  const double seconds = std::chrono::duration<double>(now - changes_report_).count();
  changes_report_ = now;
  const uint64_t draws = drawn_ - changes_previous_draws_;
  const uint64_t frames = frame_ - changes_previous_frames_;
  changes_previous_draws_ = drawn_;
  changes_previous_frames_ = frame_;
  const std::array<uint64_t, kChangesN> n = changes_pipeline_;
  changes_pipeline_.fill(0);
  if (first || !count_changes_pipeline_ || !n[kChangeBindings] || !draws || !frames) {
    return;
  }
  const double bindings = double(n[kChangeBindings]);
  const double f = double(frames);
  const auto per_draw = [&](uint64_t x) { return double(x) / double(draws); };
  const auto pct = [&](uint64_t x) { return 100.0 * double(x) / bindings; };
  const uint64_t spec = n[kChangeSpecAlpha] + n[kChangeSpecZEarly] + n[kChangeSpecOther];
  SA2_REPORT_RING(
      "[native] C6 pipeline changes (measurement only): {} vkCmdBindPipeline in {:.0f} s ({:.3f} per "
      "draw, {:.1f} per frame; {} are the first of a pass) | no previous key {} | the SAME key after "
      "starting a pass {} ({:.1f} %) | shaders change {} ({:.1f} %) | other entry {} | other formats {} | "
      "specialization {} ({:.1f} %: alpha test {}, with early Z {}, other {}) | no effect (bits that "
      "PipelineFor does not read) {} ({:.1f} %) | STATE ONLY {} ({:.1f} %, {:.3f} per draw): EDS1/EDS2 without touching the SDK "
      "{}, the rest needs EDS3",
      n[kChangeBindings], seconds, per_draw(n[kChangeBindings]), bindings / f, n[kChangeAfterPass],
      n[kFirstChange], n[kIdenticalChange], pct(n[kIdenticalChange]), n[kChangeShaders], pct(n[kChangeShaders]),
      n[kChangeEntry], n[kChangeFormats], spec, pct(spec), n[kChangeSpecAlpha], n[kChangeSpecZEarly],
      n[kChangeSpecOther], n[kChangeNoEffect], pct(n[kChangeNoEffect]), n[kChangeOnlyState],
      pct(n[kChangeOnlyState]), per_draw(n[kChangeOnlyState]), n[kChangeEds12]);
  // Avoidable binds per frame with each fix, each one separately (those of the first draw of a pass are
  // not avoided by dynamic state or by the canonical key while BeginPass forgets the bound pipeline).
  constexpr double kUsPerBinding = 3.0;  // BindPipeline: 3.3-4.5 us per call in play
  const uint64_t canonical = n[kChangeNoEffect] - n[kChangeNoEffectAfterPass];
  const uint64_t eds12 = n[kChangeEds12] - n[kChangeEds12AfterPass];
  const uint64_t eds123 = n[kChangeOnlyState] - n[kChangeAfterStatePass];
  SA2_REPORT_RING(
      "[native] C6 pipeline changes, state-only fields: topology {} (of class {}), blend {}, masks {}, "
      "Z {}, stencil {}, face {}, restart {}, bias {}; {} of them the first of a pass | avoidable binds per "
      "frame (at ~3 us each): {:.1f} with the canonical key ({:.2f} ms), {:.1f} without forgetting the pipeline at "
      "pass start ({:.2f} ms), {:.1f} with EDS1/EDS2 ({:.2f} ms), {:.1f} with EDS1/EDS2/EDS3 ({:.2f} ms)",
      n[kFieldTopology], n[kFieldClassTopology], n[kFieldBlend], n[kFieldMasks], n[kFieldZ],
      n[kFieldStencil], n[kFieldFace], n[kFieldReset], n[kFieldBias], n[kChangeAfterStatePass],
      double(canonical) / f, double(canonical) / f * kUsPerBinding / 1000.0, double(n[kIdenticalChange]) / f,
      double(n[kIdenticalChange]) / f * kUsPerBinding / 1000.0, double(eds12) / f,
      double(eds12) / f * kUsPerBinding / 1000.0, double(eds123) / f, double(eds123) / f * kUsPerBinding / 1000.0);
}

// Every 10 s, the hit rate of the direct-mapped pipeline cache.
void DrawsDkImpl::ReportPipelinesDirect() {
  const auto now = std::chrono::steady_clock::now();
  ReportChangesPipeline(now);  // reads its cvar every frame and writes every 20 s
  TryPrewarm();       // starts the thread as soon as the library is loaded
  PrewarmedReport(now);  // every 10 s, if there is anything new, and its guard
  if (now - pipelines_direct_report_ < std::chrono::seconds(10)) {
    return;
  }
  const bool first = pipelines_direct_report_ == std::chrono::steady_clock::time_point{};
  pipelines_direct_report_ = now;
  const uint64_t hits = pipelines_direct_hits_ - pipelines_direct_previous_hits_;
  const uint64_t failures = pipelines_direct_failures_ - pipelines_direct_previous_failures_;
  pipelines_direct_previous_hits_ = pipelines_direct_hits_;
  pipelines_direct_previous_failures_ = pipelines_direct_failures_;
  if (!first && (hits || failures)) {
    SA2_REPORT_RING("[native] C6 pipelines: direct cache {} hits and {} misses ({:.1f} % of the "
                "lookups the single-entry shortcut does not resolve); {} checked against the map ({})",
                hits, failures, 100.0 * double(hits) / double(hits + failures),
                pipelines_direct_checked_,
                pipelines_direct_off_ ? "SWITCHED OFF by the guard"
                : pipelines_direct_checked_ >= kPipelinesToCheck ? "guard passed"
                                                                         : "checking");
  }
}

/*
 * The "indices" stage once rose from ~1.4 to ~2.6 us per draw without any change to its code. Four
 * cuts inside it (C6 substages 15-18), only on the timed draws (1 in 64): they cost four clock reads on
 * those draws, so the "indices" stage reads ~0.2-0.3 us higher than without them.
 */
void DrawsDkImpl::CutSubstage(size_t k, std::chrono::steady_clock::time_point& t) {
  if (!time_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
  ++sub_n_[k];
  t = now;
}

void DrawsDkImpl::Stage(size_t stage, std::chrono::steady_clock::time_point& mark) {
  if (!time_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  stages_ns_[stage] += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
  mark = now;
}

// Diagnostic sa2_native_diag_vertices_repeated: vertex bytes that repeat address, size, byte order
// and content within the same frame or relative to an earlier frame.
void DrawsDkImpl::NoteVerticesRepeated(uint64_t address, uint32_t bytes, uint32_t order, const uint8_t* data) {
  const auto before = std::chrono::steady_clock::now();
  const uint64_t fingerprint = XXH3_64bits(data, bytes);
  const uint64_t key =
      XXH3_64bits_withSeed(&address, sizeof(address), (uint64_t(bytes) << 2) | order);
  if (vertices_seen_.size() > 200000) {
    vertices_seen_.clear();  // memory cap of the diagnostic
  }
  VerticesSeen& seen = vertices_seen_[key];
  if (seen.fingerprint == fingerprint && seen.frame == frame_) {
    bytes_repeated_frame_ += bytes;
  } else if (seen.fingerprint == fingerprint && seen.frame != UINT64_MAX &&
             seen.frame < frame_) {
    bytes_equal_previous_ += bytes;
  }
  seen.frame = frame_;
  seen.fingerprint = fingerprint;
  bytes_recorded_ += bytes;
  if (before - repeated_report_ >= std::chrono::seconds(10)) {
    repeated_report_ = before;
    REXLOG_INFO("[native] ME repeated vertices (10 s): {:.1f} MB seen; same range and contents within the "
                "frame {:.1f} MB ({:.0f} %), same as in an earlier frame {:.1f} MB ({:.0f} %), "
                "{} ranges remembered",
                double(bytes_recorded_) / 1048576.0, double(bytes_repeated_frame_) / 1048576.0,
                100.0 * double(bytes_repeated_frame_) / double(std::max<uint64_t>(1, bytes_recorded_)),
                double(bytes_equal_previous_) / 1048576.0,
                100.0 * double(bytes_equal_previous_) / double(std::max<uint64_t>(1, bytes_recorded_)),
                vertices_seen_.size());
    bytes_recorded_ = bytes_repeated_frame_ = bytes_equal_previous_ = 0;
  }
  ns_hash_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count());
}

// sa2-nx render resolution: the used heights per pitch are host pixels (from the draws' host scissors): scaled with
// the targets, rounded up (a taller render area is always safe). Entries are never erased (UtilHeightFor's memo).
void DrawsDkImpl::NotifyRescale(uint32_t q_old, uint32_t q_new) {
  if (!q_old || q_old == q_new) return;
  for (auto& [pitch, height] : util_height_) {
    if (!height) continue;
    const uint64_t scaled = (uint64_t(height) * q_new + q_old - 1) / q_old;
    height = uint32_t(std::min<uint64_t>((scaled + 63u) & ~uint64_t(63u), UINT32_MAX));
  }
}

void DrawsDkImpl::NotifyGraphicsExternalState() {
  pipeline_bound_ = nullptr;
  state_tracker_.Reset();  // deko3d: whoever recorded in between may have bound shaders and state
  layout_bound_valid_ = false;
  bound_valid_key_ = false;
  sets_bound_ = false;
  ubo_bound_ = false;
  recorded_state_ = false;
  recorded_bindings_ = 0;
  stencil_recorded_valid_ = false;
  indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
}

void DrawsDkImpl::FinishPass() {
  if (active_pass_) {
    /*
     * Fallback path of the deferred sky. If nothing has emitted it yet (no transparent draw arrived, or the
     * pass closes because of a copy, a resolve or a submission), it is emitted here, inside the pass and
     * before vkCmdEndRenderPass. Losing the sky would be a visible and serious bug, so this is the last
     * place it can be, and it is always reached: the command buffer is only closed through SendWork
     * -> BeforeSend -> FinishPass.
     */
    if (pending_sky_) {
      EmitPostponedSky(pass_commands_, kSkyPerPassEnd);
    }
    const auto before_end = std::chrono::steady_clock::now();
    // deko3d: there is no pass to end. What vkCmdEndRenderPass's external dependency gave the next reader of the
    // targets (a sampling draw, a resolve, a copy) is a barrier: conservative, one per closed pass
    // (sa2_dk_barrier_pass_end), until the semantics bench says which readers need which.
    if (REXCVAR_GET(sa2_dk_barrier_pass_end)) {
      sa2::dk::Barrier(pass_commands_, sa2::dk::Hazard::RenderToSample);
    }
    if (stats_pass_ != UINT32_MAX) {
      context_->FinishStats(stats_pass_);
      stats_pass_ = UINT32_MAX;
    }
    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_end).count());
    const uint32_t closed_category = category_pass_;
    active_pass_ = false;
    // Just in case. A pending sky with the pass already closed cannot be recorded; it is counted so it
    // shows up in the report instead of disappearing silently.
    if (pending_sky_) {
      pending_sky_ = false;
      ++lost_sky_;
    }
    // As soon as the shadow pass closes, submit to the GPU. Once per frame: the flag keeps reopened passes
    // from submitting again. SendAndWait submits and keeps recording in the next slot; it waits for
    // nothing (the name is misleading).
    if (closed_category == kGpuShadows && !sent_after_shadows_ && context_ &&
        REXCVAR_GET(sa2_native_send_after_shadows)) {
      sent_after_shadows_ = true;
      context_->SendAndWait();
    }
  }
}

void DrawsDkImpl::BeforeSend() {
  drawn_at_send_ = drawn_;  // ds2_native_partial_send counts from the last submission, full or partial
  WaitUploads();  // deferred vertex copies, before flushing the mapping and submitting
  FinishPass();
  // Whatever is still on the texture bind thread, with its views, descriptors, barriers and copies, goes
  // into this upload buffer before it is closed (sa2_native_textures_binding_thread). It is the last thing
  // recorded: after the pass closes (which may emit the deferred sky), and meanwhile the thread has kept
  // binding.
  // (CollectBindings: no texture bind thread in deko3d, see "Texture binds on a separate thread".)
  // deko3d: dkMemBlockFlushCpuCache over what was written (CpuCached blocks; nothing for CpuUncached).
  if (used_upload_ && !coherent_upload_ && upload_memory_) {
    sa2::dk::FlushCpu(*upload_memory_, 0, uint32_t(used_upload_));
  }
  if (shared_separate_ && shared_used_ && !shared_coherent_ && shared_memory_) {
    sa2::dk::FlushCpu(*shared_memory_, 0, uint32_t(shared_used_));
  }
}

void DrawsDkImpl::UseSlot(uint32_t slot) {
  WaitUploads();  // already empty after BeforeSend; just in case, before switching buffers
  ReportCopies();  // every 10 s, who made the vertex copies and how long the ring waited
  ReportVertexCache();  // every 10 s, ds2_vertex_cache_mb
  diag_vs_constants_ = REXCVAR_GET(ds2_diag_vs_constants);
  if (diag_vs_constants_ && diag_vs_[0] != diag_vs_reported_[0] &&
      std::chrono::steady_clock::now() - diag_vs_time_ >= std::chrono::seconds(10)) {
    const uint64_t n = diag_vs_[0] - diag_vs_reported_[0];
    SA2_REPORT_RING("[native] VS constants (10 s): {} uploads, {:.0f} bytes each, {:.1f} float4 registers changed "
                    "each ({:.1f} MB copied)", n, double(diag_vs_[1] - diag_vs_reported_[1]) / double(n),
                    double(diag_vs_[2] - diag_vs_reported_[2]) / double(n),
                    double(diag_vs_[1] - diag_vs_reported_[1]) / 1048576.0);
    diag_vs_reported_ = diag_vs_; diag_vs_time_ = std::chrono::steady_clock::now();
  }
  if (diag7e3_[0] != diag7e3_reported_[0] &&
      std::chrono::steady_clock::now() - diag7e3_time_ >= std::chrono::seconds(10)) {
    diag7e3_time_ = std::chrono::steady_clock::now();
    SA2_REPORT_RING("[native] 7e3 targets (10 s): {} draws into them, {} write alpha, {} blend color with the "
                    "destination's alpha, {} blend at all",
                    diag7e3_[0] - diag7e3_reported_[0], diag7e3_[1] - diag7e3_reported_[1],
                    diag7e3_[2] - diag7e3_reported_[2], diag7e3_[3] - diag7e3_reported_[3]);
    diag7e3_reported_ = diag7e3_;
  }
  ReportCacheFetch();  // every 10 s, the per-fetch sampler table
  // (CheckBindingsOnChangeOfBuffer: no texture bind thread in deko3d, no texture is ever in flight.)
  // deko3d: buffer and memory are one block (an sa2::dk::Arena); the bump offset stays in used_upload_.
  const sa2::dk::Arena& s = uploads_[slot % uploads_.size()];
  upload_memory_ = &s.block();
  upload_real_size_ = s.Size();
  upload_data_ = s.Cpu(0);
  upload_address_ = s.Gpu(0);
  slot_current_ = uint32_t(slot % uploads_.size());  // this slot's UBO set
  used_upload_ = 0;
  if (shared_separate_) {
    const sa2::dk::Arena& c = shared_bufs_[slot_current_];
    shared_data_ = c.Cpu(0);
    shared_memory_ = &c.block();
    shared_real_size_ = c.Size();
    shared_used_ = 0;
  }
  ++epoch_upload_;
  ++frame_;
  CloseFrameOfTheGuardOfTheSky();
  // The upload buffer is reset here, so the recorded offsets are no longer valid. This also covers
  // SendAndWait, which goes through here.
  dedupe_.NewFrame(frame_);
  sent_after_shadows_ = false;  // the post-shadow submission is once per frame
  /*
   * Diagnostic cvars are read once per frame, not per draw.
   *
   * REXCVAR_GET is not a variable read: it is FLAGS_##name##_storage_(), an out-of-line function call
   * (cvar.h:343). Four of them were being made per draw (one of them per vertex binding, not per draw),
   * and one returned a std::string that was then compared. With 2,345 draws per frame that shows, and it
   * serves no purpose: none of them changes within a frame.
   */
  diag_vertices_repeated_ = REXCVAR_GET(sa2_native_diag_vertices_repeated);
  dedupe_active_ = REXCVAR_GET(sa2_native_dedupe_vertices);
  vertices_base_zero_ = REXCVAR_GET(sa2_native_vertices_base_zero) && !vertices_base_off_zero_;
  diag_stats_draw_ = REXCVAR_GET(sa2_native_stats_per_draw_s) > 0;
  area_util_ = PassAreaUtilCvar();  // defined in the Vulkan file's anonymous namespace: read by name
  // The small per-draw savings.
  framing_cache_ = REXCVAR_GET(sa2_native_framing_cache) && !framing_cache_off_;
  util_height_memo_active_ = REXCVAR_GET(sa2_native_util_height_memo) && !util_height_memo_off_;
  key_fast_pass_ = REXCVAR_GET(sa2_native_key_fast_pass) && !key_fast_off_pass_;
  cvars_per_frame_ = REXCVAR_GET(sa2_native_cvars_per_frame);
  ps_alpha_only_frame_ = PsAlphaOnly();
  no_ps_no_color_frame_ = NoPsNoColor();
  no_vegetation_ = REXCVAR_GET(sa2_shadows_no_vegetation);
  pipelines_direct_ = REXCVAR_GET(sa2_native_pipelines_direct) && !pipelines_direct_off_;
  canonical_key_ = REXCVAR_GET(sa2_native_canonical_key) && !canonical_off_key_;  // phase 0a
  {
    const bool new_value = REXCVAR_GET(sa2_native_pipeline_between_passes);  // phase 0b
    if (new_value != pipeline_between_passes_ || !pipeline_between_recorded_passes_) {
      pipeline_between_passes_ = new_value;
      pipeline_between_recorded_passes_ = true;
      REXLOG_INFO("[native] C6 pipeline at pass start: {} (frame {})",
                  new_value ? "the binding is KEPT (dynamic state phase 0b)" : "rebound, as before",
                  frame_);
    }
  }
  // (Dynamic state phases 1 and 2, the eds_mode_ choice per command buffer: in deko3d every piece of state is
  // dynamic, so there is no mode to choose; sa2_native_dynamic_state / _state3 have no effect.)
  // The cheap PCF bit also changes the pipeline: once per frame.
  {
    const bool new_value = REXCVAR_GET(sa2_native_pcf_cheap);
    if (new_value != pcf_cheap_) {
      pcf_cheap_ = new_value;
      REXLOG_INFO("[native] C2 shadow map sampling: {}",
                  new_value ? "single texel (cheap PCF)" : "3x3 half-texel pattern");
    }
  }
  // Opt-in SotFS shader experiments, independently selectable and part of PipelineKey.
  {
    const uint32_t mask = uint32_t(std::max(0, int(REXCVAR_GET(ds2_shader_light_mask)))) & 31u;
    if (shader_light_mask_ != mask) {
      shader_light_mask_ = mask;
      REXLOG_INFO("[native] DS2 shader light mask = {} (1 particles, 2 fog, 4 debris, 8 PS961 layer, 16 Majula layer)", mask);
    }
  }
  // The shadow map's own depth bias, once per frame.
  {
    const int32_t pending = std::clamp(int32_t(REXCVAR_GET(sa2_native_shadows_pending_bias)), 0, 100);
    if (pending != shadows_pending_bias_ || !shadows_recorded_bias_) {
      shadows_pending_bias_ = pending;
      shadows_recorded_bias_ = true;
      REXLOG_INFO("[native] C4 shadow map depth bias: slope {:.1f}{}",
                  double(pending) * 0.1, pending ? "" : " (off, as before)");
    }
  }
  // The 1/size bit changes the pipeline, so it is decided once per frame.
  {
    const bool new_value = REXCVAR_GET(sa2_native_inv_tex_size);
    if (new_value != inv_tex_size_) {
      inv_tex_size_ = new_value;
      REXLOG_INFO("[native] C2 texture size: {}",
                  new_value ? "by constant" : "asked from the texture");
    }
  }
  /*
   * The early depth test also changes the pipeline (a different module), so it is decided once per
   * frame, like the PCF and the 1/size.
   */
  {
    const bool new_value = REXCVAR_GET(sa2_native_z_early);
    if (new_value != z_early_ || !alternation_z_recorded_) {
      z_early_ = new_value;
      alternation_z_recorded_ = true;
      REXLOG_INFO("[native] C6 depth test: {} (frame {})",
                  new_value ? "BEFORE shading where Z is not written (early Z)"
                        : "after shading, as before",
                  frame_);
    }
  }
  skip_invisibles_ = REXCVAR_GET(sa2_native_skip_invisibles);
  indices_cache_active_ = REXCVAR_GET(sa2_native_indices_cache);
  // The deferred sky, also once per frame. If it is switched off mid-frame with one already deferred, the
  // pending one is still emitted through its usual path.
  {
    const bool new_value = REXCVAR_GET(sa2_native_postponed_sky);
    if (new_value != postponed_sky_ || !recorded_sky_) {
      postponed_sky_ = new_value;
      recorded_sky_ = true;
      REXLOG_INFO("[native] C6 sky: {} (frame {})",
                  new_value ? "DEFERRED until after the pass's opaque draws"
                        : "in its original place (first, over the empty Z)",
                  frame_);
    }
  }
  ReportZEarly();
  ReportPipelinesDirect();
  ReportMinutiae();
  ReportBaseZero();
  ReportSet4();  // (ControlSet4, ControlDrawNvk and ReportDrawNvk: NVK only)
  ReportCanonicalKey();  // Dynamic state, phase 0a
  ReportPipelineBetweenPasses();  // Dynamic state, phase 0b
  // (ReportDynamicState, phases 1 and 2: no EDS phases in deko3d)
  // (ReportBindings: no texture bind thread in deko3d.)
  ReportReuse();  // measurement only: new textures with a live one's content, every 10 s
  EvictTexturesIfDoesMissing();
  // (pool_textures_.PerFrame: region 06 has no pool member; sa2::dk::CreateImage sub-allocates on its own.)
  ++sends_;  // "C6 UBO constants" report
  if (use_ubo_) {
    ++sends_ubo_;
  }
  // What the prewarm thread actually compiles also goes into the cache and has to be saved.
  if (const uint32_t compiled = prewarmed_compiled_.load(std::memory_order_relaxed);
      compiled != prewarmed_counted_) {
    pipelines_no_save_ += compiled - prewarmed_counted_;
    prewarmed_counted_ = compiled;
  }
  if (retry_writer_.exchange(false, std::memory_order_relaxed)) {
    pipelines_no_save_ = std::max<uint32_t>(pipelines_no_save_, 1);  // retried in a minute, as on the ring
    list_no_save_ = 1;
  }
  // The pipeline cache is saved after 64 new pipelines, or after a minute with any new one: on the
  // Switch, exiting does not always reach the destructor.
  if (pipelines_no_save_ &&
      (pipelines_no_save_ >= 64 ||
       std::chrono::steady_clock::now() - cache_saved_ >= std::chrono::seconds(60))) {
    SaveCachePipelines();
  }
}

// Only the cache entries that use a view of those images. The views stay valid (they go with their
// image); what has to be redone is which slot each fetch constant gets.
void DrawsDkImpl::InvalidateImages(sa2::dk::ImageId a, sa2::dk::ImageId b) {
  std::array<std::pair<uint32_t, uint32_t>, 64> slots{};
  size_t n = 0;
  for (sa2::dk::ImageId image : {a, b}) {
    if (image == sa2::dk::kNoImage || (image == b && a == b)) {
      continue;
    }
    const auto it = views_per_image_.find(image);
    if (it == views_per_image_.end()) {
      continue;
    }
    for (uint64_t key : it->second) {
      const auto v = views_.find(key);
      if (v != views_.end() && v->second.image == image && n < slots.size()) {
        slots[n++] = {v->second.heap, v->second.slot};
      }
    }
  }
  if (!n) {
    return;
  }
  const auto affected = [&](const CacheSampler& c) {
    for (size_t i = 0; i < n; ++i) {
      if (slots[i].first == c.heap && slots[i].second == c.slot) {
        return true;
      }
    }
    return false;
  };
  for (CacheSampler& c : cache_samplers_) {
    if (affected(c)) {
      c.frame = UINT64_MAX;
      c.valid_until = 0;
    }
  }
  for (CacheSampler& c : cache_fetch_) {
    if (affected(c)) {
      c.frame = UINT64_MAX;
      c.valid_until = 0;
    }
  }
}

/*
 * How much GPU memory there is and how much is left.
 *
 * The cache limit was picked by eye twice (384 MB and then 192) because this data was not available.
 * With VK_EXT_memory_budget the driver reports the budget and the usage; without it, at least the heap
 * sizes are visible. It goes with the cache report, every 256 new textures.
 */
void DrawsDkImpl::NoteMemoryOfTheGpu() {
  // deko3d: no Vulkan heaps or VK_EXT_memory_budget. The GPU shares the process's memory: what the kernel says
  // the process has and uses (svcGetInfo), and what the base layer holds in images.
  uint64_t total = 0, used = 0;
  const bool known = ProcessMemoryDk(total, used);
  REXLOG_INFO("[native] C3 GPU memory [dk]: {}; {}; the texture cache holds {} MB of {} MB",
              known ? fmt::format("process {} MB used of {} MB", used >> 20, total >> 20)
                    : std::string("process memory unknown"),
              sa2::dk::ImagesSummary(), bytes_textures_ >> 20, textures_mb_max_);
  // Without this line there is no way to know whether the pool is working or fragmenting. A vegetation
  // counter once existed and was never printed. (deko3d: that is sa2::dk::ImagesSummary(), in the line above.)
}

// The `count` coldest textures of one heap: unused for kFramesNoUsageForDrop frames, so no pending work
// references them (the rule of the byte limit's eviction). Returns how many went.
uint32_t DrawsDkImpl::DropColdOfHeap(uint32_t heap, uint32_t count) {
  std::vector<std::pair<uint64_t, uint64_t>> cold;  // (last frame it was prepared, key)
  for (const auto& [key, texture] : textures_) {
    if (texture.image.image == sa2::dk::kNoImage || texture.needs_upload || texture.in_flight ||
        texture.frame == UINT64_MAX || texture.frame + kFramesNoUsageForDrop >= frame_ ||
        HeapOfTexture(texture) != heap) {
      continue;
    }
    cold.emplace_back(texture.frame, key);
  }
  if (cold.empty()) return 0;
  if (cold.size() > count) {
    std::partial_sort(cold.begin(), cold.begin() + count, cold.end());
    cold.resize(count);
  }
  std::unordered_set<sa2::dk::ImageId> images;
  for (const auto& [last, key] : cold) {
    const auto it = textures_.find(key);
    images.insert(it->second.image.image);
    bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
  }
  DropImages(images);
  released_textures_ += images.size();
  released_for_slots_ += images.size();
  return uint32_t(images.size());
}

/*
 * sa2_native_texture_slots_evict: the descriptor heaps are a limit of their own.
 *
 * The byte limit above counts texel data, and SA2's textures are small: 4,096 of them are far from 384 MB. So a
 * heap that is three quarters full sheds its coldest textures, 32 every 16 frames (each batch renews the texture
 * generation, so not every frame), until it is back under five eighths. And when a view found no slot at all, an
 * eighth of the heap goes at the next frame: that view showed the empty texture for one frame instead of for the
 * rest of the session.
 */
void DrawsDkImpl::EvictTexturesForSlots() {
  if (g_dk_slots_evict.active) {
    for (uint32_t heap = 0; heap < 3; ++heap) {  // the image heaps: 2D, 3D, cube
      const uint32_t capacity = SlotCapacity(heap);
      if (capacity < 8) continue;
      const bool full = (slots_full_heaps_ >> heap) & 1u;
      const uint32_t used = SlotsUsed(heap);
      if (used > capacity / 4 * 3) {
        draining_slots_[heap] = true;
      } else if (used <= capacity / 8 * 5) {
        draining_slots_[heap] = false;
      }
      uint32_t dropped = 0;
      if (full) {
        dropped = DropColdOfHeap(heap, std::max(capacity / 8, 8u));
      } else if (draining_slots_[heap] && (frame_ & 15) == 0) {
        dropped = DropColdOfHeap(heap, 32);
      }
      if (dropped) g_dk_slots_evict.Use();
      if (full && slots_full_logged_ < 16) {
        ++slots_full_logged_;
        REXLOG_WARN("[native] C3: descriptor heap {} had no free slot ({} of {} in use): {} cold textures released "
                    "(sa2_native_texture_slots_evict); {} textures remain",
                    heap, used, capacity - 1, dropped, textures_.size());
      }
    }
  }
  slots_full_heaps_ = 0;
  if (frame_ >= report_slots_frame_ + 600) {
    report_slots_frame_ = frame_;
    REXLOG_INFO("[native] C3 slots: 2D {} of {} in use, 3D {} of {}, cube {} of {}, samplers {} of {}; {} textures "
                "in the cache ({} MB of texels); {} released for slots so far, {} views found no slot "
                "(sa2_native_texture_slots_evict {})",
                SlotsUsed(0), SlotCapacity(0) - 1, SlotsUsed(1), SlotCapacity(1) - 1, SlotsUsed(2),
                SlotCapacity(2) - 1, SlotsUsed(3), SlotCapacity(3) - 1, textures_.size(),
                bytes_textures_ >> 20, released_for_slots_, slots_full_events_,
                g_dk_slots_evict.active ? "on" : "off");
  }
}

void DrawsDkImpl::EvictTexturesIfDoesMissing() {
  EvictTexturesForSlots();
  const uint64_t limit = uint64_t(std::max(textures_mb_max_, 0)) << 20;
  if (!limit) {
    return;
  }
  // Little by little, from 75 % of the limit. That is what avoids the stutter.
  if (bytes_textures_ > limit / 4 * 3) {
    DropSomeColdFew();
  }
  // Safety net: if it still goes over the limit (because almost everything is hot), one batch every 60
  // frames. With the above working, this should almost never trigger.
  if (bytes_textures_ > limit && frame_ >= attempt_eviction_ + 60) {
    attempt_eviction_ = frame_;
    DropTextures(limit / 4 * 3, kFramesNoUsageForDrop, "above the limit");
  }
}

// The kDropPerFrame oldest cold ones. Without sorting the whole cache: they are picked on the fly.
void DrawsDkImpl::DropSomeColdFew() {
  std::array<std::pair<uint64_t, uint64_t>, kDropPerFrame> chosen{};  // (frame, key)
  uint32_t how_many = 0;
  for (const auto& [key, texture] : textures_) {
    // Nor those with a bind in flight.
    if (texture.image.image == sa2::dk::kNoImage || texture.needs_upload || texture.in_flight ||
        texture.frame == UINT64_MAX ||
        texture.frame + kFramesNoUsageForDrop >= frame_) {
      continue;
    }
    if (how_many < kDropPerFrame) {
      chosen[how_many++] = {texture.frame, key};
      continue;
    }
    // Replaces the most recent of the chosen ones, if this one is older.
    uint32_t worst = 0;
    for (uint32_t i = 1; i < how_many; ++i) {
      if (chosen[i].first > chosen[worst].first) {
        worst = i;
      }
    }
    if (texture.frame < chosen[worst].first) {
      chosen[worst] = {texture.frame, key};
    }
  }
  if (how_many == 0) {
    return;  // all hot: let it grow rather than thrash
  }
  std::unordered_set<sa2::dk::ImageId> images;
  for (uint32_t i = 0; i < how_many; ++i) {
    auto it = textures_.find(chosen[i].second);
    if (it == textures_.end()) {
      continue;
    }
    images.insert(it->second.image.image);
    bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
  }
  DropImages(images);
  released_textures_ += images.size();
  released_little_to_little_ += images.size();
  if (frame_ >= warning_trickle_ + 600) {  // one warning every 600 frames, not one per texture
    warning_trickle_ = frame_;
    REXLOG_INFO("[native] C3: texture cache near the limit: cold textures dripping out ({} in total); "
                "{} textures and {} MB of {} MB remain",
                released_little_to_little_, textures_.size(), bytes_textures_ >> 20, textures_mb_max_);
  }
}

/*
 * The last resort, when the GPU has no more memory to give.
 *
 * Once nvMapCreate started failing even for 64 KB there was no way back: the driver ended up
 * quarantining ranges and the screen went black with the audio still playing. Before it gets to that,
 * half the texture cache is released.
 *
 * Normal eviction only touches what has been unused for 120 frames, because images are destroyed at
 * once and the GPU could still be reading them. Here that margin is not needed: it waits for the GPU to
 * finish everything in flight, and then none is in use. It costs a one-frame stutter; crashing costs
 * the game.
 */
bool DrawsDkImpl::DropTexturesPerMissingOfMemory() {
  if (context_) context_->GpuMemoryIsShort();  // sa2_native_resolved_retire_idle: its share goes at the next Swap
  if (textures_.empty() || bytes_textures_ == 0 || releasing_per_missing_of_memory_) {
    return false;  // the guard keeps an allocation inside the cleanup itself from re-entering here
  }
  releasing_per_missing_of_memory_ = true;
  bool something = false;
  if (context_ && context_->WaitGpuOfTheAll()) {
    something = DropTextures(bytes_textures_ / 2, 0, "OUT OF GPU MEMORY") > 0;
  }
  releasing_per_missing_of_memory_ = false;
  return something;
}

// Actually retires a group of images. First the views and their slots (pointed at the empty texture),
// then the images. Used by both the gradual and the batch eviction.
void DrawsDkImpl::DropImages(const std::unordered_set<sa2::dk::ImageId>& images) {
  ++generation_textures_;
  for (auto it = views_.begin(); it != views_.end();) {
    if (!images.count(it->second.image)) {
      ++it;
      continue;
    }
    // deko3d: the slot shows the empty texture again (its descriptor rewritten); there is no view to destroy.
    WriteImage(it->second.heap, it->second.slot, sa2::dk::View(empty_[it->second.heap].image));
    heaps_[it->second.heap].free.push_back(it->second.slot);
    views_per_image_.erase(it->second.image);  // all views of that image go
    it = views_.erase(it);
  }
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (!images.count(it->second.image.image)) {
      ++it;
      continue;
    }
    RemoveContentTexture(it->second, it->first);  // measurement only: sa2_native_diag_reuse
    DestroyImage(it->second.image);
    it = textures_.erase(it);
  }
}

// Evicts unused textures until below `goal`. Returns the bytes freed.
uint64_t DrawsDkImpl::DropTextures(uint64_t goal, uint64_t age_min, const char* reason) {
  std::vector<std::pair<uint64_t, uint64_t>> candidate;  // (last frame it was prepared, key)
  for (const auto& [key, texture] : textures_) {
    // Nor those with a bind in flight (their image has no memory yet).
    if (texture.image.image != sa2::dk::kNoImage && !texture.needs_upload && !texture.in_flight &&
        texture.frame != UINT64_MAX &&
        texture.frame + age_min < frame_) {
      candidate.emplace_back(texture.frame, key);
    }
  }
  std::sort(candidate.begin(), candidate.end());
  const uint64_t before = bytes_textures_;
  std::unordered_set<sa2::dk::ImageId> images;
  for (const auto& [last, key] : candidate) {
    if (bytes_textures_ <= goal) {
      break;
    }
    auto it = textures_.find(key);
    images.insert(it->second.image.image);
    bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
  }
  if (images.empty()) {
    return 0;
  }
  DropImages(images);
  released_textures_ += images.size();
  REXLOG_INFO("[native] C3: texture cache {} ({} MB): {} dropped, unused for more than {} frames "
              "({} MB); {} textures and {} MB remain ({} released in total)",
              reason, before >> 20, images.size(), age_min,
              (before - bytes_textures_) >> 20, textures_.size(), bytes_textures_ >> 20, released_textures_);
  return before - bytes_textures_;
}

// ZCULL: clears a depth image by opening a pass with loadOp = CLEAR. Needed for images created without
// TRANSFER_DST (the only ones the driver gives a ZCULL plane), because vkCmdClearDepthStencilImage
// requires that usage (VUID-vkCmdClearDepthStencilImage-pRanges-02660). It costs no extra GPU time: it
// is exactly how NVK implements that command internally (nvk_cmd_clear.c, clear_image opens a pass with
// loadOp = CLEAR).
// deko3d: no render pass and no usage rule. The depth target alone is bound, the scissor covers the area (host
// pixels) and dkCmdBufClearDepthStencil clears inside it (what loadOp = CLEAR did over the render area, both
// aspects). The clear respects the scissor: deko3d sets ClearBufferFlags::Scissor once at start-up
// (source/maxwell/gpu_3d_base.cpp:69); the bench reads a partial clear back to confirm it on the console. The render targets and the scissor the draws had recorded are no longer bound:
// NotifyGraphicsExternalState (the Vulkan version did not need it: its pass ended with its own state).
bool DrawsDkImpl::ClearDepthInPass(DkCmdBuf commands, const ImageNativeDk& image, float depth, uint32_t stencil, const VkRect2D* area) {
  if (commands == nullptr || !sa2::dk::Alive(image.image) || !image.width ||
      !image.height || !SamplesSupported(image)) {
    return false;
  }
  if (area && (!area->extent.width || !area->extent.height || area->offset.x < 0 ||
      area->offset.y < 0 || uint32_t(area->offset.x) + area->extent.width > image.width ||
      uint32_t(area->offset.y) + area->extent.height > image.height)) return false;
  FinishPass();  // a pass cannot be opened inside another
  const DkImageView view = sa2::dk::View(image.image);
  dkCmdBufBindRenderTargets(commands, nullptr, 0, &view);
  // sa2-nx render resolution: the area in host pixels.
  const DkScissor scissor =
      ScissorDk(area ? ScaleRect(*area, image.scale_q) : VkRect2D{{0, 0}, {HostWidth(image), HostHeight(image)}});
  dkCmdBufSetScissors(commands, 0, &scissor, 1);
  const bool with_stencil = sa2::dk::HasStencil(sa2::dk::FromVk(image.format));
  dkCmdBufClearDepthStencil(commands, true, depth, with_stencil ? 0xFF : 0, uint8_t(stencil));
  NotifyGraphicsExternalState();
  return true;
}

// Preserve depth exactly: LOAD both aspects, then clear ONLY stencil. The
// caller proves the guest operation is a full-mask constant stencil clear.
// Unlike ClearDepthInPass this never uses a depth CLEAR load operation.
// deko3d: dkCmdBufClearDepthStencil with clearDepth = false and the full stencil mask (vkCmdClearAttachments
// with the stencil aspect only).
bool DrawsDkImpl::ClearStencilInPass(DkCmdBuf commands, const ImageNativeDk& image, uint32_t stencil, const VkRect2D& area) {
  if (commands == nullptr || !sa2::dk::Alive(image.image) ||
      !image.width || !image.height ||
      !area.extent.width || !area.extent.height || area.offset.x < 0 ||
      area.offset.y < 0 || stencil > 255u || !context_ ||
      !SamplesSupported(image) ||
      (image.format != VK_FORMAT_D16_UNORM_S8_UINT &&
       image.format != VK_FORMAT_D24_UNORM_S8_UINT &&
       image.format != VK_FORMAT_D32_SFLOAT_S8_UINT)) return false;
  const uint32_t x = uint32_t(area.offset.x), y = uint32_t(area.offset.y);
  // Subtraction after validating offsets avoids unsigned addition overflow.
  if (x >= image.width || y >= image.height ||
      area.extent.width > image.width - x ||
      area.extent.height > image.height - y) return false;
  const uint64_t generation = context_->GenerationCommands();
  FinishPass();
  // Closing a shadow pass may submit/rotate WORK. Never record into the old
  // handle supplied by the caller; it must close the pass before acquiring it.
  if (context_->GenerationCommands() != generation ||
      context_->CommandsWork() != commands) return false;
  const DkImageView view = sa2::dk::View(image.image);
  dkCmdBufBindRenderTargets(commands, nullptr, 0, &view);
  // sa2-nx render resolution: the area in host pixels.
  const DkScissor scissor = ScissorDk(ScaleRect(area, image.scale_q));
  dkCmdBufSetScissors(commands, 0, &scissor, 1);
  dkCmdBufClearDepthStencil(commands, false, 0.0f, 0xFF, uint8_t(stencil));
  // PassFor supplies external read/write dependencies for LOAD and subsequent
  // sampling/resolves; invalidate the draw-side bindings after this extra pass.
  // (deko3d: the dependencies are the barriers of whoever reads it next, sa2::dk::Barrier.)
  NotifyGraphicsExternalState();
  return true;
}

// depth-only twin of ClearStencilInPass (mode-4 redirected clear rectangles).
bool DrawsDkImpl::ClearOnlyDepthInPass(DkCmdBuf commands, const ImageNativeDk& image, float depth, const VkRect2D& area) {
  if (commands == nullptr || !sa2::dk::Alive(image.image) ||
      !image.width || !image.height ||
      !area.extent.width || !area.extent.height || area.offset.x < 0 ||
      area.offset.y < 0 || !(depth >= 0.0f && depth <= 1.0f) || !context_ ||
      !SamplesSupported(image) ||
      (image.format != VK_FORMAT_D16_UNORM_S8_UINT &&
       image.format != VK_FORMAT_D24_UNORM_S8_UINT &&
       image.format != VK_FORMAT_D32_SFLOAT_S8_UINT)) return false;
  const uint32_t x = uint32_t(area.offset.x), y = uint32_t(area.offset.y);
  // Subtraction after validating offsets avoids unsigned addition overflow.
  if (x >= image.width || y >= image.height ||
      area.extent.width > image.width - x ||
      area.extent.height > image.height - y) return false;
  const uint64_t generation = context_->GenerationCommands();
  FinishPass();
  // Closing a shadow pass may submit/rotate WORK. Never record into the old
  // handle supplied by the caller; it must close the pass before acquiring it.
  if (context_->GenerationCommands() != generation ||
      context_->CommandsWork() != commands) return false;
  const DkImageView view = sa2::dk::View(image.image);
  dkCmdBufBindRenderTargets(commands, nullptr, 0, &view);
  // sa2-nx render resolution: the area in host pixels.
  const DkScissor scissor = ScissorDk(ScaleRect(area, image.scale_q));
  dkCmdBufSetScissors(commands, 0, &scissor, 1);
  dkCmdBufClearDepthStencil(commands, true, depth, 0, 0);
  // PassFor supplies external read/write dependencies for LOAD and subsequent
  // sampling/resolves; invalidate the draw-side bindings after this extra pass.
  NotifyGraphicsExternalState();
  return true;
}

// sa2_native_clear_area_util. Clears only the `area` rectangle of a color image with a
// loadOp = CLEAR pass: what NVK does internally for vkCmdClearColorImage (nvk_cmd_clear.c), over that
// rectangle. What lies outside is not touched (Vulkan only loads and stores the renderArea).
// deko3d: the color target alone, the scissor over the rectangle and dkCmdBufClearColor (VkClearColorValue is
// the 16 bytes it takes: float, int or uint by the target's format).
bool DrawsDkImpl::ClearColorInPass(DkCmdBuf commands, const ImageNativeDk& image, const VkClearColorValue& color, const VkRect2D& area) {
  if (commands == nullptr || !sa2::dk::Alive(image.image) || !area.extent.width || !area.extent.height ||
      !SamplesSupported(image) ||
      area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) + area.extent.width > image.width ||
      uint32_t(area.offset.y) + area.extent.height > image.height) {
    return false;
  }
  FinishPass();  // a pass cannot be opened inside another
  const DkImageView view = sa2::dk::View(image.image);
  const DkImageView* colors[1] = {&view};
  dkCmdBufBindRenderTargets(commands, colors, 1, nullptr);
  // sa2-nx render resolution: the area in host pixels.
  const DkScissor scissor = ScissorDk(ScaleRect(area, image.scale_q));
  dkCmdBufSetScissors(commands, 0, &scissor, 1);
  dkCmdBufClearColor(commands, 0, DkColorMask_RGBA, &color);
  NotifyGraphicsExternalState();
  return true;
}

// Fica de fora (deko3d): ForgetView (Vulkan file line 6963, sa2_native_framebuffers_forget_views). There are
// no view objects and no framebuffers to forget; DrawsDk has no ForgetView.

void DrawsDkImpl::ForgetImage(sa2::dk::ImageId image) {
  ++generation_textures_;  // the sampler cache could point to a retired view
  const auto index = views_per_image_.find(image);  // without walking views_
  if (index == views_per_image_.end()) {
    return;
  }
  for (uint64_t key : index->second) {
    const auto it = views_.find(key);
    if (it == views_.end() || it->second.image != image) {
      continue;
    }
    WriteImage(it->second.heap, it->second.slot, sa2::dk::View(empty_[it->second.heap].image));
    heaps_[it->second.heap].free.push_back(it->second.slot);
    views_.erase(it);  // (deko3d: no view object to destroy)
  }
  views_per_image_.erase(index);
}

StatsDraws DrawsDkImpl::Stats() const {
  StatsDraws e;
  e.drawn = drawn_;
  e.rejected = rejected_;
  e.pipelines = pipelines_.size();
  e.textures = textures_.size();
  e.uploads_texture = uploads_texture_;
  e.uploaded_megabytes = megabytes_bytes_ >> 20;
  e.megabytes_textures = bytes_textures_ >> 20;
  e.ms_pipelines = ns_pipelines_ / 1000000;
  e.passes = started_passes_;
  e.full_sends = full_sends_;
  e.ns_full_sends = ns_full_sends_;
  e.bytes_vertices = bytes_vertices_;
  e.dedupe_hits = dedupe_.hits();
  e.dedupe_bytes = dedupe_.bytes_saved();
  e.dedupe_collisions = dedupe_.collisions();
  e.bytes_indices = bytes_indices_uploaded_;
  e.samplers = samplers_prepared_;
  e.samplers_cache = samplers_cache_ + samplers_cache_fetch_;
  e.ns_passes = ns_passes_;
  e.ns_vertices = ns_vertices_;
  e.computed_inputs = computed_inputs_;
  e.ns_inputs = ns_inputs_;
  e.reused_inputs = reused_inputs_ + inputs_cache_hits_;
  e.passes_per_generation = passes_per_generation_;
  e.passes_per_target = passes_per_target_;
  e.resumed_passes = resumed_passes_;
  e.ns_render_pass = ns_render_pass_;
  e.texels_passes = texels_passes_;
  e.texels_per_category = texels_per_category_;
  e.draws_per_category = draws_per_category_;
  e.draws_ps_useless = draws_ps_useless_;
  e.draws_ps_required = draws_ps_required_;
  e.shadows_active_alpha = shadows_active_alpha_;
  e.shadows_off_alpha = shadows_off_alpha_;
  e.triangles_per_category = triangles_per_category_;
  e.passes_per_category = passes_per_category_;
  e.sends = sends_;
  e.sends_ubo = sends_ubo_;
  e.shared_looked = shared_looked_;
  e.shared_changed = shared_changed_;
  e.bytes_repeated_frame = bytes_repeated_frame_;
  e.bytes_equal_previous = bytes_equal_previous_;
  e.ns_hash_vertices = ns_hash_vertices_;
  e.causes.assign(causes_.begin(), causes_.end());
  e.stages_ns = stages_ns_;
  e.scene_with_discard = scene_with_discard_;
  e.scene_no_discard = scene_no_discard_;
  e.drawn_timed = drawn_timed_;
  e.vegetation_soon = draws_vegetation_soon_;
  std::sort(e.causes.begin(), e.causes.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  return e;
}

bool DrawsDkImpl::Reject(uint32_t cause, const char* text) {
  ++rejected_;
  ++causes_[cause];
  last_cause_ = cause;
  ::g_banjo_last_reject_cause = cause;
  Warn(cause, text);
  return false;
}

void DrawsDkImpl::Warn(uint32_t cause, const char* text) {
  if (warned_.insert(cause).second) {
    REXLOG_WARN("[native] C6: {} (cause {})", text, cause);
  }
}

// --- Texture binds on a separate thread (sa2_native_textures_binding_thread) ----------------------------------
// Fica de fora (deko3d): the whole thread (Vulkan file lines 7109-7704: DecideBindings, TurnOffBindings,
// EnqueueBinding, LoopBindings, StopBindings, WaitBindings, CreateTextureInThread, SlotViewInFlight, PostponeCopy,
// MoveViewsInFlight, CollectBindings, CheckBindingsOnChangeOfBuffer, ReportBindings). It existed because
// vkBindImageMemory makes two ioctls per texture (VA reservation and mapping, ~0.6 ms of stalled ring each).
// sa2::dk::CreateImage gives an image its memory with dkImageInitialize over a sub-allocated block: no ioctl, so
// there is nothing slow to move off the ring. Region 06 drops its state the same way (Texture::in_flight stays 0).
// sa2_native_textures_binding_thread has no effect.


void DrawsDkImpl::WarnOtherThreadCopies(const char* where) {
  if (!copies_other_warned_thread_) {
    copies_other_warned_thread_ = true;
    REXLOG_ERROR("[native] C6: {} from a thread other than the one that enqueues vertex copies", where);
  }
}

bool DrawsDkImpl::EnqueueCopy(const WorkCopy& work) {
  if (copies_no_thread_) {
    // Thread switched off by a MISMATCH (or it could not be created): the copy goes on the ring.
    ++copies_inf_.in_line;
    copies_inf_.bytes_in_line += uint64_t(work.words) * 4;
    return false;
  }
  if (!copies_thread_.joinable()) {
    copies_producer_ = std::this_thread::get_id();
    copies_phase_ = copies_requested_help_ ? kCopiesLooking : kCopiesNoHelp;
    const int ring_core = RexSwitchCurrentCore();  // asked by the ring, which creates the thread
    try {
      copies_thread_ = std::thread([this, ring_core] {
        rex::thread::set_current_thread_name("SA2 vertex copies");
        // sa2_native_uploads_thread_priority (see the cvar).
        const int32_t priority = REXCVAR_GET(sa2_native_uploads_thread_priority);
        const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
        // sa2_native_uploads_thread_core (see the cvar).
        const int32_t request = REXCVAR_GET(sa2_native_uploads_thread_core);
        const int core = request == -2 ? (ring_core == 2 ? 1 : 2) : int(request);
        const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
        REXLOG_INFO("[native] C6: vertex-copy thread at priority {:#x}{}; preferred core {}{} (the ring runs "
                    "on core {}), now on core {}",
                    priority, priority_ok ? "" : " (NOT accepted: keeps the one from its creation)", core,
                    core_ok ? "" : " (NOT accepted)", ring_core, RexSwitchCurrentCore());
        LoopCopies();
      });
    } catch (const std::system_error& error) {
      copies_no_thread_ = true;
      REXLOG_ERROR("[native] C6: could not create the vertex-copy thread ({}): copies go on the ring",
                   error.what());
      return false;
    }
    REXLOG_INFO("[native] C6: vertex copies on a separate thread (sa2_native_uploads_thread); the ring copies them "
                "itself when waiting for them (sa2_native_uploads_help) = {}",
                copies_requested_help_
                    ? fmt::format("YES, in chunks of {}; LOOKING phase during the first {} waits with pending "
                                  "copies",
                                  kCopiesPerChunk, kWaitsCopiesLooking)
                    : std::string("no, waits for the thread as before"));
  } else if (std::this_thread::get_id() != copies_producer_) {
    WarnOtherThreadCopies("EnqueueCopy");
  }
  const size_t written = written_copies_.load(std::memory_order_relaxed);
  if (written - done_copies_.load(std::memory_order_acquire) >= copies_.size()) {
    ++copies_in_line_;
    ++copies_inf_.in_line;
    copies_inf_.bytes_in_line += uint64_t(work.words) * 4;
    return false;
  }
  copies_[written & (copies_.size() - 1)] = work;
  written_copies_.store(written + 1);
  ++copies_inf_.queued;
  copies_inf_.bytes_queued += uint64_t(work.words) * 4;
  // Wake-up every kCopiesPerWarning copies: waking the thread as soon as it slept woke it on almost every
  // copy, and every wake-up goes through the kernel (1.4 % of the ring and almost all of the copy thread's
  // CPU). Whatever is left without a wake-up is collected by WaitUploads.
  if (((written + 1) & (kCopiesPerWarning - 1)) == 0) {
    WakeCopies();
  }
  return true;
}

void DrawsDkImpl::WakeCopies() {
  bool warn;
  {
    std::lock_guard<std::mutex> latch(copies_mutex_);
    warn = copies_sleeping_;
  }
  if (warn) {
    copies_cv_.notify_one();
  }
}

// Fence measurement only.
size_t DrawsDkImpl::PendingCopies() const {
  if (!copies_thread_.joinable()) {
    return 0;
  }
  const size_t written = written_copies_.load(std::memory_order_relaxed);
  const size_t done = done_copies_.load(std::memory_order_relaxed);
  return written > done ? written - done : 0;
}

void DrawsDkImpl::WaitUploads() {
  if (!copies_thread_.joinable()) {
    return;
  }
  const bool producer = std::this_thread::get_id() == copies_producer_;
  if (!producer) {
    WarnOtherThreadCopies("WaitUploads");
  }
  const size_t goal = written_copies_.load(std::memory_order_relaxed);
  if (done_copies_.load(std::memory_order_acquire) == goal) {
    if (producer) {
      CheckCopies(goal);  // what the thread did since the last wait
    }
    return;
  }
  const auto before = std::chrono::steady_clock::now();
  // What the thread had not taken yet on arrival (while observing, what the ring would have copied).
  copies_inf_.no_take += goal - std::min(goal, taken_copies_.load(std::memory_order_acquire));
  uint64_t ns_helping = 0;
  uint64_t helped = 0;
  if (producer && copies_phase_ == kCopiesApplying) {
    // 1. What the thread has not taken yet, the ring copies in batches: if the thread is running, they
    //    share it.
    uint64_t bytes = 0;
    while (TakeAndCopy(true, helped, bytes)) {
    }
    const auto after_help = std::chrono::steady_clock::now();
    ns_helping = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_help - before).count());
    helped_copies_ += helped;
    copies_bytes_helped_ += bytes;
    copies_ns_helping_ += ns_helping;
    copies_inf_.helped += helped;
    copies_inf_.bytes_helped += bytes;
    copies_inf_.ns_helping += ns_helping;
    if (helped) {
      ++copies_inf_.waits_with_help;
    }
    // 2. What is missing is the batch the thread is working on: wait on its mutex, which lends the thread
    //    the ring's priority until it releases it.
    if (done_copies_.load(std::memory_order_acquire) != goal) {
      {
        std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
        if (done_copies_.load(std::memory_order_acquire) != goal) {
          RepairCopies(goal, "with the chunk lock the thread has nothing in hand and copies are still missing");
        }
      }
      const uint64_t ns_chunk = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - after_help)
                                             .count());
      ++copies_inf_.waits_chunk;
      copies_inf_.ns_chunk += ns_chunk;
      copies_inf_.ns_chunk_worst = std::max(copies_inf_.ns_chunk_worst, ns_chunk);
    }
  } else {
    WaitCopiesAsAlways(goal);
  }
  if (producer) {
    CheckCopies(goal);
    if (copies_phase_ == kCopiesLooking && ++copies_waits_looking_ >= kWaitsCopiesLooking) {
      copies_phase_ = kCopiesApplying;
      REXLOG_INFO("[native] C6: vertex copies: {} waits with pending copies in the LOOKING phase and {} copies "
                  "checked, all with their mark and the counters matching. Moves to the APPLYING phase: the ring "
                  "copies what the thread has not taken and waits for the chunk in progress with its lock",
                  copies_waits_looking_, checked_copies_);
    }
  }
  ++waits_copies_;
  const uint64_t ns_total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before)
                                         .count());
  // The waiting time now counts only the wait; what the ring copies is counted separately (as helping).
  const uint64_t ns_wait = ns_total > ns_helping ? ns_total - ns_helping : 0;
  ns_waiting_copies_ += ns_wait;
  ++copies_inf_.waits;
  copies_inf_.ns_wait += ns_wait;
  copies_inf_.ns_wait_worst = std::max(copies_inf_.ns_wait_worst, ns_wait);
  // And to the "[hitch] ring" line (sa2_waits_hitch.h). This is only reached when the ring really
  // waits, a few times per frame: the fetch_add calls do not show.
  sa2::waits::g_ns_waiting_copies.fetch_add(ns_wait, std::memory_order_relaxed);
  sa2::waits::g_waits_copies.fetch_add(1, std::memory_order_relaxed);
  sa2::waits::g_ns_helping_copies.fetch_add(ns_helping, std::memory_order_relaxed);
  sa2::waits::g_helped_copies.fetch_add(helped, std::memory_order_relaxed);
}

// The plain wait (observing and no-help phases, or when another thread waits): the ring sleeps until the
// thread finishes. After 2 s it no longer keeps waiting forever (the earlier hang): it takes the batch
// mutex, which lends its priority to the thread if it is copying, and if copies are still missing with
// the thread out of its loop, the ring does them and switches the thread off.
void DrawsDkImpl::WaitCopiesAsAlways(size_t goal) {
  bool a_time = false;
  {
    std::unique_lock<std::mutex> latch(copies_mutex_);
    if (copies_sleeping_) {
      copies_cv_.notify_one();  // a wake-up EnqueueCopy skipped (it only wakes every kCopiesPerWarning copies)
    }
    copies_waiting_ = true;
    a_time = done_copies_cv_.wait_for(latch, std::chrono::seconds(2), [this, goal] {
      return done_copies_.load(std::memory_order_acquire) == goal;
    });
    copies_waiting_ = false;
  }
  ++copies_inf_.whole_waits;
  if (a_time) {
    return;
  }
  // Should never happen: diagnostic for a hang once seen in the menus.
  REXLOG_ERROR("[native] C6: the ring thread has been waiting 2 s for vertex copies: goal {}, done {}, "
               "taken {}, written {}, copied by the thread {}",
               goal, done_copies_.load(std::memory_order_acquire),
               taken_copies_.load(std::memory_order_acquire), written_copies_.load(std::memory_order_acquire),
               copies_progress_.load(std::memory_order_relaxed));
  std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
  if (done_copies_.load(std::memory_order_acquire) != goal) {
    RepairCopies(goal, "2 s waiting for the thread and, with the thread outside its loop, copies are still missing");
  }
}

// Takes the next untaken batch (up to kCopiesPerChunk copies, in order) and copies it. Used by the
// thread, holding copies_chunk_mutex_, and by the ring when helping. false = nothing was left to take.
bool DrawsDkImpl::TakeAndCopy(bool ring, uint64_t& copies, uint64_t& bytes) {
  const size_t written = written_copies_.load(std::memory_order_acquire);
  size_t taken = taken_copies_.load(std::memory_order_acquire);
  size_t n = 0;
  do {
    if (taken >= written) {
      return false;
    }
    n = std::min<size_t>(written - taken, kCopiesPerChunk);
  } while (!taken_copies_.compare_exchange_weak(taken, taken + n, std::memory_order_acq_rel,
                                                  std::memory_order_acquire));
  const size_t mask = copies_.size() - 1;
  for (size_t i = taken; i < taken + n; ++i) {
    const WorkCopy& work = copies_[i & mask];
    CopyVertices(work);
    bytes += uint64_t(work.words) * 4;
    copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
    if (!ring) {
      copies_progress_.store(i + 1, std::memory_order_relaxed);  // for the WaitUploads diagnostic
    }
  }
  copies += n;
  done_copies_.fetch_add(n, std::memory_order_acq_rel);  // publishes the copies and their marks
  return true;
}

// The guard, on each wait and with everything finished (the ring does not queue while waiting and the
// thread has nothing half taken): what was queued since the previous one has its mark and the counters
// add up. Ring only.
void DrawsDkImpl::CheckCopies(size_t goal) {
  if (goal == verified_copies_ || copies_no_thread_) {
    return;
  }
  const size_t mask = copies_.size() - 1;
  const size_t from =
      std::max(verified_copies_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
  size_t no_mark = 0;
  size_t first = 0;
  for (size_t i = from; i < goal; ++i) {
    if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
      if (!no_mark) {
        first = i;
      }
      ++no_mark;
    }
  }
  checked_copies_ += goal - from;
  copies_inf_.checked += goal - from;
  const size_t taken = taken_copies_.load(std::memory_order_acquire);
  const size_t done = done_copies_.load(std::memory_order_acquire);
  if (!no_mark && taken == goal && done == goal) {
    verified_copies_ = goal;
    return;
  }
  const std::string reason = no_mark ? fmt::format("{} of {} copies without their mark (the first one, copy {})", no_mark,
                                                     goal - from, first)
                                       : std::string("all with their mark, but the counters do not match");
  std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);  // the thread, out of its copy loop
  if (copies_phase_ == kCopiesApplying && taken == goal && done == goal) {
    // With the counters right, the suspect is the help path: redo whatever is missing and go back to the
    // plain wait.
    const size_t redone = RedoNoMark(goal);
    verified_copies_ = goal;
    ++copies_differences_;
    copies_phase_ = kCopiesNoHelp;
    sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
    REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} ({} enqueued, counters matching; {} redone "
                 "on the ring before submitting). Ring help SWITCHED OFF for the rest of the session: waits for the thread "
                 "as before",
                 reason, goal, redone);
    return;
  }
  RepairCopies(goal, reason);
}

// Copies on the ring whatever was queued up to `goal` (since the last check, and at most one lap of
// the queue) that lacks its mark, and marks it. With copies_chunk_mutex_ held. Returns how many.
size_t DrawsDkImpl::RedoNoMark(size_t goal) {
  const size_t mask = copies_.size() - 1;
  const size_t from =
      std::max(verified_copies_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
  size_t redone = 0;
  for (size_t i = from; i < goal; ++i) {
    if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
      CopyVertices(copies_[i & mask]);
      copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
      ++redone;
    }
  }
  return redone;
}

// Should never happen. With copies_chunk_mutex_ held, i.e. with the thread out of its copy loop and
// nothing half taken: the ring copies whatever is missing up to `goal` (before submitting and before
// returning the read pointer), the counters are left consistent and the thread is switched off for the
// session.
void DrawsDkImpl::RepairCopies(size_t goal, const std::string& reason) {
  const size_t taken = taken_copies_.load(std::memory_order_acquire);
  const size_t done = done_copies_.load(std::memory_order_acquire);
  const size_t redone = RedoNoMark(goal);
  taken_copies_.store(goal, std::memory_order_release);
  done_copies_.store(goal, std::memory_order_release);
  verified_copies_ = goal;
  ++copies_differences_;
  copies_no_thread_ = true;
  sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
  REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} (enqueued {}, taken {}, done {}; {} redone on "
               "the ring before submitting). Copy thread SWITCHED OFF for the rest of the session: copies go on the "
               "ring, as before",
               reason, goal, taken, done, redone);
}

const char* DrawsDkImpl::PhaseNameCopies() const {
  if (copies_no_thread_) {
    return "NO THREAD (copies on the ring)";
  }
  return copies_phase_ == kCopiesApplying ? "APPLYING" : copies_phase_ == kCopiesLooking ? "LOOKING" : "NO HELP";
}

// Every 10 s, if there were copies: who did them and how long the ring really waited. Ring only.
void DrawsDkImpl::ReportCopies() {
  if (!copies_thread_.joinable() && !copies_no_thread_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - copies_report_ < std::chrono::seconds(10)) {
    return;
  }
  copies_report_ = now;
  const uint64_t thread_n = copies_thread_n_.load(std::memory_order_relaxed);
  const uint64_t thread_bytes = copies_thread_bytes_.load(std::memory_order_relaxed);
  const uint64_t d_thread_n = thread_n - copies_thread_n_previous_;
  const uint64_t d_thread_bytes = thread_bytes - copies_thread_bytes_previous_;
  copies_thread_n_previous_ = thread_n;
  copies_thread_bytes_previous_ = thread_bytes;
  const ReportCopiesFigures& c = copies_inf_;
  if (c.queued || c.in_line || c.waits) {
    SA2_REPORT_RING(
        "[native] C6 vertex copies, last 10 s: {} enqueued ({:.1f} MB) and {} on the ring without "
        "going through the queue ({:.1f} MB); the thread copied {} ({:.1f} MB) and the ring helped with {} ({:.1f} MB, {:.1f} ms) "
        "in {} of {} waits with pending copies ({} not taken on arrival); waited for the thread's chunk {} times "
        "({:.2f} ms, worst {:.2f} ms) and for all of it {} times; pure wait {:.1f} ms (worst {:.2f} ms); {} copies "
        "checked, {} mismatches; phase {}",
        c.queued, double(c.bytes_queued) / 1048576.0, c.in_line, double(c.bytes_in_line) / 1048576.0,
        d_thread_n, double(d_thread_bytes) / 1048576.0, c.helped, double(c.bytes_helped) / 1048576.0,
        double(c.ns_helping) / 1e6, c.waits_with_help, c.waits, c.no_take, c.waits_chunk,
        double(c.ns_chunk) / 1e6, double(c.ns_chunk_worst) / 1e6, c.whole_waits, double(c.ns_wait) / 1e6,
        double(c.ns_wait_worst) / 1e6, c.checked, copies_differences_, PhaseNameCopies());
  }
  copies_inf_ = ReportCopiesFigures{};
}

// The per-fetch-constant sampler table, every 10 s: hits, and misses by cause.
void DrawsDkImpl::ReportCacheFetch() {
  const auto now = std::chrono::steady_clock::now();
  if (now - fetch_report_ < std::chrono::seconds(10)) {
    return;
  }
  fetch_report_ = now;
  const std::array<uint64_t, 6> now_figures{samplers_cache_fetch_, fetch_failures_clash_, fetch_empty_failures_,
                                             fetch_failures_generation_, fetch_expired_failures_, samplers_cache_};
  std::array<uint64_t, 6> d{};
  for (size_t i = 0; i < d.size(); ++i) {
    d[i] = now_figures[i] - fetch_previous_report_[i];
  }
  fetch_previous_report_ = now_figures;
  const uint64_t failures = d[1] + d[2] + d[3] + d[4];
  if (d[0] + failures == 0) {
    return;
  }
  SA2_REPORT_RING(
      "[native] C6 per-fetch sampler cache ({} slots), last 10 s: {} register hits, {} "
      "table lookups with {} hits and {} misses ({:.1f} %): {} collisions with another fetch in its slot, {} in "
      "an empty slot, {} by generation and {} expired (the texture must be rechecked)",
      cache_fetch_.size(), d[5], d[0] + failures, d[0], failures,
      100.0 * double(failures) / double(std::max<uint64_t>(d[0] + failures, 1)), d[1], d[2], d[3], d[4]);
}

void DrawsDkImpl::StopCopies() {
  if (!copies_thread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> latch(copies_mutex_);
    copies_stop_ = true;
  }
  copies_cv_.notify_one();
  copies_thread_.join();
  REXLOG_INFO("[native] C6: vertex-copy thread stopped ({} copies on the ring because of a full queue, {} "
              "ring waits, {:.1f} ms waiting; the ring copied {} while waiting ({:.1f} MB, "
              "{:.1f} ms), {} checked, {} mismatches, phase {})",
              copies_in_line_, waits_copies_, double(ns_waiting_copies_) / 1e6, helped_copies_,
              double(copies_bytes_helped_) / 1048576.0, double(copies_ns_helping_) / 1e6, checked_copies_,
              copies_differences_, PhaseNameCopies());
}

void DrawsDkImpl::LoopCopies() {
  for (;;) {
    if (HasCopiesNoTake()) {
      // Holding copies_chunk_mutex_ from the first batch until there is nothing left to take, and never
      // waiting on anything inside. If the ring waits for the current batch, it waits on this mutex and the
      // thread runs at the ring's priority until it releases it.
      uint64_t copies = 0;
      uint64_t bytes = 0;
      {
        std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
        while (TakeAndCopy(false, copies, bytes)) {
        }
      }
      copies_thread_n_.fetch_add(copies, std::memory_order_relaxed);
      copies_thread_bytes_.fetch_add(bytes, std::memory_order_relaxed);
      continue;  // look again: if nothing is left, sleep through the path below
    }
    // Nothing to take (the ring may have taken the last ones). Under the lock: wake the ring if it waits,
    // and sleep until there is more. If the read of written_copies_ was stale, the ring signals at its next
    // point that takes the lock.
    std::unique_lock<std::mutex> latch(copies_mutex_);
    if (copies_waiting_) {
      done_copies_cv_.notify_one();
    }
    copies_sleeping_ = true;
    copies_cv_.wait(latch, [this] { return copies_stop_ || HasCopiesNoTake(); });
    copies_sleeping_ = false;
    /*
     * The thread used to die on its own. It exited with "if (!HasCopiesNoTake()) return;", on the
     * assumption that it was only woken without work in order to stop. But the ring takes untaken copies
     * when it waits for them (TakeAndCopy, without copies_mutex_): if the thread woke because there were
     * copies and the ring took them before this check, the thread ended for good. It was seen in
     * a busy scene with the ring at 94 %: from then on "the thread copied 0" in every report, the
     * menu included, and the ring did all the copies (3-4 ms more per frame for the rest of the session).
     * Now it only exits when asked to stop; otherwise it looks again.
     */
    if (copies_stop_ && !HasCopiesNoTake()) {
      return;  // stop, nothing pending
    }
  }
}

// The factory (sa2_native_dk.h). nullptr when the draws cannot start (the log says why).
std::unique_ptr<DrawsDk> DrawsDk::Create(rex::memory::Memory* memory, ContextTargetsDk* context) {
  auto draws = std::make_unique<DrawsDkImpl>(memory, context);
  if (!draws->Initialize()) {
    return nullptr;
  }
  return draws;
}

}  // namespace sa2::native
#endif  // SA2_DK_AVAILABLE
