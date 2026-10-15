# Writes the game's build id for netplay's handshake (hello.cpp) into OUT, a C++ file, at every build:
# the git commit, and if there are uncommitted changes "-dirty-" and a hash of them (git diff, with the
# submodules' patched trees left out: their patches are in the repo), then the platform: the system,
# processor and compiler (PLATFORM), as floating-point code can come out differently from one compiler or
# processor to another, so netplay is between the same platforms for now (fork issue #8). Two builds of
# the same commit from clean trees on the same platform have the same id; any change gives another.
# Without git, the commit is "unknown".
# The file is rewritten only when the id changes, so nothing is rebuilt otherwise.
#   cmake -DSOURCE_DIR=<repo> -DOUT=<file> -DPLATFORM=<system-processor-compiler> -P build_id.cmake
execute_process(COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE commit RESULT_VARIABLE failed
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (failed OR commit STREQUAL "")
    set(id "unknown")
else()
    set(id "${commit}")
    execute_process(COMMAND git diff HEAD --ignore-submodules=dirty
        WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE changes ERROR_QUIET)
    if (NOT changes STREQUAL "")
        string(SHA256 changes_hash "${changes}")
        string(SUBSTRING "${changes_hash}" 0 16 changes_hash)
        string(APPEND id "-dirty-${changes_hash}")
    endif()
endif()
string(APPEND id " ${PLATFORM}")
set(content "// Written by host/src/netplay/build_id.cmake at each build.\nnamespace conker::netplay {\n    const char* build_id() { return \"${id}\"; }\n}\n")
if (EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if (NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
