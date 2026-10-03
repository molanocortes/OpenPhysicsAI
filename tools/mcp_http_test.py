#!/usr/bin/env python3
"""Independent HTTP client acceptance, criteria declared before the first run:
HTTP initialization/version/session lifecycle works; invalid origins/hosts/lengths
are refused; tools have explicit annotations/output schemas; native state persists
across calls; clients have isolated embedded state; limits and cleanup are enforced.
Connected sessions share a persistent engine and a real FDM job survives DELETE.
No ChatGPT model or public deployment is exercised by this test.
"""
import http.client
import json
from pathlib import Path
import subprocess
import tempfile
import time
from printflow import box_stl, PROCESS

ROOT = Path(__file__).resolve().parent.parent
passed = 0


def check(condition, label):
    global passed
    if not condition:
        raise AssertionError(label)
    passed += 1


def main():
    with tempfile.TemporaryDirectory(prefix='nv-http-') as tmp:
        tmp = Path(tmp)
        bridge = server = None
        proc = subprocess.Popen(['python3', str(ROOT / 'tools/mcp_http.py'), '--port', '0', '--max-sessions', '2',
                                 '--', '--embedded', '--workspace', tmp], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, cwd=ROOT)
        try:
            line = proc.stdout.readline().strip()
            check(line.startswith('OpenPhysicsAI local MCP:'), 'adapter starts')
            port = int(line.split(':')[-1].split('/')[0])
            counter = 0
            def call(method, params=None, sid=None, extra=None, verb='POST', raw=None):
                nonlocal counter
                counter += 1
                message = {'jsonrpc': '2.0', 'id': counter, 'method': method}
                if params is not None:
                    message['params'] = params
                body = json.dumps(message) if raw is None else raw
                headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream'}
                if sid:
                    headers.update({'Mcp-Session-Id': sid, 'MCP-Protocol-Version': '2025-11-25'})
                headers.update(extra or {})
                conn = http.client.HTTPConnection('127.0.0.1', port, timeout=15)
                conn.request(verb, '/mcp', body=body if verb == 'POST' else None, headers=headers)
                res = conn.getresponse()
                status, out_headers, data = res.status, dict(res.getheaders()), res.read()
                conn.close()
                return status, out_headers, json.loads(data) if data else None

            init = {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'http-test', 'version': '1'}}
            status, headers, reply = call('initialize', init)
            check(status == 200, 'initialize HTTP 200')
            sid = headers['Mcp-Session-Id']
            check(len(sid) > 30 and reply['result']['protocolVersion'] == '2025-11-25', 'session/version negotiated')
            check(headers.get('Cache-Control') == 'no-store', 'responses never cached')
            status, _, reply = call('notifications/initialized', sid=sid,
                                    raw=json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}))
            check(status == 202 and reply is None, 'notification HTTP 202')
            status, _, reply = call('tools/list', sid=sid)
            check(status == 200 and len(reply['result']['tools']) > 30, 'native tools discovered')
            for tool in reply['result']['tools']:
                check(all(isinstance(tool['annotations'][key], bool) for key in
                          ('readOnlyHint', 'destructiveHint', 'idempotentHint', 'openWorldHint')), 'explicit tool hints')
                check(tool['outputSchema']['type'] == 'object', 'structured result schema')
            status, _, reply = call('tools/call', {'name': 'project_create', 'arguments': {'name': 'http_research'}}, sid)
            check(status == 200 and reply['result']['structuredContent']['ok'], 'native project creation')
            status, _, reply = call('tools/call', {'name': 'project_inspect', 'arguments': {}}, sid)
            check(status == 200 and 'http_research' in json.dumps(reply), 'native project retained across HTTP calls')
            status, headers, _ = call('initialize', init)
            sid2 = headers['Mcp-Session-Id']
            check(status == 200 and sid2 != sid, 'distinct session')
            status, _, reply = call('tools/call', {'name': 'project_inspect', 'arguments': {}}, sid2)
            check('http_research' not in json.dumps(reply), 'embedded sessions isolated')
            check(call('initialize', init)[0] == 429, 'bounded process/session count')
            check(call('ping', sid=sid, extra={'Origin': 'https://evil.example'})[0] == 403, 'origin refused')
            check(call('ping', sid=sid, extra={'Host': 'evil.example'})[0] == 403, 'host refused')
            check(call('ping', sid=sid, extra={'MCP-Protocol-Version': '2024-11-05'})[0] == 400, 'version mismatch refused')
            check(call('ping')[0] == 400, 'missing initialization refused')
            check(call('ping', sid='no-such-session')[0] == 404, 'unknown session refused')
            check(call('ping', sid=sid, raw='{bad json')[0] == 400, 'malformed JSON refused')
            check(call('ping', sid=sid, raw='{"jsonrpc":"2.0","id":NaN}')[0] == 400, 'nonfinite JSON refused')
            check(call('ping', sid=sid, raw='{"jsonrpc":"2.0","id":true,"method":"ping"}')[0] == 400, 'boolean id refused')
            check(call('ping', sid=sid, extra={'Content-Length': str(2 << 20)})[0] == 413, 'body limit checked before read')
            check(call('ping', sid=sid, extra={'Accept': 'application/json'})[0] == 406, 'transport accept enforced')
            check(call('ping', sid=sid, verb='GET')[0] == 405, 'optional SSE stream explicitly unsupported')
            status, _, _ = call('ping', sid=sid2, verb='DELETE')
            check(status == 204, 'session deleted')
            check(call('ping', sid=sid2)[0] == 404, 'deleted session inaccessible')
            check(call('initialize', init)[0] == 200, 'session capacity recovered')

            # The production default is a persistent engine, not embedded isolation.
            sock, ready = tmp / 'control.sock', tmp / 'server.ready'
            server = subprocess.Popen([str(ROOT / 'navier-server'), '--socket', str(sock), '--ready-file', str(ready),
                                       '--workspace', str(tmp / 'shared'), '--allow-read', str(tmp)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 10
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            check(ready.exists(), 'persistent native engine starts')
            bridge = subprocess.Popen(['python3', str(ROOT / 'tools/mcp_http.py'), '--port', '0', '--', '--connect', str(sock)],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
            line = bridge.stdout.readline().strip()
            check(line.startswith('OpenPhysicsAI local MCP:'), 'connected HTTP adapter starts')
            port = int(line.split(':')[-1].split('/')[0])
            status, headers, _ = call('initialize', init)
            shared_sid = headers['Mcp-Session-Id']
            check(status == 200, 'connected session initializes')

            def tool(name, args, session=shared_sid):
                status, _, reply = call('tools/call', {'name': name, 'arguments': args}, session)
                sc = reply.get('result', {}).get('structuredContent', {})
                check(status == 200 and sc.get('ok') is True, f'connected {name}: {sc.get("error")}')
                return sc['value']

            stl = tmp / 'wall.stl'
            box_stl(stl, (0, -1, 0), (4, 1, 2))
            tool('project_create', {'name': 'http_print'})
            tool('geometry_import', {'path': str(stl), 'units': 'mm', 'name': 'wall'})
            tool('material_assign', {'body': 'wall', 'material': 'pla_generic_demo', 'source': 'inferred'})
            tool('mesh_generate', {'element_size': '1 mm'})
            process = dict(PROCESS, min_layer_time='2 s', cooldown_bed_on='2 s', cooldown_bed_off='2 s',
                           thermal_substeps=2, provenance='inferred')
            job = tool('mech_print_run', {'process': process, 'label': 'HTTP thermal conservation'})['job_id']
            check(call('ping', sid=shared_sid, verb='DELETE')[0] == 204, 'printing client disconnects')
            status, headers, _ = call('initialize', init)
            resumed = headers['Mcp-Session-Id']
            check(status == 200 and resumed != shared_sid, 'replacement connected session initializes')
            check('http_print' in json.dumps(tool('project_inspect', {}, resumed)), 'connected project survives disconnect')
            state = tool('job_status', {'job_id': job, 'wait_seconds': 10}, resumed)
            deadline = time.monotonic() + 30
            while state.get('state') in ('queued', 'running') and time.monotonic() < deadline:
                state = tool('job_status', {'job_id': job, 'wait_seconds': 10}, resumed)
            check(state.get('state') == 'succeeded', f'FDM job survives disconnect: {state.get("error")}')
            check((state.get('summary') or {}).get('results', {}).get('stored_times', 0) >= 3,
                  'HTTP print produces native time-indexed results')
            print(f'mcp_http_test: {passed} passed, 0 failed')
        finally:
            for child in (bridge, proc, server):
                if child is None:
                    continue
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                for stream in (child.stdout, child.stderr):
                    if stream:
                        stream.close()


if __name__ == '__main__':
    main()
