cmake_minimum_required(VERSION 3.20)
project(vectis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-D_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic -O2)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(EPOXY REQUIRED epoxy)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(VTERM REQUIRED vterm)

include_directories(
    include
    ${GLFW_INCLUDE_DIRS}
    ${EPOXY_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${VTERM_INCLUDE_DIRS}
)

add_executable(vectis
    src/core/Pty.cpp
    src/vt/Terminal.cpp
    src/font/FontAtlas.cpp
    src/config/Config.cpp
    src/main.cpp
)

target_link_libraries(vectis PRIVATE
    ${GLFW_LIBRARIES}
    ${EPOXY_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${VTERM_LIBRARIES}
)

install(TARGETS vectis DESTINATION /usr/local/bin)
