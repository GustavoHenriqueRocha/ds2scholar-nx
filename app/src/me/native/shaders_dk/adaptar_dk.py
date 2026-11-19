#!/usr/bin/env python3
"""ds2-deko3d: the renderer's own Vulkan GLSL (../shaders) adapted for uam (deko3d), one file per module.

The Vulkan GLSL stays the only source: rerun this after changing a shader in ../shaders and commit the output next
to it (the build, gerar_dksh.py, only compiles what is here). What changes, mechanically:
  - `set = N,` goes (deko3d has one binding space per kind: uniform buffers 0-15, textures 0-31, images 0-7,
    storage buffers 0-15, so the binding numbers stay);
  - `layout(push_constant) uniform X` -> `layout(std140, binding = 0) uniform X` (no push constant block has an
    array, so std140 and std430 give the same offsets; the host pushes the same struct);
  - `layout(constant_id = N) const T x = v;` -> `#define x v` (one module per combination: see MODULES);
  - `gl_VertexIndex` -> `gl_VertexID` (only me_edram_import: draw(3) with no base vertex);
  - separate `texture2D` + `sampler` -> one `sampler2D` (only sa2_output_ramp_gamma);
  - `packed` (a reserved word in Mesa's GLSL) -> `packed_value`;
  - the CMake -D defines -> `#define` after `#version` (uam has no -D);
  - every integer / and % -> ds2_udiv / ds2_umod (divisao_exata.py: uam's division is approximate).
Usage: adaptar_dk.py [--check]   (--check: exit 1 if any output would change)
"""
import os
import re
import sys

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import divisao_exata  # noqa: E402

FONTES = os.path.join(AQUI, "..", "shaders")
R11 = "r11f_g11f_b10f"

# (module name, source in ../shaders, defines). The base list is app/CMakeLists.txt's foreach(_shader ...) plus
# me_edram_depth_1x_to_depth_msaa2 (outside the CMake list: compiled for completeness, unused by the Vulkan code) and
# ds2_resolve_copy_raw64 (deko3d only: ds2_copy_dma_as_draw);
# the _r11 ones are DS2_R11_VARIANTS; the ramp ones are the 8 combinations the output pass can ask for
# (sa2_native_targets.cpp:1729-1737: exact = index bit 0, FXAA = index bit 1, quality = ds2_output_fxaa_quality;
# kGraduacion is always false there).
BASE = """
sa2_edram_7e3_to_rgba8.comp sa2_edram_rgba8_to_7e3.comp sa2_edram_16f_to_16f.comp me_resolve_exp_bias.comp
me_resolve_exp_bias_rgba8.comp ds2_resolve_7e3_as_unorm10.comp me_edram_depth_to_rgba8.comp me_edram_depth_to_16f.comp
me_edram_import.vert me_edram_color_to_depth.frag me_edram_depth_to_depth.frag me_edram_depth_to_stencil.frag
me_edram_color_to_stencil.frag me_edram_depth_to_depth_msaa2.frag me_edram_depth_msaa2_to_depth_1x.frag
me_edram_depth_to_stencil_msaa2.frag me_edram_depth_msaa2_to_stencil_1x.frag me_edram_raw64_to_rgba8.comp
me_edram_rgba8_to_raw64.comp me_edram_raw64_to_16f.comp me_edram_16f_to_raw64.comp me_edram_raw64_to_raw64.comp
me_edram_depth_to_raw64.comp me_edram_raw64_to_depth.frag me_depth_resolve_guestspace.comp
me_depth_resolve_guestspace_msaa2.comp me_edram_stencil_to_buffer.comp me_edram_stencil_msaa2_to_buffer.comp
me_edram_color_stencil_to_buffer.comp me_edram_color_to_color.frag me_edram_r64_to_r64.frag sa2_resolve_copy.frag
sa2_depth_resolve_ms.frag me_edram_depth_1x_to_depth_msaa2.frag ds2_resolve_copy_raw64.frag
""".split()
MODULES = [(f.rsplit(".", 1)[0], f, []) for f in BASE]
MODULES += [
    ("sa2_edram_7e3_to_rgba8_r11", "sa2_edram_7e3_to_rgba8.comp", [("SRC_FMT", R11)]),
    ("sa2_edram_rgba8_to_7e3_r11", "sa2_edram_rgba8_to_7e3.comp", [("DST_FMT", R11)]),
    ("me_resolve_exp_bias_r11", "me_resolve_exp_bias.comp", [("SRC_FMT", R11)]),
    ("me_resolve_exp_bias_rgba8_r11", "me_resolve_exp_bias_rgba8.comp", [("SRC_FMT", R11)]),
    ("ds2_resolve_7e3_as_unorm10_r11", "ds2_resolve_7e3_as_unorm10.comp", [("SRC_FMT", R11)]),
    ("sa2_edram_16f_to_16f_r11src", "sa2_edram_16f_to_16f.comp", [("SRC_FMT", R11)]),
    ("sa2_edram_16f_to_16f_r11dst", "sa2_edram_16f_to_16f.comp", [("DST_FMT", R11)]),
    ("sa2_edram_16f_to_16f_r11both", "sa2_edram_16f_to_16f.comp", [("SRC_FMT", R11), ("DST_FMT", R11)]),
    ("me_edram_16f_to_raw64_r11", "me_edram_16f_to_raw64.comp", [("SRC_FMT", R11)]),
    ("me_edram_raw64_to_16f_r11", "me_edram_raw64_to_16f.comp", [("DST_FMT", R11)]),
    ("me_edram_depth_to_16f_r11", "me_edram_depth_to_16f.comp", [("DST_FMT", R11)]),
]
for _e in (0, 1):
    for _f in (0, 1):
        for _q in (0, 1):
            MODULES.append((f"sa2_output_ramp_gamma_e{_e}_f{_f}_q{_q}", "sa2_output_ramp_gamma.frag",
                            [("kExacta", "true" if _e else "false"), ("kGraduacion", "false"),
                             ("kFxaa", "true" if _f else "false"), ("kFxaaCalidad", "true" if _q else "false")]))


def adaptar(texto, defines, origem):
    t = re.sub(r"set\s*=\s*\d+\s*,\s*", "", texto)
    t = re.sub(r"layout\(push_constant\)\s*uniform", "layout(std140, binding = 0) uniform", t)
    specs = re.findall(r"layout\(constant_id\s*=\s*\d+\)\s*const\s+\w+\s+(\w+)\s*=\s*([^;]+);", t)
    t = re.sub(r"layout\(constant_id\s*=\s*\d+\)\s*const\s+\w+\s+\w+\s*=\s*[^;]+;\n", "", t)
    t = re.sub(r"\bgl_VertexIndex\b", "gl_VertexID", t)
    t = re.sub(r"\bpacked\b", "packed_value", t)
    t, n = re.subn(r"layout\(binding = (\d+)\) uniform texture2D (\w+);\nlayout\(binding = \d+\) uniform sampler (\w+);\n",
                   r"layout(binding = \1) uniform sampler2D \2;\n", t)
    if n:
        t = re.sub(r"sampler2D\((\w+),\s*\w+\)", r"\1", t)
    if "push_constant" in t or "constant_id" in t or re.search(r"\bset\s*=", t):
        raise ValueError(f"{origem}: something Vulkan-only is left")
    linhas = t.split("\n")
    v = next(i for i, l in enumerate(linhas) if l.startswith("#version"))
    dados = dict(defines)
    extra = [f"// ds2-deko3d: generated by shaders_dk/adaptar_dk.py from shaders/{origem}; do not edit."]
    extra += [f"#define {k} {val}" for k, val in defines]
    extra += [f"#define {nome} {val.strip()}" for nome, val in specs if nome not in dados]
    t = "\n".join(linhas[:v + 1] + extra + linhas[v + 1:])
    t, _ = divisao_exata.trocar(t)
    return t


def main():
    check = "--check" in sys.argv
    mudou = []
    for nome, fonte, defines in MODULES:
        with open(os.path.join(FONTES, fonte)) as f:
            novo = adaptar(f.read(), defines, fonte)
        destino = os.path.join(AQUI, nome + "." + fonte.rsplit(".", 1)[1])
        velho = open(destino).read() if os.path.exists(destino) else None
        if velho != novo:
            mudou.append(destino)
            if not check:
                with open(destino, "w") as f:
                    f.write(novo)
    if check and mudou:
        print("adaptar_dk.py: out of date:", *mudou, sep="\n  ")
        sys.exit(1)
    print(f"adaptar_dk.py: {len(MODULES)} modules, {len(mudou)} {'stale' if check else 'written'}")


if __name__ == "__main__":
    main()
