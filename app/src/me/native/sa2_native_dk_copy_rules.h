#pragma once
// ds2-deko3d: the decisions of two copy experiments of TargetsDk::CopyInternal (sa2_native_targets_dk_copias.cpp),
// kept free of deko3d so a host test can check them: tools/tests/dk_copy_rules_test.cpp.
//  - ds2_skip_repeated_color_resolve: a color resolve that repeats the previous one into the same texture with nothing
//    written since is skipped (the color mirror of ds2_skip_repeated_depth_resolve);
//  - ds2_copy_dma_as_draw: a same-format RGBA16F color resolve, which ResolveColorFrag declined (RGBA8 only) and so
//    went to the copy engine (dkCmdBufCopyImage), is drawn on the 3D engine through RGBA16_Uint views (raw bits).

#include <cstdint>

namespace sa2::native {

// What a color resolve read and wrote, and the counters that move with every write of a render target (EDRAM mode 4:
// draws, EDRAM transfers, restores, tile ownership version). Plain integers: the image ids are sa2::dk::ImageId.
struct ColorResolveKey {
  uint32_t source = 0;         // the image read (the render target view, or its 7e3 owner for the direct path)
  uint32_t render_target = 0;  // the render target view the game resolves
  uint32_t target = 0;         // the resolved texture's image
  uint32_t address = 0;        // the resolved texture's address
  int32_t x0 = 0, y0 = 0;
  uint32_t dx = 0, dy = 0, width = 0, height = 0;
  uint32_t path = 0;  // ColorResolvePath
  int32_t exp_bias = 0;
  uint32_t source_format = 0, target_format = 0;  // guest render target format, guest texture format
  uint64_t drawn = 0, transfers = 0, restored = 0, edram_version = 0;
  uint64_t revision = 0;  // the texture's revision right after the real resolve

  // Everything but the revision (that one is compared against the texture's current revision).
  bool SameAs(const ColorResolveKey& o) const {
    return source == o.source && render_target == o.render_target && target == o.target && address == o.address &&
           x0 == o.x0 && y0 == o.y0 && dx == o.dx && dy == o.dy && width == o.width && height == o.height &&
           path == o.path && exp_bias == o.exp_bias && source_format == o.source_format &&
           target_format == o.target_format && drawn == o.drawn && transfers == o.transfers &&
           restored == o.restored && edram_version == o.edram_version;
  }
};

enum ColorResolvePath : uint32_t {
  kColorResolveDirect7e3 = 1,  // ds2_resolve_7e3_direct (ResolverWithBias, pipeline 10)
  kColorResolveBias = 2,       // exponent bias (ResolverWithBias)
  kColorResolveCopy = 3,       // same format, 1 to 1 (3D-engine draw, stretch blit or copy engine)
};

// When a repeated resolve may be skipped at all. Mode 4 only: there every write of a render target moves a counter of
// the key (the other modes have writes no counter sees, as MakeAlias says). Never for a multisampled source, a texture
// whose bytes still live in another one (alias pending) or that has a deferred front-buffer copy pending, a resolve
// the guest reads back right after (ResolvedRead: its memory must be written again), or a render target whose content
// is not valid.
struct ColorResolveGate {
  bool enabled = false;  // ds2_skip_repeated_color_resolve
  bool mode4 = false;    // sa2_native_edram_alias_mode == 4
  bool multisampled = false;
  bool alias_pending = false;
  bool front_pending = false;
  bool reads_back = false;
  bool invalid_content = false;
};
inline bool MaySkipColorResolve(const ColorResolveGate& g) {
  return g.enabled && g.mode4 && !g.multisampled && !g.alias_pending && !g.front_pending && !g.reads_back &&
         !g.invalid_content;
}

// The resolve is skipped when the previous real resolve into this address (last, if any) is the same resolve and the
// texture has not been written since (its revision is still the one that resolve left).
inline bool RepeatedColorResolve(const ColorResolveGate& gate, const ColorResolveKey* last,
                                 const ColorResolveKey& now, uint64_t revision_now) {
  return MaySkipColorResolve(gate) && last && last->SameAs(now) && last->revision == revision_now;
}

// The texel classes ResolveColorFrag tells apart (host formats of the render target and of the resolved texture).
enum class CopyTexel : uint32_t { kOther, kRgba8Unorm, kRgba16Float };

enum class ResolveDrawKind : uint32_t {
  kNone,     // not drawn: the caller copies on the copy engine, as before
  kFloat,    // sa2_resolve_copy.frag on the formats' own views (RGBA8: n / 255 round-trips exactly)
  kRaw64,    // ds2_resolve_copy_raw64.frag on RGBA16_Uint views of both images: the 64 bits of each texel as they are
};

// dma_as_draw: ds2_copy_dma_as_draw; raw64: RGBA16_Uint views exist (raw64_edram_supported_). With dma_as_draw off
// the answer is the old one (RGBA8 to RGBA8 only).
inline ResolveDrawKind ColorResolveDrawKind(CopyTexel source, CopyTexel target, bool dma_as_draw, bool raw64) {
  if (source == CopyTexel::kRgba8Unorm && target == CopyTexel::kRgba8Unorm) return ResolveDrawKind::kFloat;
  if (dma_as_draw && raw64 && source == CopyTexel::kRgba16Float && target == CopyTexel::kRgba16Float)
    return ResolveDrawKind::kRaw64;
  return ResolveDrawKind::kNone;
}

}  // namespace sa2::native
