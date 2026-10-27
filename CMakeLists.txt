cmake_minimum_required(VERSION 3.16)
project(OpenNFH VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(OPENNFH_BUILD_VIEWER "Build the SDL2 resource viewer" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------------------
# Core library: archives (.bnd), TGA, XML, text encodings. Depends only on zlib.
# ---------------------------------------------------------------------------
add_library(nfhcore STATIC
  src/core/bnd_archive.cpp
  src/core/game_data.cpp
  src/core/platform.cpp
  src/core/png_writer.cpp
  src/core/text.cpp
  src/core/tga.cpp
  src/core/xml.cpp
  src/game/level_data.cpp
)
target_include_directories(nfhcore PUBLIC src)
target_link_libraries(nfhcore PUBLIC ZLIB::ZLIB)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(nfhcore PUBLIC stdc++fs)
endif()

# ---------------------------------------------------------------------------
# nfh-extract: command line unpacker / converter
# ---------------------------------------------------------------------------
add_executable(nfh-extract tools/nfh_extract.cpp)
target_link_libraries(nfh-extract PRIVATE nfhcore)

# ---------------------------------------------------------------------------
# SDL2 targets: nfh-viewer (resource browser) & opennfh (playable engine)
# ---------------------------------------------------------------------------
if(OPENNFH_BUILD_VIEWER)
  find_package(PkgConfig QUIET)

  # Prefer CMake config packages (MSYS2, recent distros), fall back to pkg-config
  # (older distros ship SDL2_ttf / SDL2_mixer without CMake configs).
  function(opennfh_find_sdl_component pkg cfgtarget pcname outvar)
    find_package(${pkg} CONFIG QUIET)
    if(TARGET ${cfgtarget})
      set(${outvar} ${cfgtarget} PARENT_SCOPE)
      message(STATUS "OpenNFH: ${pkg} via CMake config")
    elseif(PKG_CONFIG_FOUND)
      pkg_check_modules(PC_${pkg} REQUIRED IMPORTED_TARGET GLOBAL ${pcname})
      set(${outvar} PkgConfig::PC_${pkg} PARENT_SCOPE)
      message(STATUS "OpenNFH: ${pkg} via pkg-config")
    else()
      message(FATAL_ERROR "${pkg} not found (install SDL2 development packages)")
    endif()
  endfunction()

  opennfh_find_sdl_component(SDL2       SDL2::SDL2             sdl2       SDL2_LIB)
  opennfh_find_sdl_component(SDL2_ttf   SDL2_ttf::SDL2_ttf     SDL2_ttf   SDL2_TTF_LIB)
  opennfh_find_sdl_component(SDL2_mixer SDL2_mixer::SDL2_mixer SDL2_mixer SDL2_MIXER_LIB)

  add_executable(nfh-viewer
    viewer/main.cpp
    viewer/viewer.cpp
    viewer/text_renderer.cpp
  )
  target_compile_definitions(nfh-viewer PRIVATE SDL_MAIN_HANDLED)
  target_include_directories(nfh-viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(nfh-viewer PRIVATE nfhcore ${SDL2_TTF_LIB} ${SDL2_MIXER_LIB} ${SDL2_LIB})

  add_executable(opennfh
    game/main.cpp
    game/engine.cpp
    viewer/text_renderer.cpp
  )
  target_compile_definitions(opennfh PRIVATE SDL_MAIN_HANDLED)
  target_include_directories(opennfh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(opennfh PRIVATE nfhcore ${SDL2_TTF_LIB} ${SDL2_MIXER_LIB} ${SDL2_LIB})
endif()
