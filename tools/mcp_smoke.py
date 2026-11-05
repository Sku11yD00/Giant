#!/usr/bin/env python3
"""Drives the editor over MCP the way Claude Code does, and checks the answers.

    python3 tools/mcp_smoke.py <path to giant_editor> [editor options...]

Starts the editor with --mcp, then: initialize, tools/list, change the
background colour, undo it, read the log, and quit. Then starts it again and
closes the connection, which must close the editor too, and once more as a
client that crashes with requests in flight. Fails (exit 1) on any
wrong answer, or if the editor does not exit cleanly.
"""
import json
import subprocess
import sys
import threading

TIMEOUT = 60  # seconds for the whole run


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    command = [sys.argv[1], "--mcp", *sys.argv[2:]]
    editor = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=None, text=True, encoding="utf-8", bufsize=1)
    timer = threading.Timer(TIMEOUT, editor.kill)
    timer.start()
    next_id = 0

    def call(method, params=None):
        nonlocal next_id
        next_id += 1
        message = {"jsonrpc": "2.0", "id": next_id, "method": method}
        if params is not None:
            message["params"] = params
        editor.stdin.write(json.dumps(message) + "\n")
        editor.stdin.flush()
        line = editor.stdout.readline()
        if not line:
            raise RuntimeError(f"editor closed the connection during {method}")
        reply = json.loads(line)
        if reply.get("id") != next_id:
            raise RuntimeError(f"reply id {reply.get('id')} does not match {next_id}")
        if "error" in reply:
            raise RuntimeError(f"{method} failed: {reply['error']}")
        return reply["result"]

    def tool(name, arguments=None):
        result = call("tools/call", {"name": name, "arguments": arguments or {}})
        if result.get("isError"):
            raise RuntimeError(f"tool {name} failed: {result['content'][0]['text']}")
        return result.get("structuredContent", {})

    def check(condition, what):
        if not condition:
            raise RuntimeError("check failed: " + what)

    try:
        init = call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                   "clientInfo": {"name": "mcp_smoke", "version": "1"}})
        check(init["serverInfo"]["name"] == "giant-editor", "server name")
        editor.stdin.write(json.dumps({"jsonrpc": "2.0",
                                       "method": "notifications/initialized"}) + "\n")
        editor.stdin.flush()

        names = {t["name"] for t in call("tools/list")["tools"]}
        for expected in ("get_engine_info", "set_clear_color", "undo", "get_log", "quit_editor"):
            check(expected in names, f"tool {expected} is listed")

        info = tool("get_engine_info")
        check(info["frames_presented"] >= 0, "engine info")

        before = tool("get_clear_color")
        tool("set_clear_color", {"r": 0.5, "g": 0.25, "b": 1.0})
        check(tool("get_clear_color") == {"r": 0.5, "g": 0.25, "b": 1.0}, "colour changed")
        tool("undo")
        check(tool("get_clear_color") == before, "undo restored the colour")
        history = tool("get_history")
        check(len(history["redo"]) == 1, "redo history has the undone change")

        bad = call("tools/call", {"name": "set_clear_color", "arguments": {"r": 2}})
        check(bad.get("isError") is True, "bad arguments are a tool error")

        log = tool("get_log", {"count": 200})
        check(any("MCP server" in line["text"] for line in log["lines"]), "log readable")

        tool("quit_editor")
        code = editor.wait(timeout=TIMEOUT)
        check(code == 0, f"editor exit code {code}")
    except Exception as error:  # report and fail
        print(f"mcp_smoke: {error}", file=sys.stderr)
        editor.kill()
        editor.wait()
        return 1
    finally:
        timer.cancel()

    # A client that just goes away must not leave the editor running.
    editor = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                              stderr=None)
    editor.stdin.close()
    try:
        code = editor.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        editor.kill()
        print("mcp_smoke: editor kept running after the client disconnected", file=sys.stderr)
        return 1
    if code != 0:
        print(f"mcp_smoke: editor exit code {code} after disconnect", file=sys.stderr)
        return 1

    # A client that crashes with requests in flight (both pipes close while
    # the editor still has replies to write) must not kill the editor.
    editor = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=None)
    ping = (json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"}) + "\n").encode()
    editor.stdout.close()
    try:
        editor.stdin.write(ping * 200)
        editor.stdin.close()
    except (BrokenPipeError, OSError):
        pass  # the editor may already have closed; its exit code is checked below
    try:
        code = editor.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        editor.kill()
        print("mcp_smoke: editor kept running after the client crashed", file=sys.stderr)
        return 1
    if code != 0:
        print(f"mcp_smoke: editor exit code {code} after the client crashed", file=sys.stderr)
        return 1
    print("mcp_smoke: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
