// ds2-deko3d: the output chain of the deko3d present, which runs without the SDK presenter (the Present gets a null
// rex::ui::Presenter*). Pure host logic, tested by tools/tests/dk_output_plan_test.cpp.
//
// After the ramp pass (PaintOutput: the game's gamma ramp and FXAA, at the guest output's host size), the presenter
// ran its effects and a final bilinear stretch to the display (GetGuestOutputPaintFlow, sdk/src/ui/presenter.cpp
// 1218-1357). The same rules are here for the one case the port has: one guest output filling a 16:9 display
// (1280x720 handheld, 1920x1080 docked), no letterbox, no safe area cut:
//   - fsr, input smaller than the display: EASU passes, each up to 2x (at most present_fsr_max_upsampling_passes),
//     then RCAS at the last EASU size;
//   - fsr with an input that is not smaller, and cas: the SDK ran CAS (sharpen 1:1, or resample up to 2x). ffx_cas.h
//     is in no local tree, so RCAS 1:1 takes its place and the resample's size step goes to the final bilinear
//     (decision in ds2-deko3d docs/pesquisas/traducao/04-alvos-present.md, Fase C);
//   - fsr2 / fsr3 (temporal) do not exist here: plain fsr;
//   - then a bilinear stretch when the last size is not the display's. The SDK always ended with a bilinear pass; at
//     1:1 it samples texel centres (the identity), so an empty plan means "the ramp pass writes the window itself".
// The FSR constants are AMD's FsrEasuCon / FsrRcasCon (ffx_fsr1.h v1.20210629, MIT), computed here because the
// shaders (shaders_dk/output_easu.frag, output_rcas.frag) read them from a uniform buffer.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace sa2::native::dkoutput {

enum class Effect : uint8_t { kBilinear, kCas, kFsr };
enum class Pass : uint8_t { kEasu, kRcas, kBilinear };

// present_effect (sdk/src/ui/presenter.cpp ParsePresentEffect: case-insensitive; unknown = bilinear).
inline Effect ParseEffect(std::string_view name) {
  const auto is = [&](std::string_view what) {
    return name.size() == what.size() &&
           std::equal(name.begin(), name.end(), what.begin(), [](char a, char b) {
             return (a >= 'A' && a <= 'Z' ? char(a - 'A' + 'a') : a) == b;
           });
  };
  if (is("fsr") || is("fsr2") || is("fsr3")) return Effect::kFsr;
  if (is("cas")) return Effect::kCas;
  return Effect::kBilinear;
}

struct Step {
  Pass pass = Pass::kBilinear;
  uint32_t width = 0;  // the size this pass writes
  uint32_t height = 0;
};

struct Plan {
  std::array<Step, 6> steps{};  // at most 4 EASU + RCAS + bilinear
  uint32_t count = 0;
};

inline Plan PlanOutput(Effect effect, uint32_t input_width, uint32_t input_height, uint32_t output_width,
                       uint32_t output_height, uint32_t max_passes) {
  Plan plan;
  uint32_t w = input_width, h = input_height;
  max_passes = std::clamp<uint32_t>(max_passes, 1, 4);
  if (effect == Effect::kFsr && (w < output_width || h < output_height)) {
    for (uint32_t i = 0; i < max_passes && (w < output_width || h < output_height); ++i) {
      w = std::min(w * 2, output_width);
      h = std::min(h * 2, output_height);
      plan.steps[plan.count++] = {Pass::kEasu, w, h};
    }
    plan.steps[plan.count++] = {Pass::kRcas, w, h};
  } else if (effect != Effect::kBilinear) {
    plan.steps[plan.count++] = {Pass::kRcas, w, h};  // in place of CAS sharpen / resample
  }
  if (w != output_width || h != output_height) plan.steps[plan.count++] = {Pass::kBilinear, output_width, output_height};
  return plan;
}

inline uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }

// FsrEasuCon: viewport = the part of the input image that holds the picture (its top left corner), input = the
// whole input image, output = the size this pass writes. 16 words: con0..con3 of the shader.
inline std::array<uint32_t, 16> EasuConstants(float viewport_width, float viewport_height, float input_width,
                                              float input_height, float output_width, float output_height) {
  std::array<uint32_t, 16> c{};
  c[0] = Bits(viewport_width * (1.0f / output_width));
  c[1] = Bits(viewport_height * (1.0f / output_height));
  c[2] = Bits(0.5f * viewport_width * (1.0f / output_width) - 0.5f);
  c[3] = Bits(0.5f * viewport_height * (1.0f / output_height) - 0.5f);
  c[4] = Bits(1.0f / input_width);
  c[5] = Bits(1.0f / input_height);
  c[6] = Bits(1.0f * (1.0f / input_width));
  c[7] = Bits(-1.0f * (1.0f / input_height));
  c[8] = Bits(-1.0f * (1.0f / input_width));
  c[9] = Bits(2.0f * (1.0f / input_height));
  c[10] = Bits(1.0f * (1.0f / input_width));
  c[11] = Bits(2.0f * (1.0f / input_height));
  c[12] = Bits(0.0f * (1.0f / input_width));
  c[13] = Bits(4.0f * (1.0f / input_height));
  c[14] = 0;
  c[15] = 0;
  return c;
}

// FsrRcasCon, FP32 path: con.x = exp2(-stops). con.y is the packed half pair of the FP16 path, unused by
// output_rcas.frag (no A_HALF), left 0.
inline std::array<uint32_t, 4> RcasConstants(float sharpness_stops) {
  return {Bits(std::exp2(-sharpness_stops)), 0, 0, 0};
}

}  // namespace sa2::native::dkoutput
