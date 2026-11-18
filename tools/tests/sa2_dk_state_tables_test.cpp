// Xenos blend fields -> deko3d blend factors and operations (app/src/me/native/sa2_dk_state_tables.h).
// Build: g++ -std=c++23 -Iapp/src/me/native tools/tests/sa2_dk_state_tables_test.cpp
#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "sa2_dk_state_tables.h"

using namespace sa2::native::dk_tables;

int main() {
  // Every field gives a valid deko3d factor (never 0, the zero-initialized value deko3d does not define).
  for (uint32_t f = 0; f < 32; ++f) assert(BlendFactorXenos(f) != 0);
  // The fields kFactors leaves at VK_BLEND_FACTOR_ZERO.
  for (uint32_t f : {0u, 2u, 3u, 17u, 20u, 31u}) assert(BlendFactorXenos(f) == kFactorZero);
  assert(BlendFactorXenos(1) == kFactorOne);
  assert(BlendFactorXenos(4) == kFactorSrcColor && BlendFactorXenos(5) == kFactorInvSrcColor);
  assert(BlendFactorXenos(6) == kFactorSrcAlpha && BlendFactorXenos(7) == kFactorInvSrcAlpha);
  // Vulkan's order is DST_COLOR before DST_ALPHA (8, 9 / 10, 11), deko3d's is the other way round (9, 10 / 7, 8).
  assert(BlendFactorXenos(8) == 9 && BlendFactorXenos(9) == 10);
  assert(BlendFactorXenos(10) == 7 && BlendFactorXenos(11) == 8);
  assert(BlendFactorXenos(12) == 0x21 && BlendFactorXenos(15) == 0x24);
  assert(BlendFactorXenos(16) == kFactorSrcAlphaSaturate);
  // The upper bits of a register word do not leak into the 5-bit field.
  assert(BlendFactorXenos(0x20 | 6) == kFactorSrcAlpha);
  bool reverse = false;
  assert(BlendOpXenos(0, &reverse) == kOpAdd && !reverse);
  assert(BlendOpXenos(1, &reverse) == kOpSub && !reverse);
  assert(BlendOpXenos(2, &reverse) == kOpMin && BlendOpXenos(3, &reverse) == kOpMax && !reverse);
  assert(BlendOpXenos(4, &reverse) == kOpSub && reverse);  // reverse subtract: subtract, with the warning
  reverse = false;
  for (uint32_t op : {5u, 6u, 7u}) assert(BlendOpXenos(op, &reverse) == kOpAdd && !reverse);
  assert(BlendOpXenos(4, nullptr) == kOpSub);
  std::puts("sa2_dk_state_tables: ok");
  return 0;
}
