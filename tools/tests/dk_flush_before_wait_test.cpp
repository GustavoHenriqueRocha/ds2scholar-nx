// Does a CPU wait reach an order the GPU never received? (app/src/me/native/sa2_dk_flush_rule.h)
// Build: g++ -std=c++23 -Iapp/src/me/native tools/tests/dk_flush_before_wait_test.cpp
// The old behavior (sa2_dk_flush_before_wait = false): add -DSA2_TEST_OLD_RULE; the 3-split case must then fail.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "sa2_dk_flush_rule.h"

#ifdef SA2_TEST_OLD_RULE
constexpr bool kEnabled = false;
#else
constexpr bool kEnabled = true;
#endif

// A model of sa2::dk + deko3d: Submit only queues (no kick-off), a present flushes, and the GPU completes
// everything flushed. TargetsDk rotates 3 work slots and BeginRecording waits for the slot's previous order.
struct Model {
  uint64_t next = 1, flushed = 0, completed = 0;
  uint64_t slot_order[3] = {};
  uint32_t slot = 0;
  bool hung = false;

  void Gpu() { completed = flushed; }  // the GPU is fast: whatever it received is done
  uint64_t Submit() { return next++; }
  void Present() {  // output submission + dkQueuePresentImage
    Submit();
    flushed = next - 1;
    Gpu();
  }
  void Wait(uint64_t order) {  // sa2::dk::WaitOrder
    Gpu();
    if (order <= completed) return;
    if (sa2::dk::FlushBeforeWait(kEnabled, order, completed, flushed)) flushed = next - 1;
    Gpu();
    if (order > completed) hung = true;  // dkFenceWait on a syncpoint value never reached
  }
  void SendAndWait() {  // SendWork(false) + Record -> BeginRecording
    slot_order[slot] = Submit();
    slot = (slot + 1) % 3;
    Wait(slot_order[slot]);
  }
  void Frame() {  // SendWork at the Swap, the output, then BeginRecording of the next frame
    slot_order[slot] = Submit();
    Present();
    slot = (slot + 1) % 3;
    Wait(slot_order[slot]);
  }
};

int main() {
  // Normal frames: the present flushes before any slot comes back (old rule fine too).
  {
    Model m;
    for (int i = 0; i < 10; ++i) m.Frame();
    assert(!m.hung);
  }
  // 2 splits in a row (ds2_035 g8183/8184 and g8348/8349; ds2_030 g5554/5555): the console did not hang.
  {
    Model m;
    for (int i = 0; i < 4; ++i) m.Frame();
    m.SendAndWait();
    m.SendAndWait();
    m.Frame();
    assert(!m.hung);
  }
  // 3 splits in a row between two presents (ds2_036 g7456/7457/7458, ds2_035 g9082/9083/9084): the third split's
  // BeginRecording waits for the first split's order, still in the queue. Old rule: hang (order 8647, completed 8646).
  {
    Model m;
    for (int i = 0; i < 4; ++i) m.Frame();
    m.SendAndWait();
    m.SendAndWait();
    m.SendAndWait();
    assert(!m.hung);
  }
  // WaitGpu right after a SendWork (RescaleAll, ds2_027): waits for the order just submitted.
  {
    Model m;
    for (int i = 0; i < 4; ++i) m.Frame();
    m.Wait(m.Submit());
    assert(!m.hung);
  }
  // The rule itself: off never flushes; done or already sent orders need no flush.
  assert(!sa2::dk::FlushBeforeWait(false, 10, 5, 5));
  assert(!sa2::dk::FlushBeforeWait(true, 5, 5, 5));
  assert(!sa2::dk::FlushBeforeWait(true, 8, 5, 9));
  assert(sa2::dk::FlushBeforeWait(true, 8647, 8646, 8646));
  std::puts("dk_flush_before_wait_test: ok");
  return 0;
}
