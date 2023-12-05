cmake_minimum_required(VERSION 3.6)

project(SoxFilter VERSION 2.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FindPkgConfig)
if(MINGW)
    pkg_search_module(AVISYNTH avisynth>=3.5.0)
    if(AVISYNTH_FOUND)
        include_directories(${AVISYNTH_INCLUDE_DIRS})
    else()
        # the path on Windows itself, outside of pkg-config
        include_directories($ENV{AVISYNTH_SDK_PATH}/include)
    endif()
else()
    pkg_search_module(AVISYNTH REQUIRED avisynth>=3.5.0)
    include_directories(${AVISYNTH_INCLUDE_DIRS})
endif()

add_library(SoxFilter SHARED SoxFilter/soxfilter.cpp)

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -I. -Wall -O3 -ffast-math -fno-math-errno -fomit-frame-pointer")

target_link_libraries(SoxFilter sox)

include(GNUInstallDirs)
install(TARGETS SoxFilter
        LIBRARY DESTINATION lib/avisynth)

# uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
