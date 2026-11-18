// SA2 - native renderer on deko3d (ds2-deko3d): TargetsDk, region "alvos-copias". Translation of TargetsVulkan
// (sa2_native_targets.cpp) 1752-2612: Copy / CopyInternal, ClearDepth, CopyDepth; and 4064-6497: Reject,
// Rectangle, GetTarget and the EDRAM mode-4 bookkeeping (ownership, SDK clear plans, PublishEDRAM4,
// SynchronizeEDRAM4, the transfer batch, CopyTilesEDRAM4, the redirected clears, the lazy stencil, the
// PrepareDrawEDRAM4 memo). Same logic, order, counters and logs as the Vulkan code; the GPU calls go to:
//   vkCmdCopyImage            -> dkCmdBufCopyImage (copy engine DMA; whole texels, depth and stencil together)
//   vkCmdBlitImage            -> dkCmdBufBlitImage (2D engine; images created with DkImageFlags_Usage2DEngine)
//   vkCmdResolveImage         -> dkCmdBufResolveImage (2D engine; whole images of equal size only)
//   vkCmdClear*Image          -> the draws' Clear*InPass (3D engine clear on the bound target)
//   vkCmdPipelineBarrier      -> sa2::dk::Barrier(cmd, Hazard::...) named by the dependency it serves
// Members and declarations: sa2_native_targets_dk_copias.inc. Report: ds2-deko3d
// docs/pesquisas/traducao/02-alvos-copias.md, section "Fase C".

#include "sa2_native_targets_dk.h"

#if SA2_DK_AVAILABLE

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The cvars and enhancements of this region are declared in sa2_native_targets_dk_types.h (alvos-infra).


namespace sa2::native {

namespace {

namespace xenos = rex::graphics::xenos;

// Vulkan: clear_depth_ / copy_image_ were function pointers loaded at start (null = no such command). deko3d
// always has the depth clear (dkCmdBufClearDepthStencil, through the draws) and the copy engine.
constexpr bool kClearDepthDk = true;
constexpr bool kCopyImageDk = true;
// Vulkan: vkGetPhysicalDeviceFormatProperties(R32_SFLOAT) had SAMPLED | STORAGE | TRANSFER_SRC | TRANSFER_DST.
// deko3d: R32_SFLOAT is sampled, load/store and copy capable on the Tegra X1 (format_traits.inc).
constexpr bool kR32FloatFullDk = true;

DkImageRect RectDk(const VkOffset3D& offset, const VkExtent3D& extent) {
  return {uint32_t(offset.x), uint32_t(offset.y), uint32_t(offset.z), extent.width, extent.height, extent.depth};
}

// vkCmdCopyImage -> dkCmdBufCopyImage, one copy-engine launch per region. The regions here never ask for one
// aspect only: deko3d copies the whole texel (depth with its stencil), which is what a depth + stencil pair did.
void CopyImageDk(DkCmdBuf cmd, sa2::dk::ImageId source, sa2::dk::ImageId target, uint32_t count,
                 const VkImageCopy* regions) {
  const DkImageView source_view = sa2::dk::View(source);
  const DkImageView target_view = sa2::dk::View(target);
  for (uint32_t i = 0; i < count; ++i) {
    const VkImageCopy& r = regions[i];
    const DkImageRect s = RectDk(r.srcOffset, r.extent);
    const DkImageRect d = RectDk(r.dstOffset, r.extent);
    dkCmdBufCopyImage(cmd, &source_view, &s, &target_view, &d, 0);
  }
}

// vkCmdBlitImage -> dkCmdBufBlitImage (2D engine). The rectangles come from the two corner offsets (never
// flipped here); the filter is DkBlitFlag_FilterLinear or DkBlitFlag_FilterNearest.
void BlitDk(DkCmdBuf cmd, sa2::dk::ImageId source, sa2::dk::ImageId target, const VkImageBlit& blit,
            uint32_t filter) {
  const DkImageView source_view = sa2::dk::View(source);
  const DkImageView target_view = sa2::dk::View(target);
  const DkImageRect s{uint32_t(blit.srcOffsets[0].x), uint32_t(blit.srcOffsets[0].y), 0,
                      uint32_t(blit.srcOffsets[1].x - blit.srcOffsets[0].x),
                      uint32_t(blit.srcOffsets[1].y - blit.srcOffsets[0].y), 1};
  const DkImageRect d{uint32_t(blit.dstOffsets[0].x), uint32_t(blit.dstOffsets[0].y), 0,
                      uint32_t(blit.dstOffsets[1].x - blit.dstOffsets[0].x),
                      uint32_t(blit.dstOffsets[1].y - blit.dstOffsets[0].y), 1};
  dkCmdBufBlitImage(cmd, &source_view, &s, &target_view, &d, DkBlitFlag_ModeBlit | filter, 0);
}

// Vulkan: blit_ != nullptr. deko3d: the 2D engine reads and writes only images created with
// DkImageFlags_Usage2DEngine (dk_image.cpp checks it in debug; release would read garbage).
bool CanBlitDk(const ImageNativeDk& source, const ImageNativeDk& target) {
  return sa2::dk::Alive(source.image) && sa2::dk::Alive(target.image) &&
         (sa2::dk::Desc(source.image).flags & DkImageFlags_Usage2DEngine) &&
         (sa2::dk::Desc(target.image).flags & DkImageFlags_Usage2DEngine);
}

// dkCmdBufResolveImage takes no rectangle: the copy must be the whole of both images, of the same host size.
bool WholeResolveDk(const ImageNativeDk& source, const ImageNativeDk& target, const VkImageCopy& host_copy) {
  return CanBlitDk(source, target) && host_copy.srcOffset.x == 0 && host_copy.srcOffset.y == 0 &&
         host_copy.dstOffset.x == 0 && host_copy.dstOffset.y == 0 &&
         HostWidth(source) == HostWidth(target) && HostHeight(source) == HostHeight(target) &&
         host_copy.extent.width == HostWidth(target) && host_copy.extent.height == HostHeight(target);
}

void ResolveDk(DkCmdBuf cmd, sa2::dk::ImageId source, sa2::dk::ImageId target) {
  const DkImageView source_view = sa2::dk::View(source);
  const DkImageView target_view = sa2::dk::View(target);
  dkCmdBufResolveImage(cmd, &source_view, &target_view);
}

// BarrierAfterCopyResolve: what the DMA / 2D engine wrote is next sampled by the draws, rendered into, or copied
// again. The base has no "copy to anything" hazard; CopyToSample is mapped as strongly as CopyToRender until the
// semantics bench (sa2_dk.cpp Barrier), so it serves the three (see "Pedidos à base" in the report).
void AfterCopyDk(DkCmdBuf cmd) { sa2::dk::Barrier(cmd, sa2::dk::Hazard::CopyToSample); }

}  // namespace

// ---- sa2_native_targets.cpp 1752-2612 ----
// sa2_native_targets.cpp:1753

bool TargetsDk::CopyInternal(const RegistersCopy& reg) {
  edram4_operation_failed_ = false;
  // sa2_gpu_resolve_alias: the resolve right before this one is the candidate source of an alias. Any other
  // resolve, clear or failure in between ends it (only a color resolve recorded for real sets it again).
  const LastResolve previous_resolve = last_resolve_;
  last_resolve_.valid = false;
  if (DiagAlias()) {
    int32_t rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    Rectangle(reg, reg.rb_surface_info & 0x3FFF, rx0, ry0, rx1, ry1);
    REXLOG_INFO("[diag alias] frame {} resolve: drawn {} control {:08X} color_info {:08X} depth_info {:08X} surface "
                "{:08X} dest {:08X} pitch {:08X} info {:08X} rect {},{}-{},{} | previous valid {} {:08X}",
                presented_, draws_ ? draws_->Drawn() : 0, reg.rb_copy_control,
                (reg.rb_copy_control & 7) < 4 ? reg.rb_color_info[reg.rb_copy_control & 7] : 0, reg.rb_depth_info,
                reg.rb_surface_info, reg.rb_copy_dest_base, reg.rb_copy_dest_pitch, reg.rb_copy_dest_info, rx0, ry0,
                rx1, ry1, previous_resolve.valid, previous_resolve.address);
  }
  // Preflight SDK clear ranges and backing padding before resolve/clear mutations.
  if (REXCVAR_GET(sa2_native_edram_alias_mode) == 4 &&
      !ValidateSimultaneousClearEDRAM4(reg)) return false;
  // Draws since the previous copy: those of the render target being resolved now (copy inventory).
  const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
  const uint64_t draws_before = drawn - drawn_last_copy_;
  drawn_last_copy_ = drawn;
  if (draws_) {
    draws_->FinishPass();  // copying and clearing are not allowed inside a pass
    // No longer on every copy; see GetResolved and Prepare.
    if (invalidate_each_copy_) {
      draws_->InvalidateTextures();
    }
  }
  if (Record()) {
    MarkGpu(kGpuCopies);  // GPU time of copies and clears (report)
  }
  const uint32_t control = reg.rb_copy_control;
  const uint32_t source = control & 0x7;
  const bool clear_color = (control >> 8) & 0x1;
  const uint32_t command = (control >> 20) & 0x3;
  // sa2_diag_resolves_from (diagnostic): the raw registers of a window of resolves (copy number N .. N+79).
  {
    static const int32_t diag_from = REXCVAR_GET(sa2_diag_resolves_from);
    static uint64_t diag_n = 0;
    const uint64_t n = diag_n++;
    if (diag_from > 0 && n >= uint64_t(diag_from) && n < uint64_t(diag_from) + 80) {
      int32_t rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
      Rectangle(reg, reg.rb_surface_info & 0x3FFF, rx0, ry0, rx1, ry1);
      REXLOG_INFO("[native] resolve #{} rect {},{}-{},{}", n, rx0, ry0, rx1, ry1);
      REXLOG_INFO("[native] resolve #{}: draws before {} | control {:08X} (source {}, clear color {}, clear depth {}, "
                  "command {}) surface {:08X} color_info {:08X} depth_info {:08X} | dest base {:08X} pitch {:08X} "
                  "info {:08X} | window offset {:08X} scissor {:08X}-{:08X} | rect vertices fetch {:08X} {:08X}",
                  n, draws_before, control, source, clear_color ? 1 : 0, (control >> 9) & 1, command,
                  reg.rb_surface_info, source < 4 ? reg.rb_color_info[source] : 0, reg.rb_depth_info,
                  reg.rb_copy_dest_base, reg.rb_copy_dest_pitch, reg.rb_copy_dest_info, reg.pa_sc_window_offset,
                  reg.pa_sc_window_scissor_tl, reg.pa_sc_window_scissor_br, reg.fetch_vertices[0],
                  reg.fetch_vertices[1]);
    }
  }
  const bool do_copy = command == uint32_t(xenos::CopyCommand::kRaw) ||
                      command == uint32_t(xenos::CopyCommand::kConvert);
  const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
  const uint32_t msaa = (reg.rb_surface_info >> 16) & 0x3;
  msaa_edram_actual_ = msaa;
  // Do not infer draw eligibility here. ClearDepth explicitly selects
  // an expanded clear backing so untouched partial-tile borders retain X.
  depth_raster_grid_eligible_ = false;
  if (msaa != uint32_t(xenos::MsaaSamples::k1X) && warned_.insert(1).second) {
    REXLOG_INFO("[native] targets: MSAA target: used with 1 sample");
  }
  if (source >= xenos::kMaxColorRenderTargets) {
    // From depth: the copy goes to a resolved texture with the host depth
    // format, which the draws sample as k_24_8. With a depth source no color
    // is cleared (the SDK's IsClearingColor).
    const bool will_clear = ((control >> 9) & 0x1) && kClearDepthDk;
    if (do_copy) {
      CopyDepth(reg, pitch, will_clear);
    }
    if (will_clear) {
      ClearDepth(reg, pitch);
    }
    return !edram4_operation_failed_;
  }
  const uint32_t info_color = reg.rb_color_info[source];
  const uint32_t color_format = (info_color >> 16) & 0xF;
  if (HostFormatTargetColor(color_format) == VK_FORMAT_UNDEFINED) {
    return Reject(100 + color_format, "render target format not supported yet");
  }
  int32_t x0, y0, x1, y1;
  if (!Rectangle(reg, pitch, x0, y0, x1, y1)) {
    return false;
  }

  // First the objects (creating or recreating can submit work), then recording.
  Image* target_render = GetTarget(info_color & 0xFFF, color_format, pitch);
  if (!target_render) {
    return false;
  }
  // The game has just said which area of this render target matters to it. It is the data RestoreContent
  // uses to stop copying the bottom rows that nobody draws or reads (the scene render target is created
  // 1280x1280 to draw 1280x720).
  NoteAreaUtil(*target_render, y1);
  uint32_t base_resolved = 0;
  Resolved* resolved = nullptr;
  uint32_t format_resolved_target = 0;
  uint32_t dx = 0, dy = 0;
  if (do_copy) {
    const uint32_t info_target = reg.rb_copy_dest_info;
    const uint32_t target_format = (info_target >> 7) & 0x3F;
    format_resolved_target = target_format;
    if ((info_target >> 3) & 0x1) {
      Reject(3, "copy to a 3D texture or array: not supported yet");
    } else if (HostFormatResolved(target_format) == VK_FORMAT_UNDEFINED) {
      // 300 + format, apart from the draws' 200 + render target format.
      Reject(300 + target_format, "copy format not supported yet");
    } else {
      const VkFormat host_format_target = HostFormatResolved(target_format);
      const uint32_t log2_texel = host_format_target == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                          host_format_target == VK_FORMAT_R32G32_SFLOAT
                                      ? 3
                                      : 2;
      const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
      const uint32_t target_height = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
      // 4 bytes per texel: base in multiples of 32 texels (GetResolveInfo).
      const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
      const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
      const uint32_t base =
          reg.rb_copy_dest_base +
          uint32_t(TileDisplacement2D(int32_t(base_x), int32_t(base_y), pitch_target, log2_texel));
      base_resolved = base & 0x1FFFFFFF;
      dx = uint32_t(x0) - base_x;
      dy = uint32_t(y0) - base_y;
      // banjo-nx: the rows really copied, not the destination height: Nuts & Bolts resolves its second predicated
      // tile (rows 384..719) with the base advanced by 384 rows and the full 1280x720 destination size, which
      // overran the 1280x720 texture of the first tile and became a texture of its own (bottom half missing).
      DiagStaleResolved(base_resolved, pitch_target, target_height, target_format, (info_target >> 24) & 0x1,
                        host_format_target);  // sa2_diag_stale_resolved (off by default)
      resolved = SearchResolvedPartial(base_resolved, pitch_target,
                                       std::min(target_height, uint32_t(std::max(y1 - y0, 0))),
                                       target_format, (info_target >> 24) & 0x1,
                                       host_format_target, log2_texel, base_resolved, dx, dy,
                                       std::min(pitch_target, uint32_t(std::max(x1 - x0, 0))), target_height);
      if (!resolved) {
        resolved = GetResolved(base_resolved, pitch_target, target_height, target_format,
                                   (info_target >> 24) & 0x1, host_format_target);
      }
      if (resolved) {
        NoteCopy(base_resolved, resolved->image.width, resolved->image.height, draws_before);
      }
    }
  }

  if (!Record()) {
    return false;
  }
  Prepare(*target_render);
  area_edram_ = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
  // ds2-nx (ds2_resolve_7e3_direct): a UNORM10 view of the 7e3 scene, resolved straight from the 7e3 view.
  if (REXCVAR_GET(sa2_native_edram_alias_mode) == 4 && do_copy && resolved && !clear_color &&
      REXCVAR_GET(ds2_resolve_7e3_direct) && storage_unorm10_ && (color_format == 2 || color_format == 10) &&
      resolved->image.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
      !me::native::ColorResolveExponentBias(command, reg.rb_copy_dest_info) &&
      me::native::IsSingleSample(uint32_t(target_render->sample_count))) {
    if (Image* scene = Owner7e3EDRAM4(*target_render, area_edram_)) {
      const uint32_t width = std::min({uint32_t(x1 - x0), target_render->width - uint32_t(x0),
                                       resolved->image.width > dx ? resolved->image.width - dx : 0u});
      const uint32_t height = std::min({uint32_t(y1 - y0), target_render->height - uint32_t(y0),
                                        resolved->image.height > dy ? resolved->image.height - dy : 0u});
      if (width && height) {
        Prepare(*scene);
        Prepare(resolved->image);  // a new texture is still in VK_IMAGE_LAYOUT_UNDEFINED
        BeforeResolvedWrite(base_resolved, dx, dy, width, height);
        ResolverPreviousFront(base_resolved, dx == 0 && dy == 0 && width == resolved->image.width &&
                                                 height == resolved->image.height);
        if (ResolverWithBias(*scene, resolved->image, x0, y0, dx, dy, width, height, 0, 10)) {
          ++copies_;
          ++ds2_direct_7e3_resolves_;
          ResolvedWritten(base_resolved, uint64_t(width) * height);
          return !edram4_operation_failed_;
        }
      }
    }
  }
  if (REXCVAR_GET(sa2_native_edram_alias_mode) == 4 && do_copy &&
      !(edram4_reason_ = 2, SynchronizeEDRAM4(*target_render, area_edram_))) return false;
  ActivateTargetColor(info_color & 0xFFF, pitch, *target_render);
  if (resolved) {
    Prepare(resolved->image);
    // What the game asks for and what fits in the render target.
    const uint32_t request_width =
        std::min(uint32_t(x1 - x0), target_render->width - uint32_t(x0));
    const uint32_t request_height = std::min(uint32_t(y1 - y0), target_render->height - uint32_t(y0));
    const uint32_t fits_width = resolved->image.width > dx ? resolved->image.width - dx : 0;
    const uint32_t fits_height = resolved->image.height > dy ? resolved->image.height - dy : 0;
    const uint32_t width = std::min(request_width, fits_width);
    const uint32_t height = std::min(request_height, fits_height);
    // With the scene at a higher resolution than the render target (sa2_internal_resolution = 1920x1080
    // and a 1280x720 front buffer), copying 1 to 1 takes only a piece: the image comes out cropped, with part
    // of the scene and the HUD out of frame. When it does not fit, it is shrunk with a linear blit, which is exactly
    // the scaling wanted. If it fits, it is copied as always, bit for bit.
    // It only shrinks when the scene really has to be reduced (by a factor of 1.25 or more) and both images
    // are color: vkCmdBlitImage with a linear filter on a depth target is not valid and brings the process
    // down. Mismatches of a few pixels (320x184 -> 320x180) are still cropped as always.
    const bool multisampled = !me::native::IsSingleSample(uint32_t(target_render->sample_count));  // sa2_msaa
    // ds2-nx (ds2_resolve_clip): Xenos clips a resolve to its destination. Dark Souls II resolves its 256x8 tone-map
    // target (EDRAM 767, PS 2d347f339dd95c5b draws row 0 only) into a 256x1 texture (111BE000); taken for
    // supersampling, the 8 rows were averaged into 1 (7 of them stale EDRAM) and the scene came out saturated yellow.
    const bool shrink = !REXCVAR_GET(ds2_resolve_clip) && CanBlitDk(*target_render, resolved->image) && fits_width && fits_height && !multisampled &&
                         target_render->format == kColorFormat &&
                         resolved->image.format == kColorFormat &&
                         (request_width * 4 >= fits_width * 5 || request_height * 4 >= fits_height * 5);
    if (shrink && uint32_t(x0) < target_render->width && uint32_t(y0) < target_render->height) {
      BeforeResolvedWrite(base_resolved);  // sa2_gpu_resolve_alias
      // sa2_native_lazy_front. If this texture had a deferred copy, it is dropped if this resolve
      // covers it entirely, and recorded first otherwise.
      ResolverPreviousFront(base_resolved, dx == 0 && dy == 0 && fits_width == resolved->image.width &&
                                                 fits_height == resolved->image.height);
      VkImageBlit reduction{};
      reduction.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      reduction.srcOffsets[0] = {HostEdge(*target_render, x0), HostEdge(*target_render, y0), 0};
      reduction.srcOffsets[1] = {HostEdge(*target_render, x0 + int32_t(request_width)),
                                 HostEdge(*target_render, y0 + int32_t(request_height)), 1};
      reduction.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      reduction.dstOffsets[0] = {HostEdge(resolved->image, int32_t(dx)), HostEdge(resolved->image, int32_t(dy)), 0};
      reduction.dstOffsets[1] = {HostEdge(resolved->image, int32_t(dx + fits_width)),
                                 HostEdge(resolved->image, int32_t(dy + fits_height)), 1};
      sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
      BlitDk(commands_work_, target_render->image, resolved->image.image, reduction, DkBlitFlag_FilterLinear);
      AfterCopyDk(commands_work_);
      ++copies_;
      ++reductions_;
      if (reductions_ <= 4) {
        REXLOG_INFO("[resolution] the {}x{} scene is downscaled to {}x{} when resolved (supersampling)",
                    request_width, request_height, fits_width, fits_height);
      }
      // A linear-filter blit reads the large rectangle and writes the small one, so it costs more than a copy
      // of the output size. The pixels read are recorded, since they are what dominates.
      ResolvedWritten(base_resolved, uint64_t(request_width) * request_height);
      ResolvedRead(reg, *resolved, x0, y0, dx, dy, fits_width, fits_height);
    } else if (width && height && uint32_t(x0) < target_render->width &&
               uint32_t(y0) < target_render->height && dx < resolved->image.width &&
               dy < resolved->image.height) {
      const int32_t exp_bias =
          me::native::ColorResolveExponentBias(command, reg.rb_copy_dest_info);
      if (exp_bias && multisampled) {
        return Reject(14, "resolve exponent bias from a multisampled target (sa2_msaa) not supported");
      } else if (exp_bias) {
        // The HDR loop requests -3 (1/8) on each convert resolve. A numeric
        // vkCmdCopyImage / blit ignores this and amplifies repeated feedback.
        BeforeResolvedWrite(base_resolved);  // sa2_gpu_resolve_alias
        ResolverPreviousFront(base_resolved, false);
        if (!ResolverWithBias(*target_render, resolved->image, x0, y0, dx, dy,
                              width, height, exp_bias)) {
          return Reject(14, "resolve exponent bias / format not supported");
        }
        ++copies_;
        ++converted_copies_;
        ResolvedWritten(base_resolved, uint64_t(width) * height);
      } else if (target_render->format != resolved->image.format) {
        // resolve with a format conversion (HDR render target to another texture format),
        // a 1 to 1 blit.
        const uint64_t conversion_key =
            uint64_t(color_format) | (uint64_t(format_resolved_target) << 8) |
            (uint64_t(target_render->format) << 16) |
            (uint64_t(resolved->image.format) << 48);
        if (recorded_conversions_.insert(conversion_key).second) {
          REXLOG_INFO(
              "[native] targets: resolve conversion: guest RT {} host {} base {:03X}/{} -> guest texture {} host {} "
              "{:08X} {}x{} (rect {}x{} at {},{} -> {},{})",
              color_format, uint32_t(target_render->format), info_color & 0xFFF, pitch,
              format_resolved_target, uint32_t(resolved->image.format), base_resolved,
              resolved->image.width, resolved->image.height, width, height, x0, y0, dx, dy);
        }
        if (!CanBlitDk(*target_render, resolved->image) || multisampled) {
          // deko3d: the blit is the 2D engine, usable only on images created with DkImageFlags_Usage2DEngine.
          Reject(4, multisampled ? "copy with format conversion from a multisampled target (sa2_msaa)"
                                 : "[dk] copy with format conversion without the 2D engine (no Usage2DEngine)");
        } else {
          VkImageBlit conversion{};
          conversion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          conversion.srcOffsets[0] = {HostEdge(*target_render, x0), HostEdge(*target_render, y0), 0};
          conversion.srcOffsets[1] = {HostEdge(*target_render, x0 + int32_t(width)),
                                      HostEdge(*target_render, y0 + int32_t(height)), 1};
          conversion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          conversion.dstOffsets[0] = {HostEdge(resolved->image, int32_t(dx)), HostEdge(resolved->image, int32_t(dy)),
                                      0};
          conversion.dstOffsets[1] = {HostEdge(resolved->image, int32_t(dx + width)),
                                      HostEdge(resolved->image, int32_t(dy + height)), 1};
          BeforeResolvedWrite(base_resolved);  // sa2_gpu_resolve_alias
          ResolverPreviousFront(base_resolved, dx == 0 && dy == 0 && width == resolved->image.width &&
                                                     height == resolved->image.height);
          sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
          BlitDk(commands_work_, target_render->image, resolved->image.image, conversion, DkBlitFlag_FilterNearest);
          AfterCopyDk(commands_work_);
          ++copies_;
          ++converted_copies_;
          ResolvedWritten(base_resolved, uint64_t(width) * height);
        }
      } else {
      VkImageCopy copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.srcOffset = {x0, y0, 0};
      copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
      copy.extent = {width, height, 1};
      // sa2_gpu_resolve_alias: the same bytes as the previous resolve's texture, which still holds them.
      if (MakeAlias(previous_resolve, *target_render, *resolved, base_resolved, x0, y0, dx, dy, width, height)) {
        ++copies_;  // a resolve like any other for the reports (the alias report counts how many were not copied)
      } else {
      BeforeResolvedWrite(base_resolved, dx, dy, width, height);  // sa2_gpu_resolve_alias (covers the rectangle)
      // sa2_native_lazy_front. The previous copy to this texture, if still deferred, is dropped (this
      // one covers it entirely) or recorded first; and if this texture is a front buffer that only the Swap
      // reads, it is deferred.
      ResolverPreviousFront(base_resolved,
                              dx == 0 && dy == 0 && width == resolved->image.width && height == resolved->image.height);
      if (multisampled || !PostponeCopyFront(base_resolved, *target_render, *resolved, copy)) {
        // sa2_gpu_resolve_draw: on the 3D engine when it applies.
        if (multisampled) {
          // sa2_msaa: the samples averaged into the single-sample texture (the anti-aliasing).
          const VkImageCopy host_copy = HostCopy(*target_render, resolved->image, copy);
          // deko3d: dkCmdBufResolveImage (2D engine) resolves whole images of the same size only, with no
          // rectangle: a partial MSAA resolve is rejected (vkCmdResolveImage took the rectangle).
          if (!WholeResolveDk(*target_render, resolved->image, host_copy))
            return Reject(19, "[dk] partial MSAA resolve (sa2_msaa) not supported by dkCmdBufResolveImage");
          sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
          ResolveDk(commands_work_, target_render->image, resolved->image.image);
          AfterCopyDk(commands_work_);
          ++msaa_resolves_;
          sa2::render::NoteMsaaUse();
        } else if (resolved->image.fixed_native && target_render->scale_q != resolved->image.scale_q &&
                   CanBlitDk(*target_render, resolved->image)) {
          // ds2-nx (ds2_resolved_small_native): a scaled render target into a game-size texture is stretched
          // (a copy would take only the scaled extent).
          const VkRect2D s = HostRect(*target_render, VkRect2D{{x0, y0}, {width, height}});
          VkImageBlit stretch{};
          stretch.srcSubresource = stretch.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          stretch.srcOffsets[0] = {s.offset.x, s.offset.y, 0};
          stretch.srcOffsets[1] = {s.offset.x + int32_t(std::max(s.extent.width, 1u)),
                                   s.offset.y + int32_t(std::max(s.extent.height, 1u)), 1};
          stretch.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
          stretch.dstOffsets[1] = {int32_t(dx + width), int32_t(dy + height), 1};
          sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
          BlitDk(commands_work_, target_render->image, resolved->image.image, stretch, DkBlitFlag_FilterLinear);
          AfterCopyDk(commands_work_);
        } else if (!ResolveColorFrag(*target_render, resolved->image, x0, y0, dx, dy, width, height)) {
          const VkImageCopy host_copy = HostCopy(*target_render, resolved->image, copy);
          sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
          CopyImageDk(commands_work_, target_render->image, resolved->image.image, 1, &host_copy);
          AfterCopyDk(commands_work_);
        }
        ++copies_;
        ResolvedWritten(base_resolved, uint64_t(width) * height);
        ResolvedRead(reg, *resolved, x0, y0, dx, dy, width, height);
        // sa2_gpu_resolve_alias: the candidate source of the next resolve's alias (not when this command also
        // clears: the render target changes right after the copy).
        if (g_resolve_alias.active && !clear_color && !((control >> 9) & 0x1)) {
          last_resolve_.valid = true;
          last_resolve_.target = target_render->image;
          last_resolve_.x0 = x0;
          last_resolve_.y0 = y0;
          last_resolve_.width = width;
          last_resolve_.height = height;
          last_resolve_.address = base_resolved;
          last_resolve_.image = resolved->image.image;
          last_resolve_.revision = resolved->revision;
          last_resolve_.dx = dx;
          last_resolve_.dy = dy;
          last_resolve_.drawn = draws_ ? draws_->Drawn() : 0;
          last_resolve_.transfers = edram4_transfers_all_;
          last_resolve_.restored = restored_pixels_;
          last_resolve_.edram_version = edram4_version_;
        }
      }
      }
      }
    }
  }
  if (clear_color) {
    const auto clear_rect = me::native::ClipClearRectangle(
        x0, y0, x1, y1, target_render->width, target_render->height);
    const VkRect2D clear_area{{clear_rect.x, clear_rect.y},
                              {clear_rect.width, clear_rect.height}};
    const bool clear_whole = clear_rect.whole(target_render->width, target_render->height);
    if (!clear_rect.width || !clear_rect.height) return Reject(15, "empty color clear rectangle");
    const uint64_t raw_value =
        uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
    if (REXCVAR_GET(sa2_native_edram_alias_mode) == 4) {
      me::native::EdramClearPlan plan;
      VkRect2D sdk_area{};
      me::native::EdramClearRectangles rectangles;
      if (!GetPlanClearedEDRAM4(reg, plan, sdk_area) ||
          !PrepareRangeClearEDRAM4(*target_render, plan.color, sdk_area, rectangles)) return false;
      if (rectangles.count && !draws_)
        return FailureEDRAM4("SDK color clear needs rectangle render passes", *target_render);
      if (plan.color.length) {
        ForgetClear(*target_render);
        const VkClearColorValue color = ClearValueColor(color_format, raw_value);
        for (uint32_t i = 0; i < rectangles.count; ++i) {
          const auto& rectangle = rectangles.rectangles[i];
          const VkRect2D region{{int32_t(rectangle.x), int32_t(rectangle.y)},
                                {rectangle.width, rectangle.height}};
          BarrierClearEDRAM4();
          if (!draws_->ClearColorInPass(commands_work_, *target_render, color, region))
            return FailureEDRAM4("SDK color clear render pass failed", *target_render);
          BarrierClearEDRAM4();
        }
        if (rectangles.count) {
          MarkGpu(kGpuCleared);
          ++cleared_;
        }
        PublishRangeClearEDRAM4(*target_render, plan.color);
      }
    } else {
    /*
     * Two things that were missing here.
     *  1. A WHOLE-image clear leaves the target with a known color, so discarded contents need not
     *     be restored. A partial clear must preserve every pixel outside the requested rectangle
     *     (color did not).
     *  2. If the render target is already cleared to that same color and nothing has been drawn since
     *     then, the vkCmdClearColorImage does not change a single bit and is skipped.
     */
    // A partial clear does not restore discarded pixels outside its rectangle.
    if (clear_whole) target_render->invalid_content = false;
    else if (!RestoreContent(*target_render) &&
             REXCVAR_GET(sa2_native_edram_alias_mode) == 4) return false;
    if (clear_whole && REXCVAR_GET(sa2_native_edram_alias_mode) != 4 &&
        RedundantClear(*target_render, raw_value)) {
      ++cleared_skipped_;
    } else {
      // sa2_native_lazy_front. If this render target is the source of a deferred front buffer, the
      // clear goes to a spare image and the one with the content is kept for the front buffer (no copy).
      if (clear_whole) RotateFrontBeforeClear(*target_render);
      else ForgetClear(*target_render);
      MarkGpu(kGpuCleared);  // clears, separate from copies (report)
      const VkClearColorValue color = ClearValueColor(color_format, raw_value);  // per format
      // deko3d: both clears are the 3D engine's (dkCmdBufClearColor on the bound target, through the draws); the
      // copy barriers of the Vulkan transfer clear become the dependencies of a 3D write (copies before, samples
      // after).
      if (!clear_whole) {
        sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::CopyToRender);
        if (!draws_ || !draws_->ClearColorInPass(commands_work_, *target_render,
                                                     color, clear_area)) {
          return Reject(16, "partial color clear render pass failed");
        }
        sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToSample);
      } else {
        // deko3d: no transfer clear (vkCmdClearColorImage); the whole image is a full-area clear in a pass.
        sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::CopyToRender);
        if (!draws_ || !draws_->ClearColorInPass(commands_work_, *target_render, color,
                                                 VkRect2D{{0, 0}, {target_render->width, target_render->height}})) {
          return Reject(16, "[dk] whole color clear render pass failed");
        }
        sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToSample);
      }
      ++cleared_;
    }
    }
  }
  if (((control >> 9) & 0x1) && kClearDepthDk) {
    ClearDepth(reg, pitch);
  }
  return !edram4_operation_failed_;
}

// sa2_native_targets.cpp:2213

void TargetsDk::ClearDepth(const RegistersCopy& reg, uint32_t pitch) {
  const uint32_t info = reg.rb_depth_info;
  // A partial clear preserves the rest of each published physical tile. It
  // must not import retained samples into a collapsed image before clearing.
  const bool previous_eligibility = depth_raster_grid_eligible_;
  depth_raster_grid_eligible_ = msaa_edram_actual_ == 2;
  Image* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
  depth_raster_grid_eligible_ = previous_eligibility;
  if (!depth || !Record()) {
    return;
  }
  Prepare(*depth);
  int32_t x0, y0, x1, y1;
  if (!Rectangle(reg, pitch, x0, y0, x1, y1)) return;
  x0 *= 1u << depth->raster_grid_x;
  x1 *= 1u << depth->raster_grid_x;
  if (depth->guest_width && depth->guest_height) {
    x0 = int32_t(int64_t(x0) * depth->width / depth->guest_width);
    y0 = int32_t(int64_t(y0) * depth->height / depth->guest_height);
    x1 = int32_t((int64_t(x1) * depth->width + depth->guest_width - 1) /
                  depth->guest_width);
    y1 = int32_t((int64_t(y1) * depth->height + depth->guest_height - 1) /
                  depth->guest_height);
  }
  const auto clear_rect = me::native::ClipClearRectangle(
      x0, y0, x1, y1, depth->width, depth->height);
  if (!clear_rect.width || !clear_rect.height) return;
  const VkRect2D clear_area{{clear_rect.x, clear_rect.y},
                            {clear_rect.width, clear_rect.height}};
  const bool clear_whole = clear_rect.whole(depth->width, depth->height);
  const bool mode4 = REXCVAR_GET(sa2_native_edram_alias_mode) == 4;
  if (mode4) {
    me::native::EdramClearPlan plan;
    VkRect2D sdk_area{};
    me::native::EdramClearRectangles rectangles;
    if (!GetPlanClearedEDRAM4(reg, plan, sdk_area) ||
        !PrepareRangeClearEDRAM4(*depth, plan.depth, sdk_area, rectangles)) return;
    if (rectangles.count && !draws_) {
      FailureEDRAM4("SDK depth clear needs rectangle render passes", *depth);
      return;
    }
    if (!plan.depth.length) return;
    ForgetClear(*depth);
    const VkClearDepthStencilValue value{me::native::NativeDepthClearValue(info, reg.rb_depth_clear, depth->depth_float24_half),
                                         reg.rb_depth_clear & 255u};
    for (uint32_t i = 0; i < rectangles.count; ++i) {
      const auto& rectangle = rectangles.rectangles[i];
      const VkRect2D region{{int32_t(rectangle.x), int32_t(rectangle.y)},
                            {rectangle.width, rectangle.height}};
      BarrierClearEDRAM4();
      depth->edram4_bits_stencil |= uint8_t(value.stencil);
      if (!draws_->ClearDepthInPass(commands_work_, *depth,
                                              value.depth, value.stencil, &region)) {
        FailureEDRAM4("SDK depth clear render pass failed", *depth);
        return;
      }
      BarrierClearEDRAM4();
    }
    if (rectangles.count) {
      MarkGpu(kGpuCleared);
    }
    PublishRangeClearEDRAM4(*depth, plan.depth);
    return;
  }
  if (!clear_whole && !draws_) {
    Reject(17, "partial depth clear requires a render pass");
    return;
  }
  if (clear_whole) depth->invalid_content = false;
  else {
    if (!RestoreContent(*depth) && mode4) return;
    ForgetClear(*depth);
  }
  const VkClearDepthStencilValue val{me::native::NativeDepthClearValue(info, reg.rb_depth_clear, depth->depth_float24_half),
                                       reg.rb_depth_clear & 0xFF};
  // If it is already cleared with this same value and nobody has drawn anything since then, the clear does
  // not change a single bit. Mind the path below: the per-pass clear also resets the ZCULL plane, so that
  // one is never skipped (only the vkCmdClearDepthStencilImage one).
  if (!mode4 && clear_whole && depth->accepts_target_of_copy &&
      RedundantClear(*depth, uint64_t(reg.rb_depth_clear))) {
    ++cleared_skipped_depth_;
    return;
  }
  MarkGpu(kGpuCleared);  // clears, separate from copies (report)
  // ZCULL: see Prepare. Without TRANSFER_DST the clear has to be done by opening a pass.
  if ((!clear_whole || !depth->accepts_target_of_copy) && draws_) {
    // This bool cannot be dropped. If the clear pass cannot be opened, the depth keeps the previous frame's
    // content and the ZCULL hi-Z is not reset either (only a loadOp = CLEAR resets it), so the whole frame
    // culls against stale data. Without TRANSFER_DST there is no alternative path, so at least it is
    // counted and reported.
    depth->edram4_bits_stencil |= uint8_t(val.stencil);
    const bool cleared = draws_->ClearDepthInPass(commands_work_, *depth, val.depth,
                                                            val.stencil, clear_whole ? nullptr : &clear_area);
    if (!cleared) {
      if (mode4) FailureEDRAM4("depth clear render pass failed", *depth);
      if (++cleared_in_failed_pass_ <= 8) {
        REXLOG_WARN("[native] targets: could NOT clear the {}x{} depth by opening a pass; it keeps "
                    "its previous content (failure {})",
                    depth->width, depth->height, cleared_in_failed_pass_);
      }
    }
    if (mode4 && cleared) PublishEDRAM4(*depth, clear_area);
    return;
  }
  depth->edram4_bits_stencil |= uint8_t(val.stencil);
  // deko3d: no transfer clear (vkCmdClearDepthStencilImage); the whole image is cleared on the bound depth
  // target like the pass path above (dkCmdBufClearDepthStencil, which also resets ZCULL). Without the draws
  // there is nothing to bind it with.
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::CopyToRender);
  if (!draws_ || !draws_->ClearDepthInPass(commands_work_, *depth, val.depth, val.stencil)) {
    Reject(17, "[dk] whole depth clear requires the draws' depth target bind");
    return;
  }
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToSample);
  if (mode4) PublishEDRAM4(*depth, clear_area);
}

// sa2_native_targets.cpp:2325

// Depth copy: the rectangle of the depth render target to a resolved texture
// of the same host format, at the base GetResolveInfo computes
// (4 bytes per texel, like k_24_8).
// will_clear: this same game command clears the render target right after resolving it. Only then is it worth
// swapping the images: if the game kept drawing on top, the content would have to be brought back and the
// copy would be paid anyway (measured: one restore per frame on the shadow map).
void TargetsDk::CopyDepth(const RegistersCopy& reg, uint32_t pitch, bool will_clear) {
  const bool mode4 = REXCVAR_GET(sa2_native_edram_alias_mode) == 4;
  const auto failure = [&](const char* reason, const Image* owner = nullptr) {
    if (!mode4) return;
    Image diagnostic;
    diagnostic.edram_depth = true;
    diagnostic.edram_base = uint16_t(reg.rb_depth_info & 0xFFFu);
    diagnostic.edram_format = uint8_t((reg.rb_depth_info >> 16) & 1u);
    FailureEDRAM4(reason, owner ? *owner : diagnostic);
  };
  const uint32_t info_target = reg.rb_copy_dest_info;
  if ((info_target >> 3) & 0x1) {
    Reject(3, "copy to a 3D texture or array: not supported yet");
    failure("depth resolve array/3D destination unsupported");
    return;
  }
  if (!kCopyImageDk || !pitch) {
    failure("depth resolve missing transfer function or pitch");
    return;
  }
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  const bool vertex_contract =
      (reg.fetch_vertices[0] & 3u) == uint32_t(xenos::FetchConstantType::kVertex) &&
      ((reg.fetch_vertices[1] >> 2) & 0xFFFFFFu) == 6u;
  if (!Rectangle(reg, pitch, x0, y0, x1, y1)) {
    if (!vertex_contract) failure("depth resolve unsupported rectangle vertex contract");
    return;  // An empty scissor/rectangle is a successful no-op.
  }
  if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    failure("depth resolve invalid signed rectangle");
    return;
  }
  const uint32_t info = reg.rb_depth_info;
  if (mode4 && REXCVAR_GET(sa2_native_depth_samples_x) && msaa_edram_actual_ == 2) {
    // A direct resolve would collapse the retained samples again, or select
    // the collapsed allocation and hide that loss. No such shortcut in this
    // bounded experiment: sample-select/average resolve needs its own path.
    failure("4x depth resolve unsupported with horizontal sample preservation");
    return;
  }
  Image* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
  if (!depth) {
    failure("depth resolve source allocation/format failure");
    return;
  }
  if (mode4) {
    if (!Record()) {
      failure("depth resolve cannot begin recording", depth);
      return;
    }
    edram4_reason_ = 3;
    if (!SynchronizeEDRAM4(*depth,
        VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}})) {
      failure("depth resolve source synchronization failed", depth);
      return;
    }
  }
  const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
  const uint32_t target_height = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
  if (mode4 && (!pitch_target || !target_height)) {
    failure("depth resolve zero destination dimensions", depth);
    return;
  }
  const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
  const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
  const uint32_t base =
      reg.rb_copy_dest_base +
      uint32_t(TileDisplacement2D(int32_t(base_x), int32_t(base_y), pitch_target, 2));
  uint32_t dx = uint32_t(x0) - base_x;
  uint32_t dy = uint32_t(y0) - base_y;
  const uint32_t texture_format = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
  const VkFormat resolved_host_format = depth->depth_float24_half
      ? VK_FORMAT_R32_SFLOAT : depth->format;
  if (depth->depth_float24_half) {
    // deko3d: no format query; R32_SFLOAT is sampled, load/store and copy capable on the Tegra X1
    // (format_traits.inc), so this always passes.
    if (!kR32FloatFullDk) {
      FailureEDRAM4("guestspace depth resolve requires sampled/storage R32 format", *depth);
      return;
    }
  }
  if (depth->depth_float24_half && (depth->guest_width || depth->guest_height)) {
    FailureEDRAM4("scaled half-range depth resolve unsupported", *depth);
    return;
  }
  // With the scaled shadow map, the resolved texture stays at that same size.
  // The scene picks it by address, without looking at the size, and samples it with normalized coordinates
  // and point sampling, so 1024 read with the UVs of 1600 gives the same texel as 1024 upscaled to 1600
  // with NEAREST: the image does not change. What is saved is the upscaling blit (1.78 ms on the console)
  // and the larger half of the copy (10.24 MB -> 4.2 MB, twice per frame).
  uint32_t resolved_width = pitch_target, resolved_height = target_height;
  if (depth->guest_width && depth->guest_width != depth->width) {
    resolved_width = uint32_t(uint64_t(pitch_target) * depth->width / depth->guest_width);
    resolved_height = uint32_t(uint64_t(target_height) * depth->height / depth->guest_height);
  }
  uint32_t base_resolved = base & 0x1FFFFFFF;
  // banjo-nx: the rows really copied (see the color resolve).
  Resolved* resolved = SearchResolvedPartial(base_resolved, resolved_width,
                                             std::min(resolved_height, uint32_t(std::max(y1 - y0, 0))),
                                             texture_format, false, resolved_host_format, 2,
                                             base_resolved, dx, dy,
                                             std::min(resolved_width, uint32_t(std::max(x1 - x0, 0))),
                                             resolved_height);
  if (!resolved) {
    resolved = GetResolved(base_resolved, resolved_width, resolved_height,
                               texture_format, false, resolved_host_format);
  }
  if (!resolved || !Record()) {
    failure("depth resolve destination allocation/recording failure", depth);
    return;
  }
  BeforeResolvedWrite(base_resolved);  // sa2_gpu_resolve_alias
  resolved->image.resolved_depth_guestspace = depth->depth_float24_half;
  // Depth is part of the per-render-target inventory too, so the shadow map (the most expensive copy in the
  // frame) shows up in it.
  NoteCopy(base_resolved, resolved->image.width, resolved->image.height, 0);
  Prepare(*depth);
  Prepare(resolved->image);
  if (mode4 && (!depth->prepared || !resolved->image.prepared)) {
    failure("depth resolve source/destination initialization failure", depth);
    return;
  }
  NoteAreaUtil(*depth, y1);  // the area the game really resolves
  // With the scaled shadow map, the rectangle the guest sends is in 1600-pixel units even though the image
  // is smaller. Everything below reasons in guest pixels.
  const uint32_t guest_width = depth->guest_width ? depth->guest_width : depth->width;
  const uint32_t guest_height = depth->guest_height ? depth->guest_height : depth->height;
  const bool scaled = guest_width != depth->width || guest_height != depth->height;
  if (mode4 && (uint32_t(x0) >= guest_width || uint32_t(y0) >= guest_height ||
                dx >= resolved->image.width || dy >= resolved->image.height)) {
    failure("depth resolve source/destination offset outside backing", depth);
    return;
  }
  const uint32_t width = std::min({uint32_t(x1 - x0), guest_width - uint32_t(x0),
                                   resolved->image.width - dx});
  const uint32_t height = std::min({uint32_t(y1 - y0), guest_height - uint32_t(y0),
                                  resolved->image.height - dy});
  if (!width || !height || uint32_t(x0) >= guest_width ||
      uint32_t(y0) >= guest_height || dx >= resolved->image.width ||
      dy >= resolved->image.height) {
    return;
  }
  // sa2_native_resolver_valid_content. If the content of this render target went away in an earlier
  // swap and nobody brought it back, it is in another texture: it is brought back before reading it (flickering
  // shadow maps).
  if (depth->invalid_content) {
    if (REXCVAR_GET(sa2_native_resolver_valid_content)) {
      if (!RestoreContent(*depth, true) &&
          REXCVAR_GET(sa2_native_edram_alias_mode) == 4) return;
    } else {
      ++resolver_old_content_;  // previous behavior: the old image is resolved
    }
  }
  if (depth->depth_float24_half) {
    // ds2_skip_repeated_depth_resolve: the same resolve again with nothing written since.
    LastDepthResolve now;
    now.source = depth->image;
    now.target = resolved->image.image;
    now.address = base_resolved;
    now.x0 = x0;
    now.y0 = y0;
    now.dx = dx;
    now.dy = dy;
    now.width = width;
    now.height = height;
    now.sample = (reg.rb_copy_control >> 4) & 7u;
    now.drawn = draws_ ? draws_->Drawn() : 0;
    now.transfers = edram4_transfers_all_;
    now.restored = restored_pixels_;
    now.edram_version = edram4_version_;
    if (REXCVAR_GET(ds2_skip_repeated_depth_resolve) && mode4 && !will_clear) {
      const auto last = last_depth_resolves_.find(base_resolved);
      if (last != last_depth_resolves_.end() && last->second.Same(now) &&
          last->second.revision == resolved->revision) {
        ++ds2_depth_resolves_skipped_;
        return;
      }
    }
    // Eager MODE4 compute resolve: never routed through deferred VkImageCopy/swaps.
    if (!ResolverDepthGuestspaceEDRAM4(*depth, resolved->image,
        uint32_t(x0), uint32_t(y0), dx, dy, width, height,
        (reg.rb_copy_control >> 4) & 7u)) return;
    ++copies_;
    ResolvedWritten(base_resolved, uint64_t(width) * height);
    now.revision = resolved->revision;
    last_depth_resolves_[base_resolved] = now;
    return;
  }
  if (scaled) {
    // Source and destination have the same reduced size, so it is a normal 1 to 1 copy
    // with the rectangle converted to image pixels. No blit, no scaling.
    const auto aX = [&](int32_t v) {
      return int32_t(int64_t(v) * depth->width / guest_width);
    };
    const auto aY = [&](int32_t v) {
      return int32_t(int64_t(v) * depth->height / guest_height);
    };
    const uint32_t img_width = std::min(
        {uint32_t(std::max(1, aX(x0 + int32_t(width)) - aX(x0))),
         depth->width - uint32_t(aX(x0)), resolved->image.width - uint32_t(aX(int32_t(dx)))});
    const uint32_t img_height = std::min(
        {uint32_t(std::max(1, aY(y0 + int32_t(height)) - aY(y0))),
         depth->height - uint32_t(aY(y0)), resolved->image.height - uint32_t(aY(int32_t(dy)))});
    if (!img_width || !img_height) {
      return;
    }
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.srcOffset = {aX(x0), aY(y0), 0};
    copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.dstOffset = {aX(int32_t(dx)), aY(int32_t(dy)), 0};
    copy.extent = {img_width, img_height, 1};
    copy = HostCopy(*depth, resolved->image, copy);  // sa2-nx render resolution
    sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
    CopyImageDk(commands_work_, depth->image, resolved->image.image, 1, &copy);
    AfterCopyDk(commands_work_);
    ++copies_;
    ResolvedWritten(base_resolved, uint64_t(img_width) * img_height);
    return;
  }
  // If the whole render target is resolved to a texture of the same size, the images are swapped and
  // nothing is copied (the shadow map is 1600x1600: 10 MB per copy).
  if (ResolverNoCopy()) {  // and if it is not swapped, why not
    const bool whole = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                        width == depth->width && height == depth->height;
    // The clear is no longer a requirement (see sa2_native_swap_no_clear).
    const bool can = whole && (will_clear || REXCVAR_GET(sa2_native_swap_no_clear));
    if (can) {
      if (SwapWithResolved(*depth, *resolved, base_resolved)) {
        if (!will_clear) {
          ++swaps_no_clear_;
        }
        return;
      }
      ++no_swap_[2];  // the swap itself could not be done
    } else {
      /*
       * The label was backwards ever since sa2_native_swap_no_clear existed. With that setting
       * on, `can` = whole, so everything that lands here is "not resolved whole" and the log counted it as
       * "the command does not clear the render target". It is now split by the real cause: if the resolve
       * is whole, the cause is the clear; otherwise, it is the size.
       */
      ++no_swap_[whole ? 0 : 1];  // 0: the command does not clear the render target; 1: not the whole render target
      if (warnings_no_swap_ < 8) {
        ++warnings_no_swap_;
        REXLOG_INFO("[native] targets: no swap: {}x{} of {}x{} at ({},{})->({},{}), clears {} (base {:03X})",
                    width, height, depth->width, depth->height, x0, y0, dx, dy, will_clear,
                    base & 0xFFF);
      }
    }
  }
  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  copy.srcOffset = {x0, y0, 0};
  copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
  copy.extent = {width, height, 1};
  if (!me::native::IsSingleSample(uint32_t(depth->sample_count))) {
    // sa2_msaa: a multisampled depth target resolves through a fragment pass (sample 0).
    if (!ResolveDepthMs(*depth, resolved->image, copy)) {
      Reject(18, "depth resolve from a multisampled target (sa2_msaa) failed");
      failure("multisampled depth resolve failed", depth);
      return;
    }
    ++copies_;
    ResolvedWritten(base_resolved, uint64_t(width) * height);
    return;
  }
  copy = HostCopy(*depth, resolved->image, copy);  // sa2-nx render resolution
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
  CopyImageDk(commands_work_, depth->image, resolved->image.image, 1, &copy);
  AfterCopyDk(commands_work_);
  ++copies_;
  ResolvedWritten(base_resolved, uint64_t(width) * height);
}

// ---- sa2_native_targets.cpp 4064-6497 ----
// sa2_native_targets.cpp:4064
bool TargetsDk::Reject(uint32_t cause, const char* text) {
  ++rejections_;
  if (warned_.insert(cause).second) {
    REXLOG_WARN("[native] targets: {} (cause {})", text, cause);
  }
  return false;
}

// sa2_native_targets.cpp:4071

// Rectangle covered by the copy, as in the SDK's GetResolveInfo.
bool TargetsDk::Rectangle(const RegistersCopy& reg, uint32_t pitch, int32_t& x0, int32_t& y0,
                int32_t& x1, int32_t& y1) {
  const uint32_t type = reg.fetch_vertices[0] & 0x3;
  const uint32_t address = reg.fetch_vertices[0] >> 2;
  const auto order = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
  const uint32_t size = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
  if (type != uint32_t(xenos::FetchConstantType::kVertex) || size != 3 * 2) {
    return Reject(5, "copy vertices in an unsupported format");
  }
  const uint8_t* vertices = memory_->TranslatePhysical(address * 4);
  const float mid_pixel =
      (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
  int32_t fixed[6];
  for (int i = 0; i < 6; ++i) {
    float value;
    std::memcpy(&value, vertices + i * 4, sizeof(value));
    fixed[i] = Fixed16p8(xenos::GpuSwap(value, order) + mid_pixel);
  }
  x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;

  const int32_t x_displacement = ExtenderSign15(reg.pa_sc_window_offset & 0x7FFF);
  const int32_t y_displacement = ExtenderSign15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
  if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
    x0 += x_displacement;
    y0 += y_displacement;
    x1 += x_displacement;
    y1 += y_displacement;
  }
  // Window scissor (GetScissor without clamping to the pitch).
  int32_t left = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
  int32_t top = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
  int32_t right = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
  int32_t bottom = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
  if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
    left += x_displacement;
    top += y_displacement;
    right += x_displacement;
    bottom += y_displacement;
  }
  left = std::max(left, 0);
  top = std::max(top, 0);
  right = std::max(right, left);
  bottom = std::max(bottom, top);
  x0 = std::clamp(x0, left, right);
  y0 = std::clamp(y0, top, bottom);
  x1 = std::clamp(x1, left, right);
  y1 = std::clamp(y1, top, bottom);
  // D3D9 aligns to 8 (kResolveAlignmentPixels).
  x0 &= ~int32_t(7);
  y0 &= ~int32_t(7);
  x1 = (x1 + 7) & ~int32_t(7);
  y1 = (y1 + 7) & ~int32_t(7);
  const int32_t pitch_aligned = int32_t(pitch & ~uint32_t(7));
  x0 = std::min(x0, pitch_aligned);
  x1 = std::min(x1, pitch_aligned);
  if (x0 >= x1 || y0 >= y1) {
    return Reject(6, "empty copy rectangle");
  }
  return true;
}

// sa2_native_targets.cpp:4145

Image* TargetsDk::GetTarget(uint32_t base, uint32_t format, uint32_t pitch) {
  const uint64_t memo_in = (uint64_t(msaa_edram_actual_) << 40) | (uint64_t(base & 0xFFFFF) << 20) |
                           (uint64_t(format & 0xF) << 16) | (pitch & 0xFFFF);
  TargetMemo& memo = target_memo_[(base ^ (pitch >> 4)) & 3];
  if (g_target_memo.active) {
    if (memo.in == memo_in) {
      g_target_memo.UseSampled();
      return memo.image;
    }
  }
  Image* const found = GetTargetLookup(base, format, pitch);
  if (found && g_target_memo.active && base <= 0xFFFFF && format <= 0xF && pitch <= 0xFFFF)
    memo = {memo_in, found};
  return found;
}

// sa2_native_targets.cpp:4161

Image* TargetsDk::GetTargetLookup(uint32_t base, uint32_t format, uint32_t pitch) {
  if (!pitch) {
    Reject(7, "render target with pitch 0");
    return nullptr;
  }
  const VkFormat host_format = HostFormatTargetColor(format);  // per format
  if (host_format == VK_FORMAT_UNDEFINED) {
    Reject(100 + format, "render target format not supported yet");
    return nullptr;
  }
  // EDRAM is typeless storage. UE3 deliberately alternates, at the same tile base, the normal and
  // "AS" variants used for blending (3 <-> 12 and 2 <-> 10). They have the same host representation;
  // keeping separate VkImages loses everything drawn through the other guest view. Key by the guest
  // storage class as well as the host format: UNORM10, 7e3 and true FP16 may all use host RGBA16F,
  // but they must be reinterpreted through physical words rather than sharing numeric texels.
  const uint8_t class_edram = ClassEdramFORMAT(format);
  const bool edram_64bpp = xenos::IsColorRenderTargetFormat64bpp(
      xenos::ColorRenderTargetFormat(format));
  const uint64_t key = (uint64_t(msaa_edram_actual_) << 44) |
                         (uint64_t(base) << 32) | (uint64_t(class_edram) << 24) |
                         (uint64_t(uint32_t(host_format) & 0xFF) << 16) | pitch;
  auto it = targets_.find(key);
  if (it != targets_.end()) {
    return &it->second;
  }
  /*
   * The height comes from the pitch, so the scene's color target measures 1280x1280 to draw 1280x720
   * and can never be swapped with its resolved texture. With a pitch of 1280 or less the game never
   * draws below 720 (measured viewport and scissor), so 720 rows would be enough and the sizes would
   * match. Above 1280 (the internal 1080p mode) the larger height is needed.
   */
  /*
   * Creating the color target 720 rows high (instead of deriving the height from the pitch), so that it
   * matches the resolved texture and the images can be swapped instead of copied, was tried: it breaks
   * the scene (the screen fills with a yellow smear). Reverted.
   */
  const uint32_t height = std::min(kMaxTargetHeight, std::max<uint32_t>(720, (pitch + 15) & ~15u));
  Image image;
  image.edram_base = uint16_t(base);
  image.edram_format = class_edram;
  image.edram_64bpp = edram_64bpp;
  image.edram_msaa_x = msaa_edram_actual_ >= 2;
  image.edram_msaa_y = msaa_edram_actual_ >= 1;
  // sa2_msaa: the game's single-sample RGBA8 targets get host samples (no storage use: the compute conversions
  // decline multisampled views and the fragment ones take their place).
  const bool multisampled = msaa_samples_ > 1 && msaa_edram_actual_ == 0 && host_format == kColorFormat;
  // deko3d flags for COLOR_ATTACHMENT | SAMPLED | TRANSFER_SRC | TRANSFER_DST | STORAGE: sampling and the copy
  // engine need no flag; the 2D engine (blits, MSAA resolve) does.
  if (!Create(image, pitch, height,
             DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine |
                 (multisampled ? 0u : uint32_t(DkImageFlags_UsageLoadStore)),
             host_format,
             multisampled ? VkSampleCountFlagBits(msaa_samples_) : VK_SAMPLE_COUNT_1_BIT)) {
    Reject(8, "could not create a render target");
    return nullptr;
  }
  REXLOG_INFO("[native] targets: render target base {:03X}, format {}, {}x{}", base, format, pitch, height);
  return &targets_.emplace(key, image).first->second;
}

// sa2_native_targets.cpp:4267
bool TargetsDk::ContainsRect(const VkRect2D& a, const VkRect2D& b) {  // a contains b
  return b.extent.width && b.extent.height && b.offset.x >= a.offset.x && b.offset.y >= a.offset.y &&
         int64_t(b.offset.x) + b.extent.width <= int64_t(a.offset.x) + a.extent.width &&
         int64_t(b.offset.y) + b.extent.height <= int64_t(a.offset.y) + a.extent.height;
}

// sa2_native_targets.cpp:4288
std::string TargetsDk::DescribeDrawEDRAM4() const {
  if (!edram4_submission_ || !edram4_submission_->registers) return "-";
  const auto& p = *edram4_submission_;
  const uint32_t* r = p.registers;
  return fmt::format(
      "slot{} VS n{} PS n{} prim {} count {} dc {:08X} stencil {:08X} mode {} mask {:08X} blend0 {:08X} "
      "scissor {},{}+{}x{} surf {:08X} depth {:08X} color0 {:08X}",
      edram4_slot_, p.vs ? int(p.vs->number) : -1, p.ps ? int(p.ps->number) : -1,
      r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR] & 63, r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16,
      r[gr::XE_GPU_REG_RB_DEPTHCONTROL], r[gr::XE_GPU_REG_RB_STENCILREFMASK],
      r[gr::XE_GPU_REG_RB_MODECONTROL] & 7, r[gr::XE_GPU_REG_RB_COLOR_MASK],
      r[gr::XE_GPU_REG_RB_BLENDCONTROL0], edram4_draw_area_.offset.x, edram4_draw_area_.offset.y,
      edram4_draw_area_.extent.width, edram4_draw_area_.extent.height,
      r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_DEPTH_INFO], r[kDiagRegsColorInfo[0]]);
}

// sa2_native_targets.cpp:4303
// The draw being synchronized, for the transfer trace (from edram4_submission_ / edram4_slot_ / edram4_draw_area_).
std::string TargetsDk::TraceDrawEDRAM4() const {
  if (!edram4_submission_ || !edram4_submission_->registers) return "-";
  const auto& p = *edram4_submission_;
  const uint32_t* r = p.registers;
  return fmt::format(
      "slot{} VS n{} PS n{} prim {} count {} dc {:08X} clip {:08X} vte {:08X} mask {:08X} blend0 {:08X} "
      "scissor {},{}+{}x{} overwrite {:X} est {}",
      edram4_slot_, p.vs ? int(p.vs->number) : -1, p.ps ? int(p.ps->number) : -1,
      r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR] & 63, r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16,
      r[gr::XE_GPU_REG_RB_DEPTHCONTROL], r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], r[gr::XE_GPU_REG_PA_CL_VTE_CNTL],
      r[gr::XE_GPU_REG_RB_COLOR_MASK], r[gr::XE_GPU_REG_RB_BLENDCONTROL0], edram4_draw_area_.offset.x,
      edram4_draw_area_.offset.y, edram4_draw_area_.extent.width, edram4_draw_area_.extent.height,
      p.edram_overwrite_slots, p.edram_used_height_estimate ? int(*p.edram_used_height_estimate) : -1);
}

// sa2_native_targets.cpp:4320
bool TargetsDk::SynchronizedEDRAM4(const Image& view, uint32_t physical, uint32_t version) const {
  if (!version || !REXCVAR_GET(sa2_native_edram4_cache_sync)) return false;
  const auto it = edram4_views_.find(&view);
  return it != edram4_views_.end() && it->second[physical & 2047u] == version;
}

// sa2_native_targets.cpp:4325
void TargetsDk::NoteSynchronizedEDRAM4(const Image& view, uint32_t physical, uint32_t version) {
  auto& versions = edram4_views_[&view];
  if (versions.empty()) versions.assign(2048, 0);
  versions[physical & 2047u] = version;
}

// sa2_native_targets.cpp:4330
void TargetsDk::ForgetSynchronizedEDRAM4(const Image& view) {
  edram4_views_.erase(&view);
  edram4_alias_1x_.erase(&view);
  edram4_alias_1x_color_.erase(&view);
  for (auto it = edram4_alias_1x_color_.begin(); it != edram4_alias_1x_color_.end();)
    it = it->second == &view ? edram4_alias_1x_color_.erase(it) : std::next(it);
  for (auto it = edram4_consumer_.begin(); it != edram4_consumer_.end();)
    it = it->second.view == &view ? edram4_consumer_.erase(it) : std::next(it);
  edram4_stencil_source_.erase(&view);
  for (auto& [v, sources] : edram4_stencil_source_)
    for (auto& f : sources) if (f.image == &view) f = {};
  for (auto it = edram4_alias_1x_.begin(); it != edram4_alias_1x_.end();)
    it = it->second == &view ? edram4_alias_1x_.erase(it) : std::next(it);
}

// sa2_native_targets.cpp:4420
bool TargetsDk::StencilDeferredEDRAM4(const Image& view, uint32_t start, uint32_t count) const {
  const auto it = edram4_stencil_source_.find(&view);
  if (it == edram4_stencil_source_.end()) return false;
  for (uint32_t i = 0; i < count; ++i) if (it->second[(start + i) & 2047u].image) return true;
  return false;
}

// sa2_native_targets.cpp:4426
void TargetsDk::StencilRealEDRAM4(const Image& view, uint32_t start, uint32_t count) {
  const auto it = edram4_stencil_source_.find(&view);
  if (it == edram4_stencil_source_.end()) return;
  for (uint32_t i = 0; i < count; ++i) it->second[(start + i) & 2047u] = {};
}

// sa2_native_targets.cpp:4447

bool TargetsDk::FailureEDRAM4(const char* reason, const Image& target, const Image* source) {
  edram4_operation_failed_ = true;
  const auto class_value = [](const Image& image) {
    return uint32_t(image.edram_format) | (uint32_t(image.edram_depth) << 8) |
           (uint32_t(image.edram_64bpp) << 9) | (uint32_t(image.edram_msaa_x) << 10) |
           (uint32_t(image.edram_msaa_y) << 11);
  };
  const std::string key = std::string(reason) + "/" + std::to_string(class_value(target)) + "/" +
                          std::to_string(source ? class_value(*source) : UINT32_MAX);
  auto found = edram4_rejections_per_class_.find(key);
  if (found == edram4_rejections_per_class_.end()) {
    if (edram4_rejections_per_class_.size() >= 128) {
      if (!edram4_log_cap_) {
        edram4_log_cap_ = true;
        REXLOG_WARN("[native] EDRAM mode4 reject log capped at 128 distinct contracts; total failures still counted");
      }
      return false;
    }
    found = edram4_rejections_per_class_.emplace(key, 0).first;
  }
  if (++found->second <= 3) {
    REXLOG_WARN("[native] EDRAM mode4 REJECT {}: dest {:03X}/{} depth {} source {:03X}/{} depth {} "
                "(diagnostic transfers, NOT complete emulation)",
                reason, target.edram_base, target.edram_format, target.edram_depth,
                source ? source->edram_base : 0u, source ? source->edram_format : 0u,
                source ? source->edram_depth : false);
  }
  return false;
}

// sa2_native_targets.cpp:4477

bool TargetsDk::TileUsedEDRAM4(const Image& image, uint32_t tile, const VkRect2D& area,
                     bool* covered_whole) const {
  const uint32_t pitch_tiles = PitchTilesEDRAM(image);
  if (!pitch_tiles || !area.extent.width || !area.extent.height) return false;
  const uint32_t tile_width = 80u >> (uint32_t(image.edram_64bpp) + image.edram_msaa_x);
  const uint32_t tile_height = 16u >> image.edram_msaa_y;
  const uint32_t x = (tile % pitch_tiles) * tile_width;
  const uint32_t y = (tile / pitch_tiles) * tile_height;
  const int64_t x0 = std::max<int64_t>(0, area.offset.x);
  const int64_t y0 = std::max<int64_t>(0, area.offset.y);
  const int64_t x1 = int64_t(area.offset.x) + area.extent.width;
  const int64_t y1 = int64_t(area.offset.y) + area.extent.height;
  if (covered_whole)
    *covered_whole = x >= x0 && y >= y0 && x + tile_width <= x1 && y + tile_height <= y1 &&
                        x + tile_width <= image.width && y + tile_height <= image.height;
  return x < image.width && y < image.height && x < x1 && y < y1 &&
         int64_t(x + tile_width) > x0 && int64_t(y + tile_height) > y0;
}

// sa2_native_targets.cpp:4501
bool TargetsDk::RangeTilesEDRAM4(const Image& image, const VkRect2D& area, RangeTiles4& r) {
  r.pitch = PitchTilesEDRAM(image);
  if (!r.pitch || !area.extent.width || !area.extent.height) return false;
  const uint32_t tile_width = 80u >> (uint32_t(image.edram_64bpp) + image.edram_msaa_x);
  const uint32_t tile_height = 16u >> image.edram_msaa_y;
  const int64_t x0 = std::max<int64_t>(0, area.offset.x), y0 = std::max<int64_t>(0, area.offset.y);
  const int64_t x1 = std::min<int64_t>(int64_t(area.offset.x) + area.extent.width, image.width);
  const int64_t y1 = std::min<int64_t>(int64_t(area.offset.y) + area.extent.height, image.height);
  if (x1 <= x0 || y1 <= y0) return false;
  r.tx0 = uint32_t(x0 / tile_width);
  r.tx1 = std::min<uint32_t>(r.pitch, uint32_t((x1 + tile_width - 1) / tile_width));
  r.ty0 = uint32_t(y0 / tile_height);
  r.ty1 = uint32_t((y1 + tile_height - 1) / tile_height);
  return r.tx1 > r.tx0;
}

// sa2_native_targets.cpp:4516

bool TargetsDk::ValidateSimultaneousClearEDRAM4(const RegistersCopy& reg) {
  me::native::EdramClearPlan plan;
  VkRect2D area{};
  if (!GetPlanClearedEDRAM4(reg, plan, area)) return false;
  if (!plan.depth.length && !plan.color.length) return true;
  const uint32_t control = reg.rb_copy_control;
  const uint32_t source = control & 7u;
  const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
  const uint32_t msaa = (reg.rb_surface_info >> 16) & 3u;
  Image color{}, depth{};
  const uint32_t color_info = source < xenos::kMaxColorRenderTargets ? reg.rb_color_info[source] : 0;
  color.edram_base = color_info & 0xFFF;
  color.edram_format = (color_info >> 16) & 15u;
  color.edram_64bpp = xenos::IsColorRenderTargetFormat64bpp(
      xenos::ColorRenderTargetFormat(color.edram_format));
  depth.edram_base = reg.rb_depth_info & 0xFFF;
  depth.edram_format = (reg.rb_depth_info >> 16) & 1u;
  depth.edram_depth = true;
  // Match the actual unscaled native allocations, not an assumed 720px frame.
  color.width = depth.width = pitch;
  color.height = depth.height = std::min(kMaxTargetHeight,
      std::max<uint32_t>(720, (pitch + 15) & ~15u));
  color.edram_msaa_x = depth.edram_msaa_x = msaa >= 2;
  color.edram_msaa_y = depth.edram_msaa_y = msaa >= 1;
  const me::native::EdramPixelRectangle cutout{uint32_t(area.offset.x), uint32_t(area.offset.y),
      area.extent.width, area.extent.height};
  if (!me::native::PlanEdramClearRectangles(plan.color, PitchTilesEDRAM(color), msaa,
          color.edram_64bpp, color.width, color.height, cutout).valid ||
      !me::native::PlanEdramClearRectangles(plan.depth, PitchTilesEDRAM(depth), msaa,
          false, depth.width, depth.height, cutout).valid)
    return FailureEDRAM4("SDK clear span lacks complete physical backing", color, &depth);
  return true;
}

// sa2_native_targets.cpp:4550

bool TargetsDk::GetPlanClearedEDRAM4(const RegistersCopy& reg, me::native::EdramClearPlan& plan,
                               VkRect2D& area) {
  const uint32_t control = reg.rb_copy_control, source = control & 7u;
  const bool color = source < xenos::kMaxColorRenderTargets && (control & (1u << 8));
  const bool depth = (control & (1u << 9)) && kClearDepthDk;
  if (!color && !depth) { plan.valid = true; return true; }
  const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
  const uint32_t msaa = (reg.rb_surface_info >> 16) & 3u;
  Image diagnostic{};
  if (!pitch || pitch > 8192 || msaa > 2)
    return FailureEDRAM4("invalid clear physical layout", diagnostic);
  int32_t x0, y0, x1, y1;
  if (!Rectangle(reg, pitch, x0, y0, x1, y1))
    return FailureEDRAM4("clear rectangle unavailable", diagnostic);
  // Rectangle returns the original-view rectangle, including window offsets
  // and 8px resolve alignment. Unlike ResolveInfo's offset base, this code
  // never rebases the EDRAM source, so no base-offset rows must be added again.
  const uint32_t height = std::min(kMaxTargetHeight,
      std::max<uint32_t>(720, (pitch + 15) & ~15u));
  const auto rectangle = me::native::ClipClearRectangle(x0, y0, x1, y1, pitch, height);
  area = {{rectangle.x, rectangle.y}, {rectangle.width, rectangle.height}};
  me::native::EdramClearRequest request;
  request.depth_base = reg.rb_depth_info & 0xFFF;
  request.color_base = color ? reg.rb_color_info[source] & 0xFFF : 0;
  request.pitch_tiles_at_32bpp = me::native::EdramLayout(msaa, false).pitch_tiles(pitch);
  request.msaa = msaa;
  request.clear_depth = depth;
  request.clear_color = color;
  request.color_wide = color && xenos::IsColorRenderTargetFormat64bpp(
      xenos::ColorRenderTargetFormat((reg.rb_color_info[source] >> 16) & 15));
  request.x_pixels = uint32_t(rectangle.x);
  request.y_pixels = uint32_t(rectangle.y);
  request.width_pixels = rectangle.width;
  request.height_pixels = rectangle.height;
  plan = me::native::PlanEdramClearRanges(request);
  if (!plan.valid) return FailureEDRAM4("invalid SDK clear range", diagnostic);
  return true;
}

// sa2_native_targets.cpp:4589

bool TargetsDk::PrepareRangeClearEDRAM4(Image& image, me::native::EdramRelativeSpan span,
    const VkRect2D& cutout, me::native::EdramClearRectangles& rectangles) {
  if (image.guest_width || image.guest_height)
    return FailureEDRAM4("scaled clear has no physical backing", image);
  const VkRect2D host_cutout = RasterArea(image, cutout);
  rectangles = me::native::PlanEdramClearRectangles(span, PitchTilesEDRAM(image),
      image.edram_msaa_x ? 2u : image.edram_msaa_y, image.edram_64bpp, image.width, image.height,
      {uint32_t(host_cutout.offset.x), uint32_t(host_cutout.offset.y), host_cutout.extent.width, host_cutout.extent.height});
  if (!rectangles.valid) return FailureEDRAM4("clear span has missing tile padding", image);
  if (!span.length) return true;
  const VkRect2D backing{{0, 0}, {image.width, image.height}};
  // Import every contiguous SDK tile, including row gaps and partial-tile
  // borders. Fully overwritten tiles alone can skip their former contents.
  edram4_reason_ = 1;
  return SynchronizeEDRAM4(image, backing, span.end(), true, span.start, &host_cutout);
}

// sa2_native_targets.cpp:4606

void TargetsDk::PublishRangeClearEDRAM4(Image& image, me::native::EdramRelativeSpan span) {
  for (uint32_t tile = span.start; tile < span.end(); ++tile) {
    const uint32_t physical = (image.edram_base + tile) & 2047u;
    if (physical == TraceTileEDRAM4() && TraceEventEDRAM4(physical)) {
      const auto old = owners_tiles_edram4_[physical];
      REXLOG_INFO("[native] EDRAM TILE TRACE clear-publish frame={} draw={} physical={} "
                  "before=[{}] after=[{}] range={}:{}",
                  trace_frame_, draws_ ? draws_->Drawn() : 0,
                  physical, TraceImageEDRAM4(old.image, old.tile_local),
                  TraceImageEDRAM4(&image, tile), span.start, span.end());
    }
    owners_tiles_edram4_[(image.edram_base + tile) & 2047] = {&image, uint16_t(tile), ++edram4_version_};
  }
  ++edram4_epoch_;
  // An EDRAM clear writes the whole 32-bit word: the stencil is real (known) again.
  if (image.edram_depth) StencilRealEDRAM4(image, span.start, span.end() - span.start);
}

// sa2_native_targets.cpp:4624

void TargetsDk::BarrierClearEDRAM4() {
  // A rectangle loadOp=CLEAR writes attachment stages, not TRANSFER. Include
  // both sides' reads as clear destinations may have been sampled earlier.
  // deko3d: the clear is a 3D-engine write between fragment work that samples the image (an import or conversion
  // pass before it, the next draw after it). DMA copies and compute batches around it carry their own barriers
  // (CopyTilesEDRAM4, CloseComputeBatchEDRAM4), so the dependency left is fragment <-> fragment.
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToSample);
}

// sa2_native_targets.cpp:4644

uint32_t TargetsDk::TraceTileEDRAM4Slow() const {
  static const uint32_t selected = [] {
    static const std::string from_cvar = [] {
      std::string v = REXCVAR_GET(sa2_diag_trace_tile);
      std::erase(v, '"');
      REXLOG_INFO("[native] EDRAM tile trace cvar: '{}'", v);
      return v;
    }();
    const char* value = from_cvar.c_str();
    if (!value || !*value) return UINT32_MAX;
    uint32_t tile = 0;
    for (const char* p = value; *p; ++p) {
      if (*p < '0' || *p > '9' || tile > 2047u / 10u) return UINT32_MAX;
      tile = tile * 10u + uint32_t(*p - '0');
      if (tile >= 2048) return UINT32_MAX;
    }
    return tile;
  }();
  if (selected == UINT32_MAX) {
    trace_tile_none_ = true;
    if (trace_tile_fast_) return UINT32_MAX;
  }
  // Tile chosen by sa2_diag_trace_tile: traced from sa2_diag_trace_from_s after start.
  if (selected == UINT32_MAX || REXCVAR_GET(sa2_diag_trace_tile).empty()) return UINT32_MAX;
  static const auto start = std::chrono::steady_clock::now();
  if (std::chrono::steady_clock::now() - start <
      std::chrono::seconds(REXCVAR_GET(sa2_diag_trace_from_s))) return UINT32_MAX;
  static const bool warned = [&] {
    REXLOG_INFO("[native] EDRAM tile trace active: physical tile {}", selected);
    return true;
  }();
  (void)warned;
  return selected;  // TraceEventEDRAM4 caps the events (1024)
}

// sa2_native_targets.cpp:4679

bool TargetsDk::TraceEventEDRAM4(uint32_t tile) const {
  if (tile == UINT32_MAX) return false;
  // Diagnostic-only, ring-thread state: at most 1024 events per Swap interval.
  static uint64_t frame = UINT64_MAX;
  static uint32_t events = 0;
  if (frame != trace_frame_) {
    frame = trace_frame_;
    events = 0;
  }
  if (events++ < 1024) return true;
  if (events == 1025)
    REXLOG_WARN("[native] EDRAM TILE TRACE truncated frame={} physical={} cap=1024",
                frame, tile);
  return false;
}

// sa2_native_targets.cpp:4695

std::string TargetsDk::TraceImageEDRAM4(const Image* image, uint32_t local) const {
  if (!image) return "none";
  return fmt::format("token={:X} vkimage={:X} base={:03X} guestfmt={} hostfmt={} depth={} pitchtiles={} "
                     "extent={}x{} msaaXY={},{} gridX={} half={} wide={} local={}",
                     reinterpret_cast<uintptr_t>(image), (uint64_t)image->image, image->edram_base,
                     uint32_t(image->edram_format), uint32_t(image->format),
                     image->edram_depth, PitchTilesEDRAM(*image),
                     image->width, image->height, image->edram_msaa_x, image->edram_msaa_y,
                     image->raster_grid_x, image->depth_float24_half, image->edram_64bpp, local);
}

// sa2_native_targets.cpp:4706

bool TargetsDk::DrawTouchesTraceTileEDRAM4() const {
  const uint32_t physical = TraceTileEDRAM4();
  if (physical == UINT32_MAX) return false;
  for (uint32_t slot = 0; slot < 5; ++slot) {
    const Image* image = edram4_draw_images_[slot];
    if (!image) continue;
    const uint32_t local = (physical - image->edram_base) & 2047u;
    if (local < edram4_draw_plan_.length_tiles[slot] &&
        TileUsedEDRAM4(*image, local, RasterArea(*image, edram4_draw_area_))) return true;
  }
  return false;
}

// sa2_native_targets.cpp:4719

// keep_version: the view only received the owner's current contents (a load on bind, no write).
// The tile keeps its version, and the previous owner is recorded as still holding it, so binding the
// previous owner again does not import the same unchanged tiles back (Mass Effect: depth exported to a
// k_16_16 color view for reading, then a full 9-pass import back every frame).
void TargetsDk::PublishEDRAM4(Image& image, const VkRect2D& area, uint32_t limit,
                    bool keep_version) {
  const uint32_t rows = (image.height + (16u >> image.edram_msaa_y) - 1) /
                         (16u >> image.edram_msaa_y);
  const uint32_t tiles = std::min({PitchTilesEDRAM(image) * rows, limit, 2048u});
  const uint32_t trace_physical = TraceTileEDRAM4();
  RangeTiles4 range;
  ++edram4_pub_calls_;
  // Fast path: all tiles of the area already ours and nobody cached their versions: nothing changes.
  if (REXCVAR_GET(sa2_native_edram4_fast) && !keep_version &&
      image.edram4_epoch == edram4_epoch_ && !image.edram4_exported &&
      ContainsRect(image.edram4_own, area)) {
    ++edram4_pub_fast_;
    return;
  }
  if (!RangeTilesEDRAM4(image, area, range)) return;
  bool change_owner = false;
  edram4_pub_visited_ += uint64_t(range.tx1 - range.tx0) * (range.ty1 - range.ty0);
  for (uint32_t ty = range.ty0; ty < range.ty1 && ty * range.pitch < tiles; ++ty)
  for (uint32_t tile = ty * range.pitch + range.tx0; tile < std::min(ty * range.pitch + range.tx1, tiles); ++tile) {
    {
      const uint32_t physical = (image.edram_base + tile) & 2047u;
      const auto old = owners_tiles_edram4_[physical];
      if (physical == trace_physical &&
          (old.image != &image || old.tile_local != tile) && TraceEventEDRAM4(physical)) {
        REXLOG_INFO("[native] EDRAM TILE TRACE publish frame={} draw={} physical={} "
                    "before=[{}] after=[{}] area={},{}+{}x{} limit={}",
                    trace_frame_, draws_ ? draws_->Drawn() : 0,
                    physical, TraceImageEDRAM4(old.image, old.tile_local),
                    TraceImageEDRAM4(&image, tile), area.offset.x, area.offset.y,
                    area.extent.width, area.extent.height, limit);
      }
      if (old.image != &image || old.tile_local != tile) {
        change_owner = true;
        // Lazy stencil: a depth draw that does not write stencil leaves the EDRAM stencil bits as they were,
        // so this view's stencil for the tile is wherever the previous owner's was.
        if (image.edram_depth && REXCVAR_GET(sa2_native_edram4_stencil_lazy)) {
          if (edram4_published_no_stencil_ && old.image &&
              (old.image->edram_depth ||
               (!old.image->edram_64bpp && (old.image->edram_format == 0 || old.image->edram_format == 2 ||
                                             old.image->edram_format == 3)))) {
            const auto previous = edram4_stencil_source_.find(old.image);
            const SourceStencil f = previous != edram4_stencil_source_.end() && previous->second[old.tile_local & 2047u].image
                ? previous->second[old.tile_local & 2047u] : SourceStencil{old.image, uint16_t(old.tile_local & 2047u)};
            auto& sources = StencilSourcesWrite(&image);
            if (sources.empty()) sources.resize(2048);
            sources[tile & 2047u] = f.image == &image && f.tile == (tile & 2047u) ? SourceStencil{} : f;
            ++edram4_stencil_inherited_;
          } else {
            StencilRealEDRAM4(image, tile, 1);
          }
        }
      }
      if (keep_version && old.version && REXCVAR_GET(sa2_native_edram4_cache_sync)) {
        if (old.image && old.image != &image) {
          NoteSynchronizedEDRAM4(*old.image, physical, old.version);
          image.edram4_exported = true;  // the old owner holds this version of our tile
        }
        NoteSynchronizedEDRAM4(image, physical, old.version);
        owners_tiles_edram4_[physical] = {&image, uint16_t(tile), old.version};
      } else {
        owners_tiles_edram4_[physical] = {&image, uint16_t(tile), ++edram4_version_};
      }
    }
  }
  if (change_owner) ++edram4_epoch_;
  // Every tile of the area is ours now (when the tile limit did not cut it).
  if (range.ty1 * range.pitch <= tiles) {
    const VkRect2D own{{std::max(0, area.offset.x), std::max(0, area.offset.y)},
                          {uint32_t(std::min<int64_t>(int64_t(area.offset.x) + area.extent.width, image.width) -
                                    std::max(0, area.offset.x)),
                           uint32_t(std::min<int64_t>(int64_t(area.offset.y) + area.extent.height, image.height) -
                                    std::max(0, area.offset.y))}};
    const bool previous_covers = image.edram4_epoch != edram4_epoch_ || ContainsRect(own, image.edram4_own);
    if (!keep_version && previous_covers) image.edram4_exported = false;  // all our cached tiles re-versioned
    if (image.edram4_epoch != edram4_epoch_ || ContainsRect(own, image.edram4_own)) {
      image.edram4_own = own;
      image.edram4_epoch = edram4_epoch_;
    }
  }
}

// sa2_native_targets.cpp:4805

// A fully proven constant stencil clear must not make its collapsed depth
// copy authoritative. Clear just S8 in each existing canonical depth owner,
// preserving every D32 bit and its physical ownership. This deliberately
// does NOT handle conditional/partial stencil draws or color-word owners.
bool TargetsDk::PublishStencilClearEDRAM4(Image& drawn,
    const me::native::StencilClearOwnershipProof& proof, uint32_t limit) {
  if (!proof.eligible || !drawn.edram_depth || drawn.guest_width || drawn.guest_height ||
      proof.length_tiles != limit || !limit || limit > 2048 ||
      PitchTilesEDRAM(drawn) != proof.columns_tiles)
    return FailureEDRAM4("canonical stencil clear invalid physical proof", drawn);
  struct Region { Image* owner; uint32_t local, first, count; VkRect2D area; };
  std::vector<Region> regions;
  // Validate ALL old owners and complete tile backing before recording any
  // canonical write. New/unowned tiles may be published from the drawn view.
  for (uint32_t tile = 0; tile < limit;) {
    const auto owner = owners_tiles_edram4_[(drawn.edram_base + tile) & 2047u];
    if (!owner.image || owner.image == &drawn) { ++tile; continue; }
    Image& canonical = *owner.image;
    if (!canonical.edram_depth || canonical.edram_64bpp ||
        canonical.guest_width || canonical.guest_height || canonical.image == drawn.image ||
        !canonical.prepared)
      return FailureEDRAM4("canonical stencil clear needs an independent depth owner", drawn, &canonical);
    const uint32_t pitch = PitchTilesEDRAM(canonical);
    const uint32_t width = 80u >> canonical.edram_msaa_x;
    const uint32_t height = 16u >> canonical.edram_msaa_y;
    if (!pitch || canonical.width % width)
      return FailureEDRAM4("canonical stencil clear lacks physical padding", drawn, &canonical);
    const uint32_t x = (owner.tile_local % pitch) * width;
    const uint32_t y = (owner.tile_local / pitch) * height;
    if (x >= canonical.width || y >= canonical.height ||
        width > canonical.width - x || height > canonical.height - y)
      return FailureEDRAM4("canonical stencil clear exceeds depth backing", drawn, &canonical);
    uint32_t count = 1;
    while (tile + count < limit && (owner.tile_local % pitch) + count < pitch) {
      const auto next = owners_tiles_edram4_[(drawn.edram_base + tile + count) & 2047u];
      if (next.image != &canonical || next.tile_local != owner.tile_local + count) break;
      ++count;
    }
    if (uint64_t(width) * count > canonical.width - x)
      return FailureEDRAM4("canonical stencil clear row exceeds depth backing", drawn, &canonical);
    regions.push_back({&canonical, owner.tile_local, tile, count,
                      {{int32_t(x), int32_t(y)}, {width * count, height}}});
    tile += count;
  }
  if (!regions.empty()) {
    if (!draws_) return FailureEDRAM4("canonical stencil clear needs attachment commands", drawn);
    draws_->FinishPass();
    if (!Record()) return false;
    for (const auto& region : regions) {
      Image& canonical = *region.owner;
      Prepare(canonical);
      if (!canonical.prepared || !RestoreContent(canonical)) return false;
      ForgetClear(canonical);
      BarrierClearEDRAM4();
      canonical.edram4_bits_stencil |= uint8_t(proof.stencil_reference);
      StencilRealEDRAM4(canonical, region.local, region.count);
      if (!draws_->ClearStencilInPass(commands_work_, canonical,
                                         proof.stencil_reference, region.area))
        return FailureEDRAM4("canonical stencil-only attachment clear failed", drawn, &canonical);
      BarrierClearEDRAM4();
      const uint32_t physical = TraceTileEDRAM4();
      const uint32_t local = (physical - drawn.edram_base) & 2047u;
      if (physical != UINT32_MAX && local >= region.first &&
          local - region.first < region.count && TraceEventEDRAM4(physical))
        REXLOG_INFO("[native] EDRAM TILE TRACE stencil-only-preserve frame={} draw={} physical={} "
                    "drawn=[{}] canonical=[{}] stencil={} area={},{}+{}x{}",
                    trace_frame_, draws_->Drawn(), physical,
                    TraceImageEDRAM4(&drawn, local),
                    TraceImageEDRAM4(&canonical, region.local + local - region.first),
                    proof.stencil_reference, region.area.offset.x, region.area.offset.y,
                    region.area.extent.width, region.area.extent.height);
    }
  }
  StencilRealEDRAM4(drawn, 0, limit);
  for (uint32_t tile = 0; tile < limit; ++tile) {
    auto& owner = owners_tiles_edram4_[(drawn.edram_base + tile) & 2047u];
    if (!owner.image) { owner = {&drawn, uint16_t(tile)}; ++edram4_epoch_; }
    owner.version = ++edram4_version_;  // the owner's stencil changed
  }
  if (++edram4_stencil_preserves_ <= 16)
    REXLOG_INFO("[native] canonical stencil-only clear preserved depth: draw={} base={:03X} "
                "tiles={} canonical row regions={} reference={}",
                draws_ ? draws_->Drawn() : 0, drawn.edram_base, limit,
                regions.size(), proof.stencil_reference);
  return true;
}

// sa2_native_targets.cpp:4892

bool TargetsDk::SynchronizeEDRAM4(Image& target, const VkRect2D& area, uint32_t limit,
                       bool clear_overwrite, uint32_t start,
                       const VkRect2D* clear_cutout) {
  if (target.guest_width || target.guest_height)
    return FailureEDRAM4("scaled depth has no bit-preserving physical view", target);
  if (!SamplesSupported(target) && !IsDepthNative2xEDRAM4(target))  // sa2_msaa: the host multisampled targets
    return FailureEDRAM4("unsupported hardware multisample physical destination", target);
  // Never restore a swapped image AFTER physical transfers: it would overwrite imported tiles.
  Prepare(target);
  if (!target.prepared) return FailureEDRAM4("destination was not prepared", target);
  if (!RestoreContent(target)) return false;
  const uint32_t rows = (target.height + (16u >> target.edram_msaa_y) - 1) /
                         (16u >> target.edram_msaa_y);
  const uint32_t tiles = std::min({PitchTilesEDRAM(target) * rows, limit, 2048u});
  const uint32_t trace_physical = TraceTileEDRAM4();
  const uint32_t trace_local = (trace_physical - target.edram_base) & 2047u;
  if (trace_physical != UINT32_MAX && trace_local >= start && trace_local < tiles &&
      owners_tiles_edram4_[trace_physical].image != &target &&
      TileUsedEDRAM4(target, trace_local, area) && TraceEventEDRAM4(trace_physical)) {
    const auto owner = owners_tiles_edram4_[trace_physical];
    bool entire = false;
    TileUsedEDRAM4(target, trace_local, clear_cutout ? *clear_cutout : area, &entire);
    if (owner.image != &target)
      REXLOG_INFO("[native] EDRAM TILE TRACE sync-bind frame={} draw={} physical={} "
                "target=[{}] owner=[{}] same={} clear_overwrite={} entire={} "
                "area={},{}+{}x{} range={}:{}",
                trace_frame_, draws_ ? draws_->Drawn() : 0,
                trace_physical, TraceImageEDRAM4(&target, trace_local),
                TraceImageEDRAM4(owner.image, owner.tile_local), owner.image == &target,
                clear_overwrite, entire, area.offset.x, area.offset.y,
                area.extent.width, area.extent.height, start, tiles);
  }
  if (REXCVAR_GET(sa2_native_edram4_fast) && target.edram4_epoch == edram4_epoch_ &&
      ContainsRect(target.edram4_own, area)) {
    ++edram4_sync_fast_;
    return true;  // every tile of the area is ours: nothing to transfer (the slow loop would skip them all)
  }
  RangeTiles4 range;
  const bool has_range = RangeTilesEDRAM4(target, area, range);
  ++edram4_sync_calls_;
  if (has_range) edram4_sync_visited_ += uint64_t(range.tx1 - range.tx0) * (range.ty1 - range.ty0);
  const bool cache_sync = REXCVAR_GET(sa2_native_edram4_cache_sync);
  const std::vector<uint32_t>* versions_target = nullptr;
  const auto synchronized = [&](uint32_t physical, uint32_t version) {
    if (!version || !cache_sync) return false;
    if (!versions_target) {
      const auto it = edram4_views_.find(&target);
      if (it == edram4_views_.end()) return false;
      versions_target = &it->second;  // node-based map: stays valid while entries are added
    }
    return (*versions_target)[physical & 2047u] == version;
  };
  // Phase 1 collects the runs (tile-row pieces) to transfer; phase 2 records them.
  std::vector<SpanEDRAM4> spans;
  // Full-width areas are one contiguous span (a transfer run may cross rows, as before).
  const bool complete_width = has_range && range.tx0 == 0 && range.tx1 == range.pitch;
  const uint32_t ty_end = complete_width ? range.ty0 + 1 : range.ty1;
  for (uint32_t ty = has_range ? range.ty0 : 0; has_range && ty < ty_end && ty * range.pitch < tiles; ++ty) {
  const uint32_t row_end = std::min(complete_width ? range.ty1 * range.pitch : ty * range.pitch + range.tx1,
                                     tiles);
  for (uint32_t tile = std::max(ty * range.pitch + range.tx0, start); tile < row_end;) {
    const auto owner_state = owners_tiles_edram4_[(target.edram_base + tile) & 2047u];
    if (!owner_state.image || owner_state.image == &target) { ++tile; continue; }
    bool whole = false;
    if (!TileUsedEDRAM4(target, tile, area, &whole)) { ++tile; continue; }
    if (clear_cutout) {
      whole = false;
      TileUsedEDRAM4(target, tile, *clear_cutout, &whole);
      for (size_t c = 0; !whole && clear_overwrite && c < edram4_cuts_extra_.size(); ++c)
        TileUsedEDRAM4(target, tile, edram4_cuts_extra_[c], &whole);
    }
    if (clear_overwrite && whole) {
      ++tile;
      continue;
    }
    if (synchronized(target.edram_base + tile, owner_state.version)) {
      ++edram4_sync_skipped_;
      ++tile;
      continue;
    }
    Image& source = *owner_state.image;
    const bool native_msaa_import = IsDepthNative2xEDRAM4(source) &&
        (IsDepthNative2xEDRAM4(target) || IsDepthNative1xEDRAM4(target));
    // sa2_msaa: host multisampled targets go on to the transfers, which take a single-sample source into them
    // (fragment passes) and decline the rest (skipped like any unsupported transfer).
    if ((!SamplesSupported(source) || !SamplesSupported(target)) && !native_msaa_import)
      return FailureEDRAM4("mixed hardware samples / color multisample physical alias unsupported",
                         target, &source);
    if (source.guest_width || source.guest_height)
      return FailureEDRAM4("scaled source has no physical transfer", target, &source);
    const uint32_t source_tile_width = 80u >>
        (uint32_t(source.edram_64bpp) + source.edram_msaa_x);
    // The current native image is only pitch pixels wide, not tile-rounded like the SDK.
    // Missing padding samples cannot silently become zero in a cross-view transfer.
    if (source.width % source_tile_width)
      return FailureEDRAM4("source lacks tile-rounded physical padding", target, &source);
    Prepare(source);
    if (!source.prepared) return FailureEDRAM4("source was not prepared", target, &source);
    // Restored in phase 2, right before its own run (as before the split): a restore forgets lazy-stencil
    // records pointing at this source, which an earlier run may still fetch.
    uint32_t count = 1;
    while (tile + count < row_end) {
      bool whole_next = false;
      const auto next = owners_tiles_edram4_[(target.edram_base + tile + count) & 2047u];
      const bool used = TileUsedEDRAM4(target, tile + count, area, &whole_next);
      if (clear_cutout) {
        whole_next = false;
        TileUsedEDRAM4(target, tile + count, *clear_cutout, &whole_next);
        for (size_t c = 0; !whole_next && clear_overwrite && c < edram4_cuts_extra_.size(); ++c)
          TileUsedEDRAM4(target, tile + count, edram4_cuts_extra_[c], &whole_next);
      }
      if (!used ||
          (clear_overwrite && whole_next) || next.image != &source ||
          next.tile_local != owner_state.tile_local + count ||
          synchronized(target.edram_base + tile + count, next.version)) break;
      ++count;
    }
    const uint32_t source_tile_height = 16u >> source.edram_msaa_y;
    if ((uint32_t(owner_state.tile_local) + count - 1) / PitchTilesEDRAM(source) *
            source_tile_height + source_tile_height > source.height)
      return FailureEDRAM4("source tile range exceeds available physical rows", target, &source);
    if (draws_) draws_->FinishPass();
    ForgetClear(target);
    if (trace_physical != UINT32_MAX && trace_local >= tile &&
        trace_local - tile < count && TraceEventEDRAM4(trace_physical)) {
      const uint32_t source_local = owner_state.tile_local + trace_local - tile;
      const uint32_t dest_width = 80u >> (uint32_t(target.edram_64bpp) + target.edram_msaa_x);
      const uint32_t dest_height = 16u >> target.edram_msaa_y;
      REXLOG_INFO("[native] EDRAM TILE TRACE import-attempt frame={} draw={} physical={} "
                  "source=[{}] target=[{}] groupedSource={} groupedDest={} count={} "
                  "sourceTileOrigin={},{} targetTileOrigin={},{} sourceTileExtent={}x{} "
                  "targetTileExtent={}x{} physicalSampleToHostShiftSrc={},{} dst={},{}",
                  trace_frame_, draws_ ? draws_->Drawn() : 0,
                  trace_physical, TraceImageEDRAM4(&source, source_local),
                  TraceImageEDRAM4(&target, trace_local), owner_state.tile_local, tile, count,
                  (source_local % PitchTilesEDRAM(source)) * source_tile_width,
                  (source_local / PitchTilesEDRAM(source)) * source_tile_height,
                  (trace_local % PitchTilesEDRAM(target)) * dest_width,
                  (trace_local / PitchTilesEDRAM(target)) * dest_height,
                  source_tile_width, source_tile_height, dest_width, dest_height,
                  source.edram_msaa_x, source.edram_msaa_y,
                  target.edram_msaa_x, target.edram_msaa_y);
    }
    {
      uint32_t class_value = 4;
      if (target.edram_depth && source.edram_depth)
        class_value = (source.edram_msaa_x != target.edram_msaa_x ||
                 source.raster_grid_x != target.raster_grid_x) ? 0 : 1;
      else if (target.edram_depth) class_value = 2;
      else if (source.edram_depth) class_value = 3;
      const uint32_t reason = std::min<uint32_t>(edram4_reason_, 3);
      ++edram4_transfer_ops_[reason][class_value];
      ++edram4_transfers_all_;  // sa2_gpu_resolve_alias
      edram4_transfer_tiles_[reason][class_value] += count;
      if (count + 8 >= tiles) ++edram4_transfer_full_[class_value];
      if (edram4_reason_ == 0 && count >= 64 && edram4_traces_ < 60) {
        ++edram4_traces_;
        REXLOG_INFO("[native] EDRAM mode4 transfer trace {}: {} tiles {:03X} {}x{} my{} -> {}x{} my{} by {}",
                    edram4_traces_, count, target.edram_base, source.width, source.height, source.edram_msaa_y,
                    target.width, target.height, target.edram_msaa_y,
                    g_lazy_trace_text.active ? TraceDrawEDRAM4() : edram4_draw_desc_);
      }
      const auto describe = [](const Image& im) {
        return fmt::format("{}{:03X}/f{}:{}x{}:mx{}my{}g{}{}", im.edram_depth ? "D" : "C", im.edram_base,
                           im.edram_format, im.width, im.height, im.edram_msaa_x, im.edram_msaa_y,
                           im.raster_grid_x, im.depth_float24_half ? "h" : "");
      };
      // sa2-nx: the per-pair / per-draw breakdown formats two to four strings and looks them up in string maps
      // for every transfer run (hundreds per frame on the ring thread): diagnostics only.
      static const bool pair_report = REXCVAR_GET(sa2_native_edram4_pair_report);
      if (pair_report) {
      auto& par = edram4_transfer_pairs_[describe(source) + "->" + describe(target)];
      if (!par.ops) par.example = edram4_reason_ == 0 ? DescribeDrawEDRAM4() : "-";
      if (edram4_reason_ == 0 && edram4_submission_ && edram4_submission_->registers) {
        const auto& q = *edram4_submission_;
        const bool ow = q.edram_overwrite_rect && (q.edram_overwrite_slots & (1u << edram4_slot_));
        auto& d = par.draws[fmt::format("VS{}/PS{}/p{}/m{}/dc{:X}/ow{}", q.vs ? int(q.vs->number) : -1,
                                          q.ps ? int(q.ps->number) : -1,
                                          q.registers[gr::XE_GPU_REG_VGT_DRAW_INITIATOR] & 63,
                                          q.registers[gr::XE_GPU_REG_RB_MODECONTROL] & 7,
                                          q.registers[gr::XE_GPU_REG_RB_DEPTHCONTROL],
                                          ow ? fmt::format("{},{}-{},{}", (*q.edram_overwrite_rect)[0],
                                                           (*q.edram_overwrite_rect)[1],
                                                           (*q.edram_overwrite_rect)[2],
                                                           (*q.edram_overwrite_rect)[3])
                                             : std::string("-"))];
        ++d.first;
        d.second += count;
      }
      ++par.ops;
      par.tiles += count;
      }
    }
    spans.push_back({&source, owner_state.tile_local, tile, count, native_msaa_import});
    {
      const RedirectedStamp& stamp = edram4_stamps_[(target.edram_base + tile) & 2047u];
      if (stamp.site && stamp.version == owner_state.version) {
        Consumer& c = edram4_consumer_[stamp.site];
        c = c.view == &target ? Consumer{&target, std::min(c.confidence + 1, 8u)} : Consumer{&target, 1};
      }
    }
    tile += count;
  }
  }
  // Phase 2: the runs, in order. Batched, consecutive runs of one kind share one barrier pair / one pass.
  if (spans.empty()) {
    if (!target.edram_depth && !clear_overwrite) PublishEDRAM4(target, area, limit, true);
    return true;
  }
  const bool batch = REXCVAR_GET(sa2_native_edram4_batch) && spans.size() > 1 && !edram4_batch_.active &&
      slots_[slot_].conversions_edram + 2 * spans.size() + 16 < kConversionsEDRAMPerSlot;
  if (batch) {
    edram4_batch_ = {};
    edram4_batch_.active = true;
    edram4_batch_spans_ += spans.size();
    if (target.edram_depth)
      for (const auto& t : spans) {
        VkRect2D r;
        if (!RectImportEDRAM4(target, t.tile, t.count, r)) continue;
        if (!edram4_batch_.has_area) {
          edram4_batch_.area_import = r;
          edram4_batch_.has_area = true;
          continue;
        }
        VkRect2D& u = edram4_batch_.area_import;
        const int32_t x0 = std::min(u.offset.x, r.offset.x), y0 = std::min(u.offset.y, r.offset.y);
        const int32_t x1 = std::max<int32_t>(u.offset.x + u.extent.width, r.offset.x + r.extent.width);
        const int32_t y1 = std::max<int32_t>(u.offset.y + u.extent.height, r.offset.y + r.extent.height);
        u = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
      }
  }
  const auto finish_batch = [&](bool ok) {
    if (!batch) return ok;
    const bool closed = CloseBatchEDRAM4();
    edram4_batch_.active = false;
    // A deferred stencil copy that failed leaves tiles already recorded as synced: forget them all.
    if (!closed) edram4_views_.erase(&target);
    return ok && closed;
  };
  for (const SpanEDRAM4& t : spans) {
    Image& source = *t.source;
    const uint32_t tile = t.tile, count = t.count;
    const bool native_msaa_import = t.native_msaa_import;
    struct { uint32_t tile_local; } owner_state{t.tile_source};
    if (source.invalid_content) {
      if (batch && !CloseBatchEDRAM4()) return finish_batch(false);
      if (!RestoreContent(source)) return finish_batch(false);
    }
    if (target.edram_depth && source.edram_depth && source.edram_msaa_x && source.edram_msaa_y &&
        !target.edram_msaa_x && !target.edram_msaa_y && source.edram_base == target.edram_base &&
        PitchTilesEDRAM(source) == PitchTilesEDRAM(target) && source.edram_format == target.edram_format &&
        source.depth_float24_half == target.depth_float24_half)
      edram4_alias_1x_[&source] = &target;
    // ds2-nx (ds2_clear_color0_alias): the same for a color view pulling a 4x view's tiles of the same format
    // (RGBA8, or 7e3 in RGBA16F).
    if (!target.edram_depth && !source.edram_depth && source.edram_msaa_x && source.edram_msaa_y &&
        !target.edram_msaa_x && !target.edram_msaa_y && source.edram_base == target.edram_base &&
        PitchTilesEDRAM(source) == PitchTilesEDRAM(target) && source.edram_format == target.edram_format &&
        !source.edram_64bpp && !target.edram_64bpp && source.format == target.format &&
        ((source.edram_format == 0 && source.format == VK_FORMAT_R8G8B8A8_UNORM) ||
         (source.edram_format == 3 && (source.format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                        source.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32))))
      edram4_alias_1x_color_[&source] = &target;
    bool success = false;
    if (target.edram_depth) {
      if (native_msaa_import || !source.edram_depth) {
        success = ImportColorDepthEDRAM4(source, target, owner_state.tile_local, tile, count);
      } else if (source.depth_float24_half != target.depth_float24_half ||
          source.edram_format != target.edram_format ||
          source.edram_msaa_x != target.edram_msaa_x ||
          source.edram_msaa_y != target.edram_msaa_y || !target.accepts_target_of_copy)
        success = ImportColorDepthEDRAM4(source, target, owner_state.tile_local, tile, count);
      else {
        if (batch && !CloseBatchEDRAM4()) return finish_batch(false);
        success = CopyTilesEDRAM4(source, target, owner_state.tile_local, tile, count);
        target.edram4_bits_stencil |= source.edram4_bits_stencil;
        // A plain copy carries the stencil plane as it is: carry its lazy-stencil record with it.
        if (success && REXCVAR_GET(sa2_native_edram4_stencil_lazy)) {
          if (StencilDeferredEDRAM4(source, owner_state.tile_local, count)) {
            if (target.edram4_usa_stencil) ++edram4_stencil_inexact_;
            auto& sources = StencilSourcesWrite(&target);
            if (sources.empty()) sources.resize(2048);
            const auto& de = edram4_stencil_source_[&source];
            for (uint32_t i = 0; i < count; ++i)
              sources[(tile + i) & 2047u] = de[(owner_state.tile_local + i) & 2047u];
          } else {
            StencilRealEDRAM4(target, tile, count);
          }
        }
      }
    } else if (source.edram_depth) {
      if (StencilDeferredEDRAM4(source, owner_state.tile_local, count)) {
        ++edram4_stencil_exported_;  // the export packs the stencil byte: bring the real one first
        // The late fetch imports into the source view: outside this batch.
        if (batch && !CloseBatchEDRAM4()) return finish_batch(false);
        edram4_batch_.active = false;
        const bool fetched = FetchStencilDeferredEDRAM4(source, owner_state.tile_local, count);
        edram4_batch_.active = batch;
        if (!fetched) return finish_batch(false);
      }
      success = ConvertDepthEDRAM4(source, target, owner_state.tile_local, tile, count);
    } else if (source.format == target.format && source.edram_format == target.edram_format &&
               source.edram_64bpp == target.edram_64bpp &&
               source.edram_msaa_x == target.edram_msaa_x && source.edram_msaa_y == target.edram_msaa_y) {
      if (batch && !CloseBatchEDRAM4()) return finish_batch(false);
      success = CopyTilesEDRAM4(source, target, owner_state.tile_local, tile, count);
    } else {
      success = ConvertAliasEDRAM(source, target, owner_state.tile_local, tile, count);
    }
    if (!success && REXCVAR_GET(banjo_native_edram4_skip_unsupported)) {
      // banjo-nx: a conversion the renderer does not have (2x MSAA <-> 1x color alias, depth -> color) used to
      // drop the whole draw: Nuts & Bolts' 2x MSAA scene target (base 768) overlaps the 1x one (base 720), so
      // ~1800 scene draws a frame were lost. Each scene pass clears those tiles first, so the draw goes on with
      // the target's own content there; the tiles count as synchronized so the transfer is not retried.
      if (banjo_transfers_skipped_++ < 8)
        REXLOG_INFO("[native] EDRAM4 transfer skipped (unsupported): dest base {} msaa {}x{} depth {} <- source "
                    "base {} msaa {}x{} depth {}, {} tiles", target.edram_base, target.edram_msaa_x,
                    target.edram_msaa_y, target.edram_depth, source.edram_base, source.edram_msaa_x,
                    source.edram_msaa_y, source.edram_depth, count);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t physical = (target.edram_base + tile + i) & 2047u;
        NoteSynchronizedEDRAM4(target, physical, owners_tiles_edram4_[physical].version);
      }
      continue;
    }
    if (!success) return finish_batch(FailureEDRAM4("physical transfer unsupported or failed", target, &source));
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t physical = (target.edram_base + tile + i) & 2047u;
      NoteSynchronizedEDRAM4(target, physical, owners_tiles_edram4_[physical].version);
      if (owners_tiles_edram4_[physical].image) owners_tiles_edram4_[physical].image->edram4_exported = true;
    }
    edram4_sync_copied_ += count;
  }
  if (!finish_batch(true)) return false;
  // Loading into a color view is now authoritative even when a later draw is skipped. Depth remains
  // owned by its old view until an actual writing draw/clear succeeds (no claims on read-only binds).
  if (!target.edram_depth && !clear_overwrite) PublishEDRAM4(target, area, limit, true);
  return true;
}

// sa2_native_targets.cpp:5232

// Destination rectangle of a depth import of tiles [target_start, +count).
bool TargetsDk::RectImportEDRAM4(const Image& target, uint32_t target_start, uint32_t count, VkRect2D& rect) const {
  const uint32_t destination_pitch = PitchTilesEDRAM(target);
  if (!destination_pitch || !count) return false;
  const uint32_t destination_tile_width = 80u >> target.edram_msaa_x;
  const uint32_t destination_tile_height = 16u >> target.edram_msaa_y;
  const uint32_t first_row = target_start / destination_pitch;
  const uint32_t last_row = (target_start + count - 1) / destination_pitch;
  const uint32_t sx = first_row == last_row ? (target_start % destination_pitch) * destination_tile_width : 0;
  const uint32_t sy = first_row * destination_tile_height;
  const uint32_t right = first_row == last_row
      ? std::min(target.width, ((target_start + count - 1) % destination_pitch + 1) * destination_tile_width)
      : target.width;
  const uint32_t bottom = std::min(target.height, (last_row + 1) * destination_tile_height);
  if (sx >= target.width || sy >= target.height || right <= sx || bottom <= sy) return false;
  rect = {{int32_t(sx), int32_t(sy)}, {right - sx, bottom - sy}};
  return true;
}

// sa2_native_targets.cpp:5251

// Batched compute transfers: the "after" barrier of the last one (a superset of both kinds' masks).
void TargetsDk::CloseComputeBatchEDRAM4() {
  if (!edram4_batch_.compute_pending) return;
  edram4_batch_.compute_pending = false;
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::ComputeToAny);
}

// sa2_native_targets.cpp:5262

// Batched depth imports: end the shared pass, then the copy-engine stencil copies of its runs.
bool TargetsDk::CloseImportBatchEDRAM4() {
  if (edram4_batch_.import_open) {
    edram4_batch_.import_open = false;
    // deko3d: there is no render pass to end. The import's target and shaders stay bound, so the draws must
    // bind theirs again (the same notification as after vkCmdEndRenderPass).
    if (draws_) draws_->NotifyGraphicsExternalState();
    // deko3d: the import wrote depth through the 3D engine; ZCULL and the depth caches must see it before the
    // draws test against it (what the Vulkan render pass end and its barrier gave).
    sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::DepthWrite);
  }
  bool ok = true;
  std::vector<CopyStencilPending> copies;
  copies.swap(edram4_batch_.copies_stencil);
  for (const auto& c : copies)
    if (!CopyStencilEDRAM4(*c.source, *c.target, c.source_start, c.target_start, c.count, c.scissor))
      ok = FailureEDRAM4("copy-engine stencil import failed", *c.target, c.source);
  return ok;
}

// sa2_native_targets.cpp:5283

bool TargetsDk::CopyTilesEDRAM4(Image& source, Image& target, uint32_t source_start,
                       uint32_t target_start, uint32_t count) {
  // The native2x depth utility is routed separately. No multisample image
  // may silently enter the legacy physical-copy mapping.
  // sa2_msaa: vkCmdCopyImage between two images of the same sample count copies every sample.
  if (source.sample_count != target.sample_count || !SamplesSupported(source)) return false;
  if (!kCopyImageDk || source.scale_q != target.scale_q) return false;
  const uint32_t tile_width = 80u >> (uint32_t(target.edram_64bpp) + target.edram_msaa_x);
  const uint32_t tile_height = 16u >> target.edram_msaa_y;
  std::vector<VkImageCopy> regions;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t so = source_start + i, sd = target_start + i;
    const uint32_t sx = (so % PitchTilesEDRAM(source)) * tile_width;
    const uint32_t sy = (so / PitchTilesEDRAM(source)) * tile_height;
    const uint32_t dx = (sd % PitchTilesEDRAM(target)) * tile_width;
    const uint32_t dy = (sd / PitchTilesEDRAM(target)) * tile_height;
    if (sx >= source.width || sy >= source.height || dx >= target.width || dy >= target.height)
      return false;
    if (source.width - sx < std::min(tile_width, target.width - dx) ||
        source.height - sy < std::min(tile_height, target.height - dy)) return false;
    const VkExtent3D extent{std::min({tile_width, source.width - sx, target.width - dx}),
                            std::min({tile_height, source.height - sy, target.height - dy}), 1};
    const auto add = [&](VkImageAspectFlags aspect) {
      VkImageCopy r{};
      r.srcSubresource = r.dstSubresource = {aspect, 0, 0, 1};
      r.srcOffset = {int32_t(sx), int32_t(sy), 0};
      r.dstOffset = {int32_t(dx), int32_t(dy), 0};
      r.extent = extent;
      regions.push_back(r);
    };
    // deko3d: dkCmdBufCopyImage copies whole texels, depth and stencil together (no aspects), so a depth tile is
    // one region, not a depth + a stencil one.
    if (target.edram_depth) add(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    else add(VK_IMAGE_ASPECT_COLOR_BIT);
  }
  // deko3d: each region is one copy-engine launch. With one region per tile (no aspect pair any more) the runs of
  // adjacent tiles merge into rectangles, as ConvertEDRAM4's copies already do (same pixels copied).
  MergeRegionsCopy(regions);
  HostCopies(source, target, regions.data(), uint32_t(regions.size()));  // sa2-nx render resolution
  sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::RenderToCopy);
  CopyImageDk(commands_work_, source.image, target.image, uint32_t(regions.size()), regions.data());
  AfterCopyDk(commands_work_);
  // deko3d: depth written by the copy engine leaves the target's ZCULL state stale.
  if (target.edram_depth) sa2::dk::Barrier(commands_work_, sa2::dk::Hazard::DepthWrite);
  return true;
}

// sa2_native_targets.cpp:5336

// Mass Effect (Eden Prime point-light shadows): the game clears an 880x880 1x depth shadow map with a
// D3D Clear rectangle through a 440-pixel 4x MSAA view of the same EDRAM (a quarter of the pixels to
// shade). Drawn literally, every clear transfers the tiles 1x -> 4x and the next shadow draw 4x -> 1x
// (9 passes each on Switch without stencil export). A proven, tile-aligned, constant-depth rectangle
// that touches only depth (ALWAYS, stencil off) is instead applied as a depth-only clear in each tile's
// current owner; ownership stays with the owner (new version), the drawn view is not touched.
// ReXGlue xenos Float32To20e4 without rounding (me_edram_depth_a_*.comp A20e4).
uint32_t TargetsDk::WordFloat24EDRAM4(float z) {
  if (!(z > 0.0f)) return 0u;
  uint32_t bits = std::bit_cast<uint32_t>(z);
  if (bits >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (bits < 0x38800000u) bits = ((bits & 0x7FFFFFu) | 0x800000u) >> std::min(113u - (bits >> 23), 24u);
  else bits += 0xC8000000u;
  return (bits >> 3) & 0xFFFFFFu;
}

// sa2_native_targets.cpp:5352
// me_edram_depth_to_16f.comp From7e3.
float TargetsDk::From7e3EDRAM4(uint32_t f10) {
  f10 &= 0x3FFu;
  if (!f10) return 0.0f;
  uint32_t mantissa = f10 & 0x7Fu, exponent = f10 >> 7;
  if (!exponent) {
    const uint32_t displacement = 7u - uint32_t(std::bit_width(mantissa) - 1);
    exponent = 1u - displacement;
    mantissa = (mantissa << displacement) & 0x7Fu;
  }
  return std::bit_cast<float>(((exponent + 124u) << 23) | (mantissa << 16));
}

// sa2_native_targets.cpp:5364

// Partial tile of a redirected clear: [px0,px1) x [py0,py1) of tile `local` of `drawn` (its pixels), mapped
// into the tile's current owner. Same-geometry depth owners (and the drawn view itself) take a sub-rect;
// a color owner takes the words the depth layout puts there (the 40-word half swap of the export shaders),
// only when they cover whole owner pixels.
// cross (ds2-nx, ds2_clear_depth_cross): a depth owner of the other encoding can take the clear.
template <typename ColorFor, typename Region>
bool TargetsDk::PartialEDRAM4(Image& drawn, uint32_t local, uint32_t px0, uint32_t px1, uint32_t py0, uint32_t py1,
                   const ColorFor& color_for, Region& region, bool cross) {
  const auto owner = owners_tiles_edram4_[(drawn.edram_base + local) & 2047u];
  Image* w = owner.image ? owner.image : nullptr;
  if (!w || w->guest_width || w->guest_height || w->raster_grid_x || !w->prepared || w->invalid_content ||
      w->edram_64bpp || !SamplesSupported(*w)) return false;
  const uint32_t wp = PitchTilesEDRAM(*w);
  if (!wp) return false;
  const uint32_t ox = (owner.tile_local % wp) * (80u >> w->edram_msaa_x);
  const uint32_t oy = (owner.tile_local / wp) * (16u >> w->edram_msaa_y);
  // Samples of the tile the clear covers.
  const uint32_t sx0 = px0 << drawn.edram_msaa_x, sx1 = px1 << drawn.edram_msaa_x;
  const uint32_t sy0 = py0 << drawn.edram_msaa_y, sy1 = py1 << drawn.edram_msaa_y;
  const uint32_t mx = (1u << w->edram_msaa_x) - 1u, my = (1u << w->edram_msaa_y) - 1u;
  if ((sy0 & my) || (sy1 & my)) return false;
  const auto rect = [&](uint32_t a, uint32_t b) -> VkRect2D {
    return {{int32_t(ox + (a >> w->edram_msaa_x)), int32_t(oy + (sy0 >> w->edram_msaa_y))},
            {(b - a) >> w->edram_msaa_x, (sy1 - sy0) >> w->edram_msaa_y}};
  };
  region.owner = w;
  region.first_local = local;
  region.owner_local = owner.tile_local;
  region.count = 1;
  region.reassign = false;
  region.area2 = {};
  region.partial = true;
  if (w->edram_depth) {
    if (w != &drawn && (w->edram_format != drawn.edram_format ||
                        w->depth_float24_half != drawn.depth_float24_half) &&
        !(cross && w->edram_format != drawn.edram_format)) return false;
    if ((sx0 & mx) || (sx1 & mx)) return false;
    region.area = rect(sx0, sx1);
  } else {
    if (!color_for(*w)) return false;
    // Depth word d sits where color word (d < 40 ? d + 40 : d - 40) is.
    const uint32_t a0 = sx0, a1 = std::min(sx1, 40u), b0 = std::max(sx0, 40u), b1 = sx1;
    bool first = true;
    if (a1 > a0) {
      if (((a0 + 40) & mx) || ((a1 + 40) & mx)) return false;
      region.area = rect(a0 + 40, a1 + 40);
      first = false;
    }
    if (b1 > b0) {
      if (((b0 - 40) & mx) || ((b1 - 40) & mx)) return false;
      (first ? region.area : region.area2) = rect(b0 - 40, b1 - 40);
    }
  }
  const VkRect2D& r = region.area;
  return r.extent.width && r.extent.height && uint32_t(r.offset.x) + r.extent.width <= w->width &&
         uint32_t(r.offset.y) + r.extent.height <= w->height &&
         (!region.area2.extent.width ||
          uint32_t(region.area2.offset.x) + region.area2.extent.width <= w->width);
}

// sa2_native_targets.cpp:5435

bool TargetsDk::RedirectClearDepthEDRAM4(const SubmissionDraw& p) {
  if (!REXCVAR_GET(sa2_native_edram4_redirected_clear) || !p.registers || !p.edram_overwrite_rect ||
      !(p.edram_overwrite_slots & 1) || !draws_) return false;
  const auto no = [&](const char* reason) {
    if (g_redirect_key_int.active) {  // folded into the string map when the report is written: same report
      g_redirect_key_int.UseSampled();
      ++edram4_depth_no_fast_[{reason, p.registers[gr::XE_GPU_REG_RB_DEPTH_INFO]}];
      return false;
    }
    ++edram4_depth_no_reasons_[fmt::format("{} (depth info {:08X})", reason,
                                           p.registers[gr::XE_GPU_REG_RB_DEPTH_INFO])];
    return false;
  };
  if (!p.edram_overwrite_depth) return no("depth not proven constant");
  // sa2-nx: depth + color 0 (Direct3D's Clear through the 4x alias). The color half is planned first, with no
  // side effect, and committed after the depth half: both or neither.
  ColorClearEDRAM4 color_clear;
  if (p.edram_overwrite_slots == 3) {
    if (!PlanClearColor0EDRAM4(p, color_clear)) return no("also writes color (color 0 not redirectable)");
  } else if (p.edram_overwrite_slots != 1) {
    return no("also writes color");
  }
  const uint32_t* r = p.registers;
  const uint32_t mode = r[gr::XE_GPU_REG_RB_MODECONTROL] & 7;
  const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
  if (mode != 5 && !(mode == uint32_t(xenos::EdramMode::kColorDepth) &&
                     (!r[gr::XE_GPU_REG_RB_COLOR_MASK] || !color_clear.runs.empty())))
    return no("color mask on");
  if ((dc & 0x76) != 0x76) return no("depth not ALWAYS+write");
  // Stencil on: only a full-mask constant stencil write over the same rectangle (a depth+stencil D3D Clear);
  // both aspects are then cleared in the owner.
  int32_t stencil_value = -1;
  if (dc & 1) {
    if (!REXCVAR_GET(sa2_native_edram4_clear_stencil) || !p.edram_stencil_replace_rect ||
        *p.edram_stencil_replace_rect != *p.edram_overwrite_rect) return no("stencil not replaced over the rect");
    const auto value_for = [](uint32_t zpass, uint32_t refmask) -> int32_t {
      return zpass == 2 ? int32_t(refmask & 0xFFu) : zpass == 1 ? 0 : -1;
    };
    stencil_value = value_for((dc >> 14) & 7, r[gr::XE_GPU_REG_RB_STENCILREFMASK]);
    if (stencil_value < 0 || ((dc & 0x80) &&
        value_for((dc >> 26) & 7, r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]) != stencil_value))
      return no("stencil value not constant");
  }
  // D24FS8: draws round depth to float24; only 0 and 1 (exact in float24) can be cleared without it.
  if (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 1) &&
      !(*p.edram_overwrite_depth == 0.0f || *p.edram_overwrite_depth == 1.0f)) return no("D24FS8 depth not 0/1");
  const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
  const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
  if (!Record()) return false;
  Image* drawn = GetDepth(info & 0xFFF, (info >> 16) & 1, pitch);
  if (!drawn || drawn->guest_width || drawn->guest_height || drawn->raster_grid_x) return no("drawn view unsupported");
  const auto& o = *p.edram_overwrite_rect;
  const uint32_t tw = 80u >> drawn->edram_msaa_x, th = 16u >> drawn->edram_msaa_y;
  // A color owner gets the EDRAM word the clear writes, decoded as its own format (the export shaders'
  // decoding); needs the whole word, so the stencil byte too.
  std::optional<VkClearColorValue> color_rgba8, color_16f;
  if (stencil_value >= 0 && REXCVAR_GET(sa2_native_edram4_clear_color)) {
    const float z = std::clamp(*p.edram_overwrite_depth, 0.0f, 1.0f);
    const uint32_t depth = drawn->edram_format == 0 ? uint32_t(std::nearbyint(double(z) * 16777215.0))
                                                           : WordFloat24EDRAM4(z);
    const uint32_t word = (depth << 8) | uint32_t(stencil_value);
    VkClearColorValue a{}, b{};
    for (uint32_t i = 0; i < 4; ++i) a.float32[i] = float((word >> (8 * i)) & 255u) * (1.0f / 255.0f);
    color_rgba8 = a;
    for (uint32_t i = 0; i < 3; ++i) b.float32[i] = From7e3EDRAM4(word >> (10 * i));
    b.float32[3] = float(word >> 30) * (1.0f / 3.0f);
    color_16f = b;  // 7e3 (format 3); UNORM10 (format 2) is decided per owner below
  }
  const bool word_zero = color_rgba8 && stencil_value == 0 && *p.edram_overwrite_depth == 0.0f;
  const auto color_for = [&](const Image& w) -> std::optional<VkClearColorValue> {
    if (w.edram_depth || !color_rgba8) return std::nullopt;
    if (word_zero) return VkClearColorValue{};  // all-zero words are 0 in every color format
    if (w.edram_64bpp) return std::nullopt;
    if (w.format == VK_FORMAT_R8G8B8A8_UNORM && (w.edram_format == 0 || w.edram_format == 1)) return color_rgba8;
    if ((w.format == VK_FORMAT_R16G16B16A16_SFLOAT || w.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) &&
        w.edram_format == 3) return color_16f;
    if (w.format == VK_FORMAT_R16G16B16A16_SFLOAT && w.edram_format == 2) {
      const uint32_t word = [&] {
        VkClearColorValue a = *color_rgba8;
        uint32_t v = 0;
        for (uint32_t i = 0; i < 4; ++i) v |= uint32_t(std::lround(a.float32[i] * 255.0f)) << (8 * i);
        return v;
      }();
      VkClearColorValue c{};
      for (uint32_t i = 0; i < 3; ++i) c.float32[i] = float((word >> (10 * i)) & 0x3FFu) / 1023.0f;
      c.float32[3] = float(word >> 30) / 3.0f;
      return c;
    }
    return std::nullopt;
  };
  // Partial edge tiles are cleared in their owner too when the bounds are exact (the draw touches no pixel
  // outside the proven rect): sa2_native_edram4_partial_clear.
  const bool aligned = !(o[0] % tw || o[1] % th || o[2] % tw || o[3] % th);
  const bool partial_ok = REXCVAR_GET(sa2_native_edram4_partial_clear) && p.edram_bounds_rect &&
      *p.edram_bounds_rect == o && o[2] > o[0] && o[3] > o[1];
  if (o[0] < 0 || o[1] < 0 || (!aligned && !partial_ok) ||
      uint32_t(o[2]) > drawn->width || uint32_t(o[3]) > drawn->height) {
    ++edram4_redirected_no_;
    ++edram4_depth_no_reasons_[fmt::format("rect {},{}-{},{} not tile-aligned in {:03X}:{}x{}:mx{}my{}", o[0], o[1],
                                           o[2], o[3], drawn->edram_base, drawn->width, drawn->height,
                                           drawn->edram_msaa_x, drawn->edram_msaa_y)];
    return false;
  }
  const uint32_t pitch_tiles = PitchTilesEDRAM(*drawn);
  // Depth 0 is the all-zero 24-bit field in both D24S8 and D24FS8 (and 0 in every host encoding): another
  // depth encoding's owner can take the clear as is.
  const bool zero_depth = *p.edram_overwrite_depth == 0.0f && REXCVAR_GET(sa2_native_edram4_clear_zero);
  // ds2-nx (ds2_clear_depth_cross): a depth owner of the other encoding gets the guest depth whose 24-bit field in
  // ITS encoding is the field the clear writes in the drawn view's encoding (EDRAM transfers between the two
  // encodings move the field bit for bit). Only when that round trip is exact.
  std::optional<float> cross_depth;
  if (REXCVAR_GET(ds2_clear_depth_cross) && !zero_depth) {
    const float z = std::clamp(*p.edram_overwrite_depth, 0.0f, 1.0f);
    if (drawn->edram_format == 1) {  // drawn D24FS8 -> an owner in D24S8
      const uint32_t field = WordFloat24EDRAM4(z);
      const float v = float(double(field) / 16777215.0);
      if (uint32_t(std::nearbyint(double(v) * 16777215.0)) == field) cross_depth = v;
    } else {  // drawn D24S8 -> an owner in D24FS8
      const uint32_t field = uint32_t(std::nearbyint(double(z) * 16777215.0));
      const float v = me::native::DecodeDepthFloat20e4(field);
      if (WordFloat24EDRAM4(v) == field) cross_depth = v;
    }
  }
  // The guest depth to clear in owner w (its own encoding).
  const auto depth_for = [&](const Image& w) -> float {
    return cross_depth && w.edram_depth && w.edram_format != drawn->edram_format ? *cross_depth
                                                                                 : *p.edram_overwrite_depth;
  };
  using Region = RedirectRegionEDRAM4;
  // ds2_redirect_regions_reuse: the member list unless a nested call holds it (then a local one, as before).
  std::vector<Region> local_regions;
  const bool reuse = REXCVAR_GET(ds2_redirect_regions_reuse) && !redirect_regions_busy_;
  std::vector<Region>& regions = reuse ? redirect_regions_ : local_regions;
  regions.clear();
  struct BusyGuard {
    bool* flag;
    ~BusyGuard() { if (flag) *flag = false; }
  } busy_guard{reuse ? &redirect_regions_busy_ : nullptr};
  if (reuse) redirect_regions_busy_ = true;
  // Target of a tile: its current owner if that is another depth view; if the drawn view itself owns it
  // (or nobody does), the drawn view's known 1x alias, which then becomes the owner.
  const auto alias_it = edram4_alias_1x_.find(drawn);
  Image* const alias = alias_it != edram4_alias_1x_.end() && REXCVAR_GET(sa2_native_edram4_clear_alias)
                            ? alias_it->second : nullptr;
  struct Goal { Image* w; uint32_t local; bool reassign; };
  const auto goal = [&](uint32_t local) -> Goal {
    const auto owner = owners_tiles_edram4_[(drawn->edram_base + local) & 2047u];
    // A depth owner with the same encoding is cleared in place. Any other owner (a color view, a different
    // encoding) hands the tile to the 1x alias: the clear writes all of its depth, and the lazy-stencil
    // record keeps pointing at the old owner's bits (a depth-only clear leaves them in EDRAM).
    const bool owner_utilizable = owner.image && owner.image != drawn && owner.image->edram_depth &&
        ((owner.image->edram_format == drawn->edram_format &&
          owner.image->depth_float24_half == drawn->depth_float24_half) || zero_depth ||
         (cross_depth && !owner.image->edram_64bpp && owner.image->edram_format != drawn->edram_format));
    // A color owner can only be a later stencil-fetch source in the classes the import reads.
    const bool color_supported = owner.image && !owner.image->edram_depth && !owner.image->edram_64bpp &&
        (owner.image->edram_format == 0 || owner.image->edram_format == 2 || owner.image->edram_format == 3);
    if (owner_utilizable || (owner.image && owner.image != drawn && (!alias || (!owner.image->edram_depth &&
                                                                                  !color_supported))))
      return {owner.image, owner.tile_local, false};
    return {alias, local, true};
  };
  const uint32_t ty_end = (uint32_t(o[3]) + th - 1) / th, tx_end = (uint32_t(o[2]) + tw - 1) / tw;
  // Learned consumer (the view that last pulled this clear's tiles): clear straight into it and hand it the
  // tiles, so its next bind transfers nothing. Ownership stays exact whatever view is chosen.
  bool direct = false;
  const uint64_t site = SiteClear(drawn, edram4_cleared_in_frame_[drawn]++);
  if (aligned && REXCVAR_GET(sa2_native_edram4_clear_consumer)) {
    const auto it = edram4_consumer_.find(site);
    // Only a consumer seen twice in a row (one that alternates would just move the transfer).
    Image* c = it != edram4_consumer_.end() && it->second.confidence >= 2 ? it->second.view : nullptr;
    const bool c_ok = c && c != drawn && c->image != drawn->image && c->prepared && !c->invalid_content &&
        !c->guest_width && !c->guest_height && !c->raster_grid_x &&
        SamplesSupported(*c) &&
        (c->edram_depth ? !c->edram_64bpp && ((c->edram_format == drawn->edram_format &&
                                                    c->depth_float24_half == drawn->depth_float24_half) ||
                                                   zero_depth)
                              : bool(color_for(*c)));
    const uint32_t cp = c_ok ? PitchTilesEDRAM(*c) : 0;
    const uint32_t cw = c_ok ? 80u >> (c->edram_msaa_x + uint32_t(c->edram_64bpp)) : 0;
    const uint32_t ch = c_ok ? 16u >> c->edram_msaa_y : 0;
    if (cp && !(c->width % cw)) {
      direct = true;
      for (uint32_t ty = uint32_t(o[1]) / th; direct && ty < ty_end; ++ty) {
        for (uint32_t tx = uint32_t(o[0]) / tw; tx < tx_end;) {
          const uint32_t local = ty * pitch_tiles + tx;
          const uint32_t lc = (drawn->edram_base + local - c->edram_base) & 2047u;
          uint32_t count = 1;
          while (tx + count < tx_end && (lc % cp) + count < cp) ++count;
          const uint32_t x = (lc % cp) * cw, y = (lc / cp) * ch, width = cw * count;
          if (x >= c->width || y >= c->height || width > c->width - x || ch > c->height - y) {
            direct = false;
            break;
          }
          regions.push_back({c, local, lc, count, {{int32_t(x), int32_t(y)}, {width, ch}}, true});
          tx += count;
        }
      }
      if (direct) ++edram4_consumer_uses_;
      else regions.clear();
    }
  }
  const auto tile_whole = [&](uint32_t tx, uint32_t ty) {
    return int32_t(tx * tw) >= o[0] && int32_t((tx + 1) * tw) <= o[2] &&
           int32_t(ty * th) >= o[1] && int32_t((ty + 1) * th) <= o[3];
  };
  // ds2-nx (ds2_clear_depth_claim): the clear goes into the drawn view itself, which takes every tile it touches.
  const bool claim = !direct && drawn->claim_depth_clears && drawn->prepared && !drawn->invalid_content &&
                     SamplesSupported(*drawn);
  for (uint32_t ty = uint32_t(o[1]) / th; claim && ty < ty_end; ++ty) {
    for (uint32_t tx = uint32_t(o[0]) / tw; tx < tx_end;) {
      const uint32_t local = ty * pitch_tiles + tx;
      const uint32_t x0 = uint32_t(std::max<int32_t>(o[0], int32_t(tx * tw)));
      const uint32_t y0 = uint32_t(std::max<int32_t>(o[1], int32_t(ty * th)));
      const uint32_t y1 = uint32_t(std::min<int32_t>(o[3], int32_t((ty + 1) * th)));
      uint32_t count = 1;
      if (tile_whole(tx, ty))
        while (tx + count < tx_end && tile_whole(tx + count, ty)) ++count;
      const uint32_t x1 = uint32_t(std::min<int32_t>(o[2], int32_t((tx + count) * tw)));
      Region region{drawn, local, local, count, {{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}}, true};
      region.partial = !tile_whole(tx, ty);
      regions.push_back(region);
      tx += count;
    }
  }
  if (claim) ++edram4_claimed_clears_;
  for (uint32_t ty = uint32_t(o[1]) / th; !direct && !claim && ty < ty_end; ++ty) {
    for (uint32_t tx = uint32_t(o[0]) / tw; tx < tx_end;) {
      const uint32_t local = ty * pitch_tiles + tx;
      if (!tile_whole(tx, ty)) {
        // The covered part of the tile, in the drawn view's pixels relative to the tile.
        const uint32_t px0 = uint32_t(std::max<int32_t>(o[0], int32_t(tx * tw))) - tx * tw;
        const uint32_t px1 = uint32_t(std::min<int32_t>(o[2], int32_t((tx + 1) * tw))) - tx * tw;
        const uint32_t py0 = uint32_t(std::max<int32_t>(o[1], int32_t(ty * th))) - ty * th;
        const uint32_t py1 = uint32_t(std::min<int32_t>(o[3], int32_t((ty + 1) * th))) - ty * th;
        Region region{};
        if (!PartialEDRAM4(*drawn, local, px0, px1, py0, py1, color_for, region, cross_depth.has_value() || (zero_depth && REXCVAR_GET(ds2_clear_depth_cross)))) {
          ++edram4_redirected_no_;
          ++edram4_depth_no_reasons_["partial tile: owner unsupported"];
          return false;
        }
        regions.push_back(region);
        ++tx;
        continue;
      }
      const Goal t = goal(local);
      Image* w = t.w;
      const bool color_ok = w && w != drawn && !t.reassign && color_for(*w) && !w->guest_width && !w->guest_height &&
          w->prepared && !w->invalid_content && !w->raster_grid_x &&
          SamplesSupported(*w);
      if (!color_ok && (!w || w == drawn || !w->edram_depth || w->edram_64bpp || w->guest_width ||
          w->guest_height || w->image == drawn->image || !w->prepared ||
          ((w->edram_format != drawn->edram_format || w->depth_float24_half != drawn->depth_float24_half) &&
           !zero_depth) ||
          !SamplesSupported(*w))) {
        ++edram4_redirected_no_;
        ++edram4_depth_no_reasons_[!w ? std::string("no target (no 1x alias)") : w == drawn
            ? std::string("drawn view") : fmt::format("target {}{:03X}/f{}:{}x{}:mx{}my{} samples{} (clear of "
                "{:03X}/f{} mx{}my{} depth {} stencil {})",
                w->edram_depth ? "D" : "C", w->edram_base, w->edram_format, w->width, w->height,
                w->edram_msaa_x, w->edram_msaa_y, uint32_t(w->sample_count), drawn->edram_base,
                drawn->edram_format, drawn->edram_msaa_x, drawn->edram_msaa_y, *p.edram_overwrite_depth,
                stencil_value)];
        return false;
      }
      const uint32_t wp = PitchTilesEDRAM(*w);
      const uint32_t ww = 80u >> w->edram_msaa_x, wh = 16u >> w->edram_msaa_y;
      if (!wp || w->width % ww || (t.reassign && wp != pitch_tiles)) { ++edram4_redirected_no_; return false; }
      uint32_t count = 1;
      while (tx + count < tx_end && tile_whole(tx + count, ty) && (t.local % wp) + count < wp) {
        const Goal n = goal(local + count);
        if (n.w != w || n.local != t.local + count || n.reassign != t.reassign) break;
        ++count;
      }
      const uint32_t x = (t.local % wp) * ww << w->raster_grid_x;
      const uint32_t y = (t.local / wp) * wh;
      const uint32_t width = (ww * count) << w->raster_grid_x;
      if (x >= w->width || y >= w->height || width > w->width - x || wh > w->height - y) {
        ++edram4_redirected_no_;
        return false;
      }
      regions.push_back({w, local, t.local, count, {{int32_t(x), int32_t(y)}, {width, wh}}, t.reassign});
      tx += count;
    }
  }
  if (regions.empty()) return false;
  draws_->FinishPass();
  if (!Record()) return false;
  // sa2-nx: every region is prepared first, then all the clears go between ONE barrier pair, with the regions of
  // one owner that line up (consecutive tile rows of a full-width clear) merged into one rectangle: a full-screen
  // redirected clear was 45 render passes and 90 ALL -> ALL barriers (each a GPU wait-for-idle on NVK). The
  // regions are distinct tiles, so the clears have no hazard between them; the bookkeeping below is unchanged.
  struct MergedClear {
    Image* w;
    VkRect2D rect;
  };
  std::vector<MergedClear> merged;
  const auto add_clear = [&](Image* w, const VkRect2D& rect) {
    for (auto& m : merged) {
      if (m.w == w && m.rect.offset.x == rect.offset.x && m.rect.extent.width == rect.extent.width &&
          m.rect.offset.y + int32_t(m.rect.extent.height) == rect.offset.y) {
        m.rect.extent.height += rect.extent.height;
        return;
      }
    }
    merged.push_back({w, rect});
  };
  for (const auto& region : regions) {
    Image& w = *region.owner;
    Prepare(w);
    if (!w.prepared || !RestoreContent(w)) return false;
    ForgetClear(w);
    if (!w.edram_depth) {
      if (!color_for(w)) return FailureEDRAM4("redirected depth clear into a color owner failed", *drawn, &w);
    } else if (region.partial && stencil_value >= 0 && StencilDeferredEDRAM4(w, region.owner_local, 1) &&
               !FetchStencilDeferredEDRAM4(w, region.owner_local, 1)) {
      // A partial stencil clear keeps the rest of the tile's stencil: make it real here first.
      return FailureEDRAM4("partial redirected clear: stencil fetch failed", *drawn, &w);
    }
    add_clear(&w, region.area);
    if (region.area2.extent.width) add_clear(&w, region.area2);
  }
  BarrierClearEDRAM4();
  for (const auto& m : merged) {
    Image& w = *m.w;
    if (!w.edram_depth) {
      if (!draws_->ClearColorInPass(commands_work_, w, *color_for(w), m.rect))
        return FailureEDRAM4("redirected depth clear into a color owner failed", *drawn, &w);
      continue;
    }
    const float native = me::native::GuestDepthToNative(depth_for(w), w.depth_float24_half);
    if (stencil_value >= 0 ? !draws_->ClearDepthInPass(commands_work_, w, native,
                                                                uint32_t(stencil_value), &m.rect)
                           : !draws_->ClearOnlyDepthInPass(commands_work_, w, native, m.rect))
      return FailureEDRAM4("redirected depth clear failed", *drawn, &w);
  }
  BarrierClearEDRAM4();
  edram4_redirected_merged_ += merged.size();
  for (const auto& region : regions) {
    Image& w = *region.owner;
    if (!w.edram_depth) {
      for (uint32_t i = 0; i < region.count; ++i) {
        const uint32_t physical = (drawn->edram_base + region.first_local + i) & 2047u;
        auto& d = owners_tiles_edram4_[physical];
        if (region.reassign) {
          d = {&w, uint16_t((region.owner_local + i) & 2047u), 0};
          ++edram4_epoch_;
        }
        d.version = ++edram4_version_;
        edram4_stamps_[physical] = {site, d.version};
      }
      edram4_redirected_color_tiles_ += region.count;
      continue;
    }
    if (stencil_value >= 0) w.edram4_bits_stencil |= uint8_t(stencil_value);
    for (uint32_t i = 0; i < region.count; ++i) {
      auto& d = owners_tiles_edram4_[(drawn->edram_base + region.first_local + i) & 2047u];
      if (region.reassign) {
        const uint32_t local = (region.owner_local + i) & 2047u;  // the new owner's own tile index
        if (stencil_value < 0 && REXCVAR_GET(sa2_native_edram4_stencil_lazy) && d.image &&
            d.image != &w) {
          const auto previous = edram4_stencil_source_.find(d.image);
          const SourceStencil f = previous != edram4_stencil_source_.end() && previous->second[d.tile_local & 2047u].image
              ? previous->second[d.tile_local & 2047u] : SourceStencil{d.image, uint16_t(d.tile_local & 2047u)};
          auto& sources = StencilSourcesWrite(&w);
          if (sources.empty()) sources.resize(2048);
          sources[local] = f.image == &w && f.tile == local ? SourceStencil{} : f;
        }
        d = {&w, uint16_t(local), 0};
        ++edram4_epoch_;
      }
      d.version = ++edram4_version_;
      edram4_stamps_[(drawn->edram_base + region.first_local + i) & 2047u] = {site, d.version};
    }
    if (stencil_value >= 0) StencilRealEDRAM4(w, region.owner_local, region.count);  // both aspects written
    edram4_redirected_tiles_ += region.count;
  }
  if (!color_clear.runs.empty() && !CommitClearColor0EDRAM4(color_clear))
    return FailureEDRAM4("redirected color 0 clear failed", *drawn, color_clear.drawn);
  ++edram4_redirected_;
  return true;
}

// sa2_native_targets.cpp:5838
bool TargetsDk::PlanClearColor0EDRAM4(const SubmissionDraw& p, ColorClearEDRAM4& plan) {
  if (!REXCVAR_GET(sa2_native_edram4_clear_color0) || !p.edram_overwrite_color0 || !p.edram_overwrite_rect)
    return false;
  const uint32_t* r = p.registers;
  const uint32_t info = r[gr::XE_GPU_REG_RB_COLOR_INFO];
  // k_8_8_8_8 (no gamma, no float formats); ds2-nx (ds2_clear_color0_alias): also k_2_10_10_10_FLOAT (7e3, host
  // RGBA16F), Dark Souls II's half-resolution HDR target (EDRAM 000, 640x360) cleared through its 320x180 4x alias.
  const uint32_t color_format = (info >> 16) & 0xF;
  const bool f7e3 = color_format == 3 && REXCVAR_GET(ds2_clear_color0_alias) && REXCVAR_GET(ds2_clear_color0_7e3);
  if (color_format != 0 && !f7e3) return false;
  const VkFormat host_format = f7e3 ? HostFormatTargetColor(3) : VK_FORMAT_R8G8B8A8_UNORM;
  // The bytes the ROP stores: round to nearest of the clamped value. A value within float rounding of a tie
  // could round either way on the GPU: declined.
  for (uint32_t c = 0; c < 4; ++c) {
    const float v = (*p.edram_overwrite_color0)[c];
    if (std::isnan(v)) return false;
    if (f7e3) {
      // 7e3: a draw into the RGBA16F view stores the value clamped as LimitOutputs7e3 does (RGB to [0, 31.875],
      // alpha to [0, 1]), converted to half float; the clear converts the same clamped value.
      plan.value.float32[c] = std::clamp(v, 0.0f, c < 3 ? 31.875f : 1.0f);
      continue;
    }
    const double f = double(std::clamp(v, 0.0f, 1.0f)) * 255.0;
    if (std::abs(f - std::floor(f) - 0.5) < 1.0 / 1024.0) return false;
    plan.value.float32[c] = float(std::lround(f)) / 255.0f;
  }
  if (!Record()) return false;
  Image* drawn = GetTarget(info & 0xFFF, color_format, r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF);
  if (!drawn || drawn->edram_depth || drawn->edram_64bpp || drawn->guest_width || drawn->guest_height ||
      drawn->raster_grid_x || drawn->format != host_format || drawn->edram_format != color_format) return false;
  const auto& o = *p.edram_overwrite_rect;
  const uint32_t tw = 80u >> drawn->edram_msaa_x, th = 16u >> drawn->edram_msaa_y;
  // ds2-nx (ds2_clear_color0_partial): a bottom edge inside a tile row is taken below, through the alias.
  const bool bottom_partial = (o[3] % th) != 0 && REXCVAR_GET(ds2_clear_color0_partial);
  if (o[0] < 0 || o[1] < 0 || o[0] % tw || o[1] % th || o[2] % tw || ((o[3] % th) && !bottom_partial) ||
      o[2] <= o[0] || o[3] <= o[1] || uint32_t(o[2]) > drawn->width || uint32_t(o[3]) > drawn->height)
    return false;
  const uint32_t pitch_tiles = PitchTilesEDRAM(*drawn);
  if (!pitch_tiles) return false;
  // ds2-nx (ds2_clear_color0_alias): tiles whose owner cannot take the clear (another format, such as Dark Souls
  // II's 7e3 scene, or the drawn alias itself) go to the drawn view's known 1x RGBA8 alias, which becomes their
  // owner: the clear writes the same word to every sample, so the alias holds exactly the bytes the 4x -> 1x
  // transfer would have given it.
  Image* alias = nullptr;
  if (REXCVAR_GET(ds2_clear_color0_alias)) {
    const auto it = edram4_alias_1x_color_.find(drawn);
    Image* a = it != edram4_alias_1x_color_.end() ? it->second : nullptr;
    if (a && a != drawn && a->image != drawn->image && !a->edram_depth && !a->edram_64bpp &&
        a->format == host_format && a->edram_format == color_format && a->prepared && !a->invalid_content &&
        !a->guest_width && !a->guest_height && !a->raster_grid_x && SamplesSupported(*a) &&
        a->edram_base == drawn->edram_base && PitchTilesEDRAM(*a) == pitch_tiles && !(a->width % 80u))
      alias = a;
  }
  if (bottom_partial && !alias) return false;
  for (uint32_t ty = uint32_t(o[1]) / th; ty < (uint32_t(o[3]) + th - 1) / th; ++ty) {
    // ds2-nx (ds2_clear_color0_partial): the last, partly covered row goes to the alias whatever its owner; the
    // alias first takes the whole tiles (a transfer of that row only), then the covered rows are cleared.
    const bool row_partial = (ty + 1) * th > uint32_t(o[3]);
    if (row_partial) {
      const uint32_t local = ty * pitch_tiles + uint32_t(o[0]) / tw;
      const uint32_t count = (uint32_t(o[2]) - uint32_t(o[0])) / tw;
      const uint32_t x = (local % pitch_tiles) * 80u, y = (local / pitch_tiles) * 16u, width = 80u * count;
      const uint32_t height = (uint32_t(o[3]) - ty * th) * (16u / th);
      if (x >= alias->width || y >= alias->height || width > alias->width - x || 16u > alias->height - y)
        return false;
      plan.runs.push_back({alias, local, count, {{int32_t(x), int32_t(y)}, {width, height}}, true, true});
      continue;
    }
    for (uint32_t tx = uint32_t(o[0]) / tw; tx < uint32_t(o[2]) / tw;) {
      const uint32_t local = ty * pitch_tiles + tx;
      const auto owner = owners_tiles_edram4_[(drawn->edram_base + local) & 2047u];
      Image* w = owner.image;
      if (!w || w == drawn || w->image == drawn->image || w->edram_depth || w->edram_64bpp ||
          w->format != host_format || w->edram_format != color_format || !w->prepared || w->invalid_content ||
          w->guest_width || w->guest_height || w->raster_grid_x ||
          !SamplesSupported(*w)) {
        if (!alias || w == alias) return false;
        // The alias takes the run of consecutive tiles of this row with the same (unusable) owner.
        uint32_t count = 1;
        while (tx + count < uint32_t(o[2]) / tw &&
               owners_tiles_edram4_[(drawn->edram_base + local + count) & 2047u].image == w) ++count;
        const uint32_t x = (local % pitch_tiles) * 80u, y = (local / pitch_tiles) * 16u, width = 80u * count;
        if (x >= alias->width || y >= alias->height || width > alias->width - x || 16u > alias->height - y)
          return false;
        plan.runs.push_back({alias, local, count, {{int32_t(x), int32_t(y)}, {width, 16u}}, true});
        tx += count;
        continue;
      }
      const uint32_t wp = PitchTilesEDRAM(*w);
      const uint32_t ww = 80u >> w->edram_msaa_x, wh = 16u >> w->edram_msaa_y;
      if (!wp || w->width % ww) return false;
      uint32_t count = 1;
      while (tx + count < uint32_t(o[2]) / tw && (owner.tile_local % wp) + count < wp) {
        const auto next = owners_tiles_edram4_[(drawn->edram_base + local + count) & 2047u];
        if (next.image != w || next.tile_local != owner.tile_local + count) break;
        ++count;
      }
      const uint32_t x = (owner.tile_local % wp) * ww, y = (owner.tile_local / wp) * wh, width = ww * count;
      if (x >= w->width || y >= w->height || width > w->width - x || wh > w->height - y) return false;
      plan.runs.push_back({w, local, count, {{int32_t(x), int32_t(y)}, {width, wh}}});
      tx += count;
    }
  }
  plan.drawn = drawn;
  return !plan.runs.empty();
}

// sa2_native_targets.cpp:5944
bool TargetsDk::CommitClearColor0EDRAM4(const ColorClearEDRAM4& plan) {
  // Runs of consecutive tile rows of one owner that line up form one rectangle: a full-screen clear is one clear.
  struct Area {
    Image* w;
    VkRect2D rect;
  };
  std::vector<Area> areas;
  for (const auto& run : plan.runs) {
    if (!areas.empty() && areas.back().w == run.owner && areas.back().rect.offset.x == run.area.offset.x &&
        areas.back().rect.extent.width == run.area.extent.width &&
        areas.back().rect.offset.y + int32_t(areas.back().rect.extent.height) == run.area.offset.y) {
      areas.back().rect.extent.height += run.area.extent.height;
    } else {
      areas.push_back({run.owner, run.area});
    }
  }
  draws_->FinishPass();
  if (!Record()) return false;
  for (const auto& a : areas) {
    Prepare(*a.w);
    if (!a.w->prepared || !RestoreContent(*a.w)) return false;
    ForgetClear(*a.w);
  }
  // ds2-nx (ds2_clear_color0_partial): the alias takes the whole tiles of a partly covered row first.
  for (const auto& run : plan.runs) {
    if (!run.partial) continue;
    const VkRect2D tiles{{run.area.offset.x, run.area.offset.y}, {run.area.extent.width, 16u}};
    const uint32_t reason = edram4_reason_;
    edram4_reason_ = 1;
    const bool ok = SynchronizeEDRAM4(*run.owner, tiles);
    edram4_reason_ = reason;
    if (!ok) return false;
    draws_->FinishPass();
    if (!Record()) return false;
  }
  BarrierClearEDRAM4();
  for (const auto& a : areas)
    if (!draws_->ClearColorInPass(commands_work_, *a.w, plan.value, a.rect)) return false;
  BarrierClearEDRAM4();
  for (const auto& run : plan.runs) {
    for (uint32_t i = 0; i < run.count; ++i) {
      auto& d = owners_tiles_edram4_[(plan.drawn->edram_base + run.first_local + i) & 2047u];
      if (run.reassign) {  // ds2-nx (ds2_clear_color0_alias)
        d = {run.owner, uint16_t((run.first_local + i) & 2047u), 0};
        ++edram4_epoch_;
      }
      d.version = ++edram4_version_;
    }
    edram4_redirected_color0_tiles_ += run.count;
    if (run.reassign) ds2_color0_alias_tiles_ += run.count;
  }
  edram4_redirected_color0_areas_ += areas.size();
  return true;
}

// sa2_native_targets.cpp:6004

// Mass Effect (every frame, main scene depth D000): a full-screen stencil-only D3D Clear (stencil ALWAYS /
// REPLACE or ZERO, write mask 0xFF, depth test off) drawn through the 4x alias of the 1x depth buffer. Drawn
// literally it moves all depth 1x -> 4x and back (two full transfers, one with 9 stencil passes). The
// stencil of each tile is instead set in its current depth owner; depth stays, ownership stays.
bool TargetsDk::RedirectClearStencilEDRAM4(const SubmissionDraw& p) {
  if (!REXCVAR_GET(sa2_native_edram4_clear_stencil) || !p.registers || !p.edram_stencil_replace_rect ||
      !draws_) return false;
  const uint32_t* r = p.registers;
  const uint32_t mode = r[gr::XE_GPU_REG_RB_MODECONTROL] & 7;
  const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
  if (mode != 5 && !(mode == uint32_t(xenos::EdramMode::kColorDepth) && !r[gr::XE_GPU_REG_RB_COLOR_MASK]))
    return false;
  if (!(dc & 1) || (dc & 6)) return false;  // stencil on, depth test and write off: depth untouched
  const auto value_for = [](uint32_t zpass, uint32_t refmask) -> int32_t {
    return zpass == 2 ? int32_t(refmask & 0xFFu) : zpass == 1 ? 0 : -1;
  };
  const int32_t value = value_for((dc >> 14) & 7, r[gr::XE_GPU_REG_RB_STENCILREFMASK]);
  if (value < 0) return false;
  if ((dc & 0x80) && value_for((dc >> 26) & 7, r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]) != value) return false;
  const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
  const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
  if (!Record()) return false;
  Image* drawn = GetDepth(info & 0xFFF, (info >> 16) & 1, pitch);
  if (!drawn || drawn->guest_width || drawn->guest_height || drawn->raster_grid_x) return false;
  const auto& o = *p.edram_stencil_replace_rect;
  const uint32_t tw = 80u >> drawn->edram_msaa_x, th = 16u >> drawn->edram_msaa_y;
  if (o[0] < 0 || o[1] < 0 || o[2] <= o[0] || o[3] <= o[1] || o[0] % tw || o[1] % th || o[2] % tw ||
      o[3] % th || uint32_t(o[2]) > drawn->width || uint32_t(o[3]) > drawn->height) {
    ++edram4_stencil_redirected_no_;
    return false;
  }
  const uint32_t pitch_tiles = PitchTilesEDRAM(*drawn);
  struct Region { Image* owner; uint32_t first_local, owner_local, count; VkRect2D area; };
  std::vector<Region> regions;
  for (uint32_t ty = uint32_t(o[1]) / th; ty < uint32_t(o[3]) / th; ++ty) {
    for (uint32_t tx = uint32_t(o[0]) / tw; tx < uint32_t(o[2]) / tw;) {
      const uint32_t local = ty * pitch_tiles + tx;
      const auto owner = owners_tiles_edram4_[(drawn->edram_base + local) & 2047u];
      Image* w = owner.image;
      // Only another single-sample depth view can take the clear (the drawn view itself, a color owner or
      // no owner: draw it literally).
      if (!w || w == drawn || !w->edram_depth || w->edram_64bpp || w->guest_width || w->guest_height ||
          w->image == drawn->image || !w->prepared || w->invalid_content ||
          !SamplesSupported(*w)) {
        ++edram4_stencil_redirected_no_;
        ++edram4_stencil_no_reasons_[!w ? std::string("no owner") : w == drawn ? std::string("drawn view owns")
            : fmt::format("owner {}{:03X}/f{}:{}x{}:mx{}my{} samples{}", w->edram_depth ? "D" : "C",
                          w->edram_base, w->edram_format, w->width, w->height, w->edram_msaa_x, w->edram_msaa_y,
                          uint32_t(w->sample_count))];
        return false;
      }
      const uint32_t wp = PitchTilesEDRAM(*w);
      const uint32_t ww = 80u >> w->edram_msaa_x, wh = 16u >> w->edram_msaa_y;
      if (!wp || w->width % ww) { ++edram4_stencil_redirected_no_; return false; }
      uint32_t count = 1;
      while (tx + count < uint32_t(o[2]) / tw && (owner.tile_local % wp) + count < wp) {
        const auto n = owners_tiles_edram4_[(drawn->edram_base + local + count) & 2047u];
        if (n.image != w || n.tile_local != owner.tile_local + count) break;
        ++count;
      }
      const uint32_t x = (owner.tile_local % wp) * ww << w->raster_grid_x;
      const uint32_t y = (owner.tile_local / wp) * wh;
      const uint32_t width = (ww * count) << w->raster_grid_x;
      if (x >= w->width || y >= w->height || width > w->width - x || wh > w->height - y) {
        ++edram4_stencil_redirected_no_;
        return false;
      }
      regions.push_back({w, local, uint32_t(owner.tile_local), count, {{int32_t(x), int32_t(y)}, {width, wh}}});
      tx += count;
    }
  }
  if (regions.empty()) return false;
  draws_->FinishPass();
  if (!Record()) return false;
  for (const auto& region : regions) {
    Image& w = *region.owner;
    ForgetClear(w);
    BarrierClearEDRAM4();
    if (!draws_->ClearStencilInPass(commands_work_, w, uint32_t(value), region.area))
      return FailureEDRAM4("redirected stencil clear failed", *drawn, &w);
    BarrierClearEDRAM4();
    // The owner's stencil is now real (and known) for these tiles: no lazy record may bring older bits.
    StencilRealEDRAM4(w, region.owner_local, region.count);
    w.edram4_bits_stencil |= uint8_t(value);
    for (uint32_t i = 0; i < region.count; ++i)
      owners_tiles_edram4_[(drawn->edram_base + region.first_local + i) & 2047u].version = ++edram4_version_;
    edram4_stencil_redirected_tiles_ += region.count;
  }
  ++edram4_stencil_redirected_;
  return true;
}

// sa2_native_targets.cpp:6096

// Lazy stencil, second half: before a draw (or an export) needs the real stencil of `view`'s tiles in
// [start, start + count), copy it from where it stayed (8 bit passes after a stencil-only clear;
// depth untouched). Runs are grouped by source image and contiguous source tiles.
bool TargetsDk::FetchStencilDeferredEDRAM4(Image& view, uint32_t start, uint32_t count) {
  const auto it = edram4_stencil_source_.find(&view);
  if (it == edram4_stencil_source_.end()) return true;
  for (uint32_t i = 0; i < count;) {
    const SourceStencil f = it->second[(start + i) & 2047u];
    if (!f.image) { ++i; continue; }
    uint32_t n = 1;
    while (i + n < count) {
      const SourceStencil g = it->second[(start + i + n) & 2047u];
      if (g.image != f.image || g.tile != ((f.tile + n) & 2047u)) break;
      ++n;
    }
    Image& src_data = *const_cast<Image*>(f.image);
    edram4_import_stencil_only_ = true;
    const bool ok = src_data.prepared && ImportColorDepthEDRAM4(src_data, view, f.tile, (start + i) & 2047u, n);
    edram4_import_stencil_only_ = false;
    if (!ok) return FailureEDRAM4("deferred stencil fetch failed", view, &src_data);
    StencilRealEDRAM4(view, start + i, n);
    ++edram4_stencil_fetched_;
    edram4_stencil_fetched_tiles_ += n;
    i += n;
  }
  return true;
}

// sa2_native_targets.cpp:6124

// sa2_cpu_ring_edram_memo. Eligible draws: no per-draw proven rectangle, stencil replace rect or stencil clear proof
// (they make the area or the stencil work specific to one draw). The key is every register PrepareDrawEDRAM4 reads.
bool TargetsDk::EdramMemoKey(const SubmissionDraw& p, std::array<uint32_t, 14>& key) const {
  if (!p.registers || !p.vs || p.edram_bounds_rect || p.edram_overwrite_rect || p.edram_overwrite_rect_count ||
      p.edram_stencil_replace_rect || p.edram_stencil_clear.eligible)
    return false;
  const uint32_t* r = p.registers;
  key = {r[gr::XE_GPU_REG_RB_MODECONTROL], r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
         r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET],
         r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_DEPTHCONTROL], r[gr::XE_GPU_REG_RB_DEPTH_INFO],
         r[gr::XE_GPU_REG_RB_COLOR_MASK], r[kDiagRegsColorInfo[0]], r[kDiagRegsColorInfo[1]],
         r[kDiagRegsColorInfo[2]], r[kDiagRegsColorInfo[3]], r[gr::XE_GPU_REG_RB_STENCILREFMASK],
         r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]};
  return true;
}

// sa2_native_targets.cpp:6140

// What the full path checks before each of its fast paths, for every slot the memo holds: the image prepared with
// valid content (Prepare / RestoreContent return at once), the draw's raster area owned in the current epoch
// (SynchronizeEDRAM4 returns at once), stencil already marked used. No deferred stencil source was written since
// (stencil_serial, checked by the caller), so FetchStencilDeferredEDRAM4 has nothing to fetch. GetTarget/GetDepth
// give the same images for the same key: their maps never drop an entry.
bool TargetsDk::EdramMemoStillFast(const EdramDrawMemo& m) const {
  if (!recording_) return false;
  const uint32_t dc = m.key[5];
  for (uint32_t slot = 0; slot < 5; ++slot) {
    if (!(m.plan.active_mask & (1u << slot))) continue;
    const Image* image = m.images[slot];
    if (!image || !image->prepared || image->invalid_content) return false;
    if (image->edram4_epoch != edram4_epoch_) return false;
    if (!ContainsRect(image->edram4_own, RasterArea(*image, m.area))) return false;
    if (slot == 0 && (dc & 1) && !image->edram4_usa_stencil) return false;
  }
  return true;
}

// sa2_native_targets.cpp:6159

// The state PrepareDrawEDRAM4 leaves, as the full path would leave it for this draw.
void TargetsDk::EdramMemoApply(const EdramDrawMemo& m) {
  edram4_cuts_extra_.clear();
  edram4_stencil_replacement_.reset();
  edram4_submission_ = nullptr;
  edram4_draw_images_ = m.images;
  edram4_draw_writes_ = m.writes;
  if (m.skipped_mode) return;  // the full path returns before touching the rest
  edram4_draw_writes_stencil_ = m.writes_stencil;
  edram4_draw_area_ = m.area;
  edram4_draw_plan_ = m.plan;
  depth_raster_grid_eligible_ = m.depth_raster_grid_eligible;
  edram4_reason_ = 0;
  edram4_slot_ = m.last_slot;
}

// sa2_native_targets.cpp:6180
bool TargetsDk::SamePlan(const me::native::EdramBoundPlan& a, const me::native::EdramBoundPlan& b) {
  return a.valid == b.valid && a.requested_mask == b.requested_mask && a.active_mask == b.active_mask &&
         a.suppressed_mask == b.suppressed_mask && a.pitch_tiles_at_32bpp == b.pitch_tiles_at_32bpp &&
         a.length_tiles == b.length_tiles && a.sorted_slots == b.sorted_slots && a.count == b.count;
}

// sa2_native_targets.cpp:6185

// A memo hit checked against the full path: run it, then require the same outputs and no work done by it (no
// synchronization past its fast path, no deferred stencil fetched, no ownership change).
bool TargetsDk::EdramMemoCheck(const SubmissionDraw& p, const EdramDrawMemo& m) {
  const uint64_t sync_before = edram4_sync_calls_, fetched_before = edram4_stencil_fetched_;
  const uint64_t late_before = edram4_stencil_late_, epoch_before = edram4_epoch_;
  const uint64_t serial_before = edram4_stencil_sources_serial_;
  const bool ok = PrepareDrawEDRAM4(p);
  ++edram4_memo_checked_;
  sa2::enh::VerifyCheck();
  const char* diff = nullptr;
  if (!ok) diff = "the full path failed";
  else if (edram4_sync_calls_ != sync_before) diff = "a synchronization left its fast path";
  else if (edram4_stencil_fetched_ != fetched_before || edram4_stencil_late_ != late_before) diff = "stencil work";
  else if (edram4_epoch_ != epoch_before || edram4_stencil_sources_serial_ != serial_before) diff = "ownership changed";
  else if (edram4_draw_images_ != m.images || edram4_draw_writes_ != m.writes) diff = "images or writes";
  else if (!m.skipped_mode &&
           (edram4_draw_writes_stencil_ != m.writes_stencil || !SameRect(edram4_draw_area_, m.area) ||
            !SamePlan(edram4_draw_plan_, m.plan) || depth_raster_grid_eligible_ != m.depth_raster_grid_eligible ||
            edram4_slot_ != m.last_slot))
    diff = "area, plan or stencil";
  if (diff) {
    sa2::enh::VerifyDifference();
    REXLOG_ERROR("[native] DIFFERENCE: EDRAM draw memo ({}) at draw {}; sa2_cpu_ring_edram_memo off for the session",
                 diff, draws_ ? draws_->Drawn() : 0);
    edram4_memo_on_ = false;
    edram4_memo_.valid = false;
  }
  return ok;
}

// sa2_native_targets.cpp:6215

bool TargetsDk::PrepareDrawEDRAM4Memo(const SubmissionDraw& p) {
  std::array<uint32_t, 14> key;
  EdramDrawMemo& m = edram4_memo_;
  if (!g_edram_memo.active || !edram4_memo_on_ || !REXCVAR_GET(sa2_native_edram4_fast) ||
      TraceTileEDRAM4() != UINT32_MAX || !EdramMemoKey(p, key)) {
    m.valid = false;
    ++edram4_memo_ineligible_;
    return PrepareDrawEDRAM4(p);
  }
  const bool ps_null = !p.ps;
  const uint32_t ps_outputs = p.ps ? (p.ps->outputs & 15u) : 0u;
  const int64_t used = p.edram_used_height_estimate ? int64_t(*p.edram_used_height_estimate) : -1;
  if (m.valid && m.key == key && m.ps_null == ps_null && m.ps_outputs == ps_outputs && m.used_height == used &&
      m.stencil_serial == edram4_stencil_sources_serial_ && (m.skipped_mode || EdramMemoStillFast(m))) {
    ++edram4_memo_hits_;
    g_edram_memo.UseSampled();
    if (edram4_memo_checks_left_ > 0 || sa2::enh::VerifyMode()) {
      if (edram4_memo_checks_left_ > 0) --edram4_memo_checks_left_;
      return EdramMemoCheck(p, m);
    }
    EdramMemoApply(m);
    return true;
  }
  ++edram4_memo_misses_;
  const bool ok = PrepareDrawEDRAM4(p);
  if (!ok) {
    m.valid = false;
    return false;
  }
  const uint32_t mode = key[0] & 7;
  m.valid = true;
  m.key = key;
  m.ps_null = ps_null;
  m.ps_outputs = ps_outputs;
  m.used_height = used;
  m.stencil_serial = edram4_stencil_sources_serial_;
  m.skipped_mode = mode != uint32_t(xenos::EdramMode::kColorDepth) && mode != 5;
  m.images = edram4_draw_images_;
  m.writes = edram4_draw_writes_;
  m.writes_stencil = edram4_draw_writes_stencil_;
  m.depth_raster_grid_eligible = depth_raster_grid_eligible_;
  m.area = edram4_draw_area_;
  m.plan = edram4_draw_plan_;
  m.last_slot = edram4_slot_;
  return true;
}

// sa2_native_targets.cpp:6262

bool TargetsDk::PrepareDrawEDRAM4(const SubmissionDraw& p) {
  // Per-draw state must not leak past this call (early returns included): stale cutouts would make a
  // later clear skip imports, a stale replace rect would zero stencil, a stale request would dangle.
  edram4_cuts_extra_.clear();
  edram4_stencil_replacement_.reset();
  struct Clean {
    TargetsDk* d;
    ~Clean() { d->edram4_cuts_extra_.clear(); d->edram4_stencil_replacement_.reset(); d->edram4_submission_ = nullptr; }
  } clean{this};
  edram4_draw_images_.fill(nullptr);
  edram4_draw_writes_.fill(false);
  if (!p.registers || !p.vs) return false;
  const uint32_t* r = p.registers;
  const uint32_t mode = r[gr::XE_GPU_REG_RB_MODECONTROL] & 7;
  if (mode != uint32_t(xenos::EdramMode::kColorDepth) && mode != 5) return true;
  const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  const int32_t wx = (tl >> 31) ? 0 : int32_t(window << 17) >> 17;
  const int32_t wy = (tl >> 31) ? 0 : int32_t((window >> 16) << 17) >> 17;
  const int32_t x0 = std::max(0, int32_t(tl & 0x3FFF) + wx);
  const int32_t y0 = std::max(0, int32_t((tl >> 16) & 0x3FFF) + wy);
  const int32_t x1 = std::max(x0, int32_t(br & 0x3FFF) + wx);
  const int32_t y1 = std::max(y0, int32_t((br >> 16) & 0x3FFF) + wy);
  edram4_draw_area_ = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
  // A proven rectangle (CPU-executed VS) bounds every covered pixel: the draw touches nothing outside
  // it, so tiles outside need no transfer even when the scissor is the whole window (D3D Clear rects).
  if ((p.edram_bounds_rect || p.edram_overwrite_rect) && REXCVAR_GET(sa2_native_edram4_area_rect)) {
    const auto& o = p.edram_bounds_rect ? *p.edram_bounds_rect : *p.edram_overwrite_rect;
    const int32_t ax0 = std::max(x0, o[0]), ay0 = std::max(y0, o[1]);
    const int32_t ax1 = std::max(ax0, std::min(x1, o[2])), ay1 = std::max(ay0, std::min(y1, o[3]));
    edram4_draw_area_ = {{ax0, ay0}, {uint32_t(ax1 - ax0), uint32_t(ay1 - ay0)}};
  }
  me::native::EdramBoundRequest request;
  request.pitch_pixels = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
  request.msaa = msaa_edram_actual_;
  request.used_height_pixels = std::min(uint32_t(y1), kMaxTargetHeight);
  if (p.edram_used_height_estimate)
    request.used_height_pixels = std::min(request.used_height_pixels, *p.edram_used_height_estimate);
  const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
  request.depth_used = (dc & 3) != 0;
  request.views[0].base = r[gr::XE_GPU_REG_RB_DEPTH_INFO] & 0xFFF;
  const uint32_t mask = r[gr::XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t info = r[kDiagRegsColorInfo[i]];
    request.views[i + 1] = {info & 0xFFF,
        xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat((info >> 16) & 15))};
    if (mode == uint32_t(xenos::EdramMode::kColorDepth) && p.ps && (p.ps->outputs & (1u << i)))
      request.normalized_color_mask |= uint16_t(mask & (15u << (i * 4)));
  }
  depth_raster_grid_eligible_ = request.depth_used && !request.normalized_color_mask;
  edram4_draw_plan_ = me::native::PlanEdramBoundRanges(request);
  if (!edram4_draw_plan_.valid) {
    ::g_banjo_last_reject_cause = 9002;  // banjo-nx: frame trace
    if (banjo_plan_failures_++ < 12)
      REXLOG_INFO("[native] EDRAM4 plan invalid: pitch {} msaa {} used height {} (scissor {},{}-{},{} window {:08X}"
                  " est {}) depth base {} used {} color base {} mask {:04X}", request.pitch_pixels, request.msaa,
                  request.used_height_pixels, x0, y0, x1, y1, window,
                  p.edram_used_height_estimate ? int(*p.edram_used_height_estimate) : -1, request.views[0].base,
                  request.depth_used, request.views[1].base, request.normalized_color_mask);
    return false;
  }
  if (!Record()) return false;
  for (uint32_t slot = 0; slot < 5; ++slot) {
    if (!(edram4_draw_plan_.active_mask & (1u << slot))) continue;
    const uint32_t info = slot ? r[kDiagRegsColorInfo[slot - 1]] : r[gr::XE_GPU_REG_RB_DEPTH_INFO];
    Image* image = slot ? GetTarget(info & 0xFFF, (info >> 16) & 15, request.pitch_pixels)
                           : GetDepth(info & 0xFFF, (info >> 16) & 1, request.pitch_pixels);
    edram4_reason_ = 0;
    edram4_submission_ = &p;
    edram4_slot_ = slot;
    if (!image) { ::g_banjo_last_reject_cause = 9004; return false; }
    if (slot == 0 && (dc & 1) && !image->edram4_usa_stencil) {
      image->edram4_usa_stencil = true;  // from now on its imports carry stencil
      if (StencilDeferredEDRAM4(*image, 0, 2048)) ++edram4_stencil_late_;
    }
    // sa2-nx (sa2_cpu_lazy_trace_text): the text is made when a trace line is logged, not for every draw slot.
    if (edram4_traces_ < 60 && !g_lazy_trace_text.active) edram4_draw_desc_ = TraceDrawEDRAM4();
    edram4_stencil_replacement_.reset();
    if (slot == 0 && p.edram_stencil_replace_rect && REXCVAR_GET(sa2_native_edram4_stencil_lazy)) {
      const auto& sr = *p.edram_stencil_replace_rect;
      edram4_stencil_replacement_ = RasterArea(*image, VkRect2D{{sr[0], sr[1]},
          {uint32_t(sr[2] - sr[0]), uint32_t(sr[3] - sr[1])}});
    }
    edram4_cuts_extra_.clear();
    for (uint32_t k = 0; k < p.edram_overwrite_rect_count && k < p.edram_overwrite_rects.size(); ++k) {
      const auto& q = p.edram_overwrite_rects[k];
      edram4_cuts_extra_.push_back(RasterArea(*image, VkRect2D{{q[0], q[1]},
          {uint32_t(q[2] - q[0]), uint32_t(q[3] - q[1])}}));
    }
    if (p.edram_overwrite_rect && (p.edram_overwrite_slots & (1u << slot))) {
      const auto& o = *p.edram_overwrite_rect;
      const VkRect2D overwritten = RasterArea(*image, VkRect2D{{o[0], o[1]},
          {uint32_t(o[2] - o[0]), uint32_t(o[3] - o[1])}});
      if (!SynchronizeEDRAM4(*image, RasterArea(*image, edram4_draw_area_),
                             edram4_draw_plan_.length_tiles[slot], true, 0, &overwritten))
        return false;
      ++edram4_overwrites_;
    } else if (!SynchronizeEDRAM4(*image, RasterArea(*image, edram4_draw_area_),
                                  edram4_draw_plan_.length_tiles[slot])) {
      ::g_banjo_last_reject_cause = 9005;
      return false;
    }
    edram4_cuts_extra_.clear();
    edram4_stencil_replacement_.reset();
    if (slot == 0 && (dc & 1)) {
      // Tiles entirely inside a proven stencil-replace rect get a known stencil from this draw; every other
      // tile the draw touches needs its real stencil first (a partial replace keeps the rest).
      if (p.edram_stencil_replace_rect) {
        const auto& sr = *p.edram_stencil_replace_rect;
        const VkRect2D replacement = RasterArea(*image, VkRect2D{{sr[0], sr[1]},
            {uint32_t(sr[2] - sr[0]), uint32_t(sr[3] - sr[1])}});
        for (uint32_t t = 0; t < edram4_draw_plan_.length_tiles[0] && t < 2048; ++t) {
          bool whole = false;
          if (TileUsedEDRAM4(*image, t, replacement, &whole) && whole) StencilRealEDRAM4(*image, t, 1);
        }
      }
      if (!FetchStencilDeferredEDRAM4(*image, 0, edram4_draw_plan_.length_tiles[0])) return false;
    }
    edram4_draw_images_[slot] = image;
    const uint32_t front = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t back = (dc & 0x80) ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF] : front;
    const bool front_writes = ((front >> 16) & 255u) &&
        (((dc >> 11) & 7) || ((dc >> 14) & 7) || ((dc >> 17) & 7));
    const bool back_writes = ((back >> 16) & 255u) && ((dc & 0x80)
        ? (((dc >> 23) & 7) || ((dc >> 26) & 7) || ((dc >> 29) & 7)) : front_writes);
    edram4_draw_writes_[slot] = slot != 0 || ((dc & 6) == 6) ||
                                  ((dc & 1) && (front_writes || back_writes));
    if (slot == 0) edram4_draw_writes_stencil_ = (dc & 1) && (front_writes || back_writes);
    if (slot == 0 && (dc & 1)) {
      // Bits this draw can set: REPLACE writes ref & wmask; INCR/DECR/INVERT (3..7) any bit of wmask.
      const auto bits_face = [](uint32_t refmask, uint32_t ops) -> uint8_t {
        const uint32_t ref = refmask & 255u, wmask = (refmask >> 16) & 255u;
        uint32_t b = 0;
        for (uint32_t k = 0; k < 3; ++k) {
          const uint32_t op = (ops >> (k * 3)) & 7u;
          if (op == 2) b |= ref & wmask;
          else if (op >= 3) b |= wmask;
        }
        return uint8_t(b);
      };
      image->edram4_bits_stencil |= bits_face(front, (dc >> 11) & 0x1FFu);
      if (dc & 0x80) image->edram4_bits_stencil |= bits_face(back, (dc >> 23) & 0x1FFu);
    }
  }
  const std::array<uint32_t, 18> trace_state{
      r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_DEPTH_INFO], dc, mask,
      r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], r[gr::XE_GPU_REG_PA_CL_VTE_CNTL],
      r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE], r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET],
      tl, br, window, r[gr::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
      r[gr::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR], request.used_height_pixels,
      r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL], r[gr::XE_GPU_REG_PA_SU_VTX_CNTL],
      r[gr::XE_GPU_REG_RB_COLOR_INFO], mode};
  static uint64_t trace_state_frame = UINT64_MAX;
  static std::array<uint32_t, 18> last_trace_state{};
  if (DrawTouchesTraceTileEDRAM4() &&
      (trace_state_frame != trace_frame_ || last_trace_state != trace_state) &&
      TraceEventEDRAM4(TraceTileEDRAM4())) {
    trace_state_frame = trace_frame_;
    last_trace_state = trace_state;
    REXLOG_INFO("[native] EDRAM TILE TRACE draw-request frame={} nextDraw={} physical={} "
                "vs={} ps={} surface={:08X} depthinfo={:08X} depthcontrol={:08X} colorMask={:08X} "
                "clip={:08X} vte={:08X} yscale={:08X} yoffset={:08X} windowTL={:08X} "
                "windowBR={:08X} windowOffset={:08X} screenTL={:08X} screenBR={:08X} usedHeight={} "
                "suMode={:08X} vtxCntl={:08X}",
                trace_frame_, draws_ ? draws_->Drawn() + 1 : 0,
                TraceTileEDRAM4(), p.vs->number, p.ps ? int32_t(p.ps->number) : -1,
                r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_DEPTH_INFO], dc, mask,
                r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], r[gr::XE_GPU_REG_PA_CL_VTE_CNTL],
                r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE], r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET],
                tl, br, window, r[gr::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
                r[gr::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR], request.used_height_pixels,
                r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL], r[gr::XE_GPU_REG_PA_SU_VTX_CNTL]);
  }
  return true;
}

// sa2_native_targets.cpp:6439

// Draw/scissor requests are guest pixel rectangles. Ownership and transfer
// shaders operate on actual host raster texels, including retained X samples.
VkRect2D TargetsDk::RasterArea(const Image& image, VkRect2D area) {
  area.offset.x = int32_t(int64_t(area.offset.x) * (1u << image.raster_grid_x));
  area.extent.width *= 1u << image.raster_grid_x;
  return area;
}

// sa2_native_targets.cpp:6452

// Synchronize aliases by physical EDRAM tile, not merely by equal base. Mass Effect has overlapping
// targets at different bases (notably 0x2D0 and 0x400), so exact-base aliasing alone leaves stale bands.
// Merges VkImageCopy regions that tile a larger rectangle in both images: first runs along a row
// (adjacent in source and destination, same rows), then stacks of equal runs. Same pixels copied.
void TargetsDk::MergeRegionsCopy(std::vector<VkImageCopy>& regions) {
  if (regions.size() < 2) return;
  const auto per_row = [](const VkImageCopy& a, const VkImageCopy& b) {
    return std::tie(a.srcOffset.y, a.dstOffset.y, a.srcOffset.x) <
           std::tie(b.srcOffset.y, b.dstOffset.y, b.srcOffset.x);
  };
  std::sort(regions.begin(), regions.end(), per_row);
  std::vector<VkImageCopy> rows;
  rows.reserve(regions.size());
  for (const VkImageCopy& r : regions) {
    if (!rows.empty()) {
      VkImageCopy& u = rows.back();
      if (u.srcOffset.y == r.srcOffset.y && u.dstOffset.y == r.dstOffset.y &&
          u.extent.height == r.extent.height &&
          u.srcOffset.x + int32_t(u.extent.width) == r.srcOffset.x &&
          u.dstOffset.x + int32_t(u.extent.width) == r.dstOffset.x) {
        u.extent.width += r.extent.width;
        continue;
      }
    }
    rows.push_back(r);
  }
  std::sort(rows.begin(), rows.end(), [](const VkImageCopy& a, const VkImageCopy& b) {
    return std::tie(a.srcOffset.x, a.dstOffset.x, a.extent.width, a.srcOffset.y) <
           std::tie(b.srcOffset.x, b.dstOffset.x, b.extent.width, b.srcOffset.y);
  });
  regions.clear();
  for (const VkImageCopy& r : rows) {
    if (!regions.empty()) {
      VkImageCopy& u = regions.back();
      if (u.srcOffset.x == r.srcOffset.x && u.dstOffset.x == r.dstOffset.x &&
          u.extent.width == r.extent.width &&
          u.srcOffset.y + int32_t(u.extent.height) == r.srcOffset.y &&
          u.dstOffset.y + int32_t(u.extent.height) == r.dstOffset.y) {
        u.extent.height += r.extent.height;
        continue;
      }
    }
    regions.push_back(r);
  }
}


}  // namespace sa2::native
#endif  // SA2_DK_AVAILABLE
