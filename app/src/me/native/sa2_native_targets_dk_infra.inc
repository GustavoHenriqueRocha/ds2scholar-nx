// sa2_native_targets_dk_infra.inc: members of this region (see the class header). Owned by one translation region.
// Translation of TargetsVulkan (sa2_native_targets.cpp) 932-1752 (construction, Initialize, the output pass),
// 3852-4062 (submission, GPU marks and statistics lent to the draws, the guest -> host helpers) and 10021-10866
// (image creation and destruction, the work slots: Record, Complete, SendWork, the wait reports). Bodies in
// sa2_native_targets_dk_infra.cpp. Declarations keep the Vulkan order and comments.
// Also the data members of 11402-11720 (agreed with the present region, which owns that range of lines), at the end;
// the Vulkan output objects among them are replaced by the present region's deko3d output objects.

  // ---- sa2_native_targets.cpp 932-1752 ----
  explicit TargetsDk(rex::memory::Memory* memory);

  ~TargetsDk() override;

  bool Initialize();

  // CreatePipelineOutput / PipelineRamp (Vulkan 1660-1748): no pipeline objects in deko3d. The output pass and its
  // ramp variants are the present region's (InitializeOutput, ShaderRamp, PassOutput).

  // deko3d (new): the renderer's own shaders, DKSH embedded by region 07, loaded once into code memory.
  // nullptr if the shader is not valid (logged).
  const DkShader* LoadShader(dkshaders::Id id);
  std::unordered_map<uint32_t, const DkShader*> shaders_loaded_;  // LoadShader's cache, by dkshaders::Id
  sa2::dk::CodeArena code_;

  // deko3d (new): what every new command list starts with (vkBeginCommandBuffer had no equivalent): the two
  // descriptor sets, and the invalidations owed to descriptor or code memory written since the last list.
  void StartList(DkCmdBuf cmd);
  uint64_t descriptors_seen_ = 0;  // sa2::dk::DescriptorsGeneration() at the last StartList
  // deko3d (new): the push constants of the renderer's own passes, as a uniform buffer in the slot's arena
  // (DK_GPU_ADDR_INVALID if it is full: EnsureCapacityConversionEDRAM4 rotates before that).
  DkGpuAddr PushConstants(const void* data, uint32_t bytes) { return slots_[slot_].constants.Push(data, bytes); }
  // deko3d (new): a transient image descriptor of the slot being recorded (the descriptor sets the Vulkan code
  // allocated from the slot's pools). UINT32_MAX if the ring is full.
  uint32_t PushDescriptor(const DkImageView& view, bool load_store = false, bool decay_ms = false) {
    return slots_[slot_].descriptors.Push(view, load_store, decay_ms);
  }
  // deko3d (new): room in the slot being recorded for one more EDRAM operation (conversions, descriptors and
  // constants).
  bool RoomSlotEDRAM4() const;

  // ContextTargetsDk (new in deko3d): the work slot being recorded.
  uint32_t SlotWork() const override { return slot_; }

  // ---- sa2_native_targets.cpp 3852-4062 ----
  // SendAndWait (3850-3852): declared by the present region (its comment and the line close that range).

  // ds2_native_partial_send (fh2-recompiled 79212ba, same renderer family): the recording so far goes to the GPU
  // now, without a fence, and goes on in this slot with new command buffers from its pools (reset with them).
  // The slot's final submission still carries its only fence, and the read-backs wait for it.
  bool SendPartial() override;

  // The function above submits and continues; this one waits. Same order as every other place that must
  // destroy something the GPU might still be reading (see the resolved texture that changes size).
  bool WaitGpuOfTheAll() override;

  void MarkGpu(uint32_t category) override;

  void DescribeMarkGpu(uint64_t description) override {
    WorkSlot& slot = slots_[slot_];
    if (!slot.descriptions.empty()) slot.descriptions.back() = description;
  }

  void CountDrawMarkGpu() override {
    WorkSlot& slot = slots_[slot_];
    if (!slot.draws_mark.empty()) ++slot.draws_mark.back();
  }

  void LabelMarkGpu(uint32_t label) override {
    WorkSlot& slot = slots_[slot_];
    if (!slot.labels.empty() && !slot.labels.back()) slot.labels.back() = label;
  }

  // One query per pass, from before vkCmdBeginRenderPass to after EndRenderPass.
  uint32_t BeginStats(uint32_t category) override;

  void FinishStats(uint32_t index) override;

  // One query per draw, tagged with its pixel shader.
  uint32_t BeginStatsDraw(uint32_t label, uint32_t category, uint32_t vs_more_one) override;

  void FinishStatsDraw(uint32_t index) override;

  // sa2-nx render resolution (me_resolution_scale.h): guest coordinates of an image in its host pixels. Every Vulkan
  // command on a render target or resolved texture goes through these; with scale_q 4 they return their input.
  static int32_t HostEdge(const Image& image, int32_t guest) { return ScaleEdge(guest, image.scale_q); }
  static VkRect2D HostRect(const Image& image, const VkRect2D& guest) { return ScaleRect(guest, image.scale_q); }
  // A guest-unit copy between two images (each side at its own scale; the extent is what both have).
  static VkImageCopy HostCopy(const Image& source, const Image& target, VkImageCopy copy);
  static void HostCopies(const Image& source, const Image& target, VkImageCopy* copies, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) copies[i] = HostCopy(source, target, copies[i]);
  }

  // ---- sa2_native_targets.cpp 10021-10866 ----
  // width / height are the GUEST size: the image is that size scaled by scale_q_ / 4 (sa2-nx render resolution,
  // me_resolution_scale.h); image.width / height keep the guest size, image.host_width / host_height the extent.
  // deko3d: usage is DkImageFlags_* (was VkImageUsageFlags; FlagsOfUsage maps a Vulkan usage). CreateScaled adds
  // what every image needs here (UsageRender: Prepare clears through a bound target; Usage2DEngine on color images:
  // blits and whole-image resolves; HwCompression by sa2_dk_compress_*), and the image keeps the result in dk_flags to
  // be recreated at another scale.
  bool Create(Image& image, uint32_t width, uint32_t height, uint32_t usage,
             VkFormat format = kColorFormat,
             VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT) {
    return CreateScaled(image, width, height, usage, format, sample_count, scale_q_);
  }

  bool CreateScaled(Image& image, uint32_t width, uint32_t height, uint32_t usage, VkFormat format,
                    VkSampleCountFlagBits sample_count, uint32_t q);
  // deko3d (new): the DkImageFlags of a Vulkan usage, for call sites that still reason in Vulkan usage bits.
  static uint32_t FlagsOfUsage(VkImageUsageFlags usage, VkFormat format);

  void Destroy(Image& image);

  // First time: to GENERAL (valid for copy, clear and sampling) and cleared to zero.
  // deko3d: no layouts; only the clear remains.
  void Prepare(Image& image);

  // All three descriptor pools share this budget. Never reset a live pool or
  // retain a slot reference / descriptor allocation across submission rotation.
  // deko3d: the pools are the slot's transient descriptor ring; the rule is the same.
  uint64_t edram4_claimed_clears_ = 0;  // ds2_clear_depth_claim: clears taken by the drawn view since the report
  std::map<std::string, uint64_t, std::less<>> edram4_operations_;  // ME: mode-4 operations per kind since the last report
  bool EnsureCapacityConversionEDRAM4(const char* operation);

  bool Record();

  bool BeginRecording();

  // Waits for that slot's submission. Earlier submissions that have already finished are collected
  // first, so read-backs are written in frame order.
  void Complete(WorkSlot& slot);

  void CompleteA(WorkSlot& slot);

  // Per-draw statistics of a finished submission, added to its pixel shader.
  void ReadStatsDraw(WorkSlot& slot);

  // the pixel shaders with the most GPU time in the diagnostic frames since the last report
  // (sa2_native_stats_per_draw_s). Times are serialized per draw and x1.627 on Switch (NVK
  // timestamps).
  // deko3d: the counters are the same raw GPU ticks (625/384 ns), so x1.627 still holds.
  void ReportCostPerShader();

  // Pass statistics of a finished submission, added to their category.
  void ReadStats(WorkSlot& slot);

  bool SendWork(bool wait);

  // banjo-nx: parked resolved images (banjo_native_park_resolved), by address, size and host format.
  static uint64_t ParkedKey(uint32_t base, uint32_t width, uint32_t height, uint32_t format) {
    return (uint64_t(base) << 32) ^ (uint64_t(width & 0xFFF) << 20) ^ (uint64_t(height & 0xFFF) << 8) ^
           (uint64_t(format) * 0x9E3779B1ull);
  }
  std::unordered_map<uint64_t, Resolved> parked_resolved_;
  uint64_t parked_resolved_stores_ = 0, parked_resolved_reuses_ = 0;

  // full GPU drains per call site since the last report (see ReportWaitsGpu).
  std::array<uint64_t, 16> waits_gpu_reason_{};
  void ReportWaitsGpu();
  uint64_t report_sends_ = 0, report_full_sends_ = 0, report_ns_full_sends_ = 0;
  uint64_t report_partial_sends_ = 0;  // ds2_native_partial_send, in the "submissions since last report" line
  uint64_t report_waits_gpu_ = 0, report_ns_waits_gpu_ = 0;

  void WaitGpu();

  // All pending outputs, before destroying or reusing what they use.
  void WaitOutputs();

  // ---- sa2_native_targets.cpp 11402-11720: the data members (declared here by agreement with the present region) ----
  // The Vulkan-only ones are left as one-line notes where they were.

  rex::memory::Memory* memory_;

  // pools_output_, commands_output_, fences_output_ (output slots): the present region's streams_output_dk_ and
  // orders_output_.
  uint32_t reductions_ = 0;  // resolves that had to shrink
  // Those of the slot being recorded (Record switches them).
  DkCmdBuf commands_work_ = nullptr;
  DkCmdBuf commands_upload_ = nullptr;
  // Three slots by default, up to four. With two, the CPU is only one frame ahead and waits for the GPU inside
  // Record() half the time. How many are actually used is set by sa2_native_slots_work.
  std::array<WorkSlot, 4> slots_{};
  uint32_t used_slots_ = 3;  // set by the cvar (sa2_native_slots_work)
  uint32_t slot_ = 1;  // Record starts with slot 0
  uint64_t sends_ = 0;
  bool recording_ = false;
  std::array<bool, kSlotsOutput> pending_outputs_{};
  uint32_t output_current_ = 0;
  bool output_no_wait_ = false;
  bool precise_marks_ = false;
  bool marks_categories_ = true;
  bool invalidate_each_copy_ = false;
  bool recording_upload_ = false;
  uint64_t generation_commands_ = 0;
  // GPU time of the submissions (timestamps). deko3d: counter reports, Capacity() == 0 = not available.
  sa2::dk::Counters queries_;
  // Pipeline statistics per pass category.
  sa2::dk::Counters stats_;  // kReportsStat reports per query (start and end of each counter)
  std::array<uint64_t, kGpuCategories> fragments_category_{};
  std::array<uint64_t, kGpuCategories> vertices_category_{};
  std::array<uint64_t, kGpuCategories> primitives_category_{};
  uint64_t stats_no_site_ = 0;
  sa2::dk::Counters stats_draw_;
  // two timestamps per diagnostic draw (before / after), and GPU time, fragments and draws
  // per (category, pixel shader) of the diagnostic frames since the last report.
  sa2::dk::Counters marks_draw_;
  struct CostShader {
    uint64_t ns = 0, fragments = 0, draws = 0, vertices = 0, primitives = 0;
    uint32_t vs = 0, draw_more_expensive_ns = 0;  // VS of the costliest draw
  };
  std::unordered_map<uint32_t, CostShader> cost_per_shader_;
  uint64_t cost_frames_ = 0;
  uint64_t stats_draw_no_site_ = 0;
  std::array<uint64_t, 3> no_swap_{};          // no clear, not whole, not possible
  uint32_t warnings_no_swap_ = 0;
  bool window_diagnostic_ = false;
  bool read_window_ = false;
  std::chrono::steady_clock::time_point last_window_ = std::chrono::steady_clock::now();
  double period_mark_ns_ = 0.0;
  uint64_t gpu_ns_ = 0;
  std::array<uint64_t, kGpuCategories> gpu_categories_ns_{};
  uint64_t mark_previous_end_ = 0;  // end timestamp of the last submission read (gap between submissions)
  // Present: how long it waits for the previous output.
  uint64_t ns_wait_output_ = 0;
  static uint64_t Ns(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point until) {
    return until > from ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(until - from).count()) : 0;
  }
  std::chrono::steady_clock::time_point last_swap_{};
  // blit_ (vkCmdBlitImage): dkCmdBufBlitImage / sa2::dk::Blit, always there.
  // The game's shadow map, and the scale it is being drawn at (0 = not decided yet).
  static constexpr uint32_t kSideShadows = 1600;
  bool scalable_depth_ = false;
  uint32_t scale_shadows_ = 0;
  // Read-back of small resolved textures (sa2_native_resolved_read_texels).
  std::unordered_map<uint64_t, Read> reads_;
  std::unordered_map<uint64_t, TargetRead> reads_per_target_;  // target report and cadence
  std::chrono::steady_clock::time_point report_reads_{};
  std::vector<PendingRead> pending_reads_;  // recorded in the current submission
  uint32_t logged_diag_reads_ = 0, diag_reads_seen_ = 0;  // ds2_diag_fetch_base
  uint64_t done_reads_ = 0;
  // Copy inventory (NoteCopy).
  uint64_t drawn_last_copy_ = 0;
  std::unordered_map<uint32_t, CopyTarget> copies_per_target_;
  std::chrono::steady_clock::time_point report_copies_{};
  VkFormat format_depth_ = VK_FORMAT_UNDEFINED;
  // Draw code: draws with the library shaders (nullptr without the required capabilities).
  std::unique_ptr<DrawsDk> draws_;

  std::unordered_map<uint64_t, Image> targets_;
  std::unordered_map<uint64_t, Image*> targets_color_active_;
  std::array<OwnerTileEDRAM, xenos::kEdramTileCount> owners_tiles_edram_{};
  std::array<OwnerTileEDRAM, xenos::kEdramTileCount> owners_tiles_edram4_{};
  // views_depth_edram_, passes_import_depth_edram_, framebuffers_import_depth_edram_, passes_conv_color_frag_,
  // framebuffers_conv_color_frag_, views_raw64_rt_edram_: Vulkan views, render passes and framebuffers; their
  // deko3d shape (views on the spot, shaders and pass keys) is the EDRAM region's (as the copies region noted).
  const DkShader* fs_conv_r64_frag_ = nullptr;
  const DkShader* fs_resolve_copy_ = nullptr;  // sa2_gpu_resolve_draw
  const DkShader* fs_resolve_copy_raw64_ = nullptr;  // ds2_copy_dma_as_draw
  uint64_t resolve_draws_ = 0, resolve_draws_declined_ = 0, resolve_draws_report_ = 0, resolve_draws_declined_report_ = 0;
  // sa2_gpu_resolve_alias (the types are declared with MakeAlias).
  std::unordered_map<uint32_t, AliasResolve> aliases_;  // by the alias's address
  LastResolve last_resolve_;
  bool off_alias_ = false;
  uint64_t edram4_transfers_all_ = 0;
  uint64_t aliases_made_ = 0, aliases_dropped_ = 0, aliases_copied_ = 0, aliases_exchanged_ = 0;
  uint64_t aliases_made_report_ = 0, aliases_dropped_report_ = 0, aliases_copied_report_ = 0,
           aliases_exchanged_report_ = 0;
  // layout_conv_color_frag_ / layout_pipeline_conv_color_frag_: no layouts in deko3d.
  const DkShader* vs_conv_color_frag_ = nullptr;
  const DkShader* fs_conv_color_frag_ = nullptr;
  bool conv_color_frag_failed_ = false;
  uint64_t conv_color_frag_uses_ = 0;
  std::array<uint64_t, 16> conv_color_frag_reasons_{};  // why the fragment path declined (return order in the function)
  uint64_t conv_color_frag_rejections_ = 0, conv_color_frag_uses_report_ = 0, conv_color_frag_rejections_report_ = 0;
  std::array<Image*, 5> edram4_draw_images_{};
  std::array<bool, 5> edram4_draw_writes_{};
  me::native::EdramBoundPlan edram4_draw_plan_{};
  VkRect2D edram4_draw_area_{};
  uint64_t edram4_exports_ = 0;
  uint64_t edram4_imports_ = 0;
  uint64_t edram4_stencil_preserves_ = 0;
  uint64_t edram4_capacity_rollovers_ = 0, edram4_capacity_rollover_failures_ = 0;
  std::unordered_map<std::string, uint32_t> edram4_rejections_per_class_;
  bool edram4_log_cap_ = false;
  bool edram4_operation_failed_ = false;
  bool import_depth_edram_failed_ = false, edram4_float_import_warned_ = false;
  std::unordered_set<uint64_t> overlaps_edram_recorded_;
  std::unordered_set<uint64_t> conversions_edram_recorded_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  uint64_t revision_resolved_ = 0;
  std::deque<ResolvedClip> resolved_clips_;
  std::unordered_map<sa2::dk::ImageId, AlternateImage> alternates_;  // ds2_dynamic_resolution_keep
  uint64_t clips_bytes_ = 0;
  uint64_t clips_allocations_ = 0, clips_copies_ = 0, clips_cache_hits_ = 0;
  uint64_t clips_failures_ = 0;
  std::chrono::steady_clock::time_point clips_report_{};
  std::vector<ResolvedSleeping> resolved_sleeping_;
  uint64_t resolved_sleeping_bytes_ = 0;
  uint64_t order_completed_work_ = 0;
  bool resolved_pool_device_error_ = false;
  bool resolved_pool_startup_eligible_ = false;
  uint64_t resolved_pool_hits_ = 0, resolved_pool_retirements_ = 0;
  uint64_t resolved_pool_evictions_ = 0, resolved_pool_waits_ = 0;
  uint64_t resolved_pool_allocations_ = 0, resolved_pool_ineligible_ = 0, resolved_pool_busy_ = 0;
  std::chrono::steady_clock::time_point resolved_pool_report_{};
  std::unordered_set<uint64_t> partial_recorded_;
  std::unordered_set<uint64_t> partial_refused_;  // sa2_native_partial_resolve_inside: pairs already logged
  bool diag_stale_done_ = false;                  // sa2_diag_stale_resolved: made its texture
  // sa2_native_resolved_retire_idle
  uint64_t retired_resolved_count_ = 0, retired_resolved_bytes_ = 0, report_resolved_next_ = 600;
  uint64_t oldest_read_resolved_ = 0, fetches_after_retirement_ = 0;
  uint32_t oldest_read_address_ = 0, retired_resolved_logged_ = 0, fetches_after_retirement_logged_ = 0;
  std::unordered_map<uint32_t, uint64_t> retired_resolved_;  // PC build: address -> presented_ when released
  std::atomic<bool> retire_resolved_urgently_{false};        // GPU memory was short: release at the next Swap
  VkRect2D area_edram_{};
  uint32_t msaa_edram_actual_ = 0;
  // sa2-nx render resolution: the scale (quarters of the guest size, me_resolution_scale.h) new images are created at;
  // RescaleAll moves every existing image to a new one at a frame boundary (docked <-> handheld).
  uint32_t scale_q_ = kScaleQuartersNative;
  uint64_t rescales_ = 0;
  // sa2_msaa: the host sample count of the game's single-sample render targets (1 = none, the default), and the
  // resolve that copies one into a single-sample texture.
  uint32_t msaa_samples_ = 1;
  // resolve_image_ (vkCmdResolveImage): dkCmdBufResolveImage / sa2::dk::ResolveWhole (whole images only).
  uint64_t msaa_resolves_ = 0, msaa_depth_resolves_ = 0;
  bool depth_raster_grid_eligible_ = false;
  static constexpr uint32_t kDiagRegsColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};  // RB_COLOR_INFO, RB_COLORn_INFO
  std::unordered_map<uint64_t, Image> depths_;

  // sampler_ (the output set's): the present region's sampler_output_. Layouts: none in deko3d.
  uint32_t sampler_depth_edram_ = UINT32_MAX;  // sampler descriptor (nearest, clamp)
  // The compute "pipelines" are their shaders (dkCmdBufBindShaders, DkStageFlag_Compute): shaders_* folded in.
  std::array<const DkShader*, 5> pipelines_conversion_depth_edram_{};
  const DkShader* vs_import_depth_edram_ = nullptr, *fs_import_depth_edram_ = nullptr;
  const DkShader* fs_depth_to_depth_edram_ = nullptr;
  const DkShader* fs_depth_to_stencil_edram_ = nullptr, *fs_color_to_stencil_edram_ = nullptr;
  const DkShader* fs_stencil_msaa2_edram_ = nullptr, *fs_stencil_msaa2_to_1x_edram_ = nullptr;
  const DkShader* fs_depth_to_depth_msaa2_edram_ = nullptr;
  const DkShader* fs_depth_msaa2_to_depth_1x_edram_ = nullptr;
  const DkShader* pipeline_resolve_depth_msaa2_edram_ = nullptr;  // was shader_ + pipeline_resolve_depth_msaa2_edram_
  const DkShader* fs_raw64_to_depth_edram_ = nullptr;
  bool raw64_edram_supported_ = false;
  // views_raw64_edram_: views on the spot (RGBA16_Uint over the RGBA16F image), the EDRAM region's.
  std::array<const DkShader*, 21> pipelines_conversion_edram_{};
  // layout_descriptors_, layout_pipeline_, pool_descriptors_, descriptors_output_, vs_, fs_, render_pass_output_,
  // pipeline_, RampOutput / ramps_output_, fs_ramp_, pipelines_ramp_, pipelines_failed_ramp_: the present region's
  // deko3d output objects (descriptors_source_output_, vs_output_, fs_output_, ramps_output_dk_, shaders_ramp_...).
  // Output with the game's gamma ramp (sa2_native_ramp_gamma).
  bool ramp_gamma_ = false;
  bool output_fxaa_ = false;  // ds2_output_fxaa
  bool logged_output_variant_ = false;
  bool warned_bilinear_ramp_ = false;
  // The game's ramp in 10 bits (red, green, blue). Until the game loads its own, the identity the SDK starts
  // with (i * 1023 / 255).
  std::array<std::array<uint16_t, 3>, 256> ramp_game_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  // The output UBO in std140: one vec4 per entry (red, green and blue in 0-1, with the single-channel
  // post-processing applied), followed by the mix (saturation, vibrance, 1 / gamma) and effects (vignette,
  // scanlines). Filled by RecomputeRamp.
  static constexpr size_t kFloatRamp = 256 * 4 + 8;
  std::array<float, kFloatRamp> ramp_values_{};
  uint64_t version_ramp_ = 0;

  uint64_t swaps_ = 0;    // resolves without a copy
  uint64_t restores_ = 0;
  uint64_t restores_for_resolver_ = 0;  // sa2_native_resolver_valid_content
  uint64_t resolver_no_source_ = 0;           // the content was in no texture
  uint64_t resolver_old_content_ = 0;      // with the setting off, resolves of the stale image
  uint64_t swaps_no_clear_ = 0;  // the ones that used to copy 1600x1600
  // Copies and clears removed, and what they cost.
  std::unordered_map<const Image*, TargetState> target_state_;
  uint64_t cleared_skipped_ = 0;             // colour
  uint64_t cleared_skipped_depth_ = 0;
  uint64_t cleared_skipped_pixels_ = 0;
  uint64_t cleared_useless_ = 0;             // clears wiped out by a swap with no draw in between
  uint64_t cleared_useless_pixels_ = 0;
  uint64_t clipped_restores_ = 0;     // restores that did not copy the whole image
  uint64_t restored_pixels_ = 0;           // the ones actually copied (so the report adds up)
  uint64_t pixels_restore_saved_ = 0;   // the ones not copied, due to useful area or swap
  // sa2_native_lazy_front (see PendingFront).
  std::unordered_map<uint32_t, PendingFront> pending_fronts_;  // per texture address
  std::vector<ImageFront> fronts_images_;                         // spares and retained
  std::unordered_map<uint32_t, uint64_t> presented_fronts_;  // address -> presented_ at its last Swap
  std::unordered_map<uint32_t, uint64_t> read_fronts_;       // address -> last sampling by a draw
  std::unordered_set<uint32_t> expired_fronts_;              // missing a copy that can no longer be made
  bool off_front_ = false;
  uint64_t postponed_front_ = 0;
  uint64_t painted_front_target_ = 0;
  uint64_t painted_retained_front_ = 0;
  uint64_t front_rotations_ = 0;
  uint64_t front_no_replenished_ = 0;
  uint64_t copied_front_read_ = 0;
  uint64_t copied_front_write_ = 0;
  uint64_t copied_front_swap_ = 0;
  uint64_t replaced_front_ = 0;
  uint64_t front_late_reads_ = 0;
  uint64_t front_saved_pixels_ = 0;
  uint64_t front_saved_previous_pixels_ = 0;
  static constexpr uint64_t kFrontFrames = 8;    // window for "the Swap presents it" and "nobody samples it"
  static constexpr size_t kFrontImagesMax = 3;     // spares of the front buffer's target (~4 MB each at 1040)
  // The values above as they were at the previous report: the new lines are per frame.
  uint64_t restored_previous_pixels_ = 0;
  uint64_t pixels_restore_saved_previous_ = 0;
  uint64_t cleared_skipped_previous_pixels_ = 0;
  uint64_t cleared_useless_previous_pixels_ = 0;
  uint64_t presented_report_copies_ = 0;
  // State of the previous frame, for the stutter dump.
  uint64_t hitch_copies_ = 0, cleared_hitch_ = 0, hitch_resolves_ = 0;
  uint64_t hitch_restores_ = 0, hitch_waits_ = 0;
  uint64_t hitch_ns_waits_gpu_ = 0, hitch_ns_wait_output_ = 0;
  uint64_t hitch_bytes_fingerprint_ = 0, hitch_postponed_fingerprints_ = 0;
  uint64_t hitch_ns_textures_ = 0;
  uint64_t hitch_textures_uploads_ = 0, hitch_bytes_uploaded_ = 0, hitch_created_textures_ = 0;
  uint64_t hitch_ns_raw_fingerprint_ = 0, hitch_ns_data_fingerprint_ = 0;
  uint64_t hitch_ns_texture_zero_ = 0, hitch_ns_detile_ = 0;
  uint64_t hitch_ns_waiting_copies_ = 0, hitch_waits_copies_ = 0;
  uint64_t hitch_ns_helping_copies_ = 0, hitch_helped_copies_ = 0;
  uint64_t hitch_ns_create_textures_ = 0, hitch_bound_textures_thread_ = 0;
  uint64_t hitch_ns_waiting_bindings_ = 0;
  uint64_t hitch_gpu_ns_ = 0;      // GPU timestamps when the previous frame closed
  std::array<uint64_t, kGpuCategories> hitch_gpu_categories_{};  // per category, when the previous frame closed
  uint64_t hitch_ns_record_ = 0;   // ns recording when the previous frame closed
  uint32_t warnings_hitch_ = 0;
  uint64_t copies_ = 0;
  uint64_t converted_copies_ = 0;  // resolves with a format conversion (blit)
  std::unordered_set<uint64_t> recorded_conversions_;
  uint64_t cleared_in_failed_pass_ = 0;  // clears through a pass that could not be opened
  uint64_t cleared_ = 0;
  uint64_t presented_ = 0;
  uint32_t changes_resolved_recorded_format_ = 0;  // ME: log of format/size changes at a resolve address
  uint64_t rejections_ = 0;
  std::unordered_set<uint32_t> warned_;
