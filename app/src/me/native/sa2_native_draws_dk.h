// SA2 - native renderer on deko3d: the draws class (the translation of DrawsVulkanImpl, sa2_native_draws.cpp).
// Its members come from one fragment per translated region, each owned by one file:
//   sa2_native_draws_dk_1.inc + sa2_native_draws_dk_1.cpp   lines 1-8158 of the Vulkan file (Draw, state, vertices,
//                                                            constants, clears in pass, slots, stats)
//   sa2_native_draws_dk_2.inc + sa2_native_draws_dk_2.cpp   lines 8158-14763 (descriptors, textures, vertex input,
//                                                            BeginPass, shader variants, upload buffers)
// File-scope helpers and structs (Texture, View, VerticesEntry, PipelineKey...): sa2_native_draws_dk_types.h (draws 1).

#pragma once

#include "sa2_native_dk.h"

#if SA2_DK_AVAILABLE
#include "sa2_native_draws_dk_types.h"

namespace sa2::native {

class DrawsDkImpl final : public DrawsDk {
 public:
#include "sa2_native_draws_dk_1.inc"
#include "sa2_native_draws_dk_2.inc"
};

}  // namespace sa2::native
#endif  // SA2_DK_AVAILABLE
