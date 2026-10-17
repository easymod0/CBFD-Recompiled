// Netplay's lockstep (lockstep.hpp). It all runs on the game thread: the connecting in harness::start,
// before any of the game's code, then each controller read. The session (net/session.hpp) is polled only
// from here; GameNetworkingSockets keeps the connections alive from its own thread in between.
//
// Each port's pads are kept from read 0 on (5 bytes a read: 1 MB is over 15 hours for 4 players). They
// arrive in order, as they're sent reliably: an Inputs packet that doesn't start at the next read of its
// port ends the match.

#include "lockstep.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "librecomp/save_states.hpp"

#include "net/session.hpp"

// The runtime's flag for the program quitting (ultramodern::quit), set before it waits for the game's
// threads to end. librecomp's pi.cpp reads it the same way.
extern std::atomic_bool exited;

using conker::netplay::Event;
using conker::netplay::Hash;
using conker::netplay::Inputs;
using conker::netplay::Motion;
using conker::netplay::Pad;
using conker::lockstep::Input;
using conker::netplay::Reason;

namespace {
    constexpr uint64_t never = UINT64_MAX;
    // Reads between hash comparisons: one a second.
    constexpr uint64_t hash_every = 30;
    // How long a read waits for a peer's pad before the match is given up on. (A peer that's gone is
    // noticed by the library sooner, in about 10 s.)
    constexpr double wait_seconds = 20.0;
    // How long the host waits for every player to join.
    constexpr double join_seconds = 600.0;

    int host_port = -1;
    std::string host_address;       // hosting from the command line: the address the others reach, for the code
    std::string join_where;
    int wanted_players = 2;
    int local_option = 1;           // the players on this machine
    bool host_own_view = true;      // hosting: the rule this peer will start the match with
    bool match_own_view = false;    // the rule of the match under way
    uint8_t match_pairs = 0;        // ...and the ports shown two to a window (Start)
    // --input-delay's, or the menu's: the reads ahead, or auto (the host chooses from the round trips).
    constexpr int auto_delay = -1;
    int delay_option = auto_delay;
    // The match's, from the host (Start).
    int delay = 2;
    uint64_t desync_at = never;
    bool options_valid = true;

    bool library_started = false;
    conker::netplay::Session* session = nullptr;
    uint8_t own_ports = 0;          // bit n: port n
    int match_players = 0;
    bool initial_sent = false;
    bool ended = false;         // the match is over for this peer: nothing more is sent
    bool left = false;
    std::string why_over;       // why the match ended, for the player

    std::array<std::vector<Input>, conker::netplay::max_players> received;
    std::array<bool, conker::netplay::max_players> gone{};
    // Whether a peer that's gone stopped answering, rather than saying goodbye (a crash, a lost connection),
    // so it's told as maybe having quit.
    bool gone_silently = false;
    // The read the game is at, and how far ahead of it the others' Inputs and hashes may be: a peer runs at
    // most the input delay ahead of the slowest (it waits for their pads), and sends its own pads the delay
    // ahead again, so twice the delay, an Inputs packet's reads and some spare. Anything further is a broken
    // or hostile peer, and refused, so a peer can't make this one keep pads or hashes without end.
    uint64_t current_read = 0;
    uint64_t horizon() { return current_read + 2 * (uint64_t)delay + conker::netplay::input_window + 16; }

    // The session is used by the thread connecting (the menu's, or the game's), then the game's, and by the
    // main thread as the program quits (finish): one at a time. `stopping` is set by finish, after which
    // the game thread leaves the session alone.
    std::recursive_mutex session_mutex;
    std::atomic<bool> stopping = false;
    // Watches for the program quitting, to say goodbye at once: finish only runs once the runtime has
    // ended every game thread, and by then the others have long given up on this peer (the game thread
    // may be waiting in on_read for pads that won't come, as they're waiting for this one's).
    std::thread quit_watcher;
    // This peer's hashes, and those of the others (the host's for a joiner, every joiner's for the host),
    // until they're compared.
    std::map<uint32_t, uint64_t> own_hashes;
    std::map<std::pair<int, uint32_t>, uint64_t> their_hashes;

    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // "port 2", or "ports 2 and 3", for the log.
    std::string ports_text(uint8_t ports) {
        std::string numbers;
        const int count = std::popcount(ports);
        for (int port = 0, n = 0; port < conker::netplay::max_players; port++) {
            if ((ports & (1u << port)) == 0) continue;
            numbers += (n == 0 ? "" : n == count - 1 ? " and " : ", ") + std::to_string(port + 1);
            n++;
        }
        return (count == 1 ? "port " : "ports ") + numbers;
    }

    void end_match(Reason reason, const char* why) {
        std::printf("[netplay] the match is over: %s\n", why);
        why_over = why;
        if (!left && session != nullptr) {
            session->leave(reason);
            left = true;
        }
        ended = true;
    }

    // Leaves the match with a goodbye, and polls the library a moment for it to go out (it's sent as the
    // library is polled), or the others only hear the match is over when their connection times out.
    void say_goodbye() {
        std::scoped_lock lock(session_mutex);
        if (session != nullptr && !left) {
            session->leave(Reason::quit);
            left = true;
            for (int i = 0; i < 30; i++) {
                session->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        ended = true;
    }

    // Compares every hash that both sides have for the same read. Peers are never more than the input
    // delay apart, so this peer's own are kept only for the last few comparisons.
    void compare_hashes() {
        for (auto it = their_hashes.begin(); it != their_hashes.end();) {
            const auto own = own_hashes.find(it->first.second);
            if (own == own_hashes.end()) {
                ++it;
                continue;
            }
            if (own->second != it->second) {
                std::printf("[netplay] desync at read %u: port %d's game memory isn't this one's (%016llx, here %016llx)\n",
                    it->first.second, it->first.first + 1, (unsigned long long)it->second, (unsigned long long)own->second);
                end_match(Reason::desync, "the games went different ways (a desync)");
                return;
            }
            it = their_hashes.erase(it);
        }
        while (own_hashes.size() > 8) {
            own_hashes.erase(own_hashes.begin());
        }
    }

    void handle(const Event& event) {
        switch (event.type) {
            case Event::Type::joined:
            case Event::Type::welcomed:
            case Event::Type::started:
                break;
            case Event::Type::left:
                std::printf("[netplay] %s left: %s\n", ports_text(event.ports).c_str(), conker::netplay::describe(event.reason));
                for (int port = 0; port < conker::netplay::max_players; port++) {
                    if ((event.ports & (1u << port)) != 0) gone[port] = true;
                }
                gone_silently = gone_silently || event.reason == Reason::timeout;
                if (event.reason == Reason::desync) {
                    end_match(Reason::desync, "a player's game went a different way (a desync)");
                }
                break;
            case Event::Type::ended:
                if (event.reason == Reason::timeout) {
                    std::printf("[netplay] the host stopped answering (it may have quit)\n");
                }
                else {
                    std::printf("[netplay] the host ended the match: %s\n", conker::netplay::describe(event.reason));
                }
                // Every pad comes through the host.
                for (bool& g : gone) g = true;
                gone_silently = gone_silently || event.reason == Reason::timeout;
                left = true;
                if (event.reason == Reason::desync) {
                    end_match(Reason::desync, "a player's game went a different way (a desync)");
                }
                break;
            case Event::Type::packet:
                if (const Inputs* inputs = std::get_if<Inputs>(&event.packet)) {
                    std::vector<Input>& got = received[inputs->port];
                    if ((own_ports & (1u << inputs->port)) != 0 || inputs->port >= match_players || inputs->first_read != got.size() ||
                        (uint64_t)inputs->first_read + inputs->count > horizon()) {
                        end_match(Reason::protocol, "a player's inputs came out of order");
                        return;
                    }
                    for (int i = 0; i < inputs->count; i++) {
                        got.push_back({ inputs->pads[i], inputs->motions[i] });
                    }
                }
                else if (const Hash* hash = std::get_if<Hash>(&event.packet)) {
                    // Hashes come every hash_every reads, within the horizon; one for a read this peer no
                    // longer has its own of can't be compared, and is dropped.
                    if (hash->read % hash_every != 0 || hash->read > horizon()) {
                        end_match(Reason::protocol, "a player sent a broken hash");
                        return;
                    }
                    if (own_hashes.empty() || hash->read >= own_hashes.begin()->first) {
                        their_hashes[{ event.port, hash->read }] = hash->hash;
                    }
                }
                break;
        }
    }

    // This machine's player `local`'s port: the `local`th of its own (-1 if there's none).
    int nth_own_port(int local) {
        for (int port = 0; port < conker::netplay::max_players; port++) {
            if ((own_ports & (1u << port)) != 0 && local-- == 0) {
                return port;
            }
        }
        return -1;
    }

    void send_own(uint64_t read, int port, Input input) {
        Pad& pad = input.pad;
        pad.buttons &= (uint16_t)~conker::netplay::reserved_buttons;
        pad.err_no = 0;     // a player's port always has a controller
        // CONKER_NETPLAY_LOG_PADS: each change of this peer's buttons, for debugging.
        static const bool log_pads = std::getenv("CONKER_NETPLAY_LOG_PADS") != nullptr;
        if (log_pads && (received[port].empty() || received[port].back().pad.buttons != pad.buttons)) {
            std::printf("[netplay] read %llu: port %d's buttons %04x\n", (unsigned long long)read, port + 1, pad.buttons);
        }
        Inputs inputs;
        inputs.port = (uint8_t)port;
        inputs.first_read = (uint32_t)read;
        inputs.count = 1;
        inputs.pads[0] = pad;
        inputs.motions[0] = input.motion;
        session->send(inputs);
        received[port].push_back(input);
    }

    // The input delay auto chooses, from the round trips to the joiners (ms): enough reads that a pad sent
    // the delay ahead arrives before it's needed, along the slowest way a pad goes. The host passes the
    // joiners' on, so between two joiners that's half of each of their round trips, the two slowest; with
    // one joiner, half of its round trip. The game can wait a few ms at a read and still keep up (a read
    // is a frame, 33 ms at 30 frames a second, most of it spare), so a LAN's sub-millisecond trips give 0:
    // the others' inputs reach the game as soon as this one's own. Each read past that hides a frame's
    // worth of trip, with a few ms for the trip to vary.
    int delay_for(std::vector<int> round_trips, size_t joiners) {
        constexpr double frame_ms = 1000.0 / 30.0;
        constexpr double wait_ms = 8.0;     // what a read can wait without the game falling behind
        constexpr double spare_ms = 4.0;    // for the trip varying
        // A round trip the library doesn't know yet: the delay a match had before it was chosen.
        if (round_trips.size() < joiners) {
            std::printf("[netplay] the round trips to the players aren't all known: input delay 2\n");
            return 2;
        }
        std::sort(round_trips.rbegin(), round_trips.rend());
        double one_way = 0;
        for (size_t i = 0; i < round_trips.size() && i < 2; i++) {
            one_way += round_trips[i] / 2.0;
        }
        const double ahead = one_way + spare_ms - wait_ms;
        const int chosen = ahead <= 0 ? 0 : (int)std::ceil(ahead / frame_ms);
        std::printf("[netplay] round trips to the players: %s ms: input delay %d\n", [&] {
            std::string list;
            for (int t : round_trips) list += (list.empty() ? "" : ", ") + std::to_string(t);
            return list.empty() ? std::string("unknown") : list;
        }().c_str(), std::min(chosen, conker::netplay::max_delay));
        return std::min(chosen, conker::netplay::max_delay);
    }

    // Whether every player's pad for `read` is here.
    bool all_here(uint64_t read) {
        for (int port = 0; port < match_players; port++) {
            if (received[port].size() <= read) return false;
        }
        return true;
    }
}

bool conker::lockstep::parse_option(int argc, char** argv, int& i) {
    const char* option = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    auto number = [&](long low, long high, long& out) {
        char* end = nullptr;
        out = value != nullptr ? std::strtol(value, &end, 10) : 0;
        if (value == nullptr || end == value || *end != '\0' || out < low || out > high) {
            std::fprintf(stderr, "[netplay] %s needs a number from %ld to %ld\n", option, low, high);
            options_valid = false;
        }
        if (value != nullptr) i++;
    };
    long n = 0;
    if (std::strcmp(option, "--netplay-host") == 0) {
        number(1, 65535, n);
        host_port = (int)n;
        return true;
    }
    if (std::strcmp(option, "--players") == 0) {
        number(2, netplay::max_players, n);
        wanted_players = (int)n;
        return true;
    }
    if (std::strcmp(option, "--local-players") == 0) {
        number(1, netplay::max_players - 1, n);
        local_option = (int)n;
        return true;
    }
    if (std::strcmp(option, "--split-screen") == 0) {
        host_own_view = false;
        return true;
    }
    if (std::strcmp(option, "--input-delay") == 0) {
        if (value != nullptr && std::strcmp(value, "auto") == 0) {
            delay_option = auto_delay;
            i++;
            return true;
        }
        number(0, netplay::max_delay, n);
        delay_option = (int)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-desync-at") == 0) {
        number(0, LONG_MAX, n);
        desync_at = (uint64_t)n;
        return true;
    }
    if (std::strcmp(option, "--netplay-address") == 0) {
        if (value == nullptr) {
            std::fprintf(stderr, "[netplay] --netplay-address needs this computer's address, as the others reach it\n");
            options_valid = false;
            return true;
        }
        host_address = argv[++i];
        return true;
    }
    if (std::strcmp(option, "--netplay-join") == 0) {
        if (value == nullptr) {
            std::fprintf(stderr, "[netplay] --netplay-join needs the host's join code\n");
            options_valid = false;
            return true;
        }
        join_where = argv[++i];
        return true;
    }
    return false;
}

bool conker::lockstep::active() {
    return host_port > 0 || !join_where.empty();
}

void conker::lockstep::set_host(int port, int players, bool own_view, int input_delay, int local) {
    host_address.clear();
    host_port = port;
    wanted_players = players;
    local_option = local;
    host_own_view = own_view;
    delay_option = input_delay < 0 ? auto_delay : std::min(input_delay, netplay::max_delay);
    join_where.clear();
}

void conker::lockstep::set_join(const std::string& where, int local) {
    host_port = -1;
    join_where = where;
    local_option = local;
}

bool conker::lockstep::prepare() {
    if (!options_valid) {
        return false;
    }
    if (host_port > 0 && !join_where.empty()) {
        std::fprintf(stderr, "[netplay] --netplay-host and --netplay-join can't be used together\n");
        return false;
    }
    if (host_port > 0 && local_option >= wanted_players) {
        std::fprintf(stderr, "[netplay] --local-players %d needs a match of more players than that (--players)\n", local_option);
        return false;
    }
    // The others join by the code, which holds the match's key: without the address there's none to give.
    if (host_port > 0 && host_address.empty()) {
        std::fprintf(stderr, "[netplay] --netplay-host needs --netplay-address <this computer's address>, for the join code\n");
        return false;
    }
    return true;
}

std::string conker::lockstep::join_code(std::string address) {
    std::scoped_lock lock(session_mutex);
    while (!address.empty() && address.back() == ' ') address.pop_back();
    while (!address.empty() && address.front() == ' ') address.erase(address.begin());
    if (session == nullptr || !session->hosting() || address.empty()) {
        return {};
    }
    std::string code = session->join_code(address);
    if (code.empty()) {
        // Without its port: this match's. An IPv6 address has colons of its own, so brackets go around it.
        const std::string port = std::to_string(host_port);
        code = session->join_code(address.find(':') != std::string::npos && address.front() != '['
            ? "[" + address + "]:" + port : address + ":" + port);
    }
    return code;
}

namespace {
    int joined = 0;             // host: the players in so far, the host too
    double connecting_since = 0;
    bool match_started = false;
    conker::netplay::Hello sent;
}

bool conker::lockstep::begin(const netplay::Hello& me, std::string& error) {
    std::scoped_lock lock(session_mutex);
    // No mods in a match (texture packs aside): a mod is native code the others would have to trust. The
    // host refuses a joiner with any (the handshake's mods hash); this says so before connecting.
    if (me.mods != 0) {
        error = "netplay allows no mods but texture packs: turn them off in the Mods menu.";
        return false;
    }
    if (!library_started) {
        if (!netplay::startup(error)) {
            error = "couldn't start the networking: " + error;
            return false;
        }
        library_started = true;
    }
    cancel();
    session = new netplay::Session();
    const bool hosting = host_port > 0;
    netplay::Hello mine = me;
    mine.players = (uint8_t)local_option;
    if (hosting ? !session->host((uint16_t)host_port, wanted_players, mine, error, local_option) : !session->join(join_where, mine, error)) {
        delete session;
        session = nullptr;
        return false;
    }
    sent = me;
    own_ports = hosting ? session->ports() : 0;
    match_players = hosting ? wanted_players : 0;
    joined = hosting ? local_option : 0;
    connecting_since = now();
    match_started = false;
    ended = false;
    left = false;
    current_read = 0;
    match_own_view = false;
    match_pairs = 0;
    why_over.clear();
    initial_sent = false;
    for (auto& pads : received) pads.clear();
    gone = {};
    gone_silently = false;
    own_hashes.clear();
    their_hashes.clear();
    if (hosting) {
        std::printf("[netplay] hosting a match of %d on UDP port %d, %d of them here: waiting for the players\n",
            wanted_players, host_port, local_option);
        if (!host_address.empty()) {
            const std::string code = join_code(host_address);
            if (code.empty()) {
                std::fprintf(stderr, "[netplay] %s isn't an address: no join code\n", host_address.c_str());
            }
            else {
                std::printf("[netplay] join code %s (the others join with it: it holds the match's key)\n", code.c_str());
                std::fflush(stdout);
            }
        }
    }
    else {
        std::printf("[netplay] joining by code\n");
    }
    return true;
}

conker::lockstep::Progress conker::lockstep::poll_connect(std::string& status) {
    std::scoped_lock lock(session_mutex);
    if (session == nullptr) {
        status = "Not connecting.";
        return Progress::failed;
    }
    const bool hosting = session->hosting();
    for (const Event& event : session->poll()) {
        switch (event.type) {
            case Event::Type::joined:
                joined += std::popcount(event.ports);
                std::printf("[netplay] %s joined (%d of %d)\n", ports_text(event.ports).c_str(), joined, match_players);
                if (joined == match_players) {
                    const int chosen = delay_option == auto_delay ? delay_for(session->round_trips_ms(), (size_t)session->joiners()) : delay_option;
                    if (session->start(host_own_view, chosen)) {
                        match_started = true;
                        match_own_view = host_own_view;
                        match_pairs = session->pairs();
                        delay = chosen;
                    }
                }
                break;
            case Event::Type::left:
                // Before the start, a joiner leaving frees its ports, for the next to start afresh.
                joined -= std::popcount(event.ports);
                for (int port = 0; port < netplay::max_players; port++) {
                    if ((event.ports & (1u << port)) != 0) {
                        received[port].clear();
                        gone[port] = false;
                    }
                }
                std::printf("[netplay] %s left: %s\n", ports_text(event.ports).c_str(), netplay::describe(event.reason));
                break;
            case Event::Type::welcomed:
                own_ports = event.ports;
                match_players = event.players;
                std::printf("[netplay] in, on %s of %d: waiting for the others\n", ports_text(own_ports).c_str(), match_players);
                break;
            case Event::Type::started:
                match_started = true;
                match_own_view = event.own_view;
                match_pairs = event.pairs;
                delay = event.delay;
                break;
            case Event::Type::ended:
                std::printf("[netplay] couldn't join: %s (%s)\n", netplay::describe(event.reason), event.detail.c_str());
                status = std::string("Couldn't join: ") + netplay::describe(event.reason) + ".";
                // Full for this machine's players, not for one: fewer may fit.
                if (event.reason == Reason::full && local_option > 1) {
                    status += " Fewer players on this computer may fit.";
                }
                left = true;
                cancel();
                return Progress::failed;
            case Event::Type::packet:
                handle(event);
                break;
        }
    }
    if (match_started) {
        std::printf("[netplay] the match starts: %d players, this is %s, input delay %d reads%s\n", match_players,
            ports_text(own_ports).c_str(), delay, match_own_view ? ", everyone sees only their own view" : "");
        status = "The match starts.";
        return Progress::started;
    }
    if (hosting && now() - connecting_since > join_seconds) {
        std::printf("[netplay] not every player joined in %.0f minutes\n", join_seconds / 60);
        status = "Not every player joined in " + std::to_string((int)(join_seconds / 60)) + " minutes.";
        cancel();
        return Progress::failed;
    }
    if (hosting) {
        status = "Waiting for players: " + std::to_string(joined) + " of " + std::to_string(match_players) + " in.";
    }
    else if (own_ports == 0) {
        status = "Connecting...";
    }
    else {
        // "player 2", "players 2 and 3".
        std::string as = ports_text(own_ports);
        as.replace(0, 4, "player");
        status = "In, as " + as + " of " + std::to_string(match_players) + ": waiting for the others.";
    }
    return Progress::waiting;
}

void conker::lockstep::cancel() {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr) {
        say_goodbye();
        delete session;
        session = nullptr;
    }
    match_started = false;
}

bool conker::lockstep::connected() {
    return session != nullptr && match_started;
}

const conker::netplay::Hello& conker::lockstep::sent_hello() {
    return sent;
}

bool conker::lockstep::connect(const netplay::Hello& me) {
    std::string error;
    if (!begin(me, error)) {
        std::fprintf(stderr, "[netplay] %s\n", error.c_str());
        return false;
    }
    std::string status;
    while (true) {
        switch (poll_connect(status)) {
            case Progress::started:
                return true;
            case Progress::failed:
                std::fprintf(stderr, "[netplay] %s\n", status.c_str());
                return false;
            case Progress::waiting:
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int conker::lockstep::port() {
    return own_ports == 0 ? -1 : std::countr_zero(own_ports);
}

uint8_t conker::lockstep::ports() {
    return own_ports;
}

int conker::lockstep::local_players() {
    return std::popcount(own_ports);
}

int conker::lockstep::port_of_local(int local) {
    return nth_own_port(local);
}

int conker::lockstep::players() {
    return match_players;
}

int conker::lockstep::input_delay() {
    return delay;
}

bool conker::lockstep::own_view_for_everyone() {
    return match_started && match_own_view;
}

uint8_t conker::lockstep::view_pairs() {
    return match_started ? match_pairs : 0;
}

bool conker::lockstep::on_read(uint64_t read, ultramodern::input::Pad* pads, netplay::Motion* motions, const InputSource& source,
    const std::function<uint64_t()>& hash, uint8_t* rdram) {
    std::unique_lock lock(session_mutex);
    if (ended || stopping) {
        return false;
    }
    current_read = read;
    session->set_horizon(horizon());
    if (!initial_sent) {
        if (!quit_watcher.joinable()) {
            quit_watcher = std::thread([] {
                while (!stopping && !exited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (exited) {
                    std::printf("[netplay] quitting: leaving the match\n");
                    stopping = true;
                    say_goodbye();
                }
            });
        }
        for (int r = 0; r < delay; r++) {
            for (int local = 0; local < std::popcount(own_ports); local++) {
                send_own((uint64_t)r, nth_own_port(local), source((uint64_t)r, nullptr, local));
            }
        }
        initial_sent = true;
    }
    if (read + delay < UINT32_MAX - netplay::input_window) {
        for (int local = 0; local < std::popcount(own_ports); local++) {
            send_own(read + delay, nth_own_port(local), source(read + delay, pads, local));
        }
    }

    if (read == desync_at) {
        rdram[recomp::save_states::game_memory_size / 2] ^= 0x5A;
        std::printf("[netplay] read %llu: game memory changed, as --netplay-desync-at asked\n", (unsigned long long)read);
    }
    if (read % hash_every == 0) {
        const uint64_t h = hash();
        own_hashes[(uint32_t)read] = h;
        session->send(Hash{ (uint32_t)read, h });
    }

    const double since = now();
    double last_note = since;
    while (true) {
        for (const Event& event : session->poll()) {
            handle(event);
        }
        compare_hashes();
        if (ended) {
            return false;
        }
        if (all_here(read)) {
            break;
        }
        for (int port = 0; port < match_players; port++) {
            if (received[port].size() <= read && gone[port]) {
                end_match(Reason::quit, gone_silently ? "a player stopped answering (they may have quit)" : "a player left");
                return false;
            }
        }
        const double t = now();
        if (t - since > wait_seconds) {
            end_match(Reason::timeout, "a player's inputs stopped coming");
            return false;
        }
        if (t - last_note > 2.0) {
            std::printf("[netplay] read %llu: waiting for the other players\n", (unsigned long long)read);
            last_note = t;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        lock.lock();
        if (stopping) {
            return false;
        }
    }

    for (int port = 0; port < match_players; port++) {
        const Input& input = received[port][read];
        pads[port] = { input.pad.buttons, input.pad.stick_x, input.pad.stick_y, input.pad.err_no };
        motions[port] = input.motion;
    }
    return true;
}

void conker::lockstep::end(netplay::Reason reason, const char* why) {
    std::scoped_lock lock(session_mutex);
    end_match(reason, why);
}

const std::string& conker::lockstep::why_ended() {
    return why_over;
}

void conker::lockstep::leave() {
    std::scoped_lock lock(session_mutex);
    if (session != nullptr && !left) {
        session->leave(Reason::quit);
        left = true;
    }
    ended = true;
}

void conker::lockstep::finish() {
    // On the main thread as the program quits: the game thread may still be in on_read, so it's told to
    // stop first, and the session is only touched under the lock. The session isn't deleted (the game
    // thread may still look at it), and the library is stopped next anyway.
    stopping = true;
    if (quit_watcher.joinable()) {
        quit_watcher.join();
    }
    say_goodbye();
    std::scoped_lock lock(session_mutex);
    if (library_started) {
        netplay::shutdown();
        library_started = false;
    }
}
