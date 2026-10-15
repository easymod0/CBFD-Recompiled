// netplay_probe: tries the netplay transport (session.hpp) without the game. Peers connect as a match
// would and each sends made-up pads for its own port, 30 reads a second as the game reads them; each
// checks that every other port's pads arrive, and are the ones sent.
//
//   netplay_probe host <port> [--players 2] [options]
//   netplay_probe join <address:port or join code> [options]
//   netplay_probe join <address:port> --silent
//   netplay_probe code <address:port>
//     --reads <n>        how many reads each peer sends (300: 10 s)
//     --loss <percent>   the library drops this share of the packets sent (they're resent)
//     --build/--rom/--profile <hex>   the handshake's hashes (all 1 by default), to test refusals
//     --silent           connect but never say Hello: the host must drop it after its handshake time
//
// It exits 0 once it has every other port's pads for every read and they're all right, 1 otherwise;
// --silent, once the host has closed the connection for a timeout. `code` prints the address's join code.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "session.hpp"

using namespace conker::netplay;

namespace {
    // The pad `port` sends for `read`: anything valid, and known to every peer.
    Pad made_up_pad(int port, uint32_t read) {
        const uint32_t x = (read + 1) * 2654435761u ^ (uint32_t)(port + 1) * 0x9E3779B9u;
        return Pad{ (uint16_t)(x & ~reserved_buttons), (int8_t)(x >> 16), (int8_t)(x >> 24), 0 };
    }

    // The pad of `port` for `read`, to send.
    Inputs inputs_for(int port, uint32_t read) {
        Inputs inputs;
        inputs.port = (uint8_t)port;
        inputs.first_read = read;
        inputs.count = 1;
        inputs.pads[0] = made_up_pad(port, read);
        return inputs;
    }

    struct Received {
        uint32_t next = 0;      // the first read not had yet (they arrive in order)
        uint32_t wrong = 0;     // pads that weren't the ones sent
        uint32_t packets = 0;
    };

    int usage() {
        std::fprintf(stderr, "usage: netplay_probe host <port> [--players 2] | join <address:port or code> [--silent]\n"
                             "       [--reads 300] [--loss <percent>] [--build <hex>] [--rom <hex>] [--profile <hex>]\n"
                             "       netplay_probe code <address:port>\n");
        return 2;
    }

    // --silent: connects to `where` without a Session, so no Hello is sent, and waits for the host to
    // close the connection. Passes if it does so for a timeout, after 5 to 20 seconds.
    int join_silently(const std::string& where) {
        SteamNetworkingIPAddr address;
        if (!address.ParseString(where.c_str())) {
            std::fprintf(stderr, "--silent takes an address and port\n");
            return 2;
        }
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        const HSteamNetConnection connection = sockets->ConnectByIPAddress(address, 0, nullptr);
        const auto start = std::chrono::steady_clock::now();
        bool connected = false;
        while (true) {
            sockets->RunCallbacks();
            SteamNetConnectionInfo_t info;
            if (!sockets->GetConnectionInfo(connection, &info)) {
                std::printf("FAILED: the connection is gone\n");
                return 1;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (info.m_eState == k_ESteamNetworkingConnectionState_Connected && !connected) {
                connected = true;
                std::printf("connected, saying nothing\n");
            }
            if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                const bool timeout = info.m_eEndReason == k_ESteamNetConnectionEnd_App_Min + (int)Reason::timeout;
                std::printf("closed after %.1f s: %s (%d)\n", seconds, info.m_szEndDebug, info.m_eEndReason);
                sockets->CloseConnection(connection, 0, nullptr, false);
                const bool right = connected && timeout && seconds > 5 && seconds < 20;
                std::printf("%s\n", right ? "dropped for saying nothing" : "FAILED");
                return right ? 0 : 1;
            }
            if (seconds > 30) {
                std::printf("FAILED: still connected after 30 s\n");
                sockets->CloseConnection(connection, 0, nullptr, false);
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "code") == 0) {
        const std::string code = join_code_for(argv[2]);
        if (code.empty()) {
            return usage();
        }
        std::printf("%s\n", code.c_str());
        return 0;
    }
    if (argc < 3 || (std::strcmp(argv[1], "host") != 0 && std::strcmp(argv[1], "join") != 0)) {
        return usage();
    }
    const bool hosting = std::strcmp(argv[1], "host") == 0;
    const std::string where = argv[2];
    int players = 2;
    uint32_t reads = 300;
    float loss = 0;
    Hello me{ 1, 1, 1 };
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const char* value = argv[i + 1];
        if (option == "--players") players = std::atoi(value);
        else if (option == "--reads") reads = (uint32_t)std::strtoul(value, nullptr, 10);
        else if (option == "--loss") loss = std::strtof(value, nullptr);
        else if (option == "--build") me.build = std::strtoull(value, nullptr, 16);
        else if (option == "--rom") me.rom = std::strtoull(value, nullptr, 16);
        else if (option == "--profile") me.profile = std::strtoull(value, nullptr, 16);
        else return usage();
    }
    const bool silent = !hosting && argc == 4 && std::strcmp(argv[3], "--silent") == 0;
    if (reads == 0) {
        return usage();
    }

    std::string error;
    if (!startup(error)) {
        std::fprintf(stderr, "couldn't start the networking: %s\n", error.c_str());
        return 1;
    }
    if (silent) {
        const int silent_result = join_silently(where);
        shutdown();
        return silent_result;
    }
    if (loss > 0) {
        SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, loss);
    }
    int result = 1;
    {
        Session session;
        const bool started = hosting ? session.host((uint16_t)std::atoi(where.c_str()), players, me, error)
                                     : session.join(where, me, error);
        if (!started) {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
        else {
            std::printf("%s %s\n", hosting ? "hosting on port" : "joining", where.c_str());
            std::map<int, Received> received;   // by port
            int joined = 1;                     // the host counts itself
            bool playing = false;               // started: all the ports are taken
            bool ended = false;
            uint32_t sent = 0;                  // reads sent so far
            double done_at = 0;
            const auto start = std::chrono::steady_clock::now();
            auto next_tick = start;
            while (!ended) {
                for (Event& event : session.poll()) {
                    switch (event.type) {
                        case Event::Type::joined:
                            joined++;
                            std::printf("port %d joined (%d of %d)\n", event.port, joined, players);
                            if (joined == players && session.start()) {
                                playing = true;
                                std::printf("started\n");
                            }
                            break;
                        case Event::Type::welcomed:
                            players = event.players;
                            std::printf("welcomed: port %d of %d\n", event.port, event.players);
                            break;
                        case Event::Type::started:
                            playing = true;
                            std::printf("started\n");
                            break;
                        case Event::Type::left:
                            std::printf("port %d left: %s (%s)\n", event.port, describe(event.reason), event.detail.c_str());
                            break;
                        case Event::Type::ended:
                            std::printf("ended: %s (%s)\n", describe(event.reason), event.detail.c_str());
                            ended = true;
                            break;
                        case Event::Type::packet:
                            if (const Inputs* inputs = std::get_if<Inputs>(&event.packet)) {
                                Received& r = received[inputs->port];
                                r.packets++;
                                for (int i = 0; i < inputs->count; i++) {
                                    const uint32_t read = inputs->first_read + i;
                                    // Out of order, missing or not the pad sent: all wrong.
                                    if (read != r.next || !(inputs->pads[i] == made_up_pad(inputs->port, read))) {
                                        r.wrong++;
                                    }
                                    r.next = read + 1;
                                }
                            }
                            break;
                    }
                }

                // Every 1/30 s once the match has started: the next read's pad.
                const auto now = std::chrono::steady_clock::now();
                if (!playing) {
                    next_tick = now;
                }
                else if (now >= next_tick && sent < reads) {
                    next_tick += std::chrono::microseconds(33333);
                    session.send(inputs_for(session.port(), sent));
                    sent++;
                }

                bool complete = playing && sent == reads && (int)received.size() == players - 1;
                for (const auto& [port, r] : received) {
                    complete = complete && r.next >= reads;
                }
                const double seconds = std::chrono::duration<double>(now - start).count();
                if (complete && done_at == 0) {
                    done_at = seconds;
                }
                // A while longer for the others to finish, then goodbye.
                if (done_at != 0 && seconds - done_at > 2.0) {
                    session.leave(Reason::quit);
                    ended = true;
                }
                if (seconds > reads / 30.0 + 60) {
                    std::printf("gave up waiting\n");
                    session.leave(Reason::timeout);
                    ended = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            bool right = done_at != 0;
            for (const auto& [port, r] : received) {
                std::printf("port %d: %u of %u reads, %u wrong, in %u packets\n", port, r.next, reads, r.wrong, r.packets);
                right = right && r.next >= reads && r.wrong == 0;
            }
            std::printf("%s\n", right ? "all pads arrived and were right" : "FAILED");
            result = right ? 0 : 1;
        }
    }
    shutdown();
    return result;
}
