// ds2-deko3d: the renderer's own DKSH shaders, embedded. The data comes from shaders_dk/gerar_dksh.py, which the
// build runs (shaders_dk/dkshaders.cmake) over the adapted GLSL in shaders_dk/; the ids are sa2_native_dk_shaders.h.

#include "sa2_native_dk_shaders.h"

namespace sa2::native::dkshaders {
namespace {
#include "sa2_native_dk_shaders_data.inc"
static_assert(sizeof(kSizes) / sizeof(kSizes[0]) == kCount, "sa2_native_dk_shaders.h is out of date: gerar_dksh.py --enum");
}  // namespace

const uint8_t* Data(Id id) { return id < kCount ? kDatas[id] : nullptr; }
size_t Size(Id id) { return id < kCount ? kSizes[id] : 0; }
const char* Name(Id id) { return id < kCount ? kNames[id] : "?"; }

}  // namespace sa2::native::dkshaders
