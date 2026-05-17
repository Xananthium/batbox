// src/tools/ReadMcpResourceTool.cpp
// =============================================================================
// Implementation of batbox::tools::ReadMcpResourceTool.
//
// Sends "resources/read" to the specified MCP server's IMcpTransport and
// returns the content of the resource.
//
// MCP resources/read protocol (MCP spec §3.3):
//   Request  : method="resources/read", params={"uri": "<resource-uri>"}
//   Response : {
//                "contents": [
//                  {"uri":"…", "mimeType":"…", "text":"…"},   // text resource
//                  {"uri":"…", "mimeType":"…", "blob":"…"},   // base64 blob
//                  …
//                ]
//              }
//
// Blueprint contract: batbox::tools::ReadMcpResourceTool (task CPP 5.26)
// =============================================================================

#include <batbox/tools/ReadMcpResourceTool.hpp>

#include <batbox/core/Json.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

namespace {

// ---------------------------------------------------------------------------
// ContentItem — one entry in the resources/read response contents array.
// ---------------------------------------------------------------------------
struct ContentItem {
    std::string uri;
    std::string mime_type;
    std::string text;       // populated for text resources
    bool        is_blob;    // true when the server returned blob instead of text
};

// ---------------------------------------------------------------------------
// parse_contents — extract ContentItem vector from the "contents" JSON array.
// ---------------------------------------------------------------------------
std::vector<ContentItem> parse_contents(const Json& resp) {
    std::vector<ContentItem> items;

    if (!resp.is_object()) return items;
    if (!resp.contains("contents") || !resp["contents"].is_array()) return items;

    for (const auto& item : resp["contents"]) {
        if (!item.is_object()) continue;

        ContentItem ci;
        ci.is_blob = false;

        if (item.contains("uri") && item["uri"].is_string()) {
            ci.uri = item["uri"].get<std::string>();
        }
        if (item.contains("mimeType") && item["mimeType"].is_string()) {
            ci.mime_type = item["mimeType"].get<std::string>();
        }
        if (item.contains("text") && item["text"].is_string()) {
            ci.text    = item["text"].get<std::string>();
            ci.is_blob = false;
        } else if (item.contains("blob")) {
            // blob is base64-encoded binary; we don't decode it here —
            // just indicate binary content to the caller.
            ci.is_blob = true;
        }

        items.push_back(std::move(ci));
    }

    return items;
}

// ---------------------------------------------------------------------------
// format_contents — combine content items into a single human-readable string.
// ---------------------------------------------------------------------------
std::string format_contents(const std::vector<ContentItem>& items) {
    if (items.empty()) {
        return "(empty resource — no content items returned)";
    }

    std::ostringstream out;
    bool first = true;

    for (const auto& ci : items) {
        if (!first) out << "\n";
        first = false;

        if (ci.is_blob) {
            out << "<binary blob>";
            if (!ci.mime_type.empty()) {
                out << " [" << ci.mime_type << "]";
            }
        } else {
            out << ci.text;
        }
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// build_payload — structured JSON payload from the content items.
// ---------------------------------------------------------------------------
Json build_payload(const std::string& server_name,
                   const std::string& uri,
                   const std::vector<ContentItem>& items) {
    Json payload = Json::object();
    payload["server"] = server_name;
    payload["uri"]    = uri;

    Json contents_arr = Json::array();
    for (const auto& ci : items) {
        Json item_obj = Json::object();
        item_obj["uri"]      = ci.uri;
        item_obj["mimeType"] = ci.mime_type;
        if (ci.is_blob) {
            item_obj["blob"] = true;
        } else {
            item_obj["text"] = ci.text;
        }
        contents_arr.push_back(std::move(item_obj));
    }
    payload["contents"] = std::move(contents_arr);

    return payload;
}

} // anonymous namespace

// =============================================================================
// ReadMcpResourceTool — construction
// =============================================================================

ReadMcpResourceTool::ReadMcpResourceTool(
    batbox::mcp::McpServerRegistry& registry) noexcept
    : registry_(registry) {}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view ReadMcpResourceTool::name() const {
    return "ReadMcpResource";
}

std::string_view ReadMcpResourceTool::description() const {
    return "Read the content of a specific MCP resource by URI from a named "
           "MCP server. Returns text content or indicates binary blobs.";
}

Json ReadMcpResourceTool::schema_json() const {
    return Json{
        {"name",        "ReadMcpResource"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"server", Json{
                    {"type",        "string"},
                    {"description", "Name of the MCP server that owns this resource."}
                }},
                {"uri", Json{
                    {"type",        "string"},
                    {"description", "URI of the resource to read "
                                    "(e.g. \"file:///tmp/foo.txt\")."}
                }}
            }},
            {"required", Json::array({"server", "uri"})}
        }}
    };
}

// =============================================================================
// ITool execution
// =============================================================================

ToolResult ReadMcpResourceTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Validate required "server" argument.
    // ------------------------------------------------------------------
    if (!args.contains("server") || args["server"].is_null()) {
        return ToolResult::error(
            "ReadMcpResource: required argument 'server' is missing.");
    }
    if (!args["server"].is_string()) {
        return ToolResult::error(
            "ReadMcpResource: 'server' must be a string.");
    }
    const std::string server_name = args["server"].get<std::string>();
    if (server_name.empty()) {
        return ToolResult::error(
            "ReadMcpResource: 'server' must be a non-empty string.");
    }

    // ------------------------------------------------------------------
    // 2. Validate required "uri" argument.
    // ------------------------------------------------------------------
    if (!args.contains("uri") || args["uri"].is_null()) {
        return ToolResult::error(
            "ReadMcpResource: required argument 'uri' is missing.");
    }
    if (!args["uri"].is_string()) {
        return ToolResult::error(
            "ReadMcpResource: 'uri' must be a string.");
    }
    const std::string uri = args["uri"].get<std::string>();
    if (uri.empty()) {
        return ToolResult::error(
            "ReadMcpResource: 'uri' must be a non-empty string.");
    }

    // ------------------------------------------------------------------
    // 3. Look up the server transport.
    // ------------------------------------------------------------------
    batbox::mcp::IMcpTransport* transport = registry_.get(server_name);
    if (transport == nullptr) {
        return ToolResult::error(
            "ReadMcpResource: unknown server: " + server_name);
    }

    // ------------------------------------------------------------------
    // 4. Cancellation check before I/O.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 5. Send resources/read RPC.
    // ------------------------------------------------------------------
    Json params = Json::object();
    params["uri"] = uri;

    auto [child_src, child_tok] = ctx.cancel_token.child();
    (void)child_src;

    auto rpc_result = transport->request(
        "resources/read", std::move(params), std::move(child_tok));

    if (!rpc_result.has_value()) {
        return ToolResult::error(
            "ReadMcpResource: " + rpc_result.error());
    }

    // ------------------------------------------------------------------
    // 6. Parse the response contents.
    // ------------------------------------------------------------------
    const std::vector<ContentItem> items = parse_contents(rpc_result.value());

    // ------------------------------------------------------------------
    // 7. Format output and return.
    // ------------------------------------------------------------------
    std::string body    = format_contents(items);
    Json        payload = build_payload(server_name, uri, items);

    return ToolResult::ok(std::move(body), std::move(payload));
}

} // namespace batbox::tools
