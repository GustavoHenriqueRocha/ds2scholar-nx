// ds2-deko3d: the blend fields of the Xenos RB_BLENDCONTROL register as deko3d blend factors and operations.
//
// FillFixedState (sa2_native_draws.cpp) decodes each 5-bit factor field through kFactors (fields 2, 3 and 17-31
// give ZERO: the table has 17 initializers and the rest is VK_BLEND_FACTOR_ZERO = 0) and each 3-bit operation
// field through its `operation` lambda (4 = reverse subtract, drawn as subtract with a warning). deko3d numbers
// both enums from 1 (DkBlendFactor_Zero = 1, DkBlendOp_Add = 1), so a zero-initialized table would be an invalid
// factor: this table spells every field out. Plain C++ with the numeric values of deko3d.h (the translated file
// static_asserts them against the enums) so the host test needs no deko3d.

#pragma once

#include <cstdint>

namespace sa2::native::dk_tables {

// DkBlendFactor (deko3d.h)
inline constexpr uint8_t kFactorZero = 1, kFactorOne = 2, kFactorSrcColor = 3, kFactorInvSrcColor = 4,
                         kFactorSrcAlpha = 5, kFactorInvSrcAlpha = 6, kFactorDstAlpha = 7, kFactorInvDstAlpha = 8,
                         kFactorDstColor = 9, kFactorInvDstColor = 10, kFactorSrcAlphaSaturate = 11,
                         kFactorConstColor = 1 | 0x20, kFactorInvConstColor = 2 | 0x20, kFactorConstAlpha = 3 | 0x20,
                         kFactorInvConstAlpha = 4 | 0x20;
// DkBlendOp (deko3d.h)
inline constexpr uint8_t kOpAdd = 1, kOpSub = 2, kOpRevSub = 3, kOpMin = 4, kOpMax = 5;

// A 5-bit factor field -> DkBlendFactor, the same mapping as FillFixedState's kFactors.
constexpr uint8_t BlendFactorXenos(uint32_t field) {
  switch (field & 0x1F) {
    case 1: return kFactorOne;
    case 4: return kFactorSrcColor;
    case 5: return kFactorInvSrcColor;
    case 6: return kFactorSrcAlpha;
    case 7: return kFactorInvSrcAlpha;
    case 8: return kFactorDstColor;
    case 9: return kFactorInvDstColor;
    case 10: return kFactorDstAlpha;
    case 11: return kFactorInvDstAlpha;
    case 12: return kFactorConstColor;
    case 13: return kFactorInvConstColor;
    case 14: return kFactorConstAlpha;
    case 15: return kFactorInvConstAlpha;
    case 16: return kFactorSrcAlphaSaturate;
    default: return kFactorZero;  // 0, 2, 3 and 17-31
  }
}

// A 3-bit operation field -> DkBlendOp, the same mapping as FillFixedState's `operation`: 4 (reverse subtract)
// is drawn as subtract and sets *reverse so the caller can warn, as the Vulkan code does.
constexpr uint8_t BlendOpXenos(uint32_t op, bool* reverse) {
  switch (op & 0x7) {
    case 1: return kOpSub;
    case 2: return kOpMin;
    case 3: return kOpMax;
    case 4:
      if (reverse) *reverse = true;
      return kOpSub;
    default: return kOpAdd;
  }
}

}  // namespace sa2::native::dk_tables
