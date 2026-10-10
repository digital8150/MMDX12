#!/usr/bin/env python3
"""Interactive helper over mcp_pipe for exploring the UI: a sequence of short commands in one call.

    python mcp_x.py "click 플레이" "wait 30" "shot out.png" "items 셰이더" "key Escape"

Commands: click <target>[@index] | rclick <target> | hover <target> | items [query] | shot <png> [x y w h]
          | wait <frames> | key <name> | move <x> <y> | state | sleep <s> | call <tool> <json>
"""
import json
import sys
import time

from mcp_pipe import MmdxPipe, McpError


def run(app, line):
    cmd, _, rest = line.partition(" ")
    if cmd in ("click", "rclick", "hover", "dclick"):
        target, _, idx = rest.rpartition("@") if "@" in rest else (rest, "", "")
        args = {"target": target, "action": {"click": "click", "rclick": "right", "hover": "hover", "dclick": "dblclick"}[cmd]}
        if idx:
            args["index"] = int(idx)
        r = app.call("ui_click", **args)
        print(f"{cmd} -> {r['label']!r} {r['id']} {r['rect']}")
    elif cmd == "items":
        r = app.call("ui_items", query=rest, limit=300) if rest else app.call("ui_items", limit=300)
        for i in r["items"]:
            if i["window"].startswith("Debug##"):
                continue
            print(f"  {i['label']!r:40} {i['id']:24} {i['window'][:28]:28} {i['rect']}{' D' if i.get('disabled') else ''}")
        print(f"  ({r['total']} total, screen {r['screen']})")
    elif cmd == "shot":
        parts = rest.split()
        args = {"path": parts[0]}
        if len(parts) == 5:
            args["region"] = [float(v) for v in parts[1:]]
        r = app.call("screenshot", **args)
        print(f"shot -> {r['path']} {r['width']}x{r['height']}")
    elif cmd == "wait":
        app.call("wait_frames", n=int(rest or 1))
    elif cmd == "sleep":
        time.sleep(float(rest))
    elif cmd == "key":
        app.call("ui_input", events=[{"cmd": "key", "args": rest.split()}, {"cmd": "keyup", "args": rest.split()[:1]}])
    elif cmd == "move":
        x, y = rest.split()
        app.call("ui_input", events=[{"cmd": "move", "args": [x, y]}])
    elif cmd == "state":
        print(json.dumps(app.call("get_state"), ensure_ascii=False))
    elif cmd == "call":
        tool, _, js = rest.partition(" ")
        r = app.call(tool, **(json.loads(js) if js else {}))
        if isinstance(r, dict):
            r.pop("image_base64", None)
        print(json.dumps(r, ensure_ascii=False)[:3000])
    else:
        raise SystemExit(f"unknown command: {line}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    app = MmdxPipe()
    for line in sys.argv[1:]:
        try:
            run(app, line)
        except McpError as e:
            print(f"ERROR {line}: {e}")
            sys.exit(1)
