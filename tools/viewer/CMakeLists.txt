cmake_minimum_required(VERSION 3.25)
project(azahar-viewer LANGUAGES CXX)

# Standalone: the viewer is deliberately not part of Azahar's build. It only
# consumes shared-memory frame rings, so it knows nothing about the emulator.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_BUILD_TYPE Release)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

# Pads, so each player's own shoulder holds their peek. SDL rather than raw
# evdev because it carries the button mappings and the hotplug notifications.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(azahar-viewer main.cpp menus.cpp menus.h viewer.cpp viewer.h)
target_include_directories(azahar-viewer PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(azahar-viewer PRIVATE Qt6::Widgets Qt6::OpenGLWidgets ${SDL2_LIBRARIES})
