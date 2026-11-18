// SA2 - native renderer: the pure SPIR-V passes of sa2_native_draws.cpp, moved here unchanged so the offline
// deko3d tools (ds2-deko3d tools/shaders/spirv_transformar.cpp) apply exactly the same transforms. Only
// std:: types: no Vulkan, no renderer state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sa2::native {

/*
 * Whether a SPIR-V module tests bit `bit` of specialization constant 0 (an OpSpecConstantOp BitwiseAnd of the SpecId-0
 * constant with an OpConstant of that value): the shader has a variant under that bit. Read once per shader.
 */
inline bool SpirvTestsSpecBit(std::span<const uint32_t> spirv, uint32_t bit) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) return false;
  std::unordered_set<uint32_t> spec_ids, bit_constants;
  std::vector<std::pair<uint32_t, uint32_t>> ands;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16, op = spirv[i] & 0xFFFF;
    if (!words || i + words > spirv.size()) return false;
    if (op == 71 && words == 4 && spirv[i + 2] == 1 && spirv[i + 3] == 0) spec_ids.insert(spirv[i + 1]);  // SpecId 0
    if (op == 43 && words == 4 && spirv[i + 3] == bit) bit_constants.insert(spirv[i + 2]);  // OpConstant
    if (op == 52 && words == 6 && spirv[i + 3] == 199) ands.push_back({spirv[i + 4], spirv[i + 5]});  // BitwiseAnd
    i += words;
  }
  for (const auto& [a, b] : ands)
    if ((spec_ids.count(a) && bit_constants.count(b)) || (spec_ids.count(b) && bit_constants.count(a))) return true;
  return false;
}

// Copy of the SPIR-V without the writes to the color targets (output variables with Location 0..3).
// Returns empty if the module cannot be walked or there was nothing to remove.
inline std::vector<uint32_t> PruneWritesOfColor(const std::vector<uint32_t>& spirv, uint32_t& removed) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59, kOpStore = 62;
  constexpr uint32_t kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30, kDecorationIndex = 29;
  constexpr uint32_t kStoreOutput = 3;
  removed = 0;
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  std::unordered_set<uint32_t> with_location, discarded;
  // 1) decorations: Location 0..3 marks a color target; BuiltIn or Index != 0 rule it out (gl_FragDepth
  // and the second blend source are not touched).
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      return {};
    }
    if (code == kOpDecorate && words >= 3) {
      const uint32_t goal = spirv[i + 1];
      const uint32_t decoration = spirv[i + 2];
      if (decoration == kDecorationLocation && words >= 4 && spirv[i + 3] <= 3) {
        with_location.insert(goal);
      } else if (decoration == kDecorationBuiltIn ||
                 (decoration == kDecorationIndex && words >= 4 && spirv[i + 3] != 0)) {
        discarded.insert(goal);
      }
    }
    i += words;
  }
  // 2) pointers to those targets: the output variable and whatever is derived from it through
  // OpAccessChain.
  std::unordered_set<uint32_t> pointers;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (code == kOpVariable && words >= 4 && spirv[i + 3] == kStoreOutput) {
      const uint32_t id = spirv[i + 2];
      if (with_location.count(id) && !discarded.count(id)) {
        pointers.insert(id);
      }
    } else if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && words >= 4 &&
               pointers.count(spirv[i + 3])) {
      pointers.insert(spirv[i + 2]);
    }
    i += words;
  }
  if (pointers.empty()) {
    return {};
  }
  // 3) the writes to those pointers are dropped. The rest of the module is copied as is: the output
  // variable stays declared and in the entry point's interface, which is valid even if it is never written.
  std::vector<uint32_t> output;
  output.reserve(spirv.size());
  output.insert(output.end(), spirv.begin(), spirv.begin() + 5);
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    const bool outside = code == kOpStore && words >= 3 && pointers.count(spirv[i + 1]);
    if (outside) {
      ++removed;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + words);
    }
    i += words;
  }
  (void)kOpEntryPoint;
  return removed ? output : std::vector<uint32_t>();
}

// Xenos k_2_10_10_10_FLOAT stores unsigned 7e3 RGB ([0, 31.875]) and two-bit UNORM alpha. The native
// renderer represents it with RGBA16F, whose wider range would let negative values, infinities and NaNs
// escape into later passes. Insert GLSL NClamp immediately before stores to the selected color outputs.
inline std::vector<uint32_t> LimitOutputs7e3(const std::vector<uint32_t>& spirv, uint32_t outputs_mask) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpExtInstImport = 11, kOpExtInst = 12, kOpTypeFloat = 22;
  constexpr uint32_t kOpTypeVector = 23, kOpConstant = 43, kOpConstantComposite = 44;
  constexpr uint32_t kOpFunction = 54, kOpVariable = 59, kOpStore = 62, kOpDecorate = 71;
  constexpr uint32_t kDecorationLocation = 30, kStoreOutput = 3, kNClamp = 81;
  if (!outputs_mask || spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  uint32_t glsl = 0, float_type = 0, v_type4 = 0;
  size_t before_functions = 0;
  std::unordered_map<uint32_t, uint32_t> locations;
  std::unordered_set<uint32_t> outputs;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16, code = spirv[i] & 0xFFFF;
    if (!words || i + words > spirv.size()) return {};
    if (code == kOpExtInstImport && words >= 3 && !glsl) glsl = spirv[i + 1];
    if (code == kOpTypeFloat && words >= 3 && spirv[i + 2] == 32) float_type = spirv[i + 1];
    if (code == kOpTypeVector && words >= 4 && spirv[i + 2] == float_type && spirv[i + 3] == 4)
      v_type4 = spirv[i + 1];
    if (code == kOpDecorate && words >= 4 && spirv[i + 2] == kDecorationLocation)
      locations[spirv[i + 1]] = spirv[i + 3];
    if (code == kOpVariable && words >= 4 && spirv[i + 3] == kStoreOutput) {
      const auto l = locations.find(spirv[i + 2]);
      if (l != locations.end() && l->second < 4 && (outputs_mask & (1u << l->second)))
        outputs.insert(spirv[i + 2]);
    }
    if (code == kOpFunction && !before_functions) before_functions = i;
    i += words;
  }
  if (!glsl || !float_type || !v_type4 || !before_functions || outputs.empty()) return {};
  uint32_t next = spirv[3];
  const uint32_t zero = next++, one = next++, max_rgb = next++;
  const uint32_t min = next++, max = next++;
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 40);
  output.insert(output.end(), spirv.begin(), spirv.begin() + before_functions);
  const auto constant = [&](uint32_t id, uint32_t bits) {
    output.insert(output.end(), {(4u << 16) | kOpConstant, float_type, id, bits});
  };
  constant(zero, 0x00000000u);
  constant(one, 0x3F800000u);
  constant(max_rgb, 0x41FF0000u);  // 31.875, largest finite unsigned 7e3 value.
  output.insert(output.end(), {(7u << 16) | kOpConstantComposite, v_type4, min,
                               zero, zero, zero, zero});
  output.insert(output.end(), {(7u << 16) | kOpConstantComposite, v_type4, max,
                               max_rgb, max_rgb, max_rgb, one});
  uint32_t limited = 0;
  for (size_t i = before_functions; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16, code = spirv[i] & 0xFFFF;
    if (!words || i + words > spirv.size()) return {};
    if (code == kOpStore && words >= 3 && outputs.count(spirv[i + 1])) {
      const uint32_t limited_value = next++;
      output.insert(output.end(), {(8u << 16) | kOpExtInst, v_type4, limited_value, glsl,
                                   kNClamp, spirv[i + 2], min, max});
      output.push_back(spirv[i]);
      output.push_back(spirv[i + 1]);
      output.push_back(limited_value);
      ++limited;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + words);
    }
    i += words;
  }
  if (!limited) return {};
  output[3] = next;
  return output;
}

/*
 * The same module, but declaring OpExecutionMode EarlyFragmentTests.
 *
 * In SPIR-V the execution modes have their own section, right after the OpEntryPoint instructions and
 * before the debug strings and decorations. So it is enough to insert the instruction after the module's
 * last OpExecutionMode (there is always at least one, OriginUpperLeft, which is what DXC emits). No new
 * ids are created, so the header's "bound" does not change and the module stays valid.
 *
 * Returns empty (and then the normal module is used) if the module has no fragment entry point, if it
 * already declared it, or if it writes gl_FragDepth (DepthReplacing): in that last case testing earlier
 * would change the result, because the Z being tested is computed by the shader itself.
 */
inline std::vector<uint32_t> WithEarlyTests(const std::vector<uint32_t>& spirv, const char*& reason) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr uint32_t kModelFragment = 4;
  constexpr uint32_t kEarlyMode = 9, kDepthModeReplacing = 12;
  reason = "";
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    reason = "not SPIR-V";
    return {};
  }
  uint32_t entry = 0;
  size_t where = 0;  // first word after the execution mode section
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      reason = "malformed module";
      return {};
    }
    if (code == kOpEntryPoint && words >= 3) {
      if (spirv[i + 1] == kModelFragment) {
        if (entry) {
          reason = "more than one fragment entry point";
          return {};
        }
        entry = spirv[i + 2];
      }
      if (where < i + words) {
        where = i + words;
      }
    } else if (code == kOpExecutionMode && words >= 3) {
      if (spirv[i + 2] == kEarlyMode) {
        reason = "already declared";
        return {};
      }
      if (spirv[i + 2] == kDepthModeReplacing) {
        reason = "escribe gl_FragDepth";
        return {};
      }
      if (where < i + words) {
        where = i + words;
      }
    }
    i += words;
  }
  if (!entry || !where) {
    reason = "no fragment entry point";
    return {};
  }
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 3);
  output.insert(output.end(), spirv.begin(), spirv.begin() + where);
  output.push_back((3u << 16) | kOpExecutionMode);
  output.push_back(entry);
  output.push_back(kEarlyMode);
  output.insert(output.end(), spirv.begin() + where, spirv.end());
  return output;
}

}  // namespace sa2::native
