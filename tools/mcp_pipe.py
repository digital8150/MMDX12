#!/usr/bin/env python3
"""Minimal client for MMDX12's MCP named pipe (no bridge, standard library only).

The app listens on \\\\.\\pipe\\mmdx12_mcp (the first instance) or \\\\.\\pipe\\mmdx12_mcp_<pid>. Requests and answers
are JSON lines: {"id", "tool", "args"} -> {"id", "ok", "result" | "error"}. The tools are the ones in
src/app/McpTools.h (docs/mcp.md); the bridge-only ones (launch_app, connect, list_instances) are not available here.

Library use:
    from mcp_pipe import MmdxPipe
    app = MmdxPipe()                       # or MmdxPipe(pid=1234) / MmdxPipe(name="mmdx12_mcp_1234")
    app.call("ui_click", target="플레이")
    app.call("screenshot", path="out.png")

CLI: python mcp_pipe.py [--pid N] <tool> [json-args]
"""
import json
import os
import sys
import time


class McpError(RuntimeError):
    pass


def list_pipes():
    try:
        return sorted(p for p in os.listdir(r"\\.\pipe\\") if p.startswith("mmdx12_mcp"))
    except OSError:
        return []


class MmdxPipe:
    def __init__(self, name=None, pid=None, wait=30.0):
        if pid is not None:
            name = f"mmdx12_mcp_{pid}"
        deadline = time.time() + wait
        while True:
            names = [name] if name else list_pipes()
            for n in names:
                try:
                    self.f = open(r"\\.\pipe" + "\\" + n, "r+b", buffering=0)
                    self.name = n
                    self.next_id = 1
                    self.buf = b""
                    return
                except OSError:
                    pass
            if time.time() > deadline:
                raise McpError(f"no MMDX12 MCP pipe answered ({name or 'any'})")
            time.sleep(0.5)

    def call(self, tool, **args):
        rid = self.next_id
        self.next_id += 1
        self.f.write((json.dumps({"id": rid, "tool": tool, "args": args}, ensure_ascii=False) + "\n").encode("utf-8"))
        while True:
            while b"\n" not in self.buf:
                chunk = self.f.read(65536)
                if not chunk:
                    raise McpError("pipe closed")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            if not line.strip():
                continue
            resp = json.loads(line.decode("utf-8"))
            if resp.get("id") != rid:
                continue  # the answer to an earlier call that timed out on our side
            if not resp.get("ok"):
                raise McpError(f"{tool}: {resp.get('error')}")
            return resp.get("result")

    def close(self):
        self.f.close()


def main(argv):
    pid = None
    if len(argv) >= 2 and argv[0] == "--pid":
        pid = int(argv[1])
        argv = argv[2:]
    if not argv:
        print(__doc__)
        print("pipes:", list_pipes())
        return 1
    tool = argv[0]
    args = json.loads(argv[1]) if len(argv) > 1 else {}
    res = MmdxPipe(pid=pid).call(tool, **args)
    if isinstance(res, dict) and "image_base64" in res:
        res["image_base64"] = f"<{len(res['image_base64'])} base64 chars>"
    print(json.dumps(res, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
