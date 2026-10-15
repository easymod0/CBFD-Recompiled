// netplay_probe: tries the netplay transport (session.hpp) without the game. Peers connect as a match
// would and each sends made-up pads for its own port, 30 reads a second as the game reads them; each
// checks that every other port's pads arrive, and are the ones sent.
//
//   netplay_probe host <port> [--players 2] [options]     (prints its join code, for 127.0.0.1)
//   netplay_probe join <join code> [options]
//   netplay_probe join <address:port> --silent | --oversize
//   netplay_probe code <address:port> <key, hex>
//     --reads <n>        how many reads each peer sends (300: 10 s)
//     --loss <percent>   the library drops this share of the packets sent (they're resent)
//     --build/--rom/--profile/--mods <hex>   the handshake's hashes (all 1 by default), to test refusals
//     --silent           connect but never say Hello: the host must drop it after its handshake time
//     --oversize         connect and send a message bigger than any packet: the library must close it
//     --own-view 1       (hosting) the match's rule that every player sees only their own view
//     --hostile early    (joining) send Inputs before the match starts: the host must drop it
//     --hostile far      (joining) send Inputs far beyond the reads it could have got to: dropped too
//     --hostile hash     (joining) send the same Hash twice once the match starts: dropped too
//
// It exits 0 once it has every other port's pads for every read and they're all right, 1 otherwise;
// --silent, once the host has closed the connection for a timeout; --oversize, once it's closed before
// that. `code` prints the join code for an address and key.

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
        std::fprintf(stderr, "usage: netplay_probe host <port> [--players 2] | join <code> | join <address:port> --silent|--oversize\n"
                             "       [--reads 300] [--loss <percent>] [--build <hex>] [--rom <hex>] [--profile <hex>] [--mods <hex>]\n"
                             "       netplay_probe code <address:port> <key, hex>\n");
        return 2;
    }

    // --silent: connects to `where` without a Session, so no Hello is sent, and waits for the host to
    // close the connection. Passes if it does so for a timeout, after 2 to 10 seconds. --oversize: sends a
    // message bigger than any packet once connected, and passes if the connection is closed for it (not
    // for a timeout) within 2 seconds.
    int join_silently(const std::string& where, bool oversize) {
        SteamNetworkingIPAddr address;
        if (!address.ParseString(where.c_str())) {
            std::fprintf(stderr, "--silent and --oversize take an address and port\n");
            return 2;
        }
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        const HSteamNetConnection connection = sockets->ConnectByIPAddress(address, 0, nullptr);
        const auto start = std::chrono::steady_clock::now();
        bool connected = false;
        double connected_at = 0;
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
                connected_at = seconds;
                if (oversize) {
                    const std::vector<uint8_t> big(1024, 0x43);
                    sockets->SendMessageToConnection(connection, big.data(), (uint32)big.size(), k_nSteamNetworkingSend_Reliable, nullptr);
                    std::printf("connected, sent %zu bytes\n", big.size());
                }
                else {
                    std::printf("connected, saying nothing\n");
                }
            }
            if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                const bool timeout = info.m_eEndReason == k_ESteamNetConnectionEnd_App_Min + (int)Reason::timeout;
                std::printf("closed after %.1f s: %s (%d)\n", seconds, info.m_szEndDebug, info.m_eEndReason);
                sockets->CloseConnection(connection, 0, nullptr, false);
                const bool right = connected && (oversize ? !timeout && seconds - connected_at < 2 : timeout && seconds > 2 && seconds < 10);
                std::printf("%s\n", !right ? "FAILED" : oversize ? "closed for the oversized message" : "dropped for saying nothing");
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
    if (argc == 4 && std::strcmp(argv[1], "code") == 0) {
        const std::string code = join_code_for(argv[2], std::strtoull(argv[3], nullptr, 16));
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
    Hello me{ 1, 1, 1, 1 };
    std::string hostile;
    bool own_view = false;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const char* value = argv[i + 1];
        if (option == "--players") players = std::atoi(value);
        else if (option == "--reads") reads = (uint32_t)std::strtoul(value, nullptr, 10);
        else if (option == "--loss") loss = std::strtof(value, nullptr);
        else if (option == "--build") me.build = std::strtoull(value, nullptr, 16);
        else if (option == "--rom") me.rom = std::strtoull(value, nullptr, 16);
        else if (option == "--profile") me.profile = std::strtoull(value, nullptr, 16);
        else if (option == "--mods") me.mods = std::strtoull(value, nullptr, 16);
        else if (option == "--hostile") hostile = value;
        else if (option == "--own-view") own_view = std::atoi(value) != 0;
        else return usage();
    }
    const bool oversize = !hosting && argc == 4 && std::strcmp(argv[3], "--oversize") == 0;
    const bool silent = !hosting && argc == 4 && (std::strcmp(argv[3], "--silent") == 0 || oversize);
    if (reads == 0) {
        return usage();
    }

    std::string error;
    if (!startup(error)) {
        std::fprintf(stderr, "couldn't start the networking: %s\n", error.c_str());
        return 1;
    }
    if (silent) {
        const int silent_result = join_silently(where, oversize);
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
            if (hosting) {
                std::printf("join code %s\n", session.join_code("127.0.0.1:" + where).c_str());
                std::fflush(stdout);
            }
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
                            if (joined == players && session.start(own_view)) {
                                playing = true;
                                std::printf("started%s\n", own_view ? " (own view)" : "");
                            }
                            break;
                        case Event::Type::welcomed:
                            players = event.players;
                            std::printf("welcomed: port %d of %d\n", event.port, event.players);
                            if (hostile == "early") {
                                session.send(inputs_for(event.port, 0));
                                std::printf("sent Inputs before the start\n");
                            }
                            break;
                        case Event::Type::started:
                            playing = true;
                            std::printf("started%s\n", event.own_view ? " (own view)" : "");
                            if (hostile == "far") {
                                session.send(inputs_for(session.port(), 100000));
                                std::printf("sent Inputs far ahead\n");
                            }
                            if (hostile == "hash") {
                                session.send(Hash{ 30, 1 });
                                session.send(Hash{ 30, 1 });
                                std::printf("sent the same Hash twice\n");
                            }
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
                    // The peers here aren't in lockstep, only sending 30 reads a second each: they may run
                    // a few seconds apart (the joiners start as Start reaches them), so their Inputs may be
                    // that far ahead of this one's.
                    session.set_horizon(sent + 150);
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
