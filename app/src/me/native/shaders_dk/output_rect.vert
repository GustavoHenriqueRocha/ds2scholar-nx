// ds2-deko3d: the output pass's vertex shader, the deko3d form of the SDK's guest_output_triangle_strip_rect_vs (only
// SPIR-V in sdk/src/ui/shaders/vulkan_spirv; rewritten from its disassembly): a 4-vertex triangle strip covering the
// rectangle offset .. offset + size in NDC. Vertex i -> (i & 1, (i >> 1) & 1). The Vulkan push constant block
// (GuestOutputPaintRectangleConstants, 16 bytes at offset 0) is uniform buffer 0 of the vertex stage.
#version 450

layout(std140, binding = 0) uniform Rectangle {
  vec2 offset;
  vec2 size;
} rectangle;

void main() {
  uvec2 corner = (uvec2(uint(gl_VertexID)) >> uvec2(0u, 1u)) & uvec2(1u);
  vec2 position = rectangle.offset + vec2(corner) * rectangle.size;
  gl_Position = vec4(position, 0.0, 1.0);
}
