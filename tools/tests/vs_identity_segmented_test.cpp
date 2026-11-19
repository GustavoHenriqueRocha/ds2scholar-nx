// VS identity predicate by runs of words (ds2_vs_identity_segmented, app/src/me/me_shader_identity.h): the same answer
// as VertexShaderIdentityMatches for every input.
// Build: g++ -std=c++23 -O2 -Iapp/src/me tools/tests/vs_identity_segmented_test.cpp
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

#include "me_shader_identity.h"

namespace {
using me::native::ShaderIdentityStage;
using me::native::ShaderIdentityView;

struct Fetch {
  uint32_t instruction;
};

bool Old(const std::vector<uint32_t>& loaded, const std::vector<uint32_t>& selected, const std::vector<Fetch>& f,
         ShaderIdentityStage ls = ShaderIdentityStage::Vertex, ShaderIdentityStage ss = ShaderIdentityStage::Vertex) {
  return me::native::VertexShaderIdentityMatches({ls, loaded}, {ss, selected}, f,
                                                 [](const Fetch& e) { return e.instruction; });
}
bool New(const std::vector<uint32_t>& loaded, const std::vector<uint32_t>& selected, const std::vector<Fetch>& f,
         ShaderIdentityStage ls = ShaderIdentityStage::Vertex, ShaderIdentityStage ss = ShaderIdentityStage::Vertex) {
  return me::native::VertexShaderIdentityMatchesSegmented({ls, loaded}, {ss, selected}, f,
                                                          [](const Fetch& e) { return e.instruction; });
}
}  // namespace

int main() {
  constexpr uint32_t masks[3]{0x0007FFFFu, 0x80000FFFu, 0x80000000u};
  // A 4-instruction program with fetches at instructions 1 and 3 (words 3-5 and 9-11), selected words normalized.
  std::vector<uint32_t> sel(12);
  for (size_t w = 0; w < sel.size(); ++w) sel[w] = 0x11111111u * uint32_t(w + 1);
  for (uint32_t i : {1u, 3u}) {
    sel[i * 3] = 0x00012340u & masks[0];  // opcode bits 0-4 zero: a vertex fetch
    sel[i * 3 + 1] = 0x80000688u;         // swizzle 0x688 (xyzw)
    sel[i * 3 + 2] = 0x80000000u;
  }
  const std::vector<Fetch> fetches{{3}, {1}};
  std::vector<uint32_t> ld = sel;
  assert(Old(ld, sel, fetches) && New(ld, sel, fetches));  // identical
  ld[4] = (ld[4] & 0x80000000u) | 0x40000000u | 0x688u;    // fetch lane 1: loader-patched bits outside the mask
  ld[3] |= 0xFFF80000u & ~0x1Fu;                           // lane 0: bits outside its mask
  assert(Old(ld, sel, fetches) && New(ld, sel, fetches));
  ld[10] ^= 0x80000000u;  // lane 1 bit 31 differs: rejected
  assert(!Old(ld, sel, fetches) && !New(ld, sel, fetches));
  ld = sel;
  ld[7] ^= 1;  // an ALU word between the triples
  assert(!Old(ld, sel, fetches) && !New(ld, sel, fetches));
  ld = sel;
  ld[0] ^= 1;  // the first word
  assert(!Old(ld, sel, fetches) && !New(ld, sel, fetches));
  ld = sel;
  ld.push_back(0);  // different size
  assert(!Old(ld, sel, fetches) && !New(ld, sel, fetches));
  ld = sel;
  assert(!Old(ld, sel, {{4}}) && !New(ld, sel, {{4}}));  // fetch past the end
  assert(!Old(ld, sel, fetches, ShaderIdentityStage::Pixel) && !New(ld, sel, fetches, ShaderIdentityStage::Pixel));

  // Random programs: both predicates on the same inputs, many of them true and many false.
  std::mt19937_64 rng(20261119);
  auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };
  uint64_t trues = 0, falses = 0;
  for (int iter = 0; iter < 400000; ++iter) {
    const uint32_t instructions = 1 + pick(90);
    std::vector<uint32_t> s(instructions * 3 + (pick(8) == 0 ? pick(3) : 0));  // sometimes not a multiple of 3
    for (uint32_t& w : s) w = uint32_t(rng());
    std::vector<Fetch> f;
    const uint32_t nf = pick(12);
    for (uint32_t k = 0; k < nf; ++k) {
      uint32_t i = pick(instructions + (pick(30) == 0 ? 2 : 0));  // rarely out of bounds
      if (!f.empty() && pick(10) == 0) i = f[pick(uint32_t(f.size()))].instruction;  // duplicates
      f.push_back({i});
    }
    for (const Fetch& e : f) {
      const size_t st = size_t(e.instruction) * 3;
      if (st + 3 > s.size() || pick(40) == 0) continue;  // sometimes left unnormalized
      s[st] &= masks[0] & ~0x1Fu;
      s[st + 1] = (s[st + 1] & 0x80000000u) | (pick(2) ? 0x688u : uint32_t(rng()) & 0xFFFu);
      s[st + 2] &= masks[2];
    }
    std::vector<uint32_t> l = s;
    const uint32_t edits = pick(4);
    for (uint32_t k = 0; k < edits; ++k) {
      const size_t w = pick(uint32_t(l.size()));
      switch (pick(4)) {
        case 0: l[w] ^= 1u << pick(32); break;  // any bit
        case 1: l[w] |= 0xFFF80000u; break;     // outside lane 0's mask (and inside others)
        case 2: l[w] = (l[w] & 0x80000000u) | (uint32_t(rng()) & 0x7FFFFFFFu); break;  // all but bit 31
        default: l[w] = (l[w] & ~0xFFFu) | 0x688u; break;  // swizzle field
      }
    }
    if (pick(50) == 0 && !l.empty()) l.pop_back();
    const bool expected = Old(l, s, f);
    const bool got = New(l, s, f);
    if (expected != got) {
      std::printf("MISMATCH iter %d: old %d new %d (words %zu fetches %zu)\n", iter, expected, got, s.size(),
                  f.size());
      return 1;
    }
    ++(expected ? trues : falses);
  }
  assert(trues > 10000 && falses > 10000);
  // With an ALU tolerance the segmented one calls the original.
  me::native::g_vs_identity_alu_tolerance.store(2);
  ld = sel;
  ld[7] ^= 1;
  assert(Old(ld, sel, fetches) && New(ld, sel, fetches));
  me::native::g_vs_identity_alu_tolerance.store(0);
  std::printf("vs_identity_segmented_test: ok (%llu equal true, %llu equal false)\n", (unsigned long long)trues,
              (unsigned long long)falses);
  return 0;
}
