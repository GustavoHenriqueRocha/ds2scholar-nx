#pragma once

#include <cstdint>

namespace me::native {

// ds2scholar-nx (2026-11-19, cvar ds2_skip_query_draws). The ring answers every occlusion query on the CPU
// (EVENT_WRITE_ZPD: begin = 0, end = 1000 samples, me_native_system.cpp), so the samples of the draws between a ZPD
// begin and its end are never counted: such a draw matters only through what it writes. This decides, from the draw's
// own registers, whether it writes nothing at all. Bit positions are the SDK's RB_MODECONTROL / RB_COLOR_MASK /
// RB_DEPTHCONTROL / RB_STENCILREFMASK(_BF), read the same way as the EDRAM4 write plan of the renderer
// (sa2_native_targets_dk_copias.cpp, edram4_draw_writes_: depth = (dc & 6) == 6; stencil = (dc & 1) and a face with a
// non-zero write mask and a non-KEEP op; color = mode 4 and a mask nibble of a render target the pixel shader writes).
struct QueryDrawInput {
  uint32_t mode_control = 0;   // RB_MODECONTROL (edram mode in bits 0-2: 4 color+depth, 5 depth only)
  uint32_t color_mask = 0;     // RB_COLOR_MASK (4 bits per render target)
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL
  uint32_t stencil_front = 0;  // RB_STENCILREFMASK (write mask in bits 16-23)
  uint32_t stencil_back = 0;   // RB_STENCILREFMASK_BF (used when RB_DEPTHCONTROL bit 7, backface, is set)
  bool ps_known = false;       // the pixel shader's outputs are known
  uint32_t ps_outputs = 0;     // PixelShaderOutputs: bits COLOR0..3 (and DEPTH, bit 4)
};

enum class QueryDrawVerdict : uint8_t {
  kSkip = 0,      // writes no color, no depth, no stencil: dropping it changes nothing
  kRefuseMode,    // an EDRAM mode other than 4/5 (not a normal draw): kept
  kRefuseColor,   // writes a color channel of a render target
  kRefuseDepth,   // depth test and depth write both enabled
  kRefuseStencil, // stencil on with a face that can change a bit
  kCount
};

constexpr QueryDrawVerdict ClassifyQueryDraw(const QueryDrawInput& in) {
  const uint32_t mode = in.mode_control & 7;
  if (mode != 4 && mode != 5) return QueryDrawVerdict::kRefuseMode;
  if (mode == 4) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (((in.color_mask >> (i * 4)) & 0xF) && (!in.ps_known || ((in.ps_outputs >> i) & 1)))
        return QueryDrawVerdict::kRefuseColor;
    }
  }
  const uint32_t dc = in.depth_control;
  if ((dc & 6) == 6) return QueryDrawVerdict::kRefuseDepth;
  if (dc & 1) {
    const auto face_writes = [](uint32_t refmask, uint32_t ops) {
      return ((refmask >> 16) & 0xFF) != 0 && ((ops & 7) || ((ops >> 3) & 7) || ((ops >> 6) & 7));
    };
    const bool front = face_writes(in.stencil_front, dc >> 11);
    const bool back = (dc & 0x80) ? face_writes(in.stencil_back, dc >> 23) : front;
    if (front || back) return QueryDrawVerdict::kRefuseStencil;
  }
  return QueryDrawVerdict::kSkip;
}

// The ring's view of one query: open from a ZPD begin to the next ZPD end (D3D issues them in pairs per query).
struct QueryWindow {
  bool open = false;
  constexpr void OnZpd(bool end) { open = !end; }
};

}  // namespace me::native
