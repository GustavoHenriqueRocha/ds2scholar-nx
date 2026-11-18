// SA2 - native renderer on deko3d (ds2-deko3d, stage E3): device, memory and presentation only.
//
// With sa2_native_deko3d = true the Vulkan device and presenter are never created and these targets take the
// ring's copies, draws and Swaps. Nothing of the game is drawn yet: each Swap clears the window and shows two
// bars, the draws (green) and the copies (orange) of that frame over 4000 and 64, so the game's state (title,
// loading, world) can be told on the console. The fps in the log is then the game's ceiling without GPU work.
// Stages E4 (copies, resolves, clears) and E5 (draws) fill it in: ds2-deko3d docs/pesquisas/2026-11-17-plano-completo.md.

#include "sa2_native_targets.h"

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
#include <deko3d.h>
#include <switch.h>

#include <algorithm>
#include <array>

extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace sa2::native {

#if REX_PLATFORM_SWITCH
namespace {

constexpr uint32_t kWindowWidth = 1280;
constexpr uint32_t kWindowHeight = 720;
constexpr uint32_t kSwapchainImages = 2;
constexpr uint32_t kCommandHalfBytes = 64u << 10;  // one half per frame in flight
constexpr uint32_t kBarDraws = 4000;
constexpr uint32_t kBarCopies = 64;

uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

void DebugCallback(void*, const char* context, DkResult result, const char* message) {
  REXLOG_WARN("[deko3d] {}: result {}: {}", context ? context : "?", int(result), message ? message : "");
}

class TargetsDeko3d final : public TargetsNative {
 public:
  ~TargetsDeko3d() override { Destroy(); }

  bool Initialize() {
    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    device_maker.cbDebug = DebugCallback;
    device_ = dkDeviceCreate(&device_maker);
    if (!device_) return false;

    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, device_);
    layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layout_maker.format = DkImageFormat_RGBA8_Unorm;
    layout_maker.dimensions[0] = kWindowWidth;
    layout_maker.dimensions[1] = kWindowHeight;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layout_maker);
    const uint32_t image_bytes = AlignUp(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    swapchain_memory_ = MakeBlock(image_bytes * kSwapchainImages, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    command_memory_ = MakeBlock(2 * kCommandHalfBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!swapchain_memory_ || !command_memory_) return false;
    std::array<const DkImage*, kSwapchainImages> images{};
    for (uint32_t i = 0; i < kSwapchainImages; ++i) {
      dkImageInitialize(&images_[i], &layout, swapchain_memory_, i * image_bytes);
      images[i] = &images_[i];
    }
    DkSwapchainMaker swapchain_maker;
    dkSwapchainMakerDefaults(&swapchain_maker, device_, nwindowGetDefault(), images.data(), kSwapchainImages);
    swapchain_ = dkSwapchainCreate(&swapchain_maker);
    if (!swapchain_) return false;

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, device_);
    queue_maker.flags = DkQueueFlags_Graphics;
    queue_ = dkQueueCreate(&queue_maker);
    DkCmdBufMaker cmd_maker;
    dkCmdBufMakerDefaults(&cmd_maker, device_);
    cmd_ = dkCmdBufCreate(&cmd_maker);
    if (!queue_ || !cmd_) return false;
    REXLOG_INFO("[deko3d] device, queue and swapchain of {} images {}x{} ({:.1f} MB); the game is not drawn yet "
                "(stage E3)", kSwapchainImages, kWindowWidth, kWindowHeight,
                double(image_bytes * kSwapchainImages) / 1048576.0);
    return true;
  }

  bool Copy(const RegistersCopy&) override {
    ++copies_;
    ++copies_in_frame_;
    return true;
  }

  bool Present(rex::ui::Presenter*, const TextureSwap&, uint32_t, uint32_t) override {
    RexSwitchPerfCount(0);  // the profiler's "game N fps"
    const int slot = dkQueueAcquireImage(queue_, swapchain_);
    const uint32_t half = uint32_t(presented_ & 1);
    if (fenced_[half]) dkFenceWait(&fences_[half], -1);
    dkCmdBufClear(cmd_);
    dkCmdBufAddMemory(cmd_, command_memory_, half * kCommandHalfBytes, kCommandHalfBytes);
    DkImageView view;
    dkImageViewDefaults(&view, &images_[slot]);
    dkCmdBufBindRenderTarget(cmd_, &view, nullptr);
    // A background that pulses once per second at 60 Swaps: a stalled ring shows as a frozen shade.
    const float pulse = float(presented_ % 60) / 600.0f;
    Fill({0, 0, kWindowWidth, kWindowHeight}, 0.05f + pulse, 0.05f + pulse, 0.15f + pulse);
    Fill({0, kWindowHeight - 80, Bar(draws_in_frame_, kBarDraws), 24}, 0.2f, 0.8f, 0.2f);
    Fill({0, kWindowHeight - 48, Bar(copies_in_frame_, kBarCopies), 24}, 0.9f, 0.5f, 0.1f);
    dkCmdBufSignalFence(cmd_, &fences_[half], false);
    fenced_[half] = true;
    dkQueueSubmitCommands(queue_, dkCmdBufFinishList(cmd_));
    dkQueuePresentImage(queue_, swapchain_, slot);
    ++presented_;
    draws_in_frame_ = copies_in_frame_ = 0;
    return true;
  }

  void RampGamma(const std::array<std::array<uint16_t, 3>, 256>&) override {}

  bool Draw(const SubmissionDraw&) override {
    ++draws_;
    ++draws_in_frame_;
    return true;
  }
  StatsDraws StatsOfDraws() const override {
    StatsDraws stats;
    stats.drawn = draws_;
    return stats;
  }
  void WaitUploads() override {}
  void Stats(uint64_t& copies, uint64_t& cleared, uint64_t& presented, uint64_t& rejections) const override {
    copies = copies_;
    cleared = 0;
    presented = presented_;
    rejections = 0;
  }
  void TimeGpuPerCategory(std::array<uint64_t, kGpuCategories>& nanoseconds) const override { nanoseconds = {}; }
  void StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments, std::array<uint64_t, kGpuCategories>& vertices,
                     std::array<uint64_t, kGpuCategories>& primitives) const override {
    fragments = vertices = primitives = {};
  }

 private:
  DkMemBlock MakeBlock(uint32_t size, uint32_t flags) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device_, AlignUp(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
  }

  static uint32_t Bar(uint32_t count, uint32_t full) {
    return std::max<uint32_t>(4, kWindowWidth * std::min(count, full) / full);
  }

  // Clears are scissored: the rectangle only.
  void Fill(const DkScissor& rect, float red, float green, float blue) {
    dkCmdBufSetScissors(cmd_, 0, &rect, 1);
    dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, red, green, blue, 1.0f);
  }

  void Destroy() {
    if (queue_) dkQueueWaitIdle(queue_);
    if (swapchain_) dkSwapchainDestroy(swapchain_);
    if (cmd_) dkCmdBufDestroy(cmd_);
    if (queue_) dkQueueDestroy(queue_);
    if (command_memory_) dkMemBlockDestroy(command_memory_);
    if (swapchain_memory_) dkMemBlockDestroy(swapchain_memory_);
    if (device_) dkDeviceDestroy(device_);
    swapchain_ = nullptr;
    cmd_ = nullptr;
    queue_ = nullptr;
    command_memory_ = swapchain_memory_ = nullptr;
    device_ = nullptr;
  }

  DkDevice device_ = nullptr;
  DkQueue queue_ = nullptr;
  DkCmdBuf cmd_ = nullptr;
  DkMemBlock command_memory_ = nullptr;
  DkMemBlock swapchain_memory_ = nullptr;
  DkSwapchain swapchain_ = nullptr;
  std::array<DkImage, kSwapchainImages> images_{};
  std::array<DkFence, 2> fences_{};
  std::array<bool, 2> fenced_{};
  uint64_t copies_ = 0, draws_ = 0, presented_ = 0;
  uint32_t copies_in_frame_ = 0, draws_in_frame_ = 0;
};

}  // namespace

std::unique_ptr<TargetsNative> TargetsNative::CreateDeko3d(rex::memory::Memory* memory) {
  if (!memory) return nullptr;
  auto targets = std::make_unique<TargetsDeko3d>();
  if (!targets->Initialize()) {
    REXLOG_ERROR("[deko3d] targets: could not create the device, the swapchain or their memory");
    return nullptr;
  }
  return targets;
}
#else
std::unique_ptr<TargetsNative> TargetsNative::CreateDeko3d(rex::memory::Memory*) {
  REXLOG_ERROR("[deko3d] sa2_native_deko3d is Switch only");
  return nullptr;
}
#endif

}  // namespace sa2::native
