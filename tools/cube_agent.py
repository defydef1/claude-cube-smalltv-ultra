#!/usr/bin/env python3
"""Claude session faces, Mac presence and the cube's language -> Claude Cube.

Delivers each Claude Code session's state (written by tools/cube_hook.py) to the cube one
by one with retries, tells the cube every 30 s that the Mac is on and how long it has been
idle (the pet shows only while you're away), follows the cube's IP and language.
Run by launchd with KeepAlive.

Cube address: $CUBE_HOST, else ~/.claude/cube_host, else claudecube.local.
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request



def idle_seconds():
    """Seconds since the last keyboard or mouse input (the cube's pet uses it to tell he's away)."""
    out = subprocess.run(["ioreg", "-c", "IOHIDSystem"], capture_output=True, text=True, timeout=10).stdout
    m = re.search(r'"HIDIdleTime" = (\d+)', out)
    return int(m.group(1)) // 1_000_000_000 if m else 0


HOST_FILE = os.path.expanduser("~/.claude/cube_host")


def refresh_lang():
    """Keep ~/.claude/cube_lang on the cube's language, so the hooks label in it too."""
    try:
        d = json.loads(urllib.request.urlopen("http://%s/status" % host(), timeout=8).read())
        lang = d["cfg"]["lang"]
    except Exception:
        return
    path = os.path.expanduser("~/.claude/cube_lang")
    try:
        with open(path) as f:
            if f.read().strip() == lang:
                return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(lang + "\n")


def refresh_host():
    """Keep ~/.claude/cube_host on the cube's current IP: the router hands out a new one after a
    restart. IPv4-only lookup of claudecube.local is instant (the default waits 5 s for IPv6)."""
    import socket
    try:
        ip = socket.getaddrinfo("claudecube.local", 80, socket.AF_INET)[0][4][0]
    except OSError:
        return
    if ip != host():
        with open(HOST_FILE, "w") as f:
            f.write(ip + "\n")
        print("cube moved to %s" % ip, file=sys.stderr)


STATE_DIR = os.path.expanduser("~/.claude/cube_state")
sent = {}  # session id -> state timestamp already on the cube


def deliver_faces():
    """Send the cube each session's state that changed since last time, one request at a time.
    A failed send stays pending and goes again next round, so no emotion gets lost."""
    try:
        names = os.listdir(STATE_DIR)
    except OSError:
        return
    for name in names:
        if not name.endswith(".json"):
            continue
        sid, path = name[:-5], os.path.join(STATE_DIR, name)
        try:
            with open(path) as f:
                st = json.load(f)
        except (OSError, ValueError):
            continue
        if time.time() - st["ts"] > 86400:  # a session gone for a day: forget it
            os.remove(path)
            continue
        if sent.get(sid) == st["ts"]:
            continue
        q = urllib.parse.urlencode({"e": st["e"], "sid": sid, "t": st["t"]})
        try:
            urllib.request.urlopen("http://%s/face?%s" % (host(), q), timeout=5).read()
        except Exception as e:
            print("face: %s" % e, file=sys.stderr)
            return  # cube busy: try the rest next round
        sent[sid] = st["ts"]
        if st["e"] == "end":
            os.remove(path)


def main():
    last, sent_at, looked_at, badge_at = None, 0, 0, 0
    while True:
        if time.time() - looked_at > 60:
            refresh_host()
            refresh_lang()
            looked_at = time.time()
        deliver_faces()
        if time.time() - badge_at < 4:
            time.sleep(1)
            continue
        badge_at = time.time()
        try:
            if time.time() - sent_at > 30:  # the Mac's heartbeat
                urllib.request.urlopen("http://%s/presence?idle=%d" % (host(), idle_seconds()), timeout=8).read()
                sent_at = time.time()
        except Exception as e:  # cube busy: try again shortly
            print("presence: %s" % e, file=sys.stderr)
        time.sleep(1)


if __name__ == "__main__":
    main()
