// sa2_native_targets_dk_edram.inc: members of this region (see the class header). Owned by one translation region.
// Translation of TargetsVulkan (sa2_native_targets.cpp) 6500-9957: the EDRAM alias modes 1-3 (overlaps, exact base,
// pitch copies), the mode-4 transfer passes (compute conversions, depth imports with the 9-pass stencil, depth
// exports, guestspace depth resolves, the fragment color conversions), the resolve passes (bias, sa2_gpu_resolve_draw,
// the sa2_msaa depth resolve), the resolve aliases (sa2_gpu_resolve_alias), the depth targets (GetDepth), the resolved
// textures (GetResolved, park, sleep/wake, retire, reports), SwapWithResolved, RestoreContent and the resolve
// barriers. Bodies in sa2_native_targets_dk_edram.cpp. Declarations keep the Vulkan order and comments.
// Also here (copies region note): the deko3d shape of ViewsDepthEDRAM, PassImportDepthEDRAM and PassConversionColorFrag
// (Vulkan 4432-4446) and the pass maps keyed by them (Vulkan 11477-11481), which only held Vulkan objects.

  // ---- sa2_native_targets.cpp 4432-4446 (deko3d shape) ----
  // deko3d: views are made on the spot (sa2::dk::View). A depth image sampled as depth and as stencil is two
  // DkImageViews of the same image (DkDsSource_Depth / DkDsSource_Stencil); pImage == nullptr = not available.
  struct ViewsDepthEDRAM {
    DkImageView depth{};
    DkImageView stencil{};
  };

  // deko3d: no render pass or pipeline objects. What the 6 pipelines differed in is the fragment shader of each
  // (numeric color / depth / raw64 sources, each initial / stencil-bit) and whether depth is tested and written
  // (even: yes, odd: no); the rest is the fixed state of the pass. `key` is what BatchEDRAM4::import_pass compares
  // (it was the VkRenderPass).
  struct PassImportDepthEDRAM {
    uint64_t key = 0;
    std::array<const DkShader*, 6> pipelines{};  // the fragment shader of each of the 6 Vulkan pipelines
    sa2::dk::PassState state;                    // stencil ALWAYS / REPLACE, depth ALWAYS, the target's samples
    bool valid = false;
  };

  // sa2_native_conversion_frag: render pass + pipeline per destination host format.
  // deko3d: the fragment shader and the fixed state (no blend, RGBA writes, the destination's samples).
  struct PassConversionColorFrag {
    const DkShader* fs = nullptr;
    sa2::dk::PassState state;
  };
  // Vulkan 11477-11481 (passes_import_depth_edram_, passes_conv_color_frag_; the framebuffer maps and
  // views_depth_edram_ / views_raw64_rt_edram_ are gone): the pass caches, same keys.
  std::unordered_map<uint64_t, PassImportDepthEDRAM> passes_import_depth_edram_;
  std::unordered_map<uint64_t, PassConversionColorFrag> passes_conv_color_frag_;  // format | variant << 32

  // ---- sa2_native_targets.cpp 6500-9957 ----
  // Mass Effect report: mode 2/3 EDRAM overlap work since the last report.
  void ReportOverlapsEDRAM();
  uint64_t overlaps_copies_ = 0, overlaps_tiles_ = 0, overlaps_regions_ = 0;
  uint64_t overlaps_conversions_ = 0, overlaps_tiles_converted_ = 0;

  void SynchronizeOverlapsEDRAM(uint32_t base, Image& target, const VkRect2D* area = nullptr);

  // Xenos EDRAM has no image format or surface pitch attached to its tiles. Mass Effect rebinds base 2D0
  // as 1280-wide 7e3, then 640-wide RGBA8, then 1280-wide RGBA8. The old key included the pitch, leaving
  // three unrelated host images and producing horizontal strips made from stale frames. Track the newest
  // representation per base and move the physical 80x16 tiles when either format or pitch changes.
  void ActivateTargetColor(uint32_t base, uint32_t pitch, Image& target);

  bool CopyAliasEDRAMPitch(Image& source, Image& target);

  // ds2-nx (ds2_resolve_7e3_direct): the 7e3 view that owns every tile of `area` of the UNORM10 view `view` with the
  // same tile layout (same base, pitch, sample layout, scale), or null.
  Image* Owner7e3EDRAM4(const Image& view, const VkRect2D& area);
  uint64_t ds2_direct_7e3_resolves_ = 0;
  // ds2_skip_repeated_depth_resolve: the last guestspace depth resolve into each address.
  struct LastDepthResolve {
    sa2::dk::ImageId source = sa2::dk::kNoImage, target = sa2::dk::kNoImage;  // were VkImage
    uint32_t address = 0;
    int32_t x0 = 0, y0 = 0;
    uint32_t dx = 0, dy = 0, width = 0, height = 0, sample = 0;
    uint64_t drawn = 0, transfers = 0, restored = 0;
    uint32_t edram_version = 0;
    uint64_t revision = 0;  // the texture's revision right after it
    bool Same(const LastDepthResolve& o) const {
      return source == o.source && target == o.target && address == o.address && x0 == o.x0 && y0 == o.y0 &&
             dx == o.dx && dy == o.dy && width == o.width && height == o.height && sample == o.sample &&
             drawn == o.drawn && transfers == o.transfers && restored == o.restored &&
             edram_version == o.edram_version;
    }
  };
  std::unordered_map<uint32_t, LastDepthResolve> last_depth_resolves_;
  uint64_t ds2_depth_resolves_skipped_ = 0, ds2_depth_resolves_skipped_report_ = 0;
  bool storage_unorm10_ = false;  // ds2-nx: pipeline 10 exists

  bool ResolverWithBias(Image& source, Image& target, int32_t sx, int32_t sy,
                        uint32_t dx, uint32_t dy, uint32_t width, uint32_t height,
                        int32_t exp_bias, uint32_t pipeline_override = 0);

  // deko3d: the UINT view (RGBA16_Uint over the RGBA16F image), made on the spot; pImage == nullptr if unavailable.
  DkImageView GetViewRaw64EDRAM4(Image& image);

  // deko3d: returned by value (views on the spot); depth.pImage == nullptr if unavailable.
  ViewsDepthEDRAM GetViewsDepthEDRAM4(Image& source);

  static bool IsDepthNative2xEDRAM4(const Image& image);

  static bool IsDepthNative1xEDRAM4(const Image& image);

  bool AcceptsImportDepthNative2xEDRAM4(VkFormat format);
  int8_t native2x_supported_ = -1;
  bool AcceptsImportDepthNative2xNoCacheEDRAM4(VkFormat format);

  PassImportDepthEDRAM* EnsureImportDepthEDRAM4(VkFormat format,
      VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT,
      VkSampleCountFlagBits source_sample_count = VK_SAMPLE_COUNT_1_BIT);

  // ---- Copy-engine stencil import (1x destinations) ---------------------------------------------------
  // deko3d: not available (no per-aspect copy into ZF32_X24S8 / Z24S8: a copy moves whole texels, depth with its
  // stencil). EnsureStencilCopyEDRAM4 answers false, so copy_stencil is never true and every import takes the
  // masked stencil passes. The Vulkan layout, shaders and pipelines (7225-7228) are gone; the buffer stays in the
  // slot (WorkSlot::buffer_stencil_copy, never allocated).
  static constexpr uint64_t kBufferStencilCopy = 2u << 20;  // 1280x1280 bytes fit (1.6 MB)
  bool stencil_failed_copy_ = false;
  uint64_t edram4_stencil_copies_ = 0, edram4_stencil_copies_tiles_ = 0;

  bool EnsureStencilCopyEDRAM4();

  // Writes the stencil of destination tiles [target_start, +count) (1x, TRANSFER_DST) from the source's
  // stencil through the copy engine: compute -> buffer -> vkCmdCopyBufferToImage(STENCIL), one region per
  // tile-row run so tiles outside the range keep their stencil. Depth is untouched.
  // deko3d: never queued (see EnsureStencilCopyEDRAM4); returns false.
  bool CopyStencilEDRAM4(Image& source, Image& target, uint32_t source_start, uint32_t target_start,
                           uint32_t count, const VkRect2D& guest_rect);

  bool ImportColorDepthEDRAM4(Image& source, Image& target,
                                 uint32_t source_start, uint32_t target_start, uint32_t count);

  bool EnsureResolveDepthNative2xEDRAM4();

  bool ResolverDepthGuestspaceEDRAM4(Image& source, Image& target,
      uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height,
      uint32_t raw_sample_select = 0);

  bool ConvertDepthEDRAM4(Image& source, Image& target,
                             uint32_t source_start, uint32_t target_start, uint32_t count);

  // sa2_native_conversion_frag: the raw UINT view of a 64-bit color image as a color attachment.
  // deko3d: the same RGBA16_Uint view as GetViewRaw64EDRAM4 (a view has no usage), bound as the render target.
  DkImageView GetViewRaw64RTEDRAM4(Image& image);

  // sa2_native_conversion_frag. Lazily creates what the fragment version of the color <-> color
  // transfers needs: layouts, shaders and, per destination host format, a LOAD/STORE render pass and its
  // pipeline. nullptr (and the compute path from then on) if anything cannot be created.
  // samples: the destination's host sample count (sa2_msaa: a host multisampled game target; 1 otherwise).
  // deko3d: the shaders and the pass's fixed state.
  PassConversionColorFrag* EnsureConversionColorFrag(VkFormat format, uint32_t variant, uint32_t samples = 1);
  // sa2_gpu_resolve_draw: a same-format RGBA8 color resolve as one fragment pass on the 3D engine (variant 2 of the
  // color fragment passes; shaders/sa2_resolve_copy.frag) instead of vkCmdCopyImage, which NVK records on the copy
  // engine (NV90B5) with a wait for idle at each engine switch. The pass reads source texel (x, y) + delta and writes
  // destination texel (x, y); UNORM8 values round-trip exactly, as in the EDRAM color conversions. The render pass's
  // own dependencies order it like BarrierBeforeCopyResolve / BarrierAfterCopyResolve did. Returns false WITHOUT
  // recording anything when it does not apply: the caller records the copy as before.
  bool ResolveColorFrag(Image& source, Image& target, int32_t sx, int32_t sy, uint32_t dx, uint32_t dy,
                        uint32_t width, uint32_t height);
  // ---- sa2_msaa: depth resolve of a multisampled depth target ---------------------------------------------------------
  // vkCmdResolveImage has no depth: one fragment pass writes sample 0 of each source pixel (the guest's only sample) as
  // the depth of the single-sample resolved texture (shaders/sa2_depth_resolve_ms.frag). Its stencil stays: the game
  // samples resolved depth as k_24_8 depth only.
  // deko3d: the pass is the fixed state (depth ALWAYS, written); the shaders are shared by every format.
  struct PassDepthResolveMs {
    sa2::dk::PassState state;
    bool valid = false;
  };
  std::unordered_map<uint32_t, PassDepthResolveMs> passes_depth_resolve_ms_;  // by host depth format
  // framebuffers_depth_resolve_ms_, layout_depth_resolve_ms_, layout_pipeline_depth_resolve_ms_: gone in deko3d.
  const DkShader* vs_depth_resolve_ms_ = nullptr, *fs_depth_resolve_ms_ = nullptr;
  bool depth_resolve_ms_failed_ = false;

  PassDepthResolveMs* EnsureDepthResolveMs(VkFormat format);

  // Records the resolve of the guest rectangle `copy` (guest units, as for vkCmdCopyImage) of the multisampled depth
  // target `source` into the single-sample texture `target`. false = not recorded.
  bool ResolveDepthMs(Image& source, Image& target, const VkImageCopy& copy);

  /*
   * sa2_gpu_resolve_alias. A color resolve that repeats the previous one into another texture is not copied: that
   * texture (the alias) takes the bytes of the first one (the source), which holds them, later:
   *  - when something reads the alias (a draw, the Swap, a restore): copied then, from the source (MaterializeAlias);
   *  - before the source changes in any way (a resolve or a restore into it, another image at its address): copied
   *    first, or, when the alias and the source are whole textures of the same shape and the coming resolve covers the
   *    whole source, the two images are exchanged (ExchangeAlias): the alias gets the image with the bytes and the
   *    source the other one, which that resolve overwrites whole;
   *  - never, when a resolve into the alias covers it first: nobody could see those bytes.
   * Why it is exact: the previous resolve (MakeAlias's `p`) copied the same rectangle of the same render target image,
   * the command right before this one, with no draw, clear (its own command's included), EDRAM import or restore in
   * between, so the render target still holds the bytes it copied; and its texture still has the revision it left,
   * so it holds them too. Every place that writes a resolved texture calls BeforeResolvedWrite first and every place
   * that reads one materializes it first (ResolvedTexture, ResolvedTextureForFetch, Present, RestoreContent).
   * GUARD: an alias whose source is not what it was (image or revision) cannot get its bytes: DIFFERENCE in the log
   * and the enhancement turns off for the session. Checked at every materialization and every drop.
   */
  struct AliasResolve {
    uint32_t source = 0;  // the texture that holds the bytes
    sa2::dk::ImageId source_image = sa2::dk::kNoImage;  // was VkImage
    uint64_t source_revision = 0;
    sa2::dk::ImageId image = sa2::dk::kNoImage;  // the alias's image when it was made (was VkImage)
    int32_t sx = 0, sy = 0;          // the rectangle in the source
    uint32_t dx = 0, dy = 0, width = 0, height = 0;  // and in the alias
  };
  struct LastResolve {  // the color resolve recorded for real by the previous command
    bool valid = false;
    sa2::dk::ImageId target = sa2::dk::kNoImage;  // the render target image it read (was VkImage)
    int32_t x0 = 0, y0 = 0;
    uint32_t width = 0, height = 0;
    uint32_t address = 0;  // and the texture it wrote, at (dx, dy)
    sa2::dk::ImageId image = sa2::dk::kNoImage;  // was VkImage
    uint64_t revision = 0;
    uint32_t dx = 0, dy = 0;
    uint64_t drawn = 0, transfers = 0, restored = 0;  // draws, EDRAM imports and restores so far
    // EDRAM mode 4's tile ownership version (edram4_version_): every write of a render target publishes its tiles,
    // the draws and also what Drawn() does not count, Direct3D's Clear draws redirected to EDRAM clears (SA2 clears
    // its frame between the two resolves of a fade: an alias made then showed the frame from before the clear).
    uint64_t edram_version = 0;
  };

  bool MakeAlias(const LastResolve& p, const Image& target, Resolved& resolved, uint32_t address, int32_t x0,
                 int32_t y0, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height);

  bool DiagAlias() const;

  // The source of `alias` still holds the bytes the alias was made with (GUARD).
  bool AliasSourceIntact(const AliasResolve& alias);

  void AliasFailure(uint32_t address, const char* why);

  void DropAlias(uint32_t address);

  // The alias at `address` gets its bytes now: copied from its source (on the 3D engine when it applies), outside
  // any pass. false if the source no longer holds them (GUARD).
  bool MaterializeAlias(uint32_t address);

  // The source of the alias at `alias_address` is about to be overwritten whole (rectangle dx, dy, width, height):
  // the alias takes the source's image, which holds its bytes, and the source the alias's image, which that write
  // covers. Only whole textures of the same shape, nothing else holding either image. false: not done (copy it).
  bool ExchangeAlias(uint32_t alias_address, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height);

  // sa2_gpu_resolve_alias: the resolved texture at `address` is about to receive new bytes, in the rectangle
  // (dx, dy, width, height) when the caller is sure to write it (width 0: unknown). The aliases reading from it get
  // their bytes first (an exchange when the write covers it whole, otherwise a copy). Its own alias is dropped when
  // the write covers it (nobody could see those bytes), otherwise copied first.
  void BeforeResolvedWrite(uint32_t address, uint32_t dx = 0, uint32_t dy = 0, uint32_t width = 0,
                           uint32_t height = 0);

  // The image at `address` is about to be parked or destroyed: its readers and its own alias get their bytes first.
  void AliasesBeforeImageGoes(uint32_t address);

  // The copy inventory (NoteCopy) counts the pixels each texture really receives.
  void ResolvedPixelsCopied(uint32_t base, uint64_t pixels);

  // sa2_native_conversion_frag: one run of tiles of a 32-bit color view into another one as a fragment
  // pass (see src/native/native/shaders/me_edram_color_to_color.frag). Returns false WITHOUT recording anything when it does not
  // apply (the caller then records the compute shader).
  bool ConvertAliasColorFragEDRAM4(Image& source, Image& target, uint32_t source_start,
                                     uint32_t target_start, uint32_t count);

  bool ConvertAliasEDRAM(Image& source, Image& target,
                           uint32_t tile_source_start = 0,
                           uint32_t tile_target_start = 0,
                           uint32_t tiles_count = 0);

  Image* GetDepth(uint32_t base, uint32_t format, uint32_t pitch);

  Image* GetDepthLookup(uint32_t base, uint32_t format, uint32_t pitch);

  // Predicated tiling may resolve one logical texture as multiple rectangles. For the later rectangles,
  // D3D9 advances RB_COPY_DEST_BASE by the tiled byte offset and sends local (0,0)-based vertices again.
  // Such a base is not a new texture: locate the already-created parent image and turn the physical base
  // delta back into a destination x/y. This is format/geometry based and applies equally to color and depth.
  // sa2_native_partial_resolve_inside: such a rectangle belongs to the texture its command declares (`segment_width` x
  // `declared_height`, the destination's own size) and lies inside it: `rect_width` columns from its x offset, as its
  // rows from its y offset. A resolve that fails either is a texture of its own whose memory merely starts inside a
  // resolved texture's (the game freed that one and allocated this one a few pages higher): it gets its own image.
  Resolved* SearchResolvedPartial(uint32_t base_segment, uint32_t segment_width,
                                  uint32_t segment_height, uint32_t guest_format,
                                  bool swap_rb, VkFormat host_format, uint32_t log2_texel,
                                  uint32_t& base_resolved, uint32_t& dx, uint32_t& dy,
                                  uint32_t rect_width, uint32_t declared_height);

  // sa2_diag_stale_resolved = "BASE:PAGES[:HEIGHT]". Called with every color resolve's destination before it is looked
  // up: ahead of the first resolve to BASE, the resolved texture an earlier resolve PAGES pages below it would have
  // left is created, through the same GetResolved a real one goes through. Nothing else is touched.
  void DiagStaleResolved(uint32_t base, uint32_t width, uint32_t height, uint32_t guest_format, bool swap_rb,
                         VkFormat host_format);

  // sa2_native_resolved_retire_idle. Releases the resolved textures that nothing resolved to or read for `idle`
  // presented frames, and returns the bytes released. It needs no wait for the GPU, for the reason the texture
  // cache's eviction needs none: no pending submission can reference an image unused for that many frames, and
  // ForgetImage gives its descriptor slots back. A texture with deferred work attached stays: an alias in either
  // direction (sa2_gpu_resolve_alias), a deferred front copy (sa2_native_lazy_front), an EDRAM tile owner.
  uint64_t RetireIdleResolved(uint64_t idle);

  // PC build: a tiled fetch at an address whose resolved texture was released and that has not been resolved to
  // since. Either the game reuses the memory for an ordinary texture (what the release is for) or it reads a render
  // target it filled long ago: the log line names the address so the two can be told apart.
  void NoteFetchAfterRetirement(uint32_t address);

  // Every 600 presented frames: how many resolved textures there are and what they hold (with the key off this is
  // the growth of a long session), what was released, and the oldest read of one after the resolve that filled it.
  void ReportResolved();

  void ResolvedReportPool(bool detail = false);

  bool CanSleepResolved(const Image& image);

  bool ResolvedSleepingFinished(const ResolvedSleeping& sleeping);

  void EvictResolvedSleeping(size_t index);

  bool SleepResolved(uint32_t base, Image& image);

  bool WakeResolved(uint32_t base, uint32_t width, uint32_t height, VkFormat format,
                        Image& image);

  Resolved* GetResolved(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                            bool swap_rb, VkFormat host_format = kColorFormat);

  bool ResolverNoCopy() { return REXCVAR_GET(sa2_native_resolver_no_copy); }

  static bool IsDepthFormat(VkFormat format);

  // Resolve without copying. The whole render target becomes the resolved texture and that texture's
  // old image stays as the render target. What the game sees is the same; what is saved is moving the
  // pixels (the two 1600x1600 shadow map copies are 41 MB per frame). false if it is not possible.
  bool SwapWithResolved(Image& target, Resolved& resolved, uint32_t base);

  // If the render target lost its content in an image swap and the game is about to draw on top, the
  // content is brought back from the resolved texture.
  //
  // It happens once for every swap without a clear. Restores are counted and trimmed to the useful area.
  //
  // for_resolver = requested by a resolve (sa2_native_resolver_valid_content), not by a pass that
  // will draw on top. Then it is only copied.
  bool RestoreContent(Image& target, bool for_resolver = false);

  // Every ordinary resolve uses GENERAL for both images, so there is no layout transition to provide a
  // memory dependency. Queue order alone is not a cache flush: publish attachment writes before the copy,
  // then publish the resolved texture before a shader samples it (or another transfer touches it).
  // deko3d: Hazard::RenderToCopy before, Hazard::CopyToAny after.
  void BarrierBeforeCopyResolve();

  void BarrierAfterCopyResolve();

  /*
   * sa2_native_resolver_valid_content. The render target gets its content back from the resolved
   * texture where the image swap left it, just before a resolve reads it.
   *
   * The copy goes between two barriers, and here they are needed: the source is the image that was just
   * drawn as the depth target (the previous resolve moved it into the texture by swapping), and what is
   * copied will be sampled as soon as the current resolve swaps it. NVK only waits for the GPU and
   * flushes the texture cache at a barrier (the normal restore still goes without them). Each one costs a
   * pipeline drain. The whole target is copied: that is what the EDRAM held and what the resolve reads.
   */
  bool CopyOfLapForResolver(Image& target, Image& source);
