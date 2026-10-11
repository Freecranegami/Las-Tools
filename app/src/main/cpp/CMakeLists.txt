cmake_minimum_required(VERSION 3.22.1)
project(probe C CXX)

add_subdirectory(libadrenotools)

add_library(probe SHARED probe.c)
target_compile_definitions(probe PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(probe adrenotools android log dl)

# Behaves like a game's native library: links libvulkan.so normally (DT_NEEDED).
add_library(guesttest SHARED guesttest.c)
target_link_libraries(guesttest vulkan log)
