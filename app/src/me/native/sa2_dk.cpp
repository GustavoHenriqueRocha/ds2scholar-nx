// SA2 - native renderer on deko3d: the base layer (sa2_dk.h).

#include "sa2_dk.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#if SA2_DK_AVAILABLE
#include <switch.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

REXCVAR_DEFINE_BOOL(sa2_dk_barrier_full, false, "SA2 renderer",
                    "deko3d renderer (diagnosis): every named barrier becomes DkBarrier_Full with every invalidation. "
                    "A picture that changes with it on points at a barrier mapping (sa2_dk.cpp, Barrier)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sa2::dk {
namespace {

constexpr uint32_t kFences = 64;               // submissions in flight at most (more waits for the oldest)
constexpr uint32_t kDkshMagic = 0x48534B44;    // "DKSH"

uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

void DebugCallback(void*, const char* context, DkResult result, const char* message) {
  REXLOG_WARN("[dk] {}: result {}: {}", context ? context : "?", int(result), message ? message : "");
}

struct FenceSlot {
  DkFence fence{};
  uint64_t order = 0;
};

// What waits for an order before it is released.
struct Release {
  enum Kind : uint8_t { kBlock, kImageMemory, kImageSlot, kImageDescriptor, kSamplerDescriptor };
  uint64_t order;
  Kind kind;
  DkMemBlock block = nullptr;   // kBlock
  uint32_t slab = 0, offset = 0, size = 0;  // kImageMemory
  uint32_t index = 0;           // kImageSlot, descriptors
};

// One image block: a sub-allocator with a free list (offset -> size), coalescing on free.
struct Slab {
  Block block;
  std::map<uint32_t, uint32_t> free;
  bool dedicated = false;
  uint32_t used = 0;
};

struct ImageObject {
  DkImageLayout layout{};
  DkImage image{};
  ImageDesc desc;
  uint32_t slab = 0, offset = 0, size = 0;
  uint64_t bytes = 0;
  uint16_t generation = 1;
  bool alive = false;
};

struct State {
  DkDevice device = nullptr;
  DkQueue queue = nullptr;
  std::array<FenceSlot, kFences> fences{};
  uint64_t next_order = 1;
  uint64_t completed = 0;
  std::deque<Release> releases;
  bool error_logged = false;
  // Images.
  std::vector<Slab> slabs;
  std::deque<ImageObject> images;  // never moves: DkImageView points into it
  std::vector<uint32_t> free_images;
  uint32_t slabs_on_demand = 0;
  // Descriptors.
  Block image_descriptors, sampler_descriptors;
  std::vector<uint32_t> free_image_descriptors, free_sampler_descriptors;
  uint64_t descriptors_generation = 0;
  std::array<uint64_t, size_t(Hazard::kCount)> barriers{};
};

State* g = nullptr;
const ImageDesc kNoDesc{};

ImageObject* Object(ImageId id) {
  if (!g || id == kNoImage) return nullptr;
  const uint32_t index = id & 0xFFFFF;
  if (index >= g->images.size()) return nullptr;
  ImageObject& object = g->images[index];
  return object.alive && object.generation == (id >> 20) ? &object : nullptr;
}

bool SlabAlloc(Slab& slab, uint32_t size, uint32_t alignment, uint32_t& offset) {
  for (auto it = slab.free.begin(); it != slab.free.end(); ++it) {
    const uint32_t start = AlignUp(it->first, alignment);
    const uint32_t end = it->first + it->second;
    if (start >= end || end - start < size) continue;
    const uint32_t before = start - it->first, after = end - (start + size), first = it->first;
    slab.free.erase(it);
    if (before) slab.free[first] = before;
    if (after) slab.free[start + size] = after;
    offset = start;
    slab.used += size;
    return true;
  }
  return false;
}

void SlabFree(Slab& slab, uint32_t offset, uint32_t size) {
  slab.used -= size;
  auto next = slab.free.lower_bound(offset);
  if (next != slab.free.end() && offset + size == next->first) {
    size += next->second;
    next = slab.free.erase(next);
  }
  if (next != slab.free.begin()) {
    auto previous = std::prev(next);
    if (previous->first + previous->second == offset) {
      previous->second += size;
      return;
    }
  }
  slab.free[offset] = size;
}

void ReleaseNow(const Release& r) {
  switch (r.kind) {
    case Release::kBlock:
      dkMemBlockDestroy(r.block);
      break;
    case Release::kImageMemory: {
      Slab& slab = g->slabs[r.slab];
      SlabFree(slab, r.offset, r.size);
      if (slab.dedicated && slab.block) {
        dkMemBlockDestroy(slab.block.memory);
        slab.block = {};
        slab.free.clear();
      }
      break;
    }
    case Release::kImageSlot:
      g->free_images.push_back(r.index);
      break;
    case Release::kImageDescriptor:
      g->free_image_descriptors.push_back(r.index);
      break;
    case Release::kSamplerDescriptor:
      g->free_sampler_descriptors.push_back(r.index);
      break;
  }
}

void Defer(Release r) {
  if (r.order <= CompletedOrder()) {
    ReleaseNow(r);
  } else {
    g->releases.push_back(r);
  }
}

}  // namespace

// --- Device ---------------------------------------------------------------------------------------------------

bool Initialize(const DeviceConfig& config) {
  if (g) return true;
  g = new State();
  DkDeviceMaker device_maker;
  dkDeviceMakerDefaults(&device_maker);
  device_maker.cbDebug = DebugCallback;
  g->device = dkDeviceCreate(&device_maker);
  if (!g->device) {
    REXLOG_ERROR("[dk] could not create the device");
    Shutdown();
    return false;
  }
  DkQueueMaker queue_maker;
  dkQueueMakerDefaults(&queue_maker, g->device);
  queue_maker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_EnableZcull;
  queue_maker.maxConcurrentComputeJobs = config.compute_jobs;
  queue_maker.commandMemorySize = std::max<uint32_t>(config.queue_command_bytes, DK_QUEUE_MIN_CMDMEM_SIZE);
  queue_maker.flushThreshold = queue_maker.commandMemorySize / 8;
  g->queue = dkQueueCreate(&queue_maker);
  if (!g->queue) {
    REXLOG_ERROR("[dk] could not create the queue");
    Shutdown();
    return false;
  }
  g->images.emplace_back();  // index 0 = kNoImage
  g->image_descriptors = MakeBlock(kImageDescriptors * sizeof(DkImageDescriptor), kCpuToGpu, "image descriptors");
  g->sampler_descriptors =
      MakeBlock(kSamplerDescriptors * sizeof(DkSamplerDescriptor), kCpuToGpu, "sampler descriptors");
  if (!g->image_descriptors || !g->sampler_descriptors) {
    Shutdown();
    return false;
  }
  std::memset(g->image_descriptors.cpu, 0, g->image_descriptors.size);
  std::memset(g->sampler_descriptors.cpu, 0, g->sampler_descriptors.size);
  for (uint32_t i = kDescriptorsPersistentEnd; i-- > kDescriptorsDraws;) g->free_image_descriptors.push_back(i);
  for (uint32_t i = kSamplerDescriptors; i-- > kSamplersDraws;) g->free_sampler_descriptors.push_back(i);
  REXLOG_INFO("[dk] device and queue (Graphics | Compute, Zcull; {} compute jobs, {} KB of queue commands)",
              config.compute_jobs, queue_maker.commandMemorySize >> 10);
  return true;
}

void Shutdown() {
  if (!g) return;
  if (g->queue) dkQueueWaitIdle(g->queue);
  g->completed = g->next_order - 1;
  for (const Release& r : g->releases) ReleaseNow(r);
  g->releases.clear();
  for (Slab& slab : g->slabs)
    if (slab.block) dkMemBlockDestroy(slab.block.memory);
  if (g->image_descriptors) dkMemBlockDestroy(g->image_descriptors.memory);
  if (g->sampler_descriptors) dkMemBlockDestroy(g->sampler_descriptors.memory);
  if (g->queue) dkQueueDestroy(g->queue);
  if (g->device) dkDeviceDestroy(g->device);
  delete g;
  g = nullptr;
}

bool Ready() { return g && g->queue; }
DkDevice Device() { return g ? g->device : nullptr; }
DkQueue Queue() { return g ? g->queue : nullptr; }
uint64_t CurrentOrder() { return g ? g->next_order : 0; }

uint64_t Submit(DkCmdList list) {
  const uint64_t order = g->next_order;
  FenceSlot& slot = g->fences[order % kFences];
  if (slot.order && slot.order > g->completed) WaitOrder(slot.order);  // ring full: the oldest must be done
  dkQueueSubmitCommands(g->queue, list);
  dkQueueSignalFence(g->queue, &slot.fence, true);  // flush: the GPU starts now, as vkQueueSubmit
  slot.order = order;
  ++g->next_order;
  CollectReleases();
  return order;
}

void SubmitUnfenced(DkCmdList list) {
  dkQueueSubmitCommands(g->queue, list);
  dkQueueFlush(g->queue);
}

uint64_t CompletedOrder() {
  if (!g) return 0;
  while (g->completed + 1 < g->next_order) {
    FenceSlot& slot = g->fences[(g->completed + 1) % kFences];
    if (slot.order != g->completed + 1 || dkFenceWait(&slot.fence, 0) != DkResult_Success) break;
    ++g->completed;
  }
  return g->completed;
}

bool WaitOrder(uint64_t order, int64_t timeout_ns) {
  if (!g || order <= CompletedOrder()) return true;
  if (order >= g->next_order) order = g->next_order - 1;  // not submitted yet: wait for what was
  FenceSlot& slot = g->fences[order % kFences];
  if (slot.order == order && dkFenceWait(&slot.fence, timeout_ns) != DkResult_Success) {
    if (dkQueueIsInErrorState(g->queue) && !g->error_logged) {
      g->error_logged = true;
      REXLOG_ERROR("[dk] the queue is in error state (order {})", order);
    }
    return false;
  }
  // Fences signal in order: everything up to `order` is done.
  g->completed = std::max(g->completed, order);
  CompletedOrder();
  return true;
}

void WaitIdle() {
  if (!g) return;
  dkQueueWaitIdle(g->queue);
  g->completed = g->next_order - 1;
  CollectReleases();
}

void CollectReleases() {
  if (!g) return;
  const uint64_t done = CompletedOrder();
  while (!g->releases.empty() && g->releases.front().order <= done) {
    ReleaseNow(g->releases.front());
    g->releases.pop_front();
  }
  // Releases are queued in order of their `order` almost always; a later one stuck behind an earlier one only
  // waits one more Submit.
}

// --- Blocks and arenas ----------------------------------------------------------------------------------------

Block MakeBlock(uint32_t size, uint32_t flags, const char* what) {
  Block block;
  DkMemBlockMaker maker;
  dkMemBlockMakerDefaults(&maker, g->device, AlignUp(std::max<uint32_t>(size, 1), DK_MEMBLOCK_ALIGNMENT));
  maker.flags = flags;
  block.memory = dkMemBlockCreate(&maker);
  if (!block.memory) {
    REXLOG_ERROR("[dk] could not create a {} KB block ({})", maker.size >> 10, what);
    return {};
  }
  block.size = maker.size;
  block.flags = flags;
  block.gpu = dkMemBlockGetGpuAddr(block.memory);
  if ((flags & DkMemBlockFlags_CpuAccessMask) != 0) block.cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block.memory));
  return block;
}

void FreeBlock(Block& block, uint64_t after_order) {
  if (!block.memory) return;
  Release r{after_order, Release::kBlock};
  r.block = block.memory;
  Defer(r);
  block = {};
}

void FlushCpu(const Block& block, uint32_t offset, uint32_t size) {
  if (block.memory && size && (block.flags & DkMemBlockFlags_CpuAccessMask) == DkMemBlockFlags_CpuCached)
    dkMemBlockFlushCpuCache(block.memory, offset, size);
}

void InvalidateCpu(const Block& block, uint32_t offset, uint32_t size) {
  // deko3d has no invalidate call: armDCacheFlush is clean + invalidate by line (DC CIVAC), safe on a block the
  // CPU did not write.
  if (block.memory && size && (block.flags & DkMemBlockFlags_CpuAccessMask) == DkMemBlockFlags_CpuCached)
    armDCacheFlush(block.cpu + offset, size);
}

bool Arena::Initialize(uint32_t size, uint32_t flags, const char* what) {
  block_ = MakeBlock(size, flags, what);
  used_ = 0;
  return bool(block_);
}

void Arena::Release(uint64_t after_order) { FreeBlock(block_, after_order); }

bool Arena::Reserve(uint32_t bytes, uint32_t alignment, uint32_t& offset) {
  const uint32_t start = AlignUp(used_, std::max<uint32_t>(alignment, 1));
  if (start > block_.size || block_.size - start < bytes) return false;
  offset = start;
  used_ = start + bytes;
  return true;
}

DkGpuAddr Arena::Push(const void* data, uint32_t bytes, uint32_t alignment) {
  uint32_t offset = 0;
  if (!Reserve(bytes, alignment, offset)) return DK_GPU_ADDR_INVALID;
  std::memcpy(block_.cpu + offset, data, bytes);
  return block_.gpu + offset;
}

// --- Images ---------------------------------------------------------------------------------------------------

ImageId CreateImage(const ImageDesc& desc, const char* what) {
  if (!g || desc.format == DkImageFormat_None || !desc.width || !desc.height) return kNoImage;
  DkImageLayoutMaker maker;
  dkImageLayoutMakerDefaults(&maker, g->device);
  maker.type = desc.type;
  maker.flags = desc.flags;
  maker.format = desc.format;
  maker.msMode = desc.ms;
  maker.dimensions[0] = desc.width;
  maker.dimensions[1] = desc.height;
  maker.dimensions[2] = desc.depth_or_layers;
  maker.mipLevels = desc.mips;
  uint32_t index;
  if (!g->free_images.empty()) {
    index = g->free_images.back();
    g->free_images.pop_back();
  } else {
    if (g->images.size() > 0xFFFFF) return kNoImage;
    index = uint32_t(g->images.size());
    g->images.emplace_back();
  }
  ImageObject& object = g->images[index];
  dkImageLayoutInitialize(&object.layout, &maker);
  const uint64_t size64 = dkImageLayoutGetSize(&object.layout);
  const uint32_t alignment = dkImageLayoutGetAlignment(&object.layout);
  const uint32_t size = AlignUp(uint32_t(size64), alignment);
  uint32_t slab_index = UINT32_MAX, offset = 0;
  if (size >= kDedicatedBytes) {
    Slab slab;
    slab.block = MakeBlock(size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, what);
    if (slab.block) {
      slab.dedicated = true;
      slab.used = size;
      for (uint32_t i = 0; i < g->slabs.size(); ++i)
        if (!g->slabs[i].block && g->slabs[i].dedicated) slab_index = i;
      if (slab_index == UINT32_MAX) {
        slab_index = uint32_t(g->slabs.size());
        g->slabs.push_back(std::move(slab));
      } else {
        g->slabs[slab_index] = std::move(slab);
      }
    }
  } else {
    for (uint32_t i = 0; i < g->slabs.size() && slab_index == UINT32_MAX; ++i)
      if (!g->slabs[i].dedicated && g->slabs[i].block && SlabAlloc(g->slabs[i], size, alignment, offset)) slab_index = i;
    if (slab_index == UINT32_MAX) {
      Slab slab;
      slab.block = MakeBlock(kSlabBytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, what);
      if (slab.block) {
        ++g->slabs_on_demand;
        slab.free[0] = kSlabBytes;
        slab_index = uint32_t(g->slabs.size());
        g->slabs.push_back(std::move(slab));
        SlabAlloc(g->slabs[slab_index], size, alignment, offset);
      }
    }
  }
  if (slab_index == UINT32_MAX) {
    g->free_images.push_back(index);
    return kNoImage;
  }
  dkImageInitialize(&object.image, &object.layout, g->slabs[slab_index].block.memory, offset);
  object.desc = desc;
  object.slab = slab_index;
  object.offset = offset;
  object.size = size;
  object.bytes = size64;
  object.alive = true;
  return index | (uint32_t(object.generation) << 20);
}

void DestroyImage(ImageId id, uint64_t after_order) {
  ImageObject* object = Object(id);
  if (!object) return;
  const uint32_t index = id & 0xFFFFF;
  object->alive = false;
  object->generation = uint16_t((object->generation + 1) & 0xFFF);
  if (!object->generation) object->generation = 1;
  Release memory{after_order, Release::kImageMemory};
  memory.slab = object->slab;
  memory.offset = object->offset;
  memory.size = object->size;
  Defer(memory);
  Release slot{after_order, Release::kImageSlot};
  slot.index = index;
  Defer(slot);
}

void DestroyImage(ImageId id) { DestroyImage(id, CurrentOrder()); }
bool Alive(ImageId id) { return Object(id) != nullptr; }

const DkImage* Image(ImageId id) {
  ImageObject* object = Object(id);
  return object ? &object->image : nullptr;
}

const ImageDesc& Desc(ImageId id) {
  ImageObject* object = Object(id);
  return object ? object->desc : kNoDesc;
}

uint64_t Bytes(ImageId id) {
  ImageObject* object = Object(id);
  return object ? object->bytes : 0;
}

DkImageView View(ImageId id, DkImageFormat format, DkDsSource source) {
  ViewDesc desc;
  desc.format = format;
  desc.source = source;
  return View(id, desc);
}

DkImageView View(ImageId id, const ViewDesc& desc) {
  DkImageView view;
  ImageObject* object = Object(id);
  dkImageViewDefaults(&view, object ? &object->image : nullptr);
  if (!object) {
    // A dead or stale id: the caller records nothing with it (pImage is null). Logged a few times, so a
    // use-after-destroy shows in the log instead of as a missing copy or texture.
    static uint32_t logged = 0;
    if (id != kNoImage && logged < 16) {
      ++logged;
      REXLOG_WARN("[dk] view of a dead image id {:#x} (slot {}, generation {})", id, id & 0xFFFFF, id >> 20);
    }
    return view;
  }
  if (desc.type != DkImageType_None) view.type = desc.type;
  if (desc.format != DkImageFormat_None) view.format = desc.format;
  for (uint32_t i = 0; i < 4; ++i) view.swizzle[i] = desc.swizzle[i];
  view.dsSource = desc.source;
  view.layerOffset = desc.first_layer;
  view.layerCount = desc.layers;
  view.mipLevelOffset = desc.first_mip;
  view.mipLevelCount = desc.mips;
  return view;
}

std::string ImagesSummary() {
  if (!g) return "no device";
  uint64_t blocks = 0, used = 0, dedicated = 0;
  uint32_t live = 0, slabs = 0;
  for (const Slab& slab : g->slabs) {
    if (!slab.block) continue;
    blocks += slab.block.size;
    used += slab.used;
    if (slab.dedicated) ++dedicated;
    else ++slabs;
  }
  for (const ImageObject& object : g->images) live += object.alive ? 1 : 0;
  return fmt::format("{} images, {:.1f} of {:.1f} MB in {} blocks of 32 MB and {} dedicated", live,
                     double(used) / 1048576.0, double(blocks) / 1048576.0, slabs, dedicated);
}

uint32_t AddImageSlabs(uint32_t count, const char* what) {
  if (!g) return 0;
  uint32_t made = 0;
  for (; made < count; ++made) {
    Slab slab;
    slab.block = MakeBlock(kSlabBytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, what);
    if (!slab.block) break;
    slab.free[0] = kSlabBytes;
    g->slabs.push_back(std::move(slab));
  }
  return made;
}

uint64_t ImageSlabsFreeBytes() {
  uint64_t free = 0;
  if (g)
    for (const Slab& slab : g->slabs)
      if (slab.block && !slab.dedicated) free += slab.block.size - slab.used;
  return free;
}

uint64_t ImageSlabsLargestGap() {
  uint64_t largest = 0;
  if (g)
    for (const Slab& slab : g->slabs)
      if (slab.block && !slab.dedicated)
        for (const auto& [offset, size] : slab.free) largest = std::max<uint64_t>(largest, size);
  return largest;
}

uint32_t ImageSlabsOnDemand() { return g ? g->slabs_on_demand : 0; }

// --- Descriptors ----------------------------------------------------------------------------------------------

uint32_t AllocImageDescriptor() {
  if (!g || g->free_image_descriptors.empty()) return UINT32_MAX;
  const uint32_t index = g->free_image_descriptors.back();
  g->free_image_descriptors.pop_back();
  return index;
}

void FreeImageDescriptor(uint32_t index, uint64_t after_order) {
  if (index < kDescriptorsDraws || index >= kDescriptorsPersistentEnd) return;
  Release r{after_order, Release::kImageDescriptor};
  r.index = index;
  Defer(r);
}

void WriteImageDescriptor(uint32_t index, const DkImageView& view, bool load_store, bool decay_ms) {
  if (!g || index >= kImageDescriptors) return;
  auto* descriptors = reinterpret_cast<DkImageDescriptor*>(g->image_descriptors.cpu);
  dkImageDescriptorInitialize(&descriptors[index], &view, load_store, decay_ms);
  ++g->descriptors_generation;
}

uint32_t AllocSamplerDescriptor() {
  if (!g || g->free_sampler_descriptors.empty()) return UINT32_MAX;
  const uint32_t index = g->free_sampler_descriptors.back();
  g->free_sampler_descriptors.pop_back();
  return index;
}

void FreeSamplerDescriptor(uint32_t index, uint64_t after_order) {
  if (index < kSamplersDraws || index >= kSamplerDescriptors) return;
  Release r{after_order, Release::kSamplerDescriptor};
  r.index = index;
  Defer(r);
}

void WriteSamplerDescriptor(uint32_t index, const DkSampler& sampler) {
  if (!g || index >= kSamplerDescriptors) return;
  auto* descriptors = reinterpret_cast<DkSamplerDescriptor*>(g->sampler_descriptors.cpu);
  dkSamplerDescriptorInitialize(&descriptors[index], &sampler);
  ++g->descriptors_generation;
}

void BindDescriptors(DkCmdBuf cmd) {
  dkCmdBufBindImageDescriptorSet(cmd, g->image_descriptors.gpu, kImageDescriptors);
  dkCmdBufBindSamplerDescriptorSet(cmd, g->sampler_descriptors.gpu, kSamplerDescriptors);
}

bool DescriptorsWrittenSince(uint64_t& seen) {
  if (!g || g->descriptors_generation == seen) return false;
  seen = g->descriptors_generation;
  return true;
}

uint64_t DescriptorsGeneration() { return g ? g->descriptors_generation : 0; }

uint32_t TransientDescriptors::Push(const DkImageView& view, bool load_store, bool decay_ms) {
  if (used_ >= count_) return UINT32_MAX;
  const uint32_t index = first_ + used_++;
  WriteImageDescriptor(index, view, load_store, decay_ms);
  return index;
}

TransientDescriptors TransientRing(uint32_t slot, uint32_t slots) {
  TransientDescriptors ring;
  const uint32_t each = (kImageDescriptors - kDescriptorsPersistentEnd) / std::max<uint32_t>(slots, 1);
  ring.Initialize(kDescriptorsPersistentEnd + slot * each, each);
  return ring;
}

// --- Command streams ------------------------------------------------------------------------------------------

bool CmdStream::Initialize(uint32_t bytes, const char* what, uint32_t chunk_bytes) {
  what_ = what;
  chunk_ = std::max<uint32_t>(chunk_bytes, 2 * kCommandGuardBytes);
  Block main = MakeBlock(bytes, kCpuToGpu, what);
  if (!main) return false;
  blocks_.push_back(main);
  DkCmdBufMaker maker;
  dkCmdBufMakerDefaults(&maker, g->device);
  maker.userData = this;
  maker.cbAddMem = &CmdStream::AddMemory;
  cmd_ = dkCmdBufCreate(&maker);
  if (!cmd_) return false;
  Reset();
  return true;
}

void CmdStream::Release(uint64_t after_order) {
  if (cmd_) dkCmdBufDestroy(cmd_);  // the lists already submitted keep running from the blocks below
  cmd_ = nullptr;
  for (Block& block : blocks_) FreeBlock(block, after_order);
  blocks_.clear();
}

void CmdStream::Reset() {
  dkCmdBufClear(cmd_);
  block_ = 0;
  offset_ = 0;
  Feed(0);
}

void CmdStream::AddMemory(void* user, DkCmdBuf, size_t minimum) { static_cast<CmdStream*>(user)->Feed(minimum); }

void CmdStream::Feed(size_t minimum) {
  // Each piece leaves kCommandGuardBytes unused after it (dkCmdBufBindVtxBuffers writes past what it reserves).
  const uint32_t piece = std::max<uint32_t>(chunk_, AlignUp(uint32_t(minimum), DK_CMDMEM_ALIGNMENT) + kCommandGuardBytes);
  while (block_ < blocks_.size() && blocks_[block_].size - offset_ < piece) {
    ++block_;
    offset_ = 0;
  }
  if (block_ == blocks_.size()) {
    Block more = MakeBlock(std::max<uint32_t>(blocks_[0].size / 2, piece), kCpuToGpu, what_);
    if (!more) {
      REXLOG_ERROR("[dk] {}: out of command memory", what_);
      return;
    }
    blocks_.push_back(more);
    offset_ = 0;
    ++overflows_;
    REXLOG_WARN("[dk] {}: command memory overflow #{} ({} KB more)", what_, overflows_, more.size >> 10);
  }
  dkCmdBufAddMemory(cmd_, blocks_[block_].memory, offset_, piece - kCommandGuardBytes);
  offset_ += piece;
  fed_ += piece;
}

// --- Counters -------------------------------------------------------------------------------------------------

bool Counters::Initialize(uint32_t reports, const char* what) {
  // GPU uncached: the reports go straight to memory, readable by the CPU once the fence passed.
  block_ = MakeBlock(reports * 16, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached, what);
  reports_ = block_ ? reports : 0;
  if (block_) std::memset(block_.cpu, 0, block_.size);
  return bool(block_);
}

void Counters::Release(uint64_t after_order) { FreeBlock(block_, after_order); }

void Counters::Report(DkCmdBuf cmd, uint32_t index, DkCounter counter) {
  if (index < reports_) dkCmdBufReportCounter(cmd, counter, block_.gpu + index * 16u);
}

uint64_t Counters::Value(uint32_t index) const {
  uint64_t v = 0;
  if (index < reports_) std::memcpy(&v, block_.cpu + index * 16u, 8);
  return v;
}

uint64_t Counters::Ticks(uint32_t index) const {
  uint64_t v = 0;
  if (index < reports_) std::memcpy(&v, block_.cpu + index * 16u + 8, 8);
  return v;
}

// --- Barriers -------------------------------------------------------------------------------------------------

void Barrier(DkCmdBuf cmd, Hazard hazard) {
  static const bool full = REXCVAR_GET(sa2_dk_barrier_full);
  constexpr uint32_t kAll = DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                            DkInvalidateFlags_Zcull | DkInvalidateFlags_L2Cache;
  // Conservative until the semantics bench (ds2-deko3d bench-semantica, tests 3 and 2) says where less is
  // enough: it is not documented whether the DMA copy engine waits for the 3D engine by itself.
  DkBarrier mode = DkBarrier_Full;
  uint32_t invalidate = kAll;
  if (!full) {
    switch (hazard) {
      case Hazard::RenderToCopy: mode = DkBarrier_Full; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::RenderToSample: mode = DkBarrier_Fragments; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::CopyToRender: mode = DkBarrier_Full; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::CopyToSample: mode = DkBarrier_Full; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::CopyToAny: mode = DkBarrier_Full; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::ComputeToAny: mode = DkBarrier_Primitives; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::AnyToCompute: mode = DkBarrier_Primitives; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::UploadToSample: mode = DkBarrier_Full; invalidate = DkInvalidateFlags_Image; break;
      case Hazard::DepthWrite:
        mode = DkBarrier_Primitives;
        invalidate = DkInvalidateFlags_Image | DkInvalidateFlags_Zcull;
        break;
      case Hazard::Descriptors: mode = DkBarrier_None; invalidate = DkInvalidateFlags_Descriptors; break;
      case Hazard::ShaderCode: mode = DkBarrier_None; invalidate = DkInvalidateFlags_Shader; break;
      case Hazard::Full:
      case Hazard::kCount: break;
    }
  }
  dkCmdBufBarrier(cmd, mode, invalidate);
  if (g) ++g->barriers[size_t(hazard)];
}

std::array<uint64_t, size_t(Hazard::kCount)> BarrierCounts() { return g ? g->barriers : decltype(g->barriers){}; }

// --- Transfers ----------------------------------------------------------------------------------------------

void CopyImage(DkCmdBuf cmd, ImageId src, const DkImageRect& src_rect, ImageId dst, const DkImageRect& dst_rect,
               DkImageFormat src_view, DkImageFormat dst_view) {
  const DkImageView s = View(src, src_view), d = View(dst, dst_view);
  if (!s.pImage || !d.pImage) return;
  dkCmdBufCopyImage(cmd, &s, &src_rect, &d, &dst_rect, 0);
}

void Blit(DkCmdBuf cmd, ImageId src, const DkImageRect& src_rect, ImageId dst, const DkImageRect& dst_rect,
          bool linear) {
  const DkImageView s = View(src), d = View(dst);
  if (!s.pImage || !d.pImage) return;
  dkCmdBufBlitImage(cmd, &s, &src_rect, &d, &dst_rect, linear ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest,
                    0);
}

bool Has2D(ImageId id) { return (Desc(id).flags & DkImageFlags_Usage2DEngine) != 0; }

void ResolveWhole(DkCmdBuf cmd, ImageId src, ImageId dst) {
  const DkImageView s = View(src), d = View(dst);
  if (!s.pImage || !d.pImage) return;
  dkCmdBufResolveImage(cmd, &s, &d);
}

void CopyImageToBuffer(DkCmdBuf cmd, ImageId src, const DkImageRect& rect, DkGpuAddr dst, uint32_t row_bytes,
                       uint32_t layer_bytes) {
  const DkImageView s = View(src);
  if (!s.pImage) return;
  const DkCopyBuf buffer{dst, row_bytes, layer_bytes};
  dkCmdBufCopyImageToBuffer(cmd, &s, &rect, &buffer, 0);
}

void CopyBufferToImage(DkCmdBuf cmd, DkGpuAddr src, uint32_t row_bytes, uint32_t layer_bytes, ImageId dst,
                       const DkImageRect& rect, const ViewDesc* view) {
  const DkImageView d = view ? View(dst, *view) : View(dst);
  if (!d.pImage) return;
  const DkCopyBuf buffer{src, row_bytes, layer_bytes};
  dkCmdBufCopyBufferToImage(cmd, &buffer, &d, &rect, 0);
}

// --- Shaders --------------------------------------------------------------------------------------------------

bool CodeArena::Initialize(uint32_t block_bytes) {
  block_bytes_ = std::max<uint32_t>(block_bytes, 1u << 20);
  return true;
}

const DkShader* CodeArena::Load(const void* dksh, size_t bytes, const char* what) {
  struct DkshHeader {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
  } header;
  if (bytes < sizeof(header)) return nullptr;
  std::memcpy(&header, dksh, sizeof(header));
  if (header.magic != kDkshMagic || header.control_sz < sizeof(header) || !header.code_sz ||
      uint64_t(header.control_sz) + header.code_sz > bytes) {
    REXLOG_ERROR("[dk] {}: not a DKSH ({} bytes)", what, bytes);
    return nullptr;
  }
  // The last DK_SHADER_CODE_UNUSABLE_SIZE bytes of a code block cannot hold code (prefetch).
  const uint32_t need = AlignUp(header.code_sz, DK_SHADER_CODE_ALIGNMENT);
  if (blocks_.empty() || blocks_.back().size - DK_SHADER_CODE_UNUSABLE_SIZE < offset_ + need) {
    Block block = MakeBlock(std::max(block_bytes_, need + DK_SHADER_CODE_UNUSABLE_SIZE),
                            DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, what);
    if (!block) return nullptr;
    blocks_.push_back(block);
    offset_ = 0;
  }
  Block& block = blocks_.back();
  std::memcpy(block.cpu + offset_, static_cast<const uint8_t*>(dksh) + header.control_sz, header.code_sz);
  auto shader = std::make_unique<DkShader>();
  DkShaderMaker maker;
  dkShaderMakerDefaults(&maker, block.memory, offset_);
  maker.control = dksh;
  maker.programId = 0;
  dkShaderInitialize(shader.get(), &maker);
  offset_ += need;
  used_total_ += need;
  written_ = true;
  shaders_.push_back(std::move(shader));
  return shaders_.back().get();
}

bool CodeArena::TakeWritten() {
  const bool written = written_;
  written_ = false;
  return written;
}

// --- Pass state -----------------------------------------------------------------------------------------------

PassState::PassState() {
  dkRasterizerStateDefaults(&raster);
  raster.cullMode = DkFace_None;
  dkColorStateDefaults(&color);
  dkColorWriteStateDefaults(&write);
  dkDepthStencilStateDefaults(&depth);
  depth.depthTestEnable = false;
  depth.depthWriteEnable = false;
  depth.stencilTestEnable = false;
  dkMultisampleStateDefaults(&ms);
}

void PassState::Bind(DkCmdBuf cmd) const {
  dkCmdBufBindRasterizerState(cmd, &raster);
  dkCmdBufBindColorState(cmd, &color);
  dkCmdBufBindColorWriteState(cmd, &write);
  dkCmdBufBindDepthStencilState(cmd, &depth);
  dkCmdBufBindMultisampleState(cmd, &ms);
}

// --- Window ---------------------------------------------------------------------------------------------------

bool Window::Create(uint32_t width, uint32_t height, uint32_t images, DkImageFormat format) {
  count_ = std::clamp<uint32_t>(images, 2, uint32_t(images_.size()));
  width_ = width;
  height_ = height;
  DkImageLayoutMaker maker;
  dkImageLayoutMakerDefaults(&maker, g->device);
  maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
  maker.format = format;
  maker.dimensions[0] = width;
  maker.dimensions[1] = height;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &maker);
  const uint32_t each = AlignUp(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
  memory_ = MakeBlock(each * count_, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "swapchain");
  if (!memory_) return false;
  std::array<const DkImage*, 4> pointers{};
  for (uint32_t i = 0; i < count_; ++i) {
    dkImageInitialize(&images_[i], &layout, memory_.memory, i * each);
    pointers[i] = &images_[i];
  }
  DkSwapchainMaker swapchain_maker;
  dkSwapchainMakerDefaults(&swapchain_maker, g->device, nwindowGetDefault(), pointers.data(), count_);
  swapchain_ = dkSwapchainCreate(&swapchain_maker);
  if (!swapchain_) return false;
  SetInterval(0);
  REXLOG_INFO("[dk] swapchain of {} images {}x{} ({:.1f} MB), interval 0", count_, width, height,
              double(each * count_) / 1048576.0);
  return true;
}

void Window::Destroy() {
  if (swapchain_) {
    dkQueueWaitIdle(g->queue);
    dkSwapchainDestroy(swapchain_);
    swapchain_ = nullptr;
  }
  FreeBlock(memory_, 0);
}

void Window::SetInterval(uint32_t interval) {
  if (swapchain_) dkSwapchainSetSwapInterval(swapchain_, interval);
}

int Window::Acquire() { return dkQueueAcquireImage(g->queue, swapchain_); }

DkImageView Window::View(int slot) {
  DkImageView view;
  dkImageViewDefaults(&view, &images_[uint32_t(slot) % count_]);
  return view;
}

void Window::Present(int slot) { dkQueuePresentImage(g->queue, swapchain_, slot); }

// --- Formats --------------------------------------------------------------------------------------------------

DkImageFormat FromVk(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: return DkImageFormat_R8_Unorm;
    case VK_FORMAT_R8_UINT: return DkImageFormat_R8_Uint;
    case VK_FORMAT_R8G8_UNORM: return DkImageFormat_RG8_Unorm;
    case VK_FORMAT_R8G8_UINT: return DkImageFormat_RG8_Uint;
    case VK_FORMAT_R8G8B8A8_UNORM: return DkImageFormat_RGBA8_Unorm;
    case VK_FORMAT_R8G8B8A8_SNORM: return DkImageFormat_RGBA8_Snorm;
    case VK_FORMAT_R8G8B8A8_UINT: return DkImageFormat_RGBA8_Uint;
    case VK_FORMAT_R8G8B8A8_SRGB: return DkImageFormat_RGBA8_Unorm_sRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return DkImageFormat_BGRA8_Unorm;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DkImageFormat_RGB10A2_Unorm;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return DkImageFormat_RGB10A2_Uint;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return DkImageFormat_RG11B10_Float;
    case VK_FORMAT_R16_UNORM: return DkImageFormat_R16_Unorm;
    case VK_FORMAT_R16_UINT: return DkImageFormat_R16_Uint;
    case VK_FORMAT_R16_SFLOAT: return DkImageFormat_R16_Float;
    case VK_FORMAT_R16G16_UNORM: return DkImageFormat_RG16_Unorm;
    case VK_FORMAT_R16G16_SNORM: return DkImageFormat_RG16_Snorm;
    case VK_FORMAT_R16G16_SFLOAT: return DkImageFormat_RG16_Float;
    case VK_FORMAT_R16G16B16A16_UNORM: return DkImageFormat_RGBA16_Unorm;
    case VK_FORMAT_R16G16B16A16_SNORM: return DkImageFormat_RGBA16_Snorm;
    case VK_FORMAT_R16G16B16A16_UINT: return DkImageFormat_RGBA16_Uint;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DkImageFormat_RGBA16_Float;
    case VK_FORMAT_R32_UINT: return DkImageFormat_R32_Uint;
    case VK_FORMAT_R32_SFLOAT: return DkImageFormat_R32_Float;
    case VK_FORMAT_R32G32_UINT: return DkImageFormat_RG32_Uint;
    case VK_FORMAT_R32G32_SFLOAT: return DkImageFormat_RG32_Float;
    case VK_FORMAT_R32G32B32A32_UINT: return DkImageFormat_RGBA32_Uint;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return DkImageFormat_RGBA32_Float;
    case VK_FORMAT_D16_UNORM: return DkImageFormat_Z16;
    case VK_FORMAT_X8_D24_UNORM_PACK32: return DkImageFormat_Z24X8;
    case VK_FORMAT_D24_UNORM_S8_UINT: return DkImageFormat_Z24S8;
    case VK_FORMAT_D32_SFLOAT: return DkImageFormat_ZF32;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return DkImageFormat_ZF32_X24S8;
    case VK_FORMAT_S8_UINT: return DkImageFormat_S8;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return DkImageFormat_RGB_BC1;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return DkImageFormat_RGBA_BC1;
    case VK_FORMAT_BC2_UNORM_BLOCK: return DkImageFormat_RGBA_BC2;
    case VK_FORMAT_BC3_UNORM_BLOCK: return DkImageFormat_RGBA_BC3;
    case VK_FORMAT_BC4_UNORM_BLOCK: return DkImageFormat_R_BC4_Unorm;
    case VK_FORMAT_BC5_UNORM_BLOCK: return DkImageFormat_RG_BC5_Unorm;
    // The packed 16-bit formats (565, 4444, 1555) are left to the draws' texture table: their bit order in
    // deko3d is still to be read back on the console (bench-semantica test 10).
    default: return DkImageFormat_None;
  }
}

bool IsDepth(DkImageFormat format) {
  return format == DkImageFormat_Z16 || format == DkImageFormat_Z24X8 || format == DkImageFormat_Z24S8 ||
         format == DkImageFormat_ZF32 || format == DkImageFormat_ZF32_X24S8 || format == DkImageFormat_S8;
}

bool HasStencil(DkImageFormat format) {
  return format == DkImageFormat_Z24S8 || format == DkImageFormat_ZF32_X24S8 || format == DkImageFormat_S8;
}

uint32_t BytesPerTexel(DkImageFormat format) {
  switch (format) {
    case DkImageFormat_R8_Unorm: case DkImageFormat_R8_Uint: case DkImageFormat_S8: return 1;
    case DkImageFormat_RG8_Unorm: case DkImageFormat_RG8_Uint: case DkImageFormat_R16_Unorm:
    case DkImageFormat_R16_Uint: case DkImageFormat_R16_Float: case DkImageFormat_Z16: return 2;
    case DkImageFormat_RGBA16_Unorm: case DkImageFormat_RGBA16_Snorm: case DkImageFormat_RGBA16_Uint:
    case DkImageFormat_RGBA16_Float: case DkImageFormat_RG32_Uint: case DkImageFormat_RG32_Float:
    case DkImageFormat_ZF32_X24S8: return 8;
    case DkImageFormat_RGBA32_Uint: case DkImageFormat_RGBA32_Float: return 16;
    case DkImageFormat_RGBA_BC1: case DkImageFormat_RGBA_BC2: case DkImageFormat_RGBA_BC3:
    case DkImageFormat_R_BC4_Unorm: case DkImageFormat_RG_BC5_Unorm: case DkImageFormat_None: return 0;
    default: return 4;
  }
}

DkMsMode FromSamples(uint32_t samples) {
  return samples >= 8 ? DkMsMode_8x : samples >= 4 ? DkMsMode_4x : samples >= 2 ? DkMsMode_2x : DkMsMode_1x;
}

DkImageRect Rect(const VkRect2D& rect) {
  return Rect(rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
}

DkImageRect Rect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
  DkImageRect r{};
  r.x = uint32_t(std::max(x, 0));
  r.y = uint32_t(std::max(y, 0));
  r.z = 0;
  r.width = width;
  r.height = height;
  r.depth = 1;
  return r;
}

}  // namespace sa2::dk
#endif  // SA2_DK_AVAILABLE
