cmake_minimum_required(VERSION 3.16)
project(unify LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Deterministic floating point: no fused multiply-add contraction, no fast-math.
# The same binary must produce bit-identical simulation results run to run.
set(UNIFY_FP_FLAGS -ffp-contract=off -fno-fast-math)

# ---- Lua 5.4 (vendored, unmodified) -----------------------------------------------------
file(GLOB LUA_SOURCES third_party/lua/src/*.c)
# Compiled as C++: Lua errors then use C++ exceptions instead of longjmp.
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC third_party/lua/src)
target_compile_definitions(lua PRIVATE LUA_USE_POSIX)
target_compile_options(lua PRIVATE -w ${UNIFY_FP_FLAGS})

# ---- Engine core (no platform dependencies) ---------------------------------------------
file(GLOB_RECURSE UNIFY_ENGINE_SOURCES engine/*.cpp)
add_library(unify_engine STATIC ${UNIFY_ENGINE_SOURCES})
target_include_directories(unify_engine PUBLIC engine)
target_link_libraries(unify_engine PUBLIC lua)
target_compile_options(unify_engine PRIVATE -Wall -Wextra -Wno-unused-parameter ${UNIFY_FP_FLAGS})
option(UNIFY_NO_GLOBAL_NEW "Do not replace global operator new (for hosts like Unreal that own it)" OFF)
if(UNIFY_NO_GLOBAL_NEW)
  target_compile_definitions(unify_engine PUBLIC UNIFY_NO_GLOBAL_NEW=1)
endif()
find_package(Threads REQUIRED)
target_link_libraries(unify_engine PUBLIC Threads::Threads)

# ---- Backends -----------------------------------------------------------------------------
find_package(SDL2 QUIET)
if(SDL2_FOUND)
  add_library(unify_sdl STATIC backends/sdl/sdl_backend.cpp)
  target_include_directories(unify_sdl PUBLIC backends ${SDL2_INCLUDE_DIRS})
  target_link_libraries(unify_sdl PUBLIC unify_engine ${SDL2_LIBRARIES})
  message(STATUS "UNIFY: SDL2 backend enabled")
else()
  message(STATUS "UNIFY: SDL2 not found; building headless only")
endif()

# ---- Demo game ----------------------------------------------------------------------------
set(UNIFY_GAME_ASSETS ${CMAKE_SOURCE_DIR}/game/assets)
add_executable(unify_assetgen tools/assetgen.cpp)
target_link_libraries(unify_assetgen PRIVATE unify_engine)
# Binary assets (sprites, SFX, music) are generated, not committed.
add_custom_command(
  OUTPUT ${UNIFY_GAME_ASSETS}/textures/sprites.qoi ${UNIFY_GAME_ASSETS}/music/theme.wav
  COMMAND unify_assetgen ${UNIFY_GAME_ASSETS}
  DEPENDS unify_assetgen
  COMMENT "Generating game assets")
add_custom_target(unify_assets ALL DEPENDS ${UNIFY_GAME_ASSETS}/textures/sprites.qoi ${UNIFY_GAME_ASSETS}/music/theme.wav)

add_executable(unify_game game/main.cpp)
target_link_libraries(unify_game PRIVATE unify_engine)
target_compile_definitions(unify_game PRIVATE UNIFY_DEFAULT_ASSETS="${UNIFY_GAME_ASSETS}")
if(SDL2_FOUND)
  target_link_libraries(unify_game PRIVATE unify_sdl)
  target_compile_definitions(unify_game PRIVATE UNIFY_HAS_SDL=1)
endif()
add_dependencies(unify_game unify_assets)

add_executable(unify_playtest tools/playtest.cpp)
target_link_libraries(unify_playtest PRIVATE unify_engine)
add_dependencies(unify_playtest unify_assets)

# ---- Tests --------------------------------------------------------------------------------
file(GLOB UNIFY_TEST_SOURCES tests/*.cpp)
add_executable(unify_tests ${UNIFY_TEST_SOURCES})
target_link_libraries(unify_tests PRIVATE unify_engine)
target_compile_options(unify_tests PRIVATE ${UNIFY_FP_FLAGS})
enable_testing()
target_compile_definitions(unify_tests PRIVATE UNIFY_GAME_ASSETS="${UNIFY_GAME_ASSETS}")
add_dependencies(unify_tests unify_assets)
add_test(NAME unify_tests COMMAND unify_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME unify_playtest COMMAND unify_playtest ${UNIFY_GAME_ASSETS} ${CMAKE_BINARY_DIR})
