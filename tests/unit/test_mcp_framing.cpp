// tests/unit/test_mcp_framing.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::mcp::FrameWriter and batbox::mcp::FrameReader.
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include //       tests/unit/test_mcp_framing.cpp src/mcp/McpFraming.cpp //       -o /tmp/test_mcp_framing && /tmp/test_mcp_framing
//
// With vcpkg-installed doctest:
//   c++ -std=c++20 -I include //       -I build/vcpkg_installed/arm64-osx/include //       tests/unit/test_mcp_framing.cpp src/mcp/McpFraming.cpp //       -o /tmp/test_mcp_framing && /tmp/test_mcp_framing
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/mcp/McpFraming.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using batbox::mcp::FrameReader;
using batbox::mcp::FrameWriter;
using batbox::mcp::frame_message;
// ============================================================================
// TEST SUITE 1: FrameWriter
// ============================================================================

TEST_SUITE("FrameWriter -- encode") {

    TEST_CASE("simple JSON payload produces correct header") {
        FrameWriter w;
        const std::string payload = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
        std::string wire = w.encode(payload);

        std::string expected_header =
            "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";

        REQUIRE(wire.size() == expected_header.size() + payload.size());
        CHECK(wire.substr(0, expected_header.size()) == expected_header);
        CHECK(wire.substr(expected_header.size()) == payload);
    }

    TEST_CASE("empty payload produces Content-Length: 0") {
        FrameWriter w;
        std::string wire = w.encode("");
        CHECK(wire == "Content-Length: 0\r\n\r\n");
    }

    TEST_CASE("frame_message free function matches FrameWriter") {
        const std::string_view payload = R"({"method":"initialize"})";
        CHECK(frame_message(payload) == FrameWriter{}.encode(payload));
    }

    TEST_CASE("single-char payload") {
        FrameWriter w;
        std::string wire = w.encode("x");
        CHECK(wire == "Content-Length: 1\r\n\r\nx");
    }

    TEST_CASE("large payload 1 MiB") {
        FrameWriter w;
        const std::string big(1024 * 1024, 'A');
        std::string wire = w.encode(big);
        const std::string expected_hdr = "Content-Length: 1048576\r\n\r\n";
        REQUIRE(wire.size() == expected_hdr.size() + big.size());
        CHECK(wire.substr(0, expected_hdr.size()) == expected_hdr);
        CHECK(wire.substr(expected_hdr.size()) == big);
    }
}

// ============================================================================
// TEST SUITE 2: Write-Read round-trip
// ============================================================================

TEST_SUITE("FrameWriter and FrameReader -- round-trip") {

    TEST_CASE("single message fed in one chunk") {
        const std::string payload = R"({"jsonrpc":"2.0","id":1,"result":"ok"})";
        std::string wire = FrameWriter{}.encode(payload);

        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == payload);
        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("round-trip with Unicode payload") {
        // UTF-8 bytes for a string containing Japanese text and an emoji
        const std::string payload = R"({"text":"\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf","emoji":"\xf0\x9f\x8e\x89"})";
        std::string wire = FrameWriter{}.encode(payload);

        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == payload);
        CHECK(r.errors().empty());
    }

    TEST_CASE("round-trip with escaped payload") {
        const std::string payload = R"({"data":"hello world"})";
        std::string wire = FrameWriter{}.encode(payload);
        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == payload);
    }
}

// ============================================================================
// TEST SUITE 3: Partial chunk feeds
// ============================================================================

TEST_SUITE("FrameReader -- partial chunk feeds") {

    TEST_CASE("header arrives byte-by-byte") {
        const std::string payload = R"({"method":"ping"})";
        std::string wire = FrameWriter{}.encode(payload);

        FrameReader r;
        std::vector<std::string> all;
        for (char c : wire) {
            auto msgs = r.feed(std::string_view(&c, 1));
            for (auto& m : msgs) all.push_back(std::move(m));
        }
        REQUIRE(all.size() == 1);
        CHECK(all[0] == payload);
        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("header split exactly at CRLF separator") {
        const std::string payload = R"({"id":42})";
        std::string wire = FrameWriter{}.encode(payload);

        std::string header_part = "Content-Length: " +
                                   std::to_string(payload.size()) + "\r\n\r\n";
        REQUIRE(wire.substr(0, header_part.size()) == header_part);

        FrameReader r;
        auto msgs1 = r.feed(header_part);
        CHECK(msgs1.empty());
        CHECK(r.errors().empty());

        auto msgs2 = r.feed(payload);
        REQUIRE(msgs2.size() == 1);
        CHECK(msgs2[0] == payload);
        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("body split across two chunks") {
        const std::string payload = R"({"long_key":"some value here"})";
        std::string wire = FrameWriter{}.encode(payload);

        FrameReader r;
        const std::size_t half = wire.size() / 2;
        auto msgs1 = r.feed(std::string_view(wire).substr(0, half));
        CHECK(msgs1.empty());

        auto msgs2 = r.feed(std::string_view(wire).substr(half));
        REQUIRE(msgs2.size() == 1);
        CHECK(msgs2[0] == payload);
        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("body split into single-byte chunks") {
        const std::string payload = R"({"seq":99})";
        std::string wire = FrameWriter{}.encode(payload);

        FrameReader r;
        std::vector<std::string> all;
        std::string hdr = "Content-Length: " +
                           std::to_string(payload.size()) + "\r\n\r\n";
        (void)r.feed(hdr);
        for (char c : payload) {
            auto msgs = r.feed(std::string_view(&c, 1));
            for (auto& m : msgs) all.push_back(std::move(m));
        }
        REQUIRE(all.size() == 1);
        CHECK(all[0] == payload);
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("no messages returned until frame is complete") {
        const std::string payload(256, 'Z');
        std::string wire = FrameWriter{}.encode(payload);

        FrameReader r;
        auto partial = r.feed(std::string_view(wire).substr(0, wire.size() - 1));
        CHECK(partial.empty());
        CHECK(r.pending_bytes() == wire.size() - 1);

        auto last_byte = std::string_view(wire).substr(wire.size() - 1, 1);
        auto complete = r.feed(last_byte);
        REQUIRE(complete.size() == 1);
        CHECK(complete[0] == payload);
        CHECK(r.pending_bytes() == 0);
    }
}

// ============================================================================
// TEST SUITE 4: Multiple frames in one chunk
// ============================================================================

TEST_SUITE("FrameReader -- multiple frames in one chunk") {

    TEST_CASE("two frames concatenated -- both returned from one feed") {
        const std::string p1 = R"({"id":1})";
        const std::string p2 = R"({"id":2})";
        std::string wire = FrameWriter{}.encode(p1) + FrameWriter{}.encode(p2);

        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == 2);
        CHECK(msgs[0] == p1);
        CHECK(msgs[1] == p2);
        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("five frames in one feed") {
        std::vector<std::string> payloads = {
            R"({"method":"a"})",
            R"({"method":"b","params":[1,2]})",
            R"({"result":null,"id":3})",
            R"({"error":{"code":-32700,"message":"Parse error"}})",
            R"({"method":"notifications/message","params":{"level":"info"}})",
        };
        std::string wire;
        for (auto& p : payloads) wire += FrameWriter{}.encode(p);

        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == payloads.size());
        for (std::size_t i = 0; i < payloads.size(); ++i)
            CHECK(msgs[i] == payloads[i]);
        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("two frames: first complete, second partial") {
        const std::string p1 = R"({"id":10})";
        const std::string p2 = R"({"id":20,"data":"hello"})";
        std::string wire = FrameWriter{}.encode(p1) + FrameWriter{}.encode(p2);

        FrameReader r;
        auto msgs = r.feed(std::string_view(wire).substr(0, wire.size() - 1));
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == p1);

        auto rest = r.feed(std::string_view(wire).substr(wire.size() - 1, 1));
        REQUIRE(rest.size() == 1);
        CHECK(rest[0] == p2);
        CHECK(r.errors().empty());
    }

    TEST_CASE("frame split across three feeds with third containing second frame") {
        const std::string p1 = R"({"id":1,"method":"ping"})";
        const std::string p2 = R"({"id":2,"result":"pong"})";
        std::string wire1 = FrameWriter{}.encode(p1);
        std::string wire2 = FrameWriter{}.encode(p2);

        const std::size_t half = wire1.size() / 2;
        FrameReader r;

        auto m1 = r.feed(std::string_view(wire1).substr(0, half));
        CHECK(m1.empty());

        auto m2 = r.feed(std::string_view(wire1).substr(half));
        REQUIRE(m2.size() == 1);
        CHECK(m2[0] == p1);

        auto m3 = r.feed(wire2);
        REQUIRE(m3.size() == 1);
        CHECK(m3[0] == p2);

        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }
}

// ============================================================================
// TEST SUITE 5: Malformed frame handling
// ============================================================================

TEST_SUITE("FrameReader -- malformed input recovery") {

    TEST_CASE("no Content-Length header -- error logged") {
        const std::string bad_frame = "X-Custom: value\r\n\r\nbody";
        const std::string good = R"({"ok":true})";
        std::string wire = bad_frame + FrameWriter{}.encode(good);

        FrameReader r;
        (void)r.feed(wire);
        REQUIRE(!r.errors().empty());
        r.clear_errors();
        CHECK(r.errors().empty());
    }

    TEST_CASE("non-numeric Content-Length value") {
        const std::string bad_frame = "Content-Length: abc\r\n\r\nhello";
        const std::string good = R"({"method":"ok"})";
        std::string wire = bad_frame + FrameWriter{}.encode(good);

        FrameReader r;
        (void)r.feed(wire);
        REQUIRE(!r.errors().empty());
        bool found = false;
        for (auto& e : r.errors())
            if (e.find("invalid Content-Length") != std::string::npos) { found = true; break; }
        CHECK(found);
        r.clear_errors();
    }

    TEST_CASE("negative Content-Length leading minus") {
        const std::string bad_frame = "Content-Length: -1\r\n\r\nhello";
        FrameReader r;
        (void)r.feed(bad_frame);
        REQUIRE(!r.errors().empty());
        bool found = false;
        for (auto& e : r.errors())
            if (e.find("invalid Content-Length") != std::string::npos) { found = true; break; }
        CHECK(found);
    }

    TEST_CASE("errors cleared by clear_errors") {
        FrameReader r;
        (void)r.feed("Content-Length: xyz\r\n\r\nbad");
        REQUIRE(!r.errors().empty());
        r.clear_errors();
        CHECK(r.errors().empty());
    }

    TEST_CASE("valid frame after bad frame is decoded correctly") {
        const std::string bad  = "Content-Length: NOTANUMBER\r\n\r\njunk";
        const std::string good = R"({"id":99})";
        std::string good_wire  = FrameWriter{}.encode(good);

        FrameReader r;
        (void)r.feed(bad);
        REQUIRE(!r.errors().empty());

        auto msgs = r.feed(good_wire);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == good);
    }

    TEST_CASE("reset clears buffer and errors") {
        FrameReader r;
        (void)r.feed("Content-Length: 5\r\n\r\nAB");
        CHECK(r.pending_bytes() > 0);

        r.reset();
        CHECK(r.pending_bytes() == 0);
        CHECK(r.errors().empty());

        const std::string payload = R"({"fresh":"start"})";
        auto msgs = r.feed(FrameWriter{}.encode(payload));
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == payload);
    }
}

// ============================================================================
// TEST SUITE 6: Large payload stress
// ============================================================================

TEST_SUITE("FrameReader -- large payloads") {

    TEST_CASE("1 MiB payload round-trips correctly") {
        const std::string big_payload(1024 * 1024, 'B');
        std::string wire = FrameWriter{}.encode(big_payload);

        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].size() == big_payload.size());
        CHECK(msgs[0] == big_payload);
        CHECK(r.errors().empty());
    }

    TEST_CASE("1 MiB payload fed in 4 KB chunks") {
        const std::string big_payload(1024 * 1024, 'C');
        std::string wire = FrameWriter{}.encode(big_payload);

        FrameReader r;
        std::vector<std::string> all;
        constexpr std::size_t kChunk = 4096;
        for (std::size_t offset = 0; offset < wire.size(); offset += kChunk) {
            std::size_t len = std::min(kChunk, wire.size() - offset);
            auto msgs = r.feed(std::string_view(wire).substr(offset, len));
            for (auto& m : msgs) all.push_back(std::move(m));
        }
        REQUIRE(all.size() == 1);
        CHECK(all[0] == big_payload);
        CHECK(r.errors().empty());
        CHECK(r.pending_bytes() == 0);
    }

    TEST_CASE("100 small messages round-trip via sequential feed") {
        std::vector<std::string> payloads;
        payloads.reserve(100);
        for (int i = 0; i < 100; ++i)
            payloads.push_back(R"({"seq":)" + std::to_string(i) + "}");

        FrameReader r;
        std::vector<std::string> received;
        for (auto& p : payloads) {
            auto msgs = r.feed(FrameWriter{}.encode(p));
            for (auto& m : msgs) received.push_back(std::move(m));
        }
        REQUIRE(received.size() == 100);
        for (int i = 0; i < 100; ++i)
            CHECK(received[static_cast<std::size_t>(i)] == payloads[static_cast<std::size_t>(i)]);
        CHECK(r.errors().empty());
    }

    TEST_CASE("fuzz style 100 messages fed in 37-byte chunks") {
        std::vector<std::string> payloads;
        std::string all_wire;
        for (int i = 0; i < 100; ++i) {
            std::string p = R"({"fuzz":)" + std::to_string(i) + "}";
            payloads.push_back(p);
            all_wire += FrameWriter{}.encode(p);
        }

        FrameReader r;
        std::vector<std::string> received;
        constexpr std::size_t kChunk = 37;
        for (std::size_t offset = 0; offset < all_wire.size(); offset += kChunk) {
            std::size_t len = std::min(kChunk, all_wire.size() - offset);
            auto msgs = r.feed(std::string_view(all_wire).substr(offset, len));
            for (auto& m : msgs) received.push_back(std::move(m));
        }
        REQUIRE(received.size() == 100);
        for (std::size_t i = 0; i < payloads.size(); ++i)
            CHECK(received[i] == payloads[i]);
        CHECK(r.errors().empty());
    }
}

// ============================================================================
// TEST SUITE 7: Optional Content-Type header tolerance
// ============================================================================

TEST_SUITE("FrameReader -- optional Content-Type header") {

    TEST_CASE("Content-Type header present and accepted") {
        const std::string payload = R"({"method":"initialize"})";
        std::string wire = "Content-Length: " +
                            std::to_string(payload.size()) +
                            "\r\nContent-Type: application/vnd.schemastore.json; charset=utf-8\r\n\r\n" +
                            payload;

        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == payload);
        CHECK(r.errors().empty());
    }

    TEST_CASE("Content-Type before Content-Length still works") {
        const std::string payload = R"({"method":"ping"})";
        std::string wire = "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(payload.size()) + "\r\n\r\n" + payload;

        FrameReader r;
        auto msgs = r.feed(wire);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0] == payload);
        CHECK(r.errors().empty());
    }
}
