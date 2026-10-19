cmake_minimum_required(VERSION 3.16)
project(protocol_client LANGUAGES CXX)

if(WIN32)
  message(FATAL_ERROR
    "protocol_client builds on Linux under WSL2. See plantForCppRTDE.md.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(protocol_client protocol_client.cpp)
target_link_libraries(protocol_client PRIVATE Threads::Threads)
target_compile_options(protocol_client PRIVATE -Wall -Wextra -Wpedantic)
