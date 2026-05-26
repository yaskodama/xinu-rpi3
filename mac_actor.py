#!/usr/bin/env python3
"""Mac-side actor.

Sends a message to the Xinu `webactor` HTTP bridge, which delivers it to a
WebReceiver AIPL actor running on Xinu.  Uses only the standard library (raw
socket HTTP POST), so no extra packages are needed.

    Mac actor (this script) --HTTP POST--> Xinu webactor --> AIPL actor

Usage:
    python3 mac_actor.py [host] [message]
        host     Xinu IP (default 192.168.3.50)
        message  text to send (default "hello from the Mac actor")

You can also just use curl:
    curl -d 'hello from mac' http://192.168.3.50:8080/
"""
import socket
import sys

PORT = 8080


def send(host, port, msg):
    body = msg.encode("utf-8")
    req = (
        "POST / HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n" % (host, len(body))
    ).encode("ascii") + body

    s = socket.create_connection((host, port), timeout=6)
    s.sendall(req)
    s.settimeout(3)
    resp = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            resp += d
    except socket.timeout:
        pass
    s.close()
    return resp.decode("latin-1", "replace")


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.3.50"
    msg = sys.argv[2] if len(sys.argv) > 2 else "hello from the Mac actor"
    print("[mac-actor] -> %s:%d  message=%r" % (host, PORT, msg))
    try:
        resp = send(host, PORT, msg)
        print("[mac-actor] server response:")
        print(resp)
    except Exception as e:
        print("[mac-actor] FAILED:", e)
        sys.exit(1)
