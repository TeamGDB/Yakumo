# AsmJit, the machine code emitter behind the runtime's JIT (src/jit.cpp).
# Zlib licence: LICENSE.md. Unmodified sources of the commit named in
# README.md; this file replaces upstream's CMakeLists.txt with the few
# settings the runtime needs.
cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE PSPRECOMP_ASMJIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/asmjit/*.cpp")
list(FILTER PSPRECOMP_ASMJIT_SOURCES EXCLUDE REGEX "_test\\.cpp$")

add_library(psprecomp_asmjit STATIC ${PSPRECOMP_ASMJIT_SOURCES})
target_include_directories(psprecomp_asmjit SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(psprecomp_asmjit PUBLIC cxx_std_20)
# Only the backend of the machine Yakumo runs on. No shm_open: the dual
# mapping that needs it is not used, and Android has none.
target_compile_definitions(psprecomp_asmjit PUBLIC ASMJIT_STATIC ASMJIT_NO_FOREIGN PRIVATE ASMJIT_NO_SHM_OPEN)
set_target_properties(psprecomp_asmjit PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(psprecomp_asmjit PRIVATE /W0)
else()
    target_compile_options(psprecomp_asmjit PRIVATE -w -fno-exceptions)
endif()
