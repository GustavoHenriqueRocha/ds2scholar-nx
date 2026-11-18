// SA2 - native renderer on deko3d: the "alvos-infra" region of TargetsDk (ds2-deko3d
// docs/pesquisas/traducao/01-alvos-infra.md). Translation of TargetsVulkan (sa2_native_targets.cpp) 932-1752,
// 3852-4062 and 10021-10866: construction and Initialize, the output pass shaders, submission (SendWork,
// SendPartial), GPU marks and statistics, image creation and destruction, the work slots, the wait reports, and the
// factory TargetsNative::CreateDeko3d.
//
// The swaps (contract 08): command pools/buffers -> sa2::dk::CmdStream per slot; fences -> submission orders
// (sa2::dk::Submit / WaitOrder / CompletedOrder); query pools -> sa2::dk::Counters (raw ticks, the x1.627 of the
// reports holds); descriptor pools -> one transient descriptor ring per slot; VkImage + memory + view ->
// sa2::dk::ImageId with views made on the spot; samplers -> sampler descriptor indices; SPIR-V modules and
// pipelines -> DKSH shaders in a code arena.

#include "sa2_native_targets_dk.h"

#if SA2_DK_AVAILABLE

// New, deko3d only. Off = no framebuffer compression requested (conservative until the console A/B).
REXCVAR_DEFINE_BOOL(sa2_dk_compress_color, false, "SA2 renderer",
                    "deko3d: render targets with DkImageFlags_HwCompression (color without load/store). Clears cost "
                    "1/6 in the bench; copies between compressed and plain images not checked on the console yet")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_dk_compress_depth, false, "SA2 renderer",
                    "deko3d: depth render targets with DkImageFlags_HwCompression (Z24S8_2CZ kind)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sa2::native {

namespace {

// The sampler of the renderer's own passes, as the Vulkan VkSamplerCreateInfo it replaces (mipmapMode NEAREST,
// one mip level).
DkSampler SamplerClamp(DkFilter filter) {
  DkSampler sampler;
  dkSamplerDefaults(&sampler);
  sampler.minFilter = filter;
  sampler.magFilter = filter;
  sampler.mipFilter = DkMipFilter_Nearest;
  sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
  return sampler;
}

}  // namespace

// ---- sa2_native_targets.cpp 932-1752 -------------------------------------------------------------------------

TargetsDk::TargetsDk(rex::memory::Memory* memory) : memory_(memory) {
  // Mass Effect (Switch): any EDRAM mode. Outside mode 4 every resolve whose size or format changed at
  // the same address took a full GPU drain (site 4 of WaitGpu: up to ~16 per second on the console,
  // ~4 s of fence waits per 10 s) instead of retiring the old image behind its submission fence.
  resolved_pool_startup_eligible_ =
      !REXCVAR_GET(sa2_native_resolver_no_copy);
}

TargetsDk::~TargetsDk() {
  ++waits_gpu_reason_[1]; WaitGpu();
  draws_.reset();  // their descriptors and memos point to these images
  // Vulkan 948-1001: the framebuffers, views, render passes, pipelines, layouts and shader modules of the EDRAM
  // passes. deko3d has none of those objects: views are made on the spot, framebuffers and render passes do not
  // exist, and the shaders live in code_ until the process ends.
  for (auto& [key, image] : depths_) {
    Destroy(image);
  }
  for (auto& [key, image] : targets_) {
    Destroy(image);
  }
  for (auto& [key, resolved] : resolved_) {
    Destroy(resolved.image);
  }
  for (auto& [key, resolved] : parked_resolved_) Destroy(resolved.image);  // banjo-nx
  for (auto& sleeping : resolved_sleeping_) Destroy(sleeping.image);
  for (auto& clip : resolved_clips_) Destroy(clip.image);
  ReleaseAlternates();  // ds2_dynamic_resolution_keep: any twin left
  for (auto& [key, staging] : staging_reads_) Destroy(staging);  // sa2-nx render resolution
  for (ImageFront& replenished : fronts_images_) {  // sa2_native_lazy_front
    Destroy(replenished.image);
  }
  pending_reads_.clear();
  for (WorkSlot& slot : slots_) {
    slot.reads.clear();
  }
  for (auto& [key, read] : reads_) {
    DestroyRead(read);
  }
  // Vulkan 1029-1065: framebuffers_, pipeline_, the conversion pipelines and modules, the output layouts and pool:
  // nothing to free in deko3d (code memory stays with code_). The descriptors and blocks are freed below.
  if (sampler_depth_edram_ != UINT32_MAX) sa2::dk::FreeSamplerDescriptor(sampler_depth_edram_, 0);
  // The output objects (render pass, pipelines, slots, sets, ramp buffers, sampler_, vs_ / fs_ / fs_ramp_): the
  // present region's DestroyOutput (the outputs were waited for by WaitGpu).
  DestroyOutput();
  for (WorkSlot& slot : slots_) {
    // Images destroyed while the slot was recording and never sent (shutdown): the GPU is idle now.
    for (const sa2::dk::ImageId id : slot.destroy_after_send) sa2::dk::DestroyImage(id, 0);
    slot.destroy_after_send.clear();
    if (slot.buffer_stencil_copy) sa2::dk::FreeBlock(slot.buffer_stencil_copy, 0);
    slot.constants.Release(0);
    slot.work.Release(0);
    slot.upload.Release(0);
  }
  // Vulkan destroyed queries_ and marks_draw_ only (stats_ and stats_draw_ were left to the device).
  queries_.Release(0);
  marks_draw_.Release(0);
  stats_.Release(0);
  stats_draw_.Release(0);
}

bool TargetsDk::Initialize() {
  // sa2-nx render resolution: the starting scale (handheld / docked height), before any image exists.
  scale_q_ = sa2::render::WantedScaleQuarters();
  REXLOG_INFO("[res] native renderer: render targets and resolved textures at {}/4 of the guest size ({}x{} for the "
              "game's 1280x720)", scale_q_, ScaleSize(1280, scale_q_), ScaleSize(720, scale_q_));
  // sa2_msaa: host multisampling of the game's single-sample render targets, when the device has it for color,
  // depth/stencil attachments and sampling (the depth resolve reads samples), plus vkCmdResolveImage.
  // deko3d: the hardware has 2x/4x/8x, but dkCmdBufResolveImage only resolves whole images (the copies resolve a
  // rectangle) and the MSAA depth resolve pass is not translated: single sample, as with sa2_msaa = 0 (the default).
  msaa_samples_ = sa2::render::MsaaSamples();
  if (msaa_samples_ > 1) {
    REXLOG_WARN("[res] [dk] MSAA {}x is not available on this device: single sample", msaa_samples_);
    msaa_samples_ = 1;
  }
  // vkCmdCopyImage, vkCmdClearDepthStencilImage and vkCmdBlitImage were requested from the driver: in deko3d they
  // are dkCmdBufCopyImage (copy engine), dkCmdBufClearDepthStencil (3D engine, on a bound target) and
  // dkCmdBufBlitImage (2D engine), always there.
  // Attachment, copy and clear source and destination, and sampling of the resolved ones (shadows): D24S8 has all
  // of them in deko3d (format table: render, sampling, copy engine, 2D engine without filter).
  // To draw the shadow map smaller it has to be scaled up when resolving it, and that is a vkCmdBlitImage on
  // depth. If the driver does not provide it, nothing is scaled.
  // deko3d: the 2D engine takes Z24S8 without filtering, but a blit between depth images with their compression
  // kinds is not checked on the console yet: not scaled (the depth is born cleared, as before).
  raw64_edram_supported_ = true;  // RGBA16_Uint: sampling and load/store (deko3d format table)
  // ds2scholar-nx (ds2_7e3_r11g11b10): the 7e3 targets in B10G11R11_UFLOAT if the device draws, blends, samples,
  // copies and stores it (the transfer and resolve shaders write and read it as a storage image).
  // deko3d: R11G11B10Float renders, filters, uses the 2D engine and load/store (format table).
  if (REXCVAR_GET(ds2_7e3_r11g11b10)) {
    const bool ok = true;
    g_host_7e3_r11g11b10 = ok;
    REXLOG_INFO("[native] [dk] 7e3 targets in B10G11R11_UFLOAT (ds2_7e3_r11g11b10): {} (deko3d R11G11B10Float)",
                ok ? "YES" : "no, the device lacks a needed use: RGBA16F as before");
  }
  format_depth_ = VK_FORMAT_D24_UNORM_S8_UINT;
  scalable_depth_ = false;
  REXLOG_INFO("[native] targets: depth format {}{}{}{}",
              format_depth_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", "", "", "");
  if (!scalable_depth_) {
    REXLOG_INFO("[native] [dk] targets: the driver cannot scale depth: the shadow map stays at its size");
  }
  if (!code_.Initialize()) {
    return false;
  }
  // One pool per command buffer: the SDK table does not include
  // vkResetCommandBuffer either, so the whole pool is reset.
  // deko3d: one command stream per list (CmdStream: its memory is rewound in BeginRecording).
  for (WorkSlot& slot : slots_) {
    if (!slot.work.Initialize(kCommandBytesWork, "work commands") ||
        !slot.upload.Initialize(kCommandBytesUpload, "upload commands")) {
      return false;
    }
  }
  // Typeless EDRAM conversion. Work is expressed in physical 32-bit EDRAM words; true 64-bpp
  // render targets consume two adjacent words per pixel.
  // deko3d: no set or pipeline layouts. The two storage images are transient descriptors of the slot and the 19
  // words of push constants (sa2-nx: + the scaled tile size) a uniform buffer in the slot's arena.
  const std::array<dkshaders::Id, 21> codes_conversion{{
      dkshaders::kSa2Edram7e3ToRgba8,
      dkshaders::kSa2EdramRgba8To7e3,
      dkshaders::kSa2Edram16fTo16f,
      dkshaders::kMeResolveExpBias,
      dkshaders::kMeEdramRaw64ToRgba8,
      dkshaders::kMeEdramRgba8ToRaw64,
      dkshaders::kMeEdramRaw64To16f,
      dkshaders::kMeEdram16fToRaw64,
      dkshaders::kMeEdramRaw64ToRaw64,
      dkshaders::kMeResolveExpBiasRgba8,  // ds2-nx: 9
      dkshaders::kDs2Resolve7e3AsUnorm10,  // ds2-nx: 10
      // ds2scholar-nx (ds2_7e3_r11g11b10): 11-18, only created with the 7e3 targets in B10G11R11.
      dkshaders::kSa2Edram7e3ToRgba8R11,  // 11
      dkshaders::kSa2EdramRgba8To7e3R11,  // 12
      dkshaders::kMeResolveExpBiasR11,  // 13
      dkshaders::kMeResolveExpBiasRgba8R11,  // 14
      dkshaders::kDs2Resolve7e3AsUnorm10R11,  // 15
      dkshaders::kSa2Edram16fTo16fR11src,  // 16
      dkshaders::kSa2Edram16fTo16fR11dst,  // 17
      dkshaders::kSa2Edram16fTo16fR11both,  // 18
      dkshaders::kMeEdram16fToRaw64R11,  // 19 (7)
      dkshaders::kMeEdramRaw64To16fR11,  // 20 (6)
  }};
  // ds2-nx (ds2_resolve_7e3_direct): pipeline 10 stores rgb10_a2 (shaderStorageImageExtendedFormats); without the
  // feature it is not created and those resolves go the sa2-nx way.
  // deko3d: RGB10A2 has load/store in the format table.
  storage_unorm10_ = true;
  for (uint32_t i = 0; i < codes_conversion.size(); ++i) {
    if (i == 10 && !storage_unorm10_) continue;
    if (i >= 11 && !g_host_7e3_r11g11b10) continue;
    if (i == 15 && !storage_unorm10_) continue;
    pipelines_conversion_edram_[i] = LoadShader(codes_conversion[i]);
    if (!pipelines_conversion_edram_[i]) {
      return false;
    }
  }
  // Independent layout: sampled depth/stencil aspects cannot use storage-image descriptors.
  // deko3d: the same split holds (depth formats have no load/store): sampled depth, storage target, sampled stencil,
  // all three from the slot's ring.
  const std::array<dkshaders::Id, 5> codes_depth{{
      dkshaders::kMeEdramDepthToRgba8,
      dkshaders::kMeEdramDepthTo16f,
      dkshaders::kMeEdramDepthToRaw64,
      dkshaders::kMeDepthResolveGuestspace,
      dkshaders::kMeEdramDepthTo16fR11,
  }};
  for (uint32_t i = 0; i < codes_depth.size(); ++i) {
    if (i == 4 && !g_host_7e3_r11g11b10) continue;
    pipelines_conversion_depth_edram_[i] = LoadShader(codes_depth[i]);
    if (!pipelines_conversion_depth_edram_[i]) return false;
  }
  sampler_depth_edram_ = sa2::dk::AllocSamplerDescriptor();
  if (sampler_depth_edram_ == UINT32_MAX) return false;
  sa2::dk::WriteSamplerDescriptor(sampler_depth_edram_, SamplerClamp(DkFilter_Nearest));
  used_slots_ = uint32_t(std::clamp(REXCVAR_GET(sa2_native_slots_work), 1,
                                    int32_t(slots_.size())));
  REXLOG_INFO("[native] targets: work slots (sa2_native_slots_work) = {} of {}", used_slots_,
              slots_.size());
  // deko3d: the descriptor pools of every slot (conversion, depth conversion, import, color fragment, depth resolve,
  // stencil copy) are one transient ring per slot, an equal share of the transient range among the used slots
  // (Vulkan 1308-1347 created them above, before the slot count was known).
  for (uint32_t i = 0; i < slots_.size(); ++i) {
    WorkSlot& slot = slots_[i];
    slot.descriptors = sa2::dk::TransientRing(std::min(i, used_slots_ - 1), used_slots_);
    slot.descriptors_capacity = (sa2::dk::kImageDescriptors - sa2::dk::kDescriptorsPersistentEnd) / used_slots_;
    if (i < used_slots_ && !slot.constants.Initialize(kConstantsBytesSlot, sa2::dk::kCpuToGpu, "slot constants")) {
      return false;
    }
  }
  REXLOG_INFO("[native] [dk] targets: {} transient descriptors and {} KB of constants per work slot",
              slots_[0].descriptors_capacity, kConstantsBytesSlot >> 10);
  output_no_wait_ = REXCVAR_GET(sa2_native_output_no_wait);
  invalidate_each_copy_ = REXCVAR_GET(sa2_native_invalidate_textures_each_copy);
  REXLOG_INFO("[native] targets: texture caches dropped on every copy (sa2_native_invalidate_textures_each_copy) = {}",
              invalidate_each_copy_ ? "YES" : "no");
  REXLOG_INFO("[native] targets: output does not wait for the previous one (sa2_native_output_no_wait) = {}",
              output_no_wait_ ? "YES" : "no");
  precise_marks_ = REXCVAR_GET(sa2_native_precise_marks);
  marks_categories_ = REXCVAR_GET(sa2_gpu_marks_categories);
  REXLOG_INFO("[native] targets: GPU timestamps per category (sa2_gpu_marks_categories) = {}",
              marks_categories_ ? "YES" : "no");
  REXLOG_INFO("[native] targets: precise GPU timestamps (sa2_native_precise_marks) = {}",
              precise_marks_ ? "YES" : "no");
  // GPU time per work unit: two timestamps per slot. Without timestamp bits on the queue, it is not measured.
  // deko3d: counter reports (16 bytes: value, timestamp) in GPU-uncached memory, always available.
  {
    if (!queries_.Initialize(uint32_t(slots_.size() * kMarksPerSlot), "GPU timestamps")) {
      queries_ = sa2::dk::Counters{};
    }
    // Raw GPU ticks, as NVK's timestamps were (timestampPeriod 1): every x1.627 in the reports still applies.
    period_mark_ns_ = 1.0;
    REXLOG_INFO("[native] targets: GPU time per Swap {}",
                queries_.Capacity() ? "measured with timestamps"
                                    : "not available (the queue has no timestamps)");
    // Pipeline statistics per pass. The order of the counters is the order of the bits,
    // not the order of this list: vertices, clipped primitives and fragments.
    // deko3d: kCountersStat reports at the start of the pass and kCountersStat at its end (kCountersStatDk).
    if (!stats_.Initialize(uint32_t(slots_.size() * kStatsPerSlot * kReportsStat), "pass statistics")) {
      stats_ = sa2::dk::Counters{};
    }
    // deko3d: the per-draw pools (4 MB of reports for 4 slots) only when the per-draw diagnostic is on at start
    // (sa2_native_stats_per_draw_s > 0); BeginStatsDraw needs it on anyway (window_diagnostic_).
    if (stats_.Capacity() && REXCVAR_GET(sa2_native_stats_per_draw_s) > 0) {
      if (!stats_draw_.Initialize(uint32_t(slots_.size() * kStatsDrawPerSlot * kReportsStat),
                                  "per-draw statistics")) {
        stats_draw_ = sa2::dk::Counters{};
      }
      if (stats_draw_.Capacity() && queries_.Capacity()) {
        if (!marks_draw_.Initialize(uint32_t(slots_.size() * kStatsDrawPerSlot * 2), "per-draw timestamps")) {
          marks_draw_ = sa2::dk::Counters{};
        }
      }
    }
    REXLOG_INFO("[native] targets: pipeline statistics per pass {}",
                stats_.Capacity() ? "available (sa2_native_stats_pipeline)"
                                  : "not available");
  }

  // The output sampler (linear, clamp), the output set layout (image 0, sampler 1, gamma ramp 2), the pipeline
  // layout with its 16 + 16 bytes of push constants, the descriptor pool and sets of the output slots: in deko3d the
  // present region's InitializeOutput, below.
  // One ramp buffer per output slot, so it is only changed in a slot the GPU is no longer using. Without
  // them, the output has no ramp (as before).
  ramp_gamma_ = REXCVAR_GET(sa2_native_ramp_gamma);
  // MASSEFFECT loads a gamma ramp before its first visible frame, but Mass Effect may not. The table is
  // zero-initialized, so leaving it untouched makes the output shader turn every pixel black until the
  // guest eventually writes a ramp. Start from the identity ramp declared below.
  if (ramp_gamma_) RecomputeRamp();
  // deko3d: the ramp buffers, vs_ / fs_ (output_rect.vert, output_bilinear.frag), the output slots and the window
  // (InitializeOutput). The ramp buffers always exist there, so a failure fails Initialize instead of turning the
  // ramp off (Vulkan 1574-1578). No render pass and no pipeline objects.
  if (!InitializeOutput()) {
    return false;
  }
  // The ramp variants (bilinear and exact texel) are created now (PipelineRamp; in deko3d the present region's
  // ShaderRamp).
  if (ramp_gamma_ && (!ShaderRamp(0) || !ShaderRamp(1))) {
    return false;
  }
  output_fxaa_ = ramp_gamma_ && REXCVAR_GET(ds2_output_fxaa);
  if (output_fxaa_ && !ShaderRamp(2)) {
    output_fxaa_ = false;  // logged by ShaderRamp; the output stays as before
  }
  REXLOG_INFO("[native] targets: FXAA on the output (ds2_output_fxaa) = {}{}{}", output_fxaa_ ? "YES" : "no",
              output_fxaa_ && REXCVAR_GET(ds2_output_fxaa_quality) ? ", 3.11 Quality (ds2_output_fxaa_quality)" : "",
              REXCVAR_GET(ds2_output_fxaa) && !ramp_gamma_ ? " (needs sa2_native_ramp_gamma)" : "");
  REXLOG_INFO("[native] targets: the game's gamma ramp on the output (sa2_native_ramp_gamma) = {}",
              ramp_gamma_ ? "YES" : "no");
  // Draw code: without the required capabilities only copies and presentation remain.
  draws_ = DrawsDk::Create(memory_, this);
  return true;
}

const DkShader* TargetsDk::LoadShader(dkshaders::Id id) {
  // The same DKSH is asked for by several passes (kMeEdramImport three times): one copy in code memory.
  if (auto it = shaders_loaded_.find(uint32_t(id)); it != shaders_loaded_.end()) return it->second;
  const DkShader* shader = code_.Load(dkshaders::Data(id), dkshaders::Size(id), dkshaders::Name(id));
  if (!shader) {
    REXLOG_ERROR("[native] [dk] targets: shader {} is not a valid DKSH", dkshaders::Name(id));
  }
  shaders_loaded_[uint32_t(id)] = shader;
  return shader;
}

void TargetsDk::StartList(DkCmdBuf cmd) {
  sa2::dk::BindDescriptors(cmd);
  if (sa2::dk::DescriptorsWrittenSince(descriptors_seen_)) sa2::dk::Barrier(cmd, sa2::dk::Hazard::Descriptors);
  if (code_.TakeWritten()) sa2::dk::Barrier(cmd, sa2::dk::Hazard::ShaderCode);
}

bool TargetsDk::RoomSlotEDRAM4() const {
  const WorkSlot& slot = slots_[slot_];
  return slot.conversions_edram < kConversionsEDRAMPerSlot &&
         slot.descriptors.Used() + kDescriptorsConversionEDRAM <= slot.descriptors_capacity &&
         slot.constants.Used() + DK_UNIFORM_BUF_ALIGNMENT <= slot.constants.Size();
}

// ---- sa2_native_targets.cpp 3852-4062 ------------------------------------------------------------------------

bool TargetsDk::SendPartial() {
  if (!recording_) {
    return false;
  }
  if (draws_) {
    draws_->BeforeSend();  // closes the pass, collects the texture binds, flushes the upload buffer
  }
  WorkSlot& slot = slots_[slot_];
  const DkQueue queue = sa2::dk::Queue();
  if (recording_upload_) {
    recording_upload_ = false;
    sa2::dk::Barrier(commands_upload_, sa2::dk::Hazard::UploadToSample);  // as in SendWork
    dkQueueSubmitCommands(queue, slot.upload.Finish());
  }
  dkQueueSubmitCommands(queue, slot.work.Finish());
  dkQueueFlush(queue);  // no fence: the slot's final submission signals the only one
  // deko3d: a command buffer goes on in its memory after dkCmdBufFinishList, so the "new command buffers" are the
  // same streams (rewound with the slot in BeginRecording). The upload list starts again on its first use.
  ++slot.partials;
  StartList(commands_work_);
  ++generation_commands_;  // the draws bind everything again in the new buffer
  ++report_partial_sends_;
  return true;
}

bool TargetsDk::WaitGpuOfTheAll() {
  ++waits_gpu_reason_[2]; WaitGpu();
  return Record();
}

void TargetsDk::MarkGpu(uint32_t category) {
  if (!queries_.Capacity() || !recording_) {
    return;
  }
  WorkSlot& slot = slots_[slot_];
  if (slot.categories.size() + 1 >= kMarksPerSlot) {
    ++exhausted_marks_;
    return;
  }
  if (
(!slot.categories.empty() && slot.categories.back() == category)) {
    return;  // same category continues
  }
  if (!marks_categories_ && !slot.categories.empty()) {
    return;  // sa2_gpu_marks_categories = false: first mark of the submission only
  }
  // BOTTOM_OF_PIPE -> DkCounter_Timestamp (after the previous work), TOP_OF_PIPE -> DkCounter_TimestampPipelineTop.
  queries_.Report(commands_work_, slot_ * kMarksPerSlot + uint32_t(slot.categories.size()),
                  slot.precise_marks ? DkCounter_Timestamp : DkCounter_TimestampPipelineTop);
  slot.categories.push_back(uint8_t(category));
  slot.marks_written = std::max(slot.marks_written, uint32_t(slot.categories.size()));
  slot.labels.push_back(0);
  slot.descriptions.push_back(0);
  slot.draws_mark.push_back(0);
}

uint32_t TargetsDk::BeginStats(uint32_t category) {
  // With the per-draw diagnostic enabled this one is not opened: two queries of the same type cannot be
  // active at once, and the per-pass query would enclose the per-draw ones.
  if (!stats_.Capacity() || !recording_ ||
      !REXCVAR_GET(sa2_native_stats_pipeline) ||
      REXCVAR_GET(sa2_native_stats_per_draw_s) > 0) {
    return UINT32_MAX;
  }
  WorkSlot& slot = slots_[slot_];
  if (slot.stats.size() >= kStatsPerSlot) {
    ++stats_no_site_;
    if (stats_no_site_ % 200 == 1) {
      REXLOG_WARN("[native] targets: no room for pass statistics ({} times): the work has more than {} "
                  "passes", stats_no_site_, kStatsPerSlot);
    }
    return UINT32_MAX;
  }
  const uint32_t index =
      slot_ * kStatsPerSlot + uint32_t(slot.stats.size());
  // vkCmdBeginQuery: the running totals at the start (ReadStats subtracts them from the end).
  for (uint32_t k = 0; k < kCountersStat; ++k) {
    stats_.Report(commands_work_, index * kReportsStat + k, kCountersStatDk[k]);
  }
  slot.stats.emplace_back(index, uint8_t(category));
  return index;
}

void TargetsDk::FinishStats(uint32_t index) {
  if (stats_.Capacity() && index != UINT32_MAX && recording_) {
    for (uint32_t k = 0; k < kCountersStat; ++k) {  // vkCmdEndQuery
      stats_.Report(commands_work_, index * kReportsStat + kCountersStat + k, kCountersStatDk[k]);
    }
  }
}

uint32_t TargetsDk::BeginStatsDraw(uint32_t label, uint32_t category, uint32_t vs_more_one) {
  if (!stats_draw_.Capacity() || !recording_ || !window_diagnostic_) {
    return UINT32_MAX;
  }
  WorkSlot& slot = slots_[slot_];
  if (slot.stats_draw.size() >= kStatsDrawPerSlot) {
    ++stats_draw_no_site_;
    if (stats_draw_no_site_ % 2000 == 1) {
      REXLOG_WARN("[native] targets: no room for per-draw statistics ({} times)",
                  stats_draw_no_site_);
    }
    return UINT32_MAX;
  }
  const uint32_t index = slot_ * kStatsDrawPerSlot +
                         uint32_t(slot.stats_draw.size());
  // the draw's GPU time. ALL_COMMANDS waits for the previous work, so the pair brackets
  // this draw alone (serialized: only for ranking pixel shaders, not for the frame's total).
  // deko3d: DkCounter_Timestamp is released after the previous work leaves the pipeline (the same report NVK
  // emitted for ALL_COMMANDS).
  if (marks_draw_.Capacity()) {
    marks_draw_.Report(commands_work_, index * 2, DkCounter_Timestamp);
  }
  for (uint32_t k = 0; k < kCountersStat; ++k) {  // vkCmdBeginQuery
    stats_draw_.Report(commands_work_, index * kReportsStat + k, kCountersStatDk[k]);
  }
  slot.stats_draw.emplace_back(
      index, uint16_t((category % kGpuCategories) * kLabelsShader + label % kLabelsShader));
  slot.draw_ps.push_back(((category % kGpuCategories) << 24) | (label & 0xFFFFFF));
  slot.draw_vs.push_back(vs_more_one);
  return index;
}

void TargetsDk::FinishStatsDraw(uint32_t index) {
  if (stats_draw_.Capacity() && index != UINT32_MAX && recording_) {
    for (uint32_t k = 0; k < kCountersStat; ++k) {  // vkCmdEndQuery
      stats_draw_.Report(commands_work_, index * kReportsStat + kCountersStat + k, kCountersStatDk[k]);
    }
    if (marks_draw_.Capacity()) {
      marks_draw_.Report(commands_work_, index * 2 + 1, DkCounter_Timestamp);
    }
  }
}

VkImageCopy TargetsDk::HostCopy(const Image& source, const Image& target, VkImageCopy copy) {
  if (source.scale_q == kScaleQuartersNative && target.scale_q == kScaleQuartersNative) return copy;
  const VkRect2D s = ScaleRect(VkRect2D{{copy.srcOffset.x, copy.srcOffset.y}, {copy.extent.width, copy.extent.height}},
                               source.scale_q);
  const VkRect2D d = ScaleRect(VkRect2D{{copy.dstOffset.x, copy.dstOffset.y}, {copy.extent.width, copy.extent.height}},
                               target.scale_q);
  copy.srcOffset = {s.offset.x, s.offset.y, copy.srcOffset.z};
  copy.dstOffset = {d.offset.x, d.offset.y, copy.dstOffset.z};
  copy.extent.width = std::min(s.extent.width, d.extent.width);
  copy.extent.height = std::min(s.extent.height, d.extent.height);
  return copy;
}

// ---- sa2_native_targets.cpp 10021-10866 ----------------------------------------------------------------------

uint32_t TargetsDk::FlagsOfUsage(VkImageUsageFlags usage, VkFormat format) {
  const bool depth = sa2::dk::IsDepth(sa2::dk::FromVk(format));
  uint32_t flags = 0;
  if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
    flags |= DkImageFlags_UsageRender;
  if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !depth) flags |= DkImageFlags_UsageLoadStore;
  // Blits and whole-image resolves run on the 2D engine; the copy engine (dkCmdBufCopyImage) needs no flag.
  if (usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) flags |= DkImageFlags_Usage2DEngine;
  return flags;
}

bool TargetsDk::CreateScaled(Image& image, uint32_t width, uint32_t height, uint32_t usage, VkFormat format,
                             VkSampleCountFlagBits sample_count, uint32_t q) {
  ForgetSynchronizedEDRAM4(image);  // new contents
  // Deliberate interim allocation gate: no real 2x/4x image may reach the
  // unconverted utility shaders, pipeline caches or guest resolve paths.
  // Lifting it requires format/device capability AND all those contracts,
  // not merely changing VkImageCreateInfo.samples.
  // sa2_msaa: the host multisampling of the game's single-sample targets is the one exception.
  if (!me::native::IsSingleSample(uint32_t(sample_count)) &&
      !(msaa_samples_ > 1 && uint32_t(sample_count) == msaa_samples_))
    return Reject(9, "real multisample allocation is not enabled in this backend");
  sa2::dk::ImageDesc info;
  info.type = DkImageType_2D;
  info.format = sa2::dk::FromVk(format);
  if (info.format == DkImageFormat_None) {
    REXLOG_ERROR("[native] [dk] targets: no deko3d format for VkFormat {}", uint32_t(format));
    return false;
  }
  // VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT (RGBA16F viewed as RGBA16_UINT): a deko3d view takes any format of the same
  // texel size, no flag needed.
  info.width = ScaleSize(width, q);
  info.height = ScaleSize(height, q);
  info.depth_or_layers = 1;
  info.mips = 1;
  // deko3d: every image renders (Prepare clears through a bound render target; deko3d has no vkCmdClearColorImage,
  // and all the host formats of this renderer render). Color images also take the 2D engine when their format does
  // (every uncompressed one: blits and whole-image resolves; without it they are rejected). Depth keeps what the
  // caller asked (a depth blit is not checked on the console). Compression: sa2_dk_compress_color / _depth.
  const bool depth = sa2::dk::IsDepth(info.format);
  uint32_t dk_flags = usage | DkImageFlags_UsageRender;
  if (depth) dk_flags &= ~uint32_t(DkImageFlags_UsageLoadStore);
  if (!depth && sa2::dk::BytesPerTexel(info.format) != 0) dk_flags |= DkImageFlags_Usage2DEngine;
  if (depth ? REXCVAR_GET(sa2_dk_compress_depth)
            : REXCVAR_GET(sa2_dk_compress_color) && !(dk_flags & DkImageFlags_UsageLoadStore)) {
    dk_flags |= DkImageFlags_HwCompression;
  }
  info.flags = dk_flags;
  info.ms = sa2::dk::FromSamples(uint32_t(sample_count));
  info.logical = format;
  image.image = sa2::dk::CreateImage(info, "render target");
  if (image.image == sa2::dk::kNoImage) {
    // The GPU can run out of memory here too. Render targets are few and large, so one that does not fit
    // shows up at once: the texture cache is asked to release memory and the allocation is retried once.
    if (!draws_ || !draws_->DropTexturesPerMissingOfMemory() ||
        (image.image = sa2::dk::CreateImage(info, "render target")) == sa2::dk::kNoImage) {
      REXLOG_ERROR("[native] targets: out of GPU memory for a {}x{} target (format {}) and dropping the cache "
                   "was not enough",
                   info.width, info.height, uint32_t(format));
      // sa2_native_resolved_retire_idle: at the next Swap the resolved textures idle for two seconds go too (not
      // here: callers of Create may be walking resolved_).
      retire_resolved_urgently_.store(true, std::memory_order_relaxed);
      return false;
    }
  }
  // The image view: made on the spot in deko3d (sa2::dk::View).
  image.width = width;
  image.height = height;
  image.host_width = info.width;
  image.host_height = info.height;
  image.scale_q = uint8_t(q);
  image.dk_flags = dk_flags;
  image.format = format;
  image.sample_count = sample_count;
  image.prepared = false;
  return true;
}

void TargetsDk::Destroy(Image& image) {
  ReleaseAlternateOf(image.image);  // ds2_dynamic_resolution_keep
  ForgetSynchronizedEDRAM4(image);
  // sa2_native_lazy_front. A deferred copy cannot keep a destroyed image. If the texture is
  // what gets destroyed (GetResolved recreates it with another size or format), its content is lost
  // just as before: the copy is simply dropped. If it is the source (only at shutdown: render targets
  // are not destroyed, and whatever leaves a target through a swap is recorded first), the texture lacks
  // that copy: it becomes stale and, if something requests it before another full resolve, the guard
  // trips.
  if (!pending_fronts_.empty() && image.image != sa2::dk::kNoImage) {
    for (auto it = pending_fronts_.begin(); it != pending_fronts_.end();) {
      if (it->second.texture_vk == image.image) {
        ReleaseRetained(it->second);
        it = pending_fronts_.erase(it);
      } else if (it->second.source_vk == image.image) {
        expired_fronts_.insert(it->first);
        ReleaseRetained(it->second);
        it = pending_fronts_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // The DrawsVulkan framebuffers that use this view are destroyed before it: Vulkan may give the same
  // handle to a new view and FramebufferFor would return a stale one (see
  // sa2_native_framebuffers_forget_views). At shutdown, draws_ is already gone and so are its
  // framebuffers.
  // deko3d: no views or framebuffers; the draws drop what they keep by this id (a new image never gets the same
  // id: its generation differs).
  if (draws_ && image.image != sa2::dk::kNoImage) {
    draws_->ForgetImage(image.image);
  }
  // Vulkan 10140-10173: views_raw64_edram_, views_depth_edram_, framebuffers_conv_color_frag_,
  // views_raw64_rt_edram_, framebuffers_import_depth_edram_ and framebuffers_depth_resolve_ms_ of this image. None of
  // those objects exists in deko3d (views on the spot, no framebuffers).
  for (auto& owner : owners_tiles_edram4_) if (owner.image == &image) owner = {};
  ++edram4_epoch_;
  if (image.image != sa2::dk::kNoImage) {
    // deko3d: memory and id are released after the submission that last used the image. While a slot is being
    // recorded that is the slot's own final submission, whose order is known only when it is sent (SendWork);
    // otherwise everything that used it is already submitted.
    if (recording_) {
      slots_[slot_].destroy_after_send.push_back(image.image);
    } else {
      sa2::dk::DestroyImage(image.image);
    }
  }
  image = Image{};
}

void TargetsDk::Prepare(Image& image) {
  if (image.prepared) {
    return;
  }
  // In the upload command buffer, which runs before the work one (copies of resolved textures are recorded
  // there and, on their first frame, read them unprepared).
  const DkCmdBuf commands = CommandsUpload();
  if (!commands) {
    return;
  }
  // The Vulkan barrier UNDEFINED -> GENERAL: no layouts in deko3d.
  const bool depth = image.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                     image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
  const DkImageView view = sa2::dk::View(image.image);
  const DkScissor whole{0, 0, HostWidth(image), HostHeight(image)};
  if (depth) {
    // ZCULL: images created without TRANSFER_DST (the ones eligible for a ZCULL plane) cannot be
    // cleared with vkCmdClearDepthStencilImage. They are cleared by opening a pass with
    // loadOp = CLEAR. The barrier above (UNDEFINED -> GENERAL) stays as it is: it is what makes the
    // driver zero the ZCULL plane, and without it the hardware kills the context.
    // deko3d: the same split is kept (the draws' clear also resets their Zcull bookkeeping).
    if (!image.accepts_target_of_copy && draws_) {
      if (!draws_->ClearDepthInPass(commands, image, 1.0f, 0) &&
          ++cleared_in_failed_pass_ <= 8) {
        REXLOG_WARN("[native] targets: could NOT prepare the {}x{} depth by opening a clear "
                    "pass (failure {})",
                    image.width, image.height, cleared_in_failed_pass_);
      }
    } else {
      // banjo-nx: 'far' is a Windows macro. Clear depth 1 and stencil 0 (mask 0xFF) on the bound depth target;
      // a depth-only bind passes no color array (bench: a null color target faults).
      dkCmdBufBindRenderTargets(commands, nullptr, 0, &view);
      dkCmdBufSetScissors(commands, 0, &whole, 1);
      dkCmdBufClearDepthStencil(commands, true, 1.0f, 0xFF, 0);
    }
  } else {
    const DkImageView* const targets[1] = {&view};
    dkCmdBufBindRenderTargets(commands, targets, 1, nullptr);
    dkCmdBufSetScissors(commands, 0, &whole, 1);
    const uint32_t zero[4] = {};
    dkCmdBufClearColor(commands, 0, DkColorMask_RGBA, zero);
  }
  // deko3d: the clear runs on the 3D engine and the copies recorded after it in this list run on the copy engine;
  // the Vulkan layout barrier ordered them, here it is explicit.
  sa2::dk::Barrier(commands, sa2::dk::Hazard::RenderToCopy);
  image.prepared = true;
  if (draws_) {
    draws_->InvalidateTextures();  // ResolvedTexture only returns prepared ones
  }
}

bool TargetsDk::EnsureCapacityConversionEDRAM4(const char* operation) {
  if (REXCVAR_GET(sa2_native_edram_alias_mode) != 4)
    return RoomSlotEDRAM4();
  // Close BEFORE obtaining descriptors: closing a shadow pass may itself
  // submit and rotate the slot. Also avoids recursive submit in BeforeSend.
  if (draws_) draws_->FinishPass();
  if (!Record()) return false;
  // sa2-nx: heterogeneous lookup, no std::string built per operation once the kind is known.
  {
    auto it = edram4_operations_.find(std::string_view(operation));
    if (it == edram4_operations_.end()) it = edram4_operations_.emplace(operation, 0).first;
    ++it->second;
  }
  // ME: the conversion's GPU time, by kind (until the next pass marks its own category).
  MarkGpu(std::strcmp(operation, "depth import") == 0   ? kGpuEdramImport
            : std::strcmp(operation, "depth export") == 0 ? kGpuEdramExport
            : std::strcmp(operation, "guestspace depth resolve") == 0 ? kGpuEdramResolveDepth
            : std::strcmp(operation, "resolve bias") == 0 ? kGpuEdramResolveBias
                                                          : kGpuEdramAlias);
  // deko3d: the slot also runs out when its descriptor ring or its constants arena is full (RoomSlotEDRAM4).
  if (RoomSlotEDRAM4()) return true;
  const uint32_t slot_previous = slot_;
  const uint32_t used = slots_[slot_].conversions_edram;
  ++edram4_capacity_rollovers_;
  if (edram4_capacity_rollovers_ <= 8)
    REXLOG_WARN("[native] EDRAM mode4 conversion capacity rollover {}: slot {} count {}/{} operation {} [dk] "
                "descriptors {}/{}",
                edram4_capacity_rollovers_, slot_previous, used,
                kConversionsEDRAMPerSlot, operation, slots_[slot_].descriptors.Used(),
                slots_[slot_].descriptors_capacity);
  // Deliberately fenced, including the one-slot configuration. BeginRecording
  // alone is the only place resetting descriptor pools after Complete.
  if (!SendWork(true) || !Record()) {
    ++edram4_capacity_rollover_failures_;
    REXLOG_ERROR("[native] EDRAM mode4 conversion capacity rollover failed: slot {} operation {}",
                 slot_previous, operation);
    return false;
  }
  return RoomSlotEDRAM4();
}

bool TargetsDk::Record() {
  if (recording_) {
    return true;
  }
  const auto before_record = std::chrono::steady_clock::now();
  const bool recording = BeginRecording();
  ns_record_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - before_record).count());
  return recording;
}

bool TargetsDk::BeginRecording() {
  // Slots rotate: while one frame is recorded, the previous ones can still be on the GPU. It only waits
  // if this slot's last submission has not finished yet, so the more slots there are, the further ahead
  // the CPU can get before it has to stop.
  slot_ = (slot_ + 1) % used_slots_;
  WorkSlot& slot = slots_[slot_];
  Complete(slot);
  if (resolved_pool_device_error_) return false;
  // vkResetCommandPool x2 and vkResetDescriptorPool x6: the slot's command streams, its descriptor ring and its
  // constants arena are rewound (the GPU is done with all of them: Complete).
  slot.work.Reset();
  slot.upload.Reset();
  slot.descriptors.Reset();
  slot.constants.Reset();
  slot.conversions_edram = 0;
  slot.partials = 0;
  commands_work_ = slot.work.cmd();
  commands_upload_ = slot.upload.cmd();
  recording_upload_ = false;
  StartList(commands_work_);  // vkBeginCommandBuffer
  slot.categories.clear();
  slot.labels.clear();
  slot.descriptions.clear();
  slot.draws_mark.clear();
  if (queries_.Capacity() && slot.marks_written) {
    // vkCmdResetQueryPool: counter reports are plain memory writes, nothing to reset in deko3d.
    slot.marks_written = 0;
  }
  /*
   * Do not reset pools that this frame will not use.
   *
   * This used to happen always, with any toml: 64 statistics queries + 2048 per-draw statistics
   * queries = 2,144 resets per frame. And in our NVK on Tegra the bulk path is excluded for the layout
   * of these pools (nvk_query_pool.c:351: everything that is not a timestamp falls into
   * ALIGNED_INTERLEAVED), so each reset is a 5-dword SET_REPORT_SEMAPHORE with
   * RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE and PIPELINE_LOCATION_ALL: 2,144 chained writes that
   * wait on each other, 42.9 KB of command stream, at the start of every frame.
   *
   * And they are almost never used: the per-pass statistics are effectively disabled (see the `> 0`
   * check in BeginStats: with stats_per_draw_s set, the per-pass query is never
   * opened), and the per-draw ones are only used one frame every 20 seconds.
   *
   * The reset comes before the first MarkGpu, so its cost does not show up in any category of the
   * breakdown: it is swallowed by the "gap between work items" (gap between submissions). Estimated at
   * 0.3-1.1 real ms per frame; the measured upper bound (the minimum gap over a whole session) is
   * 2.81 ms.
   *
   * With both off, 32 resets remain instead of 2,144: -98.5 %. The CPU vectors are cleared anyway,
   * which costs nothing.
   *
   * deko3d: no resets at all (a statistics query is a pair of counter reports, see BeginStats).
   */
  slot.stats.clear();  // those never submitted are not read
  slot.stats_draw.clear();
  slot.draw_ps.clear();
  slot.draw_vs.clear();
  slot.precise_marks = precise_marks_;
  recording_ = true;
  MarkGpu(kGpuOther);
  ++generation_commands_;
  if (draws_) {
    draws_->UseSlot(slot_);
  }
  return true;
}

/*
 * This was what prevented CPU and GPU from overlapping.
 *
 * It used to wait as well for every earlier slot still pending, so each frame drained the whole GPU
 * before recording continued. That is why the report always said "work overlapped on the GPU 0",
 * why adding a third slot changed nothing (it waited for all of them, however many there were) and
 * why the wait moved elsewhere instead of disappearing.
 *
 * Now it only blocks on the slot about to be reused, the only one whose resources are needed. Earlier
 * ones are collected only if they have already finished (polling without waiting), so their
 * timestamps are read in order when possible; one still running is collected when its turn comes.
 */
void TargetsDk::Complete(WorkSlot& slot) {
  const uint64_t completed = sa2::dk::CompletedOrder();  // vkGetFenceStatus of every slot: one poll
  for (WorkSlot& other : slots_) {
    if (&other != &slot && other.pending && other.order < slot.order &&
        other.dk_order <= completed) {
      CompleteA(other);
    }
  }
  CompleteA(slot);
}

void TargetsDk::CompleteA(WorkSlot& slot) {
  if (!slot.pending) {
    return;
  }
  const auto before_wait = std::chrono::steady_clock::now();
  if (!sa2::dk::WaitOrder(slot.dk_order)) {
    resolved_pool_device_error_ = true;
    REXLOG_ERROR("[native] [dk] work fence wait failed (order {}, queue error); resolved allocation cache disabled",
                 slot.dk_order);
    return;
  }
  order_completed_work_ = me::native::ObserveResolvedAllocationCompletion(
      order_completed_work_, slot.order, true);
  ns_waits_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - before_wait)
                                .count());
  ++waits_gpu_;
  // vkResetFences: an order is signaled once, nothing to reset.
  slot.pending = false;
  const uint32_t marked = uint32_t(slot.categories.size());
  if (queries_.Capacity() && marked >= 2) {
    static std::vector<uint64_t> marks_buffer(kMarksPerSlot);  // 64 KB: not on the ring thread's stack
    uint64_t* const marks = marks_buffer.data();
    const uint32_t index = uint32_t(&slot - slots_.data());
    // vkGetQueryPoolResults: the reports are in GPU-uncached memory, readable once the order completed.
    for (uint32_t i = 0; i < marked; ++i) marks[i] = queries_.Ticks(index * kMarksPerSlot + i);
    if (marks[marked - 1] >= marks[0]) {
      // Diagnostic: the whole mark sequence of a few work slots, with labels and durations.
      if (dump_remaining_marks_ > 0 || (REXCVAR_GET(sa2_diag_dump_marks_s) > 0 && !dump_done_marks_ &&
          std::chrono::steady_clock::now() - dump_start_ >
              std::chrono::seconds(REXCVAR_GET(sa2_diag_dump_marks_s)))) {
        if (!dump_done_marks_) { dump_done_marks_ = true; dump_remaining_marks_ = 6; }
        --dump_remaining_marks_;
        std::string list;
        for (uint32_t i = 0; i + 1 < marked; ++i) {
          const double ms = marks[i + 1] >= marks[i] ? double(marks[i + 1] - marks[i]) * period_mark_ns_ / 1e6 : -1;
          if (ms < 0.05) continue;
          const uint32_t e = i < slot.labels.size() ? slot.labels[i] : 0;
          const uint64_t dsc = i < slot.descriptions.size() ? slot.descriptions[i] : 0;
          const uint32_t nd = i < slot.draws_mark.size() ? slot.draws_mark[i] : 0;
          list += fmt::format(" [{}:c{} VS{} PS{} {}x{} f{}/d{} {} draws {:.2f}]", i, slot.categories[i], e >> 16,
                               e & 0xFFFF, dsc >> 48, (dsc >> 32) & 0xFFFF, (dsc >> 16) & 0xFFFF, dsc & 0xFFFF, nd, ms);
        }
        REXLOG_INFO("[native] GPU marks of one slot ({} marks):{}", marked, list);
      }
      for (uint32_t i = 0; i + 1 < marked; ++i) {
        if (marks[i + 1] >= marks[i] && slot.categories[i] < kGpuCategories) {
          const uint64_t ns = uint64_t(double(marks[i + 1] - marks[i]) * period_mark_ns_);
          gpu_categories_ns_[slot.categories[i]] += ns;
          if (i < slot.labels.size())
            gpu_labels_ns_[(uint64_t(slot.categories[i]) << 32) | slot.labels[i]] += ns;
          ++cat_intervals_[slot.categories[i]];
          cat_max_ns_[slot.categories[i]] = std::max(cat_max_ns_[slot.categories[i]], ns);
          if (ns >= 1000000) ++cat_long_[slot.categories[i]];  // >= 1 ms
          // Diagnostic: the longest single interval between two marks (a stall shows up as one block).
          if (ns > mark_max_ns_) {
            mark_max_ns_ = ns;
            mark_max_desc_ = fmt::format("{:.2f} ms in category {} at mark {}/{} (prev cat {}, next cat {})",
                double(ns) / 1e6, slot.categories[i], i, marked,
                i ? int(slot.categories[i - 1]) : -1,
                i + 1 < marked - 1 ? int(slot.categories[i + 1]) : -1);
          }
        }
      }
      // GPU gap since the end of the previous submission (report). Submissions are read in the order
      // they were sent; if one could not be read, mark_previous_end_ is 0 and that gap is not counted.
      if (mark_previous_end_ != 0) {
        if (marks[0] >= mark_previous_end_) {
          gpu_categories_ns_[kGpuGapBetweenJobs] +=
              uint64_t(double(marks[0] - mark_previous_end_) * period_mark_ns_);
        }
      }
      mark_previous_end_ = marks[marked - 1];
      gpu_ns_ += uint64_t(double(marks[marked - 1] - marks[0]) * period_mark_ns_);
    } else {
      mark_previous_end_ = 0;  // without this submission's timestamps, the next gap is not counted
    }
  } else {
    mark_previous_end_ = 0;
  }
  slot.categories.clear();
  slot.labels.clear();
  slot.descriptions.clear();
  slot.draws_mark.clear();
  ReadStats(slot);
  ReadStatsDraw(slot);
  WriteReads(slot.reads);
}

void TargetsDk::ReadStatsDraw(WorkSlot& slot) {
  if (slot.stats_draw.empty()) {
    return;
  }
  const uint32_t n = uint32_t(slot.stats_draw.size());
  std::vector<uint64_t> values(size_t(n) * kCountersStat, 0);
  if (stats_draw_.Capacity()) {
    // vkGetQueryPoolResults: each counter is its end report minus its start report.
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t query = slot.stats_draw[i].first;
      for (uint32_t k = 0; k < kCountersStat; ++k) {
        values[size_t(i) * kCountersStat + k] = stats_draw_.Value(query * kReportsStat + kCountersStat + k) -
                                                stats_draw_.Value(query * kReportsStat + k);
      }
    }
    // GPU time per (category, full pixel shader number).
    std::vector<uint64_t> marks(size_t(n) * 2, 0);
    const bool with_marks = marks_draw_.Capacity() && slot.draw_ps.size() == n;
    if (with_marks) {
      const uint32_t first = slot.stats_draw.front().first * 2;
      for (uint32_t i = 0; i < n * 2; ++i) marks[i] = marks_draw_.Ticks(first + i);
    }
    if (slot.draw_ps.size() == n) {
      for (uint32_t i = 0; i < n; ++i) {
        CostShader& cost = cost_per_shader_[slot.draw_ps[i]];
        if (with_marks && marks[i * 2 + 1] > marks[i * 2]) {
          const uint64_t ns = uint64_t(double(marks[i * 2 + 1] - marks[i * 2]) * period_mark_ns_);
          cost.ns += ns;
          if (ns >= cost.draw_more_expensive_ns && i < slot.draw_vs.size()) {
            cost.draw_more_expensive_ns = uint32_t(std::min<uint64_t>(ns, UINT32_MAX));
            cost.vs = slot.draw_vs[i];
          }
        }
        cost.vertices += values[i * kCountersStat + 0];
        cost.primitives += values[i * kCountersStat + 1];
        cost.fragments += values[i * kCountersStat + 2];
        ++cost.draws;
      }
    }
    // A diagnostic frame may span several submissions: the first one of each window counts.
    if (!read_window_) {
      read_window_ = true;
      ++cost_frames_;
    }
  }
  slot.stats_draw.clear();
  slot.draw_ps.clear();
  slot.draw_vs.clear();
}

void TargetsDk::ReportCostPerShader() {
  if (cost_per_shader_.empty() || !cost_frames_) {
    return;
  }
  constexpr double kScale = 1.627;
  std::vector<std::pair<uint32_t, CostShader>> order(cost_per_shader_.begin(), cost_per_shader_.end());
  std::sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
  std::array<double, kGpuCategories> per_category{};
  double total = 0.0;
  uint64_t draws = 0, fragments = 0;
  for (const auto& [key, cost] : order) {
    const double ms = double(cost.ns) * kScale / 1e6 / double(cost_frames_);
    per_category[(key >> 24) % kGpuCategories] += ms;
    total += ms;
    draws += cost.draws;
    fragments += cost.fragments;
  }
  std::string categories;
  for (uint32_t c = 0; c < kGpuCategories; ++c) {
    if (per_category[c] >= 0.05) categories += fmt::format(" c{} {:.1f}", c, per_category[c]);
  }
  REXLOG_INFO("[native] per-draw GPU time ({} diagnostic frames, serialized): {:.1f} ms/frame, {} draws, "
              "{:.2f} Mfrag; by category (ms):{}",
              cost_frames_, total, draws / cost_frames_,
              double(fragments) / 1e6 / double(cost_frames_), categories);
  const size_t n = std::min<size_t>(order.size(), 25);
  for (size_t i = 0; i < n; ++i) {
    const auto& [key, cost] = order[i];
    const double ms = double(cost.ns) * kScale / 1e6 / double(cost_frames_);
    const uint32_t ps = key & 0xFFFFFF;
    REXLOG_INFO("[native]   #{} PS n{} cat {}: {:.2f} ms/frame, {} draws, {:.3f} Mfrag, {:.1f} ns/frag, "
                "{} vertices, {} clip prims, costliest draw {:.2f} ms with VS n{}",
                i + 1, ps ? int(ps) - 1 : -1, key >> 24, ms, cost.draws / cost_frames_,
                double(cost.fragments) / 1e6 / double(cost_frames_),
                cost.fragments ? double(cost.ns) * kScale / double(cost.fragments) : 0.0,
                cost.vertices / cost_frames_, cost.primitives / cost_frames_,
                double(cost.draw_more_expensive_ns) * kScale / 1e6, cost.vs ? int(cost.vs) - 1 : -1);
  }
  cost_per_shader_.clear();
  cost_frames_ = 0;
}

void TargetsDk::ReadStats(WorkSlot& slot) {
  if (slot.stats.empty()) {
    return;
  }
  const uint32_t n = uint32_t(slot.stats.size());
  std::array<uint64_t, kStatsPerSlot * kCountersStat> values{};
  if (stats_.Capacity()) {
    // vkGetQueryPoolResults: each counter is its end report minus its start report.
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t query = slot.stats[i].first;
      for (uint32_t k = 0; k < kCountersStat; ++k) {
        values[i * kCountersStat + k] =
            stats_.Value(query * kReportsStat + kCountersStat + k) - stats_.Value(query * kReportsStat + k);
      }
    }
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t c = slot.stats[i].second;
      if (c < kGpuCategories) {
        vertices_category_[c] += values[i * kCountersStat + 0];
        primitives_category_[c] += values[i * kCountersStat + 1];
        fragments_category_[c] += values[i * kCountersStat + 2];
      }
    }
  }
  slot.stats.clear();
}

bool TargetsDk::SendWork(bool wait) {
  if (recording_) {
    ++report_sends_;
    if (draws_) {
      draws_->BeforeSend();  // closes the pass and publishes the upload buffer
    }
    recording_ = false;
    WorkSlot& slot = slots_[slot_];
    const DkQueue queue = sa2::dk::Queue();
    if (recording_upload_) {
      recording_upload_ = false;
      // Make uploaded texture texels and attachment initialization visible to
      // the work command buffer in this same submission.
      sa2::dk::Barrier(commands_upload_, sa2::dk::Hazard::UploadToSample);
      // deko3d: the upload list goes to the queue first and the work list's fence covers both (one submission).
      dkQueueSubmitCommands(queue, slot.upload.Finish());
    }
    if (queries_.Capacity()) {
      // Final mark: closes the last span (MarkGpu leaves room for it).
      WorkSlot& marked = slots_[slot_];
      queries_.Report(commands_work_, slot_ * kMarksPerSlot + uint32_t(marked.categories.size()),
                      DkCounter_Timestamp);
      marked.categories.push_back(kGpuEnd);
      marked.marks_written = std::max(marked.marks_written, uint32_t(marked.categories.size()));
    }
    slot.dk_order = sa2::dk::Submit(slot.work.Finish());
    slot.pending = true;
    slot.order = ++sends_;
    slot.sent = std::chrono::steady_clock::now();
    // deko3d: the images destroyed while this slot recorded go after its submission.
    for (const sa2::dk::ImageId id : slot.destroy_after_send) sa2::dk::DestroyImage(id, slot.dk_order);
    slot.destroy_after_send.clear();
    // Read-backs recorded in this submission are written when it finishes.
    slot.reads.insert(slot.reads.end(), pending_reads_.begin(),
                      pending_reads_.end());
    pending_reads_.clear();
  }
  if (wait) {
    Complete(slots_[slot_]);
  }
  return true;
}

void TargetsDk::ReportWaitsGpu() {
  std::string list;
  if (parked_resolved_stores_ || parked_resolved_reuses_)
    list += fmt::format(" (banjo parked resolves: {} stored, {} reused, {} parked)", parked_resolved_stores_,
                        parked_resolved_reuses_, parked_resolved_.size());
  parked_resolved_stores_ = parked_resolved_reuses_ = 0;
  for (size_t i = 1; i < waits_gpu_reason_.size(); ++i) {
    if (waits_gpu_reason_[i]) list += fmt::format(" site{}={}", i, waits_gpu_reason_[i]);
    waits_gpu_reason_[i] = 0;
  }
  // sa2-nx: printed whether or not any EDRAM transfer happened (with the aliased clears redirected there are none).
  if (edram4_redirected_color0_tiles_ || edram4_redirected_)
    REXLOG_INFO("[native] EDRAM mode4 redirected clears: {} depth clears issued as {} merged clears; color 0 alias "
                "clears into their 1x owners: {} tiles in {} clears", edram4_redirected_, edram4_redirected_merged_,
                edram4_redirected_color0_tiles_, edram4_redirected_color0_areas_);
  edram4_redirected_color0_tiles_ = edram4_redirected_color0_areas_ = edram4_redirected_merged_ = 0;
  if (const uint64_t memo_total = edram4_memo_hits_ + edram4_memo_misses_ + edram4_memo_ineligible_) {
    REXLOG_INFO("[native] EDRAM draw memo (sa2_cpu_ring_edram_memo {}): {} draws prepared from the memo, {} by the "
                "full path, {} not eligible ({:.1f} % from the memo); {} checked against the full path",
                g_edram_memo.active && edram4_memo_on_ ? "on" : "off", edram4_memo_hits_, edram4_memo_misses_,
                edram4_memo_ineligible_, 100.0 * double(edram4_memo_hits_) / double(memo_total),
                edram4_memo_checked_);
    edram4_memo_hits_ = edram4_memo_misses_ = edram4_memo_ineligible_ = edram4_memo_checked_ = 0;
  }
  if (!edram4_operations_.empty()) {
    std::string ops;
    for (const auto& [name, n] : edram4_operations_) ops += fmt::format(" {}={}", name, n);
    edram4_operations_.clear();
    REXLOG_INFO("[native] EDRAM mode4 operations since last report:{}; sync cache: {} tiles skipped, "
                "{} transferred; batched: {} runs, {} import passes and {} compute barrier pairs saved",
                ops, edram4_sync_skipped_, edram4_sync_copied_, edram4_batch_spans_,
                edram4_batch_saved_passes_, edram4_batch_saved_barriers_);
    edram4_sync_skipped_ = edram4_sync_copied_ = 0;
    edram4_batch_spans_ = edram4_batch_saved_passes_ = edram4_batch_saved_barriers_ = 0;
    static const char* kCaller[4] = {"draw", "clear", "color-resolve", "depth-resolve"};
    static const char* kKind[5] = {"depth-repr-switch", "depth-depth", "color->depth", "depth->color",
                                   "color->color"};
    std::string causes;
    for (uint32_t m = 0; m < 4; ++m)
      for (uint32_t k = 0; k < 5; ++k)
        if (edram4_transfer_ops_[m][k])
          causes += fmt::format(" {}/{}={}ops/{}tiles", kCaller[m], kKind[k], edram4_transfer_ops_[m][k],
                                edram4_transfer_tiles_[m][k]);
    std::string full;
    for (uint32_t k = 0; k < 5; ++k)
      if (edram4_transfer_full_[k]) full += fmt::format(" {}={}", kKind[k], edram4_transfer_full_[k]);
    REXLOG_INFO("[native] EDRAM mode4 transfer causes:{}; whole-target runs:{}", causes,
                full.empty() ? " none" : full);
    if (REXCVAR_GET(sa2_native_conversion_frag)) {
      REXLOG_INFO("[native] EDRAM mode4 color conversions as fragment passes: {} done, {} declined (compute) since "
                  "the last report", conv_color_frag_uses_ - conv_color_frag_uses_report_,
                  conv_color_frag_rejections_ - conv_color_frag_rejections_report_);
      {
        std::string m;
        for (uint32_t k = 0; k < conv_color_frag_reasons_.size(); ++k)
          if (conv_color_frag_reasons_[k]) m += fmt::format(" [{}: {}]", k, conv_color_frag_reasons_[k]);
        if (!m.empty()) REXLOG_INFO("[native] EDRAM mode4 fragment conversions declined by (return #):{}", m);
        conv_color_frag_reasons_ = {};
      }
      conv_color_frag_uses_report_ = conv_color_frag_uses_;
      conv_color_frag_rejections_report_ = conv_color_frag_rejections_;
    }
    std::vector<std::pair<std::string, ParEDRAM4>> pairs(edram4_transfer_pairs_.begin(),
                                                         edram4_transfer_pairs_.end());
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.second.tiles > b.second.tiles; });  // ds2-nx: by tiles (the cost)
    std::string top;
    for (size_t i = 0; i < std::min<size_t>(pairs.size(), 10); ++i)
      top += fmt::format(" [{} {}ops/{}tiles]", pairs[i].first, pairs[i].second.ops, pairs[i].second.tiles);
    for (size_t i = 0; i < std::min<size_t>(pairs.size(), 8); ++i)
    {
      std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> d(pairs[i].second.draws.begin(),
                                                                           pairs[i].second.draws.end());
      std::sort(d.begin(), d.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
      std::string list;
      for (size_t j = 0; j < std::min<size_t>(d.size(), 5); ++j)
        list += fmt::format(" {}={}ops/{}t", d[j].first, d[j].second.first, d[j].second.second);
      REXLOG_INFO("[native] EDRAM mode4 pair {} draws:{} | first: {}", pairs[i].first, list,
                  pairs[i].second.example);
    }
    REXLOG_INFO("[native] EDRAM mode4 top transfer pairs:{}; proven-overwrite binds {}; redirected depth "
                "clears {} ({} tiles), declined {}; redirected stencil clears {} ({} tiles), declined {}; "
                "GPU marks dropped {}; claimed by the drawn view (ds2_clear_depth_claim) {}", top, edram4_overwrites_,
                edram4_redirected_, edram4_redirected_tiles_, edram4_redirected_no_,
                edram4_stencil_redirected_, edram4_stencil_redirected_tiles_, edram4_stencil_redirected_no_,
                exhausted_marks_, edram4_claimed_clears_);
    exhausted_marks_ = 0;
    edram4_claimed_clears_ = 0;
    if (!edram4_stencil_no_reasons_.empty()) {
      std::string m;
      for (const auto& [k, v] : edram4_stencil_no_reasons_) m += fmt::format(" [{}: {}]", k, v);
      REXLOG_INFO("[native] EDRAM mode4 stencil clear redirect declined by:{}", m);
      edram4_stencil_no_reasons_.clear();
    }
    for (const auto& [k, v] : edram4_depth_no_fast_)
      edram4_depth_no_reasons_[fmt::format("{} (depth info {:08X})", k.first, k.second)] += v;
    edram4_depth_no_fast_.clear();
    if (!edram4_depth_no_reasons_.empty()) {
      std::string m;
      for (const auto& [k, v] : edram4_depth_no_reasons_) m += fmt::format(" [{}: {}]", k, v);
      REXLOG_INFO("[native] EDRAM mode4 depth clear redirect declined by:{}", m);
      edram4_depth_no_reasons_.clear();
    }
    if (edram4_redirected_color_tiles_)
      REXLOG_INFO("[native] EDRAM mode4 redirected clears into color owners: {} tiles", edram4_redirected_color_tiles_);
    edram4_redirected_color_tiles_ = 0;
    if (edram4_consumer_uses_)
      REXLOG_INFO("[native] EDRAM mode4 redirected clears straight into their consumer: {}", edram4_consumer_uses_);
    edram4_consumer_uses_ = 0;
    edram4_stencil_redirected_ = edram4_stencil_redirected_tiles_ = edram4_stencil_redirected_no_ = 0;
    REXLOG_INFO("[native] GPU longest single interval since last report: {}", mark_max_desc_.empty() ? "-" : mark_max_desc_);
    {
      std::string cats;
      for (uint32_t c = 0; c < kGpuCategories; ++c)
        if (cat_intervals_[c])
          cats += fmt::format(" [cat {}: {} intervals, {} >= 1 ms, max {:.2f} ms]", c, cat_intervals_[c],
                              cat_long_[c], double(cat_max_ns_[c]) / 1e6);
      REXLOG_INFO("[native] GPU intervals by category:{}", cats);
      std::vector<std::pair<uint64_t, uint64_t>> top(gpu_labels_ns_.begin(), gpu_labels_ns_.end());
      std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      std::string list;
      for (size_t k = 0; k < std::min<size_t>(top.size(), 16); ++k)
        list += fmt::format(" [cat {} VS{} PS{}: {:.1f} ms]", top[k].first >> 32, (top[k].first >> 16) & 0xFFFF,
                             top[k].first & 0xFFFF, double(top[k].second) / 1e6);
      REXLOG_INFO("[native] GPU time by pass (category, first draw's shaders), 10 s:{}", list);
      gpu_labels_ns_.clear();
      cat_intervals_ = {};
      cat_max_ns_ = {};
      cat_long_ = {};
    }
    mark_max_ns_ = 0;
    mark_max_desc_.clear();
    REXLOG_INFO("[native] EDRAM mode4 CPU bookkeeping since last report: sync {} calls / {} tiles, publish {} calls "
                "/ {} tiles; O(1) fast paths: sync {}, publish {}", edram4_sync_calls_, edram4_sync_visited_,
                edram4_pub_calls_, edram4_pub_visited_, edram4_sync_fast_, edram4_pub_fast_);
    edram4_sync_fast_ = edram4_pub_fast_ = 0;
    REXLOG_INFO("[native] EDRAM mode4 lazy stencil: {} depth-only imports, {} returned home (stencil kept), "
                "{} inexact, {} late stencil users, {} exports of deferred stencil, {} tiles inherited by depth-only "
                "draws, {} before a full stencil replace, {} late fetches ({} tiles)", edram4_stencil_skipped_,
                edram4_stencil_returned_, edram4_stencil_inexact_, edram4_stencil_late_,
                edram4_stencil_exported_, edram4_stencil_inherited_, edram4_stencil_replaced_,
                edram4_stencil_fetched_, edram4_stencil_fetched_tiles_);
    edram4_stencil_inherited_ = edram4_stencil_replaced_ = edram4_stencil_fetched_ =
        edram4_stencil_fetched_tiles_ = 0;
    std::string nine;
    for (const auto& [k, v] : edram4_import9_pairs_) nine += fmt::format(" [{} {}ops/{}t]", k, v.first, v.second);
    REXLOG_INFO("[native] EDRAM mode4 9-pass imports:{}; stencil bit passes skipped (bit never set): {}; "
                "copy-engine stencil imports {} ({} tiles)",
                nine.empty() ? " none" : nine, edram4_bits_skipped_, edram4_stencil_copies_,
                edram4_stencil_copies_tiles_);
    edram4_bits_skipped_ = edram4_stencil_copies_ = edram4_stencil_copies_tiles_ = 0;
    edram4_import9_pairs_.clear();
    edram4_stencil_skipped_ = edram4_stencil_returned_ = edram4_stencil_inexact_ = edram4_stencil_late_ =
        edram4_stencil_exported_ = 0;
    edram4_sync_calls_ = edram4_sync_visited_ = edram4_pub_calls_ = edram4_pub_visited_ = 0;
    edram4_overwrites_ = edram4_redirected_ = edram4_redirected_tiles_ = edram4_redirected_no_ = 0;
    edram4_transfer_pairs_.clear();
    edram4_transfer_ops_ = {}; edram4_transfer_tiles_ = {}; edram4_transfer_full_ = {};
  }
  uint64_t full_count = 0, ns_full = 0;
  if (draws_) {
    const auto e = draws_->Stats();
    full_count = e.full_sends - report_full_sends_;
    ns_full = e.ns_full_sends - report_ns_full_sends_;
    report_full_sends_ = e.full_sends;
    report_ns_full_sends_ = e.ns_full_sends;
  }
  REXLOG_INFO("[native] submissions since last report: {} ({} for a full upload buffer, {:.1f} ms inside "
              "them, {} partial); slot fence waits {} ({:.1f} ms); full GPU drains:{}",
              report_sends_, full_count, double(ns_full) / 1e6, report_partial_sends_,
              waits_gpu_ - report_waits_gpu_, double(ns_waits_gpu_ - report_ns_waits_gpu_) / 1e6,
              list.empty() ? " none" : list);
  report_sends_ = 0;
  report_partial_sends_ = 0;
  report_waits_gpu_ = waits_gpu_;
  report_ns_waits_gpu_ = ns_waits_gpu_;
}

void TargetsDk::WaitGpu() {
  if (recording_) {
    if (!SendWork(false)) {
      resolved_pool_device_error_ = true;
      REXLOG_ERROR("[native] GPU submission failed; resolved allocation cache disabled");
      return;
    }
  }
  for (WorkSlot& slot : slots_) {
    Complete(slot);
  }
  WaitOutputs();
}

void TargetsDk::WaitOutputs() {
  for (uint32_t i = 0; i < kSlotsOutput; ++i) {
    if (pending_outputs_[i]) {
      if (!sa2::dk::WaitOrder(orders_output_[i])) {
        resolved_pool_device_error_ = true;
        REXLOG_ERROR("[native] [dk] output fence wait failed (order {}, queue error); resolved allocation cache "
                     "disabled", orders_output_[i]);
        return;
      }
      // vkResetFences: nothing to reset for an order.
      pending_outputs_[i] = false;
    }
  }
}

}  // namespace sa2::native

// ---- sa2_native_targets.cpp 11723-11734: the factory ---------------------------------------------------------

std::unique_ptr<sa2::native::TargetsNative> sa2::native::TargetsNative::CreateDeko3d(rex::memory::Memory* memory) {
  if (!memory) {
    return nullptr;
  }
  // deko3d: the device and the queue (Graphics | Compute, 1024 compute jobs) are the base layer's, created once.
  if (!sa2::dk::Ready() && !sa2::dk::Initialize()) {
    REXLOG_ERROR("[native] [dk] targets: could not create the deko3d device and queue");
    return nullptr;
  }
  auto targets = std::make_unique<TargetsDk>(memory);
  if (!targets->Initialize()) {
    REXLOG_ERROR("[native] targets: could not prepare the target presentation");
    return nullptr;
  }
  return targets;
}

#else  // !SA2_DK_AVAILABLE

std::unique_ptr<sa2::native::TargetsNative> sa2::native::TargetsNative::CreateDeko3d(rex::memory::Memory*) {
  REXLOG_ERROR("[native] [dk] the deko3d renderer is Switch only");
  return nullptr;
}

#endif  // SA2_DK_AVAILABLE
