#!/usr/bin/env python3
"""The editor's chat with Claude, end to end, against a stand-in for Claude's service.

    python3 tools/chat_smoke.py <path to giant_editor> --hidden [more editor options...]

(--hidden is needed: with a window the editor asks the person before
Claude's changes, and no one is there to answer.)

Starts a small local server that answers as Claude's service would (no key,
no network), and the editor with --mcp pointed at it. Then: the chat is empty
and .NET not started; a message without a key is refused with how to give
one; a key file written while the editor runs is picked up; Claude's tool
calls find and run a command in the editor (on the undo history), their
results go back to the service, and the conversation reads as it happened.
Then a second editor with --chat-asks: a command of Claude's that changes
something waits for the person (get_chat's `asking`), nothing is changed
while it does, and stopping or clearing withdraws the question.
Fails (exit 1) on any wrong answer, or if the editor does not exit cleanly.
"""
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

TIMEOUT = 600  # seconds for the whole run
FILE_KEY = "key-from-the-settings-file"
NEW_KEY = "a-corrected-key"


def message(stop_reason, *blocks):
    return {"id": "msg_1", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
            "content": list(blocks), "stop_reason": stop_reason, "stop_sequence": None,
            "usage": {"input_tokens": 100, "output_tokens": 20}}


def text(words):
    return {"type": "text", "text": words}


def use(call_id, name, tool_input):
    return {"type": "tool_use", "id": call_id, "name": name, "input": tool_input, "caller": {"type": "direct"}}


class Service:
    """Answers each request with the next reply set up, and keeps what was asked."""

    def __init__(self):
        self.replies = []
        self.requests = []  # (path, headers, body)
        self.gate = None  # when set (a threading.Event), a request is answered only once it is
        self.lock = threading.Lock()
        service = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802 (http.server's name)
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")
                with service.lock:
                    service.requests.append((self.path, {k.lower(): v for k, v in self.headers.items()}, body))
                    reply = service.replies.pop(0) if service.replies else None
                    gate = service.gate
                if gate is not None:
                    gate.wait(TIMEOUT)
                if reply is None:
                    status, out = 500, {"type": "error", "error": {"type": "api_error", "message": "no reply was set up"}}
                else:
                    status, out = 200, reply
                data = json.dumps(out).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *_):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}"


def session(options, scenario, key=False) -> bool:
    """Runs the editor (with `options` added) against a stand-in service, and `scenario` in it. True when all went well.
    `key`: the key file is there from the start."""
    service = Service()
    settings = tempfile.mkdtemp(prefix="giant_chat_")
    # The editor's environment: the stand-in service, no key yet, and the
    # person's settings folder an empty one (so a real key file is never read).
    env = {k: v for k, v in os.environ.items()
           if k.upper() not in ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY")}
    env.update({"ANTHROPIC_BASE_URL": service.url, "NO_PROXY": "127.0.0.1,localhost", "no_proxy": "127.0.0.1,localhost",
                "APPDATA": settings, "XDG_CONFIG_HOME": settings})
    if key:
        write_key(settings, FILE_KEY)
    command = [sys.argv[1], "--mcp", *sys.argv[2:], *options]
    editor = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=None, text=True, encoding="utf-8", bufsize=1, env=env)
    timer = threading.Timer(TIMEOUT, editor.kill)
    timer.start()
    next_id = 0

    def call(method, params=None):
        nonlocal next_id
        next_id += 1
        request = {"jsonrpc": "2.0", "id": next_id, "method": method}
        if params is not None:
            request["params"] = params
        editor.stdin.write(json.dumps(request) + "\n")
        editor.stdin.flush()
        line = editor.stdout.readline()
        if not line:
            raise RuntimeError(f"editor closed the connection during {method}")
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError(f"{method} failed: {reply['error']}")
        return reply["result"]

    def raw_tool(name, arguments=None):
        return call("tools/call", {"name": name, "arguments": arguments or {}})

    def tool(name, arguments=None):
        result = raw_tool(name, arguments)
        if result.get("isError"):
            raise RuntimeError(f"tool {name} failed: {result['content'][0]['text']}")
        return result.get("structuredContent", {})

    def check(condition, what):
        if not condition:
            raise RuntimeError("check failed: " + what)

    def finish():
        """Waits until Claude is done with the message, and returns the whole chat."""
        until = time.monotonic() + 120
        while time.monotonic() < until:
            chat = tool("get_chat", {"limit": 256})
            if not chat["busy"]:
                return chat
            time.sleep(0.02)
        raise RuntimeError("the chat stayed busy")

    try:
        call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                            "clientInfo": {"name": "chat_smoke", "version": "1"}})
        editor.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        editor.stdin.flush()
        scenario(tool, raw_tool, check, finish, service, settings)
        tool("quit_editor")
        code = editor.wait(timeout=TIMEOUT)
        check(code == 0, f"editor exit code {code}")
    except Exception as error:  # report and fail
        print(f"chat_smoke: {error}", file=sys.stderr)
        editor.kill()
        editor.wait()
        return False
    finally:
        timer.cancel()
        service.server.shutdown()
        shutil.rmtree(settings, ignore_errors=True)
    return True


def write_key(settings, key):
    os.makedirs(os.path.join(settings, "Giant"), exist_ok=True)
    with open(os.path.join(settings, "Giant", "claude_key.txt"), "w", encoding="utf-8") as file:
        file.write(key + "\n")


def everyday(tool, raw_tool, check, finish, service, settings):
    """The chat as it is used: no key, a key, Claude's tools, clearing, a corrected key, the service failing."""
    # Before any message: empty, and nothing was started for it.
    chat = tool("get_chat")
    check(chat["entries"] == [] and not chat["busy"] and chat["started"] is False, f"an unstarted chat is empty: {chat}")
    check(tool("stop_chat") == {"stopped": False}, "nothing to stop")
    tool("clear_chat")
    check(tool("get_chat")["started"] is False, "reading, stopping and clearing start nothing")

    # No key: refused, with how to give one; nothing is sent.
    refused = raw_tool("chat_send", {"text": "Make the background blue."})
    check(refused.get("isError") and "ANTHROPIC_API_KEY" in refused["content"][0]["text"]
          and "claude_key.txt" in refused["content"][0]["text"], f"without a key the person is told how to give one: {refused}")
    chat = tool("get_chat")
    check(chat["started"] is True and chat["key_found"] is False and chat["entries"] == [] and chat["model"] == "claude-opus-5-5",
          f"the chat started, without a key: {chat}")
    check(len(service.requests) == 0, "and nothing was sent")

    # A key file written while the editor runs is picked up.
    write_key(settings, FILE_KEY)
    until = time.monotonic() + 30
    while not tool("get_chat")["key_found"] and time.monotonic() < until:
        time.sleep(0.05)  # (reading looks for a key once a second; sending always looks)
    check(tool("get_chat")["key_found"] is True, "the key file is found")

    before = tool("get_clear_color")
    history = len(tool("get_history")["undo"])
    service.replies = [
        message("tool_use", text("I'll find the command for that."),
                use("toolu_1", "find_commands", {"words": "clear color background"}),
                use("toolu_2", "describe_command", {"name": "set_clear_color"})),
        message("tool_use",
                use("toolu_3", "run_command", {"name": "set_clear_color", "args": {"r": 0.0, "g": 0.25, "b": 1.0}}),
                use("toolu_4", "run_command", {"name": "set_clear_color", "args": {"r": 7}}),
                use("toolu_5", "run_command", {"name": "quit_editor"})),
        message("end_turn", text("The background is blue now.")),
    ]
    service.gate = threading.Event()  # (the service holds its first answer: Claude is working for as long)
    sent = tool("chat_send", {"text": "Make the background blue."})
    check(sent["started"] is True and sent["after"] == 1, f"the message went out, as line 1: {sent}")
    busy = raw_tool("chat_send", {"text": "And green."})
    check(busy.get("isError") and "still working" in busy["content"][0]["text"], f"one message at a time: {busy}")
    check(tool("get_chat")["busy"] is True, "and Claude is said to be working")
    service.gate.set()
    service.gate = None
    chat = finish()

    kinds = [e["kind"] for e in chat["entries"]]
    check(kinds == ["you", "claude", "tool", "result", "tool", "result", "tool", "result", "tool", "result", "tool",
                    "result", "claude"], f"the conversation as it happened: {kinds}")
    entries = chat["entries"]
    check(entries[0]["text"] == "Make the background blue." and entries[-1]["text"] == "The background is blue now.",
          "the person's words and Claude's")
    check(entries[2]["tool"] == "find_commands" and entries[3]["ok"] and "set_clear_color" in entries[3]["text"],
          f"the search found the command: {entries[3]}")
    check(entries[5]["ok"] and "arguments" in entries[5]["text"], "its description came back")
    check(entries[7]["ok"] and not entries[9]["ok"] and not entries[11]["ok"],
          "the good command ran; the bad arguments and closing the editor were refused")
    check([e["serial"] for e in entries] == list(range(1, 14)) and chat["after"] == 13 and chat["latest"] == 13,
          "the lines are numbered in order")
    check(chat["input_tokens"] == 300 and chat["output_tokens"] == 60, "the tokens are counted")

    # What Claude did happened in the editor, as the person's own work would.
    check(tool("get_clear_color") == {"r": 0.0, "g": 0.25, "b": 1.0}, "the command ran in the editor")
    undo = tool("get_history")["undo"]
    check(len(undo) == history + 1 and undo[-1]["command"] == "set_clear_color", "once, on the undo history")
    tool("undo")
    check(tool("get_clear_color") == before, "and the person can undo it")

    # What the service was sent.
    check(len(service.requests) == 3, f"three requests: {len(service.requests)}")
    path, headers, first = service.requests[0]
    check("/v1/messages" in path, f"to the messages endpoint: {path}")
    check(FILE_KEY in headers.get("x-api-key", "") + headers.get("authorization", ""), "with the key from the file")
    check("server-side-fallback-2026-07-01" in headers.get("anthropic-beta", "") and first.get("fallbacks") == "default",
          "asking for the service's fallback")
    check(first["model"] == "claude-opus-5-5" and [t["name"] for t in first["tools"]] ==
          ["find_commands", "describe_command", "run_command"], "the model and the three tools")
    check("Giant editor" in json.dumps(first["system"]), "the system text")
    check(first["messages"] == [{"role": "user", "content": "Make the background blue."}] or
          first["messages"][0]["content"][0]["text"] == "Make the background blue.", "the person's message")
    third = service.requests[2][2]["messages"]
    check([m["role"] for m in third] == ["user", "assistant", "user", "assistant", "user"], "the turns, in order")
    results = third[4]["content"]
    check([r["tool_use_id"] for r in results] == ["toolu_3", "toolu_4", "toolu_5"], "every call answered, in one message")
    check(not results[0].get("is_error") and results[1].get("is_error") and results[2].get("is_error"),
          "failures said to be failures")
    found = json.loads(third[2]["content"][0]["content"] if isinstance(third[2]["content"][0]["content"], str)
                       else third[2]["content"][0]["content"][0]["text"])
    check(any(c["name"] == "set_clear_color" for c in found["commands"]), "the search's result went back to Claude")

    # Reading on from a cursor; clearing forgets.
    check([e["serial"] for e in tool("get_chat", {"after": 11})["entries"]] == [12, 13], "reading on from a line")
    tool("clear_chat")
    chat = tool("get_chat")
    check([e["kind"] for e in chat["entries"]] == ["note"] and chat["oldest"] == 14, f"a cleared chat: {chat}")
    service.replies = [message("end_turn", text("Hello."))]
    tool("chat_send", {"text": "Hi again."})
    chat = finish()
    check([e["kind"] for e in chat["entries"]] == ["note", "you", "claude"], "a new conversation")
    check(len(service.requests[-1][2]["messages"]) == 1, "which starts fresh for Claude too")

    # A key corrected while the editor runs is the one used from then on.
    with open(os.path.join(settings, "Giant", "claude_key.txt"), "w", encoding="utf-8") as file:
        file.write("  " + NEW_KEY + "  \n\nnot the key\n")
    service.replies = [message("end_turn", text("Hello again."))]
    tool("chat_send", {"text": "With the new key."})
    finish()
    headers = service.requests[-1][1]
    sent_key = headers.get("x-api-key", "") + headers.get("authorization", "")
    check(NEW_KEY in sent_key and FILE_KEY not in sent_key, "the corrected key is used")

    # The service failing is said in the chat, and the editor carries on.
    service.replies = []
    tool("chat_send", {"text": "Once more."})
    chat = finish()
    check(chat["entries"][-1]["kind"] == "error", f"an error from the service is a line of the chat: {chat['entries'][-1]}")
    lines = tool("get_log", {"count": 200})["lines"]
    check(not any(FILE_KEY in line["text"] or NEW_KEY in line["text"] for line in lines), "the key is never logged")
    everything = json.dumps(tool("get_chat", {"limit": 256}))
    check(FILE_KEY not in everything and NEW_KEY not in everything, "nor in the chat")


def asking(tool, raw_tool, check, finish, service, settings):
    """With --chat-asks: a command of Claude's that changes something waits for the person. No one can answer
    here (there is no window, and on purpose no command that answers), so it waits until the work is stopped."""
    def held(command):
        until = time.monotonic() + 120
        while time.monotonic() < until:
            chat = tool("get_chat", {"limit": 256})
            if chat["asking"] is not None:
                check(chat["asking"]["command"] == command and chat["busy"] is True, f"the person is asked about {command}: {chat['asking']}")
                return chat
            check(chat["busy"], f"the chat ended without asking: {chat['entries']}")
            time.sleep(0.02)
        raise RuntimeError("the person was never asked")

    check(tool("get_chat")["asking"] is None, "nothing is asked before anything is sent")
    before = tool("get_clear_color")
    history = len(tool("get_history")["undo"])
    service.replies = [
        message("tool_use",
                use("toolu_1", "find_commands", {"words": "clear color"}),
                use("toolu_2", "run_command", {"name": "get_clear_color"}),
                use("toolu_3", "run_command", {"name": "set_clear_color", "args": {"r": 7}}),
                use("toolu_4", "run_command", {"name": "set_clear_color", "args": {"r": 0.0, "g": 0.25, "b": 1.0}}),
                use("toolu_5", "run_command", {"name": "get_clear_color"})),
    ]
    tool("chat_send", {"text": "Make the background blue."})
    chat = held("set_clear_color")
    kinds = [e["kind"] for e in chat["entries"]]
    check(kinds == ["you", "tool", "result", "tool", "result", "tool", "result", "tool"],
          f"looking ran, bad arguments were refused unasked, and the change waits with no result: {kinds}")
    check(chat["entries"][4]["ok"] and not chat["entries"][6]["ok"], "what ran and what was refused")
    check(json.loads(chat["asking"]["args"]) == {"r": 0.0, "g": 0.25, "b": 1.0} and chat["asking"]["args_hidden"] == 0,
          f"with the arguments the person is shown: {chat['asking']}")
    time.sleep(0.5)  # (frames pass: it stays held)
    check(tool("get_clear_color") == before and len(tool("get_history")["undo"]) == history, "nothing was changed while it waits")
    check(tool("get_chat")["asking"] is not None, "and the person is still asked")
    check(tool("stop_chat") == {"stopped": True}, "the work is stopped while the person is asked")
    chat = finish()
    check(chat["asking"] is None and chat["entries"][-1]["text"] == "Stopped.", f"the question is withdrawn: {chat['entries'][-1]}")
    check(tool("get_clear_color") == before and len(tool("get_history")["undo"]) == history, "and the command never ran")
    service.replies = [message("end_turn", text("All right."))]
    tool("chat_send", {"text": "Never mind."})
    chat = finish()
    results = service.requests[-1][2]["messages"][2]["content"]
    check([r["tool_use_id"] for r in results] == ["toolu_1", "toolu_2", "toolu_3", "toolu_4", "toolu_5"] and
          [bool(r.get("is_error")) for r in results] == [False, False, True, True, True], "Claude is told what ran and what did not")

    # Cleared while the person is asked: withdrawn too, and the next conversation is not held up by it.
    service.replies = [message("tool_use", use("toolu_6", "run_command", {"name": "set_clear_color", "args": {"r": 1.0, "g": 0.0, "b": 0.0}}))]
    tool("chat_send", {"text": "Red, then."})
    held("set_clear_color")
    tool("clear_chat")
    check(tool("get_chat")["asking"] is None, "clearing withdraws the question at once")
    finish()
    service.replies = [message("tool_use", use("toolu_7", "run_command", {"name": "get_clear_color"})),
                       message("end_turn", text("Still as it was."))]
    tool("chat_send", {"text": "What colour is it?"})
    chat = finish()
    check([e["kind"] for e in chat["entries"]] == ["note", "you", "tool", "result", "claude"], "the next conversation works")
    check(tool("get_clear_color") == before and len(tool("get_history")["undo"]) == history, "and nothing was ever changed")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if not session([], everyday) or not session(["--chat-asks"], asking, key=True):
        return 1
    print("chat_smoke: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
