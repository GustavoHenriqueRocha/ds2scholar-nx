// ds2-deko3d: the texture pool's policy on deko3d. See sa2_native_textures_pool_dk.h.

#include "sa2_native_textures_pool_dk.h"

#include "sa2_dk.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>

REXCVAR_DECLARE(bool, sa2_native_textures_pool);


namespace sa2::native {
namespace {

bool BaseHasBlocks() { return true; }  // the base has the shared-block calls (sa2_dk.h)

}  // namespace

bool PoolTexturesDk::Start(int32_t mb_cache_max) {
  if (!REXCVAR_GET(sa2_native_textures_pool)) {
    REXLOG_INFO("[native] [dk] texture pool (sa2_native_textures_pool) = off; image blocks are created when a "
                "texture needs one");
    return false;
  }
  if (!BaseHasBlocks()) {
    REXLOG_WARN("[native] [dk] texture pool: sa2::dk cannot add image blocks ahead yet; image blocks are created "
                "when a texture needs one");
    return false;
  }
  // The same sizing as the Vulkan pool (sa2_native_textures_pool.cpp Start).
  const int32_t mb_max = mb_cache_max > 0 ? mb_cache_max : 384;
  const int32_t mb_initial = std::clamp<int32_t>(mb_max / 2, 64, 256);
  cap_ = uint32_t(((uint64_t(mb_max) << 20) + kBlockBytes + kBlockBytes - 1) / kBlockBytes);
  const uint32_t initial =
      std::min<uint32_t>(uint32_t(((uint64_t(mb_initial) << 20) + kBlockBytes - 1) / kBlockBytes), cap_);
  prewarmed_ = sa2::dk::AddImageSlabs(initial, "texture pool prewarm");
  failed_ += initial - prewarmed_;
  blocks_ = prewarmed_;
  if (!prewarmed_) {
    REXLOG_WARN("[native] [dk] texture pool: could not create even one 32 MB block; image blocks are created "
                "when a texture needs one");
    return false;
  }
  active_ = true;
  REXLOG_INFO("[native] [dk] texture pool ON: {} blocks of 32 MB prewarmed ({} MB), cap {} blocks ({} MB)",
              prewarmed_, (uint64_t(prewarmed_) * kBlockBytes) >> 20, cap_, (uint64_t(cap_) * kBlockBytes) >> 20);
  return true;
}

void PoolTexturesDk::PerFrame(uint64_t frame) {
  if (!active_ || blocks_ + sa2::dk::ImageSlabsOnDemand() >= cap_) return;
  if (frame < last_growth_ + kFramesBetweenBlocks) return;
  if (sa2::dk::ImageSlabsFreeBytes() >= kSlackBytes) return;
  last_growth_ = frame;
  if (sa2::dk::AddImageSlabs(1, "texture pool growth") == 1) {
    ++blocks_;
    ++grown_;
    REXLOG_INFO("[native] [dk] texture pool: new block created during play (headroom); {} MB reserved so far",
                (uint64_t(blocks_ + sa2::dk::ImageSlabsOnDemand()) * kBlockBytes) >> 20);
  } else {
    ++failed_;
  }
}

PoolStateTexturesDk PoolTexturesDk::State() const {
  PoolStateTexturesDk e;
  e.active = active_;
  e.blocks_prewarmed = prewarmed_;
  e.blocks_grown = grown_;
  e.blocks_failed = failed_;
  e.blocks_cap = cap_;
  if (BaseHasBlocks()) {
    e.bytes_free = sa2::dk::ImageSlabsFreeBytes();
    e.bytes_greater_gap = sa2::dk::ImageSlabsLargestGap();
    e.blocks_on_demand = sa2::dk::ImageSlabsOnDemand();
  }
  return e;
}

std::string PoolTexturesDk::Summary() const {
  if (!active_) return "texture pool off: " + sa2::dk::ImagesSummary();
  const PoolStateTexturesDk e = State();
  // As in the Vulkan pool: plenty free with a small largest gap means fragmentation.
  const uint64_t frag = e.bytes_free > 0 ? 100u - (e.bytes_greater_gap * 100u / e.bytes_free) : 0u;
  return "texture pool: " + sa2::dk::ImagesSummary() + "; free " + std::to_string(e.bytes_free >> 20) +
         " MB with the largest gap at " + std::to_string(e.bytes_greater_gap >> 20) + " MB (fragmentation " +
         std::to_string(frag) + " %); blocks prewarmed " + std::to_string(e.blocks_prewarmed) + ", grown " +
         std::to_string(e.blocks_grown) + ", created on demand " + std::to_string(e.blocks_on_demand) +
         ", refused " + std::to_string(e.blocks_failed);
}

}  // namespace sa2::native
