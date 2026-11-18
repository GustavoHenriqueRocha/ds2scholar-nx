#version 450
// ds2-deko3d: the float24 depth-only stage (no pixel shader, kSpecDepthFloat24Quantize, round = 1): SPIR-V from
// me::native::MakeDepthOnlyFragmentForQuantization + TransformDepthHalf + TransformDepthQuantizeIncoming(round = 1)
// (ds2-deko3d tools/shaders/spirv_transformar --so-profundidade out.spv 1), GLSL by SPIRV-Cross --vulkan-semantics.
// Regenerate the same way if me_depth_quantize_spirv.h changes; do not edit.

void main()
{
    float _34 = gl_FragCoord.z * 2.0;
    uint _35 = floatBitsToUint(_34);
    uint _37 = 113u - (_35 >> 23u);
    uint _45 = (_35 < 947912704u) ? (((_35 & 8388607u) | 8388608u) >> ((_37 < 24u) ? _37 : 24u)) : (_35 + 3355443200u);
    uint _55 = (_34 > 0.0) ? ((_35 >= 1073741816u) ? 16777215u : (((_45 + (3u + ((_45 >> 3u) & 1u))) >> 3u) & 16777215u)) : 0u;
    gl_FragDepth = ((_55 < 1048576u) ? (float(_55) * 5.82076609134674072265625e-11) : uintBitsToFloat((_55 << 3u) + 939524096u)) * 0.5;
}

