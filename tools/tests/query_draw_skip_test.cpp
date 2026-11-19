// Draws inside an occlusion query that write nothing (app/src/me/native/me_query_draw_skip.h, ds2_skip_query_draws).
// Build: g++ -std=c++23 -Iapp/src/me -Iapp/src/me/native tools/tests/query_draw_skip_test.cpp
#include <cassert>
#include <cstdio>

#include "me_query_draw_skip.h"

int main() {
  using me::native::ClassifyQueryDraw;
  using me::native::QueryDrawInput;
  using V = me::native::QueryDrawVerdict;
  const auto make = [](uint32_t mode, uint32_t mask, uint32_t dc, uint32_t outputs = 1) {
    QueryDrawInput in;
    in.mode_control = mode;
    in.color_mask = mask;
    in.depth_control = dc;
    in.ps_known = true;
    in.ps_outputs = outputs;
    return in;
  };
  // The D3D occlusion-query box: color writes off, depth test (LESSEQUAL) without depth write, stencil off.
  assert(ClassifyQueryDraw(make(4, 0, 0x2 | (3u << 4))) == V::kSkip);
  // Depth write without depth test writes nothing (the renderer's own rule: (dc & 6) == 6).
  assert(ClassifyQueryDraw(make(4, 0, 0x4)) == V::kSkip);
  // The pixel shader exports depth, but depth write is off.
  assert(ClassifyQueryDraw(make(4, 0, 0x2, 0x11)) == V::kSkip);

  // Color: a mask nibble of a render target the pixel shader writes.
  assert(ClassifyQueryDraw(make(4, 0xF, 0x2)) == V::kRefuseColor);
  assert(ClassifyQueryDraw(make(4, 0x8, 0x2)) == V::kRefuseColor);  // alpha only is still a write
  assert(ClassifyQueryDraw(make(4, 0xF, 0x2, 0x10)) == V::kSkip);    // no COLOR output: nothing reaches RT0
  assert(ClassifyQueryDraw(make(4, 0x0F00, 0x2, 0x1)) == V::kSkip);  // RT2 masked in, but only COLOR0 written
  assert(ClassifyQueryDraw(make(4, 0x0F00, 0x2, 0x5)) == V::kRefuseColor);
  {
    QueryDrawInput unknown = make(4, 0x1, 0x2);
    unknown.ps_known = false;  // outputs unknown: any mask bit counts
    assert(ClassifyQueryDraw(unknown) == V::kRefuseColor);
  }
  // Depth-only mode never writes color, whatever the mask says.
  assert(ClassifyQueryDraw(make(5, 0xFFFF, 0x2)) == V::kSkip);

  // Depth write: from the console log ds2_045 (D3D Clear of depth+stencil, mode 5 dc 00008777; the scene pass
  // mode 4 dc 00700366 mask 0).
  assert(ClassifyQueryDraw(make(5, 0xFFFF, 0x00008777)) == V::kRefuseDepth);
  assert(ClassifyQueryDraw(make(4, 0, 0x00700366)) == V::kRefuseDepth);
  assert(ClassifyQueryDraw(make(5, 0, 0x76)) == V::kRefuseDepth);

  // Stencil (front ops: fail 11-13, zpass 14-16, zfail 17-19; write mask RB_STENCILREFMASK 16-23).
  {
    QueryDrawInput s = make(4, 0, 0x1 | 0x2 | (2u << 14));  // zpass REPLACE
    s.stencil_front = 0x00FF0000;
    assert(ClassifyQueryDraw(s) == V::kRefuseStencil);
    s.stencil_front = 0x000000FF;  // write mask 0: nothing changes
    assert(ClassifyQueryDraw(s) == V::kSkip);
    s.stencil_front = 0x00FF0000;
    s.depth_control = 0x1 | 0x2 | (7u << 8);  // all ops KEEP
    assert(ClassifyQueryDraw(s) == V::kSkip);
    s.depth_control = 0x1 | (4u << 17);  // zfail INCR
    assert(ClassifyQueryDraw(s) == V::kRefuseStencil);
    s.depth_control = 0x0 | (2u << 14);  // stencil off: the ops do not apply
    assert(ClassifyQueryDraw(s) == V::kSkip);
  }
  {
    // Backface on (bit 7): the back face's ops (23-25, 26-28, 29-31) and RB_STENCILREFMASK_BF.
    QueryDrawInput b = make(4, 0, 0x1 | 0x80 | (2u << 26));  // front KEEP, back zpass REPLACE
    b.stencil_front = 0x00FF0000;
    b.stencil_back = 0x00FF0000;
    assert(ClassifyQueryDraw(b) == V::kRefuseStencil);
    b.stencil_back = 0;  // back write mask 0
    assert(ClassifyQueryDraw(b) == V::kSkip);
    // Backface off: the back bits are not used, the front state is.
    b.depth_control = 0x1 | (2u << 26);
    b.stencil_back = 0x00FF0000;
    assert(ClassifyQueryDraw(b) == V::kSkip);
  }

  // Other EDRAM modes are not normal draws (copy = 6 goes to Copy(), never here; 0 unknown): kept.
  assert(ClassifyQueryDraw(make(6, 0, 0x2)) == V::kRefuseMode);
  assert(ClassifyQueryDraw(make(0, 0, 0x2)) == V::kRefuseMode);
  assert(ClassifyQueryDraw(make(0x8 | 4, 0, 0x2)) == V::kSkip);  // only bits 0-2 are the mode

  // The ring's query window: begin opens it, end closes it.
  me::native::QueryWindow w;
  assert(!w.open);
  w.OnZpd(false);
  assert(w.open);
  w.OnZpd(false);  // a second begin (another query) keeps it open
  assert(w.open);
  w.OnZpd(true);
  assert(!w.open);
  std::puts("query_draw_skip_test: ok");
  return 0;
}
