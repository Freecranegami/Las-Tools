# Lossless Scaling frame generation (lsfg-vk, MIT) and its helpers.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(VOLK_STATIC_DEFINES "VK_USE_PLATFORM_ANDROID_KHR" CACHE STRING "" FORCE)

add_subdirectory(third_party/volk EXCLUDE_FROM_ALL)
add_subdirectory(third_party/dxbc EXCLUDE_FROM_ALL)

file(GLOB PEPARSE_SRC "third_party/pe-parse/pe-parser-library/src/*.cpp")
add_library(peparse STATIC ${PEPARSE_SRC})
target_include_directories(peparse PUBLIC third_party/pe-parse/pe-parser-library/include)
set_target_properties(peparse PROPERTIES CXX_STANDARD 17)

add_subdirectory(framegen EXCLUDE_FROM_ALL)

add_library(lsfgcore SHARED probe.cpp glue.cpp extract.cpp trans.cpp)
set_target_properties(lsfgcore PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(lsfgcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsfgcore PRIVATE peparse dxbc lsfg-vk-framegen log nativewindow android)

# volk defines global function pointers named like the Vulkan commands; keep them private so the shim's exported
# vk* functions can never be bound in their place.
set_target_properties(volk PROPERTIES C_VISIBILITY_PRESET hidden)
target_link_options(lsfgcore PRIVATE -Wl,--exclude-libs,ALL -Wl,-Bsymbolic)
