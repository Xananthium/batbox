// tests/unit/test_json_rpc.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::mcp JSON-RPC 2.0 envelope helpers.
//
// Build standalone (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_json_rpc.cpp src/mcp/JsonRpc.cpp \
//       -o /tmp/test_json_rpc && /tmp/test_json_rpc
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/mcp/JsonRpc.hpp>

#include <string>
#include <variant>

using namespace batbox;
using namespace batbox::mcp;

// ============================================================================
// TEST SUITE 1: Error code constants
// ============================================================================

TEST_SUITE("errc — standard error code constants") {

    TEST_CASE("kParseError is -32700") {
        CHECK(errc::kParseError == -32700);
    }

    TEST_CASE("kInvalidRequest is -32600") {
        CHECK(errc::kInvalidRequest == -32600);
    }

    TEST_CASE("kMethodNotFound is -32601") {
        CHECK(errc::kMethodNotFound == -32601);
    }

    TEST_CASE("kInvalidParams is -32602") {
        CHECK(errc::kInvalidParams == -32602);
    }

    TEST_CASE("kInternalError is -32603") {
        CHECK(errc::kInternalError == -32603);
    }

    TEST_CASE("kMcpServerError is -32000") {
        CHECK(errc::kMcpServerError == -32000);
    }

    TEST_CASE("kMcpToolError is -32001") {
        CHECK(errc::kMcpToolError == -32001);
    }

    TEST_CASE("kMcpCapabilityError is -32002") {
        CHECK(errc::kMcpCapabilityError == -32002);
    }

    TEST_CASE("all standard codes are in reserved range") {
        // JSON-RPC 2.0 reserved codes must be <= -32000 (or exactly defined)
        CHECK(errc::kParseError     <= -32000);
        CHECK(errc::kInvalidRequest <= -32000);
        CHECK(errc::kMethodNotFound <= -32000);
        CHECK(errc::kInvalidParams  <= -32000);
        CHECK(errc::kInternalError  <= -32000);
    }
}

// ============================================================================
// TEST SUITE 2: make_request — builder
// ============================================================================

TEST_SUITE("make_request — builder") {

    TEST_CASE("integer id: has jsonrpc=2.0, id, method") {
        Json j = make_request(42, "tools/list");
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["id"]      == 42);
        CHECK(j["method"]  == "tools/list");
        CHECK(j.find("params") == j.end()); // params omitted when null
    }

    TEST_CASE("integer id with params object") {
        Json params = {{"name", "read_file"}, {"path", "/tmp/x"}};
        Json j = make_request(1, "tools/call", params);
        CHECK(j["params"]["name"] == "read_file");
        CHECK(j["params"]["path"] == "/tmp/x");
    }

    TEST_CASE("string id") {
        Json j = make_request("abc-123", "resources/list");
        CHECK(j["id"] == "abc-123");
        CHECK(j["method"] == "resources/list");
    }

    TEST_CASE("string id with params") {
        Json params = {{"uri", "file:///tmp/foo.txt"}};
        Json j = make_request("req-1", "resources/read", params);
        CHECK(j["id"] == "req-1");
        CHECK(j["params"]["uri"] == "file:///tmp/foo.txt");
    }

    TEST_CASE("params=null omits params field") {
        Json j = make_request(7, "initialize", Json(nullptr));
        CHECK(j.find("params") == j.end());
    }

    TEST_CASE("round-trip: make_request → parse_message → JsonRpcRequest") {
        Json j = make_request(99, "prompts/get",
                              Json{{"name", "my_prompt"}});
        auto result = parse_message(j);
        REQUIRE(result.has_value());
        REQUIRE(std::holds_alternative<JsonRpcRequest>(*result));
        const auto& req = std::get<JsonRpcRequest>(*result);
        CHECK(std::get<int64_t>(req.id) == 99);
        CHECK(req.method == "prompts/get");
        CHECK(req.params["name"] == "my_prompt");
    }
}

// ============================================================================
// TEST SUITE 3: make_notification — builder
// ============================================================================

TEST_SUITE("make_notification — builder") {

    TEST_CASE("no id field present") {
        Json j = make_notification("notifications/cancelled");
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["method"]  == "notifications/cancelled");
        CHECK(j.find("id") == j.end());
    }

    TEST_CASE("no params field when null") {
        Json j = make_notification("notifications/progress", Json(nullptr));
        CHECK(j.find("params") == j.end());
    }

    TEST_CASE("params included when provided") {
        Json params = {{"progressToken", "abc"}, {"progress", 50}};
        Json j = make_notification("notifications/progress", params);
        CHECK(j["params"]["progressToken"] == "abc");
        CHECK(j["params"]["progress"]      == 50);
    }

    TEST_CASE("round-trip: make_notification → parse_message → JsonRpcNotification") {
        Json j = make_notification("notifications/message",
                                   Json{{"level", "info"}, {"data", "hello"}});
        auto result = parse_message(j);
        REQUIRE(result.has_value());
        REQUIRE(std::holds_alternative<JsonRpcNotification>(*result));
        const auto& notif = std::get<JsonRpcNotification>(*result);
        CHECK(notif.method == "notifications/message");
        CHECK(notif.params["level"] == "info");
    }
}

// ============================================================================
// TEST SUITE 4: make_response — builder
// ============================================================================

TEST_SUITE("make_response — builder") {

    TEST_CASE("integer id success response") {
        Json j = make_response(5, Json{{"tools", Json::array()}});
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["id"]      == 5);
        CHECK(j.find("result") != j.end());
        CHECK(j.find("error")  == j.end());
        CHECK(j["result"]["tools"].is_array());
    }

    TEST_CASE("string id success response") {
        Json j = make_response("req-42", Json{{"ok", true}});
        CHECK(j["id"]     == "req-42");
        CHECK(j["result"] == Json{{"ok", true}});
    }

    TEST_CASE("round-trip: make_response → parse_message → JsonRpcResponse") {
        Json j = make_response(10, Json{{"content", "text"}});
        auto result = parse_message(j);
        REQUIRE(result.has_value());
        REQUIRE(std::holds_alternative<JsonRpcResponse>(*result));
        const auto& resp = std::get<JsonRpcResponse>(*result);
        CHECK(std::get<int64_t>(resp.id) == 10);
        REQUIRE(resp.result.has_value());
        CHECK((*resp.result)["content"] == "text");
        CHECK(!resp.error.has_value());
    }
}

// ============================================================================
// TEST SUITE 5: make_error_response — builder
// ============================================================================

TEST_SUITE("make_error_response — builder") {

    TEST_CASE("integer id error response has correct shape") {
        Json j = make_error_response(3, errc::kMethodNotFound,
                                     "method not found");
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["id"]      == 3);
        CHECK(j.find("error")  != j.end());
        CHECK(j.find("result") == j.end());
        CHECK(j["error"]["code"]    == errc::kMethodNotFound);
        CHECK(j["error"]["message"] == "method not found");
        CHECK(j["error"].find("data") == j["error"].end()); // no data
    }

    TEST_CASE("error response with optional data field") {
        Json data = {{"details", "unknown method xyz"}};
        Json j = make_error_response(4, errc::kInvalidParams, "bad params", data);
        CHECK(j["error"]["data"]["details"] == "unknown method xyz");
    }

    TEST_CASE("string id error response") {
        Json j = make_error_response("r-99", errc::kInternalError,
                                     "internal error");
        CHECK(j["id"] == "r-99");
        CHECK(j["error"]["code"] == errc::kInternalError);
    }

    TEST_CASE("all standard errc codes round-trip through error response") {
        constexpr int codes[] = {
            errc::kParseError, errc::kInvalidRequest, errc::kMethodNotFound,
            errc::kInvalidParams, errc::kInternalError,
            errc::kMcpServerError, errc::kMcpToolError, errc::kMcpCapabilityError
        };
        for (int code : codes) {
            Json j = make_error_response(1, code, "test");
            CHECK(j["error"]["code"] == code);
        }
    }

    TEST_CASE("round-trip: make_error_response → parse_message → JsonRpcResponse") {
        Json j = make_error_response(7, errc::kParseError, "parse error");
        auto result = parse_message(j);
        REQUIRE(result.has_value());
        REQUIRE(std::holds_alternative<JsonRpcResponse>(*result));
        const auto& resp = std::get<JsonRpcResponse>(*result);
        REQUIRE(resp.error.has_value());
        CHECK(resp.error->code    == errc::kParseError);
        CHECK(resp.error->message == "parse error");
        CHECK(!resp.result.has_value());
    }
}

// ============================================================================
// TEST SUITE 6: parse_message — dispatch logic
// ============================================================================

TEST_SUITE("parse_message — dispatch") {

    TEST_CASE("dispatches to JsonRpcRequest when method + id present") {
        Json j = {{"jsonrpc","2.0"}, {"id",1}, {"method","ping"}};
        auto r = parse_message(j);
        REQUIRE(r.has_value());
        CHECK(std::holds_alternative<JsonRpcRequest>(*r));
    }

    TEST_CASE("dispatches to JsonRpcNotification when method present, no id") {
        Json j = {{"jsonrpc","2.0"}, {"method","notify/something"}};
        auto r = parse_message(j);
        REQUIRE(r.has_value());
        CHECK(std::holds_alternative<JsonRpcNotification>(*r));
    }

    TEST_CASE("dispatches to JsonRpcResponse when result present") {
        Json j = {{"jsonrpc","2.0"}, {"id",2}, {"result", Json::object()}};
        auto r = parse_message(j);
        REQUIRE(r.has_value());
        CHECK(std::holds_alternative<JsonRpcResponse>(*r));
    }

    TEST_CASE("dispatches to JsonRpcResponse when error present") {
        Json j = {{"jsonrpc","2.0"}, {"id",3},
                  {"error", {{"code",-32600},{"message","bad request"}}}};
        auto r = parse_message(j);
        REQUIRE(r.has_value());
        CHECK(std::holds_alternative<JsonRpcResponse>(*r));
    }

    TEST_CASE("string id request dispatches correctly") {
        Json j = {{"jsonrpc","2.0"}, {"id","abc"}, {"method","tools/list"}};
        auto r = parse_message(j);
        REQUIRE(r.has_value());
        REQUIRE(std::holds_alternative<JsonRpcRequest>(*r));
        CHECK(std::get<std::string>(std::get<JsonRpcRequest>(*r).id) == "abc");
    }
}

// ============================================================================
// TEST SUITE 7: parse_message — error cases
// ============================================================================

TEST_SUITE("parse_message — error cases") {

    TEST_CASE("batch (JSON array) rejected") {
        Json j = Json::array({
            {{"jsonrpc","2.0"}, {"id",1}, {"method","ping"}},
            {{"jsonrpc","2.0"}, {"id",2}, {"method","pong"}}
        });
        auto r = parse_message(j);
        CHECK(!r.has_value());
        // Error should mention batch
        CHECK(r.error().find("batch") != std::string::npos);
    }

    TEST_CASE("missing jsonrpc field returns error") {
        Json j = {{"id",1}, {"method","ping"}};
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("jsonrpc != 2.0 returns error") {
        Json j = {{"jsonrpc","1.0"}, {"id",1}, {"method","ping"}};
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("plain scalar value returns error") {
        Json j = 42;
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("null value returns error") {
        Json j = nullptr;
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("object with neither method nor result/error returns error") {
        Json j = {{"jsonrpc","2.0"}, {"id",1}, {"unknown","field"}};
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("response with both result and error returns error") {
        Json j = {{"jsonrpc","2.0"}, {"id",1},
                  {"result", true},
                  {"error", {{"code",-32603},{"message","err"}}}};
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("response missing both result and error returns error") {
        Json j = {{"jsonrpc","2.0"}, {"id",1}};
        // Has id but no method, result, or error
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("request with float id returns error") {
        Json j = {{"jsonrpc","2.0"}, {"id",1.5}, {"method","ping"}};
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }

    TEST_CASE("request with boolean id returns error") {
        Json j = {{"jsonrpc","2.0"}, {"id",true}, {"method","ping"}};
        auto r = parse_message(j);
        CHECK(!r.has_value());
    }
}

// ============================================================================
// TEST SUITE 8: JsonRpcError struct
// ============================================================================

TEST_SUITE("JsonRpcError — to_json / from_json") {

    TEST_CASE("to_json produces correct shape without data") {
        JsonRpcError err{errc::kInternalError, "internal error", std::nullopt};
        Json j = err.to_json();
        CHECK(j["code"]    == errc::kInternalError);
        CHECK(j["message"] == "internal error");
        CHECK(j.find("data") == j.end());
    }

    TEST_CASE("to_json includes data when set") {
        JsonRpcError err{errc::kInvalidParams, "bad params",
                         Json{{"field","name"}}};
        Json j = err.to_json();
        CHECK(j["data"]["field"] == "name");
    }

    TEST_CASE("from_json succeeds with valid error object") {
        Json j = {{"code",-32600}, {"message","invalid request"}};
        auto result = JsonRpcError::from_json(j);
        REQUIRE(result.has_value());
        CHECK(result->code    == -32600);
        CHECK(result->message == "invalid request");
        CHECK(!result->data.has_value());
    }

    TEST_CASE("from_json captures data field") {
        Json j = {{"code",-32602}, {"message","bad params"},
                  {"data", {{"param","x"}}}};
        auto result = JsonRpcError::from_json(j);
        REQUIRE(result.has_value());
        REQUIRE(result->data.has_value());
        CHECK((*result->data)["param"] == "x");
    }

    TEST_CASE("from_json fails when code missing") {
        Json j = {{"message","oops"}};
        CHECK(!JsonRpcError::from_json(j).has_value());
    }

    TEST_CASE("from_json fails when message missing") {
        Json j = {{"code",-32603}};
        CHECK(!JsonRpcError::from_json(j).has_value());
    }

    TEST_CASE("from_json fails on non-object") {
        CHECK(!JsonRpcError::from_json(Json("string")).has_value());
        CHECK(!JsonRpcError::from_json(Json(42)).has_value());
        CHECK(!JsonRpcError::from_json(Json::array()).has_value());
    }

    TEST_CASE("round-trip: to_json → from_json") {
        JsonRpcError original{errc::kParseError, "parse error",
                               Json{{"offset", 42}}};
        auto round_tripped = JsonRpcError::from_json(original.to_json());
        REQUIRE(round_tripped.has_value());
        CHECK(round_tripped->code    == errc::kParseError);
        CHECK(round_tripped->message == "parse error");
        REQUIRE(round_tripped->data.has_value());
        CHECK((*round_tripped->data)["offset"] == 42);
    }
}

// ============================================================================
// TEST SUITE 9: JsonRpcRequest struct
// ============================================================================

TEST_SUITE("JsonRpcRequest — to_json / from_json") {

    TEST_CASE("to_json integer id produces correct shape") {
        JsonRpcRequest req;
        req.id     = int64_t{1};
        req.method = "initialize";
        req.params = Json(nullptr);

        Json j = req.to_json();
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["id"]      == 1);
        CHECK(j["method"]  == "initialize");
        CHECK(j.find("params") == j.end());
    }

    TEST_CASE("to_json string id produces correct shape") {
        JsonRpcRequest req;
        req.id     = std::string{"req-abc"};
        req.method = "tools/call";
        req.params = Json{{"name","bash"}};

        Json j = req.to_json();
        CHECK(j["id"]             == "req-abc");
        CHECK(j["params"]["name"] == "bash");
    }

    TEST_CASE("from_json succeeds with integer id") {
        Json j = {{"jsonrpc","2.0"}, {"id",7}, {"method","ping"},
                  {"params", Json::object()}};
        auto result = JsonRpcRequest::from_json(j);
        REQUIRE(result.has_value());
        CHECK(std::get<int64_t>(result->id) == 7);
        CHECK(result->method == "ping");
    }

    TEST_CASE("from_json succeeds with string id") {
        Json j = {{"jsonrpc","2.0"}, {"id","req-1"}, {"method","prompts/list"}};
        auto result = JsonRpcRequest::from_json(j);
        REQUIRE(result.has_value());
        CHECK(std::get<std::string>(result->id) == "req-1");
    }

    TEST_CASE("from_json fails when id missing") {
        Json j = {{"jsonrpc","2.0"}, {"method","ping"}};
        CHECK(!JsonRpcRequest::from_json(j).has_value());
    }

    TEST_CASE("from_json fails when method missing") {
        Json j = {{"jsonrpc","2.0"}, {"id",1}};
        CHECK(!JsonRpcRequest::from_json(j).has_value());
    }

    TEST_CASE("from_json fails when jsonrpc wrong version") {
        Json j = {{"jsonrpc","1.0"}, {"id",1}, {"method","ping"}};
        CHECK(!JsonRpcRequest::from_json(j).has_value());
    }

    TEST_CASE("round-trip: to_json → from_json preserves params") {
        JsonRpcRequest original;
        original.id     = int64_t{55};
        original.method = "sampling/createMessage";
        original.params = Json{{"maxTokens", 1024}, {"messages", Json::array()}};

        auto rt = JsonRpcRequest::from_json(original.to_json());
        REQUIRE(rt.has_value());
        CHECK(std::get<int64_t>(rt->id) == 55);
        CHECK(rt->method == "sampling/createMessage");
        CHECK(rt->params["maxTokens"] == 1024);
    }
}

// ============================================================================
// TEST SUITE 10: JsonRpcNotification struct
// ============================================================================

TEST_SUITE("JsonRpcNotification — to_json / from_json") {

    TEST_CASE("to_json omits id field") {
        JsonRpcNotification n;
        n.method = "notifications/initialized";
        n.params = Json(nullptr);

        Json j = n.to_json();
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["method"]  == "notifications/initialized");
        CHECK(j.find("id")     == j.end());
        CHECK(j.find("params") == j.end());
    }

    TEST_CASE("to_json includes params when not null") {
        JsonRpcNotification n;
        n.method = "notifications/progress";
        n.params = Json{{"progressToken","t1"},{"progress",75}};

        Json j = n.to_json();
        CHECK(j["params"]["progressToken"] == "t1");
        CHECK(j["params"]["progress"]      == 75);
    }

    TEST_CASE("from_json succeeds") {
        Json j = {{"jsonrpc","2.0"}, {"method","notifications/cancelled"},
                  {"params", {{"requestId",3}}}};
        auto result = JsonRpcNotification::from_json(j);
        REQUIRE(result.has_value());
        CHECK(result->method == "notifications/cancelled");
        CHECK(result->params["requestId"] == 3);
    }

    TEST_CASE("from_json fails when method missing") {
        Json j = {{"jsonrpc","2.0"}, {"params", Json::object()}};
        CHECK(!JsonRpcNotification::from_json(j).has_value());
    }

    TEST_CASE("round-trip: to_json → from_json") {
        JsonRpcNotification original;
        original.method = "$/progress";
        original.params = Json{{"value","done"}};

        auto rt = JsonRpcNotification::from_json(original.to_json());
        REQUIRE(rt.has_value());
        CHECK(rt->method == "$/progress");
        CHECK(rt->params["value"] == "done");
    }
}

// ============================================================================
// TEST SUITE 11: JsonRpcResponse struct
// ============================================================================

TEST_SUITE("JsonRpcResponse — to_json / from_json") {

    TEST_CASE("success response to_json / from_json") {
        JsonRpcResponse resp;
        resp.id     = int64_t{5};
        resp.result = Json{{"tools", Json::array()}};

        Json j = resp.to_json();
        CHECK(j["jsonrpc"] == "2.0");
        CHECK(j["id"]      == 5);
        CHECK(j.find("result") != j.end());
        CHECK(j.find("error")  == j.end());

        auto rt = JsonRpcResponse::from_json(j);
        REQUIRE(rt.has_value());
        CHECK(std::get<int64_t>(rt->id) == 5);
        REQUIRE(rt->result.has_value());
        CHECK((*rt->result)["tools"].is_array());
        CHECK(!rt->error.has_value());
    }

    TEST_CASE("error response to_json / from_json") {
        JsonRpcResponse resp;
        resp.id    = std::string{"r-1"};
        resp.error = JsonRpcError{errc::kMethodNotFound, "not found", std::nullopt};

        Json j = resp.to_json();
        CHECK(j["id"] == "r-1");
        CHECK(j.find("error")  != j.end());
        CHECK(j.find("result") == j.end());

        auto rt = JsonRpcResponse::from_json(j);
        REQUIRE(rt.has_value());
        REQUIRE(rt->error.has_value());
        CHECK(rt->error->code    == errc::kMethodNotFound);
        CHECK(rt->error->message == "not found");
        CHECK(!rt->result.has_value());
    }

    TEST_CASE("from_json fails when both result and error present") {
        Json j = {{"jsonrpc","2.0"}, {"id",1},
                  {"result",true},
                  {"error",{{"code",-32603},{"message","err"}}}};
        CHECK(!JsonRpcResponse::from_json(j).has_value());
    }

    TEST_CASE("from_json fails when neither result nor error present") {
        Json j = {{"jsonrpc","2.0"}, {"id",1}};
        CHECK(!JsonRpcResponse::from_json(j).has_value());
    }

    TEST_CASE("from_json fails when id missing") {
        Json j = {{"jsonrpc","2.0"}, {"result",true}};
        CHECK(!JsonRpcResponse::from_json(j).has_value());
    }
}

// ============================================================================
// TEST SUITE 12: next_id — thread-safe counter
// ============================================================================

TEST_SUITE("next_id — thread-safe id counter") {

    TEST_CASE("returns positive integer") {
        int64_t id = next_id();
        CHECK(id >= 1);
    }

    TEST_CASE("ids are monotonically increasing") {
        int64_t a = next_id();
        int64_t b = next_id();
        int64_t c = next_id();
        CHECK(b == a + 1);
        CHECK(c == b + 1);
    }

    TEST_CASE("ids are unique across many calls") {
        constexpr int kN = 100;
        std::vector<int64_t> ids;
        ids.reserve(kN);
        for (int i = 0; i < kN; ++i) {
            ids.push_back(next_id());
        }
        // All must be strictly increasing (no duplicates).
        for (int i = 1; i < kN; ++i) {
            CHECK(ids[static_cast<std::size_t>(i)] >
                  ids[static_cast<std::size_t>(i - 1)]);
        }
    }

    TEST_CASE("next_id used in make_request produces unique messages") {
        Json req1 = make_request(next_id(), "ping");
        Json req2 = make_request(next_id(), "ping");
        CHECK(req1["id"] != req2["id"]);
    }
}

// ============================================================================
// TEST SUITE 13: Full MCP-style round-trips
// ============================================================================

TEST_SUITE("MCP message round-trips") {

    TEST_CASE("initialize request round-trip") {
        Json params = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", Json::object()},
            {"clientInfo", {{"name","batbox"},{"version","0.1"}}}
        };
        int64_t id = next_id();
        Json wire  = make_request(id, "initialize", params);
        auto msg   = parse_message(wire);
        REQUIRE(msg.has_value());
        REQUIRE(std::holds_alternative<JsonRpcRequest>(*msg));
        const auto& req = std::get<JsonRpcRequest>(*msg);
        CHECK(req.method == "initialize");
        CHECK(req.params["protocolVersion"] == "2024-11-05");
    }

    TEST_CASE("tools/list request → response round-trip") {
        int64_t req_id = next_id();
        Json    req    = make_request(req_id, "tools/list");

        // Server replies
        Json tools_result = {{"tools", Json::array({
            Json{{"name","bash"},{"description","Run shell commands"}}
        })}};
        Json resp = make_response(req_id, tools_result);
        auto msg  = parse_message(resp);
        REQUIRE(std::holds_alternative<JsonRpcResponse>(*msg));
        const auto& r = std::get<JsonRpcResponse>(*msg);
        CHECK(std::get<int64_t>(r.id) == req_id);
        REQUIRE(r.result.has_value());
        CHECK((*r.result)["tools"][0]["name"] == "bash");
    }

    TEST_CASE("tools/call error response round-trip") {
        int64_t req_id = next_id();
        Json resp = make_error_response(req_id, errc::kMcpToolError,
                                        "tool execution failed",
                                        Json{{"tool","bash"},{"exit_code",1}});
        auto msg = parse_message(resp);
        REQUIRE(std::holds_alternative<JsonRpcResponse>(*msg));
        const auto& r = std::get<JsonRpcResponse>(*msg);
        REQUIRE(r.error.has_value());
        CHECK(r.error->code == errc::kMcpToolError);
        CHECK(r.error->message == "tool execution failed");
        REQUIRE(r.error->data.has_value());
        CHECK((*r.error->data)["tool"] == "bash");
    }

    TEST_CASE("notifications/initialized round-trip") {
        Json wire = make_notification("notifications/initialized");
        auto msg  = parse_message(wire);
        REQUIRE(std::holds_alternative<JsonRpcNotification>(*msg));
        CHECK(std::get<JsonRpcNotification>(*msg).method ==
              "notifications/initialized");
    }

    TEST_CASE("notifications/cancelled with requestId round-trip") {
        Json params = {{"requestId", 42}};
        Json wire   = make_notification("notifications/cancelled", params);
        auto msg    = parse_message(wire);
        REQUIRE(std::holds_alternative<JsonRpcNotification>(*msg));
        const auto& n = std::get<JsonRpcNotification>(*msg);
        CHECK(n.params["requestId"] == 42);
    }
}
