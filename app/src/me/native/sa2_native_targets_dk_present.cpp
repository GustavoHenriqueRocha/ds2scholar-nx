// SA2 - native renderer on deko3d (ds2-deko3d): TargetsDk, region "alvos-present". Translation of TargetsVulkan
// (sa2_native_targets.cpp) 2614-3852: gamma ramp, live rescale (ds2_dynamic_resolution_keep), Present and the
// [hitch] report, the per-draw diagnostic window, Draw, the interface lent to the draws, the resolved texture
// lookups and logical crops, the lazy front (sa2_native_lazy_front); 9957-10020: the resolve / clear bookkeeping;
// 10866-11400: the copy inventory, the read-backs (sa2_native_resolved_read_texels) and the output pass. Same logic,
// order, counters and logs as the Vulkan code; the GPU calls go to:
//   vkCmdCopyImage / vkCmdBlitImage  -> dkCmdBufCopyImage (copy engine) / dkCmdBufBlitImage (2D engine)
//   vkCmdCopyImageToBuffer           -> dkCmdBufCopyImageToBuffer into a CPU-cached block (sa2::dk::kReadback)
//   vkCmdPipelineBarrier             -> sa2::dk::Barrier(cmd, Hazard::...) named by the dependency it serves
//   the output render pass + pipelines + 3 fenced slots + the SDK presenter's effects and swapchain
//                                    -> PaintOutput: one command list per output slot (sa2::dk orders), the
//                                       shaders_dk/output_*.{vert,frag} and sa2_output_ramp_gamma_* DKSH, the
//                                       effect chain of sa2_native_dk_output_plan.h, sa2::dk::Window.
// The Present is synchronous on the ring thread (no present thread: ds2-deko3d 04-alvos-present.md 2.3). The window
// has 4 images and interval 0; the 30 fps cap still goes through __wrap_nwindowQueueBuffer (switch_perf.cpp), which
// the link wraps for libdeko3d's nwindowQueueBuffer too. Members and declarations: sa2_native_targets_dk_present.inc.
// Report: ds2-deko3d docs/pesquisas/traducao/04-alvos-present.md, section "Fase C".

#include "sa2_native_targets_dk.h"

#if SA2_DK_AVAILABLE

#include "sa2_native_dk_output_plan.h"
#include "sa2_native_dk_rescale_keep.h"
#include "sa2_native_dk_shaders.h"
#include "sa2_native_draws.h"
#include "sa2_native_shaders.h"  // SA2_REPORT_RING

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>  // present_effect, present_fsr_* (defined by the SDK presenter, which stays linked)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sa2::native {

namespace {

namespace gr = rex::graphics;

// vkCmdCopyImage of one VkImageCopy (color, one layer) -> dkCmdBufCopyImage (copy engine: whole texels).
void CopyImageDk(DkCmdBuf cmd, sa2::dk::ImageId source, sa2::dk::ImageId target, const VkImageCopy& c) {
  const DkImageView source_view = sa2::dk::View(source);
  const DkImageView target_view = sa2::dk::View(target);
  const DkImageRect s{uint32_t(c.srcOffset.x), uint32_t(c.srcOffset.y), 0, c.extent.width, c.extent.height, 1};
  const DkImageRect d{uint32_t(c.dstOffset.x), uint32_t(c.dstOffset.y), 0, c.extent.width, c.extent.height, 1};
  dkCmdBufCopyImage(cmd, &source_view, &s, &target_view, &d, 0);
}

const char* NameEffect(dkoutput::Effect effect) {
  switch (effect) {
    case dkoutput::Effect::kFsr: return "fsr";
    case dkoutput::Effect::kCas: return "cas (RCAS in its place)";
    case dkoutput::Effect::kBilinear: break;
  }
  return "bilinear";
}

}  // namespace

void TargetsDk::RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) {
  ramp_game_ = ramp;
  RecomputeRamp();
}


// The output table = the game's ramp in 0-1, followed by the neutral mix (saturation 1, vibrance 0,
// 1 / gamma 1) and effects (no vignette, no scanlines) that the output shader still reads.
void TargetsDk::RecomputeRamp() {
  for (uint32_t i = 0; i < 256; ++i) {
    for (uint32_t c = 0; c < 3; ++c) {
      ramp_values_[i * 4 + c] = float(ramp_game_[i][c] & 0x3FF) / 1023.0f;
    }
    ramp_values_[i * 4 + 3] = 1.0f;
  }
  const size_t extra = 256 * 4;
  ramp_values_[extra] = 1.0f;      // saturation
  ramp_values_[extra + 1] = 0.0f;  // vibrance
  ramp_values_[extra + 2] = 1.0f;  // 1 / gamma
  ramp_values_[extra + 3] = 0.0f;
  ramp_values_[extra + 4] = 0.0f;  // vignette
  ramp_values_[extra + 5] = 0.0f;  // scanlines
  ramp_values_[extra + 6] = 0.0f;
  ramp_values_[extra + 7] = 0.0f;
  ++version_ramp_;
}


// ---- sa2-nx render resolution: live rescale (docked <-> handheld) -------------------------------------------------
//
// Called at the end of each Present, after the Swap's image was painted: when the wanted scale (the console's mode and
// sa2_render_height_*) differs from the one the images were made at, every render target, depth target and resolved
// texture gets a new VkImage at the new scale with its content scaled into it (color linear, depth nearest), and
// keeps everything else: its ImageNative (so every pointer, EDRAM tile owner and memo stays valid), its guest size,
// its address. Deferred work that names image handles (lazy front copies, resolve aliases) is recorded first. The
// GPU is drained before and after (a one-frame hitch, only when the mode changes).
// The device memory of the images the render scale applies to (render targets, depth targets, resolved and front
// textures): for the [res] lines. Only at a rescale and in the 10 s report, when the scale or MSAA is in use.
uint64_t TargetsDk::BytesScaledImages() {
  uint64_t bytes = 0;
  const auto add = [&](const Image& image) {
    if (image.image == sa2::dk::kNoImage) return;
    bytes += sa2::dk::Bytes(image.image);  // was vkGetImageMemoryRequirements: the layout's size
  };
  for (const auto& [key, image] : targets_) add(image);
  for (const auto& [key, image] : depths_) add(image);
  for (const auto& [key, resolved] : resolved_) add(resolved.image);
  for (const auto& [key, resolved] : parked_resolved_) add(resolved.image);
  for (const ImageFront& front : fronts_images_) add(front.image);
  return bytes;
}


void TargetsDk::RescaleIfWanted() {
  const uint32_t wanted = sa2::render::WantedScaleQuarters();
  if (wanted != scale_q_) RescaleAll(wanted);
}


// The Vulkan objects of an image that is being replaced (not its bookkeeping: the ImageNative lives on).
// deko3d: the view caches keyed by the image (views_raw64_edram_, views_depth_edram_, views_raw64_rt_edram_) and
// the framebuffers (framebuffers_conv_color_frag_, framebuffers_import_depth_edram_, framebuffers_depth_resolve_ms_)
// are gone: views are made on the spot and render targets are bound per pass. What is left is the id: the draws
// forget it, and its memory goes once the GPU is past everything recorded so far (DestroyImage's default order; the
// Vulkan callers had already made sure the GPU no longer used it, so this is the same or later).
void TargetsDk::ReleaseHandles(const Image& old) {
  if (draws_ && old.image != sa2::dk::kNoImage) draws_->ForgetImage(old.image);
  if (old.image != sa2::dk::kNoImage) sa2::dk::DestroyImage(old.image);
}


void TargetsDk::ReleaseAlternates() {
  for (const auto& [key, alt] : alternates_) {
    Image old;
    old.image = alt.image;
    ReleaseHandles(old);
  }
  alternates_.clear();
}


// Destroy's companion: the twin of an image that goes away goes too. Only reached once that image is no longer
// used (Destroy's callers wait or check first), and the twin was swapped out earlier, so it is older still.
void TargetsDk::ReleaseAlternateOf(sa2::dk::ImageId image) {
  if (alternates_.empty() || image == sa2::dk::kNoImage) return;
  const auto it = alternates_.find(image);
  if (it == alternates_.end()) return;
  Image old;
  old.image = it->second.image;
  alternates_.erase(it);
  ReleaseHandles(old);
}


void TargetsDk::RescaleKeep(uint32_t q, std::chrono::steady_clock::time_point started, uint32_t q_old, uint64_t bytes_old) {
  if (draws_) draws_->FinishPass();
  while (!pending_fronts_.empty()) RecordCopyFront(pending_fronts_.begin()->first);
  while (!aliases_.empty()) {
    const uint32_t address = aliases_.begin()->first;
    if (!MaterializeAlias(address)) DropAlias(address);
  }
  last_resolve_.valid = false;
  edram4_memo_.valid = false;
  if (resolved_pool_device_error_ || !Record()) return;
  // Sleeping resolved images and crops stay: WakeResolved matches the scale, crops copy across scales, and both are
  // evicted later the usual way (only once the GPU is done with them).
  scale_q_ = q;
  uint32_t swapped = 0, created = 0, blitted = 0, cleared = 0, failed = 0;
  const auto rescale = [&](Image& image) {
    if (image.image == sa2::dk::kNoImage || image.scale_q == q || image.fixed_native) return;
    AlternateImage now{image.image, image.host_width, image.host_height, image.scale_q};
    Image fresh;
    const auto it = alternates_.find(image.image);
    if (it != alternates_.end() && it->second.scale_q == q) {
      fresh.image = it->second.image;
      fresh.host_width = it->second.host_width;
      fresh.host_height = it->second.host_height;
      fresh.scale_q = it->second.scale_q;
      alternates_.erase(it);
      ++swapped;
    } else {
      if (it != alternates_.end()) {  // a twin at a third scale (docking happened): replace it
        Image old;
        old.image = it->second.image;
        alternates_.erase(it);
        ReleaseHandles(old);
      }
      if (!CreateScaled(fresh, image.width, image.height, image.dk_flags, image.format, image.sample_count, q)) {
        ++failed;
        return;  // keeps its old scale, as in RescaleAll
      }
      ++created;
    }
    const bool depth = IsDepthFormat(image.format);
    // deko3d: no layout transition for the fresh image (UNDEFINED -> GENERAL); the dependency of the blit on the
    // work before it is BarrierBeforeCopyResolve below.
    if (BlitScaledDk(image, fresh, depth)) {
      fresh.prepared = true;
      ++blitted;
    } else {
      fresh.prepared = false;  // Prepare clears it before its next use
      if (image.prepared) ++cleared;
    }
    image.image = fresh.image;
    image.host_width = fresh.host_width;
    image.host_height = fresh.host_height;
    image.scale_q = fresh.scale_q;
    image.prepared = fresh.prepared;
    alternates_[image.image] = now;  // the old handles wait for the next switch back
  };
  for (auto& [key, image] : targets_) rescale(image);
  for (auto& [key, image] : depths_) rescale(image);
  for (auto& [key, resolved] : resolved_) rescale(resolved.image);
  for (auto& [key, resolved] : parked_resolved_) rescale(resolved.image);
  for (ImageFront& front : fronts_images_) rescale(front.image);
  for (auto& [key, resolved] : resolved_) ++resolved.revision;
  ++edram4_epoch_;
  if (draws_) {
    draws_->NotifyRescale(q_old, q);
    draws_->InvalidateTextures();
  }
  Record();
  ++rescales_;
  sa2::render::NoteRescale();
  REXLOG_INFO("[res] render scale {}/4 -> {}/4 kept (ds2_dynamic_resolution_keep): {} swapped, {} created, {} with "
              "their content scaled, {} cleared, {} failed, in {:.1f} ms; {:.1f} MB of images in use, {} twins",
              q_old, q, swapped, created, blitted, cleared, failed,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
              double(BytesScaledImages()) / 1048576.0, alternates_.size());
  (void)bytes_old;
}


void TargetsDk::RescaleAll(uint32_t q) {
  const auto started = std::chrono::steady_clock::now();
  const uint32_t q_old = scale_q_;
  const uint64_t bytes_old = BytesScaledImages();
  // ds2scholar-nx (ds2_dynamic_resolution_keep): in handheld each image keeps its other-scale twin (alternates_), so a
  // switch swaps handles and blits the content: no GPU wait, no allocation after the first time, nothing destroyed
  // that queued work may still read. Docking takes the old path below, which waits for the GPU and frees the twins.
  // deko3d: also the fixed mode's switches, not only the dynamic resolution's (sa2_native_dk_rescale_keep.h).
  const bool keep = KeepScaleTwins(REXCVAR_GET(ds2_dynamic_resolution_keep), sa2::render::Docked());
  if (keep) {
    RescaleKeep(q, started, q_old, bytes_old);
    return;
  }
  if (draws_) draws_->FinishPass();
  // Deferred copies name image handles: recorded now, at the old scale, so none refers to a replaced image.
  while (!pending_fronts_.empty()) RecordCopyFront(pending_fronts_.begin()->first);
  while (!aliases_.empty()) {
    const uint32_t address = aliases_.begin()->first;
    if (!MaterializeAlias(address)) DropAlias(address);  // (the guard has already turned the aliases off)
  }
  last_resolve_.valid = false;
  edram4_memo_.valid = false;
  // Everything recorded so far runs on the old images; then nothing uses them.
  ++waits_gpu_reason_[9];  // site9: render scale change (sa2-nx)
  WaitGpu();
  if (resolved_pool_device_error_ || !Record()) return;
  ReleaseAlternates();  // ds2_dynamic_resolution_keep: the GPU is idle, the twins can go
  // Idle pool allocations and logical crops are caches: dropped, made again on demand at the new scale.
  while (!resolved_sleeping_.empty()) EvictResolvedSleeping(resolved_sleeping_.size() - 1);
  for (auto& clip : resolved_clips_) {
    if (draws_) draws_->ForgetImage(clip.image.image);
    Destroy(clip.image);
  }
  resolved_clips_.clear();
  clips_bytes_ = 0;
  scale_q_ = q;
  std::vector<Image> olds;
  uint32_t images = 0, blitted = 0, cleared = 0, failed = 0;
  const auto rescale = [&](Image& image) {
    if (image.image == sa2::dk::kNoImage || image.scale_q == q || image.fixed_native) return;
    Image fresh;
    if (!CreateScaled(fresh, image.width, image.height, image.dk_flags, image.format, image.sample_count, q)) {
      ++failed;
      return;  // keeps its old scale: transfers with it decline (scale check), the next rescale tries again
    }
    ++images;
    const bool depth = IsDepthFormat(image.format);
    // deko3d: no layout transition for the fresh image (see RescaleKeep).
    // The content, scaled: color with a linear filter, depth / stencil and float formats with the nearest texel
    // (depth needs blit support and a TRANSFER_DST destination: without them it starts cleared to the far plane,
    // which every SA2 frame clears anyway). Multisampled images cannot be blitted: they start cleared.
    if (BlitScaledDk(image, fresh, depth)) {
      fresh.prepared = true;
      ++blitted;
    } else if (image.prepared) {
      fresh.prepared = false;  // Prepare clears it (color 0, depth far) before its next use
      ++cleared;
    }
    olds.push_back(Image{});
    Image& old = olds.back();
    old.image = image.image;
    image.image = fresh.image;
    image.host_width = fresh.host_width;
    image.host_height = fresh.host_height;
    image.scale_q = fresh.scale_q;
    image.prepared = fresh.prepared;
  };
  for (auto& [key, image] : targets_) rescale(image);
  for (auto& [key, image] : depths_) rescale(image);
  for (auto& [key, resolved] : resolved_) rescale(resolved.image);
  for (auto& [key, resolved] : parked_resolved_) rescale(resolved.image);
  for (ImageFront& front : fronts_images_) rescale(front.image);
  for (auto& [key, resolved] : resolved_) ++resolved.revision;  // new images: crops and caches resample them
  // The blits run, then the old images go.
  ++waits_gpu_reason_[9];
  WaitGpu();
  for (const Image& old : olds) ReleaseHandles(old);
  ++edram4_epoch_;
  if (draws_) {
    draws_->NotifyRescale(q_old, q);
    draws_->InvalidateTextures();
  }
  Record();
  ++rescales_;
  sa2::render::NoteRescale();
  REXLOG_INFO("[res] render scale {}/4 -> {}/4 ({}x{} -> {}x{} for 1280x720): {} images recreated ({} with their "
              "content scaled, {} cleared, {} failed) in {:.1f} ms; {:.1f} -> {:.1f} MB of images",
              q_old, q, ScaleSize(1280, q_old), ScaleSize(720, q_old), ScaleSize(1280, q), ScaleSize(720, q), images,
              blitted, cleared, failed,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
              double(bytes_old) / 1048576.0, double(BytesScaledImages()) / 1048576.0);
}


bool TargetsDk::Present(rex::ui::Presenter* presenter, const TextureSwap& swap, uint32_t width,
               uint32_t height) {
  edram4_cleared_in_frame_.clear();  // redirected clear ordinals restart every frame
  // The per-draw diagnostic window opens here, on the PM4 ring thread, which is the one that records the
  // draws. Inside the paint call it would be another thread and the window would catch an arbitrary piece
  // of the frame (33 of 500 shadow draws were measured).
  OpenWindowDiagnostic();
  RexSwitchPerfCount(0);  // the profiler's "game N fps"
  // Interval since the previous Swap, for the stutter report below.
  {
    const auto now = std::chrono::steady_clock::now();
    if (last_swap_ != std::chrono::steady_clock::time_point{}) {
      const double ms = std::chrono::duration<double, std::milli>(now - last_swap_).count();
      // Frames over 45 ms go to stack sampling. The window starts one normal frame (33 ms) earlier, because
      // the game thread prepares the frame ahead of the ring. Ticks at 19.2 MHz.
      if (ms > 45.0) {
        uint64_t end = 0;
#if defined(__aarch64__)
        asm volatile("mrs %0, cntpct_el0" : "=r"(end));  // libnx's armGetSystemTick
#endif
        const uint64_t long_value = uint64_t((ms + 33.3) * 19200.0);
        RexSwitchPerfHitch(end > long_value ? end - long_value : 0, end);
      }
      /*
       * What happens in a frame that runs long.
       *
       * Measured on the console: 7.3 % of frames exceed 50 ms and there are peaks of 2.1 seconds. Pipelines
       * are stable (116, 11 ms) and the texture cache no longer evicts, so the cause is something else.
       *
       * When a frame runs long, what that frame did (not the running total) is dumped. If the culprit is
       * loading textures, or creating pipelines, or a big copy, or simply that the game sent three times as
       * many draws, it shows here. It only writes when there is a stutter, so it costs nothing in the normal
       * case.
       */
      /*
       * The previous dump was not enough, and this is why.
       *
       * The stutters are 60-70 ms frames (median 67, against an average of 39.34) with an exactly normal
       * render load: 14 copies, 4 clears, 2 swaps, 1 restore, the same as any other frame. And they are not
       * quantized to the vblank (34 of them fall between 60.0 and 62.5 ms, which is not a multiple of
       * 16.67). So there are ~26 extra ms that are not in the drawing and the old dump did not see them.
       *
       * So now the time breakdown is dumped, not only the work count:
       *   - GPU work of that frame, with the timestamps we already measure. If it goes up to 60 ms it is
       *     scene load; if it stays at 33 the time went to the CPU or to waiting.
       *   - how long the thread took to record and how long to present.
       *   - new textures and pipelines, the usual suspects of an isolated peak.
       * With that, the log says where the 26 ms come from without guessing.
       */
      // The cap is 1000 warnings so that a whole session fits; it only writes when there is a stutter, so it
      // costs nothing in the normal case.
      if (ms > 60.0 && warnings_hitch_ < 1000) {
        ++warnings_hitch_;
        /* gpu_ns_ are raw GPU timestamps: multiply by 1.627 to get real milliseconds (NVK timestamp scale). */
        const double gpu_ms = double(gpu_ns_ - hitch_gpu_ns_) / 1e6 * 1.627;
        // The [hitch] lines go to the report thread (SA2_REPORT_RING).
        SA2_REPORT_RING(
            "[hitch] frame of {:.1f} ms (GPU {:.1f} real, record {:.1f}): {} copies, "
            "{} clears, {} swaps, {} restores, {} GPU waits",
            ms, gpu_ms, double(ns_record_ - hitch_ns_record_) / 1e6, copies_ - hitch_copies_,
            cleared_ - cleared_hitch_, swaps_ - hitch_resolves_,
            restores_ - hitch_restores_, waits_gpu_ - hitch_waits_);
        // ds2scholar-nx (2026-10-22, prop-destruction stutters with 80-90 ms of GPU): this frame's GPU time by
        // pass category (the categories of the 10 s "GPU per Swap" report), the ones over 1 ms.
        {
          static constexpr const char* kNames[kGpuCategories] = {
              "other", "shadows", "scene", "640", "320", "smaller", "copies", "clears", "scene_no_z", "gap",
              "edram_import", "edram_export", "edram_alias", "edram_import9", "edram_resolve_depth",
              "edram_resolve_bias"};
          std::string passes;
          for (uint32_t k = 0; k < kGpuCategories; ++k) {
            const double pass_ms = double(gpu_categories_ns_[k] - hitch_gpu_categories_[k]) / 1e6 * 1.627;
            if (pass_ms >= 1.0) passes += fmt::format(" {} {:.1f}", kNames[k], pass_ms);
          }
          SA2_REPORT_RING("[hitch] GPU by pass (real ms):{}", passes.empty() ? std::string(" none over 1 ms") : passes);
        }
        namespace e = sa2::waits;
        // Waits of this frame, in ms (see sa2_waits_hitch.h): the GPU fence and the output slot,
        // and what the ring re-checked of the textures.
        SA2_REPORT_RING(
            "[hitch] waits (ms): GPU fence {:.1f}, output {:.1f} | textures checked {:.1f} MB, "
            "postponed {}",
            double(ns_waits_gpu_ - hitch_ns_waits_gpu_) / 1e6,
            double(ns_wait_output_ - hitch_ns_wait_output_) / 1e6,
            double(e::g_bytes_fingerprint.load(std::memory_order_relaxed) - hitch_bytes_fingerprint_) / 1048576.0,
            e::g_postponed_fingerprints.load(std::memory_order_relaxed) - hitch_postponed_fingerprints_);
        // What the ring spent this frame on.
        const auto dif = [](const std::atomic<uint64_t>& a, uint64_t before) {
          return a.load(std::memory_order_relaxed) - before;
        };
        // And how long the ring waited for the vertex copy thread (WaitUploads). The wait before each
        // submit is inside "working" and the one before returning the read pointer is outside: it is read
        // separately, not added.
        // And what the ring spent creating textures (image, memory and view; "textures" starts afterwards),
        // how many the binding thread bound and how long the ring waited for it
        // (sa2_native_textures_binding_thread).
        SA2_REPORT_RING("[hitch] ring: textures {:.1f} ms ({} uploads, {:.1f} MB; "
                    "{} created; fingerprints {:.1f} + {:.1f} ms, zeroing {:.1f} ms, untiling {:.1f} ms); waiting for the vertex copy thread {:.1f} ms "
                    "({} times), helping it {:.1f} ms ({} copies); "
                    "creating textures {:.1f} ms on the ring, {} bound on the thread, waiting for it {:.1f} ms",
                    double(dif(e::g_ns_textures, hitch_ns_textures_)) / 1e6,
                    dif(e::g_textures_uploads, hitch_textures_uploads_),
                    double(dif(e::g_bytes_uploaded, hitch_bytes_uploaded_)) / 1048576.0,
                    dif(e::g_created_textures, hitch_created_textures_),
                    double(dif(e::g_ns_raw_fingerprint, hitch_ns_raw_fingerprint_)) / 1e6,
                    double(dif(e::g_ns_data_fingerprint, hitch_ns_data_fingerprint_)) / 1e6,
                    double(dif(e::g_ns_texture_zero, hitch_ns_texture_zero_)) / 1e6,
                    double(dif(e::g_ns_detile, hitch_ns_detile_)) / 1e6,
                    double(dif(e::g_ns_waiting_copies, hitch_ns_waiting_copies_)) / 1e6,
                    dif(e::g_waits_copies, hitch_waits_copies_),
                    double(dif(e::g_ns_helping_copies, hitch_ns_helping_copies_)) / 1e6,
                    dif(e::g_helped_copies, hitch_helped_copies_),
                    double(dif(e::g_ns_create_textures, hitch_ns_create_textures_)) / 1e6,
                    dif(e::g_bound_textures_thread, hitch_bound_textures_thread_),
                    double(dif(e::g_ns_waiting_bindings, hitch_ns_waiting_bindings_)) / 1e6);
      }
      hitch_ns_waits_gpu_ = ns_waits_gpu_;
      hitch_ns_textures_ = sa2::waits::g_ns_textures.load(std::memory_order_relaxed);
      hitch_textures_uploads_ = sa2::waits::g_textures_uploads.load(std::memory_order_relaxed);
      hitch_bytes_uploaded_ = sa2::waits::g_bytes_uploaded.load(std::memory_order_relaxed);
      hitch_created_textures_ = sa2::waits::g_created_textures.load(std::memory_order_relaxed);
      hitch_ns_raw_fingerprint_ = sa2::waits::g_ns_raw_fingerprint.load(std::memory_order_relaxed);
      hitch_ns_data_fingerprint_ = sa2::waits::g_ns_data_fingerprint.load(std::memory_order_relaxed);
      hitch_ns_texture_zero_ = sa2::waits::g_ns_texture_zero.load(std::memory_order_relaxed);
      hitch_ns_detile_ = sa2::waits::g_ns_detile.load(std::memory_order_relaxed);
      hitch_bytes_fingerprint_ = sa2::waits::g_bytes_fingerprint.load(std::memory_order_relaxed);
      hitch_postponed_fingerprints_ = sa2::waits::g_postponed_fingerprints.load(std::memory_order_relaxed);
      hitch_ns_waiting_copies_ = sa2::waits::g_ns_waiting_copies.load(std::memory_order_relaxed);  // 170
      hitch_waits_copies_ = sa2::waits::g_waits_copies.load(std::memory_order_relaxed);
      hitch_ns_helping_copies_ = sa2::waits::g_ns_helping_copies.load(std::memory_order_relaxed);  // 185
      hitch_helped_copies_ = sa2::waits::g_helped_copies.load(std::memory_order_relaxed);
      // Creating textures and the binding thread (sa2_native_textures_binding_thread).
      hitch_ns_create_textures_ = sa2::waits::g_ns_create_textures.load(std::memory_order_relaxed);
      hitch_bound_textures_thread_ = sa2::waits::g_bound_textures_thread.load(std::memory_order_relaxed);
      hitch_ns_waiting_bindings_ = sa2::waits::g_ns_waiting_bindings.load(std::memory_order_relaxed);
      hitch_ns_wait_output_ = ns_wait_output_;
      // ds2_dynamic_resolution: this frame's GPU busy time (the span minus the gaps between work items).
      {
        const uint64_t span = gpu_ns_ - hitch_gpu_ns_;
        const uint64_t gap = gpu_categories_ns_[9] - hitch_gpu_categories_[9];
        uint64_t passes = 0;  // ds2_dynamic_resolution_spike_ms: every category but the gap
        for (size_t k = 0; k < gpu_categories_ns_.size(); ++k) {
          if (k != 9) passes += gpu_categories_ns_[k] - hitch_gpu_categories_[k];
        }
        sa2::render::NoteFrameGpuBusy(double(span > gap ? span - gap : 0) / 1e6 * 1.627,
                                      double(passes) / 1e6 * 1.627);
      }
      hitch_gpu_ns_ = gpu_ns_;
      hitch_gpu_categories_ = gpu_categories_ns_;
      hitch_ns_record_ = ns_record_;
      hitch_copies_ = copies_;
      hitch_restores_ = restores_;
      hitch_waits_ = waits_gpu_;
      cleared_hitch_ = cleared_;
      hitch_resolves_ = swaps_;
    }
    last_swap_ = now;
  }
  uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
  static const bool diag_present_alias = REXCVAR_GET(sa2_diag_present_alias);
  if (diag_present_alias) {  // sa2_diag_present_alias
    for (const auto& [alias_address, alias] : aliases_) {
      if (alias.source == base && alias.sx == 0 && alias.sy == 0 && alias.dx == 0 && alias.dy == 0) {
        base = alias_address;
        break;
      }
    }
  }
  ++trace_frame_;
  // sa2_native_lazy_front. Before submitting the work: either it is drawn from the image that
  // holds the content (no copy) or the deferred copy is recorded now, ahead of the output.
  if (DiagAlias()) {
    const auto pr = resolved_.find(base);
    REXLOG_INFO("[diag alias] frame {} PRESENT {:08X} alias {} img {:X}", presented_, base, aliases_.count(base),
                pr != resolved_.end() ? pr->second.image.image : sa2::dk::kNoImage);
  }
  if (!aliases_.empty() && aliases_.count(base)) MaterializeAlias(base);  // sa2_gpu_resolve_alias: it is read
  Image* const source_front = FrontOnPresent(base, width, height);
  // Without waiting for the GPU: the output goes after the work on the same queue.
  // deko3d: there is no SDK presenter (null here); the output is PaintOutput's, so it is not required.
  (void)presenter;
  if (!SendWork(false)) {
    return false;
  }
  auto it = resolved_.find(base);
  if (it == resolved_.end() || !it->second.image.prepared) {
    Reject(4, "Swap without a resolved texture: the test color is painted");
    return false;
  }
  Resolved& resolved = it->second;
  resolved.last_read = presented_;  // sa2_native_resolved_retire_idle: the front buffer is read by the output
  const uint32_t w = std::min(width ? width : resolved.image.width, resolved.image.width);
  const uint32_t h = std::min(height ? height : resolved.image.height, resolved.image.height);
  // sa2-nx render resolution: the guest output is the front buffer's host size (1920x1080 docked by default); the
  // presenter scales it to the display with the game's 16:9 aspect.
  const Image& painted_image = source_front ? *source_front : resolved.image;
  const uint32_t host_w = std::min<uint32_t>(ScaleSize(w, painted_image.scale_q), HostWidth(painted_image));
  const uint32_t host_h = std::min<uint32_t>(ScaleSize(h, painted_image.scale_q), HostHeight(painted_image));
  // deko3d: RefreshGuestOutput(host_w, host_h, 1280, 720, ...) is gone: PaintOutput also runs what the presenter
  // did after its callback (present_effect and the stretch to the display, 16:9 like the game's 1280x720) and
  // presents the window.
  const bool painted = PaintOutput(source_front ? *source_front : resolved.image, host_w, host_h,
                                   source_front != nullptr);  // sa2_native_lazy_front
  if (painted) {
    ++presented_;
    if (retire_resolved_urgently_.load(std::memory_order_relaxed)) [[unlikely]] {  // memory ran short this frame
      retire_resolved_urgently_.store(false, std::memory_order_relaxed);
      RetireIdleResolved(120);
    } else if ((presented_ & 63) == 0) {  // sa2_native_resolved_retire_idle: about once a second
      RetireIdleResolved(uint64_t(std::max(REXCVAR_GET(sa2_native_resolved_idle_frames), 120)));
      ReportResolved();
    }
  }
  RescaleIfWanted();  // sa2-nx render resolution: docked <-> handheld, at this frame boundary
  return painted;
}


// Per-draw diagnostic window, one whole frame every N seconds. It is decided
// in the Swap because the upload buffer rotates several times per frame.
void TargetsDk::OpenWindowDiagnostic() {
  const int32_t each = REXCVAR_GET(sa2_native_stats_per_draw_s);
  if (each <= 0) {
    window_diagnostic_ = false;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (window_diagnostic_) {
    window_diagnostic_ = false;  // that frame was already measured
    return;
  }
  if (std::chrono::duration_cast<std::chrono::seconds>(now - last_window_).count() >= each) {
    ReportCostPerShader();  // the previous window, read by now
    last_window_ = now;
    window_diagnostic_ = true;
    read_window_ = false;
  }
}


void TargetsDk::Stats(uint64_t& copies, uint64_t& cleared, uint64_t& presented,
                  uint64_t& rejections) const {
  copies = copies_;
  cleared = cleared_;
  presented = presented_;
  rejections = rejections_;
}


void TargetsDk::TimeGpuPerCategory(
    std::array<uint64_t, kGpuCategories>& nanoseconds) const {
  nanoseconds = gpu_categories_ns_;
}


void TargetsDk::StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                          std::array<uint64_t, kGpuCategories>& vertices,
                          std::array<uint64_t, kGpuCategories>& primitives) const {
  fragments = fragments_category_;
  vertices = vertices_category_;
  primitives = primitives_category_;
}


bool TargetsDk::Draw(const SubmissionDraw& submission) {
  depth_raster_grid_eligible_ = false;
  if (submission.registers) {
    msaa_edram_actual_ = (submission.registers[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3;
  }
  const bool mode4 = REXCVAR_GET(sa2_native_edram_alias_mode) == 4;
  // A redirected clear is a draw the memo does not see: the next draw takes the full path.
  if (mode4 && RedirectClearDepthEDRAM4(submission)) { edram4_memo_.valid = false; return true; }
  if (mode4 && RedirectClearStencilEDRAM4(submission)) { edram4_memo_.valid = false; return true; }
  if (mode4 && !PrepareDrawEDRAM4Memo(submission)) return false;
  if (REXCVAR_GET(sa2_native_edram_alias_mode) == 3 && submission.registers && submission.ps) {
    const uint32_t* r = submission.registers;
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t wx = (tl >> 31) ? 0 : int32_t(window << 17) >> 17;
    const int32_t wy = (tl >> 31) ? 0 : int32_t((window >> 16) << 17) >> 17;
    const int32_t x0 = std::max(0, int32_t(tl & 0x3FFF) + wx);
    const int32_t y0 = std::max(0, int32_t((tl >> 16) & 0x3FFF) + wy);
    const int32_t x1 = std::max(x0, int32_t(br & 0x3FFF) + wx);
    const int32_t y1 = std::max(y0, int32_t((br >> 16) & 0x3FFF) + wy);
    area_edram_ = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    const uint32_t mask = r[gr::XE_GPU_REG_RB_COLOR_MASK];
    // A scissor can grow without opening a new render pass. Refresh ownership
    // on each color-writing draw, not only when the target is first bound.
    if ((r[gr::XE_GPU_REG_RB_MODECONTROL] & 7) == uint32_t(xenos::EdramMode::kColorDepth)) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!((mask >> (i * 4)) & 0xF) || !(submission.ps->outputs & (1u << i))) continue;
        const uint32_t info = r[kDiagRegsColorInfo[i]];
        Image* image = GetTarget(info & 0xFFF, (info >> 16) & 0xF, pitch);
        if (image && Record()) {
          Prepare(*image);
          ActivateTargetColor(info & 0xFFF, pitch, *image);
        }
      }
    }
  }
  const uint64_t draw_count_before = draws_ ? draws_->Drawn() : 0;
  const bool result = draws_ && draws_->Draw(submission);
  if (mode4 && draws_ && draws_->Drawn() == draw_count_before &&
      DrawTouchesTraceTileEDRAM4() && TraceEventEDRAM4(TraceTileEDRAM4()))
    REXLOG_INFO("[native] EDRAM TILE TRACE draw-result frame={} physical={} before={} after={} success={}",
                trace_frame_, TraceTileEDRAM4(), draw_count_before,
                draws_ ? draws_->Drawn() : 0, result);
  // A successful no-op (empty/skipped primitive, scissor or disabled pass)
  // must not claim pixels the current draw never wrote. Accepted deferred
  // sky draws also increment this counter and are flushed by FinishPass.
  if (mode4 && result && draws_->Drawn() > draw_count_before) {
    for (uint32_t slot = 0; slot < 5; ++slot) {
      if (slot == 0 && edram4_draw_images_[0] && edram4_draw_writes_[0] &&
          submission.edram_stencil_clear.eligible) {
        if (!PublishStencilClearEDRAM4(*edram4_draw_images_[0],
                                        submission.edram_stencil_clear,
                                        edram4_draw_plan_.length_tiles[0])) return false;
      } else if (edram4_draw_images_[slot] && edram4_draw_writes_[slot]) {
        edram4_published_no_stencil_ = slot == 0 && !edram4_draw_writes_stencil_;
        PublishEDRAM4(*edram4_draw_images_[slot],
                       RasterArea(*edram4_draw_images_[slot], edram4_draw_area_),
                       edram4_draw_plan_.length_tiles[slot]);
        edram4_published_no_stencil_ = false;
      }
    }
  }
  return result;
}


StatsDraws TargetsDk::StatsOfDraws() const {
  return draws_ ? draws_->Stats() : StatsDraws{};
}


void TargetsDk::WaitUploads() {
  if (draws_) {
    draws_->WaitUploads();
  }
}


size_t TargetsDk::PendingCopies() const {  // fence measurement only
  return draws_ ? draws_->PendingCopies() : 0;
}


// --- Interface lent to the draw code ----------------------------------------

DkCmdBuf TargetsDk::CommandsWork() {
  return Record() ? commands_work_ : nullptr;
}


uint32_t TargetsDk::HostMsaaSamples() const { return msaa_samples_; }


// sa2_msaa: a single-sample image, or one of the host multisampled game targets.
bool TargetsDk::SamplesSupported(const Image& image) const {
  return me::native::IsSingleSample(uint32_t(image.sample_count)) ||
         (msaa_samples_ > 1 && uint32_t(image.sample_count) == msaa_samples_);
}


DkCmdBuf TargetsDk::CommandsUpload() {
  if (!Record()) {
    return nullptr;
  }
  if (!recording_upload_) {
    // deko3d: no vkBeginCommandBuffer; the slot's upload stream was rewound by BeginRecording.
    // Cached images may still be sampled by the preceding submission. Queue
    // submission order alone does not order those reads against new uploads.
    // (Was ALL_COMMANDS -> TRANSFER, MEMORY_READ | WRITE -> TRANSFER_WRITE: the copy engine waits for the 3D work.)
    sa2::dk::Barrier(commands_upload_, sa2::dk::Hazard::RenderToCopy);
    recording_upload_ = true;
  }
  return commands_upload_;
}


uint64_t TargetsDk::GenerationCommands() const { return generation_commands_; }


ImageNativeDk* TargetsDk::TargetColor(uint32_t base, uint32_t format, uint32_t pitch) {
  Image* image = GetTarget(base, format, pitch);
  if (!image || !Record()) {
    return nullptr;
  }
  Prepare(*image);
  if (REXCVAR_GET(sa2_native_edram_alias_mode) == 4) {
    BeforeWriteColor(*image);
    if (!RestoreContent(*image)) return nullptr;
    return image; // Acquired before DrawsVulkan can start a render pass.
  }
  ActivateTargetColor(base, pitch, *image);
  BeforeWriteColor(*image);  // sa2_native_lazy_front
  // Mandatory since color is also swapped. Here a pass starts that draws on top; if the content went away
  // in a swap and nobody has cleared it, it has to be brought back. Without this, the first frame that
  // draws without clearing shows the previous frame.
  RestoreContent(*image);
  return image;
}


ImageNativeDk* TargetsDk::TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) {
  Image* image = GetDepth(base, format, pitch);
  if (!image || !Record()) {
    return nullptr;
  }
  Prepare(*image);
  // A pass that draws on top starts here, so if the content went away in a swap and nobody has cleared it
  // since then, it has to be brought back.
  if (!RestoreContent(*image) && REXCVAR_GET(sa2_native_edram_alias_mode) == 4)
    return nullptr;
  return image;
}


void TargetsDk::GpuMemoryIsShort() { retire_resolved_urgently_.store(true, std::memory_order_relaxed); }


const ImageNativeDk* TargetsDk::ResolvedTexture(uint32_t address) {
  const auto it = resolved_.find(address);
  if (it == resolved_.end()) {
    if (!retired_resolved_.empty()) [[unlikely]] NoteFetchAfterRetirement(address);
    return nullptr;
  }
  // sa2_native_resolved_retire_idle: a read keeps it alive, and the oldest read (in frames since the resolve that
  // filled it) is reported: it says how long a texture may sit unresolved and still be wanted.
  if (const uint64_t age = presented_ - it->second.last_write; age > oldest_read_resolved_) [[unlikely]] {
    oldest_read_resolved_ = age;
    oldest_read_address_ = address;
  }
  it->second.last_read = presented_;
  // And how many times each address is requested, for the per-render-target copy report. A resolved
  // texture that is copied every frame and never requested is a copy that is not needed. It is a ++ on the
  // entry that has already been looked up: it costs nothing even though this is called thousands of times
  // per frame.
  ++it->second.reads;
  if (!presented_fronts_.empty() && presented_fronts_.count(address)) {
    NoteReadFront(address);  // sa2_native_lazy_front
  }
  // sa2_gpu_resolve_alias: a draw reads it, so it gets its bytes now (before the draw). MaterializeAlias neither
  // adds nor removes entries of resolved_: `it` stays valid.
  if (it->second.alias_pending) [[unlikely]] MaterializeAlias(address);
  return it->second.image.prepared ? &it->second.image : nullptr;
}


const ImageNativeDk* TargetsDk::ResolvedTextureForFetch(uint32_t address, const uint32_t fetch[6],
                                                      const ImageNativeDk& source) {
  const uint32_t width = (fetch[2] & 0x1FFFu) + 1;
  const uint32_t height = ((fetch[2] >> 13) & 0x1FFFu) + 1;
  const auto failure = [&](const char* reason) -> const ImageNativeDk* {
    if (++clips_failures_ <= 32)
      REXLOG_ERROR("[native] resolved logical crop rejected {:08X} {}x{}: {}", address,
                   width, height, reason);
    return nullptr;  // The caller distinguishes this from no resolved image; no stale RAM fallback.
  };
  // This guard also covers the exact-size and disabled-crop fast paths. A
  // known multisample backing must never fall through to stale guest RAM or
  // become an ordinary sampler2D / buffer-image crop.
  if (!me::native::IsSingleSample(uint32_t(source.sample_count)))
    return failure("multisample backing requires a proven single-sample resolve");
  if (!REXCVAR_GET(sa2_native_logical_resolved_size) ||
      REXCVAR_GET(sa2_native_edram_alias_mode) != 4 ||
      !resolved_pool_startup_eligible_ || ResolverNoCopy() ||
      (width == source.width && height == source.height)) return &source;
  if (resolved_pool_device_error_) return failure("GPU fence/submission error");
  // ResolvedTexture was already called exactly once. Materialize any remaining logical write
  // before taking its revision, without incrementing read counters a second time.
  if (pending_fronts_.count(address)) RecordCopyFront(address);
  if (!aliases_.empty() && aliases_.count(address)) MaterializeAlias(address);  // sa2_gpu_resolve_alias
  auto r = resolved_.find(address);
  if (r == resolved_.end() || !r->second.image.prepared ||
      r->second.image.invalid_content ||
      expired_fronts_.count(address) ||
      pending_fronts_.count(address)) return failure("unmaterialized or stale source");
  const Image canonical = r->second.image;
  if (!me::native::IsSingleSample(uint32_t(canonical.sample_count)))
    return failure("materialized crop source is multisampled");
  const uint64_t source_revision = r->second.revision;
  uint32_t texel_bytes = 0;
  switch (canonical.format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: texel_bytes = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: texel_bytes = 8; break;
    default: break;  // Native depth/stencil crops need a separate aspect contract.
  }
  xenos::xe_gpu_texture_fetch_t texture_fetch{};
  std::memcpy(&texture_fetch, fetch, sizeof(texture_fetch));
  uint32_t base_page = 0, mip_page = 0, mip_min = 0, mip_max = 0;
  gr::texture_util::GetSubresourcesFromFetchConstant(texture_fetch, nullptr, nullptr, nullptr,
                                                   &base_page, &mip_page, &mip_min, &mip_max);
  me::native::ResolvedSamplingContract contract{};
  contract.logical_width = width;
  contract.logical_height = height;
  contract.guest_pitch = ((fetch[0] >> 22) & 0x1FFu) << 5;
  contract.backing_width = canonical.width;
  contract.backing_height = canonical.height;
  contract.bytes_per_texel = texel_bytes;
  contract.base_formats_compatible = canonical.resolved_guest_format < 64 &&
      gr::GetBaseFormat(xenos::TextureFormat(canonical.resolved_guest_format)) ==
      gr::GetBaseFormat(texture_fetch.format);
  contract.source_origin_zero = texture_fetch.tiled && base_page && (base_page << 12) == address &&
      (!texture_fetch.packed_mips || gr::texture_util::GetPackedMipLevel(width, height) != 0);
  contract.single_level_2d = texture_fetch.type == xenos::FetchConstantType::kTexture &&
      texture_fetch.dimension == xenos::DataDimension::k2DOrStacked && !texture_fetch.stacked &&
      mip_min == 0 && mip_max == 0 && mip_page == 0 && texture_fetch.mip_address == 0;
  contract.scale_1x = canonical.guest_width == 0 && canonical.guest_height == 0;
  constexpr VkDeviceSize kMaxBytes = 128ull << 20;
  const auto plan = me::native::AnalyzeResolvedSampling(contract, kMaxBytes);
  if (plan.mode == me::native::ResolvedSamplingMode::Unsupported) {
    if (clips_failures_ < 32)
      REXLOG_ERROR("[native] resolved crop contract failure {}: pitch {} backing {}x{} "
                   "guest formats {}/{} host {}", int(plan.failure), contract.guest_pitch,
                   canonical.width, canonical.height, canonical.resolved_guest_format,
                   uint32_t(texture_fetch.format), int(canonical.format));
    return failure("unproven logical fetch layout");
  }
  if (plan.mode == me::native::ResolvedSamplingMode::Direct) return &r->second.image;
  // deko3d: the copy engine is always there (was: if (!copy_image_) "image-copy entry point unavailable").
  ResolvedClip* crop = nullptr;
  for (auto& entry : resolved_clips_)
    if (entry.address == address && entry.image.width == width &&
        entry.image.height == height && entry.image.format == canonical.format) {
      crop = &entry;
      break;
    }
  if (crop && crop->source == canonical.image && crop->revision == r->second.revision) {
    ++clips_cache_hits_;
    return &crop->image;
  }
  if (!crop) {
    if (!me::native::ResolvedAllocationFits(resolved_clips_.size(), clips_bytes_,
                                           plan.copy_bytes)) return failure("bounded crop cache capacity");
    ResolvedClip new_value;
    new_value.address = address;
    // deko3d: SAMPLED | TRANSFER_SRC | TRANSFER_DST need no flag (sampling and the copy engine work on any image;
    // CreateScaled adds what every image needs).
    if (!Create(new_value.image, width, height, 0u,
                canonical.format)) return failure("crop allocation failed");
    const uint64_t requirements_size = sa2::dk::Bytes(new_value.image.image);  // was vkGetImageMemoryRequirements
    if (!me::native::ResolvedAllocationFits(resolved_clips_.size(), clips_bytes_,
                                           requirements_size)) {
      Destroy(new_value.image);  // Fresh, unrecorded, no descriptors or consumers.
      return failure("actual Vulkan allocation exceeds bounded crop cache");
    }
    new_value.bytes = requirements_size;
    clips_bytes_ += new_value.bytes;
    resolved_clips_.push_back(new_value);
    crop = &resolved_clips_.back();
    ++clips_allocations_;
  }
  if (draws_) draws_->FinishPass();
  if (!Record()) return failure("crop work recording failed");
  r = resolved_.find(address);  // Closing a shadow pass may record resolves or submit / rotate.
  if (r == resolved_.end() || r->second.image.image != canonical.image ||
      r->second.revision != source_revision) return failure("source changed while closing previous pass");
  Prepare(crop->image);  // Fresh only; reused crops stay GENERAL and are never cleared in UPLOAD.
  if (!crop->image.prepared) return failure("crop initialization failed");
  // Was ALL_COMMANDS -> TRANSFER (MEMORY_READ | WRITE -> TRANSFER_READ | WRITE): the canonical image was rendered
  // or copied into by the work before; the copy engine reads it.
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
  VkImageCopy copy{};
  copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.extent = {width, height, 1};
  copy = HostCopy(canonical, crop->image, copy);  // sa2-nx render resolution
  CopyImageDk(commands_work_, canonical.image, crop->image.image, copy);
  // And TRANSFER -> ALL_COMMANDS: the draws sample the crop next.
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::CopyToSample);
  crop->source = canonical.image;
  crop->revision = source_revision;
  crop->image.swap_rb = canonical.swap_rb;
  crop->image.resolved_guest_format = canonical.resolved_guest_format;
  crop->image.resolved_depth_guestspace = canonical.resolved_depth_guestspace;
  ++clips_copies_;
  const auto now = std::chrono::steady_clock::now();
  if (clips_copies_ <= 8 || now - clips_report_ >= std::chrono::seconds(10)) {
    clips_report_ = now;
    REXLOG_INFO("[native] resolved logical crop {:08X}: {}x{} -> {}x{}, revision {}; "
                "allocations={}, copies={}, cache hits={}, failures={}, bytes={}", address,
                canonical.width, canonical.height, width, height, crop->revision,
                clips_allocations_, clips_copies_, clips_cache_hits_, clips_failures_, clips_bytes_);
  }
  return &crop->image;
}


// Is this address a front buffer read only by the Swap? A Swap presented it recently and no draw has
// sampled it in that time.
bool TargetsDk::IsFrontOnlyOfTheSwap(uint32_t address) const {
  const auto presented = presented_fronts_.find(address);
  if (presented == presented_fronts_.end() || presented_ - presented->second > kFrontFrames) {
    return false;
  }
  const auto read = read_fronts_.find(address);
  return read == read_fronts_.end() || presented_ - read->second > kFrontFrames;
}


// The retained image of a deferred copy becomes free again.
void TargetsDk::ReleaseRetained(const PendingFront& pending) {
  if (pending.retained >= 0 && size_t(pending.retained) < fronts_images_.size()) {
    fronts_images_[size_t(pending.retained)].retained_per = 0;
  }
}


// The image holding the deferred copy's content, if it is still the same one; otherwise nullptr.
Image* TargetsDk::SourceFront(const PendingFront& pending) {
  Image* source = nullptr;
  if (pending.retained >= 0) {
    if (size_t(pending.retained) < fronts_images_.size()) {
      source = &fronts_images_[size_t(pending.retained)].image;
    }
  } else {
    source = pending.target;
  }
  return source && source->image == pending.source_vk && source->prepared ? source : nullptr;
}


// Records the deferred copy to that front buffer (outside a pass) and removes it from the list.
void TargetsDk::RecordCopyFront(uint32_t address) {
  const auto p = pending_fronts_.find(address);
  if (p == pending_fronts_.end()) {
    return;
  }
  const PendingFront pending = p->second;
  pending_fronts_.erase(p);
  const auto r = resolved_.find(address);
  Image* const source = SourceFront(pending);
  if (r == resolved_.end() || r->second.image.image != pending.texture_vk || !source ||  // (copy_image_: always)
      !Record()) {
    ReleaseRetained(pending);
    expired_fronts_.insert(address);  // should not happen: every image change goes through the hooks first
    return;
  }
  if (draws_) {
    draws_->FinishPass();  // may come from a draw (ResolvedTexture): the copy goes outside the pass
  }
  BeforeResolvedWrite(address);  // sa2_gpu_resolve_alias
  MarkGpu(kGpuCopies);
  const VkImageCopy host_copy = HostCopy(*source, r->second.image, pending.copy);  // sa2-nx render resolution
  BarrierBeforeCopyResolve();
  CopyImageDk(commands_work_, pending.source_vk, pending.texture_vk, host_copy);
  BarrierAfterCopyResolve();
  ++copies_;
  ResolvedWritten(address, uint64_t(pending.copy.extent.width) * pending.copy.extent.height);
  expired_fronts_.erase(address);
  ReleaseRetained(pending);
}


// Another resolve (or an image swap) reaches that texture. If it covers the whole texture, the deferred
// copy is unnecessary and is dropped: nobody will see its content. Otherwise it is recorded first.
void TargetsDk::ResolverPreviousFront(uint32_t address, bool whole) {
  if (whole && !expired_fronts_.empty()) {
    expired_fronts_.erase(address);  // fully covered: it is no longer missing any copy
  }
  if (pending_fronts_.empty()) {
    return;
  }
  const auto p = pending_fronts_.find(address);
  if (p == pending_fronts_.end()) {
    return;
  }
  if (!whole) {
    RecordCopyFront(address);
    ++copied_front_write_;
    return;
  }
  front_saved_pixels_ += uint64_t(p->second.copy.extent.width) * p->second.copy.extent.height;
  ReleaseRetained(p->second);
  pending_fronts_.erase(p);
  ++replaced_front_;
}


// Defers the copy if it is 1:1, covers the whole texture from the target's corner, and that address is
// read only by the Swap.
bool TargetsDk::PostponeCopyFront(uint32_t address, Image& target, const Resolved& resolved, const VkImageCopy& copy) {
  if (off_front_ || !REXCVAR_GET(sa2_native_lazy_front)) {
    return false;
  }
  if (copy.srcOffset.x != 0 || copy.srcOffset.y != 0 || copy.dstOffset.x != 0 || copy.dstOffset.y != 0 ||
      copy.extent.width != resolved.image.width || copy.extent.height != resolved.image.height ||
      target.format != kColorFormat || resolved.image.format != kColorFormat) {
    return false;
  }
  // Small ones are read back for the guest (ResolvedRead): those are never deferred.
  const int32_t texels_read = REXCVAR_GET(sa2_native_resolved_read_texels);
  if (uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0))) {
    return false;
  }
  if (!IsFrontOnlyOfTheSwap(address)) {
    return false;
  }
  BeforeResolvedWrite(address);  // sa2_gpu_resolve_alias
  PendingFront& pending = pending_fronts_[address];
  pending.target = &target;
  pending.source_vk = target.image;
  pending.retained = -1;
  pending.texture_vk = resolved.image.image;
  pending.copy = copy;
  ResolvedWritten(address);  // new contents (deferred)
  expired_fronts_.erase(address);
  ++postponed_front_;
  return true;
}


// Before writing to a color target in any way other than a full clear (a pass, a restore, an image
// swap): deferred copies whose source is its image are recorded now (exact).
void TargetsDk::BeforeWriteColor(const Image& target) {
  if (pending_fronts_.empty() || target.image == sa2::dk::kNoImage) {
    return;
  }
  for (auto it = pending_fronts_.begin(); it != pending_fronts_.end();) {
    if (it->second.retained >= 0 || it->second.source_vk != target.image) {
      ++it;
      continue;
    }
    const uint32_t address = it->first;
    ++it;  // RecordCopyFront erases that entry: the iterator is already on the next one
    RecordCopyFront(address);
    ++copied_front_write_;
  }
}


// Before a full clear of a color target. If its image is the source of a deferred front buffer, the
// target takes a free spare image of the same size and the one holding the content is retained for the
// front buffer: the clear does not need the old content, so nothing is copied. Without a spare (or with
// two front buffers from the same image), the copy is recorded first, as usual.
void TargetsDk::RotateFrontBeforeClear(Image& target) {
  if (pending_fronts_.empty() || target.image == sa2::dk::kNoImage) {
    return;
  }
  uint32_t base = 0;
  uint32_t how_many = 0;
  for (const auto& [address, pending] : pending_fronts_) {
    if (pending.retained < 0 && pending.source_vk == target.image) {
      base = address;
      ++how_many;
    }
  }
  if (how_many == 0) {
    return;
  }
  int32_t free = -1;
  if (how_many == 1) {
    for (size_t i = 0; i < fronts_images_.size(); ++i) {
      const Image& candidate = fronts_images_[i].image;
      if (fronts_images_[i].retained_per == 0 && candidate.image != sa2::dk::kNoImage &&
          candidate.width == target.width && candidate.height == target.height &&
          candidate.format == target.format) {
        free = int32_t(i);
        break;
      }
    }
    if (free < 0 && fronts_images_.size() < kFrontImagesMax) {
      ImageFront new_entry;
      // Same usage flags as GetTarget: the image becomes the render target.
      // deko3d: the target's own DkImageFlags (what GetTarget's usage became).
      if (Create(new_entry.image, target.width, target.height, target.dk_flags,
                target.format)) {
        Prepare(new_entry.image);
        fronts_images_.push_back(new_entry);
        free = int32_t(fronts_images_.size()) - 1;
        REXLOG_INFO("[native] targets: lazy front: spare image {} of {}x{} for the front buffer target",
                    fronts_images_.size(), target.width, target.height);
      }
    }
  }
  if (free < 0) {
    ++front_no_replenished_;
    BeforeWriteColor(target);  // the copy is recorded before the clear, as usual
    return;
  }
  Image& replenished = fronts_images_[size_t(free)].image;
  std::swap(target.image, replenished.image);  // deko3d: the id carries memory and view
  std::swap(target.prepared, replenished.prepared);
  fronts_images_[size_t(free)].retained_per = base;
  pending_fronts_[base].retained = free;  // source_vk is still the content image, now retained
  if (draws_) {
    draws_->InvalidateImages(target.image, replenished.image);
  }
  ++front_rotations_;
}


// A draw samples a front buffer. If its copy is deferred, it is recorded now (before the draw).
void TargetsDk::NoteReadFront(uint32_t address) {
  read_fronts_[address] = presented_;
  if (!pending_fronts_.empty() && pending_fronts_.count(address)) {
    RecordCopyFront(address);
    ++copied_front_read_;
  }
  if (!expired_fronts_.empty() && expired_fronts_.erase(address)) {
    ++front_late_reads_;
    if (!off_front_) {
      off_front_ = true;
      sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
      REXLOG_ERROR("[native] targets: lazy front: DIFFERENCE, a draw samples front buffer {:08X} without its last "
                   "copy. Turned off for the rest of the session: always copy",
                   address);
    }
  }
}


// The Swap of that front buffer, before the submission. Returns the image that can be presented without
// a copy (the target's or the retained one, with the front buffer in its corner) or nullptr: in that
// case any deferred copy is recorded here (in the same submission, ahead of the output) and the output
// reads the texture as usual.
Image* TargetsDk::FrontOnPresent(uint32_t address, uint32_t width, uint32_t height) {
  presented_fronts_[address] = presented_;
  if (!expired_fronts_.empty() && expired_fronts_.erase(address)) {
    ++front_late_reads_;
    if (!off_front_) {
      off_front_ = true;
      sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
      REXLOG_ERROR("[native] targets: lazy front: DIFFERENCE, the Swap paints front buffer {:08X} without its last copy. "
                   "Turned off for the rest of the session: always copy",
                   address);
    }
  }
  if (pending_fronts_.empty()) {
    return nullptr;
  }
  const auto p = pending_fronts_.find(address);
  if (p == pending_fronts_.end()) {
    return nullptr;
  }
  const auto r = resolved_.find(address);
  Image* const source = SourceFront(p->second);
  bool can = source && r != resolved_.end() && r->second.image.image == p->second.texture_vk &&
               r->second.image.prepared && ramp_gamma_ &&
               !off_front_;
  if (can) {
    // Same computation as Present: the output has the texture's size (exact variant) and the copy
    // covers it.
    const Image& texture = r->second.image;
    const uint32_t w = std::min(width ? width : texture.width, texture.width);
    const uint32_t h = std::min(height ? height : texture.height, texture.height);
    can = w == texture.width && h == texture.height && p->second.copy.extent.width == w &&
            p->second.copy.extent.height == h;
  }
  if (!can) {
    RecordCopyFront(address);
    ++copied_front_swap_;
    return nullptr;
  }
  if (p->second.retained >= 0) {
    ++painted_retained_front_;
  } else {
    ++painted_front_target_;
  }
  return source;
}


// Upload buffer full: submit what has been recorded and continue in the other slot, with its empty
// upload buffer (Record only waits if that slot's last submission is still on the GPU).
bool TargetsDk::SendAndWait() { return SendWork(false) && Record(); }

void TargetsDk::ResolvedWritten(uint32_t base, uint64_t pixels) {
  if (auto it = resolved_.find(base); it != resolved_.end()) {
    it->second.revision = ++revision_resolved_;
    // The direct image updates in place, but derived ROIs must go through PrepareTexture again,
    // including consecutive draws within one frame. Partial writes invalidate the complete ROI.
    for (const auto& crop : resolved_clips_) {
      if (crop.address == base) {
        if (draws_) draws_->InvalidateTextures();
        break;
      }
    }
  }
  if (pixels) {
    const auto c = copies_per_target_.find(base);
    if (c != copies_per_target_.end()) {
      c->second.pixels += pixels;
    }
  }
}


// The render target's content is no longer that of its last clear (something was copied over it or
// it changed image). The game's next clear cannot be skipped.
//
// It also answers the other question about clears: if the target's content changes without anything
// drawn since it was cleared, that clear was wiped out by a swap or a restore and served no purpose.
// It is the equivalent of a loadOp = CLEAR that gets thrown away: these are DONT_CARE candidates.
void TargetsDk::ForgetClear(const Image& target) {
  const auto e = target_state_.find(&target);
  if (e == target_state_.end()) {
    return;
  }
  if (e->second.clean_clear &&
      e->second.draws_on_clear == (draws_ ? draws_->Drawn() : 0)) {
    ++cleared_useless_;
    cleared_useless_pixels_ += uint64_t(target.width) * target.height;
  }
  e->second.clean_clear = false;
}


// The largest rectangle the game resolves from this render target: the area it actually uses.
void TargetsDk::NoteAreaUtil(const Image& target, int32_t y1) {
  if (y1 > 0) {
    auto& e = target_state_[&target];
    e.used_height = std::max(e.used_height, std::min(uint32_t(y1), target.height));
  }
}


// A clear that does not change a single bit. It is skipped if the render target is already cleared
// to that same value and nothing has been drawn, anywhere, since then. Returns true if it must be
// skipped.
bool TargetsDk::RedundantClear(const Image& target, uint64_t value) {
  auto& e = target_state_[&target];
  const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
  if (REXCVAR_GET(sa2_native_skip_cleared_repeated) && e.clean_clear &&
      e.clear_value == value && e.draws_on_clear == drawn) {
    cleared_skipped_pixels_ += uint64_t(target.width) * target.height;
    return true;
  }
  e.clean_clear = true;
  e.clear_value = value;
  e.draws_on_clear = drawn;
  return false;
}

// Copy inventory for the report: for each resolved texture, how many copies there are and how many had
// draws since the previous copy (a resolve without draws copies again what the render target already
// had). It has to be complete (address, size, Mpixels and reads) because it is the only thing that tells
// whether a copy is needed; ~20 addresses fit easily in one line.
void TargetsDk::NoteCopy(uint32_t base, uint32_t width, uint32_t height, uint64_t draws) {
  CopyTarget& d = copies_per_target_[base];
  d.width = width;
  d.height = height;
  ++d.copies;
  d.draws += draws;
  d.with_draws += draws != 0 ? 1 : 0;
  const auto now = std::chrono::steady_clock::now();
  if (now - report_copies_ < std::chrono::seconds(10)) {
    return;
  }
  report_copies_ = now;
  if (ds2_depth_resolves_skipped_ != ds2_depth_resolves_skipped_report_) {
    REXLOG_INFO("[native] targets: repeated depth resolves skipped (ds2_skip_repeated_depth_resolve) since the "
                "previous report: {}", ds2_depth_resolves_skipped_ - ds2_depth_resolves_skipped_report_);
    ds2_depth_resolves_skipped_report_ = ds2_depth_resolves_skipped_;
  }
  if (resolve_draws_ != resolve_draws_report_ || resolve_draws_declined_ != resolve_draws_declined_report_) {
    REXLOG_INFO("[native] targets: resolves drawn on the 3D engine (sa2_gpu_resolve_draw) since the previous "
                "report: {}, copied as before: {}", resolve_draws_ - resolve_draws_report_,
                resolve_draws_declined_ - resolve_draws_declined_report_);
    resolve_draws_report_ = resolve_draws_;
    resolve_draws_declined_report_ = resolve_draws_declined_;
  }
  if (aliases_made_ != aliases_made_report_ || aliases_copied_ != aliases_copied_report_ ||
      aliases_exchanged_ != aliases_exchanged_report_) {
    REXLOG_INFO("[native] targets: repeated resolves not copied (sa2_gpu_resolve_alias) since the previous report: "
                "{}; of the earlier ones, {} overwritten before anything read them, {} given their image by an "
                "exchange, {} copied (read, or their source changed); pending {}",
                aliases_made_ - aliases_made_report_, aliases_dropped_ - aliases_dropped_report_,
                aliases_exchanged_ - aliases_exchanged_report_, aliases_copied_ - aliases_copied_report_,
                aliases_.size());
    aliases_made_report_ = aliases_made_;
    aliases_dropped_report_ = aliases_dropped_;
    aliases_copied_report_ = aliases_copied_;
    aliases_exchanged_report_ = aliases_exchanged_;
  }
  if (REXCVAR_GET(sa2_native_edram_alias_mode) == 4)
    REXLOG_INFO("[native] EDRAM mode4 conversion capacity totals: {} rollovers, {} failed, current slot {} count {}/{}",
                edram4_capacity_rollovers_, edram4_capacity_rollover_failures_, slot_,
                slots_[slot_].conversions_edram, kConversionsEDRAMPerSlot);
  std::vector<std::pair<uint32_t, CopyTarget>> order(copies_per_target_.begin(),
                                                       copies_per_target_.end());
  std::sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string list;
  uint32_t written = 0;
  for (const auto& [address, c] : order) {
    if (++written > 48) {  // defensive cap: ~20 addresses per frame, but a load can touch many more
      list += fmt::format(" (and {} more addresses)", order.size() - 48);
      break;
    }
    const auto r = resolved_.find(address);
    const uint64_t reads = r != resolved_.end() ? r->second.reads : 0;
    if (r != resolved_.end()) {
      r->second.reads = 0;
    }
    list += fmt::format(" {:08X} {}x{}: {} copies, {} with draws ({:.1f} draws per copy), "
                         "{:.2f} Mpixels, {} reads{};",
                         address, c.width, c.height, c.copies, c.with_draws,
                         c.copies ? double(c.draws) / double(c.copies) : 0.0,
                         double(c.pixels) / 1e6, reads,
                         c.copies && !reads ? " *** NOBODY READS IT ***" : "");
  }
  copies_per_target_.clear();
  SA2_REPORT_RING("[native] targets: faces resolved since the previous report:{}", list);
  ReportOverlapsEDRAM();
  ReportWaitsGpu();
  // sa2-nx render resolution / MSAA: what the scaled images take (only once either has been used).
  if (scale_q_ != kScaleQuartersNative || msaa_samples_ > 1 || rescales_) {
    SA2_REPORT_RING("[res] images at {}/4 of the game's size{}: {:.1f} MB (render targets, depth targets, "
                           "resolved textures); {} rescales so far", scale_q_,
                           msaa_samples_ > 1 ? fmt::format(", {}x MSAA", msaa_samples_) : std::string(),
                           double(BytesScaledImages()) / 1048576.0, rescales_);
  }
  if (swaps_ || restores_) {
    // Restores are the verdict. If they go up, the swap without a clear saves nothing: the same copy is
    // paid, just later.
    SA2_REPORT_RING("[native] targets: resolves without copy: {} swaps ({} without clear) and {} "
                "restores since startup -> {}",
                swaps_, swaps_no_clear_, restores_,
                restores_ == 0 ? "not a single restore: the saving is clean"
                                     : "*** there are restores: the saving is NOT clean ***");
    // sa2_native_resolver_valid_content. One per frame when shadows are resolved twice in a row; 0 otherwise. The
    // "no source" count is resolves of a render target whose content is no longer anywhere (must be
    // 0); the other count is the resolves that, with the setting off, would read the stale image.
    if (restores_for_resolver_ || resolver_no_source_ || resolver_old_content_) {
      SA2_REPORT_RING("[native] targets: resolve with valid content: {} times the content was brought back "
                           "before resolving (the resolve used to read the previous frame's image: the "
                           "menu flicker); {} without source; {} read as before (setting off)",
                           restores_for_resolver_, resolver_no_source_, resolver_old_content_);
    }
    // And what they cost per frame, which is the only thing that decides. 0.78 real ms per Mpixel
    // copied and 0.075 per Mpixel cleared, measured here.
    const double frames = double(presented_ - presented_report_copies_);
    const double perFot = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp_rest = double(restored_pixels_ - restored_previous_pixels_) / 1e6;
    const double mp_saving =
        double(pixels_restore_saved_ - pixels_restore_saved_previous_) / 1e6;
    SA2_REPORT_RING("[native] targets: restores per frame: {:.2f} Mpixels copied ({:.2f} ms "
                "real) and {:.2f} Mpixels saved ({:.2f} ms); since startup: {} clipped "
                "to the useful area",
                mp_rest * perFot, mp_rest * perFot * 0.78, mp_saving * perFot,
                mp_saving * perFot * 0.78, clipped_restores_);
    restored_previous_pixels_ = restored_pixels_;
    pixels_restore_saved_previous_ = pixels_restore_saved_;
  }
  if (cleared_skipped_ || cleared_skipped_depth_) {
    // Clears that did not change a single bit (sa2_native_skip_cleared_repeated). A clear costs
    // 0.075 real ms per Mpixel: ten times less than a copy, but it adds up.
    const double frames = double(presented_ - presented_report_copies_);
    const double perFot = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp = double(cleared_skipped_pixels_ - cleared_skipped_previous_pixels_) / 1e6;
    SA2_REPORT_RING("[native] targets: clears skipped: {} color and {} depth since startup; "
                "{:.2f} Mpixels per frame not cleared ({:.2f} ms real)",
                cleared_skipped_, cleared_skipped_depth_, mp * perFot,
                mp * perFot * 0.075);
    cleared_skipped_previous_pixels_ = cleared_skipped_pixels_;
  }
  if (cleared_useless_) {
    // Clears wiped out by a swap or a restore before anything was drawn. These are the real candidates
    // for removal (the equivalent of a loadOp = DONT_CARE).
    const double frames = double(presented_ - presented_report_copies_);
    const double perFot = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp = double(cleared_useless_pixels_ - cleared_useless_previous_pixels_) / 1e6;
    SA2_REPORT_RING("[native] targets: clears that served no purpose (the target changed content without "
                "anything being drawn): {} since startup; {:.2f} Mpixels per frame "
                "({:.2f} ms real)",
                cleared_useless_, mp * perFot, mp * perFot * 0.075);
    cleared_useless_previous_pixels_ = cleared_useless_pixels_;
  }
  if (postponed_front_ || front_late_reads_) {  // sa2_native_lazy_front
    const double frames = double(presented_ - presented_report_copies_);
    const double perFot = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp = double(front_saved_pixels_ - front_saved_previous_pixels_) / 1e6;
    SA2_REPORT_RING("[native] targets: lazy front (deferred): {} copies to front buffers postponed since startup; "
                         "Swaps painted from the target {} and from a retained image {}; {} clears on a "
                         "spare image ({} without a spare); recorded: {} when sampled, {} before writing the "
                         "target again or another resolve, {} in the Swap (FXAA, no ramp or another output); {} dropped "
                         "without copying (another resolve covers them entirely): {:.2f} Mpixels per frame not copied (~{:.2f} "
                         "ms real); {} spare images; late reads {}{}",
                         postponed_front_, painted_front_target_, painted_retained_front_,
                         front_rotations_, front_no_replenished_, copied_front_read_,
                         copied_front_write_, copied_front_swap_, replaced_front_, mp * perFot,
                         mp * perFot * 0.60, fronts_images_.size(), front_late_reads_,
                         off_front_ ? " *** TURNED OFF BY THE GUARD ***" : " (0 = the image is the same)");
    front_saved_previous_pixels_ = front_saved_pixels_;
  }
  presented_report_copies_ = presented_;
  if (no_swap_[0] || no_swap_[1] || no_swap_[2]) {
    SA2_REPORT_RING("[native] targets: depth copies without swap since the previous report: {} because the "
                "command does not clear the target, {} because it is not resolved whole, {} because it was not possible",
                no_swap_[0], no_swap_[1], no_swap_[2]);
    no_swap_ = {};
  }
}


// Read-back of a small resolved texture: the copied rectangle goes to a host-visible buffer in the
// same submission; WriteReads moves it into guest memory.
void TargetsDk::ResolvedRead(const RegistersCopy& reg, const Resolved& resolved, int32_t x0, int32_t y0,
                  uint32_t dx, uint32_t dy, uint32_t width, uint32_t height) {
  const int32_t max = REXCVAR_GET(sa2_native_resolved_read_texels);
  if (max <= 0 || uint64_t(width) * height > uint64_t(max) ||
      resolved.image.format != kColorFormat) {
    return;
  }
  const uint64_t target_key =
      (uint64_t(reg.rb_copy_dest_base) << 28) ^ (uint64_t(width) << 14) ^ uint64_t(height);
  TargetRead& target = reads_per_target_[target_key];
  target.base = reg.rb_copy_dest_base;
  target.width = width;
  target.height = height;
  const uint64_t each = uint64_t(std::max(REXCVAR_GET(sa2_native_reads_each), 1));
  if (target.copies++ % each != 0) {
    ++target.skipped;  // exposure changes slowly: the guest keeps the previous one
    return;
  }
  const uint64_t bytes = uint64_t(width) * height * 4;
  const uint64_t key = (uint64_t(reg.rb_copy_dest_base) << 32) ^ (uint64_t(uint32_t(x0)) << 16) ^
                         uint64_t(uint32_t(y0));
  // One buffer per slot: the previous submission may still be copying into the other slot's.
  Read* pointer = &reads_[key ^ (uint64_t(slot_) << 63)];
  if (pointer->bytes < bytes && pointer->block) {
    // Larger: the old buffer may have recorded or submitted copies.
    SendWork(true);
    ++waits_gpu_reason_[5]; WaitGpu();
    DestroyRead(*pointer);
    if (!Record()) {
      return;
    }
    pointer = &reads_[key ^ (uint64_t(slot_) << 63)];
    if (pointer->bytes < bytes && pointer->block) {
      DestroyRead(*pointer);  // the GPU has nothing pending any more
    }
  }
  Read& read = *pointer;
  if (read.bytes < bytes) {
    // deko3d: MemoryPurpose::kReadback = CPU cached, GPU cached (sa2::dk::kReadback); the block is mapped from the
    // start (no vkMapMemory step that could fail) and is not coherent: WriteReads flushes the CPU cache first.
    read.block = sa2::dk::MakeBlock(uint32_t(bytes), sa2::dk::kReadback, "read-back of a resolved texture");
    if (!read.block) {
      read = Read{};
      Reject(16, "could not create a read-back buffer for a resolved texture");
      return;
    }
    read.data = read.block.cpu;
    read.bytes = bytes;
    read.coherent = (read.block.flags & DkMemBlockFlags_CpuAccessMask) != DkMemBlockFlags_CpuCached;
  }
  // VkBufferImageCopy: the image rectangle, tightly packed in the buffer (bufferRowLength 0 = rowLength 0).
  DkImageRect copy{dx, dy, 0, width, height, 1};
  sa2::dk::ImageId read_image = resolved.image.image;
  if (resolved.image.scale_q != kScaleQuartersNative) {
    // sa2-nx render resolution: the guest reads guest-size texels. The scaled rectangle is filtered down into a
    // guest-size staging image first (a box average at 2x, two taps at 1.5x), and that one is read back.
    Image* staging = StagingRead(width, height);
    if (!staging || !(resolved.image.dk_flags & DkImageFlags_Usage2DEngine)) {  // was !blit_: the 2D engine reads it
      Reject(16, "could not create a read-back staging image for a scaled resolved texture");
      return;
    }
    const VkRect2D host = HostRect(resolved.image, VkRect2D{{int32_t(dx), int32_t(dy)}, {width, height}});
    const DkImageRect down_source{uint32_t(host.offset.x), uint32_t(host.offset.y), 0, host.extent.width,
                                  host.extent.height, 1};
    const DkImageRect down_target{0, 0, 0, width, height, 1};
    const DkImageView source_view = sa2::dk::View(resolved.image.image);
    const DkImageView staging_view = sa2::dk::View(staging->image);
    BarrierBeforeCopyResolve();
    dkCmdBufBlitImage(commands_work_, &source_view, &down_source, &staging_view, &down_target,
                      DkBlitFlag_ModeBlit | DkBlitFlag_FilterLinear, 0);
    BarrierAfterCopyResolve();
    read_image = staging->image;
    copy.x = copy.y = 0;
  }
  {
    const DkImageView read_view = sa2::dk::View(read_image);
    const DkCopyBuf buffer{read.block.gpu, 0, 0};
    dkCmdBufCopyImageToBuffer(commands_work_, &read_view, &copy, &buffer, 0);
  }
  pending_reads_.push_back({&read, reg.rb_copy_dest_base, x0, y0, width, height,
                                  reg.rb_copy_dest_pitch & 0x3FFF,
                                  (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info});
  if (done_reads_++ == 0) {
    REXLOG_INFO("[native] targets: read-back of resolved textures of up to {} texels (the "
                "first: {:08X}, {}x{})",
                max, reg.rb_copy_dest_base, width, height);
  }
}


// The submission finished: the submitted read-backs go to guest memory. Loaded with its fetch
// constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the resolved
// texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it.
void TargetsDk::WriteReads(std::vector<PendingRead>& reads) {
  const auto before_reads = std::chrono::steady_clock::now();
  if (before_reads - report_reads_ >= std::chrono::seconds(10)) {
    std::string list;
    for (auto it = reads_per_target_.begin(); it != reads_per_target_.end();) {
      TargetRead& d = it->second;
      if (!d.copies) {
        it = reads_per_target_.erase(it);  // has not appeared again
        continue;
      }
      list += fmt::format(" {:08X} {}x{} {}/{};", d.base, d.width, d.height, d.copies - d.skipped,
                           d.copies);
      d.copies = 0;
      d.skipped = 0;
      ++it;
    }
    if (!list.empty()) {
      SA2_REPORT_RING("[native] targets: read-backs per target (done/copies):{}", list);
    }
    report_reads_ = before_reads;
  }
  for (const PendingRead& p : reads) {
    const Read& read = *p.read;
    if (!read.data) {
      continue;
    }
    if (!read.coherent) {
      // Was vkInvalidateMappedMemoryRanges(VK_WHOLE_SIZE): dkMemBlockFlushCpuCache cleans and invalidates.
      sa2::dk::FlushCpu(read.block, 0, read.block.size);
    }
    static const uint32_t diag_base =
        uint32_t(std::strtoul(REXCVAR_GET(ds2_diag_fetch_base).c_str(), nullptr, 16)) & 0x1FFFFFFF;
    if (diag_base && (p.base & 0x1FFFFFFF) == diag_base && logged_diag_reads_ < 6 && (++diag_reads_seen_ % 120) == 0) {
      ++logged_diag_reads_;
      std::string texels;
      for (uint32_t i = 0; i < p.width * p.height; i += std::max<uint32_t>(1, p.width * p.height / 16)) {
        texels += fmt::format(" [{}]{:02X}{:02X}{:02X}{:02X}", i, read.data[i * 4], read.data[i * 4 + 1],
                              read.data[i * 4 + 2], read.data[i * 4 + 3]);
      }
      REXLOG_INFO("[native] ds2_diag_fetch_base {:08X}: read-back {}x{} at {},{} info {:08X}, host bytes:{}", p.base,
                  p.width, p.height, p.x0, p.y0, p.info, texels);
    }
    const bool swap = (p.info >> 24) & 0x1;
    const uint32_t order_copy = p.info & 0x7;  // Endian128: 0-3 as xenos::Endian
    const auto order =
        order_copy <= 3 ? static_cast<xenos::Endian>(order_copy) : xenos::Endian::k8in32;
    // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap): it is computed once with
    // marked bytes. Addresses come from the tile and the tiling table, and the guest's physical memory
    // is contiguous.
    const uint32_t permutation =
        xenos::GpuSwap(swap ? uint32_t(0x03000102) : uint32_t(0x03020100), order);
    const uint8_t b0 = uint8_t(permutation), b1 = uint8_t(permutation >> 8),
                  b2 = uint8_t(permutation >> 16), b3 = uint8_t(permutation >> 24);
    uint8_t* const physical = memory_->TranslatePhysical(0);
    const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
    const auto& table = TableTile2DTexel4();
    const uint64_t tiles_per_row = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
    for (uint32_t j = 0; j < p.height; ++j) {
      const uint32_t ty = uint32_t(p.y0) + j;
      if (p.target_height && ty >= p.target_height) {
        break;
      }
      const uint64_t row = base + ((uint64_t(ty >> 5) * tiles_per_row) << 12);
      const uint16_t* const local = table.data() + size_t(ty & 31) * 32;
      const uint8_t* s = read.data + size_t(j) * p.width * 4;
      for (uint32_t i = 0; i < p.width; ++i, s += 4) {
        const uint32_t tx = uint32_t(p.x0) + i;
        if (tx >= p.pitch) {
          break;
        }
        const uint64_t address = row + (uint64_t(tx >> 5) << 12) + local[tx & 31];
        if (address + 4 > 0x20000000) {
          continue;
        }
        const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
        uint8_t* const d = physical + address;
        d[0] = t0;
        d[1] = t1;
        d[2] = t2;
        d[3] = t3;
      }
    }
  }
  reads.clear();
}

Image* TargetsDk::StagingRead(uint32_t width, uint32_t height) {
  auto [it, fresh] = staging_reads_.try_emplace((uint64_t(width) << 32) | height);
  // deko3d: TRANSFER_DST of the 2D engine's downscale (DkImageFlags_Usage2DEngine); TRANSFER_SRC of the copy engine
  // needs no flag.
  if (fresh && !CreateScaled(it->second, width, height, DkImageFlags_Usage2DEngine, kColorFormat,
                             VK_SAMPLE_COUNT_1_BIT, kScaleQuartersNative)) {
    staging_reads_.erase(it);
    return nullptr;
  }
  Prepare(it->second);
  return it->second.prepared ? &it->second : nullptr;
}


// deko3d: one block (no unmap, no separate buffer object). Its callers have waited for the GPU (or are about to
// record nothing more with it): freed after everything recorded so far, as DestroyImage.
void TargetsDk::DestroyRead(Read& read) {
  sa2::dk::FreeBlock(read.block, sa2::dk::CurrentOrder());
  read = Read{};
}
// ---- deko3d: the output chain and the helpers of this region ----

// RescaleKeep / RescaleAll: blit_ok and the blit (identical in both in the Vulkan code).
// Vulkan: blit_ != nullptr and TRANSFER_SRC | TRANSFER_DST usage. deko3d: the 2D engine reads and writes only images
// created with DkImageFlags_Usage2DEngine; fresh has the same flags as image (CreateScaled with image.dk_flags).
// Depth through the 2D engine is the scalable_depth_ question (infra decides it at start, as the Vulkan code did from
// the format's blit features).
bool TargetsDk::BlitScaledDk(const Image& image, const Image& fresh, bool depth) {
  const bool blit_ok = image.prepared && me::native::IsSingleSample(uint32_t(image.sample_count)) &&
                       (image.dk_flags & DkImageFlags_Usage2DEngine) && (fresh.dk_flags & DkImageFlags_Usage2DEngine) &&
                       (!depth || scalable_depth_);
  if (!blit_ok) return false;
  // Was VkImageBlit over the whole image, both aspects for depth (deko3d blits the whole texel).
  const DkImageRect s{0, 0, 0, HostWidth(image), HostHeight(image), 1};
  const DkImageRect d{0, 0, 0, HostWidth(fresh), HostHeight(fresh), 1};
  const bool linear = image.format == VK_FORMAT_R8G8B8A8_UNORM ||
                      image.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  const DkImageView source_view = sa2::dk::View(image.image);
  const DkImageView target_view = sa2::dk::View(fresh.image);
  BarrierBeforeCopyResolve();
  dkCmdBufBlitImage(commands_work_, &source_view, &s, &target_view, &d,
                    DkBlitFlag_ModeBlit | (linear ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest), 0);
  BarrierAfterCopyResolve();
  return true;
}

bool TargetsDk::PaintOutput(Image& source, uint32_t width, uint32_t height, bool from_target) {
  // With sa2_native_output_no_wait, the next of 3 slots, and it only waits if that one is still pending.
  // Without it, always slot 0: it waits for the previous output.
  const uint32_t s = output_no_wait_ ? (output_current_ + 1) % kSlotsOutput : 0;
  if (pending_outputs_[s]) {
    const auto before_wait = std::chrono::steady_clock::now();  // output-wait report
    sa2::dk::WaitOrder(orders_output_[s]);  // was vkWaitForFences(UINT64_MAX), result unchecked
    ns_wait_output_ += Ns(before_wait, std::chrono::steady_clock::now());
    pending_outputs_[s] = false;  // (vkResetFences: orders need no reset)
  }
  output_current_ = s;
  // deko3d: the slot's command memory and uniform buffers are rewound here, its last list being complete (was
  // vkResetCommandPool).
  streams_output_dk_[s].Reset();
  constants_output_dk_[s].Reset();
  // The gamma ramp, if it changed since this slot's last output (the GPU is no longer reading it).
  if (ramp_gamma_ && ramps_output_dk_[s].version != version_ramp_) {
    RampOutputDk& ramp = ramps_output_dk_[s];
    std::memcpy(ramp.block.cpu, ramp_values_.data(), sizeof(ramp_values_));
    sa2::dk::FlushCpu(ramp.block, 0, uint32_t(sizeof(ramp_values_)));  // was FlushMappedMemoryRange
    ramp.version = version_ramp_;
  }
  // deko3d: the window takes the place of the presenter's guest output image and framebuffers_ (one per image
  // version): render targets are bound per pass, there is nothing to create per window image.
  if (!EnsureWindowOutput()) {
    return false;
  }
  const uint32_t output_width = window_output_.Width();
  const uint32_t output_height = window_output_.Height();
  // What the presenter ran after this pass (GetGuestOutputPaintFlow), for this frame's present_effect (L+R+left
  // stick changes it in game: read every frame).
  dkoutput::Effect effect = dkoutput::ParseEffect(REXCVAR_GET(present_effect));
  if (!fs_easu_output_ || !fs_rcas_output_) effect = dkoutput::Effect::kBilinear;  // (logged by InitializeOutput)
  const dkoutput::Plan plan =
      dkoutput::PlanOutput(effect, width, height, output_width, output_height,
                           uint32_t(std::max<int32_t>(REXCVAR_GET(present_fsr_max_upsampling_passes), 1)));
  if (plan.count) {
    // The ramp pass and every effect but the last write an intermediate image.
    uint32_t need_w = width, need_h = height;
    for (uint32_t k = 0; k + 1 < plan.count; ++k) {
      need_w = std::max(need_w, plan.steps[k].width);
      need_h = std::max(need_h, plan.steps[k].height);
    }
    if (!EnsureIntermediatesOutput(need_w, need_h)) {
      return false;
    }
  }
  if (!logged_output_plan_ || logged_output_effect_ != uint8_t(effect)) {
    logged_output_plan_ = true;
    logged_output_effect_ = uint8_t(effect);
    std::string passes;
    for (uint32_t k = 0; k < plan.count; ++k) {
      const dkoutput::Step& step = plan.steps[k];
      passes += fmt::format(" {} {}x{};",
                            step.pass == dkoutput::Pass::kEasu   ? "EASU"
                            : step.pass == dkoutput::Pass::kRcas ? "RCAS"
                                                                 : "bilinear",
                            step.width, step.height);
    }
    REXLOG_INFO("[dk] targets: output {}x{} -> window {}x{}, present_effect {}: ramp pass{}{}", width, height,
                output_width, output_height, NameEffect(effect), plan.count ? " to an intermediate;" : " to the window",
                passes);
  }

  // The source of the ramp pass (was the slot's descriptor set, vkUpdateDescriptorSets).
  sa2::dk::WriteImageDescriptor(descriptors_source_output_[s], sa2::dk::View(source.image));

  const DkCmdBuf cmd = streams_output_dk_[s].cmd();
  StartList(cmd);  // (was vkBeginCommandBuffer) the descriptor sets, and the invalidations owed
  // That image was just written as a render target (or copy destination) in the frame's work, which goes in
  // another submission: make the output read it fully written.
  // deko3d: for every source, not only from_target: the Vulkan output also relied on the work submission's closing
  // memory barrier, and a deko3d list does not order its reads after the previous list's writes by itself. The
  // source may come from a render, a copy or a dispatch: Full, once per frame (the bench may make it lighter).
  sa2::dk::Barrier(cmd, sa2::dk::Hazard::Full);
  state_output_.Bind(cmd);
  // The window image: dkQueueAcquireImage after the work was submitted, so the queue's wait for the display only
  // holds the output (as the TriAevum present, gfx_deko3d_device.cpp:493-501).
  const int window_slot = window_output_.Acquire();
  const DkImageView window_view = window_output_.View(window_slot);
  const DkImageView intermediate_views[2] = {sa2::dk::View(intermediates_output_[0]),
                                             sa2::dk::View(intermediates_output_[1])};

  const DkShader* fragment = fs_output_;
  if (ramp_gamma_) {
    // With an output the size of the source (the normal case), the exact texel, unfiltered.
    // From a render target, the output has the front buffer's size, not the image's
    // (FrontOnPresent only requests it that way): texelFetch of the pixel, the same texel the texture
    // would hold.
    const bool exact = from_target || (width == HostWidth(source) && height == HostHeight(source));
    // ds2_output_fxaa: its uv is the output pixel over the output size, so not from a render target bigger than
    // the front buffer (sa2_native_lazy_front): that frame goes out as before.
    const bool fxaa = output_fxaa_ && !from_target;
    fragment = ShaderRamp(fxaa ? 2 : exact ? 1 : 0);
    if (!logged_output_variant_) {
      logged_output_variant_ = true;
      REXLOG_INFO("[native] targets: first output: variant {} ({}x{} from {}x{}, from a render target {})",
                  fxaa ? 2 : exact ? 1 : 0, width, height, HostWidth(source), HostHeight(source), from_target);
    }
    if (fragment == nullptr) {
      fragment = shaders_ramp_[exact ? 1 : 0];  // requested variant missing: the usual one
    }
    if (!exact && !warned_bilinear_ramp_) {
      warned_bilinear_ramp_ = true;
      REXLOG_INFO("[native] targets: {}x{} output from a {}x{} source: ramp with bilinear sampling", width,
                  height, HostWidth(source), HostHeight(source));
    }
  }
  if (fragment == nullptr) {
    return false;  // (Vulkan: a null pipeline cannot exist here; Initialize turned the ramp off without it)
  }
  // The push constants of the Vulkan pass: offset 16 of the fragment stage = uniform buffer 0 of the fragment
  // shader with the same offsets (sa2_output_ramp_gamma: layout(offset = 16) desplazamiento, 24 inverso).
  struct alignas(16) ConstantsBilinear {
    uint8_t vertex_part[16];  // the rectangle, pushed to the vertex stage
    int32_t displacement[2];
    float inverse[2];
  };
  ConstantsBilinear bilinear{{}, {0, 0}, {1.0f / float(width), 1.0f / float(height)}};
  PassOutput(cmd, s, plan.count ? intermediate_views[0] : window_view, width, height, fragment, &bilinear,
             sizeof(bilinear), descriptors_source_output_[s], ramp_gamma_);
  // The presenter's passes (dkoutput::PlanOutput), ping-pong between the two intermediates; the last one writes the
  // window. Each reads what the previous one rendered.
  uint32_t input_width = width, input_height = height;
  for (uint32_t k = 0; k < plan.count; ++k) {
    const dkoutput::Step& step = plan.steps[k];
    const bool last = k + 1 == plan.count;
    const uint32_t in = k & 1;
    sa2::dk::Barrier(cmd, sa2::dk::Hazard::RenderToSample);
    const DkImageView& target = last ? window_view : intermediate_views[in ^ 1];
    const uint32_t texture = descriptors_intermediate_output_[in];
    switch (step.pass) {
      case dkoutput::Pass::kEasu: {
        const auto con = dkoutput::EasuConstants(float(input_width), float(input_height),
                                                 float(width_intermediates_output_),
                                                 float(height_intermediates_output_), float(step.width),
                                                 float(step.height));
        PassOutput(cmd, s, target, step.width, step.height, fs_easu_output_, con.data(),
                   uint32_t(con.size() * sizeof(uint32_t)), texture, false);
        break;
      }
      case dkoutput::Pass::kRcas: {
        struct alignas(16) {
          std::array<uint32_t, 4> con;
          int32_t offset[2];
          int32_t input_size[2];
        } rcas{dkoutput::RcasConstants(float(REXCVAR_GET(present_fsr_sharpness_reduction))),
               {0, 0},
               {int32_t(input_width), int32_t(input_height)}};
        PassOutput(cmd, s, target, step.width, step.height, fs_rcas_output_, &rcas, sizeof(rcas), texture, false);
        break;
      }
      case dkoutput::Pass::kBilinear: {
        // The picture is in the top left corner of the intermediate: uv = (pixel + 0.5) / output * picture / image.
        ConstantsBilinear stretch{
            {},
            {0, 0},
            {float(input_width) / float(width_intermediates_output_) / float(step.width),
             float(input_height) / float(height_intermediates_output_) / float(step.height)}};
        PassOutput(cmd, s, target, step.width, step.height, fs_output_, &stretch, sizeof(stretch), texture, false);
        break;
      }
    }
    input_width = step.width;
    input_height = step.height;
  }
  if (from_target) {
    // And nothing submitted later (the next frame draws and clears that image again) writes it before the
    // output has read it.
    sa2::dk::Barrier(cmd, sa2::dk::Hazard::Full);
  }
  constants_output_dk_[s].FlushUsed();
  orders_output_[s] = sa2::dk::Submit(streams_output_dk_[s].Finish());
  window_output_.Present(window_slot);
  pending_outputs_[s] = true;
  // (context.SetIs8bpc(true): the presenter's dither flag; there is no presenter.)
  return true;
}

void TargetsDk::PassOutput(DkCmdBuf cmd, uint32_t s, const DkImageView& target, uint32_t width, uint32_t height,
                           const DkShader* fragment, const void* constants, uint32_t constants_bytes,
                           uint32_t texture, bool ramp) {
  dkCmdBufBindRenderTarget(cmd, &target, nullptr);
  sa2::dk::SetViewport(cmd, 0.0f, 0.0f, float(width), float(height));
  const DkScissor scissor{0, 0, width, height};
  dkCmdBufSetScissors(cmd, 0, &scissor, 1);
  const DkShader* shaders[2] = {vs_output_, fragment};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  const float rectangle[4] = {-1.0f, -1.0f, 2.0f, 2.0f};  // x, y, width, height in NDC
  dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, constants_output_dk_[s].Push(rectangle, sizeof(rectangle)),
                            sizeof(rectangle));
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, constants_output_dk_[s].Push(constants, constants_bytes),
                            constants_bytes);
  if (ramp) {
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 2, ramps_output_dk_[s].block.gpu,
                              (uint32_t(sizeof(ramp_values_)) + 15u) & ~15u);
  }
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, sa2::dk::Handle(texture, sampler_output_));
  dkCmdBufDraw(cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

bool TargetsDk::EnsureWindowOutput() {
  const bool docked = sa2::render::Docked();
  if (window_output_ready_ && docked == window_output_docked_) {
    return true;
  }
  if (window_output_ready_) {
    WaitOutputs();               // the pending outputs present into the old swapchain
    window_output_.Destroy();    // (waits for the queue to be idle)
    window_output_ready_ = false;
  }
  const uint32_t w = docked ? 1920 : 1280;
  const uint32_t h = docked ? 1080 : 720;
  if (!window_output_.Create(w, h, 4)) {
    REXLOG_ERROR("[dk] targets: could not create the {}x{} window ({})", w, h, docked ? "docked" : "handheld");
    return false;
  }
  window_output_ready_ = true;
  window_output_docked_ = docked;
  logged_output_plan_ = false;  // the chain changes with the display size
  return true;
}

bool TargetsDk::EnsureIntermediatesOutput(uint32_t width, uint32_t height) {
  if (intermediates_output_[0] != sa2::dk::kNoImage && width <= width_intermediates_output_ &&
      height <= height_intermediates_output_) {
    return true;
  }
  width = std::max(width, width_intermediates_output_);
  height = std::max(height, height_intermediates_output_);
  for (uint32_t i = 0; i < 2; ++i) {
    // Still read by a pending output: released after everything recorded so far.
    if (intermediates_output_[i] != sa2::dk::kNoImage) sa2::dk::DestroyImage(intermediates_output_[i]);
    sa2::dk::ImageDesc desc;
    desc.format = DkImageFormat_RGB10A2_Unorm;  // the presenter's guest output (A2B10G10R10)
    desc.width = width;
    desc.height = height;
    desc.flags = DkImageFlags_UsageRender;
    desc.logical = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    intermediates_output_[i] = sa2::dk::CreateImage(desc, "output intermediate");
    if (intermediates_output_[i] == sa2::dk::kNoImage) {
      REXLOG_ERROR("[dk] targets: could not create the {}x{} output intermediates", width, height);
      width_intermediates_output_ = height_intermediates_output_ = 0;
      return false;
    }
    sa2::dk::WriteImageDescriptor(descriptors_intermediate_output_[i], sa2::dk::View(intermediates_output_[i]));
  }
  width_intermediates_output_ = width;
  height_intermediates_output_ = height;
  REXLOG_INFO("[dk] targets: output intermediates of {}x{} (RGB10A2, 2 images)", width, height);
  return true;
}

const DkShader* TargetsDk::ShaderRamp(uint32_t index) {
  const DkShader*& shader = shaders_ramp_[index];
  if (shader == nullptr && !shaders_failed_ramp_[index]) {
    // index bit 0: exact texel; bit 1: FXAA (ds2_output_fxaa). Its FXAA 3.11 Quality form
    // (ds2_output_fxaa_quality, read at start, so the same for every variant).
    // deko3d: the specialization constants are baked: one DKSH per (exact, FXAA, quality), the grading constant 1
    // off in all of them (shaders_dk/adaptar_dk.py, sa2_output_ramp_gamma_e*_f*_q*).
    static constexpr dkshaders::Id kVariants[2][2][2] = {
        {{dkshaders::kSa2OutputRampGammaE0F0Q0, dkshaders::kSa2OutputRampGammaE0F0Q1},
         {dkshaders::kSa2OutputRampGammaE0F1Q0, dkshaders::kSa2OutputRampGammaE0F1Q1}},
        {{dkshaders::kSa2OutputRampGammaE1F0Q0, dkshaders::kSa2OutputRampGammaE1F0Q1},
         {dkshaders::kSa2OutputRampGammaE1F1Q0, dkshaders::kSa2OutputRampGammaE1F1Q1}}};
    const auto before = std::chrono::steady_clock::now();
    shader = vs_output_ ? LoadShader(kVariants[index & 1][(index >> 1) & 1][REXCVAR_GET(ds2_output_fxaa_quality) ? 1 : 0])
                        : nullptr;
    if (shader == nullptr) {
      shaders_failed_ramp_[index] = true;
      REXLOG_ERROR("[native] targets: could not create variant {} of the output pass", index);
    } else {
      REXLOG_INFO("[native] targets: variant {} of the output pass (exact {}, FXAA {}) created in {:.1f} ms",
                  index, (index & 1) ? "yes" : "no", (index & 2) ? "yes" : "no",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
    }
  }
  return shader;
}

// The output part of the Vulkan Initialize (1555-1650: the output sampler, set layout, pool and sets, the ramp
// buffers, vs_ / fs_ and pipeline_; the render pass and the pipelines have no deko3d object). The ramp variants are
// loaded by Initialize through ShaderRamp, as it called PipelineRamp.
bool TargetsDk::InitializeOutput() {
  vs_output_ = LoadShader(dkshaders::kOutputRect);
  fs_output_ = LoadShader(dkshaders::kOutputBilinear);
  if (!vs_output_ || !fs_output_) {
    REXLOG_ERROR("[dk] targets: could not load the output shaders");
    return false;
  }
  fs_easu_output_ = LoadShader(dkshaders::kOutputEasu);
  fs_rcas_output_ = LoadShader(dkshaders::kOutputRcas);
  if (!fs_easu_output_ || !fs_rcas_output_) {
    REXLOG_ERROR("[dk] targets: the FSR output shaders did not load: present_effect is bilinear");
  }
  for (uint32_t s = 0; s < kSlotsOutput; ++s) {
    if (!streams_output_dk_[s].Initialize(kCommandBytesOutput, "output commands") ||
        !constants_output_dk_[s].Initialize(64u << 10, sa2::dk::kCpuToGpu, "output constants")) {
      return false;
    }
    // One ramp buffer per output slot, so it is only changed in a slot the GPU is no longer using.
    ramps_output_dk_[s].block = sa2::dk::MakeBlock(uint32_t(sizeof(ramp_values_)), sa2::dk::kCpuToGpu, "gamma ramp");
    if (!ramps_output_dk_[s].block) {
      return false;
    }
    ramps_output_dk_[s].version = UINT64_MAX;
    descriptors_source_output_[s] = sa2::dk::AllocImageDescriptor();
    if (descriptors_source_output_[s] == UINT32_MAX) {
      return false;
    }
  }
  for (uint32_t& d : descriptors_intermediate_output_) {
    d = sa2::dk::AllocImageDescriptor();
    if (d == UINT32_MAX) {
      return false;
    }
  }
  // Linear, clamp to edge (the output sampler; textureGather and texelFetch ignore the filter).
  sampler_output_ = sa2::dk::AllocSamplerDescriptor();
  if (sampler_output_ == UINT32_MAX) {
    return false;
  }
  DkSampler sampler;
  dkSamplerDefaults(&sampler);
  sampler.minFilter = DkFilter_Linear;
  sampler.magFilter = DkFilter_Linear;
  sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
  sa2::dk::WriteSamplerDescriptor(sampler_output_, sampler);
  if (!EnsureWindowOutput()) {
    return false;
  }
  REXLOG_INFO("[dk] targets: output without the SDK presenter: {} slots, window {}x{} (4 images, interval 0), "
              "present_effect {}", kSlotsOutput, window_output_.Width(), window_output_.Height(),
              REXCVAR_GET(present_effect));
  return true;
}

void TargetsDk::DestroyOutput() {
  const uint64_t after = sa2::dk::CurrentOrder();
  if (window_output_ready_) {
    window_output_.Destroy();  // (waits for the queue to be idle)
    window_output_ready_ = false;
  }
  for (uint32_t s = 0; s < kSlotsOutput; ++s) {
    streams_output_dk_[s].Release(after);
    constants_output_dk_[s].Release(after);
    sa2::dk::FreeBlock(ramps_output_dk_[s].block, after);
    if (descriptors_source_output_[s] != UINT32_MAX) {
      sa2::dk::FreeImageDescriptor(descriptors_source_output_[s], after);
      descriptors_source_output_[s] = UINT32_MAX;
    }
  }
  for (uint32_t i = 0; i < 2; ++i) {
    if (intermediates_output_[i] != sa2::dk::kNoImage) sa2::dk::DestroyImage(intermediates_output_[i], after);
    intermediates_output_[i] = sa2::dk::kNoImage;
    if (descriptors_intermediate_output_[i] != UINT32_MAX) {
      sa2::dk::FreeImageDescriptor(descriptors_intermediate_output_[i], after);
      descriptors_intermediate_output_[i] = UINT32_MAX;
    }
  }
  if (sampler_output_ != UINT32_MAX) {
    sa2::dk::FreeSamplerDescriptor(sampler_output_, after);
    sampler_output_ = UINT32_MAX;
  }
}

}  // namespace sa2::native

#endif  // SA2_DK_AVAILABLE
