// What this game says in netplay's handshake (fork issue #7): a joiner whose Hello differs from the
// host's is refused (session.hpp).

#pragma once

#include "net/packets.hpp"

namespace conker::netplay {
    // The build: the git commit, with "-dirty-" and a hash of any uncommitted changes (build_id.cmake).
    const char* build_id();
    // XXH3-64s of the build id, the ROM loaded and the netplay settings profile. Once the ROM is loaded.
    Hello local_hello();
}
