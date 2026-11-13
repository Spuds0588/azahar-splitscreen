cmake_minimum_required(VERSION 3.25)
project(azahar-quadview LANGUAGES CXX)

# Standalone: this is deliberately not part of Azahar's build. It only needs Qt6
# Widgets and can be removed without touching the emulator.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_BUILD_TYPE Release)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(X11 REQUIRED)

add_executable(azahar-quadview main.cpp)
target_link_libraries(azahar-quadview PRIVATE Qt6::Widgets X11::X11)
