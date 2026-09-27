#!/usr/bin/env python3
"""A scripted stand-in for an AI agent, for testing Agent mode without a model.

Agent mode launches whatever command the user chose, with the question in $NAVIER_PROMPT and an MCP configuration in
$NAVIER_MCP_CONFIG that points at the window's own engine. This script is such a command: it reads the configuration,
starts the MCP server the way a real client would, and does what a competent agent would do with the question "build
the calibration cantilever and tell me the tip deflection", printing one line per step as an agent's transcript.

It holds no key and calls no model. It exists so tools/agentwalk.nav can prove, repeatably and in seconds, that a
question typed into Agent mode reaches a result in the window with no button pressed. The real client is verified
separately (see the wave report); this is the scripted client standing in for it, and it says so in its first line.

Run by Agent mode, e.g. with the command line:  python3 tools/agent_standin.py
"""
import json
import os
import select
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def say(text):
    print(text, flush=True)


class Mcp:
    def __init__(self, command, args):
        self.p = subprocess.Popen([command] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.buf = b""
        self.next = 1

    def request(self, method, params=None, timeout=120):
        rid = self.next
        self.next += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg).encode() + b"\n")
        self.p.stdin.flush()
        deadline = time.time() + timeout
        while True:
            while b"\n" not in self.buf:
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError(method)
                r, _, _ = select.select([self.p.stdout.fileno()], [], [], left)
                if r:
                    chunk = os.read(self.p.stdout.fileno(), 1 << 20)
                    if not chunk:
                        raise EOFError("the MCP server closed")
                    self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            resp = json.loads(line)
            if resp.get("id") == rid:
                return resp

    def call(self, name, args):
        r = self.request("tools/call", {"name": name, "arguments": args})
        res = r.get("result", {})
        return res.get("structuredContent") or {}, bool(res.get("isError"))


def main():
    prompt = os.environ.get("NAVIER_PROMPT", "")
    cfg_path = os.environ.get("NAVIER_MCP_CONFIG", "")
    say("(scripted stand-in agent: no model is called; this proves the window's side of Agent mode)")
    if not cfg_path or not Path(cfg_path).exists():
        say("no MCP configuration was passed: Agent mode did not set $NAVIER_MCP_CONFIG")
        return 2
    server = json.loads(Path(cfg_path).read_text())["mcpServers"]["navier-am"]
    mcp = Mcp(server["command"], server.get("args", []))
    mcp.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "agent-standin", "version": "1"}})
    mcp.p.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    mcp.p.stdin.flush()
    say(f"I was asked: {prompt.splitlines()[0] if prompt else '(nothing)'}")
    stl = str(ROOT / "samples" / "cantilever.stl")
    steps = [
        ("project_create", {"name": "agent-cantilever", "overwrite": True}, "I start a project for the cantilever."),
        ("geometry_import", {"path": stl, "units": "mm", "name": "cantilever"}, "I import the part, in millimetres as you said."),
        ("material_assign", {"body": "cantilever", "material": "alsi10mg_lpbf", "source": "user"},
         "I give it the library's AlSi10Mg, whose values name their sources."),
        ("mesh_generate", {"element_size": "1 mm"}, "I divide it into 1 mm cells, which is quick."),
        ("lpbf_build_run", {"body": "cantilever", "build_orientation": "Y", "layer_thickness_sim": "1 mm",
                            "inherent_strain": {"exx": -0.0012226242259139248, "eyy": -0.0034058581422344394, "ezz": -0.03,
                                                "provenance": "calibrated",
                                                "source": "the elastic strain of the P17 preset in src/ctl/print_profiles.json"},
                            "material": {"youngs_modulus": "71000 MPa", "poissons_ratio": 0.33, "provenance": "user"},
                            "cut": {"height": "2.5 mm", "kerf": "0.3 mm", "from_x": "10 mm", "provenance": "assumed"},
                            "label": "agent stand-in"}, "I start the build, layer by layer, with the sample's assumed cut."),
    ]
    job = None
    for name, args, words in steps:
        say(words)
        out, err = mcp.call(name, args)
        if err:
            say(f"that was refused: {json.dumps(out)[:300]}")
            return 1
        value = out.get("value", out)
        job = value.get("job_id", job)
    say("I wait for the build to finish.")
    while True:
        out, err = mcp.call("job_status", {"job_id": job, "wait_seconds": 20})
        value = out.get("value", out)
        if value.get("state") in ("succeeded", "failed", "cancelled"):
            break
    if value.get("state") != "succeeded":
        say(f"the build did not finish: {value.get('state')}")
        return 1
    res = (value.get("summary") or {}).get("results", {})
    say(f"The tip rises {res.get('tip_uz_after_cut_mm', float('nan')):.3f} mm once the part is cut off the plate "
        f"({res.get('tip_uz_before_cut_mm', float('nan')):.3f} mm while it is still on it).")
    mcp.p.stdin.close()
    mcp.p.wait(timeout=10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
