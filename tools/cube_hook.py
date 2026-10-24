#!/usr/bin/env python3
"""Claude Code hook -> Claude Cube face.

Reads the hook payload on stdin and records the session's state in
~/.claude/cube_state/<session>.json; tools/cube_teams.py delivers it to the cube.
Never blocks Claude. Prints nothing: stdout of some hooks (UserPromptSubmit) is fed
back to the model.

Cube address: $CUBE_HOST, else ~/.claude/cube_host, else claudecube.local.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strings import HOOK, LANGS  # noqa: E402

LANG_FILE = os.path.expanduser("~/.claude/cube_lang")  # the cube's language, kept by cube_teams.py


def word(key):
    try:
        with open(LANG_FILE) as f:
            lang = f.read().strip()
    except OSError:
        lang = ""
    col = LANGS.index(lang) if lang in LANGS else 0
    return HOOK[key][col]


ASKS_USER = {"AskUserQuestion", "ExitPlanMode"}
STATE_DIR = os.path.expanduser("~/.claude/cube_state")


def host():
    h = os.environ.get("CUBE_HOST")
    if h:
        return h
    try:
        with open(os.path.expanduser("~/.claude/cube_host")) as f:
            return f.read().strip() or "claudecube.local"
    except OSError:
        return "claudecube.local"


def tool_word(name):
    if name in HOOK:
        return word(name)
    if name.startswith("mcp__"):
        return word("_tools")
    return name


def decide(p):
    ev = p.get("hook_event_name", "")
    project = os.path.basename((p.get("cwd") or "").rstrip("/")) or "claude"
    tool = p.get("tool_name", "")
    if ev == "UserPromptSubmit":
        return "thinking", project
    if ev == "PreToolUse":
        if tool in ASKS_USER:
            return "attention", project + word("_question")
        return "working", project + " · " + tool_word(tool)
    if ev == "Notification":
        kind = p.get("notification_type", "")
        msg = (p.get("message") or "").lower()
        if kind == "idle_prompt" or "waiting for your input" in msg:
            return None
        return "attention", project + word("_permission")
    if ev == "Stop":
        return "happy", project
    if ev == "StopFailure":
        return "sad", project
    if ev == "SessionEnd":
        return "end", ""
    return None


def main():
    try:
        p = json.load(sys.stdin)
    except Exception:
        return
    d = decide(p)
    if not d:
        return
    emo, label = d
    sid = p.get("session_id", "default")
    # Don't call the cube from here: with many sessions the hooks arrive in bursts, and any that
    # land while the cube is fetching something are lost. Record the session's latest state; the
    # cube job on the Mac (cube_teams.py) delivers changes one at a time and retries.
    os.makedirs(STATE_DIR, exist_ok=True)
    tmp = os.path.join(STATE_DIR, ".%s.tmp" % sid)
    with open(tmp, "w") as f:
        json.dump({"e": emo, "t": label, "ts": time.time()}, f, ensure_ascii=False)
    os.replace(tmp, os.path.join(STATE_DIR, sid + ".json"))

if __name__ == "__main__":
    main()
