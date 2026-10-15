// A netplay session over GameNetworkingSockets: see session.hpp.

#include "session.hpp"
#include "join_code.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/types.h>
#include <sys/random.h>   // getentropy (Linux, macOS)
#endif

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

namespace conker::netplay {
    namespace {
        // A joiner that hasn't finished the handshake in this long is dropped (an honest Hello takes a
        // round trip).
        constexpr double handshake_seconds = 3.0;
        // Connections the host takes at once, joined or not: more are turned away unread.
        constexpr size_t max_connections = 8;
        // What the library buffers from one peer, unread: a message bigger than the largest packet closes
        // the connection before it's buffered, and past these limits more are dropped (and resent, as
        // everything is sent reliably). An honest peer is never more than a few dozen messages ahead.
        constexpr int recv_buffer_messages = 512;
        constexpr int recv_buffer_bytes = recv_buffer_messages * (int)max_packet_size;
        // The most messages one poll() takes: a flood can't hold the game's thread there.
        constexpr int max_messages_per_poll = 256;

        // The options for every connection, as the host's listen socket or a joiner's connection.
        struct ConnectionOptions {
            SteamNetworkingConfigValue_t values[4];
            ConnectionOptions(void (*status_changed)(SteamNetConnectionStatusChangedCallback_t*)) {
                values[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
                values[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, (int32_t)max_packet_size);
                values[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, recv_buffer_bytes);
                values[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, recv_buffer_messages);
            }
        };

        // A match's key: 64 bits from the system's random source (never 0, which is "no key").
        bool random_key(uint64_t& key) {
            do {
#if defined(_WIN32)
                if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)&key, sizeof(key), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                    return false;
                }
#else
                if (getentropy(&key, sizeof(key)) != 0) {
                    return false;
                }
#endif
            } while (key == 0);
            return true;
        }

        // Whether two keys are the same, taking the same time whatever they are (no early exit for a
        // stranger to time).
        bool same_key(uint64_t a, uint64_t b) {
            volatile uint64_t difference = 0;
            for (int i = 0; i < 64; i += 8) {
                difference = difference | (((a >> i) ^ (b >> i)) & 0xFF);
            }
            return difference == 0;
        }

        Session* current = nullptr;

        double now() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        void debug_output(ESteamNetworkingSocketsDebugOutputType, const char* message) {
            std::fprintf(stderr, "[netplay] %s\n", message);
        }

        // The reason a connection was closed for: ours if the peer gave one, otherwise the library's.
        Reason reason_of(const SteamNetConnectionInfo_t& info) {
            const int code = info.m_eEndReason - k_ESteamNetConnectionEnd_App_Min;
            if (code >= 1 && code <= last_reason) {
                return (Reason)code;
            }
            return info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally ? Reason::timeout : Reason::quit;
        }
    }

    void status_changed_callback(void* info) {
        if (current != nullptr) {
            current->on_status_changed(info);
        }
    }

    namespace {
        void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
            status_changed_callback(info);
        }
    }

    std::string join_code_for(const std::string& address, uint64_t key) {
        SteamNetworkingIPAddr parsed;
        if (!parsed.ParseString(address.c_str())) {
            return {};
        }
        Endpoint endpoint;
        endpoint.port = parsed.m_port;
        endpoint.key = key;
        if (parsed.IsIPv4()) {
            const uint32_t ip = parsed.GetIPv4();
            endpoint.ip = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
        }
        else {
            endpoint.ipv6 = true;
            std::copy(std::begin(parsed.m_ipv6), std::end(parsed.m_ipv6), endpoint.ip.begin());
        }
        return encode_join_code(endpoint);
    }

    bool startup(std::string& error) {
        SteamNetworkingErrMsg message;
        if (!GameNetworkingSockets_Init(nullptr, message)) {
            error = message;
            return false;
        }
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, debug_output);
        return true;
    }

    void shutdown() {
        GameNetworkingSockets_Kill();
    }

    Session::Session() {
        current = this;
    }

    Session::~Session() {
        if (!peers.empty()) {
            leave(Reason::quit);
        }
        if (listen_socket != k_HSteamListenSocket_Invalid) {
            SteamNetworkingSockets()->CloseListenSocket(listen_socket);
        }
        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            SteamNetworkingSockets()->DestroyPollGroup(poll_group);
        }
        current = nullptr;
    }

    bool Session::host(uint16_t port, int players, const Hello& me, std::string& error) {
        if (players < 2 || players > max_players) {
            error = "a match is 2 to 4 players";
            return false;
        }
        hello = me;
        if (!random_key(hello.key)) {
            error = "couldn't make the match's key (no random source)";
            return false;
        }
        SteamNetworkingIPAddr address;
        address.Clear();
        address.m_port = port;
        ConnectionOptions options(status_changed);
        listen_socket = SteamNetworkingSockets()->CreateListenSocketIP(address, 4, options.values);
        if (listen_socket == k_HSteamListenSocket_Invalid) {
            error = "couldn't listen on UDP port " + std::to_string(port);
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        is_host = true;
        own_port = 0;
        match_players = players;
        return true;
    }

    bool Session::join(const std::string& code, const Hello& me, std::string& error) {
        const std::optional<Endpoint> endpoint = decode_join_code(code);
        if (!endpoint) {
            error = "\"" + code + "\" isn't a join code (or has a typo)";
            return false;
        }
        SteamNetworkingIPAddr host_address;
        if (endpoint->ipv6) {
            host_address.SetIPv6(endpoint->ip.data(), endpoint->port);
        }
        else {
            const auto& ip = endpoint->ip;
            host_address.SetIPv4((uint32_t)ip[0] << 24 | ip[1] << 16 | ip[2] << 8 | ip[3], endpoint->port);
        }
        ConnectionOptions options(status_changed);
        const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectByIPAddress(host_address, 4, options.values);
        if (connection == k_HSteamNetConnection_Invalid) {
            error = "couldn't connect to the host";
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        SteamNetworkingSockets()->SetConnectionPollGroup(connection, poll_group);
        is_host = false;
        hello = me;
        hello.key = endpoint->key;
        peers.push_back({ connection, 0, now() });  // the host, on port 0
        return true;
    }

    bool Session::start(bool own_view) {
        const int joined = (int)std::count_if(peers.begin(), peers.end(), [](const Peer& p) { return p.port >= 0; });
        if (!is_host || !entry_open || joined + 1 != match_players) {
            return false;
        }
        entry_open = false;
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, Start{ (uint8_t)match_players, (uint8_t)(own_view ? 1 : 0) });
            }
        }
        return true;
    }

    std::string Session::join_code(const std::string& address) const {
        return is_host ? join_code_for(address, hello.key) : std::string();
    }

    Session::Peer* Session::find(uint32_t connection) {
        auto it = std::find_if(peers.begin(), peers.end(), [connection](const Peer& p) { return p.connection == connection; });
        return it == peers.end() ? nullptr : &*it;
    }

    void Session::on_status_changed(const void* data) {
        const auto* info = (const SteamNetConnectionStatusChangedCallback_t*)data;
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connecting:
                // Someone connecting to the host: taken in, to say Hello (or be refused) within the time.
                if (is_host && info->m_info.m_hListenSocket == listen_socket) {
                    // Once the match has started nobody more can join: turned away at once, not held until
                    // their Hello.
                    if (!entry_open) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)Reason::started, "started", false);
                        return;
                    }
                    if (peers.size() >= max_connections || sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)Reason::full, "full", false);
                        return;
                    }
                    sockets->SetConnectionPollGroup(info->m_hConn, poll_group);
                    peers.push_back({ info->m_hConn, -1, now() });
                }
                break;
            case k_ESteamNetworkingConnectionState_Connected:
                if (!is_host && find(info->m_hConn) != nullptr) {
                    send_to(info->m_hConn, hello);
                }
                break;
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                Peer* peer = find(info->m_hConn);
                sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                if (peer == nullptr) {
                    break;
                }
                Event event{ is_host ? Event::Type::left : Event::Type::ended };
                event.port = peer->port;
                event.reason = reason_of(info->m_info);
                // The peer's own words, for the log: printable characters only.
                for (const char* c = info->m_info.m_szEndDebug; *c != '\0'; c++) {
                    event.detail += (*c >= 0x20 && *c < 0x7F) ? *c : '?';
                }
                if (!is_host || peer->port >= 0) {
                    pending_events.push_back(std::move(event));
                }
                peers.erase(peers.begin() + (peer - peers.data()));
                break;
            }
            default:
                break;
        }
    }

    std::vector<Event> Session::poll() {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        sockets->RunCallbacks();
        std::vector<Event> events = std::move(pending_events);
        pending_events.clear();

        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            SteamNetworkingMessage_t* messages[32];
            int count;
            int taken = 0;
            while (taken < max_messages_per_poll && (count = sockets->ReceiveMessagesOnPollGroup(poll_group, messages, 32)) > 0) {
                taken += count;
                for (int i = 0; i < count; i++) {
                    SteamNetworkingMessage_t* message = messages[i];
                    Peer* peer = find(message->m_conn);
                    if (peer != nullptr) {
                        const std::span<const uint8_t> data((const uint8_t*)message->m_pData, (size_t)message->m_cbSize);
                        std::optional<Packet> packet = decode(data);
                        if (packet) {
                            handle(*peer, *packet, events);
                        }
                        else {
                            // Another protocol version is told so; anything else is a broken packet.
                            const bool other_version = data.size() >= 4 && data[0] == 'C' && data[1] == 'N' &&
                                (uint16_t)(data[2] | (data[3] << 8)) != protocol_version;
                            drop(*peer, other_version ? Reason::version : Reason::protocol, &events);
                        }
                    }
                    message->Release();
                }
            }
        }

        // Joiners that never said Hello.
        const double t = now();
        for (size_t i = 0; i < peers.size();) {
            if (is_host && peers[i].port < 0 && t - peers[i].connected_at > handshake_seconds) {
                drop(peers[i], Reason::timeout, &events);
            }
            else {
                i++;
            }
        }
        return events;
    }

    void Session::handle(Peer& peer, const Packet& packet, std::vector<Event>& events) {
        if (is_host) {
            if (peer.port < 0) {
                const Hello* theirs = std::get_if<Hello>(&packet);
                if (theirs == nullptr) {
                    drop(peer, Reason::protocol, &events);
                    return;
                }
                // The key first, and alone: a wrong one is closed at once, told nothing else (whether its
                // build or ROM would have done).
                if (!same_key(theirs->key, hello.key)) {
                    SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)Reason::key,
                        describe(Reason::key), false);
                    peers.erase(peers.begin() + (&peer - peers.data()));
                    return;
                }
                std::optional<Reason> refusal;
                if (theirs->build != hello.build) refusal = Reason::build;
                else if (theirs->rom != hello.rom) refusal = Reason::rom;
                else if (theirs->profile != hello.profile) refusal = Reason::profile;
                else if (theirs->mods != hello.mods) refusal = Reason::mods;
                else if (!entry_open) refusal = Reason::started;
                int port = -1;
                if (!refusal) {
                    for (int p = 1; p < match_players && port < 0; p++) {
                        if (std::none_of(peers.begin(), peers.end(), [p](const Peer& o) { return o.port == p; })) {
                            port = p;
                        }
                    }
                    if (port < 0) refusal = Reason::full;
                }
                if (refusal) {
                    send_to(peer.connection, Refuse{ *refusal });
                    SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)*refusal,
                        describe(*refusal), true);
                    peers.erase(peers.begin() + (&peer - peers.data()));
                    return;
                }
                peer.port = port;
                send_to(peer.connection, Welcome{ (uint8_t)port, (uint8_t)match_players });
                Event event{ Event::Type::joined };
                event.port = port;
                event.players = match_players;
                events.push_back(std::move(event));
                return;
            }
            // Inputs and hashes only once the match has started (before, a player can still leave and
            // another take the port), and Inputs no further ahead than the game can have got (set_horizon):
            // anything else is a broken or hostile peer, refused before it's passed on.
            const Inputs* inputs = std::get_if<Inputs>(&packet);
            const Hash* hash = std::get_if<Hash>(&packet);
            if ((inputs != nullptr || hash != nullptr) &&
                (entry_open || (inputs != nullptr && (uint64_t)inputs->first_read + inputs->count > horizon))) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            // One hash per read, in order: a repeat is a broken or hostile peer.
            if (hash != nullptr) {
                if ((int64_t)hash->read <= peer.last_hash) {
                    drop(peer, Reason::protocol, &events);
                    return;
                }
                peer.last_hash = hash->read;
            }
            if (inputs != nullptr && inputs->port == peer.port) {
                // Passed on to the others, as they only connect to the host.
                for (const Peer& other : peers) {
                    if (other.port >= 0 && other.connection != peer.connection) {
                        send_to(other.connection, *inputs);
                    }
                }
            }
            else if (const Bye* bye = std::get_if<Bye>(&packet)) {
                Event event{ Event::Type::left };
                event.port = peer.port;
                event.reason = bye->reason;
                SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
                peers.erase(peers.begin() + (&peer - peers.data()));
                events.push_back(std::move(event));
                return;
            }
            else if (!std::holds_alternative<Hash>(packet)) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = peer.port;
            event.packet = packet;
            events.push_back(std::move(event));
            return;
        }

        // A joiner, from the host.
        if (own_port < 0) {
            if (const Welcome* welcome = std::get_if<Welcome>(&packet)) {
                own_port = welcome->port;
                match_players = welcome->players;
                Event event{ Event::Type::welcomed };
                event.port = own_port;
                event.players = match_players;
                events.push_back(std::move(event));
            }
            else if (std::holds_alternative<Refuse>(packet) || std::holds_alternative<Bye>(packet)) {
                // Refused, or dropped before being welcomed (the host's handshake time ran out): with why.
                Event event{ Event::Type::ended };
                event.reason = std::holds_alternative<Refuse>(packet) ? std::get<Refuse>(packet).reason : std::get<Bye>(packet).reason;
                SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
                peers.clear();
                events.push_back(std::move(event));
            }
            else {
                drop(peer, Reason::protocol, &events);
            }
            return;
        }
        if (const Start* start = std::get_if<Start>(&packet)) {
            if (started || start->players != match_players) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            started = true;
            Event event{ Event::Type::started };
            event.own_view = start->own_view != 0;
            events.push_back(std::move(event));
        }
        else if (const Inputs* inputs = std::get_if<Inputs>(&packet); inputs != nullptr) {
            if (!started || inputs->port == own_port || inputs->port >= match_players ||
                (uint64_t)inputs->first_read + inputs->count > horizon) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = inputs->port;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Hash* hash = std::get_if<Hash>(&packet); hash != nullptr) {
            // Once started, one per read, in order.
            if (!started || (int64_t)hash->read <= peer.last_hash) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            peer.last_hash = hash->read;
            Event event{ Event::Type::packet };
            event.port = 0;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Bye* bye = std::get_if<Bye>(&packet)) {
            Event event{ Event::Type::ended };
            event.reason = bye->reason;
            SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
            peers.clear();
            events.push_back(std::move(event));
        }
        else {
            drop(peer, Reason::protocol, &events);
        }
    }

    void Session::drop(Peer& peer, Reason reason, std::vector<Event>* events) {
        send_to(peer.connection, Bye{ reason });
        SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)reason,
            describe(reason), true);
        if (events != nullptr && (!is_host || peer.port >= 0)) {
            Event event{ is_host ? Event::Type::left : Event::Type::ended };
            event.port = peer.port;
            event.reason = reason;
            events->push_back(std::move(event));
        }
        peers.erase(peers.begin() + (&peer - peers.data()));
    }

    // Everything is sent reliably (resent until it arrives, in order), and at once: lockstep needs every
    // input, and waits for it anyway.
    void Session::send_to(uint32_t connection, const Packet& packet) {
        uint8_t buffer[max_packet_size];
        const size_t size = encode(packet, buffer);
        if (size == 0) {
            std::fprintf(stderr, "[netplay] not sending an invalid packet\n");
            return;
        }
        SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size,
            k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
    }

    void Session::send(const Inputs& inputs) {
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, inputs);
            }
        }
    }

    void Session::send(const Hash& hash) {
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, hash);
            }
        }
    }

    void Session::leave(Reason reason) {
        while (!peers.empty()) {
            drop(peers.back(), reason, nullptr);
        }
        // The listen socket stays open (nobody more is let in) until the session goes: closing it closes
        // every connection accepted on it at once, goodbyes not yet sent and all.
        entry_open = false;
    }
}
