#!/usr/bin/env python3
"""Communicate with the autonomous BatBox through durable issues."""
import argparse
import json
from pathlib import Path
import sys
from urllib.error import HTTPError
from urllib.request import Request, urlopen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", default=str(Path.home() / ".batbox/autonomous"))
    parser.add_argument("--url", default="http://127.0.0.1:8772")
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create")
    create.add_argument("title")
    create.add_argument("description", nargs="?", default="")
    sub.add_parser("list")
    sub.add_parser("status")
    sub.add_parser("capabilities")
    show = sub.add_parser("show")
    show.add_argument("id")
    comment = sub.add_parser("comment")
    comment.add_argument("id")
    comment.add_argument("content")
    update = sub.add_parser("set-status")
    update.add_argument("id")
    update.add_argument("status")
    update.add_argument("--reason", default="")
    args = parser.parse_args()
    token = (Path(args.state_dir).expanduser() / "api.key").read_text().strip()
    body = None
    method = "GET"
    path = "/v1/" + args.command
    if args.command == "create":
        method, path = "POST", "/v1/issues"
        body = {"title": args.title, "description": args.description}
    elif args.command == "list":
        path = "/v1/issues"
    elif args.command == "show":
        path = "/v1/issues/" + args.id
    elif args.command == "comment":
        method, path = "POST", "/v1/issues/" + args.id + "/comments"
        body = {"content": args.content}
    elif args.command == "set-status":
        method, path = "PATCH", "/v1/issues/" + args.id
        body = {"status": args.status, "reason": args.reason}
    req = Request(args.url.rstrip("/") + path, json.dumps(body).encode() if body is not None else None,
                  {"Authorization": "Bearer " + token, "Content-Type": "application/json"}, method=method)
    try:
        with urlopen(req, timeout=30) as response:
            result = json.load(response)
    except HTTPError as exc:
        print(exc.read().decode(), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
