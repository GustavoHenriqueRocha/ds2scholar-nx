// sa2_native_targets_dk_present.inc: members of this region (see the class header). Owned by one translation region.
// Translation of TargetsVulkan (sa2_native_targets.cpp) 2614-3852 (gamma ramp, live rescale, Present, diagnostic
// window, Draw, the interface lent to the draws, resolved texture lookups, the lazy front, SendAndWait), 9957-10020
// (resolve and clear bookkeeping) and 10866-11401 (copy inventory, read-backs, the output pass). Bodies in
// sa2_native_targets_dk_present.cpp. Declarations keep the Vulkan order and comments. The data members of 11402-11720
// are declared in infra.inc (agreed with the infra region); the deko3d output objects that take the place of the
// Vulkan ones there (and of the SDK presenter) are declared here, at the end.
  // ---- sa2_native_targets.cpp 2614-3852 ----
  void RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) override;

  // The output table = the game's ramp in 0-1, followed by the neutral mix (saturation 1, vibrance 0,
  // 1 / gamma 1) and effects (no vignette, no scanlines) that the output shader still reads.
  void RecomputeRamp();

  // ---- sa2-nx render resolution: live rescale (docked <-> handheld) -------------------------------------------------
  //
  // Called at the end of each Present, after the Swap's image was painted: when the wanted scale (the console's mode and
  // sa2_render_height_*) differs from the one the images were made at, every render target, depth target and resolved
  // texture gets a new VkImage at the new scale with its content scaled into it (color linear, depth nearest), and
  // keeps everything else: its ImageNative (so every pointer, EDRAM tile owner and memo stays valid), its guest size,
  // its address. Deferred work that names image handles (lazy front copies, resolve aliases) is recorded first. The
  // GPU is drained before and after (a one-frame hitch, only when the mode changes).
  // The device memory of the images the render scale applies to (render targets, depth targets, resolved and front
  // textures): for the [res] lines. Only at a rescale and in the 10 s report, when the scale or MSAA is in use.
  uint64_t BytesScaledImages();

  void RescaleIfWanted();

  // The Vulkan objects of an image that is being replaced (not its bookkeeping: the ImageNative lives on).
  // deko3d: the image id (memory and descriptors go with it; there are no views or framebuffers to forget).
  void ReleaseHandles(const Image& old);

  // ds2_dynamic_resolution_keep: an image's handles at its other scale, keyed by the current VkImage.
  // deko3d: one ImageId holds what were the image, memory and view handles; keyed by the current ImageId.
  struct AlternateImage {
    sa2::dk::ImageId image = sa2::dk::kNoImage;
    uint32_t host_width = 0, host_height = 0;
    uint8_t scale_q = 4;
  };

  void ReleaseAlternates();

  // Destroy's companion: the twin of an image that goes away goes too. Only reached once that image is no longer
  // used (Destroy's callers wait or check first), and the twin was swapped out earlier, so it is older still.
  void ReleaseAlternateOf(sa2::dk::ImageId image);

  void RescaleKeep(uint32_t q, std::chrono::steady_clock::time_point started, uint32_t q_old, uint64_t bytes_old);

  void RescaleAll(uint32_t q);

  // deko3d: `presenter` is null (no SDK presenter): the output chain and the window are this class's (PaintOutput).
  bool Present(rex::ui::Presenter* presenter, const TextureSwap& swap, uint32_t width,
               uint32_t height) override;

  // Per-draw diagnostic window, one whole frame every N seconds. It is decided
  // in the Swap because the upload buffer rotates several times per frame.
  void OpenWindowDiagnostic();

  void Stats(uint64_t& copies, uint64_t& cleared, uint64_t& presented,
                    uint64_t& rejections) const override;

  // report: waits of the ring for the GPU in CompleteA.
  uint64_t waits_gpu_ = 0;
  uint64_t ns_waits_gpu_ = 0;
  // report: cost of Record.
  uint64_t ns_record_ = 0;

  void TimeGpuPerCategory(
      std::array<uint64_t, kGpuCategories>& nanoseconds) const override;

  void StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                            std::array<uint64_t, kGpuCategories>& vertices,
                            std::array<uint64_t, kGpuCategories>& primitives) const override;

  bool Draw(const SubmissionDraw& submission) override;

  StatsDraws StatsOfDraws() const override;

  void WaitUploads() override;

  size_t PendingCopies() const override;

  // --- Interface lent to the draw code ----------------------------------------

  DkCmdBuf CommandsWork() override;

  uint32_t HostMsaaSamples() const override;

  // sa2_msaa: a single-sample image, or one of the host multisampled game targets.
  bool SamplesSupported(const Image& image) const;

  DkCmdBuf CommandsUpload() override;

  uint64_t GenerationCommands() const override;

  ImageNativeDk* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) override;

  ImageNativeDk* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) override;

  void GpuMemoryIsShort() override;

  const ImageNativeDk* ResolvedTexture(uint32_t address) override;

  const ImageNativeDk* ResolvedTextureForFetch(uint32_t address, const uint32_t fetch[6],
                                               const ImageNativeDk& source) override;

  // sa2_native_lazy_front. Deferred copy to a front buffer, and the render target's spare images.
  // deko3d: source_vk / texture_vk keep their names (other regions read them) and hold ImageIds.
  struct PendingFront {
    Image* target = nullptr;           // source render target (targets_ never erases: the pointer stays valid)
    sa2::dk::ImageId source_vk = sa2::dk::kNoImage;  // image holding the content: the target's own or a retained one
    int32_t retained = -1;               // index in fronts_images_ if it is no longer in the target
    sa2::dk::ImageId texture_vk = sa2::dk::kNoImage;
    VkImageCopy copy{};
  };
  struct ImageFront {
    Image image;              // same size, format and usage as the target: swapped with it
    uint32_t retained_per = 0;  // address of the front buffer that retains it; 0 = free
  };

  // Is this address a front buffer read only by the Swap? A Swap presented it recently and no draw has
  // sampled it in that time.
  bool IsFrontOnlyOfTheSwap(uint32_t address) const;

  // The retained image of a deferred copy becomes free again.
  void ReleaseRetained(const PendingFront& pending);

  // The image holding the deferred copy's content, if it is still the same one; otherwise nullptr.
  Image* SourceFront(const PendingFront& pending);

  // Records the deferred copy to that front buffer (outside a pass) and removes it from the list.
  void RecordCopyFront(uint32_t address);

  // Another resolve (or an image swap) reaches that texture. If it covers the whole texture, the deferred
  // copy is unnecessary and is dropped: nobody will see its content. Otherwise it is recorded first.
  void ResolverPreviousFront(uint32_t address, bool whole);

  // Defers the copy if it is 1:1, covers the whole texture from the target's corner, and that address is
  // read only by the Swap.
  bool PostponeCopyFront(uint32_t address, Image& target, const Resolved& resolved, const VkImageCopy& copy);

  // Before writing to a color target in any way other than a full clear (a pass, a restore, an image
  // swap): deferred copies whose source is its image are recorded now (exact).
  void BeforeWriteColor(const Image& target);

  // Before a full clear of a color target. If its image is the source of a deferred front buffer, the
  // target takes a free spare image of the same size and the one holding the content is retained for the
  // front buffer: the clear does not need the old content, so nothing is copied. Without a spare (or with
  // two front buffers from the same image), the copy is recorded first, as usual.
  void RotateFrontBeforeClear(Image& target);

  // A draw samples a front buffer. If its copy is deferred, it is recorded now (before the draw).
  void NoteReadFront(uint32_t address);

  // The Swap of that front buffer, before the submission. Returns the image that can be presented without
  // a copy (the target's or the retained one, with the front buffer in its corner) or nullptr: in that
  // case any deferred copy is recorded here (in the same submission, ahead of the output) and the output
  // reads the texture as usual.
  Image* FrontOnPresent(uint32_t address, uint32_t width, uint32_t height);

  // Upload buffer full: submit what has been recorded and continue in the other slot, with its empty
  // upload buffer (Record only waits if that slot's last submission is still on the GPU).
  bool SendAndWait() override;

  // ---- sa2_native_targets.cpp 9957-10023 ----
  void ResolvedWritten(uint32_t base, uint64_t pixels = 0);

  // The render target's content is no longer that of its last clear (something was copied over it or
  // it changed image). The game's next clear cannot be skipped.
  //
  // It also answers the other question about clears: if the target's content changes without anything
  // drawn since it was cleared, that clear was wiped out by a swap or a restore and served no purpose.
  // It is the equivalent of a loadOp = CLEAR that gets thrown away: these are DONT_CARE candidates.
  void ForgetClear(const Image& target);

  // The largest rectangle the game resolves from this render target: the area it actually uses.
  void NoteAreaUtil(const Image& target, int32_t y1);

  // A clear that does not change a single bit. It is skipped if the render target is already cleared
  // to that same value and nothing has been drawn, anywhere, since then. Returns true if it must be
  // skipped.
  bool RedundantClear(const Image& target, uint64_t value);


  // ---- sa2_native_targets.cpp 10870-11402 ----
  // Copy inventory for the report: for each resolved texture, how many copies there are and how many had
  // draws since the previous copy (a resolve without draws copies again what the render target already
  // had). It has to be complete (address, size, Mpixels and reads) because it is the only thing that tells
  // whether a copy is needed; ~20 addresses fit easily in one line.
  void NoteCopy(uint32_t base, uint32_t width, uint32_t height, uint64_t draws);

  // Read-back of a small resolved texture: the copied rectangle goes to a host-visible buffer in the
  // same submission; WriteReads moves it into guest memory.
  void ResolvedRead(const RegistersCopy& reg, const Resolved& resolved, int32_t x0, int32_t y0,
                    uint32_t dx, uint32_t dy, uint32_t width, uint32_t height);

  // The submission finished: the submitted read-backs go to guest memory. Loaded with its fetch
  // constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the resolved
  // texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it.
  void WriteReads(std::vector<PendingRead>& reads);

  // sa2-nx render resolution: guest-size (unscaled) staging images for read-backs of scaled resolved textures,
  // one per size; recorded and read in the same slot order as the read-back buffers.
  std::unordered_map<uint64_t, Image> staging_reads_;
  Image* StagingRead(uint32_t width, uint32_t height);

  void DestroyRead(Read& read);

  // from_target = the source is a render target's image (or a retained one) with the front buffer in its
  // corner and larger than the output (sa2_native_lazy_front). Only requested with the exact variant.
  // deko3d: no presenter context. The ramp pass, the effects of present_effect (dkoutput::PlanOutput: what the SDK
  // presenter ran after it) and the window present, in one command list of output slot s.
  bool PaintOutput(Image& source, uint32_t width, uint32_t height, bool from_target = false);

  // ---- deko3d: the output pass objects (were pools_output_, commands_output_, fences_output_, descriptors_output_,
  // ramps_output_, sampler_, vs_, fs_, render_pass_output_, pipeline_, fs_ramp_, pipelines_ramp_, framebuffers_ of
  // 11402-11720, and the SDK presenter's effects and swapchain) ----

  // RescaleKeep / RescaleAll: the content of `image` scaled into `fresh` by the 2D engine (their blit_ok and blit,
  // which were the same lines in both). false = not blitted (the caller clears it at its next use, as before).
  bool BlitScaledDk(const Image& image, const Image& fresh, bool depth);

  // Called by Initialize where the Vulkan code made the output sampler, set layout, pool and sets, the ramp buffers,
  // vs_ / fs_, the render pass and pipeline_: the output slots, the ramp buffers, the descriptors, the sampler, the
  // shaders (output_rect, output_bilinear, output_easu, output_rcas) and the window. false if something is missing
  // (logged). DestroyOutput: from the destructor, after the outputs were waited for.
  bool InitializeOutput();
  void DestroyOutput();

  // Variant of the output pass with the ramp, by index: 0 bilinear, 1 exact texel, 2 FXAA (its Quality form from
  // ds2_output_fxaa_quality, read at start). The deko3d form of PipelineRamp: the DKSH of that combination
  // (shaders_dk/sa2_output_ramp_gamma_e*_f*_q*.frag), loaded on first use. nullptr if it fails (and it is logged).
  const DkShader* ShaderRamp(uint32_t index);

  // The window at the display's size (1920x1080 docked, 1280x720 handheld, as WindowSwitch::QueryDisplayResolution
  // without the applet query), recreated when the mode changes (the SDK presenter recreated its swapchain then).
  bool EnsureWindowOutput();

  // The two intermediate images of the effect chain (RGB10A2, the presenter's guest output format), at least
  // width x height; recreated larger when needed.
  bool EnsureIntermediatesOutput(uint32_t width, uint32_t height);

  // One fullscreen pass of the output chain: the 4-vertex strip over `target` (width x height), vs_output_ and
  // `fragment`, `constants` in the fragment's uniform buffer 0 (the rectangle in the vertex's), the slot's ramp in
  // fragment buffer 2 with `ramp`, texture 0 = image descriptor `texture` with sampler_output_.
  void PassOutput(DkCmdBuf cmd, uint32_t s, const DkImageView& target, uint32_t width, uint32_t height,
                  const DkShader* fragment, const void* constants, uint32_t constants_bytes, uint32_t texture,
                  bool ramp);

  // Output with the game's gamma ramp (sa2_native_ramp_gamma): one buffer per output slot (was RampOutput).
  struct RampOutputDk {
    sa2::dk::Block block;           // kCpuToGpu, sizeof(ramp_values_) (the shader's Rampa block)
    uint64_t version = UINT64_MAX;  // the ramp_values_ version the buffer holds
  };
  std::array<sa2::dk::CmdStream, kSlotsOutput> streams_output_dk_{};  // were pools_output_ + commands_output_
  std::array<sa2::dk::Arena, kSlotsOutput> constants_output_dk_{};     // were the push constants
  std::array<RampOutputDk, kSlotsOutput> ramps_output_dk_{};
  // The sa2::dk order of each slot's last output (were fences_output_; pending_outputs_ still says whether it is
  // waited for). WaitOutputs (infra) waits these.
  std::array<uint64_t, kSlotsOutput> orders_output_{};
  // Image descriptors (were descriptors_output_): per slot, the source of its ramp pass (rewritten every output);
  // the two intermediates.
  std::array<uint32_t, kSlotsOutput> descriptors_source_output_{UINT32_MAX, UINT32_MAX, UINT32_MAX};
  std::array<uint32_t, 2> descriptors_intermediate_output_{UINT32_MAX, UINT32_MAX};
  uint32_t sampler_output_ = UINT32_MAX;  // linear, clamp to edge (was sampler_)
  std::array<sa2::dk::ImageId, 2> intermediates_output_{};
  uint32_t width_intermediates_output_ = 0, height_intermediates_output_ = 0;
  const DkShader* vs_output_ = nullptr;       // output_rect.vert (was vs_: guest_output_triangle_strip_rect_vs)
  const DkShader* fs_output_ = nullptr;       // output_bilinear.frag (was fs_ / pipeline_: guest_output_bilinear_ps)
  const DkShader* fs_easu_output_ = nullptr;  // output_easu.frag (the presenter's FSR EASU)
  const DkShader* fs_rcas_output_ = nullptr;  // output_rcas.frag (the presenter's FSR RCAS, and CAS's place)
  std::array<const DkShader*, 3> shaders_ramp_{};  // ShaderRamp (were pipelines_ramp_)
  std::array<bool, 3> shaders_failed_ramp_{};      // (were pipelines_failed_ramp_)
  sa2::dk::PassState state_output_;
  sa2::dk::Window window_output_;
  bool window_output_ready_ = false;
  bool window_output_docked_ = false;
  bool logged_output_plan_ = false;   // the chain is logged when it changes
  uint8_t logged_output_effect_ = 0;  // dkoutput::Effect of the last logged chain
