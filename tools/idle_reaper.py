#!/usr/bin/env python3
"""Emulate a NAT/firewall that reaps idle TCP flows.

Sits between the co-op client and host and closes BOTH sides of a connection
that carries no bytes for IDLE_SECS -- which is exactly what killed six joins
on a tester's router port-forward (2026-07-31).  Point the client at this
instead of the host and you reproduce the bug locally, no router required.

    ./idle_reaper.py [listen_port] [target_host] [target_port] [idle_secs]
    ./idle_reaper.py 9090 127.0.0.1 8080 20

Then join `localhost:9090` from the client instead of `localhost:8080`.
"""
import socket
import select
import sys
import threading
import time

LISTEN_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9090
TARGET_HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
TARGET_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8080
IDLE_SECS = float(sys.argv[4]) if len(sys.argv) > 4 else 20.0


def pump(client, tag):
    try:
        upstream = socket.create_connection((TARGET_HOST, TARGET_PORT))
    except OSError as exc:
        print(f"[{tag}] upstream connect failed: {exc}", flush=True)
        client.close()
        return
    print(f"[{tag}] open -> {TARGET_HOST}:{TARGET_PORT} (reaping after "
          f"{IDLE_SECS}s of silence)", flush=True)
    socks = [client, upstream]
    last = time.monotonic()
    total = 0
    try:
        while True:
            readable, _, bad = select.select(socks, [], socks, 0.5)
            if bad:
                print(f"[{tag}] socket error", flush=True)
                return
            moved = 0
            for s in readable:
                peer = upstream if s is client else client
                try:
                    data = s.recv(65536)
                except OSError:
                    data = b""
                if not data:
                    who = "client" if s is client else "host"
                    print(f"[{tag}] {who} closed after {total} bytes", flush=True)
                    return
                peer.sendall(data)
                moved += len(data)
            if moved:
                total += moved
                last = time.monotonic()
            elif time.monotonic() - last > IDLE_SECS:
                print(f"[{tag}] *** REAPED *** no traffic for {IDLE_SECS}s "
                      f"(carried {total} bytes)", flush=True)
                return
    finally:
        client.close()
        try:
            upstream.close()
        except OSError:
            pass


def preflight():
    """Refuse to start if the host isn't listening yet.

    The co-op host opens its listen socket only AFTER it loads into the world --
    'host armed on port 8080 (thread deferred to do_turn)' in the menu means
    NOTHING is listening yet.  Without this check the relay accepts the client,
    fails to reach the host, and closes the client socket with the probe still
    unread, which the client sees as an instant 'Connection reset by peer' and
    then a 5s handshake timeout -- a confusing failure that looks like a co-op
    bug and isn't (burned a test round on 2026-07-31).
    """
    try:
        probe = socket.create_connection((TARGET_HOST, TARGET_PORT), timeout=3)
        probe.close()
        return True
    except OSError as exc:
        print(f"idle-reaper: REFUSING TO START -- nothing listening on "
              f"{TARGET_HOST}:{TARGET_PORT} ({exc}).", flush=True)
        print("  The host must be LOADED INTO THE WORLD, not just armed at the "
              "menu -- the listen thread is deferred to do_turn.", flush=True)
        print(f"  Check with:  lsof -nP -iTCP:{TARGET_PORT} -sTCP:LISTEN",
              flush=True)
        return False


def main():
    if not preflight():
        sys.exit(1)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", LISTEN_PORT))
    listener.listen(8)
    print(f"idle-reaper: :{LISTEN_PORT} -> {TARGET_HOST}:{TARGET_PORT}, "
          f"idle timeout {IDLE_SECS}s", flush=True)
    n = 0
    while True:
        client, addr = listener.accept()
        n += 1
        tag = f"{n}:{addr[0]}:{addr[1]}"
        threading.Thread(target=pump, args=(client, tag), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nidle-reaper: stopped", flush=True)
