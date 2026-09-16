# mh_net_proto -- portable networking protocol library shared by the Windows injected DLL and the
# headless-Linux relay. Pure C++, no platform/socket dependencies. See MP-DISCOVERY.
#
# Linux (relay) build:   cmake -S . -B build && cmake --build build
# Run the unit tests:    ctest --test-dir build --output-on-failure
# The Windows DLL builds these same sources via mh_net_proto.vcxproj (in ../mh_dll/mh.sln).
cmake_minimum_required(VERSION 3.16)
project(mh_net_proto LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mh_net_proto STATIC
    src/net_crypto.cpp
    src/net_wire.cpp
    src/session_info.cpp
)
target_include_directories(mh_net_proto PUBLIC include)

# Guard the portability contract: this library must not pull in platform or socket headers.
if(MSVC)
    target_compile_options(mh_net_proto PRIVATE /W4)
else()
    target_compile_options(mh_net_proto PRIVATE -Wall -Wextra)
endif()

# Unit tests (round-trips + lobby-id dedup) -- built only when mh_net_proto is the top-level project,
# so a relay that add_subdirectory()'s the lib doesn't inherit them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(net_proto_test test/net_proto_test.cpp)
    target_link_libraries(net_proto_test PRIVATE mh_net_proto)
    add_test(NAME net_proto_test COMMAND net_proto_test)
endif()
