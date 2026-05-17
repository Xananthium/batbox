# =============================================================================
# Component C8 — MCP Client (batbox_mcp)
# Four transports: stdio, sse, http, ws.
# JSON-RPC 2.0, capability negotiation, server registry.
# Depends on: batbox_core, batbox_config
# =============================================================================

add_library(batbox_mcp OBJECT)

set_target_properties(batbox_mcp PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_mcp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_mcp
    PUBLIC
        batbox_core
        batbox_config
        batbox_inference
        cpr::cpr
        ixwebsocket::ixwebsocket
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 8.x task lands:
# McpClient.cpp now included (CPP 8.9 — McpClient orchestrator)
# ---------------------------------------------------------------------------
target_sources(batbox_mcp
    PRIVATE
        .placeholder.cpp
        McpClient.cpp           # CPP 8.9 — McpClient orchestrator
        StdioTransport.cpp      # CPP 8.4 — stdin/stdout child-process transport
        HttpTransport.cpp       # CPP 8.5 — HTTP POST JSON-RPC transport
        SseTransport.cpp        # CPP 8.6 — SSE streaming transport
        WsTransport.cpp         # CPP 8.7 — IXWebSocket-based MCP transport
        JsonRpc.cpp
        McpFraming.cpp
        McpServerRegistry.cpp   # CPP 8.8 — server registry + health monitor
)
