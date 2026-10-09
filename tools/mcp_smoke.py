#!/usr/bin/env python3
"""
Smoke test client for MMDX12 Model Context Protocol (MCP).
Uses Python standard library only.
"""

import argparse
import base64
import json
import os
import subprocess
import sys
import time

EXPECTED_TOOLS = [
    "list_instances",
    "connect",
    "launch_app",
    "get_state",
    "get_recent_logs",
    "list_library",
    "screenshot",
    "wait_frames",
    "set_screen",
    "load_scene",
    "open_studio",
    "play",
    "pause",
    "seek",
    "get_render_settings",
    "set_render_settings",
    "studio_get_state",
    "studio_command",
    "studio_save",
    "studio_open",
    "studio_add_model",
    "studio_select",
    "studio_set_camera",
    "studio_set_bone",
    "studio_key",
    "undo",
    "redo",
    "render_still",
    "render_video",
    "render_status",
    "render_cancel",
    "ui_input",
    "quit_app"
]

class McpClient:
    def __init__(self, bridge_path, extra_args=None):
        cmd = [bridge_path]
        if extra_args:
            cmd.extend(extra_args)
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            encoding="utf-8"
        )
        self.seq = 1

    def send_request(self, method, params=None, timeout=30):
        req_id = self.seq
        self.seq += 1
        msg = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method
        }
        if params is not None:
            msg["params"] = params

        line = json.dumps(msg) + "\n"
        self.proc.stdin.write(line)
        self.proc.stdin.flush()

        start_time = time.time()
        while True:
            if time.time() - start_time > timeout:
                raise TimeoutError(f"Request {method} (id={req_id}) timed out after {timeout}s")
            resp_line = self.proc.stdout.readline()
            if not resp_line:
                err = self.proc.stderr.read()
                raise RuntimeError(f"Bridge process terminated unexpectedly. Stderr: {err}")
            resp_line = resp_line.strip()
            if not resp_line:
                continue
            resp = json.loads(resp_line)
            if resp.get("id") == req_id:
                return resp

    def send_notification(self, method, params=None):
        msg = {
            "jsonrpc": "2.0",
            "method": method
        }
        if params is not None:
            msg["params"] = params
        line = json.dumps(msg) + "\n"
        self.proc.stdin.write(line)
        self.proc.stdin.flush()

    def call_tool(self, name, arguments=None, timeout=60):
        params = {
            "name": name,
            "arguments": arguments or {}
        }
        resp = self.send_request("tools/call", params, timeout=timeout)
        if "error" in resp:
            raise RuntimeError(f"Tool call {name} JSON-RPC error: {resp['error']}")
        result = resp.get("result", {})
        if result.get("isError"):
            content = result.get("content", [])
            err_text = content[0].get("text", "Unknown error") if content else "Unknown error"
            raise RuntimeError(f"Tool call {name} failed: {err_text}")
        return result

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            pass

def run_handshake_only(bridge_path):
    print(f"[+] Starting bridge: {bridge_path}")
    client = McpClient(bridge_path)
    try:
        print("[+] Sending initialize...")
        init_resp = client.send_request("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp_smoke", "version": "1.0.0"}
        })
        print(f"[+] Initialize response: {init_resp}")
        assert init_resp.get("result", {}).get("protocolVersion") == "2025-06-18"

        print("[+] Sending notifications/initialized...")
        client.send_notification("notifications/initialized")

        print("[+] Sending ping...")
        ping_resp = client.send_request("ping")
        assert "result" in ping_resp

        print("[+] Sending tools/list...")
        tools_resp = client.send_request("tools/list")
        tools = tools_resp.get("result", {}).get("tools", [])
        tool_names = set(t["name"] for t in tools)
        print(f"[+] Found {len(tool_names)} tools")

        missing = [t for t in EXPECTED_TOOLS if t not in tool_names]
        if missing:
            raise RuntimeError(f"Missing expected tools: {missing}")

        # every inputSchema must be a JSON Schema object (clients reject the whole list otherwise)
        def check_schema(schema, path):
            if not isinstance(schema, dict) or "type" not in schema:
                raise RuntimeError(f"Invalid schema at {path}: {schema!r}")
            if schema["type"] == "object":
                props = schema.get("properties", {})
                if not isinstance(props, dict):
                    raise RuntimeError(f"Invalid properties at {path}")
                for r in schema.get("required", []):
                    if r not in props:
                        raise RuntimeError(f"Required '{r}' is not a property at {path}")
                for k, v in props.items():
                    check_schema(v, f"{path}.{k}")
            elif schema["type"] == "array" and "items" in schema:
                check_schema(schema["items"], f"{path}[]")
        for t in tools:
            check_schema(t.get("inputSchema"), t["name"])

        print(f"[+] All {len(EXPECTED_TOOLS)} expected tools present, schemas valid")
        print("[+] Handshake test passed successfully.")
    finally:
        client.close()

def run_scenario(bridge_path):
    print(f"[+] Starting bridge: {bridge_path}")
    client = McpClient(bridge_path)
    try:
        print("[+] Handshake...")
        client.send_request("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp_smoke", "version": "1.0.0"}
        })
        client.send_notification("notifications/initialized")

        # Check instances or launch
        print("[+] Checking running instances...")
        inst_res = client.call_tool("list_instances")
        instances_data = json.loads(inst_res["content"][0]["text"])
        instances = instances_data.get("instances", [])
        print(f"[+] Running instances: {len(instances)}")

        if not instances:
            print("[+] Launching app via launch_app tool...")
            launch_res = client.call_tool("launch_app", timeout=30)
            print(f"[+] launch_app result: {launch_res['content'][0]['text']}")
        else:
            print(f"[+] Connecting to existing instance {instances[0]['name']}...")
            conn_res = client.call_tool("connect", {"pipe_name": instances[0]["name"]})
            print(f"[+] connect result: {conn_res['content'][0]['text']}")

        # 1. get_state - wait for scan to finish (may take ~6-8s on large libraries)
        print("[+] Calling get_state...")
        state = None
        for _ in range(150):
            state_res = client.call_tool("get_state")
            state = json.loads(state_res["content"][0]["text"])
            if state.get("screen") != "scanning":
                break
            time.sleep(0.1)
        print(f"[+] Current state: screen={state.get('screen')}, fps={state.get('fps'):.1f}")

        # 2. list_library
        print("[+] Calling list_library...")
        lib_res = client.call_tool("list_library")
        lib = json.loads(lib_res["content"][0]["text"])
        chars = lib.get("characters", [])
        stages = lib.get("stages", [])
        songs = lib.get("songs", [])
        print(f"[+] Library contents: {len(chars)} characters, {len(stages)} stages, {len(songs)} songs")

        if chars and songs:
            ch_name = chars[0]["name"]
            song_name = songs[0]["name"]
            st_name = stages[0]["name"] if stages else "none"

            # 3. load_scene
            print(f"[+] Loading scene: ch='{ch_name}', st='{st_name}', song='{song_name}'...")
            load_res = client.call_tool("load_scene", {
                "character": ch_name,
                "stage": st_name,
                "song": song_name
            }, timeout=60)
            print(f"[+] load_scene result: {load_res['content'][0]['text']}")

            # 4. wait_frames
            print("[+] Waiting 15 frames...")
            client.call_tool("wait_frames", {"n": 15})

            # 5. seek
            print("[+] Seeking to 2.0 seconds...")
            seek_res = client.call_tool("seek", {"seconds": 2.0})
            print(f"[+] seek result: {seek_res['content'][0]['text']}")

        # 6. screenshot
        print("[+] Taking screenshot (max_width=640)...")
        shot_res = client.call_tool("screenshot", {"max_width": 640})
        found_png = False
        for item in shot_res.get("content", []):
            if item.get("type") == "image":
                b64_data = item.get("data", "")
                raw_bytes = base64.b64decode(b64_data)
                # Verify PNG magic header: \x89PNG\r\n\x1a\n
                if raw_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
                    found_png = True
                    print(f"[+] Valid PNG image received! Size: {len(raw_bytes)} bytes")
        if not found_png:
            raise RuntimeError("Screenshot did not return valid PNG image content")

        # 7. set_screen to studio
        print("[+] Setting screen to studio...")
        set_res = client.call_tool("set_screen", {"screen": "studio"})
        print(f"[+] set_screen result: {set_res['content'][0]['text']}")

        # 8. studio_get_state
        print("[+] Calling studio_get_state...")
        studio_state_res = client.call_tool("studio_get_state")
        st_state = json.loads(studio_state_res["content"][0]["text"])
        models = st_state.get("models", [])
        print(f"[+] Studio state: models_count={len(models)}, dirty={st_state.get('dirty')}")

        # 9. quit_app
        print("[+] Calling quit_app...")
        quit_res = client.call_tool("quit_app")
        print(f"[+] quit_app result: {quit_res['content'][0]['text']}")

        print("[+] Full scenario completed successfully!")
    finally:
        client.close()

def main():
    parser = argparse.ArgumentParser(description="MMDX12 MCP Smoke Test")
    parser.add_argument("--bridge", default="", help="Path to mmdx12_mcp.exe")
    parser.add_argument("--handshake-only", action="store_true", help="Test protocol handshake and tools/list only")
    parser.add_argument("--scenario", action="store_true", help="Run full end-to-end MCP scenario")
    args = parser.parse_args()

    bridge = args.bridge
    if not bridge:
        # Default search locations
        candidates = [
            os.path.join(os.path.dirname(__file__), "..", "build", "bin", "mmdx12_mcp.exe"),
            os.path.join(os.path.dirname(__file__), "..", "build_release", "bin", "mmdx12_mcp.exe"),
        ]
        for c in candidates:
            if os.path.exists(c):
                bridge = os.path.abspath(c)
                break

    if not bridge or not os.path.exists(bridge):
        print(f"[-] Error: mmdx12_mcp.exe not found at {bridge}", file=sys.stderr)
        sys.exit(1)

    if args.handshake_only or not args.scenario:
        run_handshake_only(bridge)

    if args.scenario:
        run_scenario(bridge)

if __name__ == "__main__":
    main()
