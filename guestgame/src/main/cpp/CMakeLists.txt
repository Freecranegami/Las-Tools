cmake_minimum_required(VERSION 3.22.1)
project(guestgame C)

set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(native_app_glue STATIC "${GLUE}/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${GLUE}")

# Builds libmain.so, the default library name a NativeActivity looks for.
add_library(gamemain SHARED game.c)
set_target_properties(gamemain PROPERTIES OUTPUT_NAME main)
target_compile_definitions(gamemain PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_include_directories(gamemain PRIVATE "${GLUE}")
target_link_libraries(gamemain native_app_glue android log vulkan m)
set_target_properties(gamemain PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
