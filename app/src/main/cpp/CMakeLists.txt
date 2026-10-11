cmake_minimum_required(VERSION 3.22.1)
project(probe C CXX)

add_subdirectory(libadrenotools)

add_library(probe SHARED probe.c)
target_compile_definitions(probe PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(probe adrenotools android log dl)
