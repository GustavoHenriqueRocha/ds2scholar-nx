// sa2_native_targets_dk_copias.inc: members of this region (see the class header). Owned by one translation region.
// Translation of TargetsVulkan (sa2_native_targets.cpp) 1752-2612 (Copy, ClearDepth, CopyDepth) and 4064-6497
// (Reject, Rectangle, GetTarget and the EDRAM mode-4 bookkeeping: ownership, clear plans, SynchronizeEDRAM4, the
// transfer batch, CopyTilesEDRAM4, redirected clears, lazy stencil, the PrepareDrawEDRAM4 memo). Bodies in
// sa2_native_targets_dk_copias.cpp. Declarations keep the Vulkan order and comments.
  // ---- sa2_native_targets.cpp 1752-2612 ----
  bool Copy(const RegistersCopy& reg) override { return CopyInternal(reg); }

  bool CopyInternal(const RegistersCopy& reg);

  void ClearDepth(const RegistersCopy& reg, uint32_t pitch);

  // Depth copy: the rectangle of the depth render target to a resolved texture
  // of the same host format, at the base GetResolveInfo computes
  // (4 bytes per texel, like k_24_8).
  // will_clear: this same game command clears the render target right after resolving it. Only then is it worth
  // swapping the images: if the game kept drawing on top, the content would have to be brought back and the
  // copy would be paid anyway (measured: one restore per frame on the shadow map).
  void CopyDepth(const RegistersCopy& reg, uint32_t pitch, bool will_clear);
  // ---- sa2_native_targets.cpp 4064-6497 ----
  bool Reject(uint32_t cause, const char* text);

  // Rectangle covered by the copy, as in the SDK's GetResolveInfo.
  bool Rectangle(const RegistersCopy& reg, uint32_t pitch, int32_t& x0, int32_t& y0,
                  int32_t& x1, int32_t& y1);

  // sa2_cpu_ring_target_memo: GetTarget / GetDepth answered from a 4-slot memo of their inputs. Both maps only ever
  // gain entries (no erase, no clear while the targets live) and an unordered_map node never moves, so the same
  // inputs always lead to the same Image. Only found or created images are memoized; rejections run the full path.
  struct TargetMemo {
    uint64_t in = ~0ull;
    Image* image = nullptr;
  };
  std::array<TargetMemo, 4> target_memo_{}, depth_memo_{};

  Image* GetTarget(uint32_t base, uint32_t format, uint32_t pitch);

  Image* GetTargetLookup(uint32_t base, uint32_t format, uint32_t pitch);

  struct OwnerTileEDRAM {
    Image* image = nullptr;
    uint16_t tile_local = 0;
    // content version of this physical tile (bumped whenever its owner writes it).
    uint32_t version = 0;
  };

  // Mass Effect mode-4 sync cache. On the Switch ~150 depth imports per frame (9-pass stencil path)
  // re-copied tiles a read-only view had already received: ownership only moves on a write, so every
  // read-only rebind imported the same unchanged tiles again. Each view remembers the version of each
  // physical tile it last received; an unchanged tile is not transferred again.
  uint32_t edram4_version_ = 0;
  // why each mode-4 transfer happens (report). Caller: 0 draw, 1 clear, 2 color resolve,
  // 3 depth resolve. Kind: 0 depth->depth X-grid/collapsed switch, 1 depth->depth other, 2 color->depth,
  // 3 depth->color, 4 color->color. Full = the run covers the whole target.
  uint32_t edram4_reason_ = 0;
  uint64_t edram4_overwrites_ = 0;  // draw binds that used a proven full overwrite
  uint64_t edram4_redirected_ = 0, edram4_redirected_tiles_ = 0, edram4_redirected_no_ = 0;
  std::map<std::string, uint64_t> edram4_stencil_no_reasons_, edram4_depth_no_reasons_;
  // sa2_cpu_ring_redirect_key: RedirectClearDepthEDRAM4's no() counts by (reason literal, depth info).
  std::map<std::pair<const char*, uint32_t>, uint64_t> edram4_depth_no_fast_;
  uint64_t edram4_redirected_color_tiles_ = 0;
  // Tiles a redirected clear wrote (by the view the game drew it on) and the view that next pulled them.
  // A redirected clear is identified by its view and its ordinal among that view's clears in the frame (the
  // same view is cleared several times per frame, each time read next by a different view).
  struct RedirectedStamp { uint64_t site = 0; uint64_t version = 0; };
  std::unordered_map<const Image*, uint32_t> edram4_cleared_in_frame_;
  static uint64_t SiteClear(const Image* drawn, uint32_t ordinal) {
    return (uint64_t(reinterpret_cast<uintptr_t>(drawn)) << 8) ^ ordinal;
  }
  std::array<RedirectedStamp, 2048> edram4_stamps_{};
  struct Consumer { Image* view = nullptr; uint32_t confidence = 0; };
  std::unordered_map<uint64_t, Consumer> edram4_consumer_;
  uint64_t edram4_consumer_uses_ = 0;
  std::array<uint64_t, kGpuCategories> cat_intervals_{}, cat_max_ns_{}, cat_long_{};
  std::unordered_map<uint64_t, uint64_t> gpu_labels_ns_;
  int32_t dump_remaining_marks_ = 0;
  bool dump_done_marks_ = false;
  std::chrono::steady_clock::time_point dump_start_ = std::chrono::steady_clock::now();  // (category, first VS/PS) -> GPU ns
  uint64_t edram4_stencil_redirected_ = 0, edram4_stencil_redirected_tiles_ = 0, edram4_stencil_redirected_no_ = 0;
  uint64_t exhausted_marks_ = 0;
  int8_t d32s8_supported_ = -1;  // cached format support of the half-range depth host format
  uint64_t mark_max_ns_ = 0;      // longest single mark-to-mark interval since the last report
  std::string mark_max_desc_;
  uint64_t edram4_epoch_ = 1;  // bumped whenever any physical tile changes owner (fast paths above)
  uint64_t edram4_sync_fast_ = 0, edram4_pub_fast_ = 0;
  static bool ContainsRect(const VkRect2D& a, const VkRect2D& b);
  // CPU cost of the mode-4 bookkeeping (report): calls and tiles visited by sync and publish.
  uint64_t edram4_sync_calls_ = 0, edram4_sync_visited_ = 0, edram4_pub_calls_ = 0, edram4_pub_visited_ = 0;  // GPU category marks dropped because a slot ran out (report)
  std::string edram4_draw_desc_;      // the draw being synchronized (transfer trace)
  uint32_t edram4_traces_ = 0;
  uint32_t banjo_plan_failures_ = 0;  // banjo-nx: invalid EDRAM4 plans logged
  uint64_t banjo_transfers_skipped_ = 0;  // banjo-nx: unsupported EDRAM4 transfers skipped
  std::array<std::array<uint64_t, 5>, 4> edram4_transfer_ops_{}, edram4_transfer_tiles_{};
  std::array<uint64_t, 5> edram4_transfer_full_{};
  struct ParEDRAM4 {
    uint64_t ops = 0, tiles = 0;
    std::string example;
    std::map<std::string, std::pair<uint64_t, uint64_t>> draws;  // short draw key -> ops, tiles
  };
  std::map<std::string, ParEDRAM4> edram4_transfer_pairs_;  // "src->dst" -> ops, tiles, first draw
  const SubmissionDraw* edram4_submission_ = nullptr;  // the draw being synchronized (nullptr: not a draw)
  uint32_t edram4_slot_ = 0;
  std::string DescribeDrawEDRAM4() const;
  // The draw being synchronized, for the transfer trace (from edram4_submission_ / edram4_slot_ / edram4_draw_area_).
  std::string TraceDrawEDRAM4() const;
  std::unordered_map<const Image*, std::vector<uint32_t>> edram4_views_;
  uint64_t edram4_sync_skipped_ = 0, edram4_sync_copied_ = 0;
  bool SynchronizedEDRAM4(const Image& view, uint32_t physical, uint32_t version) const;
  void NoteSynchronizedEDRAM4(const Image& view, uint32_t physical, uint32_t version);
  void ForgetSynchronizedEDRAM4(const Image& view);
  // A 4x MSAA depth view -> the 1x depth view with the same EDRAM base and tile pitch that last took its
  // tiles (the same bytes seen per sample: Eden Prime's 440x720 4x clear alias of the 880x880 shadow map).
  std::unordered_map<const Image*, Image*> edram4_alias_1x_;
  // ds2-nx (ds2_clear_color0_alias): a 4x RGBA8 color view -> the 1x RGBA8 view with the same base and tile pitch
  // that last took its tiles (Dark Souls II's 640x360 4x D3D Clear alias of its 1280x720 8888 target, EDRAM 000).
  std::unordered_map<const Image*, Image*> edram4_alias_1x_color_;
  // One sync's transfers recorded as a batch (sa2_native_edram4_batch): their destination tiles are
  // disjoint and sources are only read, so they need one barrier pair / one import pass, not one each.
  struct SpanEDRAM4 {
    Image* source;
    uint32_t tile_source, tile, count;
    bool native_msaa_import;
  };
  struct CopyStencilPending {
    Image* source;
    Image* target;
    uint32_t source_start, target_start, count;
    VkRect2D scissor;
  };
  struct BatchEDRAM4 {
    bool active = false;
    bool compute_pending = false;  // an "after" barrier of compute transfers is owed
    bool import_open = false;
    // deko3d: no render pass / framebuffer objects. The open import is keyed by what ImportColorDepthEDRAM4 binds:
    // its pass key (host depth format, as PassImportDepthEDRAM was per format) and the depth image it renders into.
    uint64_t import_pass = 0;                         // Vulkan: VkRenderPass import_pass
    sa2::dk::ImageId import_fb = sa2::dk::kNoImage;   // Vulkan: VkFramebuffer import_fb
    bool has_area = false;
    VkRect2D area_import{};
    std::vector<CopyStencilPending> copies_stencil;
  } edram4_batch_;
  uint64_t edram4_batch_spans_ = 0, edram4_batch_saved_passes_ = 0, edram4_batch_saved_barriers_ = 0;
  // Lazy stencil import (no VK_EXT_shader_stencil_export on the Switch: stencil costs 8 extra passes). A depth
  // view that never had a stencil-enabled draw receives depth only; per tile it records where the real
  // stencil stayed (image, tile). Importing back into that image/tile keeps the destination's own stencil
  // (exactly what the Xbox does: a depth-only pass leaves the stencil bits of the EDRAM words untouched).
  struct SourceStencil { const Image* image = nullptr; uint16_t tile = 0; };
  std::unordered_map<const Image*, std::vector<SourceStencil>> edram4_stencil_source_;
  // Every write of a deferred stencil source goes through here, so sa2_cpu_ring_edram_memo sees new pending work.
  uint64_t edram4_stencil_sources_serial_ = 0;
  std::vector<SourceStencil>& StencilSourcesWrite(const Image* view) {
    ++edram4_stencil_sources_serial_;
    return edram4_stencil_source_[view];
  }

  // sa2_cpu_ring_edram_memo: the previous draw's PrepareDrawEDRAM4 inputs and outputs.
  struct EdramDrawMemo {
    bool valid = false;
    std::array<uint32_t, 14> key{};    // the registers PrepareDrawEDRAM4 reads (EdramMemoKey)
    bool skipped_mode = false;         // RB_MODECONTROL not 4/5: the full path returns before any EDRAM work
    bool ps_null = true;
    uint32_t ps_outputs = 0;           // p.ps->outputs & 15: which color slots the pixel shader writes
    int64_t used_height = -1;          // p.edram_used_height_estimate, -1 = none
    uint64_t stencil_serial = 0;       // edram4_stencil_sources_serial_ after the full path ran
    std::array<Image*, 5> images{};
    std::array<bool, 5> writes{};
    bool writes_stencil = false;
    bool depth_raster_grid_eligible = false;
    VkRect2D area{};
    me::native::EdramBoundPlan plan{};
    uint32_t last_slot = 0;            // edram4_slot_ after the full path (the last active slot)
  };
  EdramDrawMemo edram4_memo_;
  bool edram4_memo_on_ = true;         // false for the session after a DIFFERENCE
  uint32_t edram4_memo_checks_left_ = 4096;  // the first hits also run the full path and compare
  uint64_t edram4_memo_hits_ = 0, edram4_memo_misses_ = 0, edram4_memo_ineligible_ = 0, edram4_memo_checked_ = 0;
  bool edram4_published_no_stencil_ = false;
  std::vector<VkRect2D> edram4_cuts_extra_;  // more proven-overwrite rects of the current draw (raster)
  bool edram4_import_stencil_only_ = false;  // ImportColorDepthEDRAM4: fetch only the deferred stencil
  uint64_t edram4_stencil_fetched_ = 0, edram4_stencil_fetched_tiles_ = 0;
  uint64_t edram4_bits_skipped_ = 0;
  std::optional<VkRect2D> edram4_stencil_replacement_;  // raster rect whose stencil the current draw replaces
  uint64_t edram4_stencil_replaced_ = 0;
  std::map<std::string, std::pair<uint64_t, uint64_t>> edram4_import9_pairs_;  // 9-pass imports: ops, tiles
  bool edram4_draw_writes_stencil_ = false;  // the draw being prepared writes stencil (slot 0)  // the depth publish in progress leaves the stencil bits alone
  uint64_t edram4_stencil_inherited_ = 0;
  uint64_t edram4_stencil_skipped_ = 0, edram4_stencil_returned_ = 0, edram4_stencil_inexact_ = 0,
           edram4_stencil_late_ = 0, edram4_stencil_exported_ = 0;
  bool StencilDeferredEDRAM4(const Image& view, uint32_t start, uint32_t count) const;
  void StencilRealEDRAM4(const Image& view, uint32_t start, uint32_t count);

  // ViewsDepthEDRAM, PassImportDepthEDRAM and PassConversionColorFrag (Vulkan 4432-4446) hold only Vulkan objects
  // (image views, render passes, pipelines) of the import and conversion passes, all used in 6500-9957. Their
  // deko3d shape (views made on demand, shaders and pass keys) belongs to sa2_native_targets_dk_edram.inc.

  bool FailureEDRAM4(const char* reason, const Image& target, const Image* source = nullptr);

  bool TileUsedEDRAM4(const Image& image, uint32_t tile, const VkRect2D& area,
                       bool* covered_whole = nullptr) const;

  // The tile rectangle TileUsedEDRAM4 accepts for `area` (columns tx0..tx1, rows ty0..ty1, exclusive):
  // the mode-4 loops visit only these instead of testing every tile of the view (CPU cost per draw on
  // the Switch's A57: the full scan was most of the ring thread).
  struct RangeTiles4 { uint32_t pitch = 0, tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0; };
  static bool RangeTilesEDRAM4(const Image& image, const VkRect2D& area, RangeTiles4& r);

  bool ValidateSimultaneousClearEDRAM4(const RegistersCopy& reg);

  bool GetPlanClearedEDRAM4(const RegistersCopy& reg, me::native::EdramClearPlan& plan,
                                 VkRect2D& area);

  bool PrepareRangeClearEDRAM4(Image& image, me::native::EdramRelativeSpan span,
      const VkRect2D& cutout, me::native::EdramClearRectangles& rectangles);

  void PublishRangeClearEDRAM4(Image& image, me::native::EdramRelativeSpan span);

  void BarrierClearEDRAM4();

  // Called several times per draw. With no tile selected (the normal case) every path below returns UINT32_MAX, so once
  // the first call has found that out, the rest return without the function-static guard, the cvar string or the clock.
  uint32_t TraceTileEDRAM4() const {
    if (trace_tile_none_ && trace_tile_fast_) return UINT32_MAX;
    return TraceTileEDRAM4Slow();
  }
  const bool trace_tile_fast_ = CompareActiveFast();
  uint64_t trace_frame_ = 0;  // Swap interval ordinal, for the EDRAM tile trace
  mutable bool trace_tile_none_ = false;

  uint32_t TraceTileEDRAM4Slow() const;

  bool TraceEventEDRAM4(uint32_t tile) const;

  std::string TraceImageEDRAM4(const Image* image, uint32_t local) const;

  bool DrawTouchesTraceTileEDRAM4() const;

  // keep_version: the view only received the owner's current contents (a load on bind, no write).
  // The tile keeps its version, and the previous owner is recorded as still holding it, so binding the
  // previous owner again does not import the same unchanged tiles back (Mass Effect: depth exported to a
  // k_16_16 color view for reading, then a full 9-pass import back every frame).
  void PublishEDRAM4(Image& image, const VkRect2D& area, uint32_t limit = 2048,
                      bool keep_version = false);

  // A fully proven constant stencil clear must not make its collapsed depth
  // copy authoritative. Clear just S8 in each existing canonical depth owner,
  // preserving every D32 bit and its physical ownership. This deliberately
  // does NOT handle conditional/partial stencil draws or color-word owners.
  bool PublishStencilClearEDRAM4(Image& drawn,
      const me::native::StencilClearOwnershipProof& proof, uint32_t limit);

  bool SynchronizeEDRAM4(Image& target, const VkRect2D& area, uint32_t limit = 2048,
                         bool clear_overwrite = false, uint32_t start = 0,
                         const VkRect2D* clear_cutout = nullptr);

  // Destination rectangle of a depth import of tiles [target_start, +count).
  bool RectImportEDRAM4(const Image& target, uint32_t target_start, uint32_t count, VkRect2D& rect) const;

  // Batched compute transfers: the "after" barrier of the last one (a superset of both kinds' masks).
  void CloseComputeBatchEDRAM4();

  // Batched depth imports: end the shared pass, then the copy-engine stencil copies of its runs.
  bool CloseImportBatchEDRAM4();

  bool CloseBatchEDRAM4() {
    CloseComputeBatchEDRAM4();
    return CloseImportBatchEDRAM4();
  }

  bool CopyTilesEDRAM4(Image& source, Image& target, uint32_t source_start,
                         uint32_t target_start, uint32_t count);

  // Mass Effect (Eden Prime point-light shadows): the game clears an 880x880 1x depth shadow map with a
  // D3D Clear rectangle through a 440-pixel 4x MSAA view of the same EDRAM (a quarter of the pixels to
  // shade). Drawn literally, every clear transfers the tiles 1x -> 4x and the next shadow draw 4x -> 1x
  // (9 passes each on Switch without stencil export). A proven, tile-aligned, constant-depth rectangle
  // that touches only depth (ALWAYS, stencil off) is instead applied as a depth-only clear in each tile's
  // current owner; ownership stays with the owner (new version), the drawn view is not touched.
  // ReXGlue xenos Float32To20e4 without rounding (me_edram_depth_a_*.comp A20e4).
  static uint32_t WordFloat24EDRAM4(float z);
  // me_edram_depth_to_16f.comp From7e3.
  static float From7e3EDRAM4(uint32_t f10);

  // Partial tile of a redirected clear: [px0,px1) x [py0,py1) of tile `local` of `drawn` (its pixels), mapped
  // into the tile's current owner. Same-geometry depth owners (and the drawn view itself) take a sub-rect;
  // a color owner takes the words the depth layout puts there (the 40-word half swap of the export shaders),
  // only when they cover whole owner pixels.
  // cross (ds2-nx, ds2_clear_depth_cross): a depth owner of the other encoding can take the clear.
  template <typename ColorFor, typename Region>
  bool PartialEDRAM4(Image& drawn, uint32_t local, uint32_t px0, uint32_t px1, uint32_t py0, uint32_t py1,
                     const ColorFor& color_for, Region& region, bool cross = false);


  // One run of tiles RedirectClearDepthEDRAM4 clears in one owner.
  struct RedirectRegionEDRAM4 {
    Image* owner;
    uint32_t first_local, owner_local, count;
    VkRect2D area;
    bool reassign;
    VkRect2D area2{};  // a partial tile of a color owner can map to two rects (depth/color half-tile swap)
    bool partial = false;
  };

  bool RedirectClearDepthEDRAM4(const SubmissionDraw& p);

  // sa2-nx: the color half of a depth + color 0 clear drawn through a 4x alias (Direct3D's Clear: SA2 clears its 1x
  // 1280x720 back buffers, and the 560-wide split-screen ones, through 4x aliases of the same tiles every frame).
  // Drawn literally, the alias gets the clear and every tile then moves 4x -> 1x (ConvertAliasColorFragEDRAM4).
  // Instead each tile's current owner, a 1x RGBA8 view, gets the clear word directly: a tile is 80x16 samples
  // whatever the view, the clear writes the same word to all of them, and the transfer copies words verbatim, so
  // the owner ends up with exactly the bytes it would have received. Owners other than RGBA8 color views (or the
  // drawn alias itself, on the first frame) decline: the draw then goes the normal way.
  struct ColorClearEDRAM4 {
    struct Run {
      Image* owner;
      uint32_t first_local, count;
      VkRect2D area;
      bool reassign = false;  // ds2-nx (ds2_clear_color0_alias): the owner becomes this view, at the same local tile
      bool partial = false;   // ds2-nx (ds2_clear_color0_partial): area covers part of the tiles (synchronized first)
    };
    std::vector<Run> runs;
    VkClearColorValue value{};
    Image* drawn = nullptr;
  };
  bool PlanClearColor0EDRAM4(const SubmissionDraw& p, ColorClearEDRAM4& plan);
  bool CommitClearColor0EDRAM4(const ColorClearEDRAM4& plan);
  uint64_t edram4_redirected_color0_tiles_ = 0, edram4_redirected_color0_areas_ = 0;
  uint64_t ds2_color0_alias_tiles_ = 0;  // ds2-nx (ds2_clear_color0_alias): of those, handed to the 1x alias
  uint64_t edram4_redirected_merged_ = 0;  // clears the redirected depth clears issued after merging their regions
  // RedirectClearDepthEDRAM4's region list, kept between calls with ds2_redirect_regions_reuse.
  std::vector<RedirectRegionEDRAM4> redirect_regions_;
  bool redirect_regions_busy_ = false;

  // Mass Effect (every frame, main scene depth D000): a full-screen stencil-only D3D Clear (stencil ALWAYS /
  // REPLACE or ZERO, write mask 0xFF, depth test off) drawn through the 4x alias of the 1x depth buffer. Drawn
  // literally it moves all depth 1x -> 4x and back (two full transfers, one with 9 stencil passes). The
  // stencil of each tile is instead set in its current depth owner; depth stays, ownership stays.
  bool RedirectClearStencilEDRAM4(const SubmissionDraw& p);

  // Lazy stencil, second half: before a draw (or an export) needs the real stencil of `view`'s tiles in
  // [start, start + count), copy it from where it stayed (8 bit passes after a stencil-only clear;
  // depth untouched). Runs are grouped by source image and contiguous source tiles.
  bool FetchStencilDeferredEDRAM4(Image& view, uint32_t start, uint32_t count);

  // sa2_cpu_ring_edram_memo. Eligible draws: no per-draw proven rectangle, stencil replace rect or stencil clear proof
  // (they make the area or the stencil work specific to one draw). The key is every register PrepareDrawEDRAM4 reads.
  bool EdramMemoKey(const SubmissionDraw& p, std::array<uint32_t, 14>& key) const;

  // What the full path checks before each of its fast paths, for every slot the memo holds: the image prepared with
  // valid content (Prepare / RestoreContent return at once), the draw's raster area owned in the current epoch
  // (SynchronizeEDRAM4 returns at once), stencil already marked used. No deferred stencil source was written since
  // (stencil_serial, checked by the caller), so FetchStencilDeferredEDRAM4 has nothing to fetch. GetTarget/GetDepth
  // give the same images for the same key: their maps never drop an entry.
  bool EdramMemoStillFast(const EdramDrawMemo& m) const;

  // The state PrepareDrawEDRAM4 leaves, as the full path would leave it for this draw.
  void EdramMemoApply(const EdramDrawMemo& m);

  static bool SameRect(const VkRect2D& a, const VkRect2D& b) {
    return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
           a.extent.height == b.extent.height;
  }
  static bool SamePlan(const me::native::EdramBoundPlan& a, const me::native::EdramBoundPlan& b);

  // A memo hit checked against the full path: run it, then require the same outputs and no work done by it (no
  // synchronization past its fast path, no deferred stencil fetched, no ownership change).
  bool EdramMemoCheck(const SubmissionDraw& p, const EdramDrawMemo& m);

  bool PrepareDrawEDRAM4Memo(const SubmissionDraw& p);

  bool PrepareDrawEDRAM4(const SubmissionDraw& p);

  // Draw/scissor requests are guest pixel rectangles. Ownership and transfer
  // shaders operate on actual host raster texels, including retained X samples.
  static VkRect2D RasterArea(const Image& image, VkRect2D area);

  static uint32_t PitchTilesEDRAM(const Image& image) {
    return me::native::EdramLayout(image.edram_msaa_x ? 2 : image.edram_msaa_y,
                                   image.edram_64bpp).pitch_tiles(image.width);
  }

  // Synchronize aliases by physical EDRAM tile, not merely by equal base. Mass Effect has overlapping
  // targets at different bases (notably 0x2D0 and 0x400), so exact-base aliasing alone leaves stale bands.
  // Merges VkImageCopy regions that tile a larger rectangle in both images: first runs along a row
  // (adjacent in source and destination, same rows), then stacks of equal runs. Same pixels copied.
  static void MergeRegionsCopy(std::vector<VkImageCopy>& regions);
