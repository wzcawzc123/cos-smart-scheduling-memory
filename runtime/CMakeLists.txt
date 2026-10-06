cmake_minimum_required(VERSION 3.18.1)
project(URORuntime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    set(ANDROID_STL c++_static)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -Wl,--gc-sections -Wl,--strip-all")
endif()

find_package(Threads REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(URORuntime
    src/main.cpp
    src/drivers.cpp
)
target_link_libraries(URORuntime Threads::Threads)
set_target_properties(URORuntime PROPERTIES RUNTIME_OUTPUT_DIRECTORY bin)
