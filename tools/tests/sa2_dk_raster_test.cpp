// Vulkan viewport and depth bias -> deko3d (app/src/me/native/sa2_dk_raster.h).
// Build: g++ -std=c++23 -Iapp/src/me/native tools/tests/sa2_dk_raster_test.cpp
#include <cassert>
#include <cstdio>

#include "sa2_dk_raster.h"

using namespace sa2::native::dk_raster;

int main() {
  // The draws' Vulkan viewport flips Y with a negative height (sa2_native_draws.cpp, height = -2 * scale_y):
  // deko3d draws nothing with it (bench-semantica test 6; TriAevum p6b black screen). The same rows with a
  // positive height and no swizzle: deko3d already puts clip y = -1 at the bottom, as Vulkan does here.
  ViewportPlan flipped = PlanViewport(0.0f, 720.0f, 1280.0f, -720.0f);
  assert(flipped.x == 0.0f && flipped.y == 0.0f && flipped.width == 1280.0f && flipped.height == 720.0f);
  assert(!flipped.negate_y);
  flipped = PlanViewport(16.0f, 300.0f, 64.0f, -100.0f);
  assert(flipped.y == 200.0f && flipped.height == 100.0f && !flipped.negate_y);
  // A positive Vulkan height (clip y = -1 at the top): the same viewport with DkSwizzle_NegativeY (bench test 6,
  // "viewport normal + DkSwizzle_NegativeY" = Vulkan in every cull mode; TriAevum p6c).
  const ViewportPlan plain = PlanViewport(0.0f, 0.0f, 1280.0f, 720.0f);
  assert(plain.x == 0.0f && plain.y == 0.0f && plain.width == 1280.0f && plain.height == 720.0f);
  assert(plain.negate_y);
  // dkCmdBufSetDepthBias writes constantFactor * 2 (deko3d gpu_3d_state.cpp), NVK writes the Vulkan value as is
  // (nvk_cmd_draw.c SET_DEPTH_BIAS): half of it puts the same number in the register. Slope is not scaled.
  assert(DepthBiasConstantDk(1.0f) == 0.5f && DepthBiasConstantDk(-16.0f) == -8.0f);
  assert(DepthBiasConstantDk(0.0f) == 0.0f);
  std::puts("sa2_dk_raster_test: ok");
  return 0;
}
