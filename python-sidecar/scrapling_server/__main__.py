"""Entry point for ``python3 -m scrapling_server``.

Usage (invoked by SidecarManager in src/sidecar/SidecarManager.cpp):

    python3 -m scrapling_server [--port PORT]

Port selection:
    If BATBOX_SIDECAR_PORT is set to a non-zero integer, that port is used.
    If --port is supplied it overrides the env var.
    If neither is set (or the value is 0), the OS assigns a free port on
    127.0.0.1 by binding to port 0 and releasing before uvicorn takes over.

Port discovery (contract with C++ host):
    Immediately after the port is known the server writes to stderr:

        LISTENING:<port>

    The C++ host reads this line from the pipe attached to the child stderr.
    This is the only reliable way to surface the OS-assigned port without a
    race between the parent reading and uvicorn actually binding.

    The line is flushed before uvicorn starts so the host can proceed to poll
    GET /healthz.
"""

from __future__ import annotations

import argparse
import os
import socket
import sys


def _pick_free_port() -> int:
    """Bind to port 0 on 127.0.0.1 to let the OS pick a free port, then
    release the socket and return the chosen port number.

    There is a brief TOCTOU window between releasing the socket and uvicorn
    re-binding it.  In practice this is safe on loopback with SO_REUSEADDR
    because no other process contends for the exact port the OS just assigned.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main() -> None:
    """Parse arguments, resolve the listen port, announce it, then hand off
    to uvicorn.  This function never returns on success (uvicorn replaces the
    process's event loop).
    """
    parser = argparse.ArgumentParser(
        prog="scrapling_server",
        description="BatBox Scrapling sidecar (loopback HTTP IPC bridge)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=0,
        help="TCP port to listen on (default: 0 → OS assigns). "
             "Overrides BATBOX_SIDECAR_PORT env var.",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="Bind address (default: 127.0.0.1). Do NOT expose to 0.0.0.0 "
             "in production; this sidecar has no authentication.",
    )
    args = parser.parse_args()

    # Port resolution order: --port arg > BATBOX_SIDECAR_PORT env > 0 (OS assigns)
    port: int = args.port
    if port == 0:
        env_port_str = os.environ.get("BATBOX_SIDECAR_PORT", "0")
        try:
            port = int(env_port_str)
        except ValueError:
            port = 0

    if port == 0:
        port = _pick_free_port()

    # Announce port to C++ host BEFORE uvicorn starts binding.
    # The C++ reader thread watches stderr for this exact line.
    print(f"LISTENING:{port}", file=sys.stderr, flush=True)

    # Lazy import: uvicorn and fastapi are only imported here so that the
    # LISTENING line is printed with minimal startup overhead.  The C++ host
    # starts polling /healthz immediately after reading the line; uvicorn will
    # be ready within the BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC window (default 15s).
    import uvicorn  # noqa: PLC0415

    uvicorn.run(
        "scrapling_server.app:app",
        host=args.host,
        port=port,
        log_level="warning",   # sidecar logs via spdlog in the C++ host; keep uvicorn quiet
        access_log=False,      # access logs are noise; C++ host logs at the cpr layer
        loop="asyncio",
    )


if __name__ == "__main__":
    main()
