# Warning flags for Hyoshi's own targets. Third-party targets keep their defaults.
# -Wno-missing-field-initializers: Vulkan code relies on `VkFoo info{VK_STRUCTURE_TYPE_FOO};`
# zero-filling the remaining fields.
# -no_warn_duplicate_libraries: CMake repeats static libraries on the link line to satisfy link
# order, and Apple's linker warns about each repeat.
# MSVC: /W4 is its closest match to the Clang set. /wd4324: SpscQueue pads to cache lines on
# purpose. _CRT_SECURE_NO_WARNINGS silences the deprecation warnings for standard C functions
# like std::getenv.
function(hyoshi_set_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wno-missing-field-initializers>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /wd4324>
    )
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(APPLE)
        target_link_options(${target} PRIVATE LINKER:-no_warn_duplicate_libraries)
    endif()
endfunction()
