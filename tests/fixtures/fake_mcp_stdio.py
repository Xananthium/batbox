#!/usr/bin/env python3
"""
tests/fixtures/fake_mcp_stdio.py
----------------------------------
Minimal MCP stdio server fixture for integration testing StdioTransport.

Protocol:
  - Reads JSON-RPC 2.0 messages from stdin using LSP Content-Length framing.
  - Writes JSON-RPC 2.0 responses/notifications to stdout using the same framing.

Supported methods:
  initialize          → returns capabilities with protocol version + tools capability
  tools/list          → returns one fake tool: "echo_tool"
  tools/call          → if tool name is "echo_tool", echoes input arguments as result
  notifications/exit  → exits cleanly with code 0

Usage (standalone):
  python3 fake_mcp_stdio.py

Usage from C++ integration test:
  Spawned as a child process by StdioTransport.
  Test sends initialize → tools/list → tools/call → stop().

Error cases:
  - unknown methods return a JSON-RPC error response (-32601 methodNotFound)
"""

import json
import sys
import os


# ---------------------------------------------------------------------------
# Content-Length framing helpers
# ---------------------------------------------------------------------------

def read_exact(stream, n: int) -> bytes | None:
    """Read exactly n bytes from a binary stream. Returns None on EOF."""
    data = b""
    while len(data) < n:
        chunk = stream.read(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def read_message(stream) -> dict | None:
    """Read one Content-Length-framed JSON-RPC message from a binary stream.
    Returns None on EOF or malformed input.
    """
    # Read headers byte by byte until we see \r\n\r\n
    header_bytes = b""
    while True:
        ch = stream.read(1)
        if not ch:
            return None  # EOF
        header_bytes += ch
        if header_bytes.endswith(b"\r\n\r\n"):
            break
        # Safety limit: headers should not exceed 4 KB
        if len(header_bytes) > 4096:
            return None

    # Parse Content-Length from accumulated headers
    headers = header_bytes.decode("ascii", errors="replace")
    content_length = None
    for line in headers.split("\r\n"):
        if line.lower().startswith("content-length:"):
            try:
                content_length = int(line.split(":", 1)[1].strip())
            except ValueError:
                pass
            break

    if content_length is None or content_length < 0:
        return None

    body = read_exact(stream, content_length)
    if body is None:
        return None

    try:
        return json.loads(body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def write_message(obj: dict) -> None:
    """Write a JSON-RPC message with Content-Length framing to stdout."""
    body = json.dumps(obj).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    # Write directly to the underlying binary buffer to avoid text-mode issues
    sys.stdout.buffer.write(header + body)
    sys.stdout.buffer.flush()


# ---------------------------------------------------------------------------
# MCP response builders
# ---------------------------------------------------------------------------

def make_response(req_id, result: dict) -> dict:
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "result": result,
    }


def make_error_response(req_id, code: int, message: str) -> dict:
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "error": {
            "code": code,
            "message": message,
        },
    }


# ---------------------------------------------------------------------------
# Method handlers
# ---------------------------------------------------------------------------

def handle_initialize(req_id, params: dict) -> dict:
    """Return MCP capabilities: protocol version + tools support."""
    return make_response(req_id, {
        "protocolVersion": "2024-11-05",
        "capabilities": {
            "tools": {},
        },
        "serverInfo": {
            "name": "fake-mcp-stdio",
            "version": "0.1.0",
        },
    })


def handle_tools_list(req_id, params: dict) -> dict:
    """Return a list containing one tool: echo_tool."""
    return make_response(req_id, {
        "tools": [
            {
                "name": "echo_tool",
                "description": "Echoes the input arguments back as the result.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "message": {
                            "type": "string",
                            "description": "Message to echo",
                        }
                    },
                    "required": ["message"],
                },
            }
        ]
    })


def handle_tools_call(req_id, params: dict) -> dict:
    """Execute echo_tool: returns the message argument in the content array."""
    tool_name = params.get("name", "")
    arguments = params.get("arguments", {})

    if tool_name != "echo_tool":
        return make_error_response(req_id, -32601, f"Unknown tool: {tool_name}")

    message = arguments.get("message", "")
    return make_response(req_id, {
        "content": [
            {
                "type": "text",
                "text": message,
            }
        ],
        "isError": False,
    })


# ---------------------------------------------------------------------------
# Main dispatch loop
# ---------------------------------------------------------------------------

def main() -> None:
    # Signal readiness to stderr (visible in test logs but not mixed with protocol)
    sys.stderr.write("READY\n")
    sys.stderr.flush()

    # Use the raw binary stdin buffer so we can do exact byte reads without
    # buffering surprises from text-mode line handling.
    stdin_bin = sys.stdin.buffer

    while True:
        msg = read_message(stdin_bin)
        if msg is None:
            # EOF — parent closed stdin or process is being shut down
            break

        method = msg.get("method", "")
        req_id = msg.get("id")  # None for notifications
        params = msg.get("params") or {}

        # Handle notification: no response expected
        if req_id is None:
            if method == "notifications/exit":
                sys.exit(0)
            # Ignore other notifications
            continue

        # Dispatch request methods
        if method == "initialize":
            response = handle_initialize(req_id, params)
        elif method == "tools/list":
            response = handle_tools_list(req_id, params)
        elif method == "tools/call":
            response = handle_tools_call(req_id, params)
        else:
            response = make_error_response(req_id, -32601, f"Method not found: {method}")

        write_message(response)


if __name__ == "__main__":
    main()
