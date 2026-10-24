#!/bin/sh
# Connect Claude Code on this Mac to the cube:
#   tools/install_mac.sh [cube host]     (default claudecube.local)
# - writes ~/.claude/cube_host
# - adds the cube hooks to ~/.claude/settings.json (backup: settings.json.bak-cube)
# - installs the background agent (launchd com.claudecube.agent) and, if built, CubeSenses.app
set -e
TOOLS=$(cd "$(dirname "$0")" && pwd)
HOST=${1:-claudecube.local}
mkdir -p ~/.claude
echo "$HOST" > ~/.claude/cube_host

python3 - "$TOOLS" <<'PY'
import json, os, shutil, sys
tools = sys.argv[1]
path = os.path.expanduser("~/.claude/settings.json")
cfg = json.load(open(path)) if os.path.exists(path) else {}
if os.path.exists(path):
    shutil.copy(path, path + ".bak-cube")
add = json.loads(open(os.path.join(tools, "hooks.json")).read().replace("CUBE_TOOLS", tools))
hooks = cfg.setdefault("hooks", {})
for event, entries in add.items():
    have = hooks.setdefault(event, [])
    if not any("cube_hook.py" in h.get("command", "") for e in have for h in e.get("hooks", [])):
        have.extend(entries)
json.dump(cfg, open(path, "w"), indent=2)
print("hooks added to", path)
PY

agent() {  # label, program args...
  label=$1; shift
  plist=~/Library/LaunchAgents/$label.plist
  {
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
    echo '<plist version="1.0"><dict>'
    echo "  <key>Label</key><string>$label</string>"
    echo '  <key>ProgramArguments</key><array>'
    for a in "$@"; do echo "    <string>$a</string>"; done
    echo '  </array>'
    echo '  <key>KeepAlive</key><true/><key>RunAtLoad</key><true/><key>ThrottleInterval</key><integer>30</integer>'
    echo "  <key>StandardErrorPath</key><string>/tmp/$label.log</string>"
    echo '</dict></plist>'
  } > "$plist"
  launchctl bootout gui/$(id -u)/$label 2>/dev/null || true
  launchctl bootstrap gui/$(id -u) "$plist"
  echo "started $label"
}
mkdir -p ~/Library/LaunchAgents
agent com.claudecube.agent /usr/bin/python3 -I "$TOOLS/cube_agent.py"
[ -d "$TOOLS/CubeSenses.app" ] && agent com.claudecube.senses /usr/bin/open -W "$TOOLS/CubeSenses.app"
echo "done: restart Claude Code sessions so they pick up the hooks"
