#!/usr/bin/env python3
"""
tests/fixtures/fake_mcp_ws.py
-------------------------------
Minimal JSON-RPC 2.0 WebSocket server for WsTransport integration testing.

Usage (standalone):
    python3 fake_mcp_ws.py [--port PORT]

Usage from C++ doctest via subprocess:
    The test binary spawns this script, reads "READY <port>" from its stdout,
    exercises WsTransport against it, then sends SIGTERM.

Routes / behaviours:
    method "ping"              → { "result": "pong" }
    method "echo"              → { "result": params }
    method "error_method"      → JSON-RPC error { "code": -32000, "message": "deliberate error" }
    method "push_notification" → server sends a notification
                                 { "method": "notifications/test", "params": {"n": 1} }
                                 then returns { "result": "sent" }

Port selection:
    Binds to 127.0.0.1 on a random ephemeral port (0) by default, or the
    value passed with --port.  Prints "READY <port>" to stdout so the test
    harness knows which port to use.

Requires:
    websockets >= 12.0  (pip install websockets)
"""

import argparse
import asyncio
import json
import sys
import threading


# ---------------------------------------------------------------------------
# JSON-RPC helpers
# ---------------------------------------------------------------------------

def make_response(req_id, result):
    return json.dumps({"jsonrpc": "2.0", "id": req_id, "result": result})


def make_error_response(req_id, code, message):
    return json.dumps({
        "jsonrpc": "2.0",
        "id": req_id,
        "error": {"code": code, "message": message}
    })


def make_notification(method, params):
    return json.dumps({"jsonrpc": "2.0", "method": method, "params": params})


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------

async def handle_client(websocket):
    async for raw in websocket:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            # Invalid JSON — send parse error (no id known)
            await websocket.send(make_error_response(None, -32700, "Parse error"))
            continue

        req_id = msg.get("id")
        method = msg.get("method", "")
        params = msg.get("params")

        if method == "initialize":
            await websocket.send(make_response(req_id, {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake-mcp-ws", "version": "0.1.0"},
            }))

        elif method == "ping":
            await websocket.send(make_response(req_id, "pong"))

        elif method == "echo":
            await websocket.send(make_response(req_id, params))

        elif method == "tools/list":
            await websocket.send(make_response(req_id, {"tools": [
                {
                    "name": "ws_tool",
                    "description": "Echoes the message argument (WS transport).",
                    "inputSchema": {"type": "object", "properties": {"message": {"type": "string"}}, "required": ["message"]},
                }
            ]}))

        elif method == "tools/call":
            params = params or {}
            tool_name = params.get("name", "")
            arguments = params.get("arguments", {})
            if tool_name == "ws_tool":
                message = arguments.get("message", "")
                await websocket.send(make_response(req_id, {
                    "content": [{"type": "text", "text": message}],
                    "isError": False,
                }))
            else:
                await websocket.send(make_error_response(req_id, -32601, f"Unknown tool: {tool_name}"))

        elif method == "error_method":
            await websocket.send(
                make_error_response(req_id, -32000, "deliberate error")
            )

        elif method == "push_notification":
            # First, send a server-initiated notification.
            notif = make_notification("notifications/test", {"n": 1})
            await websocket.send(notif)
            # Then confirm to the caller.
            await websocket.send(make_response(req_id, "sent"))

        else:
            await websocket.send(
                make_error_response(req_id, -32601, f"Method not found: {method}")
            )


# ---------------------------------------------------------------------------
# Server entry point
# ---------------------------------------------------------------------------

async def run_server(port: int, ready_event: asyncio.Event, result_box: list):
    """Start the WebSocket server and signal when ready."""
    import websockets

    async with websockets.serve(handle_client, "127.0.0.1", port) as server:
        actual_port = server.sockets[0].getsockname()[1]
        result_box.append(actual_port)
        ready_event.set()

        # Serve until the process is killed.
        await asyncio.Future()  # run forever


def main():
    parser = argparse.ArgumentParser(description="Fake MCP WebSocket server for batbox tests")
    parser.add_argument("--port", type=int, default=0,
                        help="Port to bind (0 = random ephemeral)")
    args = parser.parse_args()

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    ready_event = asyncio.Event()
    result_box: list = []

    async def main_coro():
        await run_server(args.port, ready_event, result_box)

    # Start server in background task.
    task = loop.create_task(main_coro())

    # Wait for ready signal, then print port to stdout.
    async def wait_and_print():
        await ready_event.wait()
        port = result_box[0]
        print(f"READY {port}", flush=True)
        # Keep running until cancelled.
        try:
            await task
        except asyncio.CancelledError:
            pass

    try:
        loop.run_until_complete(wait_and_print())
    except KeyboardInterrupt:
        pass
    finally:
        loop.close()


if __name__ == "__main__":
    main()
