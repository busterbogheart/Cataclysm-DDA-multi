#!/usr/bin/env python3
"""
Minimal test client for cataclysm-tiles multiplayer.
Simulates player 2 — connects, joins, then lets you send movement commands.

Usage:
    python3 tools/mp_client.py [host] [port] [name] [password]

    python3 tools/mp_client.py localhost 8080 player2
    python3 tools/mp_client.py localhost 8080 bob mypassword

Controls (after connected):
    w / k      move north
    s / j      move south
    d / l      move east
    a / h      move west
    q / 7      move northwest
    e / 9      move northeast
    z / 1      move southwest
    c / 3      move southeast
    quit       disconnect and exit
    raw <json> send raw JSON message
"""

import socket
import sys
import threading
import json

HOST = sys.argv[1] if len(sys.argv) > 1 else "localhost"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
NAME = sys.argv[3] if len(sys.argv) > 3 else "player2"
PASSWORD = sys.argv[4] if len(sys.argv) > 4 else ""

KEY_TO_DIR = {
    "w": "n", "k": "n",
    "s": "s", "j": "s",
    "d": "e", "l": "e",
    "a": "w", "h": "w",
    "q": "nw", "7": "nw",
    "e": "ne", "9": "ne",
    "z": "sw", "1": "sw",
    "c": "se", "3": "se",
}


def recv_loop(sock):
    buf = b""
    while True:
        try:
            data = sock.recv(4096)
            if not data:
                print("\n[disconnected by server]")
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    msg = json.loads(line.decode())
                    mtype = msg.get("type", "?")
                    if mtype == "state":
                        pos = msg.get("pos", {})
                        hp = msg.get("hp", "?")
                        hp_max = msg.get("hp_max", "?")
                        print(f"\n[state] pos=({pos.get('x')},{pos.get('y')},{pos.get('z')}) "
                              f"hp={hp}/{hp_max}", flush=True)
                    elif mtype == "hello":
                        print(f"[server] protocol={msg.get('protocol')} version={msg.get('version')}")
                    elif mtype == "welcome":
                        print(f"[server] welcome! player_id={msg.get('player_id')}")
                    elif mtype in ("player_joined", "player_left"):
                        print(f"\n[{mtype}] {msg.get('name')}")
                    elif mtype == "error":
                        print(f"\n[error] {msg.get('message')}")
                    else:
                        print(f"\n[recv] {msg}")
                except json.JSONDecodeError:
                    print(f"\n[raw] {line.decode()}")
        except OSError:
            return


def send_json(sock, obj):
    sock.sendall((json.dumps(obj) + "\n").encode())


def main():
    print(f"Connecting to {HOST}:{PORT} as '{NAME}'...")
    sock = socket.create_connection((HOST, PORT))

    t = threading.Thread(target=recv_loop, args=(sock,), daemon=True)
    t.start()

    # Authenticate
    join_msg = {"type": "join", "name": NAME}
    if PASSWORD:
        join_msg["password"] = PASSWORD
    send_json(sock, join_msg)

    print("Connected. Type a direction key + Enter to move, 'quit' to exit.")
    print("  n/s/e/w  (or w/s/d/a)  diagonals: q/e/z/c")

    while True:
        try:
            line = input("> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            break

        if not line:
            continue
        if line in ("quit", "exit", "q"):
            send_json(sock, {"type": "quit"})
            break
        if line.startswith("raw "):
            try:
                send_json(sock, json.loads(line[4:]))
            except json.JSONDecodeError as e:
                print(f"bad json: {e}")
            continue

        direction = KEY_TO_DIR.get(line)
        if direction:
            send_json(sock, {"type": "action", "action": "move", "dir": direction})
        else:
            print(f"unknown key '{line}' — use w/a/s/d or q/e/z/c, or 'quit'")

    sock.close()
    print("Bye.")


if __name__ == "__main__":
    main()
