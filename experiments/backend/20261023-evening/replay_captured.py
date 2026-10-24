#!/usr/bin/env python3
"""Replay a private captured OpenAI body; never execute returned tools.

No model calls occur without --run-live. Raw requests/responses stay in a
0700 private directory; stdout and summary.json contain metadata only.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import time
import urllib.request


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha(value):
    return hashlib.sha256(value).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def private_write(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def repetition(text):
    words = re.findall(r"\w+|[^\w\s]", text.lower())
    same = 0
    if words:
        for token in reversed(words):
            if token != words[-1]:
                break
            same += 1
    best = {"period_words": 0, "repetitions": 0, "tail_words": 0}
    for width in range(1, min(33, len(words) // 3 + 1)):
        pattern = words[-width:]
        count = 1
        while (len(words) >= (count + 1) * width
               and words[-(count + 1) * width:-count * width] == pattern):
            count += 1
        if count >= 3 and count * width > best["tail_words"]:
            best = {"period_words": width, "repetitions": count,
                    "tail_words": count * width}
    grams = [tuple(words[i:i + 8]) for i in range(max(0, len(words) - 7))]
    return {"word_tokens": len(words), "same_word_tail": same,
            "tail_cycle": best,
            "repeated_8gram_fraction": 1 - len(set(grams)) / len(grams) if grams else 0}


def metrics(base):
    try:
        with urllib.request.urlopen(base + "/metrics", timeout=10) as response:
            return response.read().decode()
    except Exception as exc:
        return "# metrics unavailable: " + type(exc).__name__


def run(args):
    raw = args.payload.read_bytes()
    original = json.loads(raw)
    if not original.get("stream"):
        raise ValueError("Capture must already be streaming; refusing implicit body changes")
    payload = json.loads(raw)
    changes = {}
    if args.seed is not None:
        changes["seed"] = args.seed
    if args.cache_condition == "fresh":
        changes["cache_salt"] = secrets.token_urlsafe(32)
    if args.cache_salt_file:
        changes["cache_salt"] = args.cache_salt_file.read_text().strip()
    if args.logprobs:
        changes.update(logprobs=True, top_logprobs=5)
    payload.update(changes)
    body = raw if not changes else json.dumps(payload).encode()
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=False, mode=0o700)
    private_write(directory / "request.json", payload)
    if "cache_salt" in payload:
        fd = os.open(directory / "cache-salt.txt", os.O_WRONLY | os.O_CREAT, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(payload["cache_salt"])
    before = metrics(args.base)
    (directory / "metrics-before.prom").write_text(before)
    started = utc()
    start = time.monotonic()
    first_event = first_output = None
    content = reasoning = ""
    calls = {}
    ids = set()
    usage = {}
    finish = []
    stop = "eof"
    error = None
    count = 0
    deltas = []
    request = urllib.request.Request(args.base + "/v1/chat/completions", data=body,
                                     headers={"Content-Type": "application/json"})
    fd = os.open(directory / "stream.jsonl", os.O_WRONLY | os.O_CREAT, 0o600)
    try:
        with os.fdopen(fd, "w") as stream, urllib.request.urlopen(
                request, timeout=args.read_timeout) as response:
            for raw_line in response:
                elapsed = time.monotonic() - start
                line = raw_line.decode().strip()
                if not line.startswith("data:"):
                    continue
                if line[5:].strip() == "[DONE]":
                    stop = "done"
                    break
                event = json.loads(line[5:].strip())
                stream.write(json.dumps({"at": utc(), "seconds": elapsed,
                                         "event": event}) + "\n")
                stream.flush()
                count += 1
                if first_event is None:
                    first_event = elapsed
                if event.get("id"):
                    ids.add(event["id"])
                if event.get("usage"):
                    usage = event["usage"]
                if event.get("error"):
                    stop = "server_error"
                    break
                for choice in event.get("choices", []):
                    delta = choice.get("delta", {})
                    if any(delta.get(k) for k in ("content", "reasoning", "reasoning_content", "tool_calls")):
                        if first_output is None:
                            first_output = elapsed
                        deltas.append(elapsed)
                    content += delta.get("content") or ""
                    reasoning += delta.get("reasoning") or delta.get("reasoning_content") or ""
                    for call in delta.get("tool_calls", []):
                        item = calls.setdefault(str(call.get("index", 0)),
                                                {"id": "", "name": "", "arguments": ""})
                        item["id"] = call.get("id") or item["id"]
                        fn = call.get("function", {})
                        item["name"] += fn.get("name") or ""
                        item["arguments"] += fn.get("arguments") or ""
                    if choice.get("finish_reason"):
                        finish.append(choice["finish_reason"])
                for channel in (content, reasoning):
                    r = repetition(channel[-12000:])
                    cycle = r["tail_cycle"]
                    if r["same_word_tail"] >= args.abort_same_word or (
                            cycle["tail_words"] >= 512 and cycle["repetitions"] >= 32):
                        stop = "early_censored_clear_repetition"
                        break
                if stop == "early_censored_clear_repetition":
                    break
                if elapsed >= args.deadline:
                    stop = "early_censored_deadline"
                    break
    except Exception as exc:
        stop = "transport_error"
        error = type(exc).__name__
        private_write(directory / "error-private.json", {"type": error, "message": str(exc)})
    wall = time.monotonic() - start
    private_write(directory / "response-private.json", {"content": content,
                  "reasoning": reasoning, "tools": calls, "usage": usage})
    (directory / "metrics-after.prom").write_text(metrics(args.base))
    tool_summary = []
    for call in calls.values():
        try:
            parsed = json.loads(call["arguments"])
            valid = isinstance(parsed, dict)
        except Exception:
            valid = False
        tool_summary.append({"name": call["name"], "argument_chars": len(call["arguments"]),
                             "arguments_valid_json_object": valid})
    sampler_keys = ("temperature", "top_p", "top_k", "min_p", "repetition_penalty",
                    "frequency_penalty", "presence_penalty", "seed", "max_tokens",
                    "max_completion_tokens", "chat_template_kwargs")
    summary = {"started_at": started, "ended_at": utc(), "label": args.label,
               "exact_captured_body_replay": not changes,
               "capture_body_sha256": sha(raw), "sent_body_sha256": sha(body),
               "prompt_tools_sha256": sha(canonical({k: payload.get(k) for k in
                   ("messages", "tools", "tool_choice", "chat_template_kwargs")})),
               "override_fields": sorted(changes), "request_fields": sorted(payload),
               "request_sampling": {k: payload[k] for k in sampler_keys if k in payload},
               "omitted_sampling_inherits_server": [k for k in sampler_keys if k not in payload],
               "cache_condition": args.cache_condition,
               "request_cache_salt_sha256": sha(payload["cache_salt"].encode()) if payload.get("cache_salt") else None,
               "cache_salt_present_in_input": bool(original.get("cache_salt")),
               "request_ids": sorted(ids),
               "first_event_seconds": first_event, "first_output_seconds": first_output,
               "wall_seconds": wall, "stream_events": count, "output_deltas": len(deltas),
               "finish_reasons": finish, "stop_condition": stop, "transport_error": error,
               "usage": usage, "content_chars": len(content), "reasoning_chars": len(reasoning),
               "content_repetition": repetition(content), "reasoning_repetition": repetition(reasoning),
               "tools": tool_summary, "tools_executed": False,
               "limits": {"same_word_abort": args.abort_same_word,
                          "deadline_seconds": args.deadline, "socket_read_seconds": args.read_timeout},
               "limitations": ["An early-censored response has no final usage or finish reason.",
                                "Wall timing includes shared-host contention unless isolation is documented.",
                                "Omitted request sampler fields require the separately recorded server defaults."]}
    private_write(directory / "summary.json", summary)
    print(json.dumps(summary, indent=2), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--payload", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--base", default="http://127.0.0.1:8001")
    p.add_argument("--run-live", action="store_true")
    p.add_argument("--seed", type=int)
    p.add_argument("--cache-condition", choices=("inherited", "fresh"), default="inherited")
    p.add_argument("--cache-salt-file", type=Path)
    p.add_argument("--logprobs", action="store_true")
    p.add_argument("--abort-same-word", type=int, default=256)
    p.add_argument("--deadline", type=float, default=1800)
    p.add_argument("--read-timeout", type=float, default=600)
    args = p.parse_args()
    if not args.run_live:
        p.error("No request sent: --run-live is required after coordinating backend access")
    if args.cache_condition == "fresh" and args.cache_salt_file:
        p.error("Choose a new salt OR a prior salt file")
    run(args)


if __name__ == "__main__":
    main()
