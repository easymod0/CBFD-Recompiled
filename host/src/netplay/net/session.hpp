// A netplay session over GameNetworkingSockets: the host listens on a UDP port and the
// others connect to it directly, by its join code (its address, port and the match's key). Connections
// are encrypted by the library (they aren't authenticated: there is no certificate authority, so someone
// in the path could still pose as the host). All that's sent are the packets in packets.hpp, reliably
// (resent until they arrive, in order); the library closes a connection that sends a bigger message, and
// buffers little from each peer.
//
// The handshake: a joiner sends Hello (the match's key, then build, ROM, settings profile and mods
// hashes) within 3 s. The host closes a wrong key at once, telling it nothing else; then it refuses any
// other difference, or a full or started match, and otherwise gives it the lowest free port (the host is
// port 0) with Welcome. Once every port is taken, the host starts the match (Start). Then each peer sends its own port's pads (Inputs) and memory hashes (Hash) to
// the host, which passes Inputs on to the others: the host is the hub, so joiners only ever connect to
// it. A peer that sends anything else (another port's inputs, a second Hello) is dropped.
//
// Single-threaded: call poll() often (each controller read, or in a loop) from one thread; it runs the
// library's callbacks and returns what happened.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "packets.hpp"

namespace conker::netplay {
    struct Event {
        enum class Type {
            joined,     // host: a joiner passed the handshake, on `port`
            welcomed,   // joiner: accepted, on `port`, in a match of `players`
            started,    // joiner: every port is taken and the match starts
            packet,     // `packet` arrived (Inputs or Hash), from `port`'s peer (via the host for a joiner)
            left,       // host: `port`'s peer left or was dropped, for `reason`
            ended,      // joiner: refused, or the connection to the host ended, for `reason`
        };
        Type type;
        int port = -1;
        int players = 0;
        bool own_view = false;  // started: the host's rule that every player sees only their own view
        Reason reason = Reason::quit;
        std::string detail;     // the library's account of a closed connection, for the log
        Packet packet;
    };

    // The join code (join_code.hpp) for `address` ("1.2.3.4:5000" or "[::1]:5000") and a match's `key`, or
    // empty if it isn't an address and port.
    std::string join_code_for(const std::string& address, uint64_t key);

    // Starts and stops the library, once per process, around any sessions.
    bool startup(std::string& error);
    void shutdown();

    class Session {
    public:
        Session();
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Hosts a match of `players` (2 to 4) on UDP `port`, on every local address, with a new random key.
        bool host(uint16_t port, int players, const Hello& me, std::string& error);
        // Host: the join code for this match at `address` (the host's, as the others reach it), or empty
        // if it isn't an address and port.
        std::string join_code(const std::string& address) const;
        // Joins the host by its join code (`me`'s key is the code's).
        bool join(const std::string& code, const Hello& me, std::string& error);
        // Host: once every port is taken (false until then), starts the match: tells the joiners, and
        // refuses any more. `own_view`: every player sees only their own view (the match's rule).
        bool start(bool own_view = false);

        // What has happened since the last poll.
        std::vector<Event> poll();

        // This peer's own port's pads (a joiner's go to the host, the host's to everyone).
        void send(const Inputs& inputs);
        // This peer's memory hash (a joiner's go to the host, the host's to everyone).
        void send(const Hash& hash);
        // Says goodbye to every peer and closes the connections. The goodbyes go out as the library is
        // polled: poll a moment before deleting the session or stopping the library, or they may be lost.
        void leave(Reason reason);
        // The furthest read a peer's Inputs may reach (their first read plus their count); beyond it they're
        // refused as broken or hostile, before the host passes them on. Lockstep sets it as the game goes;
        // until then, the first reads only.
        void set_horizon(uint64_t reads) { horizon = reads; }

        bool hosting() const { return is_host; }
        int port() const { return own_port; }
        int players() const { return match_players; }

    private:
        struct Peer {
            uint32_t connection = 0;
            int port = -1;          // -1 until the handshake is done
            double connected_at = 0;
            int64_t last_hash = -1; // the read of the last Hash from it (one per read, in order)
        };

        void on_status_changed(const void* info);
        void handle(Peer& peer, const Packet& packet, std::vector<Event>& events);
        void drop(Peer& peer, Reason reason, std::vector<Event>* events);
        void send_to(uint32_t connection, const Packet& packet);
        Peer* find(uint32_t connection);
        friend void status_changed_callback(void* info);

        bool is_host = false;
        bool entry_open = true;
        bool started = false;       // joiner: Start has arrived
        int own_port = -1;
        int match_players = 0;
        uint64_t horizon = 64;
        Hello hello;
        uint32_t listen_socket = 0;
        uint32_t poll_group = 0;
        std::vector<Peer> peers;            // host: everyone connected; joiner: the host
        std::vector<Event> pending_events;  // from the status callback, until the next poll
    };
}
