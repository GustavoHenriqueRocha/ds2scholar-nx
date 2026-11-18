// sa2_native_draws_dk_1.inc: members of the deko3d draws class (DrawsDkImpl, sa2_native_draws_dk.h) owned by
// region draws-1, the translation of lines 1-8158 of sa2_native_draws.cpp (DrawsVulkanImpl lines 2807-8157):
// the constructor and destructor, Initialize, Draw and its helpers (framing, base zero, the deferred sky, the
// pipeline-change counters, the reports), the pass end, BeforeSend / UseSlot, the texture-cache eviction, the
// clears in pass, ForgetImage, Stats and the vertex copy thread. Bodies: sa2_native_draws_dk_1.cpp, in the order of
// the Vulkan file; only methods of one to three lines are inline here. Names are the Vulkan ones. The data members
// are region 06's (sa2_native_draws_dk_2.inc), except the deko3d-only ones at the end of this file.
// What did not carry over is listed in ds2-deko3d docs/pesquisas/traducao/05-draws-1.md ("Fase C").

  DrawsDkImpl(rex::memory::Memory* memory, ContextTargetsDk* context) : memory_(memory), context_(context) {}

  ~DrawsDkImpl() override {
    ReportPayloadAudit("shutdown");
    StopPrewarmed();  // uses the pipeline cache and the layout: first
    StopPreloadShaders();
    StopCopies();
    // (StopBindings: no texture bind thread in deko3d.)
    SaveCachePipelines();
    StopWriterCache();  // writes whatever is still pending
    // deko3d: pipelines, the driver pipeline cache, framebuffers, render passes, shader modules, image views,
    // samplers, descriptor pools and layouts are not objects: nothing to destroy for them. The DKSH code lives in
    // code_arena_ and the descriptors in sa2::dk's sets, both released with the base layer.
    for (auto& [key, texture] : textures_) {
      DestroyImage(texture.image);
    }
    for (ImageNativeDk& empty : empty_) {
      DestroyImage(empty);
    }
    // (pool_textures_.Finish: no pool member, the image blocks belong to sa2::dk.)
    // Blocks are freed once whatever is submitted has completed (the GPU may still read the last slot).
    for (sa2::dk::Arena& s : shared_bufs_) {
      s.Release(sa2::dk::CurrentOrder());
    }
    for (sa2::dk::Arena& s : uploads_) {
      s.Release(sa2::dk::CurrentOrder());
    }
    sa2::dk::FreeBlock(vcache_buf_, sa2::dk::CurrentOrder());
    spec_table_.Release();
  }
  bool Initialize();
  bool Draw(const SubmissionDraw& p) override;
  void EmitPostponedSky(DkCmdBuf cmd, uint32_t reason);
  void ReportSubstages();
  bool CheckBaseZero(VkDeviceSize offset, VkDeviceSize stride, uint64_t bytes, uint32_t vmin,
                         uint32_t& first_vertex, VkDeviceSize limit);
  void ReportSet4();
  void ReportBaseZero();
  uint32_t ComputeFraming(const uint32_t* r, uint32_t vte, bool no_clip, uint32_t sc_mode, VkViewport& viewport,
                            float ndc[4], VkRect2D& scissor, me::native::Msaa2PhaseProbe phase_probe);
  void CompareFraming(const VkViewport& viewport, const float ndc[4], const VkRect2D& scissor, uint32_t empty);
  uint32_t& UtilHeightFor(uint32_t pitch);
  uint64_t CheckPassKey(const uint64_t keys[5], uint64_t n);
  void ReportMinutiae();
  bool CheckCellPipeline(uint64_t fingerprint, const PipelineKey& key, const PipelineDk* in_cell, uint64_t n);
  static uint32_t ClassTopology(uint32_t topology);
  static void CanonicalState(const PipelineKey& c, uint32_t blend[4], uint32_t& masks, uint32_t& depth);
  void CountChangePipeline(const PipelineKey& new_entry, bool after_begin_pass);
  void ReportChangesPipeline(std::chrono::steady_clock::time_point now);
  void ReportPipelinesDirect();
  void CutSubstage(size_t k, std::chrono::steady_clock::time_point& t);
  void Stage(size_t stage, std::chrono::steady_clock::time_point& mark);
  void NoteVerticesRepeated(uint64_t address, uint32_t bytes, uint32_t order,
                               const uint8_t* data);

  void SetHostMsaaSamples(uint32_t samples) { msaa_samples_ = samples > 1 ? samples : 1; }

  // sa2_msaa: single-sample images, and the host multisampled game targets.
  bool SamplesSupported(const ImageNativeDk& image) const {
    return me::native::IsSingleSample(uint32_t(image.sample_count)) ||
           (msaa_samples_ > 1 && uint32_t(image.sample_count) == msaa_samples_);
  }
  void NotifyRescale(uint32_t q_old, uint32_t q_new) override;
  void NotifyGraphicsExternalState() override;
  void FinishPass() override;
  void BeforeSend() override;
  void UseSlot(uint32_t slot) override;

  void InvalidateTextures() override { ++generation_textures_; }
  void InvalidateImages(sa2::dk::ImageId a, sa2::dk::ImageId b) override;

  uint64_t Drawn() const override { return drawn_; }

  // sa2_native_textures_mb_max: with the texture cache above the limit, evict the textures unused for
  // the longest until it drops to 75 %, at most once every 60 frames.
  // It is safe without waiting for the GPU: a texture not prepared for 120 frames cannot be referenced by
  // any pending submission. The slot caches are valid for at most 32 frames after preparing, and only the
  // previous submission can still be on the GPU (Record waits for the slot before reusing it). The heap
  // slots are updated with UPDATE_AFTER_BIND, and the new generation invalidates the caches.
  static constexpr uint64_t kFramesNoUsageForDrop = 120;
  void NoteMemoryOfTheGpu();

  /*
   * Without stutters.
   *
   * The first version waited until the limit was exceeded and evicted 25 % at once. In play that is a
   * stutter, and a stutter every few minutes is no better than running out of memory every fourteen.
   *
   * Now a few are evicted per frame as soon as the cache nears the limit, and only cold textures: those
   * unused for 120 frames, i.e. two to four seconds. They are not in the working set, so they do not have
   * to be uploaded again right away and there is no thrashing. If there are not enough cold ones, nothing
   * is forced: the cache is allowed to grow, since going a little over is better than uploading and
   * evicting the same thing.
   */
  static constexpr uint32_t kDropPerFrame = 4;

  // sa2_native_texture_slots_evict: slots in use of an image heap (slot 0 is the empty texture and never handed out).
  uint32_t SlotsUsed(uint32_t heap) const {
    const Heap& m = heaps_[heap];
    return m.next - 1 - uint32_t(m.free.size());
  }

  // A view found no free slot: this frame it shows the empty texture; the next eviction pass makes room.
  void NoteSlotsFull(uint32_t heap) {
    if (heap < 3) slots_full_heaps_ |= 1u << heap;
    ++slots_full_events_;
  }

  static uint32_t HeapOfTexture(const Texture& texture) {
    return texture.layers == 6 ? 2u : texture.background ? 1u : 0u;  // cube, 3D, 2D (PrepareTexture's `heap`)
  }
  uint32_t DropColdOfHeap(uint32_t heap, uint32_t count);
  void EvictTexturesForSlots();
  void EvictTexturesIfDoesMissing();
  void DropSomeColdFew();
  bool DropTexturesPerMissingOfMemory() override;
  void DropImages(const std::unordered_set<sa2::dk::ImageId>& images);
  uint64_t DropTextures(uint64_t goal, uint64_t age_min, const char* reason);
  bool ClearDepthInPass(DkCmdBuf commands, const ImageNativeDk& image,
                               float depth, uint32_t stencil,
                               const VkRect2D* area = nullptr) override;
  bool ClearStencilInPass(DkCmdBuf commands, const ImageNativeDk& image,
                           uint32_t stencil, const VkRect2D& area) override;
  bool ClearOnlyDepthInPass(DkCmdBuf commands, const ImageNativeDk& image,
                                   float depth, const VkRect2D& area) override;
  bool ClearColorInPass(DkCmdBuf commands, const ImageNativeDk& image, const VkClearColorValue& color,
                         const VkRect2D& area) override;
  void ForgetImage(sa2::dk::ImageId image) override;
  StatsDraws Stats() const override;
  bool Reject(uint32_t cause, const char* text);
  void Warn(uint32_t cause, const char* text);
  // (The texture bind thread, DecideBindings ... ReportBindings: fica de fora in deko3d, see the .cpp.)

  // --- Vertex copies on a separate thread (sa2_native_uploads_thread) ------------------------------------------
  // One producer (the ring thread, in Draw) and one consumer (copies_thread_). The producer writes the job
  // into written_copies_ and publishes it; the thread copies up to there and publishes done_copies_.
  // With the queue full the copy is done on the ring, as before. The destinations are distinct ranges of
  // the upload buffer, so order does not matter. The copies must be waited for before submitting the work
  // (BeforeSend) and before returning the read pointer to the game (WaitUploads from
  // the native graphics system): after that the game may reuse its vertex buffers.
  //
  // Sleeping and waking are decided under the lock. Two earlier versions hung: the copy thread asleep with
  // 1,123 copies queued and the ring waiting for them. Each side used to decide without the lock whether to
  // wake the other, Dekker-style: the ring published written_copies_ and read done_copies_, and the
  // thread the other way round. With store(..., memory_order_release) MSVC's STL emits a plain mov, and on
  // x86 (TSO) a store may not yet be visible from another core: both sides missed the wake-up. ARM gives no
  // guarantee either. Now copies_sleeping_ and copies_waiting_ are only touched under the lock, and
  // nothing is decided without it: the wake-up arrives at the next point that takes the lock (every
  // kCopiesPerWarning copies, when the ring waits, and when the thread runs out of work). There is no atomic
  // hint flag any more; the thread always signals from its no-work path, under the lock, which it goes
  // through as soon as it finishes.
  // Only the queuing thread may wait (single producer). Otherwise it wakes the thread once and waits
  // without helping.
  //
  /*
   * The ring helps instead of waiting (sa2_native_uploads_help).
   *
   * Why. In play the ring waited 14.7 ms for the copy thread during one stutter (3,965 draws, 5 waits)
   * and 17.9 ms in the next (3,890 draws, 3 waits; the game spent 21.1 ms without room in the ring). Copying
   * what those frames needed is ~9-11 MB, ~6-7 ms at the rate the profile gives the thread (1.5 GB/s): the
   * rest was the thread ready and without a core. It ran at 0x3B, below the two game threads (0x3A), and
   * Horizon only moves a ready thread to another core when that core runs out of work: with the CPU at
   * 278 % out of 300 (the profile block for that stutter), almost never. And the ring (0x2D) waited for it
   * on a condition variable, which does not lend priority: a textbook priority inversion.
   *
   * How. On every wait with pending copies (by the queuing thread, in the applying phase):
   *   1. the ring takes and copies, in batches of kCopiesPerChunk, whatever the thread has not taken yet.
   *      If the thread is running on another core they share what is left; if it has no core, the ring
   *      does it all, without waiting for it.
   *   2. whatever is missing can only be the batch the thread is working on. The ring waits by taking
   *      copies_chunk_mutex_, which the thread holds while copying: a libnx mutex waits in the kernel
   *      (svcArbitrateLock) and Horizon lends the owner the waiter's priority, so the thread rises to 0x2D
   *      until it releases it. It is one batch.
   * The thread's own priority is not changed: it takes no core from anyone while the ring is not waiting
   * for it.
   *
   * Counters. written_copies_ (ring only), taken_copies_ (the thread or the ring, with CAS, in order)
   * and done_copies_ (how many are done, whoever did them): done <= taken <= written. Outside
   * WaitUploads only the thread takes, so it finishes in order and "done = h" still means the first h
   * are done: EnqueueCopy's space accounting still holds. Inside, the ring does not queue, and it leaves
   * with everything done.
   *
   * Same data. Each copy is done exactly once by CopyVertices with the same slot (source, destination,
   * words and byte order), and all of them before returning from WaitUploads, as before: before
   * submitting and before returning the read pointer. Only who does it changes, and sometimes it is done
   * earlier.
   *
   * Self-checking guard. Each copy leaves its index + 1 in copies_marks_. On each wait the ring checks the
   * marks of what was queued since the previous one and that the counters add up; a copy without a mark is
   * done right there, before submitting and before returning the read pointer, and it is a MISMATCH
   * (REXLOG_ERROR). Phases: observing (the first kWaitsCopiesLooking waits with pending copies: the
   * thread already takes batches, the ring waits as before and checks), applying (help) and, after a
   * MISMATCH with help on, no help (the plain wait). A MISMATCH without help, or with the counters out
   * of step, turns the thread off for the session and the copies go back to the ring (the original
   * behaviour). The plain wait no longer waits forever: after 2 s it takes the batch mutex and finishes the
   * job. The batch mutex has no timeout because inside it the thread only takes and copies.
   */
  enum PhaseCopies : int32_t { kCopiesNoHelp = 0, kCopiesLooking = 1, kCopiesApplying = 2 };
  void WarnOtherThreadCopies(const char* where);
  bool EnqueueCopy(const WorkCopy& work);
  void WakeCopies();
  size_t PendingCopies() const override;
  void WaitUploads() override;
  void WaitCopiesAsAlways(size_t goal);
  bool TakeAndCopy(bool ring, uint64_t& copies, uint64_t& bytes);

  bool HasCopiesNoTake() const {
    return taken_copies_.load(std::memory_order_acquire) != written_copies_.load(std::memory_order_acquire);
  }
  void CheckCopies(size_t goal);
  size_t RedoNoMark(size_t goal);
  void RepairCopies(size_t goal, const std::string& reason);
  const char* PhaseNameCopies() const;
  void ReportCopies();
  void ReportCacheFetch();
  void StopCopies();
  void LoopCopies();

  // ---- deko3d-only data members of this region (none of them exists in the Vulkan class) ---------------------
  // What a VkPipeline baked is bound as groups of state by differences (sa2_native_draws_dk_types.h).
  StateTracker state_tracker_;
  // The vertex input of the last draw (a VkPipeline carried it): bound again only when it differs.
  VerticesLayout layout_bound_{};
  bool layout_bound_valid_ = false;
  // The deferred sky's vertex input (region 06's PostponedSky has no field for it).
  VerticesLayout sky_layout_{};
// deko3d: the descriptor generation this object last put a barrier for (sa2::dk::DescriptorsWrittenSince).
uint64_t descriptors_seen_ = 0;
