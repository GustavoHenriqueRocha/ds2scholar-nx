// L+R+left stick: the next present_effect (sdk/include/rex/ui/present_effect_cycle.h).
// Build: g++ -std=c++23 -Isdk/include tools/tests/present_effect_cycle_test.cpp
#include <cassert>
#include <cstdio>
#include <string>

#include <rex/ui/present_effect_cycle.h>

int main() {
  struct Presenter {};
  Presenter live;
  std::string cvar;
  std::string seen_by_presenter;
  int applied = 0;
  const auto get = [&] { return cvar; };
  const auto set = [&](const char* e) { cvar = e; };
  // the presenter's paint config reads the cvar: it must already hold the new value
  const auto apply = [&](Presenter*) { ++applied; seen_by_presenter = cvar; };
  const auto cycle = [&](Presenter* p) { return std::string(rex::ui::CyclePresentEffect(get, set, p, apply)); };

  cvar = "bilinear";
  assert(cycle(&live) == "fsr" && cvar == "fsr" && applied == 1 && seen_by_presenter == "fsr");
  assert(cycle(&live) == "cas" && cvar == "cas" && seen_by_presenter == "cas");
  assert(cycle(&live) == "bilinear" && cvar == "bilinear");
  // Unknown (the presenter's parser reads it as bilinear): the cycle starts over at bilinear.
  cvar = "xbr";
  assert(cycle(&live) == "bilinear");

  // deko3d output (sa2_native_deko3d): there is no SDK presenter, but the effect still changes, since the dk output
  // reads present_effect every frame. ds2_026: 15 "next upscaler" and the image never changed.
  applied = 0;
  cvar = "fsr";
  assert(cycle(nullptr) == "cas" && cvar == "cas" && applied == 0);
  std::puts("present_effect_cycle_test: ok");
  return 0;
}
