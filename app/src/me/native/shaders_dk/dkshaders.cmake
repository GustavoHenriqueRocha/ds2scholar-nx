# ds2-deko3d: the renderer's own deko3d shaders (shaders_dk/*.vert|frag|comp) compiled by uam at build time and
# embedded in the target (sa2_native_dk_shaders.cpp includes the generated data). Include this file from
# app/CMakeLists.txt and call ds2_dk_shaders(ds2) on the Switch build.
#   - the adapted GLSL is checked in (adaptar_dk.py, rerun after changing a shader in ../shaders);
#   - gerar_dksh.py applies the exact-division rewrite, compiles, fails on any integer division uam would emulate,
#     and checks that the checked-in enum header sa2_native_dk_shaders.h matches the file list.
find_program(DS2_UAM uam HINTS "$ENV{DEVKITPRO}/tools/bin" /opt/devkitpro/tools/bin)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(ds2_dk_shaders target)
    if(NOT DS2_UAM)
        message(FATAL_ERROR "ds2_dk_shaders: uam not found (devkitPro tools/bin)")
    endif()
    set(_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(_native "${_dir}/..")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/generated-dk-shaders")
    file(GLOB _sources CONFIGURE_DEPENDS "${_dir}/*.vert" "${_dir}/*.frag" "${_dir}/*.comp")
    add_custom_command(OUTPUT "${_out}/sa2_native_dk_shaders_data.inc"
        COMMAND "${Python3_EXECUTABLE}" -I -B "${_dir}/gerar_dksh.py" --uam "${DS2_UAM}" --out "${_out}"
                --check-enum "${_native}/sa2_native_dk_shaders.h"
        DEPENDS ${_sources} "${_dir}/gerar_dksh.py" "${_dir}/divisao_exata.py" "${_native}/sa2_native_dk_shaders.h"
        COMMENT "deko3d: uam over shaders_dk/" VERBATIM)
    add_custom_target(${target}_dk_shaders DEPENDS "${_out}/sa2_native_dk_shaders_data.inc")
    add_dependencies(${target} ${target}_dk_shaders)
    target_sources(${target} PRIVATE "${_native}/sa2_native_dk_shaders.cpp")
    target_include_directories(${target} PRIVATE "${_out}")
endfunction()
