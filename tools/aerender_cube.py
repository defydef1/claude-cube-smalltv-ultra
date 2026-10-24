#!/usr/bin/env python3
"""aerender with a progress ring on the Claude Cube.

Same arguments as aerender; output is passed through unchanged. Parses the
PROGRESS lines (Duration, Frame Rate, per-frame "(N)") and posts percent and
ETA to http://<cube>/render about once a second, then done/fail at the end.

    python3 aerender_cube.py -project /abs/file.aep -comp "Main" -output /abs/out.mov

Queue: "add <aerender args>" queues a render, "run" renders the queue in order with
"2 / 5" and what's next on the cube, "list" and "clear" manage it
(~/.claude/aerender_queue.json).

Cube address: $CUBE_HOST, else ~/.claude/cube_host, else claudecube.local.
aerender path: $AERENDER, else the newest /Applications/Adobe After Effects */aerender.
"""
import glob
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request

RE_DURATION = re.compile(r"PROGRESS:\s+Duration:\s+(\d+)[:;](\d+)[:;](\d+)[:;](\d+)")
RE_FPS = re.compile(r"PROGRESS:\s+Frame Rate:\s+([\d.]+)")
RE_FRAME = re.compile(r"PROGRESS:\s+\d+[:;]\d+[:;]\d+[:;]\d+\s+\((\d+)\)")
RE_DONE = re.compile(r"Total Time Elapsed", re.I)


def host():
    h = os.environ.get("CUBE_HOST")
    if h:
        return h
    try:
        with open(os.path.expanduser("~/.claude/cube_host")) as f:
            return f.read().strip() or "claudecube.local"
    except OSError:
        return "claudecube.local"


def aerender_path():
    if os.environ.get("AERENDER"):
        return os.environ["AERENDER"]
    found = sorted(glob.glob("/Applications/Adobe After Effects */aerender"))
    if not found:
        sys.exit("aerender not found; set AERENDER=/abs/path/to/aerender")
    return found[-1]


def post(params):
    url = "http://%s/render?%s" % (host(), urllib.parse.urlencode(params))

    def go():
        try:
            urllib.request.urlopen(url, timeout=2).read()
        except Exception:
            pass

    threading.Thread(target=go, daemon=True).start()


def job_name(args):
    for flag in ("-comp", "-rqindex"):
        if flag in args and args.index(flag) + 1 < len(args):
            v = args[args.index(flag) + 1]
            return v if flag == "-comp" else "#" + v
    if "-project" in args and args.index("-project") + 1 < len(args):
        return os.path.splitext(os.path.basename(args[args.index("-project") + 1]))[0]
    return "aerender"


QUEUE = os.path.expanduser("~/.claude/aerender_queue.json")


def render(args, name, extra=None, final=True):
    """Run one aerender job, mirroring progress to the cube. Returns aerender's exit code."""
    extra = extra or {}
    proc = subprocess.Popen([aerender_path()] + args, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1, errors="replace")
    total, fps, dur = 0, 0.0, None
    last_total, t_begin = 0, time.time()
    start = time.time()
    last_post = 0.0
    finished_ok = False
    post(dict({"p": 0, "name": name}, **extra))
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        m = RE_DURATION.search(line)
        if m:
            dur = [int(x) for x in m.groups()]
        m = RE_FPS.search(line)
        if m:
            fps = float(m.group(1))
        if dur and fps and not total:
            h, mi, s, f = dur
            total = int(round(((h * 60 + mi) * 60 + s) * fps)) + f
            last_total = total
            start = time.time()
        m = RE_FRAME.search(line)
        if m and total:
            done = int(m.group(1))
            frac = min(1.0, done / total)
            now = time.time()
            if now - last_post >= 1.0 or frac >= 1.0:
                elapsed = now - start
                eta = int(elapsed / frac - elapsed) if frac > 0.01 else -1
                post(dict({"p": int(frac * 100), "eta": eta, "name": name, "f": done, "n": total}, **extra))
                last_post = now
        if RE_DONE.search(line):
            finished_ok = True
            total, dur, fps = 0, None, 0.0  # next render-queue item starts over
    rc = proc.wait()
    ok = rc == 0 and finished_ok
    if final or not ok:
        post(dict({"state": "done" if ok else "fail", "name": name, "n": last_total,
                   "el": int(time.time() - t_begin)}, **extra))
        time.sleep(0.5)  # let the last request leave
    return rc if ok or rc else 1


def load_queue():
    try:
        with open(QUEUE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return []


def save_queue(q):
    with open(QUEUE, "w") as f:
        json.dump(q, f, ensure_ascii=False, indent=1)


def run_queue():
    """Render queued jobs one after another; the cube shows 'n / N' and what's next."""
    t0, n_done = time.time(), 0
    while True:
        q = load_queue()  # re-read each time: jobs can be added while it runs
        if not q:
            break
        job = q[0]
        qn = n_done + len(q)
        nxt = ", ".join(j["name"] for j in q[1:3])
        rc = render(job["args"], job["name"], {"q": n_done + 1, "qn": qn, "next": nxt}, final=len(q) == 1)
        q = load_queue()
        if q and q[0] == job:
            q.pop(0)
            save_queue(q)
        if rc:
            print("queue: %s failed (exit %d), stopping; the rest stays queued" % (job["name"], rc), file=sys.stderr)
            return rc
        n_done += 1
    print("queue: %d render(s) in %s" % (n_done, time.strftime("%H:%M:%S", time.gmtime(time.time() - t0))))
    return 0


USAGE = """usage:
  aerender_cube.py <aerender args>          render now
  aerender_cube.py add <aerender args>      put a render in the queue
  aerender_cube.py run                      render the queue in order
  aerender_cube.py list | clear"""


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(USAGE)
    cmd = args[0]
    if cmd == "add":
        q = load_queue()
        q.append({"args": args[1:], "name": job_name(args[1:])})
        save_queue(q)
        print("queued #%d: %s" % (len(q), q[-1]["name"]))
    elif cmd == "list":
        for i, j in enumerate(load_queue(), 1):
            print("%d. %s" % (i, j["name"]))
    elif cmd == "clear":
        save_queue([])
    elif cmd == "run":
        sys.exit(run_queue())
    else:
        sys.exit(render(args, job_name(args)))


if __name__ == "__main__":
    main()
