// ds2-deko3d: the output pass without the gamma ramp, the deko3d form of the SDK's guest_output_bilinear_ps (only
// SPIR-V; rewritten from its disassembly). Also the presenter's final bilinear stretch (Presenter kBilinear effect).
// The Vulkan push constants (Presenter::BilinearConstants at offset 16: output offset, 1 / output size) are uniform
// buffer 0 of the fragment stage with the same offsets, as in shaders_dk/sa2_output_ramp_gamma_*.frag.
#version 450

layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Constantes {
  layout(offset = 16) ivec2 desplazamiento;
  layout(offset = 24) vec2 inverso;
} constantes;

layout(location = 0) out vec4 color;

void main() {
  uvec2 pixel = uvec2(ivec2(gl_FragCoord.xy) - constantes.desplazamiento);
  color = vec4(textureLod(origen, (vec2(pixel) + 0.5) * constantes.inverso, 0.0).rgb, 1.0);
}
