#!/usr/bin/env python3
"""End-to-end test for the Giant MCP server.

Starts a headless editor on a throwaway copy of the test project, then drives it
over MCP the way Claude Code would: create a scene, add and edit nodes, undo,
write and attach scripts, and save.

Usage: python3 giant/tests/mcp_e2e.py <path to editor binary>
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TOKEN = "giant-e2e-token"
MODERN = "2026-07-28"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Client:
    def __init__(self, port):
        self.url = f"http://127.0.0.1:{port}/mcp"
        self.next_id = 1

    def post(self, body, headers=None, token=TOKEN):
        data = json.dumps(body).encode()
        req = urllib.request.Request(self.url, data=data, method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Accept", "application/json, text/event-stream")
        if token:
            req.add_header("Authorization", f"Bearer {token}")
        for key, value in (headers or {}).items():
            req.add_header(key, value)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                text = resp.read().decode()
                return resp.status, json.loads(text) if text else None
        except urllib.error.HTTPError as e:
            text = e.read().decode()
            return e.code, json.loads(text) if text else None

    def request(self, method, params=None, name=None):
        """A modern (2026-07-28) request with the required mirror headers."""
        params = dict(params or {})
        params["_meta"] = {"io.modelcontextprotocol/protocolVersion": MODERN}
        body = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}
        self.next_id += 1
        headers = {"MCP-Protocol-Version": MODERN, "Mcp-Method": method}
        if name:
            headers["Mcp-Name"] = name
        return self.post(body, headers)

    def tool(self, tool_name, /, **arguments):
        status, body = self.request("tools/call", {"name": tool_name, "arguments": arguments}, name=tool_name)
        assert status == 200, (tool_name, status, body)
        assert "result" in body, (tool_name, body)
        return body["result"]


failures = []


def check(label, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + label + ("" if condition else f"  {detail}"))
    if not condition:
        failures.append(label)


def text_of(result):
    return "".join(c.get("text", "") for c in result.get("content", []))


def run(client):
    # Security.
    status, _ = client.post({"jsonrpc": "2.0", "id": 0, "method": "server/discover"}, token=None)
    check("rejects requests without the token", status == 401, status)
    status, _ = client.post({"jsonrpc": "2.0", "id": 0, "method": "server/discover"}, {"Origin": "https://evil.example"})
    check("rejects requests from web pages", status == 403, status)

    # Protocol.
    status, body = client.post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "e2e", "version": "1"}}})
    check("older clients can initialize", status == 200 and body["result"]["protocolVersion"] == "2025-06-18", body)
    status, _ = client.post({"jsonrpc": "2.0", "method": "notifications/initialized"})
    check("notifications get 202", status == 202, status)
    status, body = client.request("server/discover")
    check("server/discover lists the latest version", status == 200 and MODERN in body["result"]["supportedVersions"], body)
    status, body = client.post({"jsonrpc": "2.0", "id": 9, "method": "tools/list", "params": {}}, {"MCP-Protocol-Version": MODERN})
    check("modern requests need Mcp-Method", status == 400 and body["error"]["code"] == -32020, body)
    status, body = client.request("tools/list")
    tools = [t["name"] for t in body["result"]["tools"]]
    check("lists the editor tools", status == 200 and len(tools) >= 20, tools)

    # Scenes and nodes.
    result = client.tool("new_scene", path="res://e2e/main.tscn", root_type="Node3D", overwrite=True)
    check("creates a scene", not result.get("isError"), text_of(result))
    result = client.tool("create_node", type="MeshInstance3D", name="Cube", properties={"position": [0, 1, 0], "mesh": {"_class": "BoxMesh", "size": [2, 2, 2]}})
    check("creates a node with a new mesh", not result.get("isError"), text_of(result))
    node = client.tool("get_node", node_path="Cube")["structuredContent"]
    check("reads the node back", node["type"] == "MeshInstance3D" and node["properties"]["mesh"]["_class"] == "BoxMesh", node)
    check("reads vectors as Godot literals", node["properties"]["position"] == "Vector3(0, 1, 0)", node["properties"].get("position"))

    client.tool("set_properties", node_path="Cube", properties={"position": "Vector3(3, 0, 0)"})
    pos = client.tool("get_node", node_path="Cube", filter="position")["structuredContent"]["properties"]["position"]
    check("sets a property", pos == "Vector3(3, 0, 0)", pos)
    result = client.tool("undo")
    pos = client.tool("get_node", node_path="Cube", filter="position")["structuredContent"]["properties"]["position"]
    check("undo reverts the AI's change", pos == "Vector3(0, 1, 0)", (text_of(result), pos))

    result = client.tool("set_properties", node_path="Cube", properties={"not_a_property": 1})
    check("unknown properties are refused", result.get("isError") is True, text_of(result))

    # Scripts.
    good = 'extends MeshInstance3D\n\nfunc _process(delta: float) -> void:\n\trotate_y(delta)\n'
    result = client.tool("write_file", path="res://e2e/spin.gd", content=good)["structuredContent"]
    check("writes and compiles a script", result.get("compiled") is True and not result.get("errors"), result)
    bad = 'extends MeshInstance3D\n\nfunc _process(delta: float) -> void:\n\trotate_y(delta\n'
    result = client.tool("write_file", path="res://e2e/broken.gd", content=bad)["structuredContent"]
    check("reports script errors", result.get("compiled") is False or len(result.get("errors", [])) > 0, result)
    result = client.tool("attach_script", node_path="Cube", script_path="res://e2e/spin.gd")
    check("attaches a script", not result.get("isError"), text_of(result))

    # Delete and restore.
    client.tool("delete_node", node_path="Cube")
    tree = client.tool("get_scene_tree")["structuredContent"]["tree"]
    check("deletes a node", "children" not in tree, tree)
    client.tool("undo")
    tree = client.tool("get_scene_tree")["structuredContent"]["tree"]
    check("undo restores a deleted node", any(c["name"] == "Cube" for c in tree.get("children", [])), tree)

    # Saving.
    result = client.tool("save_scene")
    check("saves the scene", not result.get("isError"), text_of(result))
    scene_text = text_of(client.tool("read_file", path="res://e2e/main.tscn"))
    check("saved scene has the cube and its script", "Cube" in scene_text and "spin.gd" in scene_text, scene_text[:400])

    # Reflection.
    info = client.tool("describe_class", class_name="GPUParticles3D")["structuredContent"]
    check("describes engine classes", any(p["name"] == "amount" for p in info["properties"]), info.get("properties", [])[:5])

    # Screenshots need a GPU; headless runs only check that the tool answers.
    result = client.tool("screenshot")
    check("screenshot answers", "content" in result, result)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    editor = os.path.abspath(sys.argv[1])
    project = tempfile.mkdtemp(prefix="giant_mcp_e2e_")
    shutil.copytree(os.path.join(HERE, "mcp_project"), project, dirs_exist_ok=True)
    port = free_port()
    env = dict(os.environ, GIANT_MCP_PORT=str(port), GIANT_MCP_TOKEN=TOKEN)
    log_path = os.path.join(project, "editor.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([editor, "--headless", "--editor", "--path", project], env=env, stdout=log, stderr=subprocess.STDOUT)
    client = Client(port)
    try:
        deadline = time.time() + 180
        while True:
            if proc.poll() is not None:
                raise RuntimeError("The editor exited early. See " + log_path)
            try:
                status, _ = client.request("server/discover")
                if status == 200:
                    break
            except (urllib.error.URLError, ConnectionError, OSError):
                pass
            if time.time() > deadline:
                raise RuntimeError("The MCP server did not start. See " + log_path)
            time.sleep(1)
        run(client)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    print(f"\n{len(failures)} failed" if failures else "\nAll checks passed")
    if failures:
        print("Editor log: " + log_path)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
