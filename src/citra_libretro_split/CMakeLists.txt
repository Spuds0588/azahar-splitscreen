cmake_minimum_required(VERSION 3.16)
project(azahar-splitscreen-libretro LANGUAGES CXX)

# A launcher core, not a port: it spawns the emulator and composites its rings,
# so it deliberately links nothing from Azahar.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(azahar_splitscreen_libretro SHARED core.cpp)
target_include_directories(azahar_splitscreen_libretro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../externals/libretro-common/libretro-common/include)
target_link_libraries(azahar_splitscreen_libretro PRIVATE dl pthread)

set_target_properties(azahar_splitscreen_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "azahar_splitscreen_libretro")
