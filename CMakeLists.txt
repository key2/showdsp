cmake_minimum_required(VERSION 3.20)
project(showdsp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(showdsp-cli
    cli/main.cpp
    src/audiofile.cpp
    src/miniaudio_impl.c
    third_party/libebur128/ebur128.c
)
target_include_directories(showdsp-cli PRIVATE src third_party)
target_compile_options(showdsp-cli PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O2 -Wall -Wextra>)
if(UNIX)
    target_link_libraries(showdsp-cli m ${CMAKE_DL_LIBS} pthread)
endif()
