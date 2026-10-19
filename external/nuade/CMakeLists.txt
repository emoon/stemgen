cmake_minimum_required(VERSION 3.16)
project(nuade VERSION 0.1.0 LANGUAGES C)

set(NUADE_PLAYERS "all" CACHE STRING "Players compiled into the library: \"all\" (every player) or a list of ids")
# A parent project can add players of its own: a list of directories laid out like players/<id>/, set before
# add_subdirectory(nuade).
set(NUADE_EXTRA_PLAYER_DIRS "" CACHE STRING "Directories of extra players to compile in (players/<id>/ layout)")

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(cmake/nuade_players.cmake)
nuade_resolve_players(NUADE_PLAYER_DIRS)

###################################################################################################################
# The library: public API in include/nuade, the framework in src/, one directory per player in players/.

add_library(nuade STATIC
    src/nuade_file_index.c
    src/nuade_grid.c
    src/nuade_io_default.c
    src/nuade_mixer.c
    src/nuade_paula_channel.c
    src/nuade_path.c
    src/nuade_registry.c
    src/nuade_scheduler.c
    src/nuade_song.c
    src/nuade_trace.c
    src/nuade_tracker.c
    src/nuade_viz.c
)
add_library(nuade::nuade ALIAS nuade)
target_include_directories(nuade PUBLIC include PRIVATE src)
target_compile_features(nuade PUBLIC c_std_11)
# Strict C11 (no GNU extensions), so non-portable code fails here and not on another platform.
set_target_properties(nuade PROPERTIES C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nuade PRIVATE -Wall -Wextra)
elseif(MSVC)
    # Sources are UTF-8; fopen and friends are standard C, not "unsafe".
    target_compile_options(nuade PRIVATE /utf-8)
    target_compile_definitions(nuade PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(UNIX)
    target_link_libraries(nuade PUBLIC m)
endif()

nuade_add_players(nuade "${NUADE_PLAYER_DIRS}")
