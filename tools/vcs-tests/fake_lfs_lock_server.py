#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""A Git LFS lock API server for the version control tests (docs/specs/S29-version-control.md D8).

Implements the lock endpoints of the Git LFS API (POST /locks, POST /locks/verify, GET /locks,
POST /locks/:id/unlock) for the users named on the command line, in memory. A client tells who it
is by the first segment of its lfs.url path:

    git config lfs.url http://127.0.0.1:<port>/<user>/repo.git/info/lfs

so two clones with different users need no credentials. Tests only: it listens on 127.0.0.1, an
OS-chosen port written to --port-file once it listens, and logs one line per request to --log
(the tests count lock-server requests with it).

    python3 fake_lfs_lock_server.py --port-file PORT --log LOG --users alice,bob
"""

import argparse
import datetime
import json
import os
import socketserver
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

LOCKS = {}          # id -> lock
LOCK = threading.Lock()
NEXT_ID = [1]
USERS = set()
LOG_PATH = None


def log_request(line):
    if LOG_PATH:
        with LOCK:
            with open(LOG_PATH, "a", encoding="utf-8") as log:
                log.write(line + "\n")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # noqa: A002 (the base class's name)
        pass

    def route(self):
        """(user, endpoint) of the request path, e.g. ("alice", "/locks/3/unlock")"""
        path = urlsplit(self.path).path
        parts = [p for p in path.split("/") if p]
        if not parts or parts[0] not in USERS:
            return None, None
        if "locks" not in parts:
            return parts[0], None
        return parts[0], "/" + "/".join(parts[parts.index("locks"):])

    def reply(self, status, body):
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/vnd.git-lfs+json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(length).decode("utf-8") or "{}")
        except ValueError:
            return {}

    def do_GET(self):
        user, endpoint = self.route()
        log_request(f"GET {endpoint} {user}")
        if user is None or endpoint != "/locks":
            return self.reply(404, {"message": "not found"})
        query = parse_qs(urlsplit(self.path).query)
        with LOCK:
            locks = list(LOCKS.values())
        if "path" in query:
            locks = [lock for lock in locks if lock["path"] == query["path"][0]]
        if "id" in query:
            locks = [lock for lock in locks if lock["id"] == query["id"][0]]
        return self.reply(200, {"locks": locks, "next_cursor": ""})

    def do_POST(self):
        user, endpoint = self.route()
        request = self.body()
        log_request(f"POST {endpoint} {user}")
        if user is None or endpoint is None:
            return self.reply(404, {"message": "not found"})
        if endpoint == "/locks":
            path = request.get("path", "")
            with LOCK:
                for lock in LOCKS.values():
                    if lock["path"] == path:
                        return self.reply(409, {"lock": lock, "message": "already created lock"})
                lock = {
                    "id": str(NEXT_ID[0]),
                    "path": path,
                    "locked_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    "owner": {"name": user},
                }
                NEXT_ID[0] += 1
                LOCKS[lock["id"]] = lock
            return self.reply(201, {"lock": lock})
        if endpoint == "/locks/verify":
            with LOCK:
                locks = list(LOCKS.values())
            ours = [lock for lock in locks if lock["owner"]["name"] == user]
            theirs = [lock for lock in locks if lock["owner"]["name"] != user]
            return self.reply(200, {"ours": ours, "theirs": theirs, "next_cursor": ""})
        parts = endpoint.split("/")
        if len(parts) == 4 and parts[1] == "locks" and parts[3] == "unlock":
            with LOCK:
                lock = LOCKS.get(parts[2])
                if lock is None:
                    return self.reply(404, {"message": "unable to find lock"})
                if lock["owner"]["name"] != user and not request.get("force"):
                    return self.reply(403, {"message": f"lock is owned by {lock['owner']['name']}"})
                del LOCKS[parts[2]]
            return self.reply(200, {"lock": lock})
        return self.reply(404, {"message": "not found"})


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):
        # HTTPServer's also names the host with socket.getfqdn, a reverse lookup that can outlast
        # the tests' wait for the port (macOS runners); nothing here reads server_name
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def main():
    global LOG_PATH
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port-file", required=True)
    parser.add_argument("--log")
    parser.add_argument("--users", default="alice,bob")
    args = parser.parse_args()
    USERS.update(u for u in args.users.split(",") if u)
    LOG_PATH = args.log
    server = Server(("127.0.0.1", 0), Handler)
    with open(args.port_file + ".tmp", "w", encoding="utf-8") as out:
        out.write(str(server.server_address[1]))
    # renamed into place: a reader never sees a half-written port
    os.replace(args.port_file + ".tmp", args.port_file)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
