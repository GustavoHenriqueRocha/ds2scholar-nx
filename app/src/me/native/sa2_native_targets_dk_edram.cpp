// SA2 - native renderer on deko3d (ds2-deko3d): TargetsDk, region "alvos-edram-resolve". Translation of TargetsVulkan
// (sa2_native_targets.cpp) 6500-9957: the EDRAM alias modes 1-3, the mode-4 transfer passes (compute conversions,
// depth imports, depth exports, guestspace depth resolves, the fragment color conversions), the resolve passes, the
// resolve aliases, GetDepth, the resolved textures (park, sleep/wake, retire), SwapWithResolved, RestoreContent and the
// resolve barriers. Same logic, order, counters and logs as the Vulkan code; the GPU calls go to:
//   vkCmdCopyImage / vkCmdBlitImage     -> sa2::dk::CopyImage (DMA, whole texels) / sa2::dk::Blit (2D engine)
//   compute pipeline + descriptor set   -> dkCmdBufBindShaders(Compute) + transient descriptors of the slot
//                                          (PushDescriptor) bound with dkCmdBufBindImages / dkCmdBufBindTextures
//   vkCmdPushConstants                  -> PushConstants (Arena::Push: one uniform buffer address per dispatch / draw)
//                                          + dkCmdBufBindUniformBuffer(stage, 0)
//   render pass + framebuffer           -> dkCmdBufBindRenderTargets + viewport / scissor; the pass's external
//                                          dependency 0 is a sa2::dk::Barrier before the first draw
//   graphics pipeline                   -> sa2::dk::PassState + dkCmdBufBindShaders (vertex + fragment)
//   vkCmdSetStencilReference/WriteMask  -> dkCmdBufSetStencil
//   vkCmdClearAttachments (stencil)     -> scissor per rectangle + dkCmdBufClearDepthStencil (the clear is scissored)
//   vkCmdPipelineBarrier                -> sa2::dk::Barrier(cmd, Hazard::...) named by the dependency it serves
//   vkGetImageMemoryRequirements        -> sa2::dk::Bytes; vkGetFenceStatus -> sa2::dk::CompletedOrder
// Members and declarations: sa2_native_targets_dk_edram.inc. Report: ds2-deko3d
// docs/pesquisas/traducao/03-alvos-edram-resolve.md, section "Fase C".

#include "sa2_native_targets_dk.h"

#if SA2_DK_AVAILABLE

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The cvars and enhancements of this region are declared in sa2_native_targets_dk_types.h (alvos-infra).

namespace gr = rex::graphics;

namespace sa2::native {

namespace {

namespace xenos = rex::graphics::xenos;
using sa2::dk::Hazard;

// Vulkan: copy_image_ / blit_ were function pointers loaded at start (null = no such command). deko3d always has the
// copy engine (dkCmdBufCopyImage); the 2D engine needs DkImageFlags_Usage2DEngine on both images (sa2::dk::Has2D).
constexpr bool kCopyImageDk = true;

DkImageRect RectDk(const VkOffset3D& offset, const VkExtent3D& extent) {
  return {uint32_t(offset.x), uint32_t(offset.y), uint32_t(offset.z), extent.width, extent.height, extent.depth};
}

// vkCmdCopyImage with N regions -> N copy-engine launches. deko3d copies whole texels (a depth region carries its
// stencil), so a depth + stencil region pair is one region here (the callers pass only the first).
void CopyRegionsDk(DkCmdBuf cmd, sa2::dk::ImageId source, sa2::dk::ImageId target, uint32_t count,
                   const VkImageCopy* regions) {
  for (uint32_t i = 0; i < count; ++i) {
    sa2::dk::CopyImage(cmd, source, RectDk(regions[i].srcOffset, regions[i].extent), target,
                       RectDk(regions[i].dstOffset, regions[i].extent));
  }
}

// vkCmdBlitImage (one region, two corners, never flipped here) -> sa2::dk::Blit.
void BlitDk(DkCmdBuf cmd, sa2::dk::ImageId source, sa2::dk::ImageId target, const VkImageBlit& blit, bool linear) {
  const DkImageRect s{uint32_t(blit.srcOffsets[0].x), uint32_t(blit.srcOffsets[0].y), 0,
                      uint32_t(blit.srcOffsets[1].x - blit.srcOffsets[0].x),
                      uint32_t(blit.srcOffsets[1].y - blit.srcOffsets[0].y), 1};
  const DkImageRect d{uint32_t(blit.dstOffsets[0].x), uint32_t(blit.dstOffsets[0].y), 0,
                      uint32_t(blit.dstOffsets[1].x - blit.dstOffsets[0].x),
                      uint32_t(blit.dstOffsets[1].y - blit.dstOffsets[0].y), 1};
  sa2::dk::Blit(cmd, source, s, target, d, linear);
}

DkScissor ScissorDk(const VkRect2D& r) {
  return {uint32_t(r.offset.x), uint32_t(r.offset.y), r.extent.width, r.extent.height};
}

// The bound size of a push-constant uniform buffer: std140 blocks are read in 16-byte rows. Arena::Push hands out
// DK_UNIFORM_BUF_ALIGNMENT (256) aligned blocks, so the rounded size stays inside the block pushed.
constexpr uint32_t UniformBytes(uint32_t bytes) { return (bytes + 15u) & ~15u; }

// The procedural triangle of the renderer's passes reads no vertex attribute (gl_VertexID): the draws' vertex state
// must not stay bound (attributes enabled with another list's buffers).
void BindNoVerticesDk(DkCmdBuf cmd) {
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
}

// The viewport + scissor + vertex + fragment shader + one fragment uniform buffer + the 3-vertex draw of a fragment
// pass (the render target and the fixed state are bound by the caller).
void DrawPassDk(DkCmdBuf cmd, const DkShader* vs, const DkShader* fs, DkGpuAddr constants, uint32_t bytes) {
  const DkShader* shaders[2] = {vs, fs};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, constants, UniformBytes(bytes));
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
}

void DispatchDk(DkCmdBuf cmd, const DkShader* cs, DkGpuAddr constants, uint32_t bytes, uint32_t groups_x,
                uint32_t groups_y) {
  dkCmdBufBindShaders(cmd, DkStageFlag_Compute, &cs, 1);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Compute, 0, constants, UniformBytes(bytes));
  dkCmdBufDispatchCompute(cmd, groups_x, groups_y, 1);
}

// The external dependency 0 of the import and conversion render passes (ALL -> fragment shader / attachment). With
// sa2_native_pass_narrow_dependency only attachment and transfer writes needed it (compute and copy writes end with
// their own barrier): the 3D engine's own writes, Hazard::RenderToSample. Without it, MEMORY_READ | MEMORY_WRITE.
Hazard HazardPassStartDk() {
  return REXCVAR_GET(sa2_native_pass_narrow_dependency) ? Hazard::RenderToSample : Hazard::Full;
}

}  // namespace

// ---- sa2_native_targets.cpp 6500-6913 ----------------------------------------------------------------------------

// Mass Effect report: mode 2/3 EDRAM overlap work since the last report.
void TargetsDk::ReportOverlapsEDRAM() {
  if (!overlaps_copies_ && !overlaps_conversions_) return;
  REXLOG_INFO("[native] EDRAM overlap sync: {} copy batches, {} tiles in {} merged regions; "
              "{} format conversions over {} tiles",
              overlaps_copies_, overlaps_tiles_, overlaps_regions_, overlaps_conversions_,
              overlaps_tiles_converted_);
  overlaps_copies_ = overlaps_tiles_ = overlaps_regions_ = 0;
  overlaps_conversions_ = overlaps_tiles_converted_ = 0;
}

void TargetsDk::SynchronizeOverlapsEDRAM(uint32_t base, Image& target, const VkRect2D* area) {
  if (!kCopyImageDk || !target.width || !target.height) return;
  constexpr uint32_t kTileWidth = xenos::kEdramTileWidthSamples;
  constexpr uint32_t kTileHeight = xenos::kEdramTileHeightSamples;
  constexpr uint32_t kTilesMask = xenos::kEdramTileCount - 1;
  const uint32_t pitch_target = PitchTilesEDRAM(target);
  const uint32_t tile_height_target = kTileHeight >> target.edram_msaa_y;
  const uint32_t rows_target = (target.height + tile_height_target - 1) / tile_height_target;
  const uint32_t tiles_target =
      std::min(pitch_target * rows_target, xenos::kEdramTileCount);
  const auto tile_used = [&](uint32_t tile) {
    if (!area) return true;
    if (!area->extent.width || !area->extent.height) return false;
    const uint32_t tile_width = kTileWidth >> (uint32_t(target.edram_64bpp) + target.edram_msaa_x);
    const uint32_t x = (tile % pitch_target) * tile_width;
    const uint32_t y = (tile / pitch_target) * tile_height_target;
    return x < target.width && y < target.height &&
        x < uint32_t(area->offset.x) + area->extent.width &&
        x + tile_width > uint32_t(area->offset.x) &&
        y < uint32_t(area->offset.y) + area->extent.height &&
        y + tile_height_target > uint32_t(area->offset.y);
  };
  std::unordered_map<Image*, std::vector<VkImageCopy>> per_source;
  struct ConversionRange {
    Image* source;
    uint32_t tile_source;
    uint32_t tile_target;
    uint32_t count;
  };
  std::vector<ConversionRange> conversions;
  uint32_t distinct_formats = 0;
  for (uint32_t tile_target = 0; tile_target < tiles_target; ++tile_target) {
    if (!tile_used(tile_target)) continue;
    OwnerTileEDRAM& owner = owners_tiles_edram_[(base + tile_target) & kTilesMask];
    if (!owner.image || owner.image == &target) continue;
    Image& source = *owner.image;
    if (source.format != target.format ||
        source.edram_format != target.edram_format ||
        source.edram_64bpp != target.edram_64bpp ||
        source.edram_msaa_x != target.edram_msaa_x ||
        source.edram_msaa_y != target.edram_msaa_y || !source.width || !source.height) {
      const uint32_t source_start = owner.tile_local;
      uint32_t count = 1;
      // One compute dispatch can convert a physically contiguous run even when the
      // two image views use different pitches.
      while (tile_target + count < tiles_target && tile_used(tile_target + count)) {
        OwnerTileEDRAM& next =
            owners_tiles_edram_[(base + tile_target + count) & kTilesMask];
        if (next.image != &source ||
            uint32_t(next.tile_local) != source_start + count) {
          break;
        }
        ++count;
      }
      conversions.push_back({&source, source_start, tile_target, count});
      distinct_formats += count;
      tile_target += count - 1;
      continue;
    }
    const uint32_t pitch_source = PitchTilesEDRAM(source);
    const uint32_t tile_width_source = kTileWidth >> (uint32_t(source.edram_64bpp) + source.edram_msaa_x);
    const uint32_t tile_width_target = kTileWidth >> (uint32_t(target.edram_64bpp) + target.edram_msaa_x);
    const uint32_t sx = (uint32_t(owner.tile_local) % pitch_source) * tile_width_source;
    const uint32_t sy = (uint32_t(owner.tile_local) / pitch_source) * (kTileHeight >> source.edram_msaa_y);
    const uint32_t dx = (tile_target % pitch_target) * tile_width_target;
    const uint32_t dy = (tile_target / pitch_target) * tile_height_target;
    if (sx >= source.width || sy >= source.height || dx >= target.width || dy >= target.height) continue;
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {int32_t(sx), int32_t(sy), 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {int32_t(dx), int32_t(dy), 0};
    region.extent = {std::min(tile_width_target, std::min(source.width - sx, target.width - dx)),
                     std::min(tile_height_target, std::min(source.height - sy, target.height - dy)), 1};
    per_source[&source].push_back(region);
  }
  if (!per_source.empty()) {
    if (draws_) draws_->FinishPass();
    ForgetClear(target);
    BarrierBeforeCopyResolve();
    uint32_t regions_total = 0;
    for (auto& [source, regions] : per_source) {
      regions_total += uint32_t(regions.size());
      overlaps_tiles_ += regions.size();
      // The same pixels in fewer regions: NVK pays a fixed cost per VkImageCopy region, and a whole
      // 1280x720 rebind is 720 tiles of 80x16 (measured on the Switch: ~110 ms per frame in mode 2).
      // deko3d: each region is one copy-engine launch, so merging still saves launches.
      MergeRegionsCopy(regions);
      overlaps_regions_ += regions.size();
      HostCopies(*source, target, regions.data(), uint32_t(regions.size()));  // sa2-nx render resolution
      CopyRegionsDk(commands_work_, source->image, target.image, uint32_t(regions.size()), regions.data());
    }
    ++overlaps_copies_;
    BarrierAfterCopyResolve();
    const uint64_t label = (uint64_t(base) << 32) | uint32_t(target.format);
    if (overlaps_edram_recorded_.insert(label).second) {
      REXLOG_INFO("[native] targets: physical EDRAM base {:03X}: {} tiles from {} images",
                  base, regions_total, per_source.size());
    }
  }
  uint32_t tiles_converted = 0;
  bool conversion_failed = false;
  if (!conversions.empty()) {
    if (draws_) draws_->FinishPass();
    ForgetClear(target);
  }
  for (const ConversionRange& conversion : conversions) {
    ++overlaps_conversions_;
    overlaps_tiles_converted_ += conversion.count;
    if (ConvertAliasEDRAM(*conversion.source, target, conversion.tile_source,
                            conversion.tile_target, conversion.count)) {
      tiles_converted += conversion.count;
    } else {
      conversion_failed = true;
    }
  }
  if (distinct_formats) {
    const uint64_t label = (uint64_t(base) << 32) | 0xFFFFFFFFu;
    if (overlaps_edram_recorded_.insert(label).second) {
      REXLOG_INFO("[native] targets: physical EDRAM base {:03X}: {}/{} tiles converted between formats in {} ranges",
                  base, tiles_converted, distinct_formats, conversions.size());
    }
  }
  if (conversion_failed) {
    REXLOG_WARN("[native] targets: physical EDRAM base {:03X}: a conversion failed; the new owner is not published",
                base);
    return;
  }
  // This image now contains the newest known snapshot for the full physical range. Marking on bind is
  // conservative but correct: unchanged tiles were copied above, and later draws write into this image.
  for (uint32_t tile_target = 0; tile_target < tiles_target; ++tile_target) {
    if (!tile_used(tile_target)) continue;
    owners_tiles_edram_[(base + tile_target) & kTilesMask] =
        {&target, uint16_t(tile_target)};
  }
}

void TargetsDk::ActivateTargetColor(uint32_t base, uint32_t pitch, Image& target) {
  // Physical ownership is authoritative. An additional exact-base copy can
  // overwrite tiles that have since been written through another base.
  const int32_t mode = REXCVAR_GET(sa2_native_edram_alias_mode);
  if (!mode) return;
  if (mode == 4) return; // Strict common ownership is prepared outside the native draw pass.
  if (mode == 2 || mode == 3) {
    SynchronizeOverlapsEDRAM(base, target, mode == 3 ? &area_edram_ : nullptr);
    return;
  }
  const uint64_t key = base;
  auto [it, new_entry] = targets_color_active_.try_emplace(key, &target);
  Image* previous = it->second;
  if (new_entry || previous == &target) {
    return;
  }
  if (draws_) {
    draws_->FinishPass();
  }
  Prepare(*previous);
  Prepare(target);
  bool copied = false;
  // If format and pitch change together, first convert the complete old representation into the
  // destination format at the old pitch. It preserves the tiles outside the narrower view, which are
  // needed when the 1280-wide view is rebound after the 640-wide pass.
  const bool same_representation = previous->format == target.format &&
      previous->edram_format == target.edram_format &&
      previous->edram_64bpp == target.edram_64bpp;
  if (!same_representation && previous->width != target.width) {
    const uint64_t intermediate_key = (uint64_t(base) << 32) |
                                      (uint64_t(target.edram_format) << 24) |
                                      (uint64_t(uint32_t(target.format) & 0xFF) << 16) |
                                      previous->width;
    auto intermediate_it = targets_.find(intermediate_key);
    if (intermediate_it != targets_.end()) {
      Image& intermediate = intermediate_it->second;
      Prepare(intermediate);
      if (ConvertAliasEDRAM(*previous, intermediate)) {
        copied = CopyAliasEDRAMPitch(intermediate, target);
      }
    }
  }
  if (!copied && same_representation && previous->width != target.width) {
    copied = CopyAliasEDRAMPitch(*previous, target);
  }
  if (!copied && ConvertAliasEDRAM(*previous, target)) {
    copied = true;
  }
  if (copied) {
    ForgetClear(target);
    const uint64_t pair = (uint64_t(uint32_t(previous->format)) << 32) |
                            uint32_t(target.format);
    if (conversions_edram_recorded_.insert(pair).second) {
      REXLOG_INFO("[native] targets: exact EDRAM alias base {:03X}: {}/host {} -> {}/host {}", base,
                  previous->width, uint32_t(previous->format), target.width,
                  uint32_t(target.format));
    }
  } else if (sa2::dk::Has2D(previous->image) && sa2::dk::Has2D(target.image)) {  // Vulkan: blit_
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {HostEdge(*previous, int32_t(std::min(previous->width, target.width))),
                            HostEdge(*previous, int32_t(std::min(previous->height, target.height))), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {HostEdge(target, int32_t(std::min(previous->width, target.width))),
                            HostEdge(target, int32_t(std::min(previous->height, target.height))), 1};
    // Both aliases stay in GENERAL, so ending the render pass is not by itself a memory
    // dependency for this transfer. Publish the last attachment/transfer write before reading
    // the old representation, and publish the blit before the new representation is used as an
    // attachment or sampled texture. Without these barriers, Metal may observe an older EDRAM
    // alias: Mass Effect then alternates correctly shaded characters with flat-colour geometry.
    BarrierBeforeCopyResolve();
    BlitDk(commands_work_, previous->image, target.image, region, false);  // VK_FILTER_NEAREST
    BarrierAfterCopyResolve();
    ForgetClear(target);
    const uint64_t pair = (uint64_t(uint32_t(previous->format)) << 32) |
                            uint32_t(target.format);
    if (conversions_edram_recorded_.insert(pair).second) {
      REXLOG_INFO("[native] targets: alias EDRAM base {:03X}/{}: host {} -> {}", base, pitch,
                  uint32_t(previous->format), uint32_t(target.format));
    }
  } else {
    // deko3d: the 2D engine needs both images created with DkImageFlags_Usage2DEngine.
    Reject(4, "EDRAM alias between formats without vkCmdBlitImage");
  }
  it->second = &target;
}

bool TargetsDk::CopyAliasEDRAMPitch(Image& source, Image& target) {
  if (!kCopyImageDk || source.format != target.format ||
      source.edram_format != target.edram_format ||
      source.edram_64bpp != target.edram_64bpp || !source.width || !target.width) {
    return false;
  }
  const uint32_t kTileWidth = xenos::kEdramTileWidthSamples >> uint32_t(source.edram_64bpp);
  constexpr uint32_t kTileHeight = xenos::kEdramTileHeightSamples;
  const uint32_t pitch_source = (source.width + kTileWidth - 1) / kTileWidth;
  const uint32_t pitch_target = (target.width + kTileWidth - 1) / kTileWidth;
  const uint32_t rows_source = (source.height + kTileHeight - 1) / kTileHeight;
  const uint32_t rows_target = (target.height + kTileHeight - 1) / kTileHeight;
  const uint32_t tiles = std::min(pitch_source * rows_source, pitch_target * rows_target);
  std::vector<VkImageCopy> regions;
  regions.reserve(std::min<uint32_t>(tiles, 256));
  for (uint32_t tile = 0; tile < tiles;) {
    const uint32_t sx_tile = tile % pitch_source;
    const uint32_t sy_tile = tile / pitch_source;
    const uint32_t dx_tile = tile % pitch_target;
    const uint32_t dy_tile = tile / pitch_target;
    const uint32_t run = std::min({tiles - tile, pitch_source - sx_tile,
                                       pitch_target - dx_tile});
    const uint32_t sx = sx_tile * kTileWidth;
    const uint32_t sy = sy_tile * kTileHeight;
    const uint32_t dx = dx_tile * kTileWidth;
    const uint32_t dy = dy_tile * kTileHeight;
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {int32_t(sx), int32_t(sy), 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {int32_t(dx), int32_t(dy), 0};
    region.extent = {std::min({run * kTileWidth, source.width - sx,
                               target.width - dx}),
                     std::min({kTileHeight, source.height - sy, target.height - dy}), 1};
    if (region.extent.width && region.extent.height) {
      regions.push_back(region);
    }
    tile += run;
  }
  if (regions.empty()) {
    return false;
  }
  HostCopies(source, target, regions.data(), uint32_t(regions.size()));  // sa2-nx render resolution
  BarrierBeforeCopyResolve();
  CopyRegionsDk(commands_work_, source.image, target.image, uint32_t(regions.size()), regions.data());
  BarrierAfterCopyResolve();
  return true;
}

// ds2-nx (ds2_resolve_7e3_direct): the 7e3 view that owns every tile of `area` of the UNORM10 view `view` with the
// same tile layout (same base, pitch, sample layout, scale), or null.
Image* TargetsDk::Owner7e3EDRAM4(const Image& view, const VkRect2D& area) {
  RangeTiles4 range;
  if (view.edram_depth || view.edram_64bpp || !RangeTilesEDRAM4(view, area, range)) return nullptr;
  Image* owner_7e3 = nullptr;
  for (uint32_t ty = range.ty0; ty < range.ty1; ++ty) {
    for (uint32_t tx = range.tx0; tx < range.tx1; ++tx) {
      const uint32_t tile = ty * range.pitch + tx;
      const auto owner = owners_tiles_edram4_[(view.edram_base + tile) & 2047u];
      if (!owner.image || owner.tile_local != tile) return nullptr;
      if (!owner_7e3) owner_7e3 = owner.image;
      else if (owner_7e3 != owner.image) return nullptr;
    }
  }
  const Image* w = owner_7e3;
  if (!w || w == &view || w->edram_depth || w->edram_64bpp || w->edram_format != 3 ||
      w->edram_base != view.edram_base || PitchTilesEDRAM(*w) != range.pitch ||
      w->edram_msaa_x != view.edram_msaa_x || w->edram_msaa_y != view.edram_msaa_y ||
      w->raster_grid_x || view.raster_grid_x || w->scale_q != view.scale_q || !w->prepared ||
      (w->format != VK_FORMAT_R16G16B16A16_SFLOAT && w->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) ||
      w->guest_width || w->guest_height) return nullptr;
  return owner_7e3;
}

bool TargetsDk::ResolverWithBias(Image& source, Image& target, int32_t sx, int32_t sy,
                                 uint32_t dx, uint32_t dy, uint32_t width, uint32_t height,
                                 int32_t exp_bias, uint32_t pipeline_override) {
  // ds2-nx (ds2_resolve_bias_rgba8): an RGBA8 destination too, through the same shader with an rgba8 image
  // (pipeline 9). Dark Souls II's water refraction (10951000) is the HDR scene resolved to 8888 with bias -2; it was
  // rejected (cause 14) and the stream in the Forest of Fallen Giants came out white.
  const bool to_rgba8 = REXCVAR_GET(ds2_resolve_bias_rgba8) && target.format == VK_FORMAT_R8G8B8A8_UNORM;
  // ds2-nx: pipeline 10 (ds2_resolve_7e3_direct) writes an A2B10G10R10 target.
  const bool to_unorm10 = pipeline_override == 10 && target.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  const bool source_r11 = source.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;  // ds2_7e3_r11g11b10
  if ((source.format != VK_FORMAT_R16G16B16A16_SFLOAT && !source_r11) ||
      (target.format != VK_FORMAT_R16G16B16A16_SFLOAT && !to_rgba8 && !to_unorm10) ||
      source.image == target.image || !width || !height) return false;
  if (pipeline_override && source.scale_q != target.scale_q) return false;
  uint32_t index_bias = pipeline_override ? pipeline_override : to_rgba8 ? 9 : 3;
  if (source_r11) index_bias = index_bias == 10 ? 15 : index_bias == 9 ? 14 : index_bias == 3 ? 13 : UINT32_MAX;
  if (index_bias >= pipelines_conversion_edram_.size()) return false;
  const DkShader* const pipeline_bias = pipelines_conversion_edram_[index_bias];
  if (!pipeline_bias ||
      !EnsureCapacityConversionEDRAM4("resolve bias")) return false;
  WorkSlot& slot = slots_[slot_];
  // Vulkan: one descriptor set with two storage images from the slot's pool. deko3d: two load/store descriptors of
  // the slot's transient ring.
  const uint32_t descriptor_source = PushDescriptor(sa2::dk::View(source.image), true);
  const uint32_t descriptor_target = PushDescriptor(sa2::dk::View(target.image), true);
  if (descriptor_source == UINT32_MAX || descriptor_target == UINT32_MAX) return false;
  ++slot.conversions_edram;
  // Order previous sampling as well as attachment/resolve writes against this
  // replacement. Keep the dependency outside render passes.
  sa2::dk::Barrier(commands_work_, Hazard::AnyToCompute);
  // sa2-nx render resolution: the rectangles in host pixels (each image at its own scale).
  const VkRect2D host_source = HostRect(source, VkRect2D{{sx, sy}, {width, height}});
  const VkRect2D host_target = HostRect(target, VkRect2D{{int32_t(dx), int32_t(dy)}, {width, height}});
  const uint32_t host_width = std::min(host_source.extent.width, host_target.extent.width);
  const uint32_t host_height = std::min(host_source.extent.height, host_target.extent.height);
  const struct {
    int32_t source_offset[2];
    int32_t destination_offset[2];
    uint32_t extent[2];
    int32_t exponent_bias;
  } constants = {{host_source.offset.x, host_source.offset.y}, {host_target.offset.x, host_target.offset.y},
                 {host_width, host_height}, exp_bias};
  static_assert(sizeof(constants) == 28);
  const DkGpuAddr constants_gpu = PushConstants(&constants, sizeof(constants));
  if (constants_gpu == DK_GPU_ADDR_INVALID) return false;
  const DkResHandle images[2] = {sa2::dk::Handle(descriptor_source, 0), sa2::dk::Handle(descriptor_target, 0)};
  dkCmdBufBindImages(commands_work_, DkStage_Compute, 0, images, 2);
  DispatchDk(commands_work_, pipeline_bias, constants_gpu, sizeof(constants), (host_width + 7) / 8,
             (host_height + 7) / 8);
  sa2::dk::Barrier(commands_work_, Hazard::ComputeToAny);
  return true;
}

DkImageView TargetsDk::GetViewRaw64EDRAM4(Image& image) {
  // Vulkan: a VkImageView in R16G16B16A16_UINT (sampled + storage), cached per VkImage. deko3d: a view of the same
  // image in RGBA16_Uint (same 8 bytes per texel), made on the spot; its descriptor is pushed by the caller.
  if (!raw64_edram_supported_ || image.format != VK_FORMAT_R16G16B16A16_SFLOAT) return DkImageView{};
  return sa2::dk::View(image.image, DkImageFormat_RGBA16_Uint);
}

TargetsDk::ViewsDepthEDRAM TargetsDk::GetViewsDepthEDRAM4(Image& source) {
  // Vulkan: two VkImageViews (DEPTH and STENCIL aspects), cached per VkImage. deko3d: the depth source and the
  // stencil source of the same image.
  ViewsDepthEDRAM views;
  views.depth = sa2::dk::View(source.image, DkImageFormat_None, DkDsSource_Depth);
  if (!views.depth.pImage) return ViewsDepthEDRAM{};
  views.stencil = sa2::dk::View(source.image, DkImageFormat_None, DkDsSource_Stencil);
  return views;
}

bool TargetsDk::IsDepthNative2xEDRAM4(const Image& image) {
  return image.sample_count == VK_SAMPLE_COUNT_2_BIT && image.edram_depth &&
      image.format == VK_FORMAT_D32_SFLOAT_S8_UINT && !image.edram_64bpp &&
      !image.guest_width && !image.guest_height && image.raster_grid_x == 0 &&
      image.edram_msaa_x == 0 && image.edram_msaa_y == 1 && image.edram_format <= 1 &&
      (!image.depth_float24_half || image.edram_format == 1);
}

bool TargetsDk::IsDepthNative1xEDRAM4(const Image& image) {
  // Actual 1x AND guest physical 1x, not a legacy collapsed 2x/4x image.
  return image.sample_count == VK_SAMPLE_COUNT_1_BIT && image.edram_depth &&
      image.format == VK_FORMAT_D32_SFLOAT_S8_UINT && !image.edram_64bpp &&
      !image.guest_width && !image.guest_height && image.raster_grid_x == 0 &&
      image.edram_msaa_x == 0 && image.edram_msaa_y == 0 && image.edram_format <= 1 &&
      (!image.depth_float24_half || image.edram_format == 1);
}

bool TargetsDk::AcceptsImportDepthNative2xEDRAM4(VkFormat format) {
  if (format != VK_FORMAT_D32_SFLOAT_S8_UINT) return false;
  if (native2x_supported_ >= 0) return native2x_supported_ != 0;  // device capabilities do not change
  native2x_supported_ = AcceptsImportDepthNative2xNoCacheEDRAM4(format) ? 1 : 0;
  return native2x_supported_ != 0;
}

bool TargetsDk::AcceptsImportDepthNative2xNoCacheEDRAM4(VkFormat format) {
  // Vulkan: sampleRateShading, standardSampleLocations, 2x in the four depth/stencil sample-count masks, and
  // vkGetPhysicalDevice(Image)FormatProperties for D32S8 as a 2x sampled depth attachment.
  // deko3d: ZF32_X24S8 renders and samples at 2x (format table), the sample positions are the standard ones
  // (dkMultisampleStateDefaults), and there is no sample-rate shading state: the 2x shaders run per sample because
  // they read gl_SampleID. Answered as supported for D32S8; to be confirmed on the console (report, "o que testar").
  return format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

TargetsDk::PassImportDepthEDRAM* TargetsDk::EnsureImportDepthEDRAM4(VkFormat format,
    VkSampleCountFlagBits sample_count, VkSampleCountFlagBits source_sample_count) {
  // sa2_msaa: a single-sample source imported into a host multisampled game target (every sample of a pixel gets
  // the pixel's value, as the target's single guest sample).
  const bool host_msaa = msaa_samples_ > 1 && uint32_t(sample_count) == msaa_samples_ &&
                         me::native::IsSingleSample(uint32_t(source_sample_count));
  const bool native2x = sample_count == VK_SAMPLE_COUNT_2_BIT && !host_msaa;
  const bool source_msaa2 = source_sample_count == VK_SAMPLE_COUNT_2_BIT;
  if ((!me::native::IsSingleSample(uint32_t(sample_count)) && !native2x && !host_msaa) ||
      (!me::native::IsSingleSample(uint32_t(source_sample_count)) && !source_msaa2) ||
      (native2x && !source_msaa2) ||
      (source_msaa2 && !AcceptsImportDepthNative2xEDRAM4(format))) return nullptr;
  if (import_depth_edram_failed_) return nullptr;
  const auto fail = [&]() -> PassImportDepthEDRAM* {
    import_depth_edram_failed_ = true;
    return nullptr;
  };
  // Vulkan: the descriptor set layout (2 combined samplers), the pipeline layout (80 bytes of fragment push
  // constants), the shader modules and one descriptor pool per slot. deko3d: only the shaders.
  if (!vs_import_depth_edram_) {
    vs_import_depth_edram_ = LoadShader(dkshaders::kMeEdramImport);
    fs_import_depth_edram_ = LoadShader(dkshaders::kMeEdramColorToDepth);
    fs_raw64_to_depth_edram_ = LoadShader(dkshaders::kMeEdramRaw64ToDepth);
    fs_depth_to_depth_edram_ = LoadShader(dkshaders::kMeEdramDepthToDepth);
    fs_depth_to_stencil_edram_ = LoadShader(dkshaders::kMeEdramDepthToStencil);
    fs_color_to_stencil_edram_ = LoadShader(dkshaders::kMeEdramColorToStencil);
    if (!vs_import_depth_edram_ || !fs_import_depth_edram_ || !fs_raw64_to_depth_edram_ ||
        !fs_depth_to_depth_edram_ || !fs_depth_to_stencil_edram_ || !fs_color_to_stencil_edram_)
      return fail();
  }
  if (native2x && !fs_stencil_msaa2_edram_) {
    fs_stencil_msaa2_edram_ = LoadShader(dkshaders::kMeEdramDepthToStencilMsaa2);
    if (!fs_stencil_msaa2_edram_) return fail();
  }
  if (source_msaa2 && !native2x && !fs_stencil_msaa2_to_1x_edram_) {
    fs_stencil_msaa2_to_1x_edram_ = LoadShader(dkshaders::kMeEdramDepthMsaa2ToStencil1x);
    if (!fs_stencil_msaa2_to_1x_edram_) return fail();
  }
  if (native2x && !fs_depth_to_depth_msaa2_edram_) {
    fs_depth_to_depth_msaa2_edram_ = LoadShader(dkshaders::kMeEdramDepthToDepthMsaa2);
    if (!fs_depth_to_depth_msaa2_edram_) return fail();
  }
  if (source_msaa2 && !native2x && !fs_depth_msaa2_to_depth_1x_edram_) {
    fs_depth_msaa2_to_depth_1x_edram_ = LoadShader(dkshaders::kMeEdramDepthMsaa2ToDepth1x);
    if (!fs_depth_msaa2_to_depth_1x_edram_) return fail();
  }
  // Source type is part of the PSO identity: 1x->1x and 2x->1x both have a
  // 1x attachment but use incompatible sampler2D / sampler2DMS shaders.
  const uint64_t pass_key = uint64_t(uint32_t(format)) | (uint64_t(sample_count) << 32) |
                            (uint64_t(source_sample_count) << 40);
  auto [it, new_value] = passes_import_depth_edram_.try_emplace(pass_key);
  PassImportDepthEDRAM& pass = it->second;
  if (!new_value) return pass.valid && pass.pipelines[2] && pass.pipelines[3] &&
      (source_msaa2 || (pass.pipelines[0] && pass.pipelines[1] && pass.pipelines[4] && pass.pipelines[5]))
      ? &pass : nullptr;
  pass.key = pass_key;
  // Vulkan: the render pass (one depth/stencil attachment, LOAD/STORE, GENERAL, two external dependencies: see
  // ImportColorDepthEDRAM4) and the pipelines' fixed state: triangle list, no cull, the attachment's samples with
  // sample shading at 2x, depth ALWAYS, stencil ALWAYS / REPLACE (KEEP on fail), compare mask 255, the stencil
  // reference and write mask dynamic (dkCmdBufSetStencil).
  DkDepthStencilState& depth = pass.state.depth;
  depth.depthTestEnable = depth.depthWriteEnable = true;
  depth.depthCompareOp = DkCompareOp_Always;
  depth.stencilTestEnable = true;
  depth.stencilFrontFailOp = depth.stencilFrontDepthFailOp = DkStencilOp_Keep;
  depth.stencilFrontPassOp = DkStencilOp_Replace;
  depth.stencilFrontCompareOp = DkCompareOp_Always;
  depth.stencilBackFailOp = depth.stencilFrontFailOp;
  depth.stencilBackDepthFailOp = depth.stencilFrontDepthFailOp;
  depth.stencilBackPassOp = depth.stencilFrontPassOp;
  depth.stencilBackCompareOp = depth.stencilFrontCompareOp;
  // deko3d: no sampleShading state; at 2x the shaders read gl_SampleID, which runs them per sample.
  pass.state.ms.mode = pass.state.ms.rasterizerMode = sa2::dk::FromSamples(uint32_t(sample_count));
  for (uint32_t i = 0; i < 6; ++i) {
    if (source_msaa2 && (i < 2 || i >= 4)) continue;  // Only same-format depth sources.
    // deko3d: the depth test and write of pipeline i ((i & 1) == 0) are set per step in ImportColorDepthEDRAM4.
    const DkShader* module = source_msaa2 ? (native2x ? fs_depth_to_depth_msaa2_edram_ :
        fs_depth_msaa2_to_depth_1x_edram_) : i < 2 ? fs_import_depth_edram_ :
        i < 4 ? fs_depth_to_depth_edram_ : fs_raw64_to_depth_edram_;
    // Stencil-bit pipelines (odd): a shader that only fetches the stencil byte and discards, without
    // gl_FragDepth (which forced late tests and the full depth path on every one of the 8 passes).
    if ((i & 1) && REXCVAR_GET(sa2_native_edram4_stencil_light))
      module = source_msaa2 ? (native2x ? fs_stencil_msaa2_edram_ : fs_stencil_msaa2_to_1x_edram_)
          : i == 1 ? fs_color_to_stencil_edram_ : i == 3 ? fs_depth_to_stencil_edram_ : module;
    if (!module) return fail();
    pass.pipelines[i] = module;
  }
  pass.valid = true;
  return &pass;
}

bool TargetsDk::EnsureStencilCopyEDRAM4() {
  // Vulkan 7232-7297: the stencil-to-buffer compute pipelines (me_edram_stencil_to_buffer, _msaa2,
  // me_edram_color_stencil_to_buffer), a 2 MB device-local buffer and a descriptor pool per slot, for
  // vkCmdCopyBufferToImage into the STENCIL aspect.
  // deko3d: a buffer-to-image copy writes whole texels: ZF32_X24S8 / Z24S8 have no stencil-only copy, and a second
  // DkImage of another format over the same memory would remap its page kind (dk_memblock.cpp). Never available:
  // copy_stencil stays false and the imports take the 8 masked stencil passes (decided for the fase C).
  if (stencil_failed_copy_) return false;
  stencil_failed_copy_ = true;
  REXLOG_WARN("[native] [dk] copy-engine stencil import unavailable: the masked stencil passes import it");
  return false;
}

bool TargetsDk::CopyStencilEDRAM4(Image& source, Image& target, uint32_t source_start, uint32_t target_start,
                                  uint32_t count, const VkRect2D& guest_rect) {
  // Never queued: copy_stencil needs EnsureStencilCopyEDRAM4, which is false on deko3d (see there). A call here is a
  // logic error upstream: reported as a failed import (the caller logs FailureEDRAM4).
  (void)source;
  (void)target;
  (void)source_start;
  (void)target_start;
  (void)count;
  (void)guest_rect;
  return false;
}

bool TargetsDk::ImportColorDepthEDRAM4(Image& source, Image& target,
                                       uint32_t source_start, uint32_t target_start, uint32_t count) {
  const bool source_depth = source.edram_depth;
  const bool source_raw64 = source.edram_64bpp;
  const bool native_msaa_import = IsDepthNative2xEDRAM4(source) &&
      (IsDepthNative2xEDRAM4(target) || IsDepthNative1xEDRAM4(target));
  // sa2_msaa: a single-sample source into a host multisampled target is an ordinary import (N-sample pass). A host
  // multisampled source has no import here (it would need a sample-reading shader): declined quietly, and the
  // caller skips the transfer like any unsupported one (banjo_native_edram4_skip_unsupported).
  const bool host_msaa_target = me::native::IsSingleSample(uint32_t(source.sample_count)) && msaa_samples_ > 1 &&
                                uint32_t(target.sample_count) == msaa_samples_;
  if (msaa_samples_ > 1 && uint32_t(source.sample_count) == msaa_samples_ && !IsDepthNative2xEDRAM4(source))
    return false;
  if ((!me::native::IsSingleSample(uint32_t(source.sample_count)) ||
       !me::native::IsSingleSample(uint32_t(target.sample_count))) && !host_msaa_target) {
    if (!native_msaa_import || source.edram_format != target.edram_format ||
        source.depth_float24_half != target.depth_float24_half || source.image == target.image)
      return FailureEDRAM4("native2x import requires distinct same-format depth views", target, &source);
    // The shader also discards invalid contracts, but a discard cannot be
    // reported as a successful physical transfer / ownership publication.
    if (!count || source_start >= 2048 || target_start >= 2048 ||
        count > 2048 - source_start || count > 2048 - target_start)
      return FailureEDRAM4("native2x import tile range exceeds physical EDRAM", target, &source);
  }
  const DkImageView raw_source = source_raw64 ? GetViewRaw64EDRAM4(source) : DkImageView{};
  if (source_raw64 && (!raw_source.pImage || source.edram_format != 7))
    return FailureEDRAM4("raw FP16 depth import UINT view unavailable", target, &source);
  if (!target.edram_depth ||
      source.guest_width || target.guest_width || !count || target.edram_format > 1 ||
      (source_depth ? source.edram_format > 1 :
        (!source_raw64 && source.edram_format != 0 && source.edram_format != 2 && source.edram_format != 3)))
    return FailureEDRAM4("depth import source class / scaled view unsupported", target, &source);
  const uint32_t source_pitch = PitchTilesEDRAM(source);
  if (native_msaa_import && (source_pitch > 2048 || PitchTilesEDRAM(target) > 2048))
    return FailureEDRAM4("native2x import pitch exceeds physical EDRAM", target, &source);
  const uint32_t source_tile_width = 80u >> (source.edram_msaa_x + uint32_t(source_raw64));
  const uint32_t source_tile_height = 16u >> source.edram_msaa_y;
  if (!source_pitch || source.width % source_tile_width ||
      (source_start + count - 1) / source_pitch * source_tile_height + source_tile_height > source.height)
    return FailureEDRAM4("color->depth missing source sample/padding", target, &source);
  const uint32_t destination_pitch = PitchTilesEDRAM(target);
  if (!destination_pitch || !count)
    return FailureEDRAM4("depth import empty destination tile range", target, &source);
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
  if (sx >= target.width || sy >= target.height || right <= sx || bottom <= sy)
    return FailureEDRAM4("depth import destination tile range outside attachment", target, &source);
  const VkRect2D scissor{{int32_t(sx), int32_t(sy)}, {right - sx, bottom - sy}};
  // sa2-nx render resolution: the pass works in host pixels (both views at the same scale).
  const VkRect2D host_scissor = HostRect(target, scissor);
  if (source.scale_q != target.scale_q)
    return FailureEDRAM4("depth import between images of different render scales", target, &source);
  PassImportDepthEDRAM* pass = EnsureImportDepthEDRAM4(target.format, target.sample_count,
                                                      source.sample_count);
  if (!pass || sampler_depth_edram_ == UINT32_MAX)
    return FailureEDRAM4("color->depth graphics resources unavailable", target, &source);
  // Vulkan: a framebuffer per target VkImage (framebuffers_import_depth_edram_), kept as a handle across the
  // capacity helper. deko3d: the depth view is bound on the spot; the batch compares the image id (import_fb).
  const sa2::dk::ImageId import_framebuffer = target.image;
  if (!EnsureCapacityConversionEDRAM4("depth import")) return false;
  WorkSlot& slot = slots_[slot_];
  // Vulkan: one descriptor set of 1 or 2 combined image samplers. deko3d: transient descriptors of the slot,
  // sampled with sampler_depth_edram_ (nearest, clamp).
  const ViewsDepthEDRAM depth_views = source_depth ? GetViewsDepthEDRAM4(source) : ViewsDepthEDRAM{};
  if (source_depth && !depth_views.depth.pImage) return false;
  const uint32_t descriptor_source = PushDescriptor(
      source_depth ? depth_views.depth : source_raw64 ? raw_source : sa2::dk::View(source.image));
  const uint32_t descriptor_stencil = source_depth ? PushDescriptor(depth_views.stencil) : 0u;
  if (descriptor_source == UINT32_MAX || descriptor_stencil == UINT32_MAX) return false;
  ++slot.conversions_edram;
  struct Constants {
    uint32_t source_size[2], target_size[2];
    uint32_t pitch_source, pitch_target, source_start, target_start, count;
    uint32_t source_format, target_format, source_64bpp, target_64bpp;
    uint32_t source_msaa_x, source_msaa_y, target_msaa_x, target_msaa_y;
    uint32_t stencil_mask;
    uint32_t tile_width, tile_height;  // sa2-nx render resolution: the tile in host samples
  } constants{{HostWidth(source), HostHeight(source)}, {HostWidth(target), HostHeight(target)},
              PitchTilesEDRAM(source), PitchTilesEDRAM(target), source_start, target_start, count,
              uint32_t(source.edram_format) | (source.depth_float24_half ? 256u : 0u),
              uint32_t(target.edram_format) | (target.depth_float24_half ? 256u : 0u),
              uint32_t(source_raw64), 0, source.edram_msaa_x, source.edram_msaa_y,
              target.edram_msaa_x, target.edram_msaa_y, 0, ScaledTileWidth(target.scale_q),
              ScaledTileHeight(target.scale_q)};
  static_assert(sizeof(Constants) == 80);
  if (draws_) draws_->FinishPass();
  const bool in_batch = edram4_batch_.active;
  if (in_batch) CloseComputeBatchEDRAM4();
  const bool continue_pass = in_batch && edram4_batch_.import_open &&
      edram4_batch_.import_pass == pass->key && edram4_batch_.import_fb == import_framebuffer;
  if (in_batch && !continue_pass && !CloseImportBatchEDRAM4()) return false;
  // The render pass's external dependencies (EnsureImportDepthEDRAM4) already make the needed barriers;
  // on NVK every explicit barrier is a GPU idle wait plus cache flushes, ~100 small imports per frame.
  // deko3d: the pass start below is that dependency 0 (one barrier per opened pass, none for a continued one).
  if (continue_pass) {
    ++edram4_batch_saved_passes_;
  } else {
  // Vulkan: vkCmdBeginRenderPass with the render area = the tiles' rectangle (sa2_native_edram4_import_area_tiles),
  // or the batch's union of them. deko3d has no render area: the scissor below bounds every fragment, which is what
  // the narrowed area bought on NVK.
  sa2::dk::Barrier(commands_work_, HazardPassStartDk());
  const DkImageView depth_target = sa2::dk::View(target.image);
  dkCmdBufBindRenderTargets(commands_work_, nullptr, 0, &depth_target);
  pass->state.Bind(commands_work_);
  BindNoVerticesDk(commands_work_);
  if (in_batch) {
    edram4_batch_.import_open = true;
    edram4_batch_.import_pass = pass->key;
    edram4_batch_.import_fb = import_framebuffer;
  }
  }
  // The procedural triangle covers the viewport: make it the tiles' rectangle, so each of the up to 9
  // passes rasterizes only those tiles (the shaders use gl_FragCoord, not the viewport). With a
  // whole-target viewport every pass of a 1-tile import cost ~0.4 ms on the Switch.
  sa2::dk::SetViewport(commands_work_, REXCVAR_GET(sa2_native_edram4_import_area_tiles)
                                           ? host_scissor
                                           : VkRect2D{{0, 0}, {HostWidth(target), HostHeight(target)}});
  const DkScissor scissor_dk = ScissorDk(host_scissor);
  dkCmdBufSetScissors(commands_work_, 0, &scissor_dk, 1);
  dkCmdBufBindTexture(commands_work_, DkStage_Fragment, 0, sa2::dk::Handle(descriptor_source, sampler_depth_edram_));
  if (source_depth)
    dkCmdBufBindTexture(commands_work_, DkStage_Fragment, 1, sa2::dk::Handle(descriptor_stencil, sampler_depth_edram_));
  uint32_t steps = 9;  // depth + 8 stencil bits
  bool keep_stencil = false;
  const bool stencil_replaced = edram4_stencil_replacement_ &&
      scissor.offset.x >= edram4_stencil_replacement_->offset.x &&
      scissor.offset.y >= edram4_stencil_replacement_->offset.y &&
      int64_t(scissor.offset.x) + scissor.extent.width <=
          int64_t(edram4_stencil_replacement_->offset.x) + edram4_stencil_replacement_->extent.width &&
      int64_t(scissor.offset.y) + scissor.extent.height <=
          int64_t(edram4_stencil_replacement_->offset.y) + edram4_stencil_replacement_->extent.height;
  if (stencil_replaced) {
    steps = 1;  // the draw about to run replaces all of this stencil: only depth must arrive
    StencilRealEDRAM4(target, target_start, count);
    ++edram4_stencil_replaced_;
  } else if (REXCVAR_GET(sa2_native_edram4_stencil_lazy)) {
    const auto source_source = edram4_stencil_source_.find(&source);
    const auto source_for = [&](uint32_t i) -> SourceStencil {
      const uint32_t local = (source_start + i) & 2047u;
      if (source_source != edram4_stencil_source_.end() && source_source->second[local].image)
        return source_source->second[local];
      return {&source, uint16_t(local)};
    };
    // Per draw: a draw that does not test or write stencil gets depth only; the stencil is fetched later,
    // for the tiles a stencil-using draw actually binds (FetchStencilDeferredEDRAM4). Clears and resolves
    // keep the per-view rule.
    // An inert stencil (test ALWAYS on both faces, no write) is no stencil use either.
    const uint32_t dc_draw = edram4_submission_ && edram4_submission_->registers
        ? edram4_submission_->registers[gr::XE_GPU_REG_RB_DEPTHCONTROL] : 1u;
    const bool stencil_inert = REXCVAR_GET(sa2_native_edram4_stencil_inert) &&
        ((dc_draw >> 8) & 7) == 7 && (!(dc_draw & 0x80) || ((dc_draw >> 20) & 7) == 7) &&
        !edram4_draw_writes_stencil_;
    const bool draw_no_stencil = edram4_reason_ == 0 && edram4_submission_ && edram4_submission_->registers &&
        edram4_slot_ == 0 && (!(dc_draw & 1) || stencil_inert);
    if (edram4_import_stencil_only_) {
      // explicit late stencil fetch: no decision here
    } else if (!target.edram4_usa_stencil || draw_no_stencil) {
      auto& sources = StencilSourcesWrite(&target);
      if (sources.empty()) sources.resize(2048);
      for (uint32_t i = 0; i < count; ++i) sources[(target_start + i) & 2047u] = source_for(i);
      steps = 1;
      ++edram4_stencil_skipped_;
    } else if (StencilDeferredEDRAM4(source, source_start, count)) {
      bool own = true;
      for (uint32_t i = 0; i < count && own; ++i) {
        const SourceStencil f = source_for(i);
        own = f.image == &target && f.tile == ((target_start + i) & 2047u);
      }
      if (own) {
        steps = 1;
        keep_stencil = true;  // the real stencil never left the destination
        ++edram4_stencil_returned_;
      } else {
        ++edram4_stencil_inexact_;  // stencil lived elsewhere: the 8 passes copy the view's stale bits
      }
      StencilRealEDRAM4(target, target_start, count);
    } else {
      StencilRealEDRAM4(target, target_start, count);
    }
  }
  uint32_t first_step = 0;
  // 1x TRANSFER_DST destination with a depth source: the stencil goes through the copy engine after the
  // pass (CopyStencilEDRAM4) instead of 8 masked draws; the pass keeps only the depth draw.
  // Color sources too (RGBA8 / UNORM10 / 7e3 classes, 1x): the stencil is the color word's low byte.
  // deko3d: EnsureStencilCopyEDRAM4 is false (no stencil-only copy), so this is always false: the masked passes.
  const bool copy_stencil = REXCVAR_GET(sa2_native_edram4_stencil_copy) &&
      (source_depth || (source.edram_format == 0 || source.edram_format == 2 || source.edram_format == 3)) &&
      !source_raw64 && (steps > 1 || edram4_import_stencil_only_) &&
      me::native::IsSingleSample(uint32_t(target.sample_count)) && target.accepts_target_of_copy &&
      !target.edram_msaa_x && !target.edram_msaa_y && !target.raster_grid_x &&
      (me::native::IsSingleSample(uint32_t(source.sample_count)) ||
       (source.sample_count == VK_SAMPLE_COUNT_2_BIT && source.edram_msaa_y == 1 && !source.edram_msaa_x)) &&
      (source_depth || me::native::IsSingleSample(uint32_t(source.sample_count))) &&
      host_scissor.extent.width % 4 == 0 &&
      uint64_t(host_scissor.extent.width) * host_scissor.extent.height <= kBufferStencilCopy &&
      // Small imports: the copy path adds 3D->compute->copy->3D switches (each a GPU wait-for-idle on NVK),
      // which cost more than a few masked stencil draws inside the same 3D pass.
      count >= uint32_t(REXCVAR_GET(sa2_native_edram4_stencil_copy_min_tiles)) &&
      EnsureStencilCopyEDRAM4();
  if (copy_stencil) {
    steps = edram4_import_stencil_only_ ? 0 : 1;
  } else if (edram4_import_stencil_only_) {
    steps = 9;
    first_step = 1;  // depth stays: zero just the stencil of the tiles, then OR the 8 bits in
    // One rect per tile-row run: a multi-row range has a full-width scissor that also covers tiles
    // before/after the run, whose stencil must survive.
    // deko3d: vkCmdClearAttachments(STENCIL) -> a stencil-only dkCmdBufClearDepthStencil per rectangle, each under
    // its own scissor (the 3D clear is scissored); the import's scissor comes back after.
    std::vector<VkRect2D> rects;
    const uint32_t dp = PitchTilesEDRAM(target);
    const uint32_t tw = 80u >> target.edram_msaa_x, th = 16u >> target.edram_msaa_y;
    for (uint32_t t = 0; t < count && dp;) {
      const uint32_t tile = target_start + t, col = tile % dp;
      const uint32_t n = std::min(count - t, dp - col);
      const uint32_t x = (col * tw) << target.raster_grid_x, y = (tile / dp) * th;
      if (x < target.width && y < target.height)
        rects.push_back(HostRect(target, VkRect2D{{int32_t(x), int32_t(y)},
                                                   {std::min((n * tw) << target.raster_grid_x, target.width - x),
                                                    std::min(th, target.height - y)}}));
      t += n;
    }
    for (const VkRect2D& rect : rects) {
      const DkScissor clear_scissor = ScissorDk(rect);
      dkCmdBufSetScissors(commands_work_, 0, &clear_scissor, 1);
      dkCmdBufClearDepthStencil(commands_work_, false, 0.0f, 0xFF, 0);
    }
    if (!rects.empty()) dkCmdBufSetScissors(commands_work_, 0, &scissor_dk, 1);
  }
  if (steps > 1) {
    MarkGpu(kGpuEdramImport9);
    auto& n = edram4_import9_pairs_[fmt::format("{}{:03X}:{}x{}:mx{}my{}->{:03X}:{}x{}:mx{}my{}{}",
        source.edram_depth ? "D" : "C", source.edram_base, source.width, source.height, source.edram_msaa_x,
        source.edram_msaa_y, target.edram_base, target.width, target.height, target.edram_msaa_x,
        target.edram_msaa_y, StencilDeferredEDRAM4(source, source_start, count) ? " deferred" : "")];
    ++n.first;
    n.second += count;
  }
  const uint8_t bits_source = source_depth && !source_raw64 ? source.edram4_bits_stencil : uint8_t(255);
  target.edram4_bits_stencil |= bits_source;
  const bool skip_bits = REXCVAR_GET(sa2_native_edram4_bits_stencil);
  for (uint32_t step = first_step; step < steps; ++step) {
    if (step && skip_bits && !(bits_source & (1u << (step - 1)))) {
      ++edram4_bits_skipped_;
      continue;  // this bit is 0 everywhere in the source: pass 0 / the clear already wrote it
    }
    constants.stencil_mask = step ? (1u << (step - 1)) : 0u;
    const uint32_t mask = step ? constants.stencil_mask : (keep_stencil ? 0u : 255u);
    const uint32_t index = (source_depth ? 2u : source_raw64 ? 4u : 0u) + (step != 0);
    // vkCmdBindPipeline: the pipeline's fragment shader and its depth test / write (even pipelines only).
    DkDepthStencilState depth = pass->state.depth;
    depth.depthTestEnable = depth.depthWriteEnable = (index & 1) == 0;
    dkCmdBufBindDepthStencilState(commands_work_, &depth);
    // vkCmdSetStencilReference + vkCmdSetStencilWriteMask (compare mask 255 as in the pipeline).
    dkCmdBufSetStencil(commands_work_, DkFace_FrontAndBack, uint8_t(mask), uint8_t(constants.stencil_mask), 0xFF);
    // One uniform buffer address per draw: the mask changes between the steps.
    const DkGpuAddr constants_gpu = PushConstants(&constants, sizeof(constants));
    if (constants_gpu == DK_GPU_ADDR_INVALID)
      return FailureEDRAM4("depth import constants do not fit the slot", target, &source);
    DrawPassDk(commands_work_, vs_import_depth_edram_, pass->pipelines[index], constants_gpu, sizeof(constants));
  }
  if (in_batch) {
    // The pass stays open for the next run; the stencil copy follows when the batch closes it.
    if (copy_stencil)
      edram4_batch_.copies_stencil.push_back({&source, &target, source_start, target_start, count, scissor});
  } else {
  // vkCmdEndRenderPass: nothing to end. The depth written through gl_FragDepth leaves ZCULL stale (DepthWrite).
  sa2::dk::Barrier(commands_work_, Hazard::DepthWrite);
  if (copy_stencil && !CopyStencilEDRAM4(source, target, source_start, target_start, count, scissor))
    return FailureEDRAM4("copy-engine stencil import failed", target, &source);
  if (draws_) draws_->NotifyGraphicsExternalState();
  }
  if (++edram4_imports_ <= 32)
    REXLOG_INFO("[native] EDRAM mode4 {}->depth {:03X}/{} to {:03X}/{} tiles {}:{} count {} "
                "(9-pass stencil fallback; hardware samples {}->{}, nativeMSimport={})",
                source_depth ? "depth" : "color", source.edram_base, source.edram_format,
                target.edram_base, target.edram_format,
                source_start, target_start, count,
                uint32_t(source.sample_count), uint32_t(target.sample_count), native_msaa_import);
  if (target.edram_format == 1 && !target.depth_float24_half && !edram4_float_import_warned_) {
    edram4_float_import_warned_ = true;
    REXLOG_WARN("[native] EDRAM mode4 D24FS8 import clamps guest depth >1: native unhalved host range "
                "cannot preserve these raw float24 encodings; NOT conformance");
  }
  return true;
}

bool TargetsDk::EnsureResolveDepthNative2xEDRAM4() {
  if (pipeline_resolve_depth_msaa2_edram_) return true;
  // Vulkan: also needed layout_pipeline_conversion_depth_edram_; deko3d has no pipeline layout.
  if (!AcceptsImportDepthNative2xEDRAM4(VK_FORMAT_D32_SFLOAT_S8_UINT)) return false;
  pipeline_resolve_depth_msaa2_edram_ = LoadShader(dkshaders::kMeDepthResolveGuestspaceMsaa2);
  return pipeline_resolve_depth_msaa2_edram_ != nullptr;
}

bool TargetsDk::ResolverDepthGuestspaceEDRAM4(Image& source, Image& target,
    uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height,
    uint32_t raw_sample_select) {
  if (!source.depth_float24_half || target.format != VK_FORMAT_R32_SFLOAT ||
      !target.resolved_depth_guestspace || !width || !height ||
      sx >= source.width || sy >= source.height || dx >= target.width || dy >= target.height ||
      width > source.width - sx || height > source.height - sy ||
      width > target.width - dx || height > target.height - dy)
    return FailureEDRAM4("guestspace depth resolve invalid contract/rectangle", target, &source);
  const bool native2x = IsDepthNative2xEDRAM4(source);
  if (!me::native::IsSingleSample(uint32_t(target.sample_count)) ||
      (!me::native::IsSingleSample(uint32_t(source.sample_count)) && !native2x))
    return FailureEDRAM4("guestspace depth resolve hardware sample contract unsupported", target, &source);
  const auto selected = native2x ? me::native::SanitizeNative2xDepthCopySampleSelect(raw_sample_select)
                                 : std::optional<uint32_t>(0);
  if (!selected || (native2x && !EnsureResolveDepthNative2xEDRAM4()))
    return FailureEDRAM4("native2x guestspace resolve selector/capability/resources unsupported", target, &source);
  const DkShader* const pipeline = native2x ? pipeline_resolve_depth_msaa2_edram_
                                            : pipelines_conversion_depth_edram_[3];
  if (!EnsureCapacityConversionEDRAM4("guestspace depth resolve"))
    return FailureEDRAM4("guestspace resolve cannot acquire recording slot", target, &source);
  const ViewsDepthEDRAM views = GetViewsDepthEDRAM4(source);
  if (!views.depth.pImage || !pipeline)
    return FailureEDRAM4("guestspace depth resolve resources unavailable", target, &source);
  WorkSlot& slot = slots_[slot_];
  // Vulkan: a set with the sampled depth (binding 0) and the R32F storage target (binding 1). deko3d: texture 0 and
  // image 1 of the compute stage, from the slot's transient ring.
  const uint32_t descriptor_depth = PushDescriptor(views.depth);
  const uint32_t descriptor_target = PushDescriptor(sa2::dk::View(target.image), true);
  if (descriptor_depth == UINT32_MAX || descriptor_target == UINT32_MAX)
    return FailureEDRAM4("guestspace resolve descriptor allocation failed", target, &source);
  ++slot.conversions_edram;
  struct Constants {
    uint32_t source_size[2], destination_size[2], source_offset[2], destination_offset[2], extent[2];
    float guest_factor;
    uint32_t sample_select;
  } constants{{HostWidth(source), HostHeight(source)}, {HostWidth(target), HostHeight(target)},
              {sx, sy}, {dx, dy}, {width, height}, 2.0f, *selected};
  // sa2-nx render resolution: the rectangles in host pixels.
  {
    const VkRect2D hs = HostRect(source, VkRect2D{{int32_t(sx), int32_t(sy)}, {width, height}});
    const VkRect2D hd = HostRect(target, VkRect2D{{int32_t(dx), int32_t(dy)}, {width, height}});
    constants.source_offset[0] = uint32_t(hs.offset.x);
    constants.source_offset[1] = uint32_t(hs.offset.y);
    constants.destination_offset[0] = uint32_t(hd.offset.x);
    constants.destination_offset[1] = uint32_t(hd.offset.y);
    constants.extent[0] = std::min(hs.extent.width, hd.extent.width);
    constants.extent[1] = std::min(hs.extent.height, hd.extent.height);
  }
  static_assert(offsetof(Constants, sample_select) == 44 && sizeof(Constants) == 48);
  // Keep the original 1x shader's 44-byte ABI byte-for-byte. The dormant MS
  // variant adds ONLY the already sanitized guest selector at offset44.
  const uint32_t constant_bytes = native2x ? 48u : 44u;
  sa2::dk::Barrier(commands_work_, Hazard::AnyToCompute);
  const DkGpuAddr constants_gpu = PushConstants(&constants, constant_bytes);
  if (constants_gpu == DK_GPU_ADDR_INVALID)
    return FailureEDRAM4("guestspace resolve constants do not fit the slot", target, &source);
  dkCmdBufBindTexture(commands_work_, DkStage_Compute, 0, sa2::dk::Handle(descriptor_depth, sampler_depth_edram_));
  dkCmdBufBindImage(commands_work_, DkStage_Compute, 1, sa2::dk::Handle(descriptor_target, 0));
  DispatchDk(commands_work_, pipeline, constants_gpu, constant_bytes, (constants.extent[0] + 7) / 8,
             (constants.extent[1] + 7) / 8);
  sa2::dk::Barrier(commands_work_, Hazard::ComputeToAny);
  return true;
}

bool TargetsDk::ConvertDepthEDRAM4(Image& source, Image& target,
                                   uint32_t source_start, uint32_t target_start, uint32_t count) {
  if (!source.edram_depth || target.edram_depth)
    return FailureEDRAM4("depth export source/destination class unsupported", target, &source);
  if (!me::native::IsSingleSample(uint32_t(source.sample_count)) ||
      !me::native::IsSingleSample(uint32_t(target.sample_count)))
    return false;  // sa2_msaa: the compute export reads and writes single-sample views (skipped as unsupported)
  const DkImageView raw_destination = target.edram_64bpp ? GetViewRaw64EDRAM4(target) : DkImageView{};
  if (target.edram_64bpp && (!raw_destination.pImage || target.edram_format != 7))
    return FailureEDRAM4("raw FP16 UINT sampled/storage view unavailable", target, &source);
  const uint32_t pipeline = target.edram_64bpp ? 2u : target.format == VK_FORMAT_R8G8B8A8_UNORM ? 0u :
      (target.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
       (target.edram_format == 2 || target.edram_format == 3)) ? 1u :
      (target.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && target.edram_format == 3) ? 4u : UINT32_MAX;
  if (pipeline == UINT32_MAX || source.edram_format > 1 || !count) return false;
  const ViewsDepthEDRAM views = GetViewsDepthEDRAM4(source);
  if (!views.depth.pImage) return false;
  if (!pipelines_conversion_depth_edram_[pipeline] ||
      !EnsureCapacityConversionEDRAM4("depth export")) return false;
  WorkSlot& slot = slots_[slot_];
  // Vulkan: sampled depth (0), storage target (1), sampled stencil (2). deko3d: textures 0 and 2, image 1.
  const uint32_t descriptor_depth = PushDescriptor(views.depth);
  const uint32_t descriptor_target =
      PushDescriptor(raw_destination.pImage ? raw_destination : sa2::dk::View(target.image), true);
  const uint32_t descriptor_stencil = PushDescriptor(views.stencil);
  if (descriptor_depth == UINT32_MAX || descriptor_target == UINT32_MAX || descriptor_stencil == UINT32_MAX)
    return false;
  ++slot.conversions_edram;
  if (source.scale_q != target.scale_q)
    return FailureEDRAM4("depth export between images of different render scales", target, &source);
  struct Constants {
    uint32_t source_size[2], target_size[2];
    uint32_t pitch_source, pitch_target, source_start, target_start, count;
    uint32_t source_format, target_format, source_64bpp, target_64bpp;
    uint32_t source_msaa_x, source_msaa_y, target_msaa_x, target_msaa_y;
    uint32_t tile_width, tile_height;  // sa2-nx render resolution: the tile in host samples
  } constants{{HostWidth(source), HostHeight(source)}, {HostWidth(target), HostHeight(target)},
              PitchTilesEDRAM(source), PitchTilesEDRAM(target), source_start, target_start,
              count, uint32_t(source.edram_format) | (source.depth_float24_half ? 256u : 0u),
              target.edram_format, 0, uint32_t(target.edram_64bpp),
              source.edram_msaa_x, source.edram_msaa_y, target.edram_msaa_x, target.edram_msaa_y,
              ScaledTileWidth(target.scale_q), ScaledTileHeight(target.scale_q)};
  static_assert(sizeof(Constants) == 76);
  if (draws_) draws_->FinishPass();
  if (edram4_batch_.active && !CloseImportBatchEDRAM4()) return false;
  if (edram4_batch_.compute_pending) {
    ++edram4_batch_saved_barriers_;  // the previous compute transfer of this batch is still "inside"
  } else {
  sa2::dk::Barrier(commands_work_, Hazard::AnyToCompute);
  }
  const DkGpuAddr constants_gpu = PushConstants(&constants, sizeof(constants));
  if (constants_gpu == DK_GPU_ADDR_INVALID) return false;
  dkCmdBufBindTexture(commands_work_, DkStage_Compute, 0, sa2::dk::Handle(descriptor_depth, sampler_depth_edram_));
  dkCmdBufBindTexture(commands_work_, DkStage_Compute, 2, sa2::dk::Handle(descriptor_stencil, sampler_depth_edram_));
  dkCmdBufBindImage(commands_work_, DkStage_Compute, 1, sa2::dk::Handle(descriptor_target, 0));
  DispatchDk(commands_work_, pipelines_conversion_depth_edram_[pipeline], constants_gpu, sizeof(constants),
             (count * constants.tile_width + 7) / 8, (constants.tile_height + 7) / 8);
  if (edram4_batch_.active) {
    edram4_batch_.compute_pending = true;
  } else {
  sa2::dk::Barrier(commands_work_, Hazard::ComputeToAny);
  }
  if (++edram4_exports_ <= 32)
    REXLOG_INFO("[native] EDRAM mode4 depth->color {:03X}/{} to {:03X}/{} tiles {}:{} count {} "
                "guest MSAA source {}{} destination {}{} (single-sample approximation)",
                source.edram_base, source.edram_format, target.edram_base, target.edram_format,
                source_start, target_start, count, source.edram_msaa_x, source.edram_msaa_y,
                target.edram_msaa_x, target.edram_msaa_y);
  return true;
}

DkImageView TargetsDk::GetViewRaw64RTEDRAM4(Image& image) {
  // Vulkan: a separate VkImageView with COLOR_ATTACHMENT usage, cached per VkImage. deko3d: a view has no usage.
  if (!raw64_edram_supported_ || image.format != VK_FORMAT_R16G16B16A16_SFLOAT) return DkImageView{};
  return sa2::dk::View(image.image, DkImageFormat_RGBA16_Uint);
}

TargetsDk::PassConversionColorFrag* TargetsDk::EnsureConversionColorFrag(VkFormat format, uint32_t variant,
                                                                         uint32_t samples) {
  if (conv_color_frag_failed_ || sampler_depth_edram_ == UINT32_MAX) return nullptr;
  const auto fail = [&]() -> PassConversionColorFrag* {
    conv_color_frag_failed_ = true;
    REXLOG_WARN("[native] EDRAM mode4 fragment color conversions unavailable: compute path");
    return nullptr;
  };
  // Vulkan: the set layout (1 combined sampler), the pipeline layout (76 bytes of fragment push constants), the
  // modules and one pool per slot. deko3d: only the shaders.
  if (!vs_conv_color_frag_) {
    vs_conv_color_frag_ = LoadShader(dkshaders::kMeEdramImport);
    fs_conv_color_frag_ = LoadShader(dkshaders::kMeEdramColorToColor);
    fs_conv_r64_frag_ = LoadShader(dkshaders::kMeEdramR64ToR64);
    if (!vs_conv_color_frag_ || !fs_conv_color_frag_ || !fs_conv_r64_frag_) return fail();
  }
  if (variant == 2 && !fs_resolve_copy_) {  // sa2_gpu_resolve_draw
    fs_resolve_copy_ = LoadShader(dkshaders::kSa2ResolveCopy);
    if (!fs_resolve_copy_) {
      return nullptr;
    }
  }
  auto [it, new_value] = passes_conv_color_frag_.try_emplace(uint64_t(uint32_t(format)) | (uint64_t(variant) << 32) |
                                                             (uint64_t(samples > 1 ? samples : 0) << 40));
  PassConversionColorFrag& pass = it->second;
  if (!new_value) return pass.fs ? &pass : nullptr;
  // A failure of the resolve copy's own pass (variant 2) only declines that path.
  const auto fail_pass = [&]() -> PassConversionColorFrag* {
    if (variant != 2) return fail();
    REXLOG_WARN("[native] targets: the resolve copy pass (sa2_gpu_resolve_draw) could not be created: copies as before");
    return nullptr;
  };
  // Vulkan: the render pass (one color attachment, LOAD/STORE, GENERAL; its two external dependencies are the
  // barriers around the draw, see ConvertAliasColorFragEDRAM4 / ResolveColorFrag) and the pipeline: triangle list,
  // no cull, no blend, RGBA writes, the attachment's samples. deko3d: PassState's defaults plus the samples.
  pass.state.ms.mode = pass.state.ms.rasterizerMode = sa2::dk::FromSamples(samples > 1 ? samples : 1);
  if (sa2::dk::FromVk(format) == DkImageFormat_None) return fail_pass();
  pass.fs = variant == 2 ? fs_resolve_copy_ : variant ? fs_conv_r64_frag_ : fs_conv_color_frag_;
  if (!pass.fs) return fail_pass();
  return &pass;
}

bool TargetsDk::ResolveColorFrag(Image& source, Image& target, int32_t sx, int32_t sy, uint32_t dx, uint32_t dy,
                                 uint32_t width, uint32_t height) {
  if (!g_resolve_draw.active || conv_color_frag_failed_ || sampler_depth_edram_ == UINT32_MAX) return false;
  if (source.format != VK_FORMAT_R8G8B8A8_UNORM || target.format != VK_FORMAT_R8G8B8A8_UNORM ||
      source.image == target.image || !sa2::dk::Alive(source.image) || !sa2::dk::Alive(target.image) ||
      !width || !height ||
      source.guest_width || source.guest_height || target.guest_width || target.guest_height ||
      source.raster_grid_x || target.raster_grid_x || source.scale_q != target.scale_q ||
      !me::native::IsSingleSample(uint32_t(source.sample_count)) ||
      !me::native::IsSingleSample(uint32_t(target.sample_count)) || sx < 0 || sy < 0 ||
      uint64_t(sx) + width > source.width || uint64_t(sy) + height > source.height ||
      uint64_t(dx) + width > target.width || uint64_t(dy) + height > target.height ||
      slots_[slot_].conversions_edram >= kConversionsEDRAMPerSlot || !RoomSlotEDRAM4()) {
    ++resolve_draws_declined_;
    return false;
  }
  PassConversionColorFrag* pass = EnsureConversionColorFrag(VK_FORMAT_R8G8B8A8_UNORM, 2);
  if (!pass) {
    ++resolve_draws_declined_;
    return false;
  }
  // Vulkan: a framebuffer per (target image, variant). deko3d: the color view is bound on the spot.
  if (draws_) draws_->FinishPass();
  if (edram4_batch_.active) {
    CloseComputeBatchEDRAM4();
    if (!CloseImportBatchEDRAM4()) {
      ++resolve_draws_declined_;
      return false;
    }
  }
  WorkSlot& slot = slots_[slot_];
  const uint32_t descriptor_source = PushDescriptor(sa2::dk::View(source.image));
  if (descriptor_source == UINT32_MAX) {
    ++resolve_draws_declined_;
    return false;
  }
  ++slot.conversions_edram;
  // sa2-nx render resolution: in host pixels (resolve rectangles are 8-aligned: exact at every scale).
  const VkRect2D source_area = HostRect(source, VkRect2D{{sx, sy}, {width, height}});
  VkRect2D area = HostRect(target, VkRect2D{{int32_t(dx), int32_t(dy)}, {width, height}});
  area.extent.width = std::min(area.extent.width, source_area.extent.width);
  area.extent.height = std::min(area.extent.height, source_area.extent.height);
  const int32_t delta[2] = {source_area.offset.x - area.offset.x, source_area.offset.y - area.offset.y};
  const DkGpuAddr constants_gpu = PushConstants(delta, sizeof(delta));
  if (constants_gpu == DK_GPU_ADDR_INVALID) {
    ++resolve_draws_declined_;
    return false;
  }
  // Render pass start (dependency 0, exact masks for variant 2: attachment, transfer and shader writes before; the
  // copy and compute writes end with their own barriers on deko3d, so the 3D engine's are what is left).
  sa2::dk::Barrier(commands_work_, Hazard::RenderToSample);
  const DkImageView color_target = sa2::dk::View(target.image);
  dkCmdBufBindRenderTarget(commands_work_, &color_target, nullptr);
  pass->state.Bind(commands_work_);
  BindNoVerticesDk(commands_work_);
  sa2::dk::SetViewport(commands_work_, area);
  const DkScissor scissor = ScissorDk(area);
  dkCmdBufSetScissors(commands_work_, 0, &scissor, 1);
  dkCmdBufBindTexture(commands_work_, DkStage_Fragment, 0, sa2::dk::Handle(descriptor_source, sampler_depth_edram_));
  DrawPassDk(commands_work_, vs_conv_color_frag_, pass->fs, constants_gpu, sizeof(delta));
  // Render pass end (dependency 1: the texture written by the 3D engine, next sampled, copied or drawn over).
  sa2::dk::Barrier(commands_work_, Hazard::RenderToSample);
  if (draws_) draws_->NotifyGraphicsExternalState();
  if (++resolve_draws_ == 1)
    REXLOG_INFO("[native] targets: resolves drawn on the 3D engine (sa2_gpu_resolve_draw): first one {}x{} at "
                "{},{} -> {},{}", width, height, sx, sy, dx, dy);
  g_resolve_draw.UseSampled();
  return true;
}

TargetsDk::PassDepthResolveMs* TargetsDk::EnsureDepthResolveMs(VkFormat format) {
  if (depth_resolve_ms_failed_ || sampler_depth_edram_ == UINT32_MAX) return nullptr;
  const auto fail = [&]() -> PassDepthResolveMs* {
    depth_resolve_ms_failed_ = true;
    REXLOG_WARN("[res] MSAA depth resolve pass could not be created: depth resolves of multisampled targets fail");
    return nullptr;
  };
  // Vulkan: set layout (1 combined sampler), pipeline layout (8 bytes of fragment push constants), the modules and
  // one pool per slot. deko3d: only the shaders.
  if (!vs_depth_resolve_ms_) {
    vs_depth_resolve_ms_ = LoadShader(dkshaders::kMeEdramImport);
    fs_depth_resolve_ms_ = LoadShader(dkshaders::kSa2DepthResolveMs);
    if (!vs_depth_resolve_ms_ || !fs_depth_resolve_ms_) return fail();
  }
  auto [it, fresh] = passes_depth_resolve_ms_.try_emplace(uint32_t(format));
  PassDepthResolveMs& pass = it->second;
  if (!fresh) return pass.valid ? &pass : nullptr;
  // Vulkan: one single-sample depth/stencil attachment (LOAD/STORE) and a pipeline with depth ALWAYS, written.
  pass.state.depth.depthTestEnable = pass.state.depth.depthWriteEnable = true;
  pass.state.depth.depthCompareOp = DkCompareOp_Always;
  if (sa2::dk::FromVk(format) == DkImageFormat_None) return fail();
  pass.valid = true;
  return &pass;
}

bool TargetsDk::ResolveDepthMs(Image& source, Image& target, const VkImageCopy& copy) {
  // Vulkan: target.vk_usage & DEPTH_STENCIL_ATTACHMENT. deko3d: DkImageFlags_UsageRender.
  if (!IsDepthFormat(target.format) || !me::native::IsSingleSample(uint32_t(target.sample_count)) ||
      !(target.dk_flags & DkImageFlags_UsageRender))
    return false;
  PassDepthResolveMs* pass = EnsureDepthResolveMs(target.format);
  const ViewsDepthEDRAM views = GetViewsDepthEDRAM4(source);
  if (!pass || !views.depth.pImage || !EnsureCapacityConversionEDRAM4("msaa depth resolve")) return false;
  // Vulkan: a framebuffer per destination image. deko3d: the depth view is bound on the spot.
  WorkSlot& slot = slots_[slot_];
  const uint32_t descriptor_depth = PushDescriptor(views.depth);
  if (descriptor_depth == UINT32_MAX) return false;
  ++slot.conversions_edram;
  const VkImageCopy host = HostCopy(source, target, copy);
  const VkRect2D area{{host.dstOffset.x, host.dstOffset.y}, {host.extent.width, host.extent.height}};
  const int32_t delta[2] = {host.srcOffset.x - host.dstOffset.x, host.srcOffset.y - host.dstOffset.y};
  if (draws_) draws_->FinishPass();
  if (edram4_batch_.active) {
    CloseComputeBatchEDRAM4();
    if (!CloseImportBatchEDRAM4()) return false;
  }
  const DkGpuAddr constants_gpu = PushConstants(delta, sizeof(delta));
  if (constants_gpu == DK_GPU_ADDR_INVALID) return false;
  sa2::dk::Barrier(commands_work_, HazardPassStartDk());
  const DkImageView depth_target = sa2::dk::View(target.image);
  dkCmdBufBindRenderTargets(commands_work_, nullptr, 0, &depth_target);
  pass->state.Bind(commands_work_);
  BindNoVerticesDk(commands_work_);
  sa2::dk::SetViewport(commands_work_, area);
  const DkScissor scissor = ScissorDk(area);
  dkCmdBufSetScissors(commands_work_, 0, &scissor, 1);
  dkCmdBufBindTexture(commands_work_, DkStage_Fragment, 0, sa2::dk::Handle(descriptor_depth, sampler_depth_edram_));
  DrawPassDk(commands_work_, vs_depth_resolve_ms_, fs_depth_resolve_ms_, constants_gpu, sizeof(delta));
  // Render pass end: the resolved depth texture is sampled next; written through gl_FragDepth (ZCULL stale).
  sa2::dk::Barrier(commands_work_, Hazard::RenderToSample);
  sa2::dk::Barrier(commands_work_, Hazard::DepthWrite);
  if (draws_) draws_->NotifyGraphicsExternalState();
  ++msaa_depth_resolves_;
  sa2::render::NoteMsaaUse();
  return true;
}

bool TargetsDk::MakeAlias(const LastResolve& p, const Image& target, Resolved& resolved, uint32_t address, int32_t x0,
                          int32_t y0, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height) {
  if (!g_resolve_alias.active || off_alias_ || !p.valid || REXCVAR_GET(sa2_native_lazy_front) ||
      !pending_fronts_.empty())
    return false;
  // Only in EDRAM mode 4, where every write of a render target moves edram4_version_ (the other modes have writes
  // no counter here sees).
  if (REXCVAR_GET(sa2_native_edram_alias_mode) != 4) return false;
  if (p.target != target.image || target.invalid_content || p.x0 != x0 || p.y0 != y0 || p.width != width ||
      p.height != height || p.address == address || p.drawn != (draws_ ? draws_->Drawn() : 0) ||
      p.transfers != edram4_transfers_all_ || p.restored != restored_pixels_ || p.edram_version != edram4_version_)
    return false;
  // Small resolves are read back for the guest right after their copy (ResolvedRead): never aliased.
  const int32_t texels_read = REXCVAR_GET(sa2_native_resolved_read_texels);
  if (texels_read > 0 && uint64_t(width) * height <= uint64_t(texels_read)) return false;
  const auto s = resolved_.find(p.address);
  if (s == resolved_.end() || s->second.image.image != p.image || s->second.revision != p.revision ||
      s->second.alias_pending || !s->second.image.prepared || !resolved.image.prepared ||
      s->second.image.format != resolved.image.format || resolved.image.image == p.image ||
      s->second.image.scale_q != resolved.image.scale_q)  // sa2-nx render resolution
    return false;
  // This texture's bytes change: the aliases reading from it get theirs first, and its own alias is covered by
  // this one or copied first.
  BeforeResolvedWrite(address, dx, dy, width, height);
  if (off_alias_ || aliases_.count(address)) return false;
  AliasResolve& alias = aliases_[address];
  alias.source = p.address;
  alias.source_image = p.image;
  alias.source_revision = p.revision;
  alias.image = resolved.image.image;
  alias.sx = int32_t(p.dx);
  alias.sy = int32_t(p.dy);
  alias.dx = dx;
  alias.dy = dy;
  alias.width = width;
  alias.height = height;
  resolved.alias_pending = true;
  ResolvedWritten(address);  // new bytes for its readers (crops)
  // The draws keep the slot of a texture for the rest of the frame without asking for it again
  // (PrepareTexture's caches): those that use this image ask again, so a later read materializes it. Nothing
  // to do for a texture nobody samples.
  if (draws_) draws_->InvalidateImages(resolved.image.image, sa2::dk::kNoImage);
  ++aliases_made_;
  if (DiagAlias())
    REXLOG_INFO("[diag alias] frame {} MAKE {:08X} ({},{} {}x{}) = {:08X} ({},{}) rev {} img {}", presented_, address,
                dx, dy, width, height, p.address, p.dx, p.dy, p.revision, resolved.image.image);
  g_resolve_alias.UseSampled();
  return true;
}

bool TargetsDk::DiagAlias() const {
  static const int32_t from = REXCVAR_GET(sa2_diag_alias_frames);
  return from > 0 && presented_ >= uint64_t(from) && presented_ < uint64_t(from) + 40;
}

// The source of `alias` still holds the bytes the alias was made with (GUARD).
bool TargetsDk::AliasSourceIntact(const AliasResolve& alias) {
  sa2::enh::VerifyCheck();
  const auto s = resolved_.find(alias.source);
  return s != resolved_.end() && s->second.image.image == alias.source_image &&
         s->second.revision == alias.source_revision && s->second.image.prepared;
}

void TargetsDk::AliasFailure(uint32_t address, const char* why) {
  if (off_alias_) return;
  off_alias_ = true;
  sa2::enh::VerifyDifference();  // sa2_cpu_verify: counted in the [verify] line
  REXLOG_ERROR("[native] targets: DIFFERENCE: resolve alias {:08X} ({}). sa2_gpu_resolve_alias off for the rest of "
               "the session: every resolve is copied", address, why);
}

void TargetsDk::DropAlias(uint32_t address) {
  aliases_.erase(address);
  if (const auto r = resolved_.find(address); r != resolved_.end()) r->second.alias_pending = false;
}

// The alias at `address` gets its bytes now: copied from its source (on the 3D engine when it applies), outside
// any pass. false if the source no longer holds them (GUARD).
bool TargetsDk::MaterializeAlias(uint32_t address) {
  const auto a = aliases_.find(address);
  if (a == aliases_.end()) return true;
  const AliasResolve alias = a->second;
  DropAlias(address);
  const auto r = resolved_.find(address);
  const auto s = resolved_.find(alias.source);
  if (r == resolved_.end() || r->second.image.image != alias.image || !r->second.image.prepared) {
    AliasFailure(address, "its image changed without its bytes");
    return false;
  }
  if (!AliasSourceIntact(alias)) {
    AliasFailure(address, "its source changed before it was read");
    return false;
  }
  if (!Record()) {
    AliasFailure(address, "the copy could not be recorded");
    return false;
  }
  if (draws_) draws_->FinishPass();  // may come from a draw (ResolvedTexture): the copy goes outside the pass
  MarkGpu(kGpuCopies);
  if (!ResolveColorFrag(s->second.image, r->second.image, alias.sx, alias.sy, alias.dx, alias.dy, alias.width,
                        alias.height)) {
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.srcOffset = {alias.sx, alias.sy, 0};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstOffset = {int32_t(alias.dx), int32_t(alias.dy), 0};
    copy.extent = {alias.width, alias.height, 1};
    copy = HostCopy(s->second.image, r->second.image, copy);  // sa2-nx render resolution
    BarrierBeforeCopyResolve();
    CopyRegionsDk(commands_work_, s->second.image.image, r->second.image.image, 1, &copy);
    BarrierAfterCopyResolve();
  }
  // Not ResolvedWritten: its revision changed when the alias was made, and these are the bytes it promised.
  ResolvedPixelsCopied(address, uint64_t(alias.width) * alias.height);
  ++aliases_copied_;
  if (DiagAlias())
    REXLOG_INFO("[diag alias] frame {} COPY {:08X} ({},{} {}x{}) <- {:08X} ({},{}) img {} <- {}", presented_, address,
                alias.dx, alias.dy, alias.width, alias.height, alias.source, alias.sx, alias.sy,
                r->second.image.image, s->second.image.image);
  return true;
}

// The source of the alias at `alias_address` is about to be overwritten whole (rectangle dx, dy, width, height):
// the alias takes the source's image, which holds its bytes, and the source the alias's image, which that write
// covers. Only whole textures of the same shape, nothing else holding either image. false: not done (copy it).
bool TargetsDk::ExchangeAlias(uint32_t alias_address, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height) {
  const auto a = aliases_.find(alias_address);
  if (a == aliases_.end()) return false;
  const AliasResolve alias = a->second;
  const auto r = resolved_.find(alias_address);
  const auto s = resolved_.find(alias.source);
  if (r == resolved_.end() || s == resolved_.end() || r == s) return false;
  Image& mine = r->second.image;
  Image& source = s->second.image;
  if (!width || dx || dy || width != source.width || height != source.height || alias.sx || alias.sy ||
      alias.dx || alias.dy || alias.width != mine.width || alias.height != mine.height ||
      mine.width != source.width || mine.height != source.height || mine.format != source.format ||
      mine.scale_q != source.scale_q ||  // sa2-nx render resolution
      mine.sample_count != source.sample_count || mine.accepts_target_of_copy != source.accepts_target_of_copy ||
      mine.guest_width || mine.guest_height || source.guest_width || source.guest_height ||
      mine.image != alias.image || !mine.prepared || !AliasSourceIntact(alias))
    return false;
  // A render target whose content went out to one of them in a resolve without copy reads it back by address.
  for (const auto* images : {&targets_, &depths_})
    for (const auto& [key, image] : *images)
      if (image.invalid_content && (image.resolved_base == alias_address || image.resolved_base == alias.source))
        return false;
  // deko3d: the id carries the memory and the descriptors (there is no memory or view handle to swap).
  std::swap(mine.image, source.image);
  std::swap(mine.pool_block, source.pool_block);
  std::swap(mine.prepared, source.prepared);
  DropAlias(alias_address);
  ResolvedWritten(alias_address);  // another image at both addresses
  ResolvedWritten(alias.source);
  // Only what points to these two images (as SwapWithResolved): the views go with their images.
  if (draws_) draws_->InvalidateImages(mine.image, source.image);
  ++aliases_exchanged_;
  return true;
}

void TargetsDk::BeforeResolvedWrite(uint32_t address, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height) {
  if (DiagAlias()) {
    REXLOG_INFO("[diag alias] frame {} WRITE {:08X} ({},{} {}x{}), {} aliases; transfers {} restored {}", presented_,
                address, dx, dy, width, height, aliases_.size(), edram4_transfers_all_, restored_pixels_);
    for (const uint32_t t : {0u, 8u, 16u, 24u, 0x100u, 0x108u, 0x2C0u, 0x2C8u}) {
      const auto& o = owners_tiles_edram4_[t];
      if (o.image)
        REXLOG_INFO("[diag alias]   tile {:03X}: owner {} base {:03X} {}x{} depth {} local {} version {}", t,
                    o.image->image, o.image->edram_base, o.image->width, o.image->height,
                    o.image->edram_depth, o.tile_local, o.version);
      else
        REXLOG_INFO("[diag alias]   tile {:03X}: no owner", t);
    }
  }
  if (aliases_.empty()) return;
  std::array<uint32_t, 8> readers{};
  size_t n = 0;
  bool more = false;
  for (const auto& [alias_address, alias] : aliases_) {
    if (alias.source != address) continue;
    if (n < readers.size()) readers[n++] = alias_address;
    else more = true;
  }
  // An exchange gives the source's image to one alias: with several, the others would lose their bytes, so then
  // all are copied.
  for (size_t i = 0; i < n; ++i)
    if (n > 1 || more || !ExchangeAlias(readers[i], dx, dy, width, height)) MaterializeAlias(readers[i]);
  if (more) {  // (more than 8 aliases of one texture: not expected)
    for (auto it = aliases_.begin(); it != aliases_.end();) {
      const uint32_t alias_address = it->first;
      const bool reader = it->second.source == address;
      ++it;
      if (reader) MaterializeAlias(alias_address);
    }
  }
  const auto a = aliases_.find(address);
  if (a == aliases_.end()) return;
  const AliasResolve& alias = a->second;
  if (width && dx <= alias.dx && dy <= alias.dy && uint64_t(dx) + width >= uint64_t(alias.dx) + alias.width &&
      uint64_t(dy) + height >= uint64_t(alias.dy) + alias.height) {
    if (!AliasSourceIntact(alias)) {  // GUARD: a write to its source went around BeforeResolvedWrite
      AliasFailure(address, "its source changed without giving it its bytes");
    }
    DropAlias(address);
    ++aliases_dropped_;
    if (DiagAlias())
      REXLOG_INFO("[diag alias] frame {} DROP {:08X} (covered by {},{} {}x{})", presented_, address, dx, dy, width,
                  height);
  } else {
    MaterializeAlias(address);
  }
}

// The image at `address` is about to be parked or destroyed: its readers and its own alias get their bytes first.
void TargetsDk::AliasesBeforeImageGoes(uint32_t address) {
  if (aliases_.empty()) return;
  BeforeResolvedWrite(address);
  MaterializeAlias(address);
}

// The copy inventory (NoteCopy) counts the pixels each texture really receives.
void TargetsDk::ResolvedPixelsCopied(uint32_t base, uint64_t pixels) {
  const auto c = copies_per_target_.find(base);
  if (c != copies_per_target_.end()) c->second.pixels += pixels;
}

bool TargetsDk::ConvertAliasColorFragEDRAM4(Image& source, Image& target, uint32_t source_start,
                                            uint32_t target_start, uint32_t count) {
  const auto NoFrag = [&](uint32_t reason) {
    ++conv_color_frag_reasons_[std::min<uint32_t>(reason, 15)];
    return false;
  };
  if (conv_color_frag_failed_ || !count || source.edram_64bpp != target.edram_64bpp ||
      source.scale_q != target.scale_q ||  // sa2-nx render resolution: one scale per transfer
      source.edram_depth || target.edram_depth || source.guest_width || target.guest_width ||
      !me::native::IsSingleSample(uint32_t(source.sample_count)) ||
      !SamplesSupported(target))  // sa2_msaa: into a host multisampled target (every sample gets the pixel)
    return NoFrag(0);
  // Variant 0: the three 32-bit combinations of the compute shaders (16F -> 8888, 8888 -> 16F, 16F -> 16F).
  // Variant 1: 64-bit FP16 views both sides (raw UINT texels, the compute raw64 -> raw64 shader).
  const bool source_8888 = source.format == VK_FORMAT_R8G8B8A8_UNORM;
  const bool target_8888 = target.format == VK_FORMAT_R8G8B8A8_UNORM;
  const uint32_t variant = source.edram_64bpp ? 1u : 0u;
  if (variant == 0) {
    // RGBA8 -> RGBA8 only between different sample layouts (ConvertAliasEDRAM's MSAA alias case): the bytes
    // pass through (bit 16 of both formats); an identical layout is a plain tile copy.
    const bool same_layout = source.edram_msaa_x == target.edram_msaa_x &&
                             source.edram_msaa_y == target.edram_msaa_y;
    // ds2_7e3_r11g11b10: a 7e3 side in B10G11R11 too (the source is sampled and the target is an attachment, so the
    // host format does not matter to the pass; the guest classes travel in the constants).
    const auto float32 = [](const Image& i) {
      return i.format == VK_FORMAT_R16G16B16A16_SFLOAT ||
             (i.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && i.edram_format == 3);
    };
    if ((source_8888 && target_8888 && same_layout) || (!source_8888 && !float32(source)) ||
        (!target_8888 && !float32(target)))
      return NoFrag(1);
  } else if (!target.edram_64bpp || source.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
             target.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
    return NoFrag(2);
  }
  if (!PitchTilesEDRAM(source) || !PitchTilesEDRAM(target)) return NoFrag(3);
  // Destination rectangle of the run (a 64-bit tile is 40 pixels wide, a 32-bit one 80, before MSAA).
  VkRect2D scissor;
  {
    const uint32_t pitch = PitchTilesEDRAM(target);
    const uint32_t tile_w = 80u >> (uint32_t(target.edram_64bpp) + target.edram_msaa_x);
    const uint32_t tile_h = 16u >> target.edram_msaa_y;
    const uint32_t first = target_start / pitch, last = (target_start + count - 1) / pitch;
    const uint32_t sx = first == last ? (target_start % pitch) * tile_w : 0;
    const uint32_t sy = first * tile_h;
    const uint32_t right = first == last
        ? std::min(target.width, ((target_start + count - 1) % pitch + 1) * tile_w) : target.width;
    const uint32_t bottom = std::min(target.height, (last + 1) * tile_h);
    if (!tile_w || !tile_h || sx >= target.width || sy >= target.height || right <= sx || bottom <= sy)
      return NoFrag(4);
    scissor = {{int32_t(sx), int32_t(sy)}, {right - sx, bottom - sy}};
  }
  DkImageView view_source = sa2::dk::View(source.image), view_target = sa2::dk::View(target.image);
  if (variant) {
    view_source = GetViewRaw64EDRAM4(source);
    view_target = GetViewRaw64RTEDRAM4(target);
    if (!view_source.pImage || !view_target.pImage) return NoFrag(5);
  }
  PassConversionColorFrag* pass = EnsureConversionColorFrag(
      variant ? VK_FORMAT_R16G16B16A16_UINT : target.format, variant, uint32_t(target.sample_count));
  if (!pass) return NoFrag(6);
  // Vulkan: a framebuffer per (target image, variant) (NoFrag(7) when it failed). deko3d: bound on the spot.
  if (!EnsureCapacityConversionEDRAM4("color alias")) return false;
  // Diagnostic label of the GPU interval (report "GPU time by pass": VS = 16 + class, PS = tiles).
  LabelMarkGpu(((16u + (variant ? 4u : source_8888 ? 2u : target_8888 ? 1u : 3u)) << 16) |
                    std::min(count, 0xFFFFu));
  WorkSlot& slot = slots_[slot_];
  const uint32_t descriptor_source = PushDescriptor(view_source);
  if (descriptor_source == UINT32_MAX) return false;
  ++slot.conversions_edram;
  struct Constants {
    uint32_t source_size[2], target_size[2];
    uint32_t pitch_source, pitch_target, source_start, target_start, count;
    uint32_t source_format, target_format, source_64bpp, target_64bpp;
    uint32_t source_msaa_x, source_msaa_y, target_msaa_x, target_msaa_y;
    uint32_t tile_width, tile_height;  // sa2-nx render resolution: the tile in host samples
  } constants{{HostWidth(source), HostHeight(source)}, {HostWidth(target), HostHeight(target)},
               PitchTilesEDRAM(source), PitchTilesEDRAM(target), source_start, target_start, count,
               uint32_t(source.edram_format) | (source_8888 ? 0x10000u : 0u),
               uint32_t(target.edram_format) | (target_8888 ? 0x10000u : 0u), variant, variant,
               source.edram_msaa_x, source.edram_msaa_y, target.edram_msaa_x, target.edram_msaa_y,
               ScaledTileWidth(target.scale_q), ScaledTileHeight(target.scale_q)};
  static_assert(sizeof(Constants) == 76);
  if (draws_) draws_->FinishPass();
  if (edram4_batch_.active) {
    CloseComputeBatchEDRAM4();
    if (!CloseImportBatchEDRAM4()) return false;
  }
  const DkGpuAddr constants_gpu = PushConstants(&constants, sizeof(constants));
  if (constants_gpu == DK_GPU_ADDR_INVALID) return false;
  // sa2-nx render resolution: the run's rectangle in host pixels.
  const VkRect2D host_scissor = HostRect(target, scissor);
  // Render pass start (dependency 0) and the attachment.
  sa2::dk::Barrier(commands_work_, HazardPassStartDk());
  dkCmdBufBindRenderTarget(commands_work_, &view_target, nullptr);
  pass->state.Bind(commands_work_);
  BindNoVerticesDk(commands_work_);
  sa2::dk::SetViewport(commands_work_, host_scissor);
  const DkScissor scissor_dk = ScissorDk(host_scissor);
  dkCmdBufSetScissors(commands_work_, 0, &scissor_dk, 1);
  dkCmdBufBindTexture(commands_work_, DkStage_Fragment, 0, sa2::dk::Handle(descriptor_source, sampler_depth_edram_));
  DrawPassDk(commands_work_, vs_conv_color_frag_, pass->fs, constants_gpu, sizeof(constants));
  // Render pass end (dependency 1): the converted tiles are read next by the draws (as an attachment or sampled) or
  // by a copy or a dispatch with its own barrier.
  sa2::dk::Barrier(commands_work_, Hazard::RenderToSample);
  if (draws_) draws_->NotifyGraphicsExternalState();
  if (++conv_color_frag_uses_ == 1)
    REXLOG_INFO("[native] EDRAM mode4 color conversions as fragment passes (sa2_native_conversion_frag): "
                "first one {:03X}/f{} -> {:03X}/f{}, {} tiles, rect {},{}+{}x{}",
                source.edram_base, source.edram_format, target.edram_base, target.edram_format, count,
                scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height);
  return true;
}

bool TargetsDk::ConvertAliasEDRAM(Image& source, Image& target, uint32_t tile_source_start,
                                  uint32_t tile_target_start, uint32_t tiles_count) {
  if (source.scale_q != target.scale_q) return false;  // sa2-nx render resolution: one scale per transfer
  // sa2_msaa: the compute conversions read and write single-sample storage views; the fragment one (RGBA8 below)
  // takes a single-sample source into a multisampled target.
  if (!me::native::IsSingleSample(uint32_t(source.sample_count)) ||
      (!me::native::IsSingleSample(uint32_t(target.sample_count)) &&
       !(source.format == VK_FORMAT_R8G8B8A8_UNORM && target.format == VK_FORMAT_R8G8B8A8_UNORM)))
    return false;
  // sa2-nx: Direct3D's Clear of SA2's 1x 1280x720 back buffer draws through a 4x MSAA alias of the same EDRAM
  // tiles (640x360 pixels of 4 samples each, pitch 640): both views are RGBA8, only the sample layout differs.
  // No compute shader covers that pair (they convert formats), so it always takes the fragment pass, which maps
  // physical samples between the two layouts. It used to be skipped as unsupported
  // (banjo_native_edram4_skip_unsupported): the back buffer was never cleared and every boot screen stayed
  // behind the next ones as a ghost.
  if (source.format == VK_FORMAT_R8G8B8A8_UNORM && target.format == VK_FORMAT_R8G8B8A8_UNORM) {
    if (!tiles_count || source.edram_64bpp || target.edram_64bpp) return false;
    return ConvertAliasColorFragEDRAM4(source, target, tile_source_start, tile_target_start, tiles_count);
  }
  uint32_t pipeline = UINT32_MAX;
  // ds2_7e3_r11g11b10: a 7e3 side in B10G11R11 takes the r11 variants (11, 12, 16-18).
  constexpr VkFormat kR11 = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
  constexpr VkFormat kF16 = VK_FORMAT_R16G16B16A16_SFLOAT;
  constexpr VkFormat kRgba8 = VK_FORMAT_R8G8B8A8_UNORM;
  if ((source.format == kR11 || target.format == kR11) && !source.edram_64bpp && !target.edram_64bpp) {
    pipeline = source.format == kR11 && target.format == kRgba8 ? 11u
             : source.format == kRgba8 && target.format == kR11 ? 12u
             : source.format == kR11 && target.format == kF16  ? 16u
             : source.format == kF16 && target.format == kR11  ? 17u
             : source.format == kR11 && target.format == kR11  ? 18u
                                                               : UINT32_MAX;
    if (pipeline == UINT32_MAX) return FailureEDRAM4("B10G11R11 7e3 alias pair", target, &source);
  } else if ((source.format == kR11 && target.edram_64bpp && target.format == kF16) ||
             (target.format == kR11 && source.edram_64bpp && source.format == kF16)) {
    pipeline = source.format == kR11 ? 7u : 6u;  // the raw64 block below takes the r11 variants (19, 20)
  } else if (source.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
      target.format == VK_FORMAT_R8G8B8A8_UNORM) {
    pipeline = 0;
  } else if (source.format == VK_FORMAT_R8G8B8A8_UNORM &&
             target.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
    pipeline = 1;
  } else if (source.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
             target.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
    pipeline = 2;
  } else {
    return false;
  }
  const bool raw_alias = REXCVAR_GET(sa2_native_edram_alias_mode) == 4 &&
                         (source.edram_64bpp || target.edram_64bpp);
  DkImageView raw_source{}, raw_destination{};
  if (raw_alias) {
    if (source.edram_64bpp) raw_source = GetViewRaw64EDRAM4(source);
    if (target.edram_64bpp) raw_destination = GetViewRaw64EDRAM4(target);
    if ((source.edram_64bpp && !raw_source.pImage) || (target.edram_64bpp && !raw_destination.pImage))
      return FailureEDRAM4("raw FP16 color alias UINT views unavailable", target, &source);
    pipeline = source.edram_64bpp
        ? (target.edram_64bpp ? 8u : target.format == VK_FORMAT_R8G8B8A8_UNORM ? 4u : 6u)
        : source.format == VK_FORMAT_R8G8B8A8_UNORM ? 5u : 7u;
    // ds2_7e3_r11g11b10: the 32-bit 7e3 side in B10G11R11 (Dark Souls II reuses its 7e3 scene tiles as a 64bpp FP16
    // target every frame).
    if (pipeline == 7 && source.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) pipeline = 19;
    if (pipeline == 6 && target.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) pipeline = 20;
  }
  // sa2_native_conversion_frag: the 32-bit pipelines (0-2) and raw64 -> raw64 (8) as a fragment pass.
  // ds2_7e3_r11g11b10: the r11 variants of 0-2 (11, 12, 16-18) take the fragment pass as well.
  const bool frag_pipeline = pipeline <= 2 || pipeline == 8 || pipeline == 11 || pipeline == 12 ||
                             (pipeline >= 16 && pipeline <= 18);
  if (frag_pipeline && tiles_count && REXCVAR_GET(sa2_native_edram_alias_mode) == 4 &&
      REXCVAR_GET(sa2_native_conversion_frag) &&
      ConvertAliasColorFragEDRAM4(source, target, tile_source_start, tile_target_start, tiles_count))
    return true;
  else if (frag_pipeline && tiles_count && REXCVAR_GET(sa2_native_conversion_frag))
    ++conv_color_frag_rejections_;
  if (pipeline >= pipelines_conversion_edram_.size() ||
      pipelines_conversion_edram_[pipeline] == nullptr ||
      !EnsureCapacityConversionEDRAM4("color alias")) {
    return false;
  }
  LabelMarkGpu(((pipeline + 1u) << 16) | std::min(tiles_count, 0xFFFFu));  // report label (compute)
  WorkSlot& slot = slots_[slot_];
  // Vulkan: a set of two storage images. deko3d: two load/store descriptors of the slot's ring (images 0 and 1).
  const uint32_t descriptor_source =
      PushDescriptor(raw_source.pImage ? raw_source : sa2::dk::View(source.image), true);
  const uint32_t descriptor_target =
      PushDescriptor(raw_destination.pImage ? raw_destination : sa2::dk::View(target.image), true);
  if (descriptor_source == UINT32_MAX || descriptor_target == UINT32_MAX) {
    return false;
  }
  ++slot.conversions_edram;

  struct ConstantsConversion {
    uint32_t source_size[2];
    uint32_t target_size[2];
    uint32_t pitch_tiles_source;
    uint32_t pitch_tiles_target;
    uint32_t tile_source_start;
    uint32_t tile_target_start;
    uint32_t tiles_count;
    uint32_t source_format;
    uint32_t target_format;
    uint32_t source_64bpp;
    uint32_t target_64bpp;
    uint32_t source_msaa_x, source_msaa_y;
    uint32_t target_msaa_x, target_msaa_y;
    uint32_t tile_width, tile_height;  // sa2-nx render resolution: the tile in host samples
  } constants = {{HostWidth(source), HostHeight(source)},
                   {HostWidth(target), HostHeight(target)},
                   PitchTilesEDRAM(source),
                   PitchTilesEDRAM(target),
                   tile_source_start,
                   tile_target_start,
                   tiles_count,
                   source.edram_format,
                   target.edram_format,
                   uint32_t(source.edram_64bpp),
                   uint32_t(target.edram_64bpp),
                   source.edram_msaa_x, source.edram_msaa_y,
                   target.edram_msaa_x, target.edram_msaa_y,
                   ScaledTileWidth(target.scale_q), ScaledTileHeight(target.scale_q)};
  static_assert(sizeof(ConstantsConversion) == 76);
  // Destination aliases may still be sampled by preceding draws. Order reads as
  // well as writes; COLOR_ATTACHMENT_OUTPUT alone does not cover fragment reads.
  if (edram4_batch_.active && !CloseImportBatchEDRAM4()) return false;
  if (edram4_batch_.compute_pending)
    ++edram4_batch_saved_barriers_;
  else
  sa2::dk::Barrier(commands_work_, Hazard::AnyToCompute);
  const DkGpuAddr constants_gpu = PushConstants(&constants, sizeof(constants));
  if (constants_gpu == DK_GPU_ADDR_INVALID) return false;
  const DkResHandle images[2] = {sa2::dk::Handle(descriptor_source, 0), sa2::dk::Handle(descriptor_target, 0)};
  dkCmdBufBindImages(commands_work_, DkStage_Compute, 0, images, 2);
  const uint32_t dispatch_width = tiles_count
                                      ? tiles_count * constants.tile_width
                                      : constants.pitch_tiles_target * constants.tile_width;
  const uint32_t dispatch_height = tiles_count ? constants.tile_height
                                                 : HostHeight(target) << target.edram_msaa_y;
  DispatchDk(commands_work_, pipelines_conversion_edram_[pipeline], constants_gpu, sizeof(constants),
             (dispatch_width + 7) / 8, (dispatch_height + 7) / 8);
  if (edram4_batch_.active) {
    edram4_batch_.compute_pending = true;
    return true;
  }
  sa2::dk::Barrier(commands_work_, Hazard::ComputeToAny);
  return true;
}

Image* TargetsDk::GetDepth(uint32_t base, uint32_t format, uint32_t pitch) {
  // The inputs of the path below: the arguments, the EDRAM MSAA mode, whether the draw may use the depth raster
  // grid, and the two cvars it reads (format_depth_ and the D32S8 answer are fixed once known).
  const int32_t alias_mode = REXCVAR_GET(sa2_native_edram_alias_mode);
  const bool samples_x = REXCVAR_GET(sa2_native_depth_samples_x);
  const uint64_t memo_in = (uint64_t(uint32_t(alias_mode) & 0xFF) << 48) | (uint64_t(samples_x) << 47) |
                           (uint64_t(depth_raster_grid_eligible_) << 46) |
                           (uint64_t(msaa_edram_actual_ & 0xF) << 40) | (uint64_t(base & 0xFFFFF) << 20) |
                           (uint64_t(format & 0xF) << 16) | (pitch & 0xFFFF);
  TargetMemo& memo = depth_memo_[(base ^ (pitch >> 4)) & 3];
  if (g_target_memo.active && memo.in == memo_in) {
    g_target_memo.UseSampled();
    return memo.image;
  }
  Image* const found = GetDepthLookup(base, format, pitch);
  if (found && g_target_memo.active && base <= 0xFFFFF && format <= 0xF && pitch <= 0xFFFF &&
      msaa_edram_actual_ <= 0xF)
    memo = {memo_in, found};
  return found;
}

Image* TargetsDk::GetDepthLookup(uint32_t base, uint32_t format, uint32_t pitch) {
  if (!pitch || format_depth_ == VK_FORMAT_UNDEFINED) {
    Reject(11, "no depth target (pitch 0 or format unavailable)");
    return nullptr;
  }
  const bool half = format == 1 && REXCVAR_GET(sa2_native_edram_alias_mode) == 4;
  const VkFormat host_format = half ? VK_FORMAT_D32_SFLOAT_S8_UINT : format_depth_;
  if (half) {
    // Asked once: this ran on every draw (2 % of the Switch ring thread inside NVK).
    // deko3d: ZF32_X24S8 renders, samples and copies (format table): the answer is fixed.
    if (d32s8_supported_ < 0) {
      d32s8_supported_ = sa2::dk::FromVk(host_format) != DkImageFormat_None ? 1 : 0;
    }
    if (!d32s8_supported_) {
      Reject(12, "MODE4 half-range requires sampled D32S8 attachment and transfers");
      return nullptr;
    }
  }
  const auto raster_grid = me::native::GetMsaaRasterGrid(
      REXCVAR_GET(sa2_native_depth_samples_x) &&
        REXCVAR_GET(sa2_native_edram_alias_mode) == 4,
      msaa_edram_actual_, depth_raster_grid_eligible_);
  if (!raster_grid.valid) {
    Reject(12, "invalid depth MSAA raster grid");
    return nullptr;
  }
  const uint32_t grid_x_log2 = raster_grid.grid_x == 2 ? 1u : 0u;
  const uint64_t key = (uint64_t(grid_x_log2) << 49) |
                         (uint64_t(half) << 48) | (uint64_t(msaa_edram_actual_) << 32) |
                         (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
  auto it = depths_.find(key);
  if (it != depths_.end()) {
    return &it->second;
  }
  const uint32_t height = std::min(kMaxTargetHeight, std::max<uint32_t>(720, (pitch + 15) & ~15u));
  // Smaller shadow map.
  //
  // The game requests 1600x1600 shadow maps, which is 2000 of the 2048 80x16 tiles of the Xbox 360 EDRAM. The
  // map can be drawn at sa2_native_shadows_scale percent and upscaled when resolved, so the texture
  // the scene samples is the usual one and only the detail goes down.
  //
  // It is recognized by its 1600x1600 size, which no other render target uses. The scale is read only
  // once (when the first map is created) so that changing it at runtime does not leave old images unused.
  uint32_t image_width = pitch, image_height = height;
  uint32_t guest_width = 0, guest_height = 0;
  if (pitch == kSideShadows && height == kSideShadows) {
    if (scale_shadows_ == 0) {
      scale_shadows_ = uint32_t(std::clamp(REXCVAR_GET(sa2_native_shadows_scale), 50, 100));
      if (!scalable_depth_) {  // Vulkan: !blit_ || !scalable_depth_ (deko3d always has the 2D engine)
        scale_shadows_ = 100;
      }
    }
    if (scale_shadows_ < 100) {
      image_width = std::max<uint32_t>(64, ((pitch * scale_shadows_ / 100) + 15) & ~15u);
      image_height = std::max<uint32_t>(64, ((height * scale_shadows_ / 100) + 15) & ~15u);
      guest_width = pitch;
      guest_height = height;
      REXLOG_INFO("[native] targets: shadow map at {} %: drawn and resolved at {}x{} (the game asks for {}x{})",
                  scale_shadows_, image_width, image_height, pitch, height);
    }
  }
  if (grid_x_log2 && (guest_width || guest_height || image_width > 4096)) {
    Reject(12, "expanded MSAA depth cannot combine with scaled shadows or exceed 8192 width");
    return nullptr;
  }
  image_width <<= grid_x_log2;
  // The usage flag that decides whether there is hierarchical depth culling.
  //
  // The driver only assigns a ZCULL plane if the usage flags fit in
  //   DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC | SAMPLED | INPUT_ATTACHMENT
  // (nvk_image.c). TRANSFER_DST disqualifies the image, and we only request it for
  // vkCmdClearDepthStencilImage, which the specification requires with that usage.
  //
  // TRANSFER_DST is dropped from the scene depth and kept for the shadow map. The scene resolves a
  // sub-rectangle of a larger image, so it is never "whole" and never swaps with its resolved texture; the
  // shadow map does swap (SwapWithResolved requires accepts_target_of_copy) and a restore is a copy
  // into this image (RestoreContent -> copy_image_), which requires TRANSFER_DST.
  //
  // In exchange, the scene depth can no longer be cleared with vkCmdClearDepthStencilImage: it is cleared
  // by opening a pass with loadOp = CLEAR (DrawsVulkan::ClearDepthInPass).
  // With sa2_native_zcull off, every image gets TRANSFER_DST: none receives a ZCULL plane and clears
  // go back to vkCmdClearDepthStencilImage, the path used before ZCULL.
  // deko3d: ZCULL belongs to the queue and every depth render target can use it, and any image takes copies. The
  // usage (and accepts_target_of_copy) keep the Vulkan rule so the callers decide as before (report 03, risk 5).
  const bool zcull = REXCVAR_GET(sa2_native_zcull);
  const bool is_map_of_shadows = (pitch == kSideShadows && height == kSideShadows);
  // 1x depth views receive imported stencil through the copy engine (TRANSFER_DST).
  // sa2-nx (sa2_native_zcull_1x_depth): SA2's 1x depth views never import stencil once the aliased Clears are
  // redirected (0 EDRAM conversions per frame), so the copy-engine stencil path is not worth their ZCULL plane;
  // an import that does happen takes the masked stencil passes (copy_stencil requires accepts_target_of_copy).
  // ds2-nx: ds2_zcull_depth_views (see its definition).
  static const std::string zcull_views = "," + REXCVAR_GET(ds2_zcull_depth_views) + ",";
  static const std::string claim_views = "," + REXCVAR_GET(ds2_clear_depth_claim) + ",";
  const bool claim_clears = claim_views.size() > 2 &&
      claim_views.find(fmt::format(",{:03X}:{}x{},", base, pitch, height)) != std::string::npos;
  const bool keep_zcull = zcull_views.size() > 2 &&
      zcull_views.find(fmt::format(",{:03X}:{}x{},", base, pitch, height)) != std::string::npos;
  const bool with_transfer_dst = is_map_of_shadows || !zcull ||
      (REXCVAR_GET(sa2_native_edram4_stencil_copy) && msaa_edram_actual_ == 0 &&
       !REXCVAR_GET(sa2_native_zcull_1x_depth) && !keep_zcull);
  VkImageUsageFlags usage_depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT;
  if (with_transfer_dst) {
    usage_depth |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  Image image;
  image.edram_depth = true;
  image.depth_float24_half = half;
  image.claim_depth_clears = claim_clears;
  image.edram_base = uint16_t(base);
  image.edram_format = uint8_t(format);
  image.raster_grid_x = grid_x_log2;
  image.edram_msaa_x = uint32_t(msaa_edram_actual_ >= 2) - grid_x_log2;
  image.edram_msaa_y = msaa_edram_actual_ >= 1;
  // Source of the depth copies (CopyDepth): TRANSFER_SRC is required.
  // SAMPLED: with sa2_native_resolver_no_copy this image can end up being the resolved texture.
  // sa2_msaa: the game's single-sample depth targets get host samples (not the expanded or scaled-shadow ones).
  const bool multisampled = msaa_samples_ > 1 && msaa_edram_actual_ == 0 && !grid_x_log2 && !half &&
                            !guest_width && !guest_height;
  if (!Create(image, image_width, image_height, FlagsOfUsage(usage_depth, host_format), host_format,
              multisampled ? VkSampleCountFlagBits(msaa_samples_) : VK_SAMPLE_COUNT_1_BIT)) {
    Reject(12, "could not create a depth target");
    return nullptr;
  }
  image.guest_width = guest_width;
  image.guest_height = guest_height;
  image.accepts_target_of_copy = with_transfer_dst;
  REXLOG_INFO("[native] targets: depth target base {:03X}, format {}, {}x{}{}", base,
              format, pitch, height, with_transfer_dst ? "" : " (no TRANSFER_DST: keeps ZCULL)");
  if (grid_x_log2)
    REXLOG_INFO("[native] depth raster X samples retained: base {:03X}/{} guest {}x{} -> host {}x{}; "
                "Y still collapsed, not native MSAA", base, format, pitch, height, image_width, image_height);
  return &depths_.emplace(key, image).first->second;
}

Resolved* TargetsDk::SearchResolvedPartial(uint32_t base_segment, uint32_t segment_width,
                                           uint32_t segment_height, uint32_t guest_format,
                                           bool swap_rb, VkFormat host_format, uint32_t log2_texel,
                                           uint32_t& base_resolved, uint32_t& dx, uint32_t& dy,
                                           uint32_t rect_width, uint32_t declared_height) {
  if (resolved_.count(base_segment)) {
    return nullptr;  // An exact texture at this address takes precedence.
  }
  for (auto& [base_candidate, candidate] : resolved_) {
    if (base_candidate & 0x80000000u || candidate.guest_format != guest_format ||
        candidate.swap_rb != swap_rb || candidate.image.format != host_format ||
        candidate.image.width != segment_width) {
      continue;
    }
    for (uint32_t y = 0; y < candidate.image.height; y += 32) {
      for (uint32_t x = 0; x < candidate.image.width; x += 32) {
        const uint32_t address =
            (base_candidate + uint32_t(TileDisplacement2D(int32_t(x), int32_t(y),
                                                               segment_width, log2_texel))) &
            0x1FFFFFFF;
        if (address != base_segment || x + dx >= candidate.image.width ||
            y + dy >= candidate.image.height ||
            y + dy + segment_height > candidate.image.height) {
          continue;
        }
        if (g_partial_inside.active && (candidate.image.height != declared_height ||
                                        x + dx + rect_width > candidate.image.width)) {
          g_partial_inside.Use();
          const uint64_t key = (uint64_t(base_candidate) << 32) | base_segment;
          if (partial_refused_.insert(key).second) {
            REXLOG_INFO("[native] targets: resolve to {:08X} ({}x{}, rectangle {}x{}) starts inside the resolved "
                        "texture at {:08X} ({}x{}), at {},{}, but is not a rectangle of it: it gets its own texture "
                        "(sa2_native_partial_resolve_inside)",
                        base_segment, segment_width, declared_height, rect_width, segment_height, base_candidate,
                        candidate.image.width, candidate.image.height, x + dx, y + dy);
          }
          continue;
        }
        base_resolved = base_candidate;
        dx += x;
        dy += y;
        const uint64_t key = (uint64_t(base_candidate) << 32) | base_segment;
        if (partial_recorded_.insert(key).second) {
          REXLOG_INFO("[native] targets: partial resolve: advanced base {:08X} belongs to {:08X}; "
                      "local destination {},{} inside {}x{} (segment {}x{})",
                      base_segment, base_candidate, dx, dy, candidate.image.width,
                      candidate.image.height, segment_width, segment_height);
        }
        return &candidate;
      }
    }
  }
  return nullptr;
}

void TargetsDk::DiagStaleResolved(uint32_t base, uint32_t width, uint32_t height, uint32_t guest_format, bool swap_rb,
                                  VkFormat host_format) {
  struct Request {
    uint32_t base = 0, pages = 0, height = 0;
  };
  static const Request request = [] {
    Request r;
    const std::string text = REXCVAR_GET(sa2_diag_stale_resolved);
    unsigned int b = 0, p = 0, h = 0;
    if (std::sscanf(text.c_str(), "%x:%u:%u", &b, &p, &h) >= 2) {
      r.base = b & 0x1FFFFFFFu;
      r.pages = p;
      r.height = h;
    }
    return r;
  }();
  if (!request.pages || base != request.base || diag_stale_done_ || resolved_.count(base)) return;
  diag_stale_done_ = true;
  const uint32_t stale = (base - request.pages * 4096u) & 0x1FFFFFFFu;
  const uint32_t stale_height = request.height ? request.height : height;
  const bool made = !resolved_.count(stale) &&
                    GetResolved(stale, width, stale_height, guest_format, swap_rb, host_format) != nullptr;
  REXLOG_INFO("[native] targets: sa2_diag_stale_resolved: {} a {}x{} format {} resolved texture at {:08X}, {} pages "
              "below the new destination {:08X} ({}x{})",
              made ? "left" : "could NOT leave", width, stale_height, guest_format, stale, request.pages, base, width,
              height);
}

uint64_t TargetsDk::RetireIdleResolved(uint64_t idle) {
  if (!g_resolved_retire.active || resolved_pool_device_error_ || ResolverNoCopy()) return 0;
  uint64_t released = 0;
  uint32_t count = 0;
  for (auto it = resolved_.begin(); it != resolved_.end();) {
    Resolved& r = it->second;
    const uint32_t base = it->first;
    bool keep = presented_ - std::max(r.last_write, r.last_read) < idle || r.alias_pending ||
                aliases_.count(base) != 0 || pending_fronts_.count(base) != 0;
    if (!keep) {
      for (const auto& [alias_address, alias] : aliases_)
        if (alias.source == base) { keep = true; break; }
    }
    if (!keep) {
      for (const auto& [address, pending] : pending_fronts_)
        if (pending.texture_vk == r.image.image || pending.source_vk == r.image.image) { keep = true; break; }
    }
    if (!keep) {
      for (const auto& owner : owners_tiles_edram4_)
        if (owner.image == &r.image) { keep = true; break; }
    }
    if (keep) {
      ++it;
      continue;
    }
    // vkGetImageMemoryRequirements -> sa2::dk::Bytes (dkImageLayoutGetSize).
    const uint64_t bytes = r.image.image != sa2::dk::kNoImage ? sa2::dk::Bytes(r.image.image) : 0;
    if (retired_resolved_logged_ < 24) {
      ++retired_resolved_logged_;
      REXLOG_INFO("[native] targets: resolved texture at {:08X} ({}x{}, {} KB) released: not resolved to for {} "
                  "presented frames, not read for {} (sa2_native_resolved_retire_idle)",
                  base, r.image.width, r.image.height, bytes >> 10, presented_ - r.last_write,
                  presented_ - r.last_read);
    }
    if (r.image.image != sa2::dk::kNoImage) {
      if (draws_) draws_->ForgetImage(r.image.image);
      Destroy(r.image);
    }
    presented_fronts_.erase(base);
    expired_fronts_.erase(base);
#if !defined(__SWITCH__)
    // The PC build remembers the address: a fetch there before the next resolve is reported (ResolvedTexture).
    if (retired_resolved_.size() < 4096) retired_resolved_[base] = presented_;
#endif
    released += bytes;
    ++count;
    it = resolved_.erase(it);
  }
  // The parked images too (banjo_native_park_resolved keeps up to 64 old shapes of an address for its next change
  // of shape): one that waited this long for its shape to come back is not coming back.
  for (auto it = parked_resolved_.begin(); it != parked_resolved_.end();) {
    Resolved& r = it->second;
    bool keep = presented_ - std::max(r.last_write, r.last_read) < idle;
    if (!keep) {
      for (const auto& [address, pending] : pending_fronts_)
        if (pending.texture_vk == r.image.image || pending.source_vk == r.image.image) { keep = true; break; }
    }
    if (keep) {
      ++it;
      continue;
    }
    if (r.image.image != sa2::dk::kNoImage) {
      released += sa2::dk::Bytes(r.image.image);
      if (draws_) draws_->ForgetImage(r.image.image);
      Destroy(r.image);
    }
    ++count;
    it = parked_resolved_.erase(it);
  }
  if (count) {
    g_resolved_retire.Use();
    retired_resolved_count_ += count;
    retired_resolved_bytes_ += released;
    if (draws_) draws_->InvalidateTextures();  // those addresses are no longer sampled from a resolved texture
  }
  return released;
}

void TargetsDk::NoteFetchAfterRetirement(uint32_t address) {
  const auto it = retired_resolved_.find(address);
  if (it == retired_resolved_.end()) return;
  ++fetches_after_retirement_;
  if (it->second == UINT64_MAX) return;  // reported already
  if (fetches_after_retirement_logged_ < 32) {
    ++fetches_after_retirement_logged_;
    REXLOG_INFO("[native] targets: fetch at {:08X}, whose resolved texture was released {} presented frames ago "
                "(sa2_native_resolved_retire_idle): the texture in guest memory is read",
                address, presented_ - it->second);
  }
  it->second = UINT64_MAX;
}

void TargetsDk::ReportResolved() {
  if (presented_ < report_resolved_next_) return;
  report_resolved_next_ = presented_ + 600;
  uint64_t bytes = 0;
  for (const auto& [base, r] : resolved_) {
    if (r.image.image == sa2::dk::kNoImage) continue;
    bytes += sa2::dk::Bytes(r.image.image);
  }
  for (const auto& [key, r] : parked_resolved_) {
    if (r.image.image == sa2::dk::kNoImage) continue;
    bytes += sa2::dk::Bytes(r.image.image);
  }
  REXLOG_INFO("[native] targets: {} resolved textures and {} parked hold {} MB; {} released when idle so far "
              "({} MB; sa2_native_resolved_retire_idle {}); the oldest read came {} presented frames after its "
              "resolve (at {:08X}); {} fetches at released addresses",
              resolved_.size(), parked_resolved_.size(), bytes >> 20, retired_resolved_count_,
              retired_resolved_bytes_ >> 20,
              g_resolved_retire.active ? "on" : "off", oldest_read_resolved_, oldest_read_address_,
              fetches_after_retirement_);
}

void TargetsDk::ResolvedReportPool(bool detail) {
  if (REXCVAR_GET(sa2_native_edram_alias_mode) != 4) return;
  const auto now = std::chrono::steady_clock::now();
  if (!detail && now - resolved_pool_report_ < std::chrono::seconds(10)) return;
  resolved_pool_report_ = now;
  REXLOG_INFO("[native] resolved allocation pool enabled={}: allocations={}, reuse={}, "
              "retirements={}, evictions={}, eviction waits={}, ineligible={}, busy={}, "
              "dormant={} / {} bytes", REXCVAR_GET(sa2_native_reuse_alloc_resolved),
              resolved_pool_allocations_, resolved_pool_hits_, resolved_pool_retirements_,
              resolved_pool_evictions_, resolved_pool_waits_, resolved_pool_ineligible_,
              resolved_pool_busy_, resolved_sleeping_.size(), resolved_sleeping_bytes_);
}

bool TargetsDk::CanSleepResolved(const Image& image) {
  // Swapped attachments and deferred jobs carry logical identity beyond a resolve. Leave their
  // original fenced replacement path unchanged, including all legacy modes.
  // banjo-nx: which condition refuses (diagnostic, first occurrences only).
  static uint32_t why_logged = 0;
  auto refuse = [&](const char* why) {
    if (why_logged < 24) {
      ++why_logged;
      REXLOG_INFO("[native] banjo: resolved image {}x{} fmt {} not poolable: {}", image.width, image.height,
                  uint32_t(image.format), why);
    }
    return false;
  };
  if (!resolved_pool_startup_eligible_) return refuse("startup");
  if (resolved_pool_device_error_) return refuse("device error");
  if (!me::native::IsSingleSample(uint32_t(image.sample_count))) return refuse("msaa");
  if (!REXCVAR_GET(sa2_native_reuse_alloc_resolved)) return refuse("cvar");
  if (ResolverNoCopy()) return refuse("resolve without copy");
  if (IsDepthFormat(image.format)) return refuse("depth format");
  if (!image.prepared) return refuse("not prepared");
  if (image.invalid_content) return refuse("invalid content");
  if (!pending_fronts_.empty()) return refuse("pending fronts");
  for (const auto& owner : owners_tiles_edram4_)
    if (owner.image == &image) return refuse("edram4 owner");
  for (const auto& [key, target] : targets_)
    if (target.invalid_content) return refuse("a color target has invalid content");
  for (const auto& [key, target] : depths_)
    if (target.invalid_content) return refuse("a depth target has invalid content");
  return true;
}

bool TargetsDk::ResolvedSleepingFinished(const ResolvedSleeping& sleeping) {
  // Both native WORK and OUTPUT are submitted to family_, queue 0. A *future* WORK fence
  // therefore covers output already queued when this image was logically retired. An older
  // work fence by itself would not prove this. Never reuse an unsubmitted retirement marker.
  // deko3d: one queue for work and output too; a slot's fence is its sa2::dk order (dk_order), and the completed
  // orders are read without waiting (vkGetFenceStatus). There is no fence error to report.
  if (resolved_pool_device_error_ || sends_ < sleeping.order_retirement) return false;
  const uint64_t completed = sa2::dk::CompletedOrder();
  for (const auto& slot : slots_) {
    if (!slot.pending || slot.order <= order_completed_work_) continue;
    if (slot.dk_order && slot.dk_order <= completed) order_completed_work_ =
        me::native::ObserveResolvedAllocationCompletion(order_completed_work_, slot.order, true);
  }
  return me::native::ResolvedAllocationReady(sleeping.order_retirement, order_completed_work_);
}

void TargetsDk::EvictResolvedSleeping(size_t index) {
  auto& sleeping = resolved_sleeping_[index];
  if (draws_) draws_->ForgetImage(sleeping.image.image);
  Destroy(sleeping.image);
  resolved_sleeping_bytes_ -= sleeping.bytes;
  resolved_sleeping_.erase(resolved_sleeping_.begin() + index);
  ++resolved_pool_evictions_;
}

bool TargetsDk::SleepResolved(uint32_t base, Image& image) {
  if (!CanSleepResolved(image)) return false;
  const uint64_t bytes = sa2::dk::Bytes(image.image);  // vkGetImageMemoryRequirements
  constexpr uint64_t kMaxBytes = 128ull * 1024 * 1024;
  if (bytes > kMaxBytes) return false;
  while (!me::native::ResolvedAllocationFits(resolved_sleeping_.size(),
                                            resolved_sleeping_bytes_, bytes)) {
    size_t safe = resolved_sleeping_.size();
    for (size_t i = 0; i < resolved_sleeping_.size(); ++i)
      if (ResolvedSleepingFinished(resolved_sleeping_[i])) { safe = i; break; }
    if (safe == resolved_sleeping_.size()) {
      // Rare bounded-cache eviction only: cover both work and presenter consumers before
      // ForgetImage mutates UPDATE_AFTER_BIND descriptors or destroys their views.
      ++resolved_pool_waits_;
      ++waits_gpu_reason_[3]; WaitGpu();
      if (resolved_pool_device_error_) return false;
      if (!ResolvedSleepingFinished(resolved_sleeping_[0])) {
        resolved_pool_device_error_ = true;
        REXLOG_ERROR("[native] resolved allocation eviction lacks successful retirement fence");
        return false;
      }
      safe = 0;
    }
    EvictResolvedSleeping(safe);
  }
  if (draws_) draws_->FinishPass();
  if (!Record() || !CanSleepResolved(image)) return false;
  resolved_sleeping_.push_back({base, image, bytes, sends_ + 1});
  resolved_sleeping_bytes_ += bytes;
  image = {};
  ++resolved_pool_retirements_;
  return true;
}

bool TargetsDk::WakeResolved(uint32_t base, uint32_t width, uint32_t height, VkFormat format,
                             Image& image) {
  if (resolved_pool_device_error_ || !REXCVAR_GET(sa2_native_reuse_alloc_resolved) ||
      ResolverNoCopy() ||
      !pending_fronts_.empty()) return false;
  for (size_t i = 0; i < resolved_sleeping_.size(); ++i) {
    const auto& sleeping = resolved_sleeping_[i];
    if (sleeping.base != base || sleeping.image.width != width ||
        sleeping.image.height != height || sleeping.image.format != format ||
        sleeping.image.scale_q != scale_q_) continue;
    if (!CanSleepResolved(sleeping.image)) continue;
    if (!ResolvedSleepingFinished(sleeping)) { ++resolved_pool_busy_; continue; }
    if (draws_) draws_->FinishPass();
    if (!Record()) return false;
    // Do not use Prepare / UPLOAD here: upload executes before older recorded WORK reads.
    // Keep GENERAL and order a whole-image discard inside WORK, exactly like a new zero image.
    const Image backing = resolved_sleeping_[i].image;
    image = {};
    image.image = backing.image;  // deko3d: the id carries memory and view
    image.width = backing.width;
    image.height = backing.height;
    image.host_width = backing.host_width;  // sa2-nx render resolution
    image.host_height = backing.host_height;
    image.scale_q = backing.scale_q;
    image.dk_flags = backing.dk_flags;
    image.format = backing.format;
    image.prepared = true;
    resolved_sleeping_bytes_ -= resolved_sleeping_[i].bytes;
    resolved_sleeping_.erase(resolved_sleeping_.begin() + i);
    // Vulkan: image barrier (ALL -> TRANSFER), vkCmdClearColorImage to zero, barrier (TRANSFER -> ALL).
    // deko3d: the zero clear is a 3D-engine clear of the image bound as the render target, whole-image scissor.
    // Before: earlier work (draws sampling it, copies reading it) is done with the old bytes; after: what the 3D
    // engine wrote is published to the copy / sampling that comes next.
    const DkCmdBuf cmd = slots_[slot_].work.cmd();
    sa2::dk::Barrier(cmd, Hazard::CopyToRender);
    const DkImageView target_view = sa2::dk::View(image.image);
    dkCmdBufBindRenderTarget(cmd, &target_view, nullptr);
    const DkScissor whole{0, 0, HostWidth(image), HostHeight(image)};
    dkCmdBufSetScissors(cmd, 0, &whole, 1);
    const uint32_t zero[4] = {0, 0, 0, 0};
    dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, zero);
    sa2::dk::Barrier(cmd, Hazard::RenderToSample);
    if (draws_) draws_->NotifyGraphicsExternalState();  // the render target and scissor changed
    image.swap_rb = false;
    image.depth_float24_half = false;
    image.resolved_depth_guestspace = false;
    image.resolved_guest_format = UINT32_MAX;
    ++resolved_pool_hits_;
    ResolvedReportPool(resolved_pool_hits_ <= 8);
    return true;
  }
  return false;
}

Resolved* TargetsDk::GetResolved(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                                 bool swap_rb, VkFormat host_format) {
  if (!width || !height) {
    Reject(9, "copy to a texture of size 0");
    return nullptr;
  }
  uint64_t reads = 0;
  auto it = resolved_.find(base);
  if (it != resolved_.end()) {
    if (it->second.image.width == width && it->second.image.height == height &&
        it->second.image.format == host_format &&
        me::native::IsSingleSample(uint32_t(it->second.image.sample_count))) {
      if (it->second.guest_format != format || it->second.swap_rb != swap_rb) {
        BeforeResolvedWrite(base);  // sa2_gpu_resolve_alias: before the revision moves (a resolve follows)
        it->second.revision = ++revision_resolved_;
        if (draws_) draws_->InvalidateTextures();  // changes the swizzle it is sampled with
      }
      it->second.guest_format = format;
      it->second.swap_rb = swap_rb;
      it->second.image.swap_rb = swap_rb;
      it->second.image.resolved_guest_format = format;
      it->second.last_write = presented_;  // sa2_native_resolved_retire_idle
      ResolvedReportPool();
      return &it->second;
    }
    // Another size or format at the same address: the old image may still be in use.
    // Diagnostic (cutscene dark frame): the new image starts empty, the 360 would keep the bytes.
    if (changes_resolved_recorded_format_ < 400) {
      ++changes_resolved_recorded_format_;
      REXLOG_INFO("[native] resolved texture changes at {:08X}: {}x{} guest format {} host {} -> {}x{} guest {} host {} "
                  "(swap {})",
                  base, it->second.image.width, it->second.image.height, it->second.guest_format,
                  uint32_t(it->second.image.format), width, height, format, uint32_t(host_format), presented_);
    }
    // sa2_gpu_resolve_alias: the image goes (parked with its bytes, or destroyed): the aliases reading from it, and
    // its own alias, get their bytes while it is still here. `it` stays valid (no entry is added or removed).
    AliasesBeforeImageGoes(base);
    reads = it->second.reads;
    // banjo-nx: park the old shape and reuse a parked one of the requested shape (banjo_native_park_resolved).
    // Not while an EDRAM tile owner points at the image (the owner keeps an Image*), nor with resolves
    // swapped into targets (ResolverNoCopy), and at most 64 parked images.
    bool owned = false;
    for (const auto& owner : owners_tiles_edram4_)
      if (owner.image == &it->second.image) { owned = true; break; }
    if (REXCVAR_GET(banjo_native_park_resolved) && !ResolverNoCopy() && !owned && parked_resolved_.size() < 64) {
      const uint64_t old_key = ParkedKey(base, it->second.image.width, it->second.image.height,
                                         uint32_t(it->second.image.format));
      it->second.last_write = presented_;  // sa2_native_resolved_retire_idle: its idle time counts from here
      parked_resolved_[old_key] = std::move(it->second);
      resolved_.erase(it);
      ++parked_resolved_stores_;
      auto p = parked_resolved_.find(ParkedKey(base, width, height, uint32_t(host_format)));
      if (p != parked_resolved_.end()) {
        Resolved back = std::move(p->second);
        parked_resolved_.erase(p);
        back.revision = ++revision_resolved_;
        back.guest_format = format;
        back.swap_rb = swap_rb;
        back.image.swap_rb = swap_rb;
        back.image.resolved_guest_format = format;
        back.reads = reads;
        back.last_write = back.last_read = presented_;
        ++parked_resolved_reuses_;
        if (draws_) draws_->InvalidateTextures();  // that address is now sampled from this image
        return &resolved_.emplace(base, std::move(back)).first->second;
      }
      it = resolved_.end();
    }
    if (it != resolved_.end() && !SleepResolved(base, it->second.image)) {
      ++resolved_pool_ineligible_;
      if (!SendWork(true)) {
        resolved_pool_device_error_ = true;
        return nullptr;
      }
      ++waits_gpu_reason_[4]; WaitGpu();
      if (resolved_pool_device_error_) return nullptr;
      if (draws_) draws_->ForgetImage(it->second.image.image);
      Destroy(it->second.image);
    }
    if (it != resolved_.end()) resolved_.erase(it);
  }
  Resolved resolved;
  resolved.reads = reads;
  resolved.revision = ++revision_resolved_;
  resolved.last_write = resolved.last_read = presented_;
  if (!retired_resolved_.empty()) retired_resolved_.erase(base);  // resolved to again: a live texture
  // The attachment usage (color or depth) is needed for sa2_native_resolver_no_copy: this image can
  // end up being the render target it is swapped with.
  const VkImageUsageFlags usage_target = host_format == VK_FORMAT_R32_SFLOAT ? 0u :
      IsDepthFormat(host_format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  // ds2-nx (ds2_resolved_small_native): small resolved textures stay at the game's size under a render scale.
  const bool small_native = REXCVAR_GET(ds2_resolved_small_native) && (width < 64 || height < 64);
  const bool reused = !small_native && WakeResolved(base, width, height, host_format, resolved.image);
  if (resolved_pool_device_error_) return nullptr;
  const VkImageUsageFlags usage_resolved = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | usage_target |
                 ((host_format == VK_FORMAT_R16G16B16A16_SFLOAT || host_format == VK_FORMAT_R32_SFLOAT ||
                   (host_format == VK_FORMAT_R8G8B8A8_UNORM && REXCVAR_GET(ds2_resolve_bias_rgba8)) ||
                   (host_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && REXCVAR_GET(ds2_resolve_7e3_direct) &&
                    storage_unorm10_))
                      ? VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT) : VkImageUsageFlags(0));
  if (!reused && !(small_native
                       ? CreateScaled(resolved.image, width, height, FlagsOfUsage(usage_resolved, host_format),
                                      host_format, VK_SAMPLE_COUNT_1_BIT, kScaleQuartersNative)
                       : Create(resolved.image, width, height, FlagsOfUsage(usage_resolved, host_format),
                                host_format))) {
    Reject(10, "could not create a resolved texture");
    return nullptr;
  }
  resolved.image.fixed_native = small_native;
  resolved.guest_format = format;
  resolved.swap_rb = swap_rb;
  resolved.image.swap_rb = swap_rb;
  resolved.image.resolved_guest_format = format;
  if (!reused) {
    ++resolved_pool_allocations_;
    REXLOG_INFO("[native] targets: resolved texture at {:08X}, {}x{}, format {}", base, width, height,
                format);
  }
  if (draws_) {
    draws_->InvalidateTextures();  // that address is now sampled from the resolved texture
  }
  ResolvedReportPool();
  return &resolved_.emplace(base, resolved).first->second;
}

bool TargetsDk::IsDepthFormat(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
         format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
         format == VK_FORMAT_X8_D24_UNORM_PACK32;
}

bool TargetsDk::SwapWithResolved(Image& target, Resolved& resolved, uint32_t base) {
  // Resolved textures are sampled with an ordinary sampler2D. Do not swap a
  // future multisample allocation into this 1x identity (or vice versa).
  if (!me::native::IsSingleSample(uint32_t(target.sample_count)) ||
      !me::native::IsSingleSample(uint32_t(resolved.image.sample_count))) return false;
  // Old depth swap restoration copies only the depth aspect. Physical aliasing also needs the
  // original stencil byte; keep both aspects in their original owner in diagnostic mode 4.
  if (REXCVAR_GET(sa2_native_edram_alias_mode) == 4 && target.edram_depth) return false;
  if (target.width != resolved.image.width || target.height != resolved.image.height ||
      target.format != resolved.image.format || target.scale_q != resolved.image.scale_q) {
    return false;
  }
  // ZCULL safety net. If the render target was created without TRANSFER_DST (so it can have a ZCULL
  // plane), swapping it would put it in the resolved texture's place, which does receive copies: an
  // illegal write. And the other way round, the resolved texture (with TRANSFER_DST and no plane) would
  // become the depth target and ZCULL would switch off on alternate frames. Fall back to the usual copy
  // path instead.
  // deko3d: kept as the Vulkan rule (accepts_target_of_copy is computed the same way in GetDepthLookup).
  if (!target.accepts_target_of_copy) {
    return false;
  }
  // Careful: ForgetImage must not be called here. It frees the descriptor slot and reuses it in the
  // same frame, so draws already recorded with it end up reading another image (flicker). Bumping the
  // generation is enough: later draws resolve their view again, and existing views stay valid because
  // they go with their image.
  // sa2_native_lazy_front. Same for front buffers: deferred copies whose source is this target
  // are recorded first, and the texture receives the whole target (its deferred copy is unnecessary).
  if (!pending_fronts_.empty()) {
    BeforeWriteColor(target);
    ResolverPreviousFront(base, true);
  }
  BeforeResolvedWrite(base);  // sa2_gpu_resolve_alias
  // deko3d: the id carries the memory and the descriptors (memory and view handles are gone).
  std::swap(target.image, resolved.image.image);
  std::swap(target.prepared, resolved.image.prepared);
  resolved.image.swap_rb = resolved.swap_rb;  // belongs to the content, not the image
  resolved.image.invalid_content = false;
  target.invalid_content = true;  // its content is now in the resolved texture
  target.resolved_base = base;
  ++swaps_;
  ResolvedWritten(base);   // if it was lent, it has valid content again
  ForgetClear(target);  // and the target keeps another image: its clear no longer holds
  if (draws_) {
    // Only what points to these two images: invalidating the whole cache here costs more than the copy
    // it saves (+0.18 ms of scene per frame when measured).
    draws_->InvalidateImages(target.image, resolved.image.image);
  }
  return true;
}

bool TargetsDk::RestoreContent(Image& target, bool for_resolver) {
  if (!target.invalid_content) {
    return true;
  }
  ForgetSynchronizedEDRAM4(target);  // its content is replaced
  const bool mode4 = REXCVAR_GET(sa2_native_edram_alias_mode) == 4;
  // Preserve the legacy behavior outside the experimental strict path.
  if (!mode4) target.invalid_content = false;
  // sa2_native_lazy_front. Same for front buffers: if the texture about to be read has a
  // deferred copy, it is recorded first; and so are deferred copies whose source is this render target.
  if (!pending_fronts_.empty()) {
    if (pending_fronts_.count(target.resolved_base)) {
      RecordCopyFront(target.resolved_base);
      ++copied_front_read_;
    }
    BeforeWriteColor(target);
  }
  // sa2_gpu_resolve_alias: that texture is read.
  if (!aliases_.empty() && aliases_.count(target.resolved_base)) MaterializeAlias(target.resolved_base);
  auto it = resolved_.find(target.resolved_base);
  if (mode4 && target.depth_float24_half)
    return FailureEDRAM4("half-range owner cannot restore stencil from guestspace R32 resolve", target);
  if (it == resolved_.end() || it->second.image.width != target.width ||
      it->second.image.height != target.height || it->second.image.format != target.format ||
      it->second.image.scale_q != target.scale_q || !kCopyImageDk || !Record()) {
    if (for_resolver) {
      ++resolver_no_source_;  // unknown where to bring it from; resolve whatever is there
    }
    if (mode4) FailureEDRAM4("swapped owner restoration has no valid source", target);
    return false;
  }
  if (for_resolver) {
    // sa2_native_resolver_valid_content. Only the copy, with its barriers.
    if (!CopyOfLapForResolver(target, it->second.image)) return false;
    target.invalid_content = false;
    return true;
  }
  Prepare(it->second.image);
  Prepare(target);
  if (mode4 && (!target.prepared || !it->second.image.prepared ||
                !target.accepts_target_of_copy)) {
    FailureEDRAM4("swapped owner restore images cannot receive the copy", target);
    return false;
  }
  const VkImageAspectFlags aspect =
      IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  // Render targets are created with height = max(720, pitch): the scene measures 1280x1280 to draw
  // 1280x720 and the blur targets 320x720 to draw 320x180. What lies below the useful area is never
  // drawn or resolved, so copying it wastes bandwidth. The useful area is the largest y1 the game has
  // asked to resolve from this target; until there is data, the whole target is copied.
  uint32_t height = target.height;
  if (!mode4 && REXCVAR_GET(sa2_native_restore_area_util)) {
    const auto e = target_state_.find(&target);
    if (e != target_state_.end() && e->second.used_height &&
        e->second.used_height < target.height) {
      height = e->second.used_height;
      ++clipped_restores_;
      pixels_restore_saved_ += uint64_t(target.width) * (target.height - height);
    }
  }
  VkImageCopy copy{};
  copy.srcSubresource = {aspect, 0, 0, 1};
  copy.dstSubresource = {aspect, 0, 0, 1};
  copy.extent = {target.width, height, 1};
  MarkGpu(kGpuCopies);
  std::array<VkImageCopy, 2> restore_regions{copy, copy};
  uint32_t restore_count = 1;
  if (mode4 && aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
    // A whole D32S8 image swap moved BOTH aspects into the resolved backing.
    // deko3d: a copy moves whole texels, so the depth region already carries the stencil: one region.
    restore_regions[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    restore_regions[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    restore_count = 1;
  }
  if (mode4) {
    if (draws_) draws_->FinishPass();
    // Vulkan: ALL -> TRANSFER (MEMORY_R/W -> TRANSFER_R/W). deko3d: the render target's last writes before the copy.
    sa2::dk::Barrier(commands_work_, Hazard::RenderToCopy);
  }
  HostCopies(it->second.image, target, restore_regions.data(), restore_count);  // sa2-nx render resolution
  CopyRegionsDk(commands_work_, it->second.image.image, target.image, restore_count, restore_regions.data());
  if (mode4) BarrierAfterCopyResolve();
  // deko3d: depth written by the copy engine leaves the target's ZCULL state stale.
  if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) sa2::dk::Barrier(commands_work_, Hazard::DepthWrite);
  target.invalid_content = false;
  ++restores_;
  restored_pixels_ += uint64_t(target.width) * height;
  ForgetClear(target);
  return true;
}

void TargetsDk::BarrierBeforeCopyResolve() {
  // Vulkan: COLOR_ATTACHMENT_OUTPUT | EARLY/LATE_FRAGMENT_TESTS | TRANSFER (attachment and transfer writes) ->
  // TRANSFER (read / write).
  sa2::dk::Barrier(commands_work_, Hazard::RenderToCopy);
}

void TargetsDk::BarrierAfterCopyResolve() {
  // Vulkan: TRANSFER -> VERTEX / FRAGMENT shader, TRANSFER, COLOR_ATTACHMENT_OUTPUT, EARLY / LATE_FRAGMENT_TESTS:
  // the copy is next sampled, copied again or rendered into.
  sa2::dk::Barrier(commands_work_, Hazard::CopyToAny);
}

bool TargetsDk::CopyOfLapForResolver(Image& target, Image& source) {
  Prepare(source);
  Prepare(target);
  const bool mode4 = REXCVAR_GET(sa2_native_edram_alias_mode) == 4;
  if (mode4 && (!target.prepared || !source.prepared ||
                !target.accepts_target_of_copy))
    return FailureEDRAM4("resolve owner restore images cannot receive the copy", target, &source);
  const VkImageAspectFlags aspect =
      IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  // Vulkan: depth / color attachment and transfer writes (MEMORY_R/W from ALL in mode 4) -> TRANSFER.
  if (mode4) {
    if (draws_) draws_->FinishPass();
  }
  sa2::dk::Barrier(commands_work_, Hazard::RenderToCopy);
  VkImageCopy copy{};
  copy.srcSubresource = {aspect, 0, 0, 1};
  copy.dstSubresource = {aspect, 0, 0, 1};
  copy.extent = {target.width, target.height, 1};
  MarkGpu(kGpuCopies);
  std::array<VkImageCopy, 2> restore_regions{copy, copy};
  uint32_t restore_count = 1;
  if (mode4 && aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
    // deko3d: whole texels, the stencil travels with the depth region (one region).
    restore_regions[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    restore_regions[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    restore_count = 1;
  }
  HostCopies(source, target, restore_regions.data(), restore_count);  // sa2-nx render resolution
  CopyRegionsDk(commands_work_, source.image, target.image, restore_count, restore_regions.data());
  // And the copied data made visible to what follows: the current resolve (copy or swap), the draws that
  // sample the texture that receives it, and the pass that draws to the render target again.
  sa2::dk::Barrier(commands_work_, Hazard::CopyToAny);
  // deko3d: depth written by the copy engine leaves the target's ZCULL state stale.
  if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) sa2::dk::Barrier(commands_work_, Hazard::DepthWrite);
  ++restores_for_resolver_;
  restored_pixels_ += uint64_t(target.width) * target.height;
  ForgetClear(target);
  return true;
}

}  // namespace sa2::native

#endif  // SA2_DK_AVAILABLE
