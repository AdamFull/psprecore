# Slang shaders are compiled to SPIR-V at build time on the host (slangc from the Vulkan SDK) and embedded in
# the executable, so no shader compiler is needed on the target device.

set(_FF7CC_EMBED_BINARY "${CMAKE_CURRENT_LIST_DIR}/EmbedBinary.cmake")
find_program(FF7CC_SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)

# ff7cc_add_shader(<target> <file.slang> <entry> <stage> <name>): adds <name>.h defining
# `alignas(4) inline constexpr unsigned char <name>[]` to the target's sources and include path.
function(ff7cc_add_shader target source entry stage name)
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    set(spv ${out_dir}/${name}.spv)
    set(header ${out_dir}/${name}.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${FF7CC_SLANGC} ${source} -target spirv -profile spirv_1_3 -entry ${entry} -stage ${stage} -O2 -o ${spv}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${header} -DNAME=${name}
                -P ${_FF7CC_EMBED_BINARY}
        DEPENDS ${source} ${_FF7CC_EMBED_BINARY}
        COMMENT "Compiling ${name} (${entry}, ${stage})"
        VERBATIM)
    target_sources(${target} PRIVATE ${header})
    target_include_directories(${target} PRIVATE ${out_dir})
endfunction()
