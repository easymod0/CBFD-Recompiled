# Netplay's transport: the packets, sessions over GameNetworkingSockets, a test program
# and the packets' tests and fuzz target. The game includes it with CONKER_NETPLAY (host/CMakeLists.txt);
# it also builds alone, without the game or the ROM (as the CI does):
#
#   cmake -S host/src/netplay/net -B build-net -G Ninja && cmake --build build-net
#   build-net/packets_test && build-net/netplay_probe host 5000 & build-net/netplay_probe join 127.0.0.1:5000
#
# GameNetworkingSockets is downloaded while configuring (FetchContent, a pinned commit), only when this
# is built: not a submodule, as its own submodules (WebRTC, gigabytes) would come with build.sh's
# recursive update. It needs protobuf (on Windows from vcpkg: vcpkg.cmake), and OpenSSL for its crypto but
# on Windows (bcrypt, built in). Its ICE (NAT traversal, relays) is off: direct connections only.
cmake_minimum_required(VERSION 3.20)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include("${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.cmake")
    project(ConkerNetplay C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED True)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(CONKER_NETPLAY_FUZZ "Build the packets' libFuzzer target (clang)" OFF)

# The packets and join codes alone: no dependencies, for the tests and the fuzzer.
add_library(conker_netplay_packets STATIC packets.cpp join_code.cpp)
target_include_directories(conker_netplay_packets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(conker_netplay_packets PUBLIC cxx_std_20)

add_executable(packets_test packets_test.cpp)
target_link_libraries(packets_test PRIVATE conker_netplay_packets)
add_executable(join_code_test join_code_test.cpp)
target_link_libraries(join_code_test PRIVATE conker_netplay_packets)

if (CONKER_NETPLAY_FUZZ)
    add_executable(fuzz_packets fuzz_packets.cpp packets.cpp)
    target_include_directories(fuzz_packets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(fuzz_packets PRIVATE -fsanitize=fuzzer,address,undefined -g)
    target_link_options(fuzz_packets PRIVATE -fsanitize=fuzzer,address,undefined)
    return()
endif()

include(FetchContent)
# v1.6.1. Its submodules (abseil, WebRTC, vjson) are only for ICE, WebRTC and its tests.
FetchContent_Declare(GameNetworkingSockets
    GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
    GIT_TAG d534c19aa760df3fb75fd20db13ba1932b8a5463
    GIT_SUBMODULES ""
    EXCLUDE_FROM_ALL)
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
if (WIN32)
    set(USE_CRYPTO "BCrypt" CACHE STRING "" FORCE)
endif()
FetchContent_MakeAvailable(GameNetworkingSockets)

add_library(conker_netplay_net STATIC session.cpp)
target_link_libraries(conker_netplay_net PUBLIC conker_netplay_packets GameNetworkingSockets::static)

add_executable(netplay_probe probe.cpp)
target_link_libraries(netplay_probe PRIVATE conker_netplay_net)
