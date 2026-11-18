// Output chain of the deko3d present (app/src/me/native/sa2_native_dk_output_plan.h).
// g++ -std=c++23 -Iapp/src/me/native tools/tests/dk_output_plan_test.cpp && ./a.out
#include <cassert>
#include <cstdio>
#include <sa2_native_dk_output_plan.h>

using namespace sa2::native::dkoutput;

int main() {
  assert(ParseEffect("fsr") == Effect::kFsr);
  assert(ParseEffect("FSR") == Effect::kFsr);
  assert(ParseEffect("fsr2") == Effect::kFsr);  // temporal FSR is not there: plain FSR
  assert(ParseEffect("cas") == Effect::kCas);
  assert(ParseEffect("bilinear") == Effect::kBilinear);
  assert(ParseEffect("anything") == Effect::kBilinear);

  // The owner's config (present_effect = "fsr", DRS 720/540, handheld 1280x720).
  Plan p = PlanOutput(Effect::kFsr, 960, 540, 1280, 720, 4);  // 540: EASU to 720, RCAS, no stretch
  assert(p.count == 2);
  assert(p.steps[0].pass == Pass::kEasu && p.steps[0].width == 1280 && p.steps[0].height == 720);
  assert(p.steps[1].pass == Pass::kRcas && p.steps[1].width == 1280 && p.steps[1].height == 720);
  p = PlanOutput(Effect::kFsr, 1280, 720, 1280, 720, 4);  // 720: CAS sharpen in the SDK -> RCAS 1:1
  assert(p.count == 1 && p.steps[0].pass == Pass::kRcas && p.steps[0].width == 1280);
  p = PlanOutput(Effect::kBilinear, 1280, 720, 1280, 720, 4);  // 1:1 bilinear: the ramp writes the window
  assert(p.count == 0);
  p = PlanOutput(Effect::kBilinear, 960, 540, 1280, 720, 4);
  assert(p.count == 1 && p.steps[0].pass == Pass::kBilinear && p.steps[0].width == 1280 && p.steps[0].height == 720);
  // Docked 1080 display from a 540 source: 2 EASU passes (x2 each, clamped), then RCAS.
  p = PlanOutput(Effect::kFsr, 960, 540, 1920, 1080, 4);
  assert(p.count == 2 && p.steps[0].width == 1920 && p.steps[0].height == 1080);
  p = PlanOutput(Effect::kFsr, 640, 360, 1920, 1080, 4);
  assert(p.count == 3 && p.steps[0].width == 1280 && p.steps[1].width == 1920 && p.steps[2].pass == Pass::kRcas);
  // present_fsr_max_upsampling_passes 1: one EASU (x2), RCAS there, then the bilinear stretch.
  p = PlanOutput(Effect::kFsr, 640, 360, 1920, 1080, 1);
  assert(p.count == 3 && p.steps[0].width == 1280 && p.steps[1].pass == Pass::kRcas && p.steps[1].width == 1280 &&
         p.steps[2].pass == Pass::kBilinear && p.steps[2].width == 1920);
  // cas with a smaller input (CAS resample in the SDK): RCAS at the input size, then the stretch.
  p = PlanOutput(Effect::kCas, 960, 540, 1280, 720, 4);
  assert(p.count == 2 && p.steps[0].pass == Pass::kRcas && p.steps[0].width == 960 &&
         p.steps[1].pass == Pass::kBilinear);
  // Bigger than the display (one frame after undocking, before the rescale): FSR does not upscale -> RCAS + stretch.
  p = PlanOutput(Effect::kFsr, 1920, 1080, 1280, 720, 4);
  assert(p.count == 2 && p.steps[0].pass == Pass::kRcas && p.steps[0].width == 1920 &&
         p.steps[1].pass == Pass::kBilinear && p.steps[1].width == 1280);

  // FsrEasuCon for 960x540 in the corner of a 1280x720 image, to 1280x720 (frame-tap upscale.h:784-787 numbers).
  const auto e = EasuConstants(960, 540, 1280, 720, 1280, 720);
  assert(e[0] == Bits(0.75f) && e[1] == Bits(0.75f));
  assert(e[2] == Bits(0.5f * 960.0f * (1.0f / 1280.0f) - 0.5f));
  assert(e[4] == Bits(1.0f / 1280.0f) && e[7] == Bits(-1.0f / 720.0f));
  assert(e[9] == Bits(2.0f / 720.0f) && e[13] == Bits(4.0f / 720.0f) && e[12] == Bits(0.0f) && e[15] == 0);
  // RCAS: 0.2 stops (kFsrSharpnessReductionDefault) = exp2(-0.2); 0 stops = full sharpness 1.0.
  assert(RcasConstants(0.0f)[0] == Bits(1.0f));
  assert(RcasConstants(0.2f)[0] == Bits(std::exp2(-0.2f)));
  std::puts("dk_output_plan_test: ok");
  return 0;
}
