#!/usr/bin/env bash
# Shared Ghidra MCP server (LaurieWired/GhidraMCP 2.0, headless) for all agents.
#
# One JVM holds the iOS kernelcache analysis in a persistent Ghidra project;
# Codex (.codex/config.toml) and Claude (.mcp.json) connect over HTTP, so
# several agents can use it at once without fighting over the project lock.
# The first start imports + analyzes the whole kernelcache (slow, once);
# later starts reopen the saved analysis.
#
# Usage: scripts/ghidra-mcp.sh start|save|stop|status|log
#   BINARY=... to serve a different binary (default: iPad6,11 20H350 kernelcache)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${BINARY:-$ROOT/build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12}"
PROJECT="$ROOT/build/ghidra"
PORT="${GHIDRA_MCP_PORT:-8765}"
PIDFILE="$PROJECT/server.pid"
LOG="$PROJECT/server.log"
export GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-/opt/homebrew/opt/ghidra/libexec}"

running() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

case "${1:-}" in
start)
	if running; then echo "already running (pid $(cat "$PIDFILE")) on :$PORT"; exit 0; fi
	mkdir -p "$PROJECT"
	nohup ghidra-mcp --headless --transport streamable-http \
		--mcp-host 127.0.0.1 --mcp-port "$PORT" \
		--binary "$BINARY" --project "$PROJECT" --save \
		--ready-timeout 7200 >"$LOG" 2>&1 &
	echo $! >"$PIDFILE"
	echo "started pid $! -> http://127.0.0.1:$PORT/mcp (log: $LOG)"
	;;
save)
	# Persist the live analysis (and agents' renames/comments) to the project
	# without stopping the server. Signals don't reach ghidra-mcp's own save
	# path (the embedded JVM swallows SIGINT), so we attach lldb and run
	# tools/ghidra/save.py inside the process on its main thread.
	running || { echo "not running"; exit 1; }
	rm -f /tmp/ghidra-save.log
	lldb --batch -p "$(cat "$PIDFILE")" -o 'thread select 1' \
		-o "expr -l c -i false -t 0 -- int \$g = (int)PyGILState_Ensure(); int \$r = (int)PyRun_SimpleString(\"exec(open(\\\"$ROOT/tools/ghidra/save.py\\\").read())\"); (void)PyGILState_Release(\$g); \$r" \
		>/dev/null 2>&1
	cat /tmp/ghidra-save.log
	;;
stop)
	# Save first (see above), then terminate.
	if running; then
		"$0" save || true
		kill "$(cat "$PIDFILE")"
		echo stopped
	else
		echo "not running"
	fi
	rm -f "$PIDFILE"
	;;
status)
	if running; then
		echo "running pid $(cat "$PIDFILE") on :$PORT"
		# Ready only once "Serving <binary>" appears after the last "Analyzing".
		grep -E 'Analyzing|Serving [^M]' "$LOG" | tail -1
	else echo "not running"; fi
	;;
log) tail -f "$LOG" ;;
*) sed -n '2,12p' "$0"; exit 1 ;;
esac
