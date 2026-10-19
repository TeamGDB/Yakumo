# Mods with code of their own: the Luau engine scripts run on, the game's
# symbol table compiled in for mods to look names up, and the tests of code,
# native and script mods and of .ymod packages.
#
# Defines the interface target mhp3rd_mod_api (the SDK's headers, the symbol
# table and Luau, for what builds the mod host) and mhp3rd_add_mod_tests(),
# called once mhp3rd_test_host exists.

add_subdirectory("${MHP3RD_PROFILE_DIR}/third_party/luau" "${CMAKE_CURRENT_BINARY_DIR}/luau")

# The game's known functions and data (sdk/symbols.txt), as one string per line.
set(MHP3RD_MOD_SYMBOLS_TXT "${MHP3RD_PROFILE_DIR}/sdk/symbols.txt")
set(MHP3RD_MOD_SYMBOLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_mods")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MHP3RD_MOD_SYMBOLS_TXT}")
file(READ "${MHP3RD_MOD_SYMBOLS_TXT}" MHP3RD_MOD_SYMBOLS_SOURCE)
string(REPLACE "\r" "" MHP3RD_MOD_SYMBOLS_SOURCE "${MHP3RD_MOD_SYMBOLS_SOURCE}")
string(REGEX REPLACE "\n$" "" MHP3RD_MOD_SYMBOLS_SOURCE "${MHP3RD_MOD_SYMBOLS_SOURCE}")
string(REPLACE "\n" ")sym\",\nR\"sym(" MHP3RD_MOD_SYMBOLS_SOURCE "${MHP3RD_MOD_SYMBOLS_SOURCE}")
file(CONFIGURE OUTPUT "${MHP3RD_MOD_SYMBOLS_DIR}/mod_symbols.inc"
    CONTENT "R\"sym(@MHP3RD_MOD_SYMBOLS_SOURCE@)sym\",\n" @ONLY)

add_library(mhp3rd_mod_api INTERFACE)
target_include_directories(mhp3rd_mod_api INTERFACE "${MHP3RD_PROFILE_DIR}/sdk/include" "${MHP3RD_MOD_SYMBOLS_DIR}")
target_link_libraries(mhp3rd_mod_api INTERFACE mhp3rd_luau)

# A test mod: a shared library built from the SDK's headers alone, as a mod
# author builds one.
function(mhp3rd_add_test_mod name source)
    add_library(${name} MODULE "${source}")
    target_include_directories(${name} PRIVATE "${MHP3RD_PROFILE_DIR}/sdk/include")
    target_compile_features(${name} PRIVATE cxx_std_17)
    set_target_properties(${name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        CXX_VISIBILITY_PRESET hidden)
endfunction()

function(mhp3rd_add_mod_test name)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 60)
    endif()
endfunction()

function(mhp3rd_add_mod_tests)
    # The community's mod folders, choices, conflicts and import, the DATA.BIN
    # obfuscation and an archive served with mods' files; then code mods and
    # .ymod packages. No game data.
    set(mod_sources
        host/mods/file_overlay.cpp
        host/mods/mhp3rd_added_files.cpp
        host/mods/mhp3rd_code_mods.cpp
        host/mods/mhp3rd_data_bin.cpp
        host/mods/mhp3rd_mod_format.cpp
        host/mods/mod_import.cpp
        host/mods/mod_ini.cpp
        host/mods/mod_library.cpp
        host/mods/mod_package.cpp
        host/mods/mod_session.cpp
        host/game/quest_file.cpp
        host/game/guest_ram.cpp)
    foreach(test mods_tests code_mods_tests mod_package_tests)
        add_executable(mhp3rd_${test} tests/${test}.cpp ${mod_sources})
        target_include_directories(mhp3rd_${test} PRIVATE "${MHP3RD_PROFILE_DIR}/host")
        target_link_libraries(mhp3rd_${test} PRIVATE psprecomp_core)
        mhp3rd_add_mod_test(mhp3rd_${test})
    endforeach()

    # Native mods through the real loader: the test mod, the SDK's template
    # (so it keeps building and starting against the API), and the test mod
    # again, saying it was made for a newer interface.
    mhp3rd_add_test_mod(mhp3rd_native_test_mod tests/native_mods_test_library.cpp)
    mhp3rd_add_test_mod(mhp3rd_template_test_mod sdk/templates/native/src/mod.cpp)
    mhp3rd_add_test_mod(mhp3rd_future_test_mod tests/native_mods_test_library.cpp)
    target_compile_definitions(mhp3rd_future_test_mod PRIVATE MHP3RD_FUTURE_API_TEST_MOD)
    add_executable(mhp3rd_native_mods_tests tests/native_mods_tests.cpp)
    target_link_libraries(mhp3rd_native_mods_tests PRIVATE mhp3rd_test_host)
    target_compile_definitions(mhp3rd_native_mods_tests PRIVATE
        MHP3RD_NATIVE_TEST_MOD="$<TARGET_FILE:mhp3rd_native_test_mod>"
        MHP3RD_TEMPLATE_TEST_MOD="$<TARGET_FILE:mhp3rd_template_test_mod>"
        MHP3RD_FUTURE_TEST_MOD="$<TARGET_FILE:mhp3rd_future_test_mod>")
    add_dependencies(mhp3rd_native_mods_tests mhp3rd_native_test_mod mhp3rd_template_test_mod mhp3rd_future_test_mod)
    mhp3rd_add_mod_test(mhp3rd_native_mods_tests)

    # Script mods through the real Luau engine. The SDK's script template is
    # compiled in, so the test runs on devices without the sources too.
    set(example "${MHP3RD_PROFILE_DIR}/sdk/templates/script/main.luau")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${example}")
    file(READ "${example}" example_source)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_tests/example_script.inc"
        CONTENT "R\"luau(@example_source@)luau\"\n" @ONLY)
    add_executable(mhp3rd_script_mods_tests tests/script_mods_tests.cpp)
    target_include_directories(mhp3rd_script_mods_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_tests")
    target_link_libraries(mhp3rd_script_mods_tests PRIVATE mhp3rd_test_host)
    mhp3rd_add_mod_test(mhp3rd_script_mods_tests)
endfunction()
