// SA2 - native renderer on deko3d: the render target class (the translation of TargetsVulkan,
// sa2_native_targets.cpp). Its members come from one fragment per translated region, each owned by one file:
//   sa2_native_targets_dk_infra.inc   + sa2_native_targets_dk_infra.cpp    slots, memory, images, stats, Initialize
//   sa2_native_targets_dk_copias.inc  + sa2_native_targets_dk_copias.cpp   Copy, clears, depth copies, EDRAM mode 4
//   sa2_native_targets_dk_edram.inc   + sa2_native_targets_dk_edram.cpp    EDRAM sync, resolves, aliases, sleep/wake
//   sa2_native_targets_dk_present.inc + sa2_native_targets_dk_present.cpp  Present, gamma, rescale, lazy front, reads
// File-scope helpers, structs and cvar declarations: sa2_native_targets_dk_types.h (infra).

#pragma once

#include "sa2_native_dk.h"
#include "sa2_native_targets.h"

#if SA2_DK_AVAILABLE
#include "sa2_native_targets_dk_types.h"

namespace sa2::native {

class TargetsDk final : public TargetsNative, public ContextTargetsDk {
 public:
#include "sa2_native_targets_dk_copias.inc"
#include "sa2_native_targets_dk_edram.inc"
#include "sa2_native_targets_dk_present.inc"
  // infra last: its inline members use the other regions' members.
#include "sa2_native_targets_dk_infra.inc"
};

}  // namespace sa2::native
#endif  // SA2_DK_AVAILABLE
