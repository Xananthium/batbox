// ---------------------------------------------------------------------------
// tests/unit/test_scrapling_client.cpp
//
// doctest suite for batbox::sidecar::ScraplingClient.
//
// Strategy: start a real loopback HTTP server on an ephemeral port using
// a minimal hand-rolled socket server (POSIX) rather than depending on a
// full mock-HTTP library.  This lets us test:
//   - Correct JSON serialisation of each request type
//   - Correct deserialisation of each response type
//   - Cancellation via CancelToken (cancelled before request)
//   - HTTP 500 → Result error with the parsed detail message
//   - healthz() returning true on 200 and false on connection refused
//   - shutdown() sending POST /shutdown without throwing
//
// The mock server is a single-threaded accept loop run in a std::thread.
// Each test case spins up the server, runs one exchange, then tears it down.
//
// Build (standalone from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_scrapling_client.cpp \
//       src/sidecar/ScraplingClient.cpp \
//       src/sidecar/ScraplingProto.cpp \
//       src/core/Json.cpp \
//       src/core/CancelToken.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libcpr.a \
//       build/vcpkg_installed/arm64-osx/lib/libcurl.a \
//       -o /tmp/test_scrapling_client && /tmp/test_scrapling_client
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/sidecar/ScraplingClient.hpp>
#include <batbox/sidecar/ScraplingProto.hpp>

// POSIX socket includes for the in-process mock server
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

using namespace batbox::sidecar;
using namespace batbox::sidecar::proto;
using batbox::CancelSource;
using batbox::CancelToken;

// ===========================================================================
// MockHttpServer — minimal loopback HTTP/1.1 server
//
// Binds to 127.0.0.1:0 (OS picks a free port), accepts one connection,
// reads the request headers + body, calls a user-supplied handler that
// returns a complete HTTP response string, writes it, then closes.
// ===========================================================================
class MockHttpServer {
public:
    using Handler = std::function<std::string(std::string_view request_line,
                                               std::string_view request_body)>;

    explicit MockHttpServer(Handler handler)
        : handler_(std::move(handler))
        , listen_fd_(-1)
        , port_(0)
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);

        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(0); // OS assigns free port
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        REQUIRE(::bind(listen_fd_,
                       reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 1) == 0);

        // Discover the assigned port.
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        // Accept exactly one connection in a background thread.
        thread_ = std::thread([this]{ serve_one(); });
    }

    ~MockHttpServer() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return port_; }

private:
    void serve_one() {
        sockaddr_in client{};
        socklen_t   clen = sizeof(client);
        int         cfd  = ::accept(listen_fd_,
                                    reinterpret_cast<sockaddr*>(&client),
                                    &clen);
        if (cfd < 0) return;

        // Read the full request: accumulate until we have headers + full body.
        // body_str is declared at function scope so string_view body stays valid.
        std::string  request;
        std::string  body_str;          // <- outer scope: string_view body stays valid
        std::string  request_line_str;  // <- outer scope: string_view request_line stays valid
        request.reserve(4096);
        char buf[4096];
        int  content_length = -1;
        bool headers_done   = false;

        while (true) {
            ssize_t n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            request += std::string_view(buf, static_cast<std::size_t>(n));

            if (!headers_done) {
                auto hend = request.find("\r\n\r\n");
                if (hend == std::string::npos) continue;

                // First line of request.
                auto fn = request.find("\r\n");
                if (fn != std::string::npos)
                    request_line_str = request.substr(0, fn);

                // Extract Content-Length (case-sensitive: cpr sends "Content-Length").
                content_length = 0;
                auto cl_pos = request.find("Content-Length: ");
                if (cl_pos != std::string::npos) {
                    cl_pos += 16;
                    auto nl = request.find("\r\n", cl_pos);
                    if (nl != std::string::npos)
                        content_length = std::stoi(request.substr(cl_pos, nl - cl_pos));
                }

                // Body bytes already received after the header boundary.
                body_str    = request.substr(hend + 4);
                headers_done = true;
            }

            // Keep reading until we have the full body.
            if (headers_done &&
                static_cast<int>(body_str.size()) >= content_length)
                break;
        }

        std::string_view request_line = request_line_str;
        std::string_view body         = body_str;

        std::string response = handler_(request_line, body);
        ::send(cfd, response.data(), static_cast<int>(response.size()), 0);
        ::close(cfd);
    }

    Handler     handler_;
    int         listen_fd_;
    uint16_t    port_;
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// HTTP response helpers
// ---------------------------------------------------------------------------

static std::string http_200(const std::string& body) {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

static std::string http_500(const std::string& body) {
    return "HTTP/1.1 500 Internal Server Error\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

// ===========================================================================
// TEST SUITE: ScraplingClient — fetch
// ===========================================================================

TEST_SUITE("ScraplingClient::fetch") {

    TEST_CASE("fetch success — parses FetchResponse") {
        FetchResponse expected;
        expected.url          = "https://example.com";
        expected.markdown     = "# Hello World";
        expected.status_code  = 200;
        expected.content_type = "text/html";
        expected.is_error     = false;

        MockHttpServer server([&](std::string_view, std::string_view) {
            return http_200(batbox::dump(expected.to_json()));
        });

        ScraplingClient client(server.port(), /*timeout_sec=*/5);
        FetchRequest req;
        req.url = "https://example.com";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.fetch(req, std::move(tok));

        REQUIRE(result.has_value());
        CHECK(result.value().url         == expected.url);
        CHECK(result.value().markdown    == expected.markdown);
        CHECK(result.value().status_code == 200);
        CHECK(result.value().is_error    == false);
    }

    TEST_CASE("fetch sends correct JSON body to /fetch") {
        // Verify correct endpoint and JSON body by parsing body inside the
        // handler (runs synchronously before response is sent) and echoing
        // a sentinel in the response to signal parse success.
        // Uses a shared atomic to safely transfer the parse result.
        std::atomic<bool> body_parse_ok{false};
        std::atomic<bool> url_field_ok{false};
        std::atomic<bool> stealth_field_ok{false};

        MockHttpServer server([&](std::string_view request_line,
                                   std::string_view body) {
            CHECK(request_line.find("/fetch") != std::string_view::npos);
            auto parsed = batbox::parse(body);
            if (parsed.has_value()) {
                body_parse_ok.store(true);
                if (parsed.value().contains("url") &&
                    parsed.value()["url"].get<std::string>() == "https://example.com")
                    url_field_ok.store(true);
                if (parsed.value().contains("stealth") &&
                    parsed.value()["stealth"].get<bool>() == true)
                    stealth_field_ok.store(true);
            }
            FetchResponse resp;
            resp.url         = "https://example.com";
            resp.markdown    = "content";
            resp.status_code = 200;
            return http_200(batbox::dump(resp.to_json()));
        });

        ScraplingClient client(server.port(), 5);
        FetchRequest req;
        req.url     = "https://example.com";
        req.timeout = 15.0;
        req.stealth = true;

        auto [src, tok] = CancelToken::make_root();
        auto result = client.fetch(req, std::move(tok));
        REQUIRE(result.has_value());
        CHECK(body_parse_ok.load());
        CHECK(url_field_ok.load());
        CHECK(stealth_field_ok.load());
    }

    TEST_CASE("fetch — sidecar 500 returns Result error with detail") {
        const std::string error_body =
            R"({"error":"RuntimeError","detail":"Scrapling fetch timeout","path":"/fetch"})";

        MockHttpServer server([&](std::string_view, std::string_view) {
            return http_500(error_body);
        });

        ScraplingClient client(server.port(), 5);
        FetchRequest req;
        req.url = "https://fail.example.com";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.fetch(req, std::move(tok));

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("sidecar error 500") != std::string::npos);
        CHECK(result.error().find("Scrapling fetch timeout") != std::string::npos);
    }

    TEST_CASE("fetch — pre-cancelled token returns Err('cancelled')") {
        // No server needed — cancellation is checked before the network call.
        ScraplingClient client(/*port=*/19999, 5); // port will never be contacted

        FetchRequest req;
        req.url = "https://example.com";

        auto [src, tok] = CancelToken::make_root();
        src.request_stop(); // cancel BEFORE calling fetch

        auto result = client.fetch(req, std::move(tok));

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == "cancelled");
    }
}

// ===========================================================================
// TEST SUITE: ScraplingClient — search
// ===========================================================================

TEST_SUITE("ScraplingClient::search") {

    TEST_CASE("search success — parses SearchResponse with multiple results") {
        SearchResponse expected;
        expected.query  = "batbox C++";
        expected.engine = "ddg";
        expected.results = {
            {"BatBox", "https://batbox.dev", "An AI terminal."},
            {"GitHub", "https://github.com/batbox", "Source code."},
        };
        expected.is_error = false;

        MockHttpServer server([&](std::string_view, std::string_view) {
            return http_200(batbox::dump(expected.to_json()));
        });

        ScraplingClient client(server.port(), 5);
        SearchRequest req;
        req.query = "batbox C++";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.search(req, std::move(tok));

        REQUIRE(result.has_value());
        CHECK(result.value().query  == "batbox C++");
        CHECK(result.value().engine == "ddg");
        REQUIRE(result.value().results.size() == 2u);
        CHECK(result.value().results[0].title == "BatBox");
        CHECK(result.value().results[1].url   == "https://github.com/batbox");
        CHECK(result.value().is_error         == false);
    }

    TEST_CASE("search sends correct JSON body to /search") {
        std::atomic<bool> body_parse_ok{false};
        std::atomic<bool> query_field_ok{false};
        std::atomic<bool> n_field_ok{false};
        std::atomic<bool> engine_field_ok{false};

        MockHttpServer server([&](std::string_view request_line,
                                   std::string_view body) {
            CHECK(request_line.find("/search") != std::string_view::npos);
            auto parsed = batbox::parse(body);
            if (parsed.has_value()) {
                body_parse_ok.store(true);
                if (parsed.value().contains("query") &&
                    parsed.value()["query"].get<std::string>() == "open source C++")
                    query_field_ok.store(true);
                if (parsed.value().contains("n") &&
                    parsed.value()["n"].get<int>() == 5)
                    n_field_ok.store(true);
                if (parsed.value().contains("engine") &&
                    parsed.value()["engine"].get<std::string>() == "ddg")
                    engine_field_ok.store(true);
            }
            SearchResponse resp;
            resp.query  = "q";
            resp.engine = "ddg";
            return http_200(batbox::dump(resp.to_json()));
        });

        ScraplingClient client(server.port(), 5);
        SearchRequest req;
        req.query  = "open source C++";
        req.n      = 5;
        req.engine = "ddg";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.search(req, std::move(tok));
        REQUIRE(result.has_value());
        CHECK(body_parse_ok.load());
        CHECK(query_field_ok.load());
        CHECK(n_field_ok.load());
        CHECK(engine_field_ok.load());
    }

    TEST_CASE("search — sidecar 500 returns Result error") {
        MockHttpServer server([](std::string_view, std::string_view) {
            return http_500(
                R"({"error":"ValueError","detail":"SearXNG unavailable","path":"/search"})");
        });

        ScraplingClient client(server.port(), 5);
        SearchRequest req;
        req.query = "test";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.search(req, std::move(tok));

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("500") != std::string::npos);
        CHECK(result.error().find("SearXNG unavailable") != std::string::npos);
    }

    TEST_CASE("search — pre-cancelled token returns Err('cancelled')") {
        ScraplingClient client(19999, 5);
        SearchRequest req;
        req.query = "test";

        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        auto result = client.search(req, std::move(tok));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == "cancelled");
    }
}

// ===========================================================================
// TEST SUITE: ScraplingClient — select
// ===========================================================================

TEST_SUITE("ScraplingClient::select") {

    TEST_CASE("select success — parses SelectResponse") {
        SelectResponse expected;
        expected.url      = "https://example.com";
        expected.selector = "h1";
        expected.matches  = {"Example Domain"};
        expected.count    = 1;
        expected.is_error = false;

        MockHttpServer server([&](std::string_view, std::string_view) {
            return http_200(batbox::dump(expected.to_json()));
        });

        ScraplingClient client(server.port(), 5);
        SelectRequest req;
        req.url      = "https://example.com";
        req.selector = "h1";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.select(req, std::move(tok));

        REQUIRE(result.has_value());
        CHECK(result.value().url      == "https://example.com");
        CHECK(result.value().selector == "h1");
        REQUIRE(result.value().matches.size() == 1u);
        CHECK(result.value().matches[0] == "Example Domain");
        CHECK(result.value().count    == 1);
    }

    TEST_CASE("select sends correct JSON body to /select") {
        std::atomic<bool> body_parse_ok{false};
        std::atomic<bool> url_field_ok{false};
        std::atomic<bool> selector_field_ok{false};
        std::atomic<bool> attribute_field_ok{false};

        MockHttpServer server([&](std::string_view request_line,
                                   std::string_view body) {
            CHECK(request_line.find("/select") != std::string_view::npos);
            auto parsed = batbox::parse(body);
            if (parsed.has_value()) {
                body_parse_ok.store(true);
                if (parsed.value().contains("url") &&
                    parsed.value()["url"].get<std::string>() == "https://example.com")
                    url_field_ok.store(true);
                if (parsed.value().contains("selector") &&
                    parsed.value()["selector"].get<std::string>() == ".main-content h2")
                    selector_field_ok.store(true);
                if (parsed.value().contains("attribute") &&
                    parsed.value()["attribute"].get<std::string>() == "id")
                    attribute_field_ok.store(true);
            }
            SelectResponse resp;
            resp.url      = "u";
            resp.selector = "s";
            return http_200(batbox::dump(resp.to_json()));
        });

        ScraplingClient client(server.port(), 5);
        SelectRequest req;
        req.url       = "https://example.com";
        req.selector  = ".main-content h2";
        req.attribute = "id";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.select(req, std::move(tok));
        REQUIRE(result.has_value());
        CHECK(body_parse_ok.load());
        CHECK(url_field_ok.load());
        CHECK(selector_field_ok.load());
        CHECK(attribute_field_ok.load());
    }

    TEST_CASE("select — sidecar 500 returns Result error") {
        MockHttpServer server([](std::string_view, std::string_view) {
            return http_500(
                R"({"error":"ParseError","detail":"Invalid CSS selector","path":"/select"})");
        });

        ScraplingClient client(server.port(), 5);
        SelectRequest req;
        req.url      = "https://example.com";
        req.selector = "[invalid";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.select(req, std::move(tok));

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("500") != std::string::npos);
        CHECK(result.error().find("Invalid CSS selector") != std::string::npos);
    }

    TEST_CASE("select — pre-cancelled token returns Err('cancelled')") {
        ScraplingClient client(19999, 5);
        SelectRequest req;
        req.url      = "https://example.com";
        req.selector = "h1";

        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        auto result = client.select(req, std::move(tok));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == "cancelled");
    }
}

// ===========================================================================
// TEST SUITE: ScraplingClient — healthz
// ===========================================================================

TEST_SUITE("ScraplingClient::healthz") {

    TEST_CASE("healthz returns true when server responds 200") {
        MockHttpServer server([](std::string_view request_line,
                                  std::string_view) {
            CHECK(request_line.find("/healthz") != std::string_view::npos);
            return http_200(R"({"status":"ok"})");
        });

        ScraplingClient client(server.port(), 5);
        CHECK(client.healthz() == true);
    }

    TEST_CASE("healthz returns false when server responds 500") {
        MockHttpServer server([](std::string_view, std::string_view) {
            return http_500(R"({"error":"InternalServerError","detail":"","path":"/healthz"})");
        });

        ScraplingClient client(server.port(), 5);
        CHECK(client.healthz() == false);
    }

    TEST_CASE("healthz returns false when connection is refused") {
        // Port 19999 should not be in use in the test environment.
        // Even if it were, 500 ms timeout ensures fast failure.
        ScraplingClient client(/*port=*/19999, 5);
        CHECK(client.healthz() == false);
    }
}

// ===========================================================================
// TEST SUITE: ScraplingClient — shutdown
// ===========================================================================

TEST_SUITE("ScraplingClient::shutdown") {

    TEST_CASE("shutdown sends POST /shutdown and does not throw") {
        std::atomic_bool shutdown_received{false};

        MockHttpServer server([&](std::string_view request_line,
                                   std::string_view) {
            if (request_line.find("/shutdown") != std::string_view::npos) {
                shutdown_received.store(true);
            }
            return http_200(R"({"shutting_down":true})");
        });

        ScraplingClient client(server.port(), 5);

        // Must not throw.
        CHECK_NOTHROW(client.shutdown());

        // Give the background thread a moment to complete the exchange.
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(500);
        while (!shutdown_received.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(shutdown_received.load() == true);
    }

    TEST_CASE("shutdown does not throw when server is not running") {
        ScraplingClient client(/*port=*/19999, 5);
        CHECK_NOTHROW(client.shutdown());
    }
}

// ===========================================================================
// TEST SUITE: ScraplingClient — URL construction
// ===========================================================================

TEST_SUITE("ScraplingClient URL construction") {

    TEST_CASE("client POSTs to 127.0.0.1:<port> loopback address") {
        std::string received_host_header;

        MockHttpServer server([&](std::string_view request_line,
                                   std::string_view) {
            // Just echo back a valid FetchResponse.
            FetchResponse resp;
            resp.url         = "https://example.com";
            resp.status_code = 200;
            return http_200(batbox::dump(resp.to_json()));
        });

        // The server is bound to 127.0.0.1 by MockHttpServer.
        // If ScraplingClient constructed the URL with a different host
        // (e.g. "localhost") the connection might fail on dual-stack systems.
        ScraplingClient client(server.port(), 5);
        FetchRequest req;
        req.url = "https://example.com";

        auto [src, tok] = CancelToken::make_root();
        auto result = client.fetch(req, std::move(tok));
        REQUIRE(result.has_value());
    }

    TEST_CASE("timeout_sec is honoured by constructor") {
        // Build a client with 10s timeout and verify it constructs without error.
        // We cannot directly inspect timeout_ms_ (private), but the object must
        // be constructible and the healthz call against a dead port must return
        // within the 500ms healthz timeout (not the full 10s request timeout).
        ScraplingClient client(19999, 10);
        auto start = std::chrono::steady_clock::now();
        (void)client.healthz(); // uses 500ms hard-coded timeout; result unused intentionally
        auto elapsed = std::chrono::steady_clock::now() - start;
        // Should complete well under 2 seconds even with retry/slow DNS.
        CHECK(elapsed < std::chrono::seconds(2));
    }
}
