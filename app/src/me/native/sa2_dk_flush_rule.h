#pragma once
// ds2-deko3d: must sa2::dk::WaitOrder flush the queue before it waits? Test: tools/tests/dk_flush_before_wait_test.cpp
//
// In deko3d neither dkQueueSubmitCommands nor dkQueueSignalFence sends anything to the GPU: their `flush` argument is
// a cache flush (Queue::signalFence in libdeko3d writes the syncpoint increment twice with the flush bit, and never
// calls Queue::flush). Only dkQueueFlush, dkQueuePresentImage, dkQueueWaitIdle and the queue's own command memory
// running out (Queue::onCmdBufAddMem) kick the GPU. A fence of a submission still in the queue is never reached: a
// CPU wait on it lasts for ever, with the queue not in error (ds2_036: "order 8647 not done after 2..40 s
// (completed 8646, next 8650, queue in error false)"; 8646 was the output, flushed by the present; 8647-8649 the
// three texture update splits, never flushed; the third split's BeginRecording waited for the first).

#include <cstdint>

namespace sa2::dk {

// enabled: sa2_dk_flush_before_wait; order: the order waited for; completed: the highest order known done;
// flushed: the highest order known to have been sent to the GPU (by a flush or a present).
inline bool FlushBeforeWait(bool enabled, uint64_t order, uint64_t completed, uint64_t flushed) {
  return enabled && order > completed && order > flushed;
}

}  // namespace sa2::dk
