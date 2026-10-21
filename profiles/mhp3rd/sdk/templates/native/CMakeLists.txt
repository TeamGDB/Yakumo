# A Yakumo native mod. Copy this folder to start one of your own: rename the
# project and MOD_LIBRARY, and give mod.ini's [Native] Library the same name.
#
#   cmake -S . -B build -DYAKUMO_SDK_DIR=<Yakumo>/profiles/mhp3rd/sdk/include
#   cmake --build build --config Release
#   cmake --install build --config Release --prefix <mods folder>/<Your Mod>
#
# The install step lays the mod folder out as Yakumo reads it: mod.ini at the
# top and the library in native/<platform>/. Build once per platform into the
# same folder to make a mod that runs on all of them.
cmake_minimum_required(VERSION 3.20)
project(example_mod LANGUAGES CXX)

set(MOD_LIBRARY example_mod)

# The folder that holds yakumo/mod.h. Inside Yakumo's sources it is found on
# its own; elsewhere, point it at a copy of profiles/mhp3rd/sdk/include.
set(YAKUMO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include" CACHE PATH "Folder holding yakumo/mod.h")
if(NOT EXISTS "${YAKUMO_SDK_DIR}/yakumo/mod.h")
    message(FATAL_ERROR "yakumo/mod.h is not in YAKUMO_SDK_DIR (${YAKUMO_SDK_DIR}). Set it to the "
        "profiles/mhp3rd/sdk/include folder of Yakumo's sources.")
endif()

# The platform folder, named as Yakumo names it.
if(ANDROID)
    set(_yakumo_platform android-arm64)
elseif(WIN32)
    set(_yakumo_platform windows-x64)
elseif(APPLE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"))
        set(_yakumo_platform macos-x64)
    else()
        set(_yakumo_platform macos-arm64)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_yakumo_platform linux-arm64)
else()
    set(_yakumo_platform linux-x64)
endif()
set(YAKUMO_PLATFORM "${_yakumo_platform}" CACHE STRING "Yakumo's name for the target platform")

add_library(${MOD_LIBRARY} MODULE src/mod.cpp)
target_compile_features(${MOD_LIBRARY} PRIVATE cxx_std_17)
target_include_directories(${MOD_LIBRARY} PRIVATE "${YAKUMO_SDK_DIR}")
# Only yakumo_mod_main is exported; the library carries its own C++ runtime
# where the platform allows, so it does not depend on Yakumo's.
set_target_properties(${MOD_LIBRARY} PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(APPLE)
    set_target_properties(${MOD_LIBRARY} PROPERTIES SUFFIX ".dylib")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    target_link_options(${MOD_LIBRARY} PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(FILES mod.ini DESTINATION .)
install(TARGETS ${MOD_LIBRARY} LIBRARY DESTINATION "native/${YAKUMO_PLATFORM}")
