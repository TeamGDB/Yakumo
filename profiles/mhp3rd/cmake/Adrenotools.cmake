# The project above only declares CXX: adrenotools' own CMakeLists.txt (below)
# is the first to need C, with a fresh project(... LANGUAGES CXX C), and the
# NDK's toolchain file only wires up a language's flags and sysroot at the
# project() call that first enables it. Enabling C here, before that nested
# project() runs, instead of leaving the NDK to wire it up lazily and
# incompletely there, is what a plain configure needs to generate rather than
# failing with "CMAKE_C_COMPILE_OBJECT" unset.
enable_language(C)

# A custom Vulkan ICD the player can pick instead of the phone's own vendor
# driver (a Turnip/Mesa build for their Adreno GPU), as Winlator, Skyline and
# yuzu-Android already let players do. adrenotools (github.com/bylaws/
# libadrenotools) opens a dlopen-like handle for libvulkan.so whose driver is
# redirected to a chosen file, by loading a small hook library into an
# isolated linker namespace; liblinkernsbypass, its own git submodule, does
# the namespace trick the hook needs. Android only (host/platform/
# android_gpu_driver.cpp, host/gpu/vulkan_renderer.cpp).
#
# Pinned source tarballs with a checked SHA-256, not a live git submodule
# checkout, so a build is reproducible the way this project's other fetched
# sources are (see FFmpeg.cmake). liblinkernsbypass is adrenotools' own
# submodule; its tarball is extracted into adrenotools' lib/linkernsbypass,
# exactly where adrenotools' own CMakeLists.txt (unmodified, add_subdirectory
# ()'d below) expects to find it.
set(MHP3RD_ADRENOTOOLS_COMMIT 8fae8ce254dfc1344527e05301e43f37dea2df80)
set(MHP3RD_ADRENOTOOLS_URL
    "https://github.com/bylaws/libadrenotools/archive/${MHP3RD_ADRENOTOOLS_COMMIT}.tar.gz")
set(MHP3RD_ADRENOTOOLS_SHA256 ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb)
set(MHP3RD_LINKERNSBYPASS_COMMIT aa3975893d83ef1bc84c321ec60c65fbf1287887)
set(MHP3RD_LINKERNSBYPASS_URL
    "https://github.com/bylaws/liblinkernsbypass/archive/${MHP3RD_LINKERNSBYPASS_COMMIT}.tar.gz")
set(MHP3RD_LINKERNSBYPASS_SHA256 da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638)

set(MHP3RD_ADRENOTOOLS_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/downloads" CACHE PATH
    "Where the adrenotools and liblinkernsbypass archives are downloaded to, or found without downloading")
set(_mhp3rd_adrenotools_root "${CMAKE_BINARY_DIR}/_deps/adrenotools")
set(_mhp3rd_adrenotools_src "${_mhp3rd_adrenotools_root}/libadrenotools-${MHP3RD_ADRENOTOOLS_COMMIT}")

# Downloads <url> to <name> in MHP3RD_ADRENOTOOLS_DOWNLOAD_DIR unless a copy
# with the pinned SHA-256 is already there; sets <out> to its path.
function(_mhp3rd_adrenotools_fetch out name url sha256)
    set(archive "${MHP3RD_ADRENOTOOLS_DOWNLOAD_DIR}/${name}")
    if(EXISTS "${archive}")
        file(SHA256 "${archive}" existing)
        if(existing STREQUAL sha256)
            set(${out} "${archive}" PARENT_SCOPE)
            return()
        endif()
    endif()
    message(STATUS "mhp3rd: downloading ${url}")
    file(DOWNLOAD "${url}" "${archive}.part" STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${archive}.part")
        message(FATAL_ERROR "mhp3rd: could not download ${url}: ${status}\n"
            "Put the file in ${MHP3RD_ADRENOTOOLS_DOWNLOAD_DIR} by hand.")
    endif()
    file(SHA256 "${archive}.part" actual)
    if(NOT actual STREQUAL sha256)
        file(REMOVE "${archive}.part")
        message(FATAL_ERROR "mhp3rd: ${url} has SHA-256 ${actual}, expected ${sha256}")
    endif()
    file(RENAME "${archive}.part" "${archive}")
    set(${out} "${archive}" PARENT_SCOPE)
endfunction()

# What the extracted tree was made from. The directory is named after the
# adrenotools commit alone, so a bumped liblinkernsbypass pin must show up
# here, or the old sources would be built silently.
set(_mhp3rd_adrenotools_stamp
    "${MHP3RD_ADRENOTOOLS_COMMIT} ${MHP3RD_ADRENOTOOLS_SHA256} ${MHP3RD_LINKERNSBYPASS_COMMIT} ${MHP3RD_LINKERNSBYPASS_SHA256}\n")
set(_mhp3rd_adrenotools_stamped "")
if(EXISTS "${_mhp3rd_adrenotools_src}/yakumo.stamp")
    file(READ "${_mhp3rd_adrenotools_src}/yakumo.stamp" _mhp3rd_adrenotools_stamped)
endif()
if(NOT _mhp3rd_adrenotools_stamped STREQUAL _mhp3rd_adrenotools_stamp)
    _mhp3rd_adrenotools_fetch(_mhp3rd_adrenotools_archive "libadrenotools-${MHP3RD_ADRENOTOOLS_COMMIT}.tar.gz"
        "${MHP3RD_ADRENOTOOLS_URL}" "${MHP3RD_ADRENOTOOLS_SHA256}")
    _mhp3rd_adrenotools_fetch(_mhp3rd_linkernsbypass_archive
        "liblinkernsbypass-${MHP3RD_LINKERNSBYPASS_COMMIT}.tar.gz"
        "${MHP3RD_LINKERNSBYPASS_URL}" "${MHP3RD_LINKERNSBYPASS_SHA256}")
    file(REMOVE_RECURSE "${_mhp3rd_adrenotools_src}")
    file(ARCHIVE_EXTRACT INPUT "${_mhp3rd_adrenotools_archive}" DESTINATION "${_mhp3rd_adrenotools_root}")
    file(ARCHIVE_EXTRACT INPUT "${_mhp3rd_linkernsbypass_archive}" DESTINATION "${_mhp3rd_adrenotools_root}")
    file(REMOVE_RECURSE "${_mhp3rd_adrenotools_src}/lib/linkernsbypass")
    file(RENAME "${_mhp3rd_adrenotools_root}/liblinkernsbypass-${MHP3RD_LINKERNSBYPASS_COMMIT}"
        "${_mhp3rd_adrenotools_src}/lib/linkernsbypass")
    file(WRITE "${_mhp3rd_adrenotools_src}/yakumo.stamp" "${_mhp3rd_adrenotools_stamp}")
endif()

# adrenotools' own CMakeLists.txt: its arm64-v8a-only check, the "adrenotools"
# static library (LIB_SOURCES, linked against android and linkernsbypass) and
# src/hook's own add_subdirectory(), which builds the hook shared libraries
# (main_hook, hook_impl, and two more for features this port does not use) the
# isolated namespace loads. Unmodified.
add_subdirectory("${_mhp3rd_adrenotools_src}" "${CMAKE_BINARY_DIR}/_deps/adrenotools-build")

# Packed into the APK beside libmain.so (build_apk.sh), where
# adrenotools_open_libvulkan's hookLibDir (the app's own nativeLibraryDir)
# expects to find them. Neither is a dependency Yakumo links against (they
# are dlopen'd at run time through the isolated namespace, never by name at
# link time), so without this a plain build of the Yakumo target - what
# release_android.sh and a first-time build both do - would leave them
# unbuilt and build_apk.sh would find nothing to pack.
set_target_properties(main_hook hook_impl PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lib")
add_dependencies(Yakumo main_hook hook_impl)
