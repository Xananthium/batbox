// src/tools/ListMcpResourcesTool.cpp
// =============================================================================
// Implementation of batbox::tools::ListMcpResourcesTool.
//
// Sends "resources/list" to one or all connected MCP servers via their
// registered IMcpTransport, aggregates results, and returns a human-readable
// listing with server-prefixed names.
//
// MCP resources/list protocol (MCP spec §3.3):
//   Request  : method="resources/list", params=null or {}
//   Response : {"resources": [{"name":"…","uri":"…","description":"…","mimeType":"…"}, …]}
//
// Blueprint contract: batbox::tools::ListMcpResourcesTool (task CPP 5.26)
// =============================================================================

#include <batbox/tools/ListMcpResourcesTool.hpp>

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
// ServerResult — holds the resources (or error) for one server.
// ---------------------------------------------------------------------------
struct ServerResult {
    std::string server_name;
    // Each resource entry: name, uri, description, mimeType
    struct Resource {
        std::string name;
        std::string uri;
        std::string description;
        std::string mime_type;
    };
    std::vector<Resource> resources;
    std::string           error; // non-empty on transport failure
};

// ---------------------------------------------------------------------------
// query_server — sends resources/list to one transport and parses the result.
//
// Returns a ServerResult populated with resources on success, or an error
// field on failure.  Never throws — all exceptions are caught.
// ---------------------------------------------------------------------------
ServerResult query_server(const std::string& server_name,
                          batbox::mcp::IMcpTransport* transport,
                          CancelToken ct) {
    ServerResult result;
    result.server_name = server_name;

    if (!transport) {
        result.error = "transport not available";
        return result;
    }

    // Send resources/list with null params (no cursor pagination for now).
    auto rpc_result = transport->request("resources/list", Json(nullptr), std::move(ct));
    if (!rpc_result.has_value()) {
        result.error = rpc_result.error();
        return result;
    }

    // Parse the response: {"resources": [...]}
    const Json& resp = rpc_result.value();
    if (!resp.is_object() || !resp.contains("resources") || !resp["resources"].is_array()) {
        // Some servers return a null result for empty resource lists.
        // That's valid — just return an empty list.
        return result;
    }

    for (const auto& item : resp["resources"]) {
        if (!item.is_object()) continue;

        ServerResult::Resource res;
        if (item.contains("name") && item["name"].is_string()) {
            res.name = item["name"].get<std::string>();
        }
        if (item.contains("uri") && item["uri"].is_string()) {
            res.uri = item["uri"].get<std::string>();
        }
        if (item.contains("description") && item["description"].is_string()) {
            res.description = item["description"].get<std::string>();
        }
        if (item.contains("mimeType") && item["mimeType"].is_string()) {
            res.mime_type = item["mimeType"].get<std::string>();
        }
        result.resources.push_back(std::move(res));
    }

    return result;
}

// ---------------------------------------------------------------------------
// format_results — formats server results into a human-readable string.
// ---------------------------------------------------------------------------
std::string format_results(const std::vector<ServerResult>& results) {
    std::ostringstream out;
    bool any_resource = false;

    for (const auto& sr : results) {
        if (!sr.error.empty()) {
            out << "[" << sr.server_name << " error: " << sr.error << "]\n";
            continue;
        }
        for (const auto& r : sr.resources) {
            any_resource = true;
            // Format: server:name  uri  [description]
            out << sr.server_name << ":" << r.name;
            if (!r.uri.empty()) {
                out << "  " << r.uri;
            }
            if (!r.description.empty()) {
                out << "  [" << r.description << "]";
            }
            out << "\n";
        }
    }

    std::string body = out.str();
    if (!body.empty() && body.back() == '\n') {
        body.pop_back();
    }

    if (!any_resource && body.empty()) {
        return "No resources found.";
    }
    return body;
}

// ---------------------------------------------------------------------------
// build_payload — builds the structured JSON payload from server results.
// ---------------------------------------------------------------------------
Json build_payload(const std::vector<ServerResult>& results) {
    Json payload = Json::object();
    Json servers_arr = Json::array();

    for (const auto& sr : results) {
        Json server_obj = Json::object();
        server_obj["server"] = sr.server_name;

        if (!sr.error.empty()) {
            server_obj["error"] = sr.error;
            server_obj["resources"] = Json::array();
        } else {
            Json resources_arr = Json::array();
            for (const auto& r : sr.resources) {
                Json res_obj = Json::object();
                res_obj["name"]        = r.name;
                res_obj["uri"]         = r.uri;
                res_obj["description"] = r.description;
                res_obj["mimeType"]    = r.mime_type;
                resources_arr.push_back(std::move(res_obj));
            }
            server_obj["resources"] = std::move(resources_arr);
        }

        servers_arr.push_back(std::move(server_obj));
    }

    payload["servers"] = std::move(servers_arr);
    return payload;
}

} // anonymous namespace

// =============================================================================
// ListMcpResourcesTool — construction
// =============================================================================

ListMcpResourcesTool::ListMcpResourcesTool(
    batbox::mcp::McpServerRegistry& registry) noexcept
    : registry_(registry) {}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view ListMcpResourcesTool::name() const {
    return "ListMcpResources";
}

std::string_view ListMcpResourcesTool::description() const {
    return "List resources exposed by one or all connected MCP servers. "
           "Returns resource names, URIs, and descriptions. "
           "Optionally filtered to a single server via args.server.";
}

Json ListMcpResourcesTool::schema_json() const {
    return Json{
        {"name",        "ListMcpResources"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"server", Json{
                    {"type",        "string"},
                    {"description", "Name of the MCP server to query. "
                                    "If omitted, all connected servers are queried."}
                }}
            }},
            {"required", Json::array()}
        }}
    };
}

// =============================================================================
// ITool execution
// =============================================================================

ToolResult ListMcpResourcesTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Parse optional "server" argument.
    // ------------------------------------------------------------------
    std::string filter_server;
    if (args.contains("server") && !args["server"].is_null()) {
        if (!args["server"].is_string()) {
            return ToolResult::error(
                "ListMcpResources: 'server' must be a string.");
        }
        filter_server = args["server"].get<std::string>();
        if (filter_server.empty()) {
            return ToolResult::error(
                "ListMcpResources: 'server' must be a non-empty string.");
        }
    }

    // ------------------------------------------------------------------
    // 2. Determine which servers to query.
    // ------------------------------------------------------------------
    std::vector<std::string> server_names;

    if (!filter_server.empty()) {
        // Single-server mode: validate the name first.
        if (registry_.get(filter_server) == nullptr) {
            return ToolResult::error(
                "ListMcpResources: unknown server: " + filter_server);
        }
        server_names.push_back(filter_server);
    } else {
        // All-servers mode.
        server_names = registry_.server_names();
        if (server_names.empty()) {
            return ToolResult::ok("No MCP servers are connected.");
        }
    }

    // ------------------------------------------------------------------
    // 3. Query each server.
    // ------------------------------------------------------------------
    std::vector<ServerResult> results;
    results.reserve(server_names.size());

    for (const auto& name : server_names) {
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        auto [child_src, child_tok] = ctx.cancel_token.child();
        (void)child_src;

        batbox::mcp::IMcpTransport* transport = registry_.get(name);
        results.push_back(query_server(name, transport, std::move(child_tok)));
    }

    // ------------------------------------------------------------------
    // 4. Format and return.
    // ------------------------------------------------------------------
    std::string body    = format_results(results);
    Json        payload = build_payload(results);

    return ToolResult::ok(std::move(body), std::move(payload));
}

} // namespace batbox::tools
