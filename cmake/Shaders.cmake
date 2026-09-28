# Slang -> SPIR-V at build time. The compiler is a pinned prebuilt release for the host, so no
# manual install is needed. Output targets SPIR-V 1.3, the newest version Vulkan 1.1 accepts.

set(HYOSHI_SLANG_VERSION 2026.18.2)

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(HYOSHI_SLANG_ARCH aarch64)
else()
    set(HYOSHI_SLANG_ARCH x86_64)
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(HYOSHI_SLANG_OS macos)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(HYOSHI_SLANG_OS linux)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(HYOSHI_SLANG_OS windows)
else()
    message(FATAL_ERROR "No pinned Slang release for host ${CMAKE_HOST_SYSTEM_NAME}")
endif()

set(HYOSHI_SLANG_SHA256_macos-aarch64 410c0ed14e231089a58437bb0d8e00389c48e29ed9ffe23e3b04a5c74f1ce809)
set(HYOSHI_SLANG_SHA256_macos-x86_64 d09a2993b4ed6d81c0c19ff8bede27944bf73437d982549bd7b2ac5a1a3202b9)
set(HYOSHI_SLANG_SHA256_linux-x86_64 8a097d4365e1cab10265d0b0d77b461b5d30576f99ddc0521a35723c34cad816)
set(HYOSHI_SLANG_SHA256_linux-aarch64 3bd1fb739b9a7d51bbe9993af087fe9e52f32692cfb322a048c5047b70558547)
set(HYOSHI_SLANG_SHA256_windows-x86_64 81a2e749480b136ab7ee0060f55be561eb19131b69d2f81652b147bcbb7a1865)
set(HYOSHI_SLANG_SHA256_windows-aarch64 42db8014d9b565a4569fe64d2e44e428f5efc54ba4c546bb6b275b5b55f09cd8)

set(HYOSHI_SLANG_PACKAGE slang-${HYOSHI_SLANG_VERSION}-${HYOSHI_SLANG_OS}-${HYOSHI_SLANG_ARCH})
CPMAddPackage(
    NAME slang
    URL https://github.com/shader-slang/slang/releases/download/v${HYOSHI_SLANG_VERSION}/${HYOSHI_SLANG_PACKAGE}.tar.gz
    URL_HASH SHA256=${HYOSHI_SLANG_SHA256_${HYOSHI_SLANG_OS}-${HYOSHI_SLANG_ARCH}}
    DOWNLOAD_ONLY YES
)

find_program(HYOSHI_SLANGC slangc HINTS ${slang_SOURCE_DIR}/bin NO_DEFAULT_PATH REQUIRED)
find_program(HYOSHI_SPIRV_VAL spirv-val HINTS $ENV{VULKAN_SDK}/bin /opt/homebrew/bin /usr/local/bin)

set(HYOSHI_EMBED_SPIRV_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/EmbedSpirv.cmake)

# hyoshi_add_shaders(<target> <file.slang>...)
# Compiles each file (all [shader(...)] entry points, names preserved) to SPIR-V, validates it for
# Vulkan 1.1 when spirv-val is available, and embeds it as a header: Triangle.slang becomes
# "Triangle.spv.h" defining TRIANGLE_SPIRV.
function(hyoshi_add_shaders target)
    set(outputDir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${outputDir})

    foreach(source IN LISTS ARGN)
        get_filename_component(name ${source} NAME_WE)
        string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" symbol ${name})
        string(TOUPPER "${symbol}_SPIRV" symbol)

        set(spirv ${outputDir}/${name}.spv)
        set(header ${outputDir}/${name}.spv.h)

        set(validateCommand)
        if(HYOSHI_SPIRV_VAL)
            set(validateCommand COMMAND ${HYOSHI_SPIRV_VAL} --target-env vulkan1.1 ${spirv})
        endif()

        add_custom_command(
            OUTPUT ${spirv} ${header}
            COMMAND ${HYOSHI_SLANGC} ${source} -target spirv -profile spirv_1_3 -fvk-use-entrypoint-name
                    -warnings-as-errors all -o ${spirv} -depfile ${spirv}.d
            ${validateCommand}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${spirv} -DOUTPUT=${header} -DNAME=${symbol}
                    -P ${HYOSHI_EMBED_SPIRV_SCRIPT}
            DEPENDS ${source} ${HYOSHI_EMBED_SPIRV_SCRIPT}
            DEPFILE ${spirv}.d
            COMMENT "Compiling shader ${name}.slang"
            VERBATIM
        )
        target_sources(${target} PRIVATE ${header})
    endforeach()

    target_include_directories(${target} PRIVATE ${outputDir})
endfunction()
