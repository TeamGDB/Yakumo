# Luau, the scripting language of script mods (host/mods/script_mods.cpp).
# MIT licence: LICENSE.txt (Luau) and lua_LICENSE.txt (the Lua code it
# derives from). Unmodified sources of the release named in README.md; this
# file replaces upstream's build files with the parts the host needs: the
# VM and the compiler (Common, Ast, Bytecode, Compiler, VM).
file(GLOB MHP3RD_LUAU_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Ast/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Bytecode/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/VM/src/*.cpp")
add_library(mhp3rd_luau STATIC ${MHP3RD_LUAU_SOURCES})
target_include_directories(mhp3rd_luau SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Ast/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Bytecode/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/VM/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/VM/src")
target_compile_features(mhp3rd_luau PUBLIC cxx_std_17)
set_target_properties(mhp3rd_luau PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(mhp3rd_luau PRIVATE /W0)
else()
    target_compile_options(mhp3rd_luau PRIVATE -w)
endif()
