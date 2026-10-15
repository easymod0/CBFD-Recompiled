# Netplay on Windows: protobuf (for GameNetworkingSockets) comes from vcpkg, as the manifest beside
# this file says, built static (x64-windows-static-md) so the game needs no more DLLs. Included before
# project() (host/CMakeLists.txt with CONKER_NETPLAY, and this folder's CMakeLists.txt).
#
# The vcpkg is VCPKG_ROOT's, which Visual Studio 2022 (17.6 and later) sets in its developer
# environment for the vcpkg it installs with the C++ workload, or else Visual Studio's own. The first
# build takes about 20 minutes (protobuf and abseil); vcpkg then keeps them in its own cache, so later
# builds, even from a clean folder, take seconds. A toolchain file given on the command line is left
# alone. Linux and macOS use their system's protobuf instead.
if (NOT CMAKE_HOST_WIN32 OR DEFINED CMAKE_TOOLCHAIN_FILE)
    return()
endif()

set(_conker_vcpkg "$ENV{VCPKG_ROOT}")
if (NOT _conker_vcpkg AND EXISTS "$ENV{VSINSTALLDIR}VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(_conker_vcpkg "$ENV{VSINSTALLDIR}VC/vcpkg")
endif()
if (NOT _conker_vcpkg)
    message(FATAL_ERROR "Netplay needs vcpkg, for protobuf: install Visual Studio's \"vcpkg package manager\" "
        "component (in the Visual Studio Installer), or set VCPKG_ROOT to a vcpkg of your own.")
endif()
file(TO_CMAKE_PATH "${_conker_vcpkg}" _conker_vcpkg)
set(CMAKE_TOOLCHAIN_FILE "${_conker_vcpkg}/scripts/buildsystems/vcpkg.cmake")
set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md")
message(STATUS "Netplay: protobuf from vcpkg (${_conker_vcpkg})")
