// The determinism harness (netplay, issue #126): before two machines can play in lockstep, the same
// inputs have to give the same game on each. This records the game's controller inputs and replays
// them, and logs a hash of the game's memory at each controller read, so two runs can be compared
// read by read (tools/netplay/compare_hashes.py finds the first read they differ at, and where).
// Built only with CONKER_NETPLAY.
//
//   --record <file>     writes the pads the game reads, at each controller read, to <file>
//   --replay <file>     gives the game the pads from <file> instead of the controllers' (it starts the
//                       game directly), and quits after the file's last read
//   --hash-log <file>   one line per controller read: its number and the XXH3-64 of the game's memory as
//                       the read is made (so it's the result of the reads before it)
//   --dump-at <n>       also writes the game's memory at read <n> to <hash-log>.<n>.bin, big-endian (the
//                       N64's byte order, as tools/netplay reads it); it can be given more than once
//   --ignore <a>:<n>    leaves <n> bytes from address <a> (both in hex, e.g. 800BE6F0:8) out of the hash
//                       and zeroes them in dumps, for differences already known (it can be given more
//                       than once; ranges are widened to whole 32-bit words)
//   --stop-after <n>    quits after read <n>
//   --ports <n>         the controllers the game sees: ports 1 to <n> connected, no paks (1 by default; a
//                       replay uses its recording's)
//   --real-time         runs the game on the time, as normally, not the virtual timeline (below), to compare
//   --fast              doesn't wait for real time on the timeline: as fast as the host can
//   --state-test <s>:<l>  takes a save state after read <s> and loads it at read <l> (later), so the
//                       game goes back to <s> and on again from there: the hash log then holds the
//                       reads after the load in place of the first ones (they should match a run
//                       without it). The keys' save states (F5, F7) don't work in that run
//   --jitter <us>       sleeps at random, up to <us> microseconds, at the game's OS calls and in the
//                       graphics thread: a test that the host's speed doesn't change the game
//
// Controller reads are counted from the first, so read <n> of one run is read <n> of another. The pads
// are the N64's own values (after the sticks are converted), as osContGetReadData gives them to the
// game: what netplay will send.
//
// While the harness is used, the game runs on the runtime's virtual timeline (deterministic mode,
// ultramodern/timeline.hpp): its clock, VIs, timers and task completions come at fixed points in its
// progress, not when the host gets to them, so the same inputs give the same game.
//
// The recording: "CBFDPADS", then a little-endian u32 version (1) and u32 port count, then for each
// read the number of controllers the game reads (u8) and four pads of 5 bytes each (u16 buttons, s8
// stick x, s8 stick y, u8 err_no).

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../conker.hpp"
#include "librecomp/save_states.hpp"
#include "recomp.h"
#include "ultramodern/save_states.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/timeline.hpp"
#include "ultramodern/ultramodern.hpp"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

namespace {
    constexpr char magic[8] = { 'C', 'B', 'F', 'D', 'P', 'A', 'D', 'S' };
    constexpr uint32_t version = 1;
    constexpr size_t header_size = 16;
    constexpr int max_pads = 4;
    constexpr size_t pad_size = 5;
    constexpr size_t read_size = 1 + max_pads * pad_size;
    constexpr uint64_t no_limit = UINT64_MAX;

    std::string record_path;
    std::string replay_path;
    std::string hash_log_path;
    std::vector<uint64_t> dump_at;
    uint64_t stop_after = no_limit;
    int ports = 1;
    // --state-test's.
    uint64_t state_save_at = no_limit;
    uint64_t state_load_at = no_limit;
    enum class StateRequest { None, Save, Load };
    StateRequest state_request = StateRequest::None;
    std::vector<uint8_t> state;
    std::vector<uint8_t> state_host;
    uint64_t state_next_read = 0;
    long state_log_position = 0;
    bool real_time = false;
    bool fast = false;
    uint64_t jitter_us = 0;
    bool options_valid = true;

    uint8_t* rdram = nullptr;
    FILE* record_file = nullptr;
    FILE* hash_log = nullptr;
    std::vector<uint8_t> replay;
    uint64_t replay_reads = 0;
    // Reads past this aren't logged, and the game is quit at the first.
    uint64_t last_read = no_limit;
    uint64_t reads = 0;

    // The game is quit from a thread of its own, not the game thread that reads the controllers.
    std::thread quitter;
    std::mutex quit_mutex;
    std::condition_variable quit_wanted;
    bool quit_requested = false;
    bool quit_finished = false;

    bool active() {
        return !record_path.empty() || !replay_path.empty() || !hash_log_path.empty();
    }

    bool parse_count(const char* text, uint64_t& out) {
        char* end = nullptr;
        unsigned long long value = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0') {
            return false;
        }
        out = value;
        return true;
    }

    void put_u32(uint8_t* out, uint32_t value) {
        for (int i = 0; i < 4; i++) {
            out[i] = (uint8_t)(value >> (8 * i));
        }
    }

    uint32_t get_u32(const uint8_t* in) {
        return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    }

    // The whole recording, checked before the game starts: its header, its size and every read's
    // controller count, so a read during the game can't go wrong.
    bool load_replay() {
        FILE* file = std::fopen(replay_path.c_str(), "rb");
        if (file == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't open it\n", replay_path.c_str());
            return false;
        }
        uint8_t buffer[65536];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            replay.insert(replay.end(), buffer, buffer + got);
        }
        std::fclose(file);
        if (replay.size() < header_size || std::memcmp(replay.data(), magic, sizeof(magic)) != 0) {
            std::fprintf(stderr, "[harness] %s isn't a recording of inputs\n", replay_path.c_str());
            return false;
        }
        if (get_u32(replay.data() + 8) != version) {
            std::fprintf(stderr, "[harness] %s is a version %u recording; this build reads version %u\n",
                replay_path.c_str(), get_u32(replay.data() + 8), version);
            return false;
        }
        const uint32_t recorded_ports = get_u32(replay.data() + 12);
        if (recorded_ports < 1 || recorded_ports > max_pads) {
            std::fprintf(stderr, "[harness] %s has %u ports\n", replay_path.c_str(), recorded_ports);
            return false;
        }
        if ((replay.size() - header_size) % read_size != 0) {
            std::fprintf(stderr, "[harness] %s ends partway through a read\n", replay_path.c_str());
            return false;
        }
        replay_reads = (replay.size() - header_size) / read_size;
        for (uint64_t i = 0; i < replay_reads; i++) {
            if (replay[header_size + i * read_size] > max_pads) {
                std::fprintf(stderr, "[harness] %s: read %llu has %u controllers\n", replay_path.c_str(),
                    (unsigned long long)i, replay[header_size + i * read_size]);
                return false;
            }
        }
        ports = (int)recorded_ports;
        std::printf("[harness] replaying %llu reads from %s (%d ports)\n", (unsigned long long)replay_reads,
            replay_path.c_str(), ports);
        return true;
    }

    // What's left out of the hash and zeroed in dumps, as offsets into game memory: whole 32-bit words,
    // so they're the same bytes in the runtime's order and the N64's.
    struct Range {
        uint32_t offset;
        uint32_t size;
    };
    // --ignore's.
    std::vector<Range> ignored;

    // Where the runtime keeps a host pointer in game memory, which differs from run to run: each game
    // thread's OSThread holds its native context (8 bytes at 0x20, ultramodern's ultra64.h).
    constexpr uint32_t thread_context_offset = 0x20;
    constexpr uint32_t thread_context_size = 8;

    // The runtime's pointers and --ignore's ranges, in order.
    std::vector<Range> left_out() {
        std::vector<Range> out = ignored;
        for (const auto& thread : ultramodern::save_states::get_threads(rdram)) {
            const uint32_t offset = (uint32_t)thread.thread - 0x80000000u + thread_context_offset;
            if (offset + thread_context_size <= recomp::save_states::game_memory_size) {
                out.push_back({ offset, thread_context_size });
            }
        }
        std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
        return out;
    }

    uint64_t hash_memory() {
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        uint32_t at = 0;
        for (const Range& skip : left_out()) {
            if (skip.offset > at) {
                XXH3_64bits_update(&state, rdram + at, skip.offset - at);
            }
            at = std::max(at, skip.offset + skip.size);
        }
        XXH3_64bits_update(&state, rdram + at, recomp::save_states::game_memory_size - at);
        return XXH3_64bits_digest(&state);
    }

    void dump_memory(uint64_t read) {
        const std::string path = hash_log_path + "." + std::to_string(read) + ".bin";
        FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't create it\n", path.c_str());
            return;
        }
        // The runtime keeps each 32-bit word of game memory in the host's order; the dump has the N64's.
        std::vector<uint8_t> image(recomp::save_states::game_memory_size);
        for (size_t i = 0; i < image.size(); i += 4) {
            image[i + 0] = rdram[i + 3];
            image[i + 1] = rdram[i + 2];
            image[i + 2] = rdram[i + 1];
            image[i + 3] = rdram[i + 0];
        }
        for (const Range& skip : left_out()) {
            std::fill_n(image.begin() + skip.offset, skip.size, 0);
        }
        std::fwrite(image.data(), 1, image.size(), file);
        std::fclose(file);
        std::printf("[harness] read %llu: memory written to %s\n", (unsigned long long)read, path.c_str());
    }

    // At a safe point (a delivery point, on the idle thread), with a --state-test save or load asked for.
    void on_safe_point(uint8_t* game_rdram) {
        if (state_request == StateRequest::None || !ultramodern::save_states::is_quiet()) {
            return;
        }
        std::string error;
        if (state_request == StateRequest::Save) {
            if (!recomp::save_states::capture(game_rdram, state, error)) {
                return;
            }
            state_host = conker::host_state::save();
            // The next read is the state's, and the hash log goes on from there.
            state_next_read = reads;
            if (hash_log != nullptr) {
                std::fflush(hash_log);
                state_log_position = std::ftell(hash_log);
            }
            std::printf("[harness] state taken before read %llu (%zu bytes)\n", (unsigned long long)state_next_read, state.size());
        }
        else {
            switch (recomp::save_states::restore(game_rdram, state, error)) {
                case recomp::save_states::LoadResult::Loaded:
                    conker::host_state::load(state_host);
                    std::printf("[harness] state loaded before read %llu: back to read %llu\n", (unsigned long long)reads,
                        (unsigned long long)state_next_read);
                    reads = state_next_read;
                    if (hash_log != nullptr) {
                        std::fflush(hash_log);
                        std::fseek(hash_log, state_log_position, SEEK_SET);
                    }
                    break;
                case recomp::save_states::LoadResult::NotNow:
                    return;
                case recomp::save_states::LoadResult::Invalid:
                    std::fprintf(stderr, "[harness] the state couldn't be loaded: %s\n", error.c_str());
                    break;
            }
        }
        state_request = StateRequest::None;
        ultramodern::save_states::set_safe_points_wanted(false);
    }

    void request_quit() {
        std::lock_guard lock(quit_mutex);
        quit_requested = true;
        quit_wanted.notify_one();
    }

    // On the game thread, at each controller read, with the pads the game is about to get.
    void on_read_data(ultramodern::input::Pad* pads, int count) {
        const uint64_t read = reads++;
        // Each once (after the load the game passes read <l> again).
        if (read == state_save_at || read == state_load_at) {
            state_request = read == state_save_at ? StateRequest::Save : StateRequest::Load;
            (read == state_save_at ? state_save_at : state_load_at) = no_limit;
            ultramodern::save_states::set_safe_points_wanted(true);
        }
        if (read > last_read) {
            // Waiting for the quit: the game gets no input.
            for (int i = 0; i < max_pads; i++) {
                pads[i] = { 0, 0, 0, pads[i].err_no };
            }
            return;
        }

        if (hash_log != nullptr) {
            const uint64_t hash = hash_memory();
            std::fprintf(hash_log, "%llu %016llx\n", (unsigned long long)read, (unsigned long long)hash);
            if (std::find(dump_at.begin(), dump_at.end(), read) != dump_at.end()) {
                dump_memory(read);
            }
        }

        if (!replay.empty()) {
            const uint8_t* in = replay.data() + header_size + read * read_size;
            for (int i = 0; i < max_pads; i++) {
                const uint8_t* pad = in + 1 + i * pad_size;
                pads[i].button = (uint16_t)(pad[0] | (pad[1] << 8));
                pads[i].stick_x = (int8_t)pad[2];
                pads[i].stick_y = (int8_t)pad[3];
                pads[i].err_no = pad[4];
            }
            if (in[0] != count) {
                std::fprintf(stderr, "[harness] read %llu: the game reads %d controllers, the recording %u\n",
                    (unsigned long long)read, count, in[0]);
            }
        }

        if (record_file != nullptr) {
            uint8_t out[read_size];
            out[0] = (uint8_t)count;
            for (int i = 0; i < max_pads; i++) {
                uint8_t* pad = out + 1 + i * pad_size;
                pad[0] = (uint8_t)pads[i].button;
                pad[1] = (uint8_t)(pads[i].button >> 8);
                pad[2] = (uint8_t)pads[i].stick_x;
                pad[3] = (uint8_t)pads[i].stick_y;
                pad[4] = pads[i].err_no;
            }
            std::fwrite(out, 1, sizeof(out), record_file);
            // Written out every 2 s of game or so: quitting can crash (issue #127) before finish() closes it.
            if (read % 60 == 59) {
                std::fflush(record_file);
            }
        }

        if (read == last_read) {
            // Written out now: quitting can crash on Linux (issue #127), before finish() closes them.
            if (record_file != nullptr) {
                std::fflush(record_file);
            }
            if (hash_log != nullptr) {
                std::fflush(hash_log);
            }
            std::printf("[harness] read %llu was the last: quitting\n", (unsigned long long)read);
            request_quit();
        }
    }
}

bool conker::harness::parse_option(int argc, char** argv, int& i) {
    const char* option = argv[i];
    const bool has_value = i + 1 < argc;
    auto count_option = [&](uint64_t& out) {
        if (!has_value || !parse_count(argv[i + 1], out)) {
            std::fprintf(stderr, "[harness] %s needs a number\n", option);
            options_valid = false;
        }
        if (has_value) {
            i++;
        }
    };
    std::string* path = nullptr;
    if (std::strcmp(option, "--record") == 0) {
        path = &record_path;
    }
    else if (std::strcmp(option, "--replay") == 0) {
        path = &replay_path;
    }
    else if (std::strcmp(option, "--hash-log") == 0) {
        path = &hash_log_path;
    }
    if (path != nullptr) {
        if (!has_value) {
            std::fprintf(stderr, "[harness] %s needs a file\n", option);
            options_valid = false;
            return true;
        }
        *path = argv[++i];
        return true;
    }
    if (std::strcmp(option, "--dump-at") == 0) {
        uint64_t value = 0;
        count_option(value);
        dump_at.push_back(value);
        return true;
    }
    if (std::strcmp(option, "--ignore") == 0) {
        unsigned long long address = 0, size = 0;
        char end = 0;
        if (!has_value || std::sscanf(argv[i + 1], "%llx:%llx%c", &address, &size, &end) != 2 ||
            address < 0x80000000ull || size == 0 || address + size > 0x80000000ull + recomp::save_states::game_memory_size) {
            std::fprintf(stderr, "[harness] --ignore needs <address>:<size>, in hex, inside game memory\n");
            options_valid = false;
        }
        else {
            // Out to whole words.
            const uint32_t first = (uint32_t)(address - 0x80000000ull) & ~3u;
            const uint32_t last = ((uint32_t)(address + size - 0x80000000ull) + 3u) & ~3u;
            ignored.push_back({ first, last - first });
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--stop-after") == 0) {
        count_option(stop_after);
        return true;
    }
    if (std::strcmp(option, "--real-time") == 0) {
        real_time = true;
        return true;
    }
    if (std::strcmp(option, "--state-test") == 0) {
        unsigned long long save_at = 0, load_at = 0;
        char end = 0;
        if (!has_value || std::sscanf(argv[i + 1], "%llu:%llu%c", &save_at, &load_at, &end) != 2 || load_at <= save_at) {
            std::fprintf(stderr, "[harness] --state-test needs <save read>:<load read>, the load after the save\n");
            options_valid = false;
        }
        else {
            state_save_at = save_at;
            state_load_at = load_at;
        }
        if (has_value) {
            i++;
        }
        return true;
    }
    if (std::strcmp(option, "--fast") == 0) {
        fast = true;
        return true;
    }
    if (std::strcmp(option, "--jitter") == 0) {
        count_option(jitter_us);
        return true;
    }
    if (std::strcmp(option, "--ports") == 0) {
        uint64_t value = 1;
        count_option(value);
        if (value < 1 || value > max_pads) {
            std::fprintf(stderr, "[harness] --ports is 1 to 4\n");
            options_valid = false;
            value = 1;
        }
        ports = (int)value;
        return true;
    }
    return false;
}

bool conker::harness::prepare() {
    if (!options_valid) {
        return false;
    }
    if (!active()) {
        return true;
    }
    if (!record_path.empty() && !replay_path.empty()) {
        std::fprintf(stderr, "[harness] --record and --replay can't be used together\n");
        return false;
    }
    if (!dump_at.empty() && hash_log_path.empty()) {
        std::fprintf(stderr, "[harness] --dump-at needs --hash-log (the dump is named after it)\n");
        return false;
    }
    if (!replay_path.empty() && !load_replay()) {
        return false;
    }
    if (!record_path.empty()) {
        record_file = std::fopen(record_path.c_str(), "wb");
        if (record_file == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't create it\n", record_path.c_str());
            return false;
        }
        uint8_t header[header_size];
        std::memcpy(header, magic, sizeof(magic));
        put_u32(header + 8, version);
        put_u32(header + 12, (uint32_t)ports);
        std::fwrite(header, 1, sizeof(header), record_file);
        std::printf("[harness] recording to %s (%d ports)\n", record_path.c_str(), ports);
    }
    if (!hash_log_path.empty()) {
        hash_log = std::fopen(hash_log_path.c_str(), "w");
        if (hash_log == nullptr) {
            std::fprintf(stderr, "[harness] %s: couldn't create it\n", hash_log_path.c_str());
            return false;
        }
    }
    last_read = stop_after;
    if (!replay.empty() && replay_reads - 1 < last_read) {
        last_read = replay_reads - 1;
    }
    else if (!replay_path.empty() && replay_reads == 0) {
        std::fprintf(stderr, "[harness] %s has no reads\n", replay_path.c_str());
        return false;
    }

    quitter = std::thread([] {
        std::unique_lock lock(quit_mutex);
        quit_wanted.wait(lock, [] { return quit_requested || quit_finished; });
        if (quit_requested && !quit_finished) {
            lock.unlock();
            ultramodern::quit();
        }
    });
    return true;
}

bool conker::harness::replaying() {
    return !replay_path.empty();
}

bool conker::harness::fixes_ports() {
    return active();
}

ultramodern::input::connected_device_info_t conker::harness::connected_device_info(int controller_num) {
    if (controller_num < ports) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// Every run starts from an empty save, in a folder of the harness's own (saves/harness, emptied here):
// the player's own save is left alone. The save file is switched once saving has started, before any
// of the game's code runs.
void conker::harness::entrypoint(uint8_t* game_rdram, recomp_context* ctx) {
    const std::filesystem::path folder = ultramodern::get_save_file_path().parent_path() / "harness";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (error) {
        std::fprintf(stderr, "[harness] couldn't empty %s: the game may start from its save\n", folder.string().c_str());
    }
    ultramodern::change_save_file(u8"harness", u8"empty");
    recomp_entrypoint(game_rdram, ctx);
}

bool conker::harness::active() {
    return ::active();
}

void conker::harness::start(uint8_t* game_rdram) {
    if (!active()) {
        return;
    }
    rdram = game_rdram;
    if (!real_time) {
        ultramodern::timeline::enable(fast);
    }
    ultramodern::timeline::set_jitter((uint32_t)std::min<uint64_t>(jitter_us, 1'000'000));
    ultramodern::input::set_read_data_callback(on_read_data);
    if (state_load_at != no_limit) {
        ultramodern::save_states::set_safe_point_callback(on_safe_point);
    }
}

void conker::harness::finish() {
    if (quitter.joinable()) {
        {
            std::lock_guard lock(quit_mutex);
            quit_finished = true;
            quit_wanted.notify_one();
        }
        quitter.join();
    }
    if (record_file != nullptr) {
        std::fclose(record_file);
        record_file = nullptr;
        std::printf("[harness] recorded %llu reads\n", (unsigned long long)(reads <= last_read ? reads : last_read + 1));
    }
    if (hash_log != nullptr) {
        std::fclose(hash_log);
        hash_log = nullptr;
    }
}
