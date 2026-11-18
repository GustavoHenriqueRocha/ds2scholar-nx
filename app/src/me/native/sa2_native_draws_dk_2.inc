// sa2_native_draws_dk_2.inc: members of the deko3d draws class (DrawsDkImpl, sa2_native_draws_dk.h) owned by
// region 06, the translation of lines 8158-14743 of sa2_native_draws.cpp: the descriptors, the textures, the
// vertex input, BeginPass, the shader variants, the upload buffers, the pipelines and their list, and every data
// member of the class (lines 13741-14743 of the Vulkan file declare all of them). Bodies:
// sa2_native_draws_dk_2.cpp. Names are the Vulkan ones; what changed type or went away is marked [dk].

// ---- Methods (bodies in sa2_native_draws_dk_2.cpp, in the order of the Vulkan file) -------------------------
  // ---- part A ----------------------------------------------------------------------------------------------------
  // ---- Part A: lines 8158-9333 of the Vulkan file (audits, upload buffers, the pipelines file, descriptors) ----
  static bool ResolveFetchAuditEnabled();
  void AuditFetchResolved(uint32_t base, const uint32_t* fetch, const ImageNativeDk& image);
  static bool PayloadAuditEnabled();
  bool StartPayloadAudit(uint32_t kind, uint64_t bytes);
  void ReportPayloadAudit(const char* reason);
  void FinalizePayloadAudit(uint32_t kind, uint64_t address, uint64_t bytes, uint64_t before, uint64_t after,
                            uint64_t staged);
  // sa2_cpu_ring_compare_uploaded. memcmp(last, current) == 0, where last is the uploaded block (from_upload) or the
  // CPU-side copy. Verify mode keeps the copy as well and checks the uploaded block equals it; a difference turns the
  // enhancement off for the session (the copies are current in verify mode, so the old path takes over at once).
  bool SameAsUploaded(const uint8_t* last, const uint8_t* copy, const uint32_t* current, uint32_t bytes,
                      bool from_upload);
  bool SameSharedUploaded(const uint8_t* uploaded, const uint32_t* shared, size_t bytes);
  void CheckUploadedCopy(const uint8_t* uploaded, const uint8_t* copy, uint32_t bytes, const char* what);
  bool compare_uploaded_failed_ = false;
  void AuditCacheConstants(bool vs, uint32_t shader, uint64_t generation, const uint32_t* current,
                           VkDeviceSize offset, uint32_t bytes);
  bool Reserve(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& offset);
  // Like Reserve, but the offset is a multiple of `multiple` (the stride: a multiple of 4, not always a
  // power of 2). Draw has already asked for space including that extra padding (gap_base_zero).
  bool ReserveMultiple(VkDeviceSize bytes, VkDeviceSize multiple, VkDeviceSize& offset);
  // The pipelines file. [dk] cache/sa2_dk_pipelines.bin: the prewarm list only (no driver cache).
  static std::filesystem::path PathFilePipelines();
  static void ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& data);
  void ReadFilePipelines(std::vector<uint8_t>& cache, std::vector<uint8_t>& list);
  void LoadCachePipelines();
  void SaveCachePipelines();
  void SaveCachePipelinesTimed();
  void SaveCachePipelinesNoGuard();
  // [dk] ds2_pipeline_cache_copy_off_ring and sa2_native_pipeline_cache_drop_copy need a driver cache to take the
  // data from: never here.
  bool CopyOffRing() const { return false; }
  bool DropCopyCache() const { return false; }
  void WriterCacheMain();
  static std::string HeapState();
  void WriteFilePipelines();
  void StopWriterCache();
  bool CreateUpload();
  void CreateVertexCache();
  void ReportVertexCache();
  void CreateSharedSeparate();
  bool CreateBufferUpload(sa2::dk::Arena& upload);
  void MeasureMemoryUpload();
  bool CreateDescriptors();
  bool CreateEmpty();
  bool PrepareEmpty();
  void Barrier(DkCmdBuf cmd, sa2::dk::ImageId image, uint32_t layers);
  void WriteImage(uint32_t heap, uint32_t slot, const DkImageView& view);
  void WriteSampler(uint32_t slot, const DkSampler& sampler);
  // Slots an image heap hands out, plus slot 0 (sa2_diag_texture_slots lowers it for the 2D heap).
  uint32_t SlotCapacity(uint32_t heap) const {
    return heap == 0 && diag_slots_2d_ ? diag_slots_2d_ : heaps_[heap].capacity;
  }
  uint32_t ReserveSlot(uint32_t heap);
  // ---- part B ----------------------------------------------------------------------------------------------------
    // ---- parte B: vertex input (EntryFor, KeyEntry, ComputeEntry) and PrepareTexture (Vulkan 9333-10414) ----
  // The draw's vertex input, recomputed only when the patched VS changes.
  const VerticesEntry* EntryFor(const SubmissionDraw& p);
  // Key of the input cache: declarations/fetches plus the FULL loaded program.
  // ALU/CF changes can invalidate ordered identity and switch exact selection
  // to the unique-candidate fallback even if every fetch word is unchanged.
  // Seed with the selected VS; 0 = do not cache.
  uint64_t KeyEntry(const SubmissionDraw& p) const;
  mutable bool entry_hash_ = true;          // sa2_cpu_ring_entry_hash: off for the session after a DIFFERENCE
  std::array<uint32_t, 4> bias_seen_{};      // sa2_cpu_ring_bias_seen (0 = empty: a biased info is never 0)
  uint32_t bias_seen_next_ = 0;
  mutable uint32_t entry_hash_checks_ = 2048;

  const VerticesEntry* ComputeEntry(const SubmissionDraw& p);

  std::unordered_set<uint32_t> seen_signs_;  // Mass Effect diagnostic (cause 31)
  uint32_t logged_diag_fetch_ = 0;  // ds2_diag_fetch_base
  uint32_t logged_diag_constants_ = 0, logged_diag_framing_ = 0;  // ds2_diag_ps_constants
  std::unordered_map<uint32_t, uint64_t> diag_ps_census_;  // ds2_diag_ps_census
  std::chrono::steady_clock::time_point diag_ps_census_at_{};

  static uint32_t CapInterval(const Texture& t);

  void PrepareTexture(const uint32_t* f, uint32_t& slot, uint32_t& heap,
                      VkDeviceSize& bytes_upload, bool& punctual_sampling, uint64_t& valid_until,
                      uint32_t& host_width_out, uint32_t& host_height_out);

  // --- Measurement only: new textures with the content of another (sa2_native_diag_reuse) ---------------
  /*
   * What it is for. The game reloads the zone packs at new addresses every time it returns to a zone (in
   * the cache diagnostic, of 1,024 textures created, only 6 landed at an address already used). Since the
   * address is part of the key, a returning texture is a new texture: the image is created, bound,
   * untiled and uploaded again, even though the same image, with the same bytes, is still in the cache
   * under the old key and nothing uses it. This measures how often that happens:
   *   - how many new textures have the same content and shape as a live one (the image would be the same);
   *   - how many of those others are cold: not checked for more than kFramesNoUsageForDrop frames,
   *     which is what eviction uses to mean "unused" (a texture in use is checked at least every 39
   *     frames). Those are the duplicates the cache holds that nothing in use depends on.
   * With MB, to see how much GPU memory and upload traffic is behind them. It changes nothing: no
   * decision of the ring reads what is recorded here.
   *
   * How. live_per_content_ holds every texture with a valid raw hash under its content key: the XXH3
   * of its shape without the address (the key words without the base and mip addresses, plus the
   * VkFormat, the host size, the levels and the layers) seeded with its raw hash (the guest bytes of base
   * and mips). Two textures with the same content key have the same bytes read with the same layout: the
   * same host data. It is maintained:
   *   - when the image is created (the previous one is removed if the image is recreated);
   *   - when the raw hash is set: the usual path of PrepareTexture;
   *   - when the texture is released: DropImages, which the gradual, batch and out-of-memory
   *     evictions all go through.
   * A new texture, once its first hash is known, looks at those with the same content key, at most
   * kReuseLookMax: O(1) per texture, never a walk over the live ones. Removing one is O(k), with k
   * the ones with the same content (the repeats; almost always 0 or 1). Candidates are validated when
   * looked at (still under that content key, with a prepared image), so a
   * stale entry cannot count.
   */
  // ---- part C ----------------------------------------------------------------------------------------------------
// ---- Part C (Vulkan lines 10151-11090 after region 07's extraction; 10386-11325 at 98ce31b9): reuse
// measurement, guest texture reads, texture images, uploads, views, samplers and BeginPass.
// Bodies in sa2_native_draws_dk_2.cpp.
  // --- Measurement only: new textures with the content of another (sa2_native_diag_reuse) ---------------
  /*
   * What it is for. The game reloads the zone packs at new addresses every time it returns to a zone (in
   * the cache diagnostic, of 1,024 textures created, only 6 landed at an address already used). Since the
   * address is part of the key, a returning texture is a new texture: the image is created, bound,
   * untiled and uploaded again, even though the same image, with the same bytes, is still in the cache
   * under the old key and nothing uses it. This measures how often that happens:
   *   - how many new textures have the same content and shape as a live one (the image would be the same);
   *   - how many of those others are cold: not checked for more than kFramesNoUsageForDrop frames,
   *     which is what eviction uses to mean "unused" (a texture in use is checked at least every 39
   *     frames). Those are the duplicates the cache holds that nothing in use depends on.
   * With MB, to see how much GPU memory and upload traffic is behind them. It changes nothing: no
   * decision of the ring reads what is recorded here.
   *
   * How. live_per_content_ holds every texture with a valid raw hash under its content key: the XXH3
   * of its shape without the address (the key words without the base and mip addresses, plus the
   * VkFormat, the host size, the levels and the layers) seeded with its raw hash (the guest bytes of base
   * and mips). Two textures with the same content key have the same bytes read with the same layout: the
   * same host data. It is maintained:
   *   - when the image is created (the previous one is removed if the image is recreated);
   *   - when the raw hash is set: the usual path of PrepareTexture;
   *   - when the texture is released: DropImages, which the gradual, batch and out-of-memory
   *     evictions all go through.
   * A new texture, once its first hash is known, looks at those with the same content key, at most
   * kReuseLookMax: O(1) per texture, never a walk over the live ones. Removing one is O(k), with k
   * the ones with the same content (the repeats; almost always 0 or 1). Candidates are validated when
   * looked at (still under that content key, with a prepared image), so a
   * stale entry cannot count.
   */
  static constexpr uint32_t kReuseLookMax = 8;  // candidates checked per new texture
  static constexpr uint32_t kReuseDetails = 8;  // lines detailing the first matches
  bool DiagReuse();
  // The content key of a texture with a valid raw hash. Never 0 (0 = not recorded).
  static uint64_t ContentKey(const Texture& texture);
  // Stops counting a texture (when it is released, when its image is recreated, or before recording it
  // with another hash).
  void RemoveContentTexture(Texture& texture, uint64_t key);
  // With the raw hash just set. Ring thread only (see the body).
  void NoteContentTexture(Texture& texture, uint64_t key);
  // Every 10 s, if there were new textures with a hash.
  void ReportReuse();
  // Every 30 s, one line with what hash sampling has done (see PrepareTexture).
  void ReportSampling(std::chrono::steady_clock::time_point now);
  // The blocks of one level (one layer) of a 2D texture or cubemap (see the body).
  bool ReadLevel(uint64_t address, bool tile, uint32_t pitch_blocks, uint64_t row_bytes,
                 const TextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                 uint8_t* target, bool fused = false, uint32_t unit = 0, uint32_t order = 0);
  // The original byte swap, word by word.
  static void OrderOfAlways(std::vector<uint8_t>& data, uint32_t unit, xenos::Endian order);
  // The original untiling, block by block.
  static void ReadLevelTileOfAlways(const uint8_t* source, uint32_t pitch_blocks, uint32_t log2, uint32_t bytes,
                                    uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                                    uint8_t* target);
  // Base level of a 3D texture, slice by slice.
  bool ReadVolume(const uint32_t* f, const TextureFormat& tf, uint32_t blocks_x,
                  uint32_t blocks_y, uint32_t background, uint32_t blocks_x_host,
                  uint32_t blocks_y_host, uint32_t pitch_blocks, bool tile,
                  std::vector<uint8_t>& data);
  // [dk] VkImageCreateInfo -> sa2::dk::ImageDesc.
  static sa2::dk::ImageDesc InfoImageTexture(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                                             uint32_t background, uint32_t levels);
  bool CreateTexture(ImageNativeDk& image, VkFormat format, uint32_t width, uint32_t height,
                     uint32_t layers = 1, uint32_t background = 0, uint32_t levels = 1, bool emergency = true);
  bool UploadTexture(Texture& texture);
  void RecordCopyTexture(DkCmdBuf upload, const Texture& texture, VkDeviceSize offset);
  // [dk] Returns false instead of a VkResult; the view is a DkImageView made on the spot.
  bool CreateViewTexture(sa2::dk::ImageId image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                         uint32_t heap, DkImageView& view);
  static uint64_t ViewKey(sa2::dk::ImageId image, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap);
  uint32_t SlotView(sa2::dk::ImageId image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                    uint32_t heap = 0);
  // point: no filtering (depth textures).
  uint32_t SlotSampler(const uint32_t* f, bool punctual = false);
  bool PsAlphaOnly();
  bool NoPsNoColor();
  // 1/size of a slot's host image, in the shared constants.
  static void WriteInvSize(uint32_t* shared, uint32_t register_value, uint32_t width, uint32_t height);
  bool BeginPass(const uint32_t* r, const uint64_t keys[5], uint32_t pitch, uint64_t pass_key);
  // ---- part D ----------------------------------------------------------------------------------------------------
  // ---- Part D (Vulkan lines 11327-12150): load modes, shader facts, 16-bit indices, reports, shader variants ----
  // ZCULL: three load modes. kLoadClear is needed to clear the depth of images created without
  // TRANSFER_DST, the only ones eligible for a ZCULL plane. [dk] BeginPass and the clears read them; there is no
  // render pass object (PassFor / CreatePass / FramebufferFor are gone).
  enum : uint32_t { kLoadRead = 0, kLoadIgnore = 1, kLoadClear = 2 };

  bool HasShaderLight(const ShaderEntry& entry);
  uint32_t shader_light_mask_ = 0;
  std::vector<int8_t> shader_light_by_number_;

  // sa2_gpu_structured_cf: whether the shader has the SPEC_CONSTANT_STRUCTURED_CF variant, found once per
  // library entry ([dk] from the DKSH package's facts, or its SPIR-V when the package does not know).
  bool HasStructuredCf(const ShaderEntry& entry);
  std::vector<int8_t> structured_cf_by_number_;

  // sa2_gpu_index_base_held: whether the vertex shader has the SPEC_CONSTANT_VERTEX_INDEX_UNUSED form (the translator
  // emits it only when it proved the shader never reads the vertex index), found once per library entry.
  bool VsIndexUnused(const ShaderEntry& entry);
  std::vector<int8_t> index_unused_by_number_;

  // The plain loop for 16-bit indices (byte-swapped or not), untouched.
  static void IndicesFrom16Scale(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                                 uint32_t& max, bool restart = false);
  // The same loop with NEON, 16 indices per iteration.
  static void IndicesFrom16Neon(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                                uint32_t& max, bool restart = false);
  static void IndicesFrom16NeonRestart(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate,
                                       uint32_t& min, uint32_t& max);
  // The 16-bit indices of a draw (see sa2_native_indices_neon). Ring only.
  void IndicesFrom16(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                     uint32_t& max, bool restart = false);

  void ReportZEarly();
  void ReportSky(double frames);
  void CloseFrameOfTheGuardOfTheSky();

  // bit L set = the fragment shader reads input Location L (specialization constant 60 of the vertex stage).
  // Unknown: all bits. [dk] Keyed by library entry (there are no module handles); read by the ring and the
  // preload thread, under masks_inputs_mutex_.
  uint32_t InputsMaskPS(const ShaderEntry* ps);
  std::mutex masks_inputs_mutex_;
  std::unordered_map<const ShaderEntry*, uint32_t> masks_inputs_ps_;

  // [dk] The pixel stage's "constant 60" in the variant key: the guest XY raster mode bits of the key
  // (kNativeMsaa2PhaseProbe / PhaseMode2), which pick the FragCoord XY rewrite but are not in the specialization.
  static uint32_t PsVariantMask60(const PipelineKey& key) {
    return key.rasterization & (me::native::kNativeMsaa2PhaseProbe | me::native::kNativeMsaa2PhaseMode2);
  }
  // [dk] ModuleFor, ModuleRectangle, ModuleAlphaOnly, Module7e3, ModuleZEarly, ModuleGuestFragCoordXY,
  // ModuleDepthHalf and ModuleDepthQuantize in one: the DKSH variant of `entry` for (spec, mask60). nullptr when
  // there is none, or when only the generic DKSH exists and the missing rewrite changes the image (rectangle,
  // FragCoord XY, depth half, float24 quantize: the Vulkan code rejects those too). `generic` = the generic DKSH
  // stands in (it reads the specialization from UBO 3).
  const DkShader* ModuleFor(const ShaderEntry* entry, uint32_t spec, uint32_t mask60, bool* generic);
  // [dk] The synthetic depth-only fragment stage of the float24 quantize experiment (key.ps == 0). nullptr until
  // region 07 embeds it (MakeDepthOnlyFragmentForQuantization has no DKSH yet).
  const DkShader* ModuleDepthOnlyQuantize(bool round_float24);
  std::unordered_set<uint64_t> variants_logged_;  // [dk] (entry number << 8 | kind): one log line each
  // ---- part E ----------------------------------------------------------------------------------------------------
    // ---- parte E: fixed state, canonical key, dynamic state, PipelineFor (Vulkan 12147-13221) ----
  /*
   * The fixed state of a pipeline, exactly as PipelineFor sets it. It is its usual code, moved here without
   * changing any computation. Used by PipelineFor and by the guards of the canonical key (phase 0a) and of
   * dynamic state (phases 1 and 2), which thus compare against what the pipeline really carries and not
   * against another copy of the same computations.
   * [dk] FixedPipelineState (the eight Vk*CreateInfo of a VkGraphicsPipelineCreateInfo) is FixedState
   * (sa2_native_draws_dk_types.h): the deko3d state groups, bound by StateTracker.
   */
  using FixedPipelineState = FixedState;

  // warn = false from the prewarm thread (warnings belong to the ring only).
  void FillFixedState(const PipelineKey& key, FixedPipelineState& fixed, bool warn = true);

  // One stencil face, in what deko3d looks at in the state (masks and reference are dynamic).
  static bool SameFaceStencil(const DkDepthStencilState& a, const DkDepthStencilState& b, bool back);

  /*
   * Two fixed states, field by field, in what the GPU looks at: the blend equation only with blending
   * enabled, each half only if the mask writes its channels, the Z function only with a Z test, stencil
   * operations only with stencil. Returns the first differing field, or nullptr if there is none.
   */
  static const char* EffectiveDifference(const FixedPipelineState& a, const FixedPipelineState& b);

  // Phase 0a (sa2_native_canonical_key). A draw's key in canonical form.
  static void Canonicalize(PipelineKey& c);

  // The phase 0a guard.
  bool CheckCanonicalKey(const PipelineKey& raw, const PipelineKey& canonical, uint64_t n);

  // The key used to look up a draw's pipeline.
  PipelineKey SearchKey(const PipelineKey& key);

  // Every 20 s, phase 0a (sa2_native_canonical_key).
  void ReportCanonicalKey();

  // Every 20 s, phase 0b (sa2_native_pipeline_between_passes).
  void ReportPipelineBetweenPasses();

  /*
   * Dynamic state phases 1 and 2. The state that leaves the pipeline, with the Vulkan values the usual
   * pipeline would carry (the criterion of FillFixedState, which is PipelineFor's).
   * [dk] Kept as data (PostponedSky::eds, eds_recorded_): in deko3d every group is dynamic and eds_mode_ stays
   * 0, so nothing fills it outside EdsStateFor.
   */
  struct EdsState {
    uint32_t face = 0;         // VkCullModeFlags
    uint32_t front = 0;       // VkFrontFace
    uint32_t topology = 0;    // VkPrimitiveTopology
    uint32_t reset = 0;     // primitiveRestartEnable
    uint32_t bias = 0;        // depthBiasEnable
    uint32_t test_z = 0;     // depthTestEnable
    uint32_t writes_z = 0;    // depthWriteEnable
    uint32_t function_z = 0;    // VkCompareOp
    uint32_t stencil = 0;     // stencilTestEnable
    uint32_t ahead[4] = {};  // failOp, passOp, depthFailOp (VkStencilOp) and compareOp (VkCompareOp)
    uint32_t behind[4] = {};   // the same for the back face
    uint32_t n_colors = 0;                    // phase 2: the pass's color targets, compacted as in PipelineFor
    VkBool32 active_blend[4] = {};            // blendEnable
    VkColorBlendEquationEXT equation[4] = {};  // color and alpha factors and operations
    VkColorComponentFlags mask[4] = {};     // colorWriteMask
  };

  // Without EDS3 the dynamic topology must be of the same class as the pipeline's: the lookup key carries
  // one per class. Classes the ring does not draw stay as they are.
  static uint32_t RepresentativeTopology(uint32_t topology);

  static const char* NameEdsMode(uint32_t mode);

  // The dynamic state of a raw key in the given mode (kEds12 | kEds3).
  void EdsStateFor(const PipelineKey& c, EdsState& e, uint32_t mode);

  // [dk] No vkCmdSet* to record: every deko3d state group is dynamic and eds_mode_ is always 0 (see the body).
  void EmitDynamicState(DkCmdBuf cmd, const EdsState& d, bool all, uint32_t mode);

  // [dk] Phases 1 and 2 never run (eds_mode_ = 0): true, nothing recorded.
  bool PinDynamicState(DkCmdBuf cmd, const PipelineKey& key);

  // [dk] No EDS functions to load: phases 1 and 2 stay off, one log line says why.
  void LoadDynamicState();

  // Every 20 s, dynamic state phases 1 and 2.
  void ReportDynamicState();

  // Phase 2. The blend operation of a register field, like the lambda in FillFixedState.
  VkBlendOp OperationBlend(uint32_t op);

  // [dk] A failed creation stays in pipelines_ (as the VK_NULL_HANDLE the Vulkan map kept) as a PipelineDk without
  // shaders: this is the pointer the lookups, the cells and CheckCellPipeline hand out for it (nullptr).
  static const PipelineDk* PipelineOf(const PipelineDk& pipeline) { return pipeline.vs ? &pipeline : nullptr; }

  const PipelineDk* PipelineFor(const PipelineKey& key, const VerticesEntry& entry, const SubmissionDraw& p);

  /*
   * The deko3d "pipeline" for a key: CreatePipelineVulkan's fixed state (FillFixedState) and its two
   * specialization constants (UBO 3, SpecTable), so the prewarm could build the same thing from its thread.
   * The vertex input is not in it (the draw binds VerticesLayoutFor(entry) when the entry changes): only
   * what vkCreateGraphicsPipelines would refuse is checked. warn = false outside the ring.
   */
  bool CreatePipelineDk(const PipelineKey& key, const VerticesEntry& entry, const DkShader* vs, const DkShader* ps,
                        uint32_t mask60, bool generic, bool warn, PipelineDk& pipeline, uint32_t& n_colors_output);
  // ---- part F ----------------------------------------------------------------------------------------------------
  // ---- Part F (lines 13205-13749): the prewarm list, the shader preload, the prewarm thread, DestroyImage ----
  /*
   * Pipeline prewarm (sa2_native_pipelines_prewarm). [dk] In deko3d there is nothing to compile: the thread checks
   * the records against the library and reads ahead their DKSH variants (LibraryDksh::Preload). See TryPrewarm.
   */
  enum : uint8_t { kPendingList = 0, kPrewarmedList = 1, kListNoShader = 2, kFailedList = 3,
                   kOtherListMode = 4 };
  static constexpr uint64_t kNsCompiled = 5000000;  // more than this: really compiled (a cache hit is 0-2 ms)
  static constexpr uint64_t kNsSlow = 20000000;     // the ring creating one already prewarmed: mismatch

  void LoadPipelinesList();
  std::vector<uint8_t> SerializePipelinesList() const;
  void NotePipelineCreated(const PipelineKey& key, const VerticesEntry& entry, const SubmissionDraw& p,
                           bool created, uint64_t ns);
  void TryPreloadShaders();
  void StopPreloadShaders();
  void TryPrewarm();
  void PrewarmedLoop();
  void PrewarmedReport(std::chrono::steady_clock::time_point now);
  void StopPrewarmed();
  void DestroyImage(ImageNativeDk& image);

// ---- Data members (lines 13751-14742 of the Vulkan file) ---------------------------------------------------
  struct Heap {
    uint32_t capacity = 0;
    uint32_t next = 1;
    std::vector<uint32_t> free;
  };

  // [dk] vulkan_device_, dfn_, device_ (one device per process: sa2::dk::Device()), buffer_address_ (blocks carry
  // their GPU address) and label_gpu_ (no debug labels in deko3d) are gone.
  rex::memory::Memory* memory_ = nullptr;
  ContextTargetsDk* context_ = nullptr;

  // [dk] upload_ (VkBuffer) is gone: vertices, indices and uniform buffers are bound by address
  // (upload_address_ + offset). upload_memory_ is the current slot's block (for the flush in BeforeSend) and
  // upload_type_ its DkMemBlockFlags (sa2_dk_upload_uncached).
  const sa2::dk::Block* upload_memory_ = nullptr;
  uint32_t upload_type_ = UINT32_MAX;
  VkDeviceSize upload_real_size_ = 0;
  uint8_t* upload_data_ = nullptr;
  DkGpuAddr upload_address_ = 0;
  VkDeviceSize used_upload_ = 0;
  bool coherent_upload_ = false;
  bool cached_upload_ = false;  // the upload buffer's memory type has CPU caching (sa2_cpu_ring_compare_uploaded)
  uint64_t epoch_upload_ = 0;
  uint64_t frame_ = 0;
  // Fast untiling and its guard (see ReadLevel).
  static constexpr uint32_t kLevelsToCheck = 200;  // it used to be 2000
  static constexpr uint32_t kCheckAOfEach = 64;   // after the first ones, by sampling
  uint64_t seen_levels_ = 0;
  uint64_t seen_orders_ = 0;
  int tile_fast_ = -1;
  uint32_t checked_levels_ = 0;
  uint32_t checked_orders_ = 0;
  std::vector<uint8_t> check_tile_;
  // Texture check budget (see PrepareTexture).
  static constexpr uint32_t kPostponementMax = 8;
  int32_t budget_fingerprints_ = -1;
  uint64_t frame_fingerprints_ = UINT64_MAX;
  uint64_t bytes_frame_fingerprint_ = 0;
  // Sampled recheck of stable textures (see PrepareTexture and SampleFingerprint).
  static constexpr uint64_t kSamplesToCheck = 3000;  // the first ones, with both hashes: the guard
  int32_t sampling_fingerprints_ = -1;      // -1 cvar not read; 0 off; N: 1 in N rechecks is full
  uint64_t checked_samples_ = 0;  // stable rechecks with both hashes, since start-up
  uint64_t samples_with_the_two_ = 0;  // the same, since the last report line
  uint64_t samples_hits_ = 0;     // decided on the sample alone
  uint64_t distinct_samples_ = 0;    // the sample changed and the full path followed
  uint64_t bytes_sample_ = 0;
  uint64_t bytes_saved_sample_ = 0;
  std::chrono::steady_clock::time_point report_sampling_{};
  // Vertex copies on a separate thread (EnqueueCopy). Power-of-2 capacity.
  const bool active_copies_ = REXCVAR_GET(sa2_native_uploads_thread);
  std::array<WorkCopy, 8192> copies_{};
  std::atomic<size_t> written_copies_{0};
  // How many copies are done, whether by the thread or the ring (see WaitUploads). Outside
  // WaitUploads they finish in order, and then it is also the index of the first one not done.
  std::atomic<size_t> done_copies_{0};
  bool copies_sleeping_ = false;   // under copies_mutex_: the copy thread sleeps or is about to
  bool copies_waiting_ = false;   // under copies_mutex_: the ring waits in WaitUploads
  std::mutex copies_mutex_;
  std::condition_variable copies_cv_;
  std::condition_variable done_copies_cv_;
  bool copies_stop_ = false;  // guarded by copies_mutex_
  std::thread copies_thread_;
  uint64_t copies_in_line_ = 0;
  uint64_t waits_copies_ = 0;
  uint64_t ns_waiting_copies_ = 0;
  std::atomic<size_t> copies_progress_{0};  // copies done by the thread, one by one (diagnostic)
  std::thread::id copies_producer_;
  bool copies_other_warned_thread_ = false;
  // The ring's help (sa2_native_uploads_help) and its guard. See WaitUploads.
  const bool copies_requested_help_ = REXCVAR_GET(sa2_native_uploads_help);
  int32_t copies_phase_ = 0;         // PhaseCopies; ring only
  bool copies_no_thread_ = false;    // off after a MISMATCH: copies go on the ring; ring only
  std::atomic<size_t> taken_copies_{0};  // how many were taken, in order, by the thread or the ring (with CAS)
  std::mutex copies_chunk_mutex_;          // held by the thread while it copies what it took (lends priority)
  std::array<std::atomic<uint32_t>, 8192> copies_marks_{};  // index + 1 of the last copy done in each slot
  std::atomic<uint64_t> copies_thread_n_{0};  // copies done by the thread, for the report
  std::atomic<uint64_t> copies_thread_bytes_{0};
  size_t verified_copies_ = 0;  // marks checked up to here; ring only
  uint64_t checked_copies_ = 0;
  uint64_t copies_waits_looking_ = 0;
  uint64_t helped_copies_ = 0;
  uint64_t copies_bytes_helped_ = 0;
  uint64_t copies_ns_helping_ = 0;
  uint64_t copies_differences_ = 0;
  struct ReportCopiesFigures {  // the last 10 s (ReportCopies); ring only
    uint64_t queued = 0, bytes_queued = 0, in_line = 0, bytes_in_line = 0;
    uint64_t helped = 0, bytes_helped = 0, ns_helping = 0, waits_with_help = 0;
    uint64_t waits = 0, no_take = 0, waits_chunk = 0, ns_chunk = 0, ns_chunk_worst = 0;
    uint64_t whole_waits = 0, ns_wait = 0, ns_wait_worst = 0, checked = 0;
  };
  ReportCopiesFigures copies_inf_{};
  std::chrono::steady_clock::time_point copies_report_{};
  uint64_t copies_thread_n_previous_ = 0;
  uint64_t copies_thread_bytes_previous_ = 0;
  // [dk] BufferUpload (VkBuffer + VkDeviceMemory + mapping + address) became sa2::dk::Arena (the block, its CPU
  // pointer and GPU address); the bump offset stays in used_upload_ / shared_used_ as in the Vulkan code.
  // One per work slot (there are 3). With fewer than there are slots, two slots would share a buffer and
  // the CPU would write over what the GPU is still reading.
  std::array<sa2::dk::Arena, kSlotsOfWork> uploads_{};  // upload_* are the current slot's
  // Separate shared constants, CPU-cached (sa2_shared_native_cache).
  std::array<sa2::dk::Arena, kSlotsOfWork> shared_bufs_{};
  bool shared_separate_ = false;
  bool shared_coherent_ = true;
  uint32_t shared_type_ = 0;  // [dk] DkMemBlockFlags of the shared blocks
  uint8_t* shared_data_ = nullptr;
  const sa2::dk::Block* shared_memory_ = nullptr;  // [dk] the current slot's block
  VkDeviceSize shared_real_size_ = 0;
  VkDeviceSize shared_used_ = 0;

  // [dk] layouts_, pool_, sets_, layout_pipeline_, layout_ubo_, pool_ubo_ and sets_ubo_ are gone: one image set
  // and one sampler set for the process (sa2::dk::BindDescriptors), and the constants are uniform buffers
  // bound by address (UBO 0 VS, 1 PS, 2 shared, 3 specialization).
  // Set 4, the constants through dynamic UBOs. [dk] Always on: the deko3d shaders only have UBOs.
  bool use_ubo_ = true;
  VkDeviceSize alignment_ubo_ = DK_UNIFORM_BUF_ALIGNMENT;
  uint32_t slot_current_ = 0;
  bool ubo_bound_ = false;
  uint32_t slot_ubo_bound_ = UINT32_MAX;
  std::array<uint32_t, 3> offsets_ubo_bound_{};
  // Set 4 by differences (ControlSet4, ReportSet4).
  std::array<uint64_t, 8> set4_changes_{};  // binds by what changes: bit 0 VS, bit 1 PS, bit 2 shared
  uint64_t set4_first_ = 0;              // binds after a new command buffer, a new slot or the sky
  uint64_t set4_draws_ = 0;
  std::array<uint64_t, 3> reuploaded_vs_{};  // constants uploaded again: by generation, by epoch, because they grow
  std::array<uint64_t, 3> reuploaded_ps_{};
  std::chrono::steady_clock::time_point set4_report_{};
  bool set4_request_ = true;
  bool set4_recorded_request_ = false;
  bool set4_warned_difference_ = false;
  uint64_t set4_nvk_previous_[9] = {};  // NVK counts at the previous report
  // The draw path in NVK (ControlDrawNvk and ReportDrawNvk). Ring only.
  bool nvk_preload_app_ = false;  // hand the pipeline to NVK after PipelineFor
  bool nvk_recorded_draw_ = false;
  bool nvk_off_warned_[5] = {};
  std::chrono::steady_clock::time_point nvk_report_{};
  uint64_t nvk_previous_parts_[16][2] = {};
  uint64_t nvk_previous_counts_[13] = {};
  uint64_t nvk_previous_improvements_[5][4] = {};
  uint64_t sends_ = 0;
  uint64_t sends_ubo_ = 0;
  std::array<Heap, 4> heaps_{};
  std::array<ImageNativeDk, 3> empty_{};
  uint32_t sampler_empty_ = UINT32_MAX;  // [dk] its sampler descriptor (slot 0 of the samplers heap)
  bool empty_prepared_ = false;

  // [dk] A shader "module" is a DKSH variant of the package (LibraryDksh::Variant, region 07), which keeps the
  // loaded ones itself. The SPIR-V rewrites the Vulkan code makes at run time (rectangle, depth half, depth
  // quantize, FragCoord XY, alpha only, early Z, 7e3) are variants built offline and chosen by their bits in
  // the specialization, so modules_, codes_modules_depth_, modules_depth_half_, modules_depth_quantize_,
  // modules_fragcoord_xy_, modules_rectangle_ and modules_depth_mutex_ are gone.
  LibraryDksh* dksh_ = nullptr;     // ActiveLibraryDksh(), read when the first pipeline is created
  sa2::dk::CodeArena code_arena_;   // the draws' shader code memory (never freed; ring thread only)
  std::unordered_map<const ShaderEntry*, me::native::RectangleShader> shaders_rectangle_;
  std::unordered_set<uint32_t> recorded_rectangles_;
  std::unordered_set<uint32_t> rectangles_recorded_constants_;
  uint64_t me_bound_collisions_ = 0;
  uint32_t me_depth_rectangle_audits_ = 0;
  // [dk] The value is a PipelineDk (sa2_native_draws_dk_types.h: the shaders, the FixedState FillFixedState
  // decodes, the UBO 3 addresses): what vkCreateGraphicsPipelines baked. Nodes of an unordered_map do not move,
  // so &pipelines_[k].second is stable for the session and pointer comparisons stand for handle comparisons.
  std::unordered_map<uint64_t, std::pair<PipelineKey, PipelineDk>> pipelines_;
  // [dk] UBO 3 of the generic shaders ({spec 0, spec 60}): one block per distinct pair, never rewritten.
  SpecTable spec_table_;
  // One-entry shortcut for PipelineFor (see the comment there).
  PipelineKey last_key_{};
  const PipelineDk* last_pipeline_ = nullptr;
  bool last_valid_key_ = false;
  // direct cache in front of pipelines_ (sa2_native_pipelines_direct).
  struct CellPipeline {
    PipelineKey key{};
    const PipelineDk* pipeline = nullptr;  // nullptr = empty slot
  };
  static constexpr size_t kCellsPipeline = 256;  // power of 2; 256 x 88 bytes = 22 KB contiguous
  std::array<CellPipeline, kCellsPipeline> cells_pipeline_{};
  bool pipelines_direct_ = true;           // the cvar, once per frame
  bool pipelines_direct_off_ = false;  // the guard saw a difference
  uint64_t pipelines_direct_hits_ = 0;
  uint64_t pipelines_direct_failures_ = 0;
  uint64_t pipelines_direct_checked_ = 0;
  uint64_t pipelines_direct_previous_hits_ = 0;
  uint64_t pipelines_direct_previous_failures_ = 0;
  std::chrono::steady_clock::time_point pipelines_direct_report_{};
  static constexpr uint64_t kPipelinesToCheck = 200000;
  // sa2_native_count_changes_pipeline (CountChangePipeline and ReportChangesPipeline).
  enum : uint32_t {
    kChangeBindings = 0,        // vkCmdBindPipeline calls from Draw, counted
    kChangeAfterPass,           // of those, the first of a pass (BeginPass forgets the bound pipeline)
    kFirstChange,            // no previous key in the buffer (new buffer or after the deferred sky)
    kIdenticalChange,           // the same key: only after BeginPass
    kChangeShaders,            // another VS or PS
    kChangeEntry,            // same shaders, different vertex input
    kChangeFormats,           // same shaders and input, different formats (another pass)
    kChangeSpecAlpha,           // all the above equal; specialization only changes the alpha test (bits 1, 16-18)
    kChangeSpecZEarly,      // ... the alpha test and the early Z (bit 20)
    kChangeSpecOther,           // ... other specialization bits
    kChangeNoEffect,          // state only, in bits PipelineFor does not read: effectively the same pipeline
    kChangeNoEffectAfterPass,  // ... and the first of a pass
    kChangeOnlyState,         // real state-only changes: what dynamic state would avoid
    kChangeAfterStatePass,     // ... and the first of a pass
    kChangeEds12,              // ... without blending, masks or another topology class (EDS1/EDS2, core 1.3)
    kChangeEds12AfterPass,      // ... and the first of a pass
    kFieldTopology,           // per field, on the canonical state (one bind may change several)
    kFieldClassTopology,
    kFieldBlend,
    kFieldMasks,
    kFieldZ,
    kFieldStencil,
    kFieldFace,
    kFieldReset,
    kFieldBias,
    kChangesN
  };
  std::array<uint64_t, kChangesN> changes_pipeline_{};
  PipelineKey bound_key_{};       // the last key bound in this command buffer
  bool bound_valid_key_ = false;   // false: new buffer, after the deferred sky, or not counting
  bool count_changes_pipeline_ = true;  // the cvar, once per frame (ReportChangesPipeline)
  uint64_t changes_previous_draws_ = 0;
  uint64_t changes_previous_frames_ = 0;
  std::chrono::steady_clock::time_point changes_report_{};
  // Dynamic state, phase 0a (sa2_native_canonical_key, SearchKey).
  static constexpr uint64_t kCanonicalToCheck = 200000;
  bool canonical_key_ = true;           // the cvar, once per command buffer (UseSlot)
  bool canonical_off_key_ = false;  // the guard saw a difference
  bool canonical_valid_ = false;         // canonical_raw_ -> canonical_result_ is the previous draw's
  PipelineKey canonical_raw_{};
  PipelineKey canonical_result_{};
  uint64_t canonical_changed_ = 0;      // keys Canonicalize changes (not counting consecutive repeats)
  uint64_t canonical_checked_ = 0;
  uint64_t canonical_changed_previous_ = 0;
  uint64_t canonical_pipelines_previous_ = 0;
  std::chrono::steady_clock::time_point canonical_report_{};
  // Dynamic state, phase 0b (sa2_native_pipeline_between_passes).
  bool pipeline_between_passes_ = true;  // the cvar, once per command buffer (UseSlot)
  bool pipeline_between_recorded_passes_ = false;
  uint64_t passes_with_pipeline_ = 0;   // passes started with a bound pipeline that is kept
  uint64_t between_passes_previous_passes_ = 0;
  uint64_t between_kept_previous_passes_ = 0;
  std::chrono::steady_clock::time_point between_passes_report_{};
  // Dynamic state phases 1 and 2 (PinDynamicState, its guard and its report).
  // [dk] Everything is dynamic state in deko3d: eds_mode_ stays 0 (no EDS phase), kDynamicEds12/kDynamicEds3 and
  // the vkCmdSet* pointers are gone. The fields stay as data for the code that reads them (sky_, reports).
  enum : uint32_t { kEds12 = 1, kEds3 = 2 };  // bits of eds_mode_ and of PipelineKey::fill2
  static constexpr uint64_t kEdsToCheck = 200000;
  enum : uint32_t {
    kEdsFace = 0,
    kEdsFront,
    kEdsTopology,
    kEdsReset,
    kEdsBias,
    kEdsTestZ,
    kEdsWritesZ,
    kEdsFunctionZ,
    kEdsStencil,
    kEdsStencilOps,
    kEdsActiveBlend,  // phase 2
    kEdsEquation,      // phase 2
    kEdsMask,       // phase 2
    kEdsAll,          // times everything is set (not a call)
    kEdsN
  };
  bool eds12_available_ = false;  // API 1.3 and the ten functions
  bool eds_off_ = false;       // the guard saw a difference: for the rest of the session, everything in the pipeline
  bool eds_recorded_mode_ = false;
  uint32_t eds_mode_ = 0;          // kEds12 | kEds3 of this command buffer (UseSlot; the guard sets it to 0)
  bool eds_valid_ = false;        // eds_recorded_ is what is set in this buffer (false: new buffer, sky or pass)
  EdsState eds_recorded_{};
  PipelineKey eds_key_{};      // raw key of the last state set
  uint64_t eds_draws_ = 0;       // draws with dynamic state
  uint64_t eds_repeated_ = 0;     // ... with the same raw key as the previous one: nothing to set or check
  uint64_t eds_checkable_ = 0;  // ... with something to set (what the guard counts)
  uint64_t eds_checked_ = 0;
  uint64_t eds_previous_draws_ = 0;
  uint64_t eds_repeated_previous_ = 0;
  uint64_t eds_previous_frames_ = 0;
  std::array<uint64_t, kEdsN> eds_calls_{};    // vkCmdSet* recorded per state since the last report
  std::array<uint64_t, 4> pipelines_per_mode_{};  // pipelines created per mode (fill2) since start-up
  std::chrono::steady_clock::time_point eds_report_{};
  // The same mapping as kFactors in FillFixedState (fields 2, 3 and 17-31 give ZERO).
  static constexpr VkBlendFactor kFactorsBlend[32] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  bool eds3_available_ = false;  // the SDK enables the extension and its three features; the driver provides the functions
  // [dk] passes_, framebuffers_, framebuffers_views_ and fb_retired_ are gone (no render pass or framebuffer
  // objects: BeginPass binds the targets). fb_* stay as counters for the reports.
  bool fb_retire_ = false;
  uint64_t fb_forgotten_views_ = 0;
  uint64_t fb_forgotten_ = 0;
  std::unordered_map<uint64_t, Texture> textures_;
  // Measurement only (sa2_native_diag_reuse): textures with a hash, by content key (content key ->
  // texture key; see NoteContentTexture), and the counts for the 10 s report.
  std::unordered_multimap<uint64_t, uint64_t> live_per_content_;
  uint64_t distinct_contents_ = 0;
  int32_t diag_reuse_ = -1;  // -1 = cvar not read
  uint64_t reuse_new_ = 0;
  uint64_t reuse_match_ = 0;
  uint64_t reuse_cold_ = 0;
  uint64_t reuse_no_look_ = 0;
  uint64_t reuse_bytes_new_ = 0;
  uint64_t reuse_bytes_match_ = 0;
  uint64_t reuse_bytes_cold_ = 0;
  uint32_t reuse_details_ = 0;
  std::chrono::steady_clock::time_point report_reuse_{};
  std::unordered_map<uint64_t, View> views_;
  // The views_ keys of each image. InvalidateImages (every resolve and every copy) used to walk all of
  // views_: 2.7 % of the ring thread in play. Maintained together with views_ in SlotView
  // (insertion) and in RemoveView (every removal).
  std::unordered_map<sa2::dk::ImageId, std::vector<uint64_t>> views_per_image_;
  // [dk] The sampler key -> (its DkSampler, its slot in the samplers heap). No object to destroy.
  std::unordered_map<uint32_t, std::pair<DkSampler, uint32_t>> samplers_;
  // Diagnostic: filter values already logged (SlotSampler).
  uint32_t aniso_seen_ = 0;
  bool aniso_recorded_ = false;  // sa2_anisotropic_native, one line when the first one is created
  bool lod_bias_recorded_ = false;  // ds2_texture_lod_bias, likewise
  // sa2_native_shadows_pending_bias, read once per frame.
  int32_t shadows_pending_bias_ = 0;
  bool shadows_recorded_bias_ = false;
  uint32_t mip_max_seen_ = 0;
  std::array<bool, 1024> seen_biases_{};
  std::vector<Texture*> textures_to_upload_;
  uint64_t me_ordered_updates_ = 0;
  std::vector<uint8_t> temporal_;
  std::vector<uint32_t, NoInitialize<uint32_t>> indices_;
  std::vector<uint32_t, NoInitialize<uint32_t>> converted_;
  std::vector<uint16_t, NoInitialize<uint16_t>> indices16_;  // fast path: 16 bits, not converted
  // sa2_native_indices_cache: guest index range -> its copy in this upload buffer (direct-mapped).
  struct IndicesEntryCache {
    uint64_t key = 0, offset = 0, fingerprint = 0, frame = UINT64_MAX;
    uint32_t count = 0, vmin = 0, vmax = 0, generation = 0;
  };
  std::array<IndicesEntryCache, 4096> indices_cache_{};
  bool indices_cache_active_ = false;
  uint32_t indices_cache_generation_ = 0, indices_cache_sinc_view_ = 0;
  uint64_t indices_cache_hits_ = 0, indices_cache_bytes_saved_ = 0, indices_cache_lost_ = 0;
  uint64_t indices_cache_stale_ = 0;  // ds2_indices_cache_verify
  uint64_t lazy_baselines_ = 0;  // ds2_texture_lazy_fingerprint
  int fused_untile_ = -1;        // ds2_texture_fused_untile: -1 = not read yet
  uint64_t seen_fused_ = 0;
  uint32_t checked_fused_ = 0;
  std::vector<uint8_t> check_fused_;
  uint64_t lazy_skipped_ = 0;
  uint64_t lazy_skipped_bytes_ = 0;
  std::chrono::steady_clock::time_point report_lazy_{};
  // IndicesFrom16 (sa2_native_indices_neon). Ring only.
  int32_t indices_neon_ = -1;  // -1 cvar not read, 0 plain loop, 1 NEON
  uint64_t indices_neon_draws_ = 0;
  uint64_t indices_neon_checked_ = 0;
  std::vector<uint16_t, NoInitialize<uint16_t>> indices_neon_test_;
  // Dynamic state already recorded in the current command buffer and pass.
  bool recorded_state_ = false;
  uint64_t push_recorded_[3] = {};
  VkViewport viewport_recorded_{};
  VkRect2D recorded_scissor_{};
  float recorded_blend_[4] = {};
  float recorded_bias_[2] = {};  // depth bias: constant and slope
  float warned_bias_[2] = {};
  uint32_t warnings_bias_ = 0;
  bool label_pass_ = false;
  std::unordered_set<uint64_t> me_fetch_ambig_diagnosed_;
  uint32_t stencil_recorded_[2] = {};  // RB_STENCILREFMASK for front and back
  bool stencil_recorded_valid_ = false;
  VkIndexType indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  // Shared constants of the previous draw and where they were uploaded.
  uint32_t shared_previous_[kSharedWords] = {};
  uint64_t indexed_fetch_uploads_ = 0;  // banjo-nx: indexed vertex fetch buffers passed to draws
  uint32_t banjo_vertex_range_logs_ = 0;  // banjo-nx diagnostic
  uint32_t banjo_exp_logs_ = 0;  // banjo-nx diagnostic
  std::unordered_set<uint32_t> banjo_index_layouts_;  // banjo-nx diagnostic
  uint64_t shared_looked_ = 0;    // C6 report, to decide how to make the block cheaper
  // sa2_diag_shared_words
  uint64_t diag_shared_draws_ = 0, diag_shared_changed_ = 0, diag_shared_only_base_no_r0_ = 0,
           diag_shared_base_and_slots_ = 0;
  std::array<uint64_t, 8> diag_shared_only_group_{};
  std::array<uint64_t, kSharedWords> diag_shared_word_{};
  uint64_t shared_changed_ = 0;
  VkDeviceSize shared_offset_ = 0;
  uint64_t shared_epoch_ = UINT64_MAX;

  VerticesEntry entry_;
  const VerticesEntry* entry_current_ = &entry_;  // entry_ or a cache element
  std::unordered_map<uint64_t, VerticesEntry> inputs_cache_;
  uint64_t inputs_cache_hits_ = 0;
  const ShaderEntry* vs_entry_ = nullptr;
  uint64_t generation_entry_ = UINT64_MAX;
  bool valid_entry_ = false;

  uint64_t constants_vs_generation_ = UINT64_MAX;
  bool me_constant_audit_hdr_ = false;
  bool me_payload_hdr_ = false;
  uint32_t me_payload_vs_ = 0;
  uint32_t me_resolve_fetch_vs_ = 0, me_resolve_fetch_sampler_ = 0;
  int me_resolve_fetch_ps_ = -1;
  std::unordered_set<uint64_t> me_resolve_fetch_contracts_;
  bool me_resolve_fetch_cap_logged_ = false;
  bool me_resolved_fetch_failed_this_draw_ = false;
  int me_payload_ps_ = -1;
  uint64_t me_payload_checked_[2] = {}, me_payload_bytes_[2] = {};
  uint64_t me_payload_changed_[2] = {}, me_payload_staged_[2] = {};
  uint64_t me_payload_skipped_[2] = {}, me_payload_logged_[2] = {};
  uint64_t me_payload_invalid_ = 0;
  uint64_t me_constant_audit_checked_[2] = {};
  uint64_t me_constant_audit_mismatches_[2] = {};
  uint64_t me_constant_audit_bytes_[2] = {};
  uint64_t constants_vs_epoch_ = UINT64_MAX;
  VkDeviceSize constants_vs_offset_ = 0;
  uint64_t constants_ps_generation_ = UINT64_MAX;
  uint64_t constants_ps_epoch_ = UINT64_MAX;
  uint8_t constants_ps_mode_ = UINT8_MAX;
  uint32_t constants_ps_tonemap_scale_bits_ = UINT32_MAX;
  VkDeviceSize constants_ps_offset_ = 0;
  uint32_t constants_vs_bytes_ = 0;
  // sa2-nx: CPU copies of the constant blocks uploaded last (sa2_native_constants_dedupe) and the reuses they gave.
  std::array<uint8_t, kRegistersConstants * 4> shadow_constants_vs_{}, shadow_constants_ps_{};
  std::array<uint64_t, 2> deduped_constants_{};
  uint32_t constants_ps_bytes_ = 0;

  bool active_pass_ = false;
  uint32_t category_pass_ = kGpuOther;
  bool sent_after_shadows_ = false;        // once per frame
  // Per-frame copies of the diagnostic cvars (see the comment at the Swap).
  bool diag_vertices_repeated_ = false;
  bool diag_stats_draw_ = false;
  std::array<DkGpuAddr, 16> buffers_vertices_{};  // [dk] the base address each binding's offset is relative to
  // Height actually used by each render target, deduced from the scissor. It only grows.
  std::unordered_map<uint32_t, uint32_t> util_height_;
  bool area_util_ = true;
  // Small per-draw savings. See their cvars.
  bool framing_cache_ = true;             // sa2_native_framing_cache, once per frame
  bool framing_cache_off_ = false;    // its guard saw a difference
  bool valid_framing_ = false;
  bool framing_depth_half_ = false;
  uint32_t framing_phase_probe_ = 0;
  uint64_t framing_generation_ = 0;
  uint64_t framing_pass_series_ = 0;
  uint64_t pass_series_ = 0;                // incremented on every BeginPass
  VkViewport framing_viewport_{};
  VkRect2D framing_scissor_{};
  float framing_ndc_[4] = {};
  uint32_t empty_framing_ = 0;
  uint64_t framing_hits_ = 0;         // since start-up (the guard)
  uint64_t checked_framing_ = 0;
  uint64_t framing_hits_report_ = 0;
  uint64_t framing_computations_report_ = 0;
  static constexpr uint64_t kFramingsToCheck = 200000;
  bool util_height_memo_active_ = true;      // sa2_native_util_height_memo
  bool util_height_memo_off_ = false;
  uint32_t* util_height_memo_ = nullptr;     // util_height_ element of the last pitch (the map never erases)
  uint32_t util_height_memo_pitch_ = 0;
  uint64_t util_height_memo_hits_ = 0;
  uint64_t util_height_memo_previous_hits_ = 0;
  static constexpr uint64_t kUtilHeightToCheck = 200000;
  bool key_fast_pass_ = true;          // sa2_native_key_fast_pass
  bool key_fast_off_pass_ = false;
  uint64_t pass_keys_[5] = {};           // the render target bytes that produced pass_key_
  bool pass_valid_keys_ = false;
  uint64_t keys_fast_pass_ = 0;
  uint64_t keys_fast_previous_pass_ = 0;
  static constexpr uint64_t kKeysPassToCheck = 200000;
  bool cvars_per_frame_ = true;        // sa2_native_cvars_per_frame
  bool ps_alpha_only_frame_ = true;
  bool no_ps_no_color_frame_ = true;
  std::chrono::steady_clock::time_point minutiae_report_{};
  bool no_vegetation_ = false;
  // Shadow map vegetation draws discarded early (see the early discard in Draw).
  uint64_t draws_vegetation_soon_ = 0;
  /*
   * The sky draw, saved in full to replay later (sa2_native_postponed_sky).
   *
   * This holds everything the vkCmd* calls of that draw need, by copy: handles (which do not change
   * within the pass) and values (which depend on no guest register once copied). The only thing kept "by
   * reference" are the offsets within the upload buffer, and that buffer only moves forward until
   * UseSlot resets it, always after the pass is closed.
   */
  enum : uint32_t {
    kSkyPerBlend = 0,  // a blended draw arrived: that one does read the background color
    kSkyPerNoZ,        // an opaque draw without Z write arrived: it could not cover the sky
    kSkyPerPassEnd,   // the pass closes (fallback path)
    kSkyPerOtherSky,   // a second sky in the same pass: the first one is emitted now
    kSkyReasons
  };
  struct PostponedSky {
    const PipelineDk* pipeline = nullptr;
    DkGpuAddr buffer = 0;   // the upload one: vertices and indices ([dk] its base address)
    bool usa_ubo = false;
    uint64_t push[3] = {};             // only with constants through pointers
    std::array<uint32_t, 3> offsets_ubo{};
    uint32_t slot_ubo = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    float bias[2] = {};
    bool with_stencil = false;
    uint32_t stencil[2] = {};          // RB_STENCILREFMASK for front and back
    uint32_t n_bindings = 0;
    std::array<VkDeviceSize, 16> offsets_vertices{};
    bool with_indices = false;
    bool indices_from_16 = false;
    uint32_t indices = 0;
    uint32_t first_index = 0;
    uint32_t vmin = 0;
    uint32_t count = 0;               // no indices
    uint32_t ps_more_one = 0;           // for the diagnostic per-draw query
    uint32_t vs_more_one = 0;           // the same, its vertex shader
    uint32_t category = 0;
    uint64_t draws_on_postpone = 0;   // position in the pass when it was deferred
    uint32_t eds_mode = 0;  // phases 1 and 2 its pipeline was looked up with (eds_mode_)
    EdsState eds{};        // and the state that pipeline lacks
  };
  PostponedSky sky_{};
  bool pending_sky_ = false;
  bool postponed_sky_ = true;  // the cvar, read once per frame
  bool recorded_sky_ = false;
  uint64_t draws_in_pass_ = 0;
  uint64_t seen_sky_ = 0;
  uint64_t seen_previous_sky_ = 0;
  // The ones that pass both marks (480 indices and first of the pass). This is the number to watch: if it
  // is not 1.00 per frame, the criterion still does not isolate the dome and nothing should be deferred.
  // The fingerprint-only "detected" once gave 7.00.
  uint64_t sky_candidates_ = 0;
  uint64_t sky_previous_candidates_ = 0;
  /*
   * The self-checking guard.
   *
   * Two earlier versions broke the image by enabling this blindly. The check that was missing ("the
   * criterion detects one draw per frame, not seven") no longer needs a diagnostic build: the game itself
   * runs it, before anything is deferred.
   *
   * Phase 0, observing: for the first kSkyFramesTest frames nothing is deferred (the image
   *   is exactly the non-deferred one) and the candidates in each frame are counted.
   * Phase 1, deferring: only if those frames never had more than one and at least kSkyFramesWithOne
   *   had exactly one. Then yes, and that is 3.4-4.3 ms of GPU time.
   * Phase 2, off for good: as soon as two are seen in the same frame, or if the test does not come out
   *   clean. It is not retried for the whole session.
   *
   * So the worst case of this change is that it does nothing. Breaking the image is not among the
   * possible outcomes.
   */
  enum : uint32_t { kSkyLooking = 0, kSkyPostponing = 1, kDiscardedSky = 2 };
  uint32_t sky_guard_ = kSkyLooking;
  uint32_t sky_guard_frames_ = 0;
  uint32_t sky_guard_with_one_ = 0;
  uint32_t sky_guard_in_frame_ = 0;
  uint32_t sky_guard_max_ = 0;
  uint64_t postponed_sky_count_ = 0;
  uint64_t postponed_previous_sky_ = 0;
  uint64_t sky_no_postponable_ = 0;
  uint64_t sky_two_in_pass_ = 0;
  uint64_t lost_sky_ = 0;
  std::array<uint64_t, kSkyReasons> emitted_sky_{};
  std::array<uint64_t, kSkyReasons> emitted_previous_sky_{};
  uint64_t sky_position_sum_ = 0;
  uint64_t sky_position_previous_sum_ = 0;
  uint64_t sky_position_max_ = 0;
  // The last binding made with vkCmdBindVertexBuffers, so it is not repeated.
  std::array<VkDeviceSize, 16> offsets_recorded_{};
  std::array<DkGpuAddr, 16> buffers_recorded_{};
  std::array<uint64_t, 4> diag7e3_{}, diag7e3_reported_{};
  bool diag_vs_constants_ = false;  // ds2_diag_vs_constants (read per slot)
  std::array<uint64_t, 3> diag_vs_{}, diag_vs_reported_{};  // uploads, bytes, changed float4 registers
  std::array<uint8_t, 4096> diag_vs_last_{}; std::chrono::steady_clock::time_point diag_vs_time_{};  // 7e3 targets: draws, alpha written, dst alpha, blending
  std::chrono::steady_clock::time_point diag7e3_time_{};
  // Draw's rectangle-list scratch (type 8), kept between draws for their capacity.
  VerticesEntry rectangle_entry_;
  std::array<std::vector<uint8_t>, 16> data_rectangle_;  // with ds2_vertex_cache_mb the buffer differs per binding
  // ds2_vertex_cache_mb (sa2_native_vertex_cache.h).
  VertexCache vcache_;
  sa2::dk::Block vcache_buf_{};  // [dk] CpuCached | GpuCached; the cache flushes what it wrote
  bool vcache_active_ = false;
  bool vcache_verify_ = false;
  int32_t vcache_hash_ = 1;
  VertexCache::Figures vcache_reported_{};
  std::chrono::steady_clock::time_point vcache_report_{};
  uint32_t recorded_bindings_ = 0;
  // sa2_native_vertices_base_zero (see the cvar) and its guard.
  bool vertices_base_zero_ = true;           // the cvar, read once per frame (UseSlot)
  bool vertices_base_off_zero_ = false;  // the guard saw a difference: the rest of the session, as before
  uint64_t base_checked_zero_ = 0;       // draws on the base-zero path since start-up (the guard)
  uint64_t base_zero_draws_ = 0;           // since the last report: without binding their own offset
  uint64_t base_zero_total_ = 0;             // since the last report: draws recorded with vertices
  uint64_t base_zero_several_bindings_ = 0;
  uint64_t base_misaligned_zero_ = 0;      // one binding, but the dedupe gave a copy off a multiple
  uint64_t base_zero_recorded_bindings_ = 0;  // vkCmdBindVertexBuffers recorded since the last report
  // sa2_cpu_ring_bound_plan_memo: the last request and its plan.
  me::native::EdramBoundRequest bound_memo_request_{};
  me::native::EdramBoundPlan bound_memo_plan_{};
  bool bound_memo_valid_ = false;
  // The "C6 draw stages" report: timed draws and the stage totals at the last report.
  uint64_t stage_draws_ = 0, report_stage_draws_ = 0;
  std::array<uint64_t, kStagesDraw> report_stages_ns_{};
  // The vertex dedupe's report (ReportBaseZero): fingerprints computed and the bytes they read, and the counters at
  // the last report.
  uint64_t dedupe_fingerprints_ = 0, dedupe_bytes_fingerprinted_ = 0;
  uint64_t report_dedupe_queries_ = 0, report_dedupe_hits_ = 0, report_dedupe_saved_ = 0,
           report_dedupe_discrepancies_ = 0, report_dedupe_collisions_ = 0, report_dedupe_fingerprints_ = 0,
           report_dedupe_fingerprinted_ = 0;
  std::chrono::steady_clock::time_point base_zero_report_{};
  static constexpr uint64_t kBaseZeroToCheck = 200000;
  std::array<uint64_t, 19> sub_ns_{};  // C6 substages, measurement only
  std::array<uint64_t, 19> sub_n_{};
  uint64_t sub_samples_ = 0;
  std::chrono::steady_clock::time_point sub_next_{};
  uint32_t stats_pass_ = UINT32_MAX;
  DkCmdBuf pass_commands_ = nullptr;
  uint64_t pass_key_ = 0;
  uint64_t pass_generation_ = UINT64_MAX;
  uint32_t pass_width_ = 0;
  uint32_t pass_height_ = 0;
  float pass_scale_ = 1.0f;  // 1 except in the scaled shadow map
  uint32_t pass_res_q_ = kScaleQuartersNative;  // sa2-nx render resolution of the pass (me_resolution_scale.h)
  uint32_t pass_samples_ = 1;  // sa2_msaa: host samples of the pass's attachments
  uint32_t msaa_samples_ = 1;  // sa2_msaa: the host sample count of multisampled targets (1 = none)
  float max_line_width_ = 1.0f;  // sa2-nx render resolution: widest line the device draws (1 without wideLines)
  float pass_raster_scale_x_ = 1.0f;  // independent host-only X grid expansion
  std::atomic<uint64_t> raster_grid_ps_rejected_{0};
  std::chrono::steady_clock::time_point pass_start_{};  // pass change breakdown
  uint32_t pass_formats_[5] = {};
  bool pass_depth_float24_half_ = false;
  // [dk] pass_rp_ is gone (no render pass objects).
  uint64_t recording_generation_ = UINT64_MAX;
  const PipelineDk* pipeline_bound_ = nullptr;
  bool sets_bound_ = false;  // [dk] the descriptor sets of this command list (sa2::dk::BindDescriptors)

  std::unordered_set<uint32_t> warned_vs_;
  uint32_t warnings_indices_ = 0;
  uint32_t warnings_swizzle_ = 0;
  uint64_t drawn_ = 0;
  int32_t partial_send_ = -1;  // ds2_native_partial_send, read once
  uint64_t drawn_at_send_ = 0;  // drawn_ at the last submission, full or partial
  uint64_t drawn_timed_ = 0;  // of those, with the stage stopwatch
  uint32_t stopwatch_counter_ = 0;
  bool time_ = false;  // the current draw carries the stage stopwatch
  uint64_t rejected_ = 0;
  // 8 stages. Stage 7 is the pass change, split from the pass stage to tell how much of the 3.9 us per
  // draw is the actual change (15 per frame at 162 us) and how much is what is done on every draw.
  std::array<uint64_t, kStagesDraw> stages_ns_{};
  std::unordered_map<uint32_t, uint64_t> causes_;
  uint32_t last_cause_ = 0;
  uint32_t cause_entry_ = 0;  // cause of the failed vertex input
  uint64_t uploads_texture_ = 0;
  uint64_t megabytes_bytes_ = 0;
  uint64_t bytes_textures_ = 0;  // texture images created (C6 report)
  int32_t textures_mb_max_ = 0;   // sa2_native_textures_mb_max
  // [dk] PoolTextures (large slabs instead of a dedicated allocation per texture) is sa2::dk::CreateImage's own
  // sub-allocator over 32 MB image blocks; its report line is sa2::dk::ImagesSummary().
  // [dk] The texture bind thread (sa2_native_textures_binding_thread: vkBindImageMemory off the ring) is gone:
  // creating a deko3d image over block memory is CPU work only. SubmissionBinding, TextureInFlight,
  // ViewInFlight, queue_bindings_ and the rest of its state go with it; Texture::in_flight stays 0.
  bool releasing_per_missing_of_memory_ = false;  // guard against reentry
  // sa2_native_texture_slots_evict
  uint32_t slots_full_heaps_ = 0;  // bit per image heap: a view found no slot since the last eviction pass
  uint32_t slots_full_logged_ = 0;
  uint64_t slots_full_events_ = 0, released_for_slots_ = 0, report_slots_frame_ = 0;
  bool draining_slots_[3] = {};    // above three quarters, until back under five eighths
  uint32_t diag_slots_2d_ = 0;     // sa2_diag_texture_slots
  uint64_t released_little_to_little_ = 0;
  uint64_t warning_trickle_ = 0;
  uint64_t attempt_eviction_ = 0;
  uint64_t released_textures_ = 0;
  // Diagnostic of why the cache grows: creations per address and last key per shape (address, format and
  // size). Only touched when a texture is created.
  uint64_t created_textures_ = 0;
  uint64_t created_in_view_address_ = 0;
  uint64_t created_same_shape_other_key_ = 0;
  std::unordered_map<uint32_t, uint32_t> created_per_address_;
  std::unordered_map<uint64_t, uint64_t> key_per_shape_;
  std::unordered_map<uint64_t, std::array<uint32_t, 5>> words_per_shape_;
  uint64_t incoherent_keys_ = 0;  // texture key guard (PrepareTexture)
  uint32_t warnings_other_key_ = 0;
  // "C6 counters".
  uint64_t started_passes_ = 0;
  uint64_t full_sends_ = 0;
  uint64_t ns_full_sends_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t bytes_indices_uploaded_ = 0;
  uint64_t samplers_prepared_ = 0;
  uint64_t samplers_cache_ = 0;
  uint64_t ns_passes_ = 0;
  uint64_t ns_vertices_ = 0;
  uint64_t computed_inputs_ = 0;
  uint64_t ns_inputs_ = 0;
  uint64_t reused_inputs_ = 0;
  uint64_t passes_per_generation_ = 0;
  uint64_t passes_per_target_ = 0;
  uint64_t resumed_passes_ = 0;
  uint64_t texels_passes_ = 0;
  std::array<uint64_t, kGpuCategories> texels_per_category_{};
  std::array<uint64_t, kGpuCategories> draws_per_category_{};
  bool inv_tex_size_ = false;
  bool pcf_cheap_ = false;                    // a single shadow map sample
  // Deduplication of vertex uploads within the frame.
  DedupeVertices dedupe_;
  bool dedupe_active_ = true;
  uint32_t dedupe_sinc_view_ = 0;  // last value seen of g_synchronizations_ring
  std::set<uint64_t> warnings_inv_resolved_size_;  // one trace per size
  // [dk] modules_alpha_only_, modules_z_early_ (OpExecutionMode EarlyFragmentTests, sa2_native_z_early) and
  // modules_7e3_ are package variants (kSpecAlphaOnly, kSpecZEarly, kSpecTarget7e3 in the specialization).
  std::unordered_set<uint32_t> biases_color_recorded_;
  uint64_t draws_ps_useless_ = 0;     // no color and a PS that does not discard
  uint64_t draws_ps_required_ = 0;  // no color, but the PS is needed
  uint64_t shadows_active_alpha_ = 0;
  uint64_t scene_with_discard_ = 0;
  uint64_t scene_no_discard_ = 0;   // shadow map draws with the alpha test enabled
  /*
   * sa2_native_z_early and sa2_native_skip_invisibles. The first four split C6's "prevented by"
   * in two: those fixed by testing earlier and those that cannot be fixed because they write depth or
   * stencil (the latter are the exact size of a depth pre-pass).
   */
  enum : uint32_t {
    kZSet = 0,       // its depth test is moved earlier
    kZWritesZ,         // not possible: the draw writes depth
    kZStencil,         // not possible: stencil is involved
    kZAlreadyEarly,       // no alpha test or kill: the GPU already tested early
    kInvisibleBlend,   // blending copies the destination: the draw paints nothing
    kInvisibleAlpha,     // alpha test with the NEVER function
    kInvisibleColorOnly,  // invisible in color but writes Z: only its color is removed
    kZCounts
  };
  std::array<uint64_t, kZCounts> counts_z_{};
  std::array<uint64_t, kZCounts> counts_z_previous_{};
  uint64_t z_early_no_module_ = 0;
  uint64_t frames_z_ = 0;
  uint64_t frames_z_previous_ = 0;
  std::chrono::steady_clock::time_point last_report_z_ = std::chrono::steady_clock::now();
  bool z_early_ = true;
  bool alternation_z_recorded_ = false;
  bool skip_invisibles_ = true;
  uint64_t shadows_off_alpha_ = 0;  // ... and without it (the stage would be unnecessary)
  std::array<uint64_t, kGpuCategories> triangles_per_category_{};
  std::array<uint64_t, kGpuCategories> passes_per_category_{};
  uint64_t ns_render_pass_ = 0;
  // Diagnostic sa2_native_diag_vertices_repeated.
  struct VerticesSeen {
    uint64_t frame = UINT64_MAX;
    uint64_t fingerprint = 0;
  };
  std::unordered_map<uint64_t, VerticesSeen> vertices_seen_;
  uint64_t bytes_repeated_frame_ = 0;
  uint64_t bytes_recorded_ = 0;
  std::chrono::steady_clock::time_point repeated_report_ = std::chrono::steady_clock::now();
  uint64_t bytes_equal_previous_ = 0;
  uint64_t ns_hash_vertices_ = 0;
  // Last texture and sampler result of each PS sampler register.
  struct CacheSampler {
    std::array<uint32_t, 6> fetch{};
    uint64_t frame = UINT64_MAX;
    uint64_t generation = 0;
    uint32_t slot = 0;
    uint32_t heap = 0;
    uint32_t sampler = 0;
    uint64_t valid_until = 0;  // last frame it is valid without going back to PrepareTexture
    uint32_t width = 0;  // host image size, for 1/size
    uint32_t height = 0;
  };
  bool cache_between_frames_ = true;  // sa2_native_cache_textures_between_frames
  bool mipmaps_ = true;                 // sa2_native_mipmaps
  std::array<CacheSampler, 16> cache_samplers_{};
  // Second cache, by the whole fetch constant: a direct-mapped table (the hash picks the slot). It went
  // from 256 to 1024 slots because in play there are ~340 distinct textures per frame and 256 slots
  // collided (64 KB: fits in L2).
  /*
   * From 1024 to 4096 (288 KB, still fits in the 2 MB L2). In a busy scene there are ~3,000
   * draws per frame and ~205 misses of this table per frame (C6 counters: 97,709 in 20 s), at ~15 us
   * each through PrepareTexture (the texture stage measures 1.2-1.5 us per draw with 3.4 % misses). With
   * N distinct fetches in M slots, the fraction sharing a slot with another is 1 - e^(-N/M): with ~600
   * that is 44 % at 1024 and 14 % at 4096. The hit conditions do not change: there is just more room. The
   * misses-by-cause report (every 10 s) says how many were collisions.
   */
  std::array<CacheSampler, 4096> cache_fetch_{};
  uint64_t samplers_cache_fetch_ = 0;
  uint64_t fetch_failures_clash_ = 0;      // The slot held another fetch constant
  uint64_t fetch_empty_failures_ = 0;       // slot not used yet
  uint64_t fetch_failures_generation_ = 0;  // same fetch, but generation_textures_ changed
  uint64_t fetch_expired_failures_ = 0;    // same fetch, but the texture is due for a check (valid_until)
  std::array<uint64_t, 6> fetch_previous_report_{};
  std::chrono::steady_clock::time_point fetch_report_{};
  uint64_t generation_textures_ = 0;  // changes with every render target copy and every retired image
  // On-disk pipeline cache (LoadCachePipelines and SaveCachePipelines). [dk] There is no driver pipeline cache:
  // the file keeps only the prewarm list (the inventory of DKSH variants), with an empty cache part.
  uint32_t pipelines_no_save_ = 0;
  uint32_t saved_cache_ = 0;
  size_t bytes_cache_saved_ = 0;  // size of the last file read or saved
  uint32_t failed_saves_cache_ = 0;  // saves postponed by sa2_native_pipeline_cache_save_guard
  std::chrono::steady_clock::time_point cache_saved_{};
  // Thread that writes the pipeline cache to disk (WriterCacheMain). It is persistent, because on
  // Horizon detach() closes the game, and wake-ups are decided with the lock held. [dk] It writes the list.
  std::thread writer_cache_;
  std::mutex writer_mutex_;
  std::condition_variable writer_warning_;
  std::vector<uint8_t> writer_data_;
  bool pending_writer_ = false;
  bool writer_stop_ = false;
  bool writer_take_cache_ = false;  // ds2_pipeline_cache_copy_off_ring: the writer takes the cache from the driver
  std::atomic<bool> retry_writer_{false};  // the writer could not take it: the ring thread asks again
  uint32_t timed_saves_cache_ = 0;
  // The single pipelines file. The last written version of each part (or the one read at start-up): owned
  // by the writer thread once it is running, and by Initialize before that. read_list_ goes from
  // LoadCachePipelines to LoadPipelinesList.
  std::vector<uint8_t> written_cache_;
  std::vector<uint8_t> written_list_;
  std::vector<uint8_t> read_list_;
  bool old_files_ = false;  // the two older files were read: deleted when the new one is written
  bool renamed_file_ = false;  // cache/masseffect_native_pipelines.bin was read: deleted when the new one is written
  uint64_t ns_pipelines_ = 0;  // creating pipelines (C6 report)
  std::unordered_set<uint32_t> warned_;
  // Pipeline prewarm (PrewarmedLoop). file_list_ and state_list_ are sized before the thread
  // starts and never change size; the thread writes state_list_[i] before publishing
  // prewarmed_until_ > i. Anything not atomic and not marked otherwise belongs to the ring only.
  std::vector<RegisterPipeline> file_list_;         // the list read at start-up (walked by the thread)
  std::vector<uint8_t> state_list_;                   // kList* of each record of file_list_
  std::vector<uint8_t> other_settings_list_;          // 1: a record of other settings (not prewarmed; kept in the list)
  std::unordered_map<uint64_t, size_t> index_list_;   // XXH3 of the key -> index (SIZE_MAX: from this session)
  std::vector<RegisterPipeline> session_list_;          // this session's new ones
  uint32_t list_no_save_ = 0;
  std::vector<uint8_t> writer_list_;                 // guarded by writer_mutex_
  std::thread prewarmed_thread_;
  std::thread preload_thread_;  // TryPreloadShaders
  std::atomic<bool> preload_stop_{false};
  bool decided_preload_ = false;  // ring only
  const ShadersNative* prewarmed_library_ = nullptr;
  uint32_t prewarmed_eds_ = 0;
  bool prewarmed_decided_ = false;
  bool prewarmed_difference_ = false;
  std::chrono::steady_clock::time_point prewarmed_start_{};
  std::chrono::steady_clock::time_point prewarmed_report_{};
  uint64_t prewarmed_previous_changes_ = 0;
  std::atomic<bool> prewarmed_stop_{false};
  std::atomic<bool> prewarmed_finished_{false};
  std::atomic<size_t> prewarmed_until_{0};
  std::atomic<uint32_t> prewarmed_done_{0};
  std::atomic<uint32_t> prewarmed_compiled_{0};
  std::atomic<uint64_t> prewarmed_ns_compiled_{0};
  std::atomic<uint32_t> prewarmed_no_shader_{0};
  std::atomic<uint32_t> prewarmed_other_mode_{0};
  std::atomic<uint32_t> prewarmed_failed_{0};
  std::atomic<int32_t> prewarmed_priority_{-1};
  uint32_t prewarmed_counted_ = 0;  // of prewarmed_compiled_, already added to pipelines_no_save_
  uint64_t prewarmed_ring_ = 0, prewarmed_slow_ring_ = 0, ns_prewarmed_ring_ = 0;
  uint64_t ring_of_list_ = 0, ns_ring_of_list_ = 0;
  uint64_t new_ring_ = 0, ns_new_ring_ = 0, ring_to_list_ = 0;
