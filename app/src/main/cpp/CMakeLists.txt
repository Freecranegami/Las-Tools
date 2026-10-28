cmake_minimum_required(VERSION 3.22.1)
project(probe C CXX)

add_subdirectory(libadrenotools)

add_library(probe SHARED probe.c)
target_compile_definitions(probe PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(probe adrenotools android log dl)

# Behaves like a game's native library: links libvulkan.so normally (DT_NEEDED).
add_library(guesttest SHARED guesttest.c)
target_link_libraries(guesttest vulkan log)

# Vulkan forwarding shim, generated from the NDK's own Vulkan headers.
if(CMAKE_SYSROOT)
    set(VK_SYSROOT "${CMAKE_SYSROOT}")
else()
    set(VK_SYSROOT "${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/sysroot")
endif()
set(SHIM_C "${CMAKE_CURRENT_BINARY_DIR}/vkshim_gen.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/gen_shim.py")
execute_process(
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/gen_shim.py"
            "${VK_SYSROOT}/usr/include/vulkan" "${SHIM_C}"
    RESULT_VARIABLE GEN_RES
    OUTPUT_VARIABLE GEN_OUT
    ERROR_VARIABLE GEN_ERR)
if(NOT GEN_RES EQUAL 0)
    message(FATAL_ERROR "Vulkan shim generation failed: ${GEN_ERR} ${GEN_OUT}")
endif()
add_library(vkshim SHARED "${SHIM_C}" cas.c sgsr.c)
target_compile_definitions(vkshim PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(vkshim dl log m)
