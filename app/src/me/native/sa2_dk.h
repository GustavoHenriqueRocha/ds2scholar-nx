// SA2 - native renderer on deko3d (ds2-deko3d): the base layer every translated file (*_dk.cpp) uses.
//
// One device and one queue per process, owned here. What the Vulkan files get from VkDevice, VkDeviceMemory,
// VkImage/VkImageView, descriptor pools, command pools, query pools, fences and pipeline barriers, the deko3d
// files get from these few pieces:
//   - submission orders: every Submit gets a number; CompletedOrder() says how far the GPU got. Anything the GPU
//     may still read (image memory, descriptor slots, blocks) is released "after an order", never at once;
//   - Block / Arena: CPU-visible memory (uploads, uniform buffers, read-backs) and a bump allocator over it;
//   - images: ImageId (index + generation) over objects that never move, memory from a sub-allocator over
//     32 MB image blocks, views made on demand (no view objects to cache or forget);
//   - one image descriptor set and one sampler descriptor set, bound once per command list. The game shaders'
//     bindless handle is image | sampler << 20 (ds2-deko3d tools/shaders/reescrever.py), so all image kinds
//     share the set in ranges (kDescriptorsDraws below);
//   - CmdStream: a DkCmdBuf fed in chunks, with the guard dkCmdBufBindVtxBuffers needs (it writes 6 words and
//     reserves 5: TriAevum, "Armadilhas conhecidas");
//   - Counters: DkCounter reports in raw GPU ticks (the Vulkan code multiplies NVK's raw ticks by 1.627 =
//     625/384 itself, so they must stay raw);
//   - Barrier(cmd, Hazard): every barrier is named by the dependency it serves, so the mapping can be tuned in
//     one place from the semantics bench (ds2-deko3d bench-semantica) instead of hunting calls;
//   - CodeArena: DKSH (uam) loaded into code memory;
//   - Window: the swapchain on the default NWindow.
// Vulkan structs (VkFormat, VkRect2D, VkImageCopy, VkClearColorValue...) stay in the translated logic as plain
// data so it reads like the Vulkan original; FromVk() converts formats at the GPU boundary.
// Contract and decisions: ds2-deko3d docs/pesquisas/traducao/08-contrato-sa2-dk.md.

#pragma once

#if REX_PLATFORM_SWITCH || defined(__SWITCH__)
#define SA2_DK_AVAILABLE 1
#include <deko3d.h>
#else
#define SA2_DK_AVAILABLE 0
#endif

#include <rex/ui/vulkan/api.h>  // only the plain types (VkFormat, VkRect2D...), never a call

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#if SA2_DK_AVAILABLE
namespace sa2::dk {

// --- Device, queue and submission orders ----------------------------------------------------------------------

struct DeviceConfig {
  // Compute jobs the queue keeps in flight. deko3d's default (128) makes the queue wait for idle each time its
  // job ring wraps; the EDRAM conversions dispatch up to 2048 per slot (report 03).
  uint32_t compute_jobs = 1024;
  uint32_t queue_command_bytes = 1u << 20;
};

bool Initialize(const DeviceConfig& config = {});  // device + queue (Graphics | Compute, Zcull on)
void Shutdown();                                    // waits for idle, frees everything this layer owns
bool Ready();
DkDevice Device();
DkQueue Queue();

// The order the next Submit will get (orders start at 1; 0 = "nothing", always complete).
uint64_t CurrentOrder();
// Submits a command list and signals the order's fence. Returns its order.
uint64_t Submit(DkCmdList list);
// Highest order the GPU has completed (polls fences, never waits).
uint64_t CompletedOrder();
// Waits until `order` is complete. false on timeout or a queue in error state (logged once).
bool WaitOrder(uint64_t order, int64_t timeout_ns = -1);
void WaitIdle();
// Releases everything queued with an order the GPU has completed (Submit and CompletedOrder call it).
void CollectReleases();

// --- CPU-visible memory -----------------------------------------------------------------------------------------

struct Block {
  DkMemBlock memory = nullptr;
  uint8_t* cpu = nullptr;  // nullptr for GPU-only blocks
  DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
  uint32_t size = 0;
  uint32_t flags = 0;
  explicit operator bool() const { return memory != nullptr; }
};
// size rounded up to DK_MEMBLOCK_ALIGNMENT. `what` names the block in the log when creation fails.
Block MakeBlock(uint32_t size, uint32_t flags, const char* what);
// Frees once the GPU completed `after_order` (0 = now: only when the GPU cannot be using it).
void FreeBlock(Block& block, uint64_t after_order);
// CpuCached blocks: makes CPU writes visible to the GPU (no-op for CpuUncached).
void FlushCpu(const Block& block, uint32_t offset, uint32_t size);
constexpr uint32_t kCpuToGpu = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
constexpr uint32_t kReadback = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;

// Bump allocator over one CPU-visible block (one per work slot: uploads, uniform buffers, push constants).
class Arena {
 public:
  bool Initialize(uint32_t size, uint32_t flags, const char* what);
  void Release(uint64_t after_order);
  // Same contract as the draws' Reserve: false when it does not fit (the caller submits and retries).
  bool Reserve(uint32_t bytes, uint32_t alignment, uint32_t& offset);
  uint8_t* Cpu(uint32_t offset) const { return block_.cpu + offset; }
  DkGpuAddr Gpu(uint32_t offset) const { return block_.gpu + offset; }
  // Reserve + copy; DK_GPU_ADDR_INVALID if it does not fit. Uniform buffers: alignment DK_UNIFORM_BUF_ALIGNMENT.
  DkGpuAddr Push(const void* data, uint32_t bytes, uint32_t alignment = DK_UNIFORM_BUF_ALIGNMENT);
  void Reset() { used_ = 0; }
  void FlushUsed() const { FlushCpu(block_, 0, used_); }
  uint32_t Used() const { return used_; }
  uint32_t Size() const { return block_.size; }
  const Block& block() const { return block_; }

 private:
  Block block_;
  uint32_t used_ = 0;
};

// --- Images -----------------------------------------------------------------------------------------------------

// Stable image id: bits 0-19 index, 20-31 generation. 0 = none. Swapping two ImageIds swaps everything (memory,
// descriptors); a stale id (destroyed image) is detected by its generation.
using ImageId = uint32_t;
constexpr ImageId kNoImage = 0;

struct ImageDesc {
  DkImageType type = DkImageType_2D;
  DkImageFormat format = DkImageFormat_None;
  uint32_t width = 0, height = 0;
  uint32_t depth_or_layers = 1;  // 3D depth, array layers, 6 for a cube
  uint32_t mips = 1;
  uint32_t flags = 0;            // DkImageFlags_* (UsageRender, UsageLoadStore, Usage2DEngine, HwCompression...)
  DkMsMode ms = DkMsMode_1x;
  VkFormat logical = VK_FORMAT_UNDEFINED;  // the Vulkan code's format, kept for keys and logs
};

// Creates the layout, the memory (sub-allocated; at least kDedicatedBytes = its own block) and the DkImage.
// kNoImage if memory ran out (the caller frees textures and retries, as the Vulkan code does).
ImageId CreateImage(const ImageDesc& desc, const char* what);
// The memory and the id are released after `after_order` (default: everything recorded so far).
void DestroyImage(ImageId id, uint64_t after_order);
void DestroyImage(ImageId id);
bool Alive(ImageId id);
const DkImage* Image(ImageId id);       // nullptr if not alive
const ImageDesc& Desc(ImageId id);      // a static empty one if not alive
uint64_t Bytes(ImageId id);             // dkImageLayoutGetSize
// A view made on the spot (DkImageView holds a pointer to the DkImage, which never moves).
DkImageView View(ImageId id, DkImageFormat format = DkImageFormat_None, DkDsSource source = DkDsSource_Depth);
struct ViewDesc {
  DkImageType type = DkImageType_None;  // None = the image's
  DkImageFormat format = DkImageFormat_None;
  DkImageSwizzle swizzle[4] = {DkImageSwizzle_Red, DkImageSwizzle_Green, DkImageSwizzle_Blue, DkImageSwizzle_Alpha};
  DkDsSource source = DkDsSource_Depth;
  uint16_t first_layer = 0, layers = 0;  // 0 = all
  uint8_t first_mip = 0, mips = 0;       // 0 = all
};
DkImageView View(ImageId id, const ViewDesc& desc);
// Totals for the reports (bytes of live images, blocks, wasted space at the end of blocks).
std::string ImagesSummary();

// --- Descriptors ------------------------------------------------------------------------------------------------

// One image set and one sampler set for the whole process, bound once per command list.
//   images [0, kDescriptorsDraws): the draws' texture heaps, which the game shaders index: 2D 0-4095, 3D
//          4096-4111, cube 4112-4175 (reports 05/06). The draws write them in place and keep their rule of
//          never rewriting a slot a submission in flight may read;
//   images [kDescriptorsDraws, kDescriptorsPersistentEnd): AllocImageDescriptor (render targets, resolved
//          textures, the renderer's own passes);
//   images [kDescriptorsPersistentEnd, kImageDescriptors): per-slot transient rings (TransientDescriptors).
constexpr uint32_t kDescriptorsDraws2D = 0;
constexpr uint32_t kDescriptorsDraws3D = 4096;
constexpr uint32_t kDescriptorsDrawsCube = 4112;
constexpr uint32_t kDescriptorsDraws = 4176;
constexpr uint32_t kDescriptorsPersistentEnd = 12288;
constexpr uint32_t kImageDescriptors = 16384;
constexpr uint32_t kSamplerDescriptors = 1024;  // the draws' samplers 0-511, the rest AllocSamplerDescriptor
constexpr uint32_t kSamplersDraws = 512;

uint32_t AllocImageDescriptor();                        // UINT32_MAX when full
void FreeImageDescriptor(uint32_t index, uint64_t after_order);
void WriteImageDescriptor(uint32_t index, const DkImageView& view, bool load_store = false, bool decay_ms = false);
uint32_t AllocSamplerDescriptor();
void FreeSamplerDescriptor(uint32_t index, uint64_t after_order);
void WriteSamplerDescriptor(uint32_t index, const DkSampler& sampler);
// Binds both sets (every new command list needs it).
void BindDescriptors(DkCmdBuf cmd);
// true once after any descriptor write: the next command list starts with Barrier(cmd, Hazard::Descriptors).
bool TakeDescriptorsWritten();
// The game shaders' bindless handle.
constexpr DkResHandle Handle(uint32_t image, uint32_t sampler) { return image | (sampler << 20); }

// A work slot's transient image descriptors (views of another format, stencil views, storage images of the
// conversions): handed out in order and rewound with Reset() when the slot's fence has passed.
class TransientDescriptors {
 public:
  void Initialize(uint32_t first, uint32_t count) { first_ = first; count_ = count; used_ = 0; }
  uint32_t Push(const DkImageView& view, bool load_store = false, bool decay_ms = false);  // UINT32_MAX if full
  void Reset() { used_ = 0; }
  uint32_t Used() const { return used_; }

 private:
  uint32_t first_ = 0, count_ = 0, used_ = 0;
};
// The ring of slot `slot` out of `slots` (equal parts of [kDescriptorsPersistentEnd, kImageDescriptors)).
TransientDescriptors TransientRing(uint32_t slot, uint32_t slots);

// --- Command streams --------------------------------------------------------------------------------------------

// Bytes left unused at the end of every chunk fed to a command buffer (the dkCmdBufBindVtxBuffers overrun).
constexpr uint32_t kCommandGuardBytes = 4096;

class CmdStream {
 public:
  bool Initialize(uint32_t bytes, const char* what, uint32_t chunk_bytes = 64u << 10);
  void Release(uint64_t after_order);
  DkCmdBuf cmd() const { return cmd_; }
  // Rewinds the memory: only once the GPU completed every list finished from it.
  void Reset();
  DkCmdList Finish() { return dkCmdBufFinishList(cmd_); }
  uint64_t BytesFed() const { return fed_; }
  uint32_t Overflows() const { return overflows_; }

 private:
  static void AddMemory(void* user, DkCmdBuf cmd, size_t minimum);
  void Feed(size_t minimum);
  DkCmdBuf cmd_ = nullptr;
  std::vector<Block> blocks_;  // [0] = main; more on overflow, kept for the next frames
  size_t block_ = 0;
  uint32_t offset_ = 0, chunk_ = 0;
  uint64_t fed_ = 0;
  uint32_t overflows_ = 0;
  const char* what_ = "";
};

// --- Counters (timestamps and statistics) -----------------------------------------------------------------------

// A block of DkCounter reports, 16 bytes each (value, timestamp), read after the fence.
class Counters {
 public:
  bool Initialize(uint32_t reports, const char* what);
  void Release(uint64_t after_order);
  void Report(DkCmdBuf cmd, uint32_t index, DkCounter counter = DkCounter_Timestamp);
  uint64_t Value(uint32_t index) const;  // the counter (statistics)
  uint64_t Ticks(uint32_t index) const;  // the GPU timestamp, raw ticks (x 625/384 = ns)
  uint32_t Capacity() const { return reports_; }

 private:
  Block block_;
  uint32_t reports_ = 0;
};

// --- Barriers ---------------------------------------------------------------------------------------------------

// Every barrier names the dependency it serves. The mapping (sa2_dk.cpp) starts conservative and is tuned from
// the semantics bench; sa2_dk_barrier_full forces DkBarrier_Full with every invalidation for all of them.
enum class Hazard : uint8_t {
  RenderToCopy,      // a render target the 3D engine wrote is read by DMA copy / 2D blit
  RenderToSample,    // ... sampled or loaded by a later draw or dispatch
  CopyToRender,      // a DMA copy / 2D blit wrote what the 3D engine reads or renders into next
  CopyToSample,      // ... what a later draw or dispatch samples
  ComputeToAny,      // a dispatch wrote an image read or written by anything later
  AnyToCompute,      // a dispatch reads or writes what earlier work wrote
  UploadToSample,    // buffer-to-image copies (texture uploads) before the draws that sample them
  DepthWrite,        // depth written by copy / compute / fragment as color: Zcull state is stale
  Descriptors,       // descriptor memory rewritten
  ShaderCode,        // code memory written (CodeArena)
  Full,              // everything, both ways
  kCount
};
void Barrier(DkCmdBuf cmd, Hazard hazard);
// Barriers emitted per Hazard since the start (for the reports).
std::array<uint64_t, size_t(Hazard::kCount)> BarrierCounts();

// --- Shaders ----------------------------------------------------------------------------------------------------

// DKSH (uam output) loaded into code memory that is never freed (blocks of 8 MB, more on demand).
class CodeArena {
 public:
  bool Initialize(uint32_t block_bytes = 8u << 20);
  // nullptr if the data is not a valid DKSH. The DkShader lives as long as the arena.
  const DkShader* Load(const void* dksh, size_t bytes, const char* what);
  // true once after any Load: the next command list starts with Barrier(cmd, Hazard::ShaderCode).
  bool TakeWritten();
  uint64_t BytesUsed() const { return used_total_; }

 private:
  std::vector<Block> blocks_;
  std::vector<std::unique_ptr<DkShader>> shaders_;
  uint32_t offset_ = 0;
  uint64_t used_total_ = 0;
  uint32_t block_bytes_ = 0;
  bool written_ = false;
};

// --- Fixed-function state for the renderer's own full-screen and compute passes ---------------------------------

struct PassState {
  DkRasterizerState raster;
  DkColorState color;
  DkColorWriteState write;
  DkDepthStencilState depth;
  DkMultisampleState ms;
  PassState();  // no cull, no blend, RGBA writes, no depth/stencil, single sample
  void Bind(DkCmdBuf cmd) const;
};

// --- Window -----------------------------------------------------------------------------------------------------

class Window {
 public:
  // Swapchain of `images` (4, as the Vulkan presenter's kSubmissionCount + 1: with 2 libnx forces interval 1)
  // on nwindowGetDefault(). Interval 0 from the start (the frame pacing is the game's and __wrap_nwindowQueueBuffer).
  bool Create(uint32_t width, uint32_t height, uint32_t images = 4, DkImageFormat format = DkImageFormat_RGBA8_Unorm);
  void Destroy();
  void SetInterval(uint32_t interval);
  int Acquire();                 // dkQueueAcquireImage
  DkImageView View(int slot);    // a render target view of that image
  void Present(int slot);        // dkQueuePresentImage
  uint32_t Width() const { return width_; }
  uint32_t Height() const { return height_; }

 private:
  DkSwapchain swapchain_ = nullptr;
  Block memory_;
  std::array<DkImage, 4> images_{};
  uint32_t count_ = 0, width_ = 0, height_ = 0;
};

// --- Formats ----------------------------------------------------------------------------------------------------

// The host formats the Vulkan renderer uses (render targets, resolved textures, depth) to deko3d.
// DkImageFormat_None = no equivalent (the caller rejects, as with VK_FORMAT_UNDEFINED).
DkImageFormat FromVk(VkFormat format);
bool IsDepth(DkImageFormat format);
bool HasStencil(DkImageFormat format);
uint32_t BytesPerTexel(DkImageFormat format);  // 0 for compressed formats
DkMsMode FromSamples(uint32_t samples);
DkImageRect Rect(const VkRect2D& rect);
DkImageRect Rect(int32_t x, int32_t y, uint32_t width, uint32_t height);

}  // namespace sa2::dk
#endif  // SA2_DK_AVAILABLE
