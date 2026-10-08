cmake_minimum_required(VERSION 3.18.1)
project(URORuntime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    set(ANDROID_STL c++_static)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror=multichar")
# -Werror=multichar：C++ 单引号'abc'是多字符字面量(打包成整数)而非字符串——
# telemetry 曾因此把 {"ts": 写成十进制数字（GCC 仅警告、静默通过），故升级为错误

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -Wl,--gc-sections -Wl,--strip-all")
endif()

find_package(Threads REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(URORuntime
    src/main.cpp
    src/drivers.cpp
    src/adapter.cpp
    src/controllers.cpp
)
target_link_libraries(URORuntime Threads::Threads)
set_target_properties(URORuntime PROPERTIES RUNTIME_OUTPUT_DIRECTORY bin)
