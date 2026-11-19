#pragma once
// ds2-deko3d: does a render scale switch keep each image's other-scale twin (TargetsDk::RescaleKeep: swap handles and
// blit, no GPU wait) or wait for the GPU and recreate the images (RescaleAll)? Test: tools/tests/dk_rescale_keep_test.cpp

namespace sa2::native {

// keep_cvar: ds2_dynamic_resolution_keep; docked: in the dock. Whether the dynamic resolution was ever on does not
// matter (it used to: a handheld switch of the fixed mode, L+R+right stick, then waited for the GPU, and on the
// console that wait hung, ds2_027; every switch through the twins went through, ds2_025/026).
inline bool KeepScaleTwins(bool keep_cvar, bool docked) { return keep_cvar && !docked; }

}  // namespace sa2::native
