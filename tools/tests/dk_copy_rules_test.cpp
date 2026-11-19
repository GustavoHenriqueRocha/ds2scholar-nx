// deko3d copy decisions: repeated color resolves (ds2_skip_repeated_color_resolve) and same-format RGBA16F resolves
// drawn instead of copied on the copy engine (ds2_copy_dma_as_draw). app/src/me/native/sa2_native_dk_copy_rules.h
// Build: g++ -std=c++23 -Iapp/src/me/native tools/tests/dk_copy_rules_test.cpp
#include <cassert>
#include <cstdio>

#include "sa2_native_dk_copy_rules.h"

using namespace sa2::native;

namespace {

ColorResolveGate Open() {
  ColorResolveGate g;
  g.enabled = true;
  g.mode4 = true;
  return g;
}

// ds2_045 (19-11q): 117B8000 (1280x720, k_2_10_10_10) gets 2 resolves a frame, 251 of 502 with draws before them:
// the second one repeats the first (direct 7e3 path, ResolverWithBias).
ColorResolveKey Scene() {
  ColorResolveKey k;
  k.source = 0x00300011;  // the 7e3 owner
  k.render_target = 0x00100012;
  k.target = 0x00200013;
  k.address = 0x117B8000;
  k.width = 1280;
  k.height = 720;
  k.path = kColorResolveDirect7e3;
  k.source_format = 2;
  k.target_format = 7;
  k.drawn = 1000;
  k.transfers = 50;
  k.restored = 7;
  k.edram_version = 9000;
  k.revision = 41;
  return k;
}

}  // namespace

int main() {
  // 1. The repeat with nothing in between is skipped.
  const ColorResolveKey last = Scene();
  ColorResolveKey now = Scene();
  now.revision = 0;  // the key of the resolve being decided carries no revision yet
  assert(RepeatedColorResolve(Open(), &last, now, 41));
  // Off (the default), outside mode 4, or with nothing remembered: resolved as before.
  ColorResolveGate off = Open();
  off.enabled = false;
  assert(!RepeatedColorResolve(off, &last, now, 41));
  ColorResolveGate mode3 = Open();
  mode3.mode4 = false;
  assert(!RepeatedColorResolve(mode3, &last, now, 41));
  assert(!RepeatedColorResolve(Open(), nullptr, now, 41));
  // 2. Anything written since: the texture (revision), the render target (draw, EDRAM transfer, restore, any tile
  // publish: clears redirected to EDRAM move only this one).
  assert(!RepeatedColorResolve(Open(), &last, now, 42));
  for (int field = 0; field < 4; ++field) {
    ColorResolveKey c = now;
    (field == 0 ? c.drawn : field == 1 ? c.transfers : field == 2 ? c.restored : c.edram_version) += 1;
    assert(!RepeatedColorResolve(Open(), &last, c, 41));
  }
  // 3. Another resolve: source image, render target view, texture image, rectangle, offsets, path, bias, formats.
  for (int field = 0; field < 14; ++field) {
    ColorResolveKey c = now;
    switch (field) {
      case 0: c.source ^= 1; break;
      case 1: c.render_target ^= 1; break;
      case 2: c.target ^= 1; break;
      case 3: c.address += 0x1000; break;
      case 4: c.x0 += 8; break;
      case 5: c.y0 += 8; break;
      case 6: c.dx += 8; break;
      case 7: c.dy += 8; break;
      case 8: c.width -= 8; break;
      case 9: c.height -= 8; break;
      case 10: c.path = kColorResolveBias; break;
      case 11: c.exp_bias = -3; break;
      case 12: c.source_format = 6; break;
      default: c.target_format = 6; break;
    }
    assert(!RepeatedColorResolve(Open(), &last, c, 41));
  }
  // 4. The gate: multisampled source, alias pending, deferred front copy, guest read-back, invalid content.
  for (int field = 0; field < 5; ++field) {
    ColorResolveGate g = Open();
    (field == 0 ? g.multisampled : field == 1 ? g.alias_pending : field == 2 ? g.front_pending
                 : field == 3 ? g.reads_back : g.invalid_content) = true;
    assert(!MaySkipColorResolve(g));
    assert(!RepeatedColorResolve(g, &last, now, 41));
  }
  assert(MaySkipColorResolve(Open()));

  // 5. ds2_copy_dma_as_draw. ds2_045: 5 resolves a frame "copied as before" (copy engine), and the only resolves of the
  // frame not drawn are the k_16_16_16_16_FLOAT ones (12552000 64x64, 1255A000..12562000 32x16: 1255 per 10 s, the
  // exact "copied as before" count; the 39 RGBA8 resolves a frame are the 9789 drawn).
  using K = ResolveDrawKind;
  using T = CopyTexel;
  assert(ColorResolveDrawKind(T::kRgba16Float, T::kRgba16Float, true, true) == K::kRaw64);
  // Off (the default), or no RGBA16_Uint views: as before, the copy engine.
  assert(ColorResolveDrawKind(T::kRgba16Float, T::kRgba16Float, false, true) == K::kNone);
  assert(ColorResolveDrawKind(T::kRgba16Float, T::kRgba16Float, true, false) == K::kNone);
  // RGBA8 is drawn as before whatever the cvar says; mixed or other formats are never drawn by this pass.
  assert(ColorResolveDrawKind(T::kRgba8Unorm, T::kRgba8Unorm, false, false) == K::kFloat);
  assert(ColorResolveDrawKind(T::kRgba8Unorm, T::kRgba8Unorm, true, true) == K::kFloat);
  assert(ColorResolveDrawKind(T::kRgba8Unorm, T::kRgba16Float, true, true) == K::kNone);
  assert(ColorResolveDrawKind(T::kRgba16Float, T::kRgba8Unorm, true, true) == K::kNone);
  assert(ColorResolveDrawKind(T::kOther, T::kOther, true, true) == K::kNone);
  std::puts("dk_copy_rules_test: ok");
  return 0;
}
