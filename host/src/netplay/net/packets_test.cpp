// Tests of the netplay packets (packets.cpp): each kind round-trips, its size and layout are as
// documented, and every out-of-range field, size, version and kind is refused. Exits 1 on a failure.

#include <cstdio>
#include <vector>

#include "packets.hpp"

using namespace conker::netplay;

namespace {
    int failures = 0;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("packets_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    std::vector<uint8_t> bytes_of(const Packet& packet) {
        uint8_t buffer[max_packet_size];
        const size_t size = encode(packet, buffer);
        return std::vector<uint8_t>(buffer, buffer + size);
    }

    bool decodes(const std::vector<uint8_t>& data) {
        return decode(data).has_value();
    }

    bool round_trips(const Packet& packet, size_t size) {
        const std::vector<uint8_t> data = bytes_of(packet);
        const std::optional<Packet> back = decode(data);
        return data.size() == size && back && *back == packet;
    }

    Inputs some_inputs() {
        Inputs inputs;
        inputs.port = 2;
        inputs.first_read = 1000;
        inputs.count = 3;
        inputs.pads[0] = { 0x8000, -128, 127, 0 };
        inputs.pads[1] = { 0xFF3F, 5, -5, 0 };
        inputs.pads[2] = { 0, 0, 0, err_no_controller };
        inputs.motions[0] = { -32768, 32767, 0x1234, -2, -128, 0x0305 };
        inputs.motions[2] = { 1, 0, 0, 0, 3, controls_mask };
        return inputs;
    }
}

int main() {
    // Each kind, at its documented size.
    CHECK(round_trips(Hello{ 0x0123456789ABCDEFull, 2, 3, 4, 5 }, 45));
    CHECK(round_trips(Welcome{ 1, 2 }, 7));
    CHECK(round_trips(Welcome{ 3, 4 }, 7));
    CHECK(round_trips(Refuse{ Reason::rom }, 6));
    CHECK(round_trips(some_inputs(), 139));
    CHECK(round_trips(Hash{ 4000000000u, 0xFEDCBA9876543210ull }, 17));
    CHECK(round_trips(Bye{ Reason::desync }, 6));
    CHECK(round_trips(Start{ 3, 0 }, 8));
    CHECK(round_trips(Start{ 4, 1, max_delay }, 8));

    // The layout: header, then little-endian fields.
    const std::vector<uint8_t> hello = bytes_of(Hello{ 0x0807060504030201ull, 0, 0, 0x1112131415161718ull, 0x2122232425262728ull });
    CHECK(hello[0] == 'C' && hello[1] == 'N' && hello[2] == protocol_version && hello[3] == 0 && hello[4] == 1);
    CHECK(hello[5] == 1 && hello[12] == 8 && hello[29] == 0x18 && hello[36] == 0x11 && hello[37] == 0x28 && hello[44] == 0x21);
    const std::vector<uint8_t> inputs = bytes_of(some_inputs());
    CHECK(inputs[4] == 4 && inputs[5] == 2);                                    // kind, port
    CHECK(inputs[6] == 0xE8 && inputs[7] == 0x03 && inputs[8] == 0 && inputs[9] == 0);  // first read 1000
    CHECK(inputs[10] == 3);                                                     // count
    CHECK(inputs[11] == 0x00 && inputs[12] == 0x80 && inputs[13] == 0x80 && inputs[14] == 0x7F);  // pad 0
    CHECK(inputs[51] == 0x00 && inputs[52] == 0x80 && inputs[53] == 0xFF && inputs[54] == 0x7F &&  // motion 0
          inputs[55] == 0x34 && inputs[56] == 0x12 && inputs[57] == 0xFE && inputs[58] == 0xFF && inputs[59] == 0x80 &&
          inputs[60] == 0x05 && inputs[61] == 0x03);
    const std::vector<uint8_t> start = bytes_of(Start{ 2, 1, 3 });
    CHECK(start.size() == 8 && start[5] == 2 && start[6] == 1 && start[7] == 3);

    // Out-of-range fields are neither sent nor accepted.
    CHECK(bytes_of(Welcome{ 2, 2 }).empty());       // port not below players
    CHECK(bytes_of(Welcome{ 0, 1 }).empty());       // players below 2
    CHECK(bytes_of(Welcome{ 0, 5 }).empty());       // players above 4
    CHECK(bytes_of(Start{ 1 }).empty());
    CHECK(bytes_of(Start{ 5 }).empty());
    CHECK(bytes_of(Start{ 2, 2 }).empty());          // own view is 0 or 1
    CHECK(bytes_of(Start{ 2, 0, max_delay + 1 }).empty());

    CHECK(bytes_of(Refuse{ (Reason)0 }).empty());
    CHECK(bytes_of(Bye{ (Reason)(last_reason + 1) }).empty());
    Inputs bad = some_inputs();
    bad.port = 4;
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.count = 0;
    bad.pads = {};
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.count = input_window + 1;
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.pads[0].buttons |= 0x0040;                  // a reserved button
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.pads[1].err_no = 0x04;
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.pads[5].stick_x = 1;                        // an unused pad that isn't zero
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.motions[3].wheel = 1;                       // an unused motion that isn't zero
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.motions[1].controls = (fov_codes + 1) << fov_shift;  // a field of view past the codes
    CHECK(bytes_of(bad).empty());
    bad = some_inputs();
    bad.motions[1].controls = (fov_codes << fov_shift) | controls_mask;  // the last code, every other bit: fine
    CHECK(!bytes_of(bad).empty());
    bad = some_inputs();
    bad.first_read = UINT32_MAX - 1;                // its last read past 2^32 - 1
    CHECK(bytes_of(bad).empty());
    bad.first_read = UINT32_MAX - 2;                // ...and just fitting
    CHECK(!bytes_of(bad).empty());

    // The same, arriving as bytes.
    std::vector<uint8_t> data = bytes_of(Welcome{ 1, 2 });
    data[5] = 2;
    CHECK(!decodes(data));
    data = bytes_of(Bye{ Reason::quit });
    data[5] = 0;
    CHECK(!decodes(data));
    data[5] = last_reason + 1;
    CHECK(!decodes(data));
    data = inputs;
    data[11 + 5 * 6] = 1;                           // pad 6, unused
    CHECK(!decodes(data));
    data = inputs;
    data[11] |= 0x80;                               // pad 0's reserved button
    CHECK(!decodes(data));
    data = inputs;
    data[51 + 11 * 7] = 1;                          // motion 7, unused
    CHECK(!decodes(data));
    data = inputs;
    data[51 + 11 * 1 + 10] = (uint8_t)(((fov_codes + 1) << fov_shift) >> 8);  // motion 1's field of view: past the codes
    CHECK(!decodes(data));
    data = start;
    data[7] = max_delay + 1;
    CHECK(!decodes(data));

    // Sizes, versions, kinds and the magic.
    for (size_t size = 0; size <= max_packet_size; size++) {
        std::vector<uint8_t> resized = inputs;
        resized.resize(size);
        CHECK(decodes(resized) == (size == inputs.size()));
    }
    data = hello;
    data[2]++;
    CHECK(!decodes(data));
    data = hello;
    data[3] = 1;
    CHECK(!decodes(data));
    data = hello;
    data[0] = 'X';
    CHECK(!decodes(data));
    for (int kind : { 0, 8, 255 }) {
        data = hello;
        data[4] = (uint8_t)kind;
        CHECK(!decodes(data));
    }
    data = hello;
    data[4] = 3;                                    // Refuse's kind with Hello's size
    CHECK(!decodes(data));

    if (failures == 0) {
        std::printf("packets_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
