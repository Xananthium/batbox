"""Count response SSE fields for one local native PID; never save packet/text data.

Requires tcpdump capture privileges. Only server->client packets on the PID's
current established localhost connections are captured. No requests are sent.
"""
import argparse
from collections import Counter
import datetime
import json
import os
from pathlib import Path
import selectors
import signal
import struct
import subprocess
import time


def client_ports(pid, server_port):
    inodes = set()
    for path in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = os.readlink(path)
        except OSError:
            continue
        if target.startswith("socket:["):
            inodes.add(target[8:-1])
    ports = []
    for row in Path(f"/proc/{pid}/net/tcp").read_text().splitlines()[1:]:
        fields = row.split()
        local, remote = fields[1].split(":"), fields[2].split(":")
        if (fields[9] in inodes and fields[3] == "01" and
                local[0] == remote[0] == "0100007F" and
                int(remote[1], 16) == server_port):
            ports.append(int(local[1], 16))
    return sorted(set(ports))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--server-port", type=int, default=8001)
    parser.add_argument("--seconds", type=float, default=45)
    args = parser.parse_args()
    ports = client_ports(args.pid, args.server_port)
    if not ports:
        raise SystemExit("No matching established localhost sockets")
    capture_filter = (f"src host 127.0.0.1 and dst host 127.0.0.1 and tcp src port {args.server_port} and (" +
                      " or ".join(f"tcp dst port {port}" for port in ports) + ")")
    process = subprocess.Popen(["tcpdump", "-i", "lo", "-nn", "-U", "-s", "0", "-w", "-", capture_filter],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    deadline = time.monotonic() + args.seconds
    packet_buffer = bytearray()
    streams = {}
    counters = {port: Counter() for port in ports}
    byte_order = None
    linktype = None
    try:
        while time.monotonic() < deadline and Path(f"/proc/{args.pid}").exists():
            if not selector.select(timeout=min(0.5, max(0, deadline - time.monotonic()))):
                continue
            block = os.read(process.stdout.fileno(), 65536)
            if not block:
                break
            packet_buffer.extend(block)
            if byte_order is None:
                if len(packet_buffer) < 24:
                    continue
                byte_order = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">"}.get(bytes(packet_buffer[:4]))
                if byte_order is None:
                    raise RuntimeError("Unsupported pcap format")
                linktype = struct.unpack_from(byte_order + "I", packet_buffer, 20)[0]
                del packet_buffer[:24]
                if linktype != 1:
                    raise RuntimeError("This capture reader requires Ethernet framing on lo")
            while len(packet_buffer) >= 16:
                length = struct.unpack_from(byte_order + "I", packet_buffer, 8)[0]
                if length > 1048576:
                    raise RuntimeError("Unexpected capture record size")
                if len(packet_buffer) < 16 + length:
                    break
                packet = bytes(packet_buffer[16:16 + length])
                del packet_buffer[:16 + length]
                if len(packet) < 54 or packet[12:14] != b"\x08\x00":
                    continue
                ip = 14
                if packet[ip] >> 4 != 4 or packet[ip + 9] != 6:
                    continue
                tcp = ip + (packet[ip] & 15) * 4
                source, destination, sequence = struct.unpack_from("!HHI", packet, tcp)
                if source != args.server_port or destination not in counters:
                    continue
                payload = packet[tcp + (packet[tcp + 12] >> 4) * 4:]
                if not payload:
                    continue
                count = counters[destination]
                previous_end, pending = streams.get(destination, (sequence, bytearray()))
                if sequence < previous_end:
                    payload = payload[previous_end - sequence:]
                elif sequence > previous_end:
                    count["tcp_gaps"] += 1
                    pending.clear()
                if not payload:
                    continue
                count["payload_bytes"] += len(payload)
                pending.extend(payload)
                streams[destination] = (max(previous_end, sequence + len(packet[tcp + (packet[tcp + 12] >> 4) * 4:])), pending)
                while True:
                    begin = pending.find(b"data: ")
                    if begin < 0:
                        if len(pending) > 32:
                            del pending[:-32]
                        break
                    end = pending.find(b"\n\n", begin)
                    if end < 0:
                        if begin:
                            del pending[:begin]
                        break
                    frame = bytes(pending[begin + 6:end]).strip()
                    del pending[:end + 2]
                    if frame == b"[DONE]":
                        count["done_events"] += 1
                        continue
                    try:
                        event = json.loads(frame)
                    except (ValueError, UnicodeDecodeError):
                        count["unparsed_frames"] += 1
                        continue
                    count["json_sse_events"] += 1
                    for choice in event.get("choices", []):
                        delta = choice.get("delta", {})
                        for name in ("role", "content", "reasoning", "reasoning_content", "tool_calls"):
                            if name in delta:
                                count[name + "_fields"] += 1
                                value = delta[name]
                                if isinstance(value, str) and value:
                                    count[name + "_nonempty"] += 1
                                    count[name + "_utf8_bytes"] += len(value.encode())
                        for tool in delta.get("tool_calls") or []:
                            arguments = tool.get("function", {}).get("arguments")
                            if isinstance(arguments, str) and arguments:
                                count["tool_argument_nonempty_fragments"] += 1
                                count["tool_argument_utf8_bytes"] += len(arguments.encode())
                    del event, frame
                if len(pending) > 524288:
                    pending.clear()
                    count["oversized_incomplete_frame_discarded"] += 1
    finally:
        selector.close()
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    print(json.dumps({"started_at": started, "finished_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      "native_pid": args.pid, "server_port": args.server_port, "client_ports": ports,
                      "native_pid_still_present": Path(f"/proc/{args.pid}").exists(),
                      "field_counts_by_client_port": counters, "capture_filter": capture_filter,
                      "raw_packets_saved": False, "text_values_saved": False, "requests_sent": 0,
                      "limitation": "Bounded passive sample starts mid-connection; gaps/incomplete frames are counted and discarded."}, indent=2))


if __name__ == "__main__":
    main()
