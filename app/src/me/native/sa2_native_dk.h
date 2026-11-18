// SA2 - native renderer on deko3d (ds2-deko3d): the interfaces between the translated render target code
// (sa2_native_targets_dk*.cpp) and the translated draws (sa2_native_draws_dk*.cpp).
//
// They are the Vulkan ones (sa2_native_draws.h: ImageNative, ContextTargets, DrawsVulkan) with the GPU handles
// swapped: VkCommandBuffer -> DkCmdBuf, VkImage + VkDeviceMemory + VkImageView -> sa2::dk::ImageId. Field and
// method names are kept so the translated logic reads like the original. SubmissionDraw, StatsDraws and the
// kGpu* categories are shared as they are.

#pragma once

#include "sa2_dk.h"
#include "sa2_native_draws.h"

#include <memory>

#include "cpu/sa2_enhancements.h"

#if SA2_DK_AVAILABLE
namespace sa2::native {

// ImageNative (sa2_native_draws.h) for deko3d: the same logical fields. `image` is the stable id (swapping two
// images = swapping ids), `memory` and `view` are gone (memory lives with the id, views are made on demand),
// `vk_usage` became `dk_flags`. `format` stays the logical VkFormat (keys, logs and format checks unchanged;
// sa2::dk::FromVk at the GPU boundary).
struct ImageNativeDk {
  sa2::dk::ImageId image = sa2::dk::kNoImage;
  uint32_t pool_block = 0xFFFFFFFFu;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t guest_width = 0;
  uint32_t guest_height = 0;
  uint32_t host_width = 0;
  uint32_t host_height = 0;
  uint8_t scale_q = 4;
  bool fixed_native = false;
  uint32_t dk_flags = 0;  // DkImageFlags_* it was created with, to recreate it at another scale
  bool accepts_target_of_copy = true;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT;
  uint8_t edram_format = 0;
  bool edram_depth = false;
  bool depth_float24_half = false;
  bool claim_depth_clears = false;
  bool resolved_depth_guestspace = false;
  uint32_t resolved_guest_format = 0xFFFFFFFFu;
  uint16_t edram_base = 0;
  bool edram_64bpp = false;
  uint32_t raster_grid_x = 0;
  uint8_t edram_msaa_x = 0;
  uint8_t edram_msaa_y = 0;
  bool prepared = false;
  bool swap_rb = false;
  bool invalid_content = false;
  uint32_t resolved_base = 0;
  uint64_t edram4_epoch = 0;
  VkRect2D edram4_own{};
  bool edram4_exported = true;
  bool edram4_usa_stencil = false;
  uint8_t edram4_bits_stencil = 0;
};

inline uint32_t HostWidth(const ImageNativeDk& image) { return image.host_width ? image.host_width : image.width; }
inline uint32_t HostHeight(const ImageNativeDk& image) { return image.host_height ? image.host_height : image.height; }

// ContextTargets for deko3d. Same contract, method by method (see sa2_native_draws.h).
class ContextTargetsDk {
 public:
  virtual ~ContextTargetsDk() = default;
  virtual DkCmdBuf CommandsWork() = 0;
  virtual uint32_t HostMsaaSamples() const { return 1; }
  virtual DkCmdBuf CommandsUpload() = 0;
  virtual uint64_t GenerationCommands() const = 0;
  virtual ImageNativeDk* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) = 0;
  virtual ImageNativeDk* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) = 0;
  virtual const ImageNativeDk* ResolvedTexture(uint32_t address) = 0;
  virtual void GpuMemoryIsShort() {}
  virtual const ImageNativeDk* ResolvedTextureForFetch(uint32_t address, const uint32_t fetch[6],
                                                       const ImageNativeDk& source) {
    (void)address;
    (void)fetch;
    return &source;
  }
  virtual bool SendAndWait() = 0;
  virtual bool SendPartial() { return false; }
  virtual bool WaitGpuOfTheAll() = 0;
  virtual void MarkGpu(uint32_t category) = 0;
  virtual void LabelMarkGpu(uint32_t label) { (void)label; }
  virtual void DescribeMarkGpu(uint64_t description) { (void)description; }
  virtual void CountDrawMarkGpu() {}
  virtual uint32_t BeginStats(uint32_t category) = 0;
  virtual void FinishStats(uint32_t index) = 0;
  virtual uint32_t BeginStatsDraw(uint32_t label, uint32_t category, uint32_t vs_more_one = 0) = 0;
  virtual void FinishStatsDraw(uint32_t index) = 0;
  // New in deko3d: the work slot being recorded (0..slots-1) and its order, for the draws' per-slot rings.
  virtual uint32_t SlotWork() const = 0;
};

// DrawsVulkan for deko3d. Same contract; ForgetView is gone (no view objects), the image arguments are ids.
class DrawsDk {
 public:
  static std::unique_ptr<DrawsDk> Create(rex::memory::Memory* memory, ContextTargetsDk* context);
  virtual ~DrawsDk() = default;
  virtual bool Draw(const SubmissionDraw& submission) = 0;
  virtual void FinishPass() = 0;
  virtual void NotifyGraphicsExternalState() = 0;
  virtual void NotifyRescale(uint32_t q_old, uint32_t q_new) { (void)q_old; (void)q_new; }
  virtual bool ClearDepthInPass(DkCmdBuf commands, const ImageNativeDk& image, float depth, uint32_t stencil,
                                const VkRect2D* area = nullptr) = 0;
  virtual bool ClearStencilInPass(DkCmdBuf commands, const ImageNativeDk& image, uint32_t stencil,
                                  const VkRect2D& area) = 0;
  virtual bool ClearOnlyDepthInPass(DkCmdBuf commands, const ImageNativeDk& image, float depth,
                                    const VkRect2D& area) = 0;
  virtual bool ClearColorInPass(DkCmdBuf commands, const ImageNativeDk& image, const VkClearColorValue& color,
                                const VkRect2D& area) = 0;
  virtual void BeforeSend() = 0;
  virtual void UseSlot(uint32_t slot) = 0;
  virtual void ForgetImage(sa2::dk::ImageId image) = 0;
  virtual bool DropTexturesPerMissingOfMemory() = 0;
  virtual void InvalidateTextures() = 0;
  virtual void InvalidateImages(sa2::dk::ImageId a, sa2::dk::ImageId b) = 0;
  virtual StatsDraws Stats() const = 0;
  virtual uint64_t Drawn() const = 0;
  virtual void WaitUploads() = 0;
  virtual size_t PendingCopies() const { return 0; }
};

}  // namespace sa2::native

// An enhancement of a Vulkan draws file mirrored in a deko3d file: reads the same cvar (no second definition) and
// reports as "<key> (dk)". Use it at global scope (REXCVAR_DECLARE only works there), with a VAR name of its own.
#define SA2_ENHANCEMENT_DK(VAR, KEY, GROUP, DESC)                                                \
  REXCVAR_DECLARE(bool, KEY);                                                                    \
  ::sa2::enh::Enhancement VAR{#KEY " (dk)", GROUP, DESC, [] { return REXCVAR_GET(KEY); }};      \
  [[maybe_unused]] static const bool VAR##_registered_ = ::sa2::enh::Register(&VAR)
// An enhancement with external linkage in a Vulkan file (the render targets' ones, which are at global scope):
// declare it at global scope (outside any namespace) and use the same object.
#define SA2_ENHANCEMENT_EXTERN(VAR) extern ::sa2::enh::Enhancement VAR
#endif  // SA2_DK_AVAILABLE
