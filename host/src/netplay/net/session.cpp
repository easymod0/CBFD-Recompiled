// A netplay session over GameNetworkingSockets: see session.hpp.

#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

namespace conker::netplay {
    namespace {
        // A joiner that hasn't finished the handshake in this long is dropped.
        constexpr double handshake_seconds = 10.0;
        // Connections the host takes at once, joined or not: more are turned away unread.
        constexpr size_t max_connections = 8;

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
        if (!peers.empty() || listen_socket != k_HSteamListenSocket_Invalid) {
            leave(Reason::quit);
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
        SteamNetworkingIPAddr address;
        address.Clear();
        address.m_port = port;
        SteamNetworkingConfigValue_t option;
        option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
        listen_socket = SteamNetworkingSockets()->CreateListenSocketIP(address, 1, &option);
        if (listen_socket == k_HSteamListenSocket_Invalid) {
            error = "couldn't listen on UDP port " + std::to_string(port);
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        is_host = true;
        own_port = 0;
        match_players = players;
        hello = me;
        return true;
    }

    bool Session::join(const std::string& address, const Hello& me, std::string& error) {
        SteamNetworkingIPAddr host_address;
        if (!host_address.ParseString(address.c_str()) || host_address.m_port == 0) {
            error = "\"" + address + "\" isn't an address and port (1.2.3.4:5000 or [::1]:5000)";
            return false;
        }
        SteamNetworkingConfigValue_t option;
        option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
        const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectByIPAddress(host_address, 1, &option);
        if (connection == k_HSteamNetConnection_Invalid) {
            error = "couldn't connect to " + address;
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        SteamNetworkingSockets()->SetConnectionPollGroup(connection, poll_group);
        is_host = false;
        hello = me;
        peers.push_back({ connection, 0, now() });  // the host, on port 0
        return true;
    }

    bool Session::start() {
        const int joined = (int)std::count_if(peers.begin(), peers.end(), [](const Peer& p) { return p.port >= 0; });
        if (!is_host || !entry_open || joined + 1 != match_players) {
            return false;
        }
        entry_open = false;
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, Start{ (uint8_t)match_players });
            }
        }
        return true;
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
                event.detail = info->m_info.m_szEndDebug;
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
            while ((count = sockets->ReceiveMessagesOnPollGroup(poll_group, messages, 32)) > 0) {
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
                std::optional<Reason> refusal;
                if (theirs->build != hello.build) refusal = Reason::build;
                else if (theirs->rom != hello.rom) refusal = Reason::rom;
                else if (theirs->profile != hello.profile) refusal = Reason::profile;
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
            if (const Inputs* inputs = std::get_if<Inputs>(&packet); inputs != nullptr && inputs->port == peer.port) {
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
            else if (const Refuse* refuse = std::get_if<Refuse>(&packet)) {
                Event event{ Event::Type::ended };
                event.reason = refuse->reason;
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
            events.push_back(Event{ Event::Type::started });
        }
        else if (const Inputs* inputs = std::get_if<Inputs>(&packet); inputs != nullptr) {
            if (inputs->port == own_port || inputs->port >= match_players) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = inputs->port;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (std::holds_alternative<Hash>(packet)) {
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
        if (listen_socket != k_HSteamListenSocket_Invalid) {
            SteamNetworkingSockets()->CloseListenSocket(listen_socket);
            listen_socket = k_HSteamListenSocket_Invalid;
        }
    }
}
