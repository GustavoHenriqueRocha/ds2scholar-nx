// ds2-deko3d: the rasterizer state Vulkan and deko3d read differently, measured on the console
// (ds2-deko3d bench-semantica, resultados/2026-11-18-bench-semantica.txt). Plain C++ so the host test needs no
// deko3d (tools/tests/sa2_dk_raster_test.cpp).
#pragma once

namespace sa2::native::dk_raster {

// A viewport for dkCmdBufSetViewports plus the Y swizzle for dkCmdBufSetViewportSwizzles.
struct ViewportPlan {
  float x, y, width, height;
  bool negate_y;  // DkSwizzle_NegativeY instead of DkSwizzle_PositiveY
};

// deko3d 0.5.0 (OriginUpperLeft) puts clip y = -1 at the bottom of the viewport, Vulkan at the top, and a negative
// height draws nothing (bench test 6; the TriAevum hit the same: p6b black screen, p6c NegativeY swizzle). A Vulkan
// negative height (its Y flip) is deko3d's plain viewport over the same rows; a positive one needs NegativeY.
inline ViewportPlan PlanViewport(float x, float y, float width, float height) {
  if (height < 0.0f) return ViewportPlan{x, y + height, width, -height, false};
  return ViewportPlan{x, y, width, height, true};
}

// dkCmdBufSetDepthBias writes constantFactor * 2 (deko3d source/maxwell/gpu_3d_state.cpp), NVK writes the Vulkan
// value as is (nvk_cmd_draw.c SET_DEPTH_BIAS): half of it puts NVK's number in the register (bench test 9 measured
// the doubling). The slope factor is written as is by both.
inline float DepthBiasConstantDk(float constant_factor) { return constant_factor * 0.5f; }

}  // namespace sa2::native::dk_raster
