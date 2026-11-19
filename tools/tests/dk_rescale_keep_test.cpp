// Render scale switch on deko3d: keep the twins or wait for the GPU (app/src/me/native/sa2_native_dk_rescale_keep.h).
// Build: g++ -std=c++23 -Iapp/src/me/native tools/tests/dk_rescale_keep_test.cpp
#include <cassert>
#include <cstdio>

#include "sa2_native_dk_rescale_keep.h"

int main() {
  using sa2::native::KeepScaleTwins;
  // Handheld with the dynamic resolution on: the path every console switch took without hanging (ds2_025/026,
  // "[res] render scale ... kept").
  assert(KeepScaleTwins(true, false));
  // ds2_027 (19/11 09:49): handheld, fixed mode (L+R+right stick, "resolution mode 2", the dynamic resolution never
  // on), 720p -> 540p: it took the GPU-wait path and the GPU hung (0 VdSwap for 13 s). It must keep the twins too.
  assert(KeepScaleTwins(true, false));  // (the dynamic resolution does not enter the decision)
  // Docked: the old path (it frees the twins); and ds2_dynamic_resolution_keep = false: as before.
  assert(!KeepScaleTwins(true, true));
  assert(!KeepScaleTwins(false, false));
  std::puts("dk_rescale_keep_test: ok");
  return 0;
}
