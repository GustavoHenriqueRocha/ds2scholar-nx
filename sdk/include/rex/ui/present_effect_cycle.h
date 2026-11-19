// L+R+left stick (presenter.cpp RexPresenterCycleEffect): present_effect bilinear -> fsr -> cas -> bilinear.
// Test: tools/tests/present_effect_cycle_test.cpp
#pragma once

#include <cstddef>
#include <iterator>
#include <string>

namespace rex::ui {

inline constexpr const char* kPresentEffectOrder[] = {"bilinear", "fsr", "cas"};

// Reads present_effect (get), writes the next one (set) and only then, with a live presenter, applies it (its paint
// config reads the cvar). Returns the new name. The deko3d output (sa2_native_deko3d) has no SDK presenter and reads
// present_effect every frame, so the value must change without one.
template <class Get, class Set, class Presenter, class Apply>
const char* CyclePresentEffect(Get&& get, Set&& set, Presenter* presenter, Apply&& apply) {
  const std::string current = get();
  size_t next = 0;
  for (size_t i = 0; i < std::size(kPresentEffectOrder); ++i) {
    if (current == kPresentEffectOrder[i]) next = (i + 1) % std::size(kPresentEffectOrder);
  }
  set(kPresentEffectOrder[next]);
  if (presenter) apply(presenter);
  return kPresentEffectOrder[next];
}

}  // namespace rex::ui
