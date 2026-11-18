// ds2-deko3d: the texture pool's policy on deko3d (sa2_native_textures_pool.h is the Vulkan one, with the
// measurements that motivate it).
//
// What carries over and what does not
//
//   The Vulkan pool does two things: (1) textures take pieces of large blocks instead of one dedicated allocation
//   each, and (2) those blocks are created ahead (prewarm at start, growth with headroom once per frame), never at
//   the moment a texture needs one, because creating a block in the middle of play is the stutter it exists to
//   remove.
//   (1) is already sa2::dk::CreateImage: it sub-allocates every image under 8 MB from 32 MB blocks with a free
//   list. So there is no Reserve/Release here: textures call CreateImage/DestroyImage directly, and ImageNativeDk::
//   pool_block stays kBlockPoolInvalid.
//   (2) is what this class keeps. CreateImage creates a block on demand when none has room; with this policy that
//   only happens if a burst outruns the headroom, and it is counted (blocks created on demand) so the report shows
//   it. A deko3d block costs one nvMapCreate and its mappings (dkMemBlockCreate; no memset without
//   DkMemBlockFlags_ZeroFillInit), so the price of an on-demand block is smaller than NVK's, but not zero.
//
//   The safety invariant of the Vulkan pool (eviction only after 120 frames or after WaitGpuOfTheAll) is no longer
//   needed for memory reuse: sa2::dk::DestroyImage keeps the memory until the submission order passed has
//   completed. It is still what lets the draws pass CurrentOrder() there.
//
// Single thread: the ring thread, as the Vulkan pool.

#pragma once

#include <cstdint>

#include "sa2_dk.h"
#include <string>

namespace sa2::native {

// For the NoteMemoryOfTheGpu report (the fields of PoolStateTextures that still mean something).
struct PoolStateTexturesDk {
  bool active = false;
  uint32_t blocks_prewarmed = 0;  // created by Start
  uint32_t blocks_grown = 0;      // created by PerFrame (headroom)
  uint32_t blocks_failed = 0;     // times a block could not be created
  uint32_t blocks_cap = 0;
  uint64_t bytes_free = 0;        // free bytes in the shared blocks (not the dedicated ones)
  uint64_t bytes_greater_gap = 0; // largest contiguous free range: measures fragmentation
  uint32_t blocks_on_demand = 0;  // created by CreateImage because none had room (the stutter this avoids)
};

class PoolTexturesDk {
 public:
  // Prewarms half the cache limit (between 64 and 256 MB) in 32 MB blocks; the cap is one block above the
  // cache limit, as the Vulkan pool. false if sa2_native_textures_pool is off or the base cannot add blocks
  // (then CreateImage just creates them on demand, which is still correct).
  bool Start(int32_t mb_cache_max);
  // Nothing to free: the blocks belong to sa2::dk and go with it.
  void Finish() { active_ = false; }
  bool Active() const { return active_; }
  // Growth with headroom: one block when less than 8 MB is free, at most one every 120 frames.
  void PerFrame(uint64_t frame);
  PoolStateTexturesDk State() const;
  // Line for the texture-memory report. No trailing newline.
  std::string Summary() const;

 private:
  static constexpr uint64_t kBlockBytes = sa2::dk::kSlabBytes;
  static constexpr uint64_t kSlackBytes = 8ull << 20;
  static constexpr uint64_t kFramesBetweenBlocks = 120;

  bool active_ = false;
  uint32_t blocks_ = 0, cap_ = 0, prewarmed_ = 0, grown_ = 0, failed_ = 0;
  uint64_t last_growth_ = 0;
};

}  // namespace sa2::native
