"""Exercise the real HTTP bridge against a CLI-generated local world. Standard library only."""
import argparse
import json
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--dotnet', default='dotnet')
p.add_argument('--build-plan', help='Optional real Twin-O-Matic export to exercise')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
cli = root / 'src/Repoverse.Cli/bin/Debug/net8.0/Repoverse.Cli.dll'
service = root / 'src/Repoverse.Sidecar/bin/Debug/net8.0/Repoverse.Sidecar.dll'
with tempfile.TemporaryDirectory(prefix='repoverse-http-') as temp:
    work = Path(temp)
    subprocess.run([a.dotnet, str(cli), 'ingest-local', str(root), '--owner', 'SNAPKITTYWEST', '--out', str(work/'snapshot.json')], check=True)
    subprocess.run([a.dotnet, str(cli), 'generate', '--snapshot', str(work/'snapshot.json'), '--out', str(work/'manifest.json')], check=True)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    token = secrets.token_hex(32)
    args = [a.dotnet, str(service), '--manifest', str(work/'manifest.json'), '--token', token,
            '--port', str(port), '--save', str(work/'save.json'), '--source-root', str(root), '--source-repo', 'snapkittywest/repoverse']
    with (work/'service.log').open('w') as log:
        child = subprocess.Popen(args, stdout=log, stderr=log)
        try:
            def request(route, data=None, auth=token, origin=None):
                headers = {'X-Repoverse-Token': auth}
                if origin: headers['Origin'] = origin
                if data is not None: headers['Content-Type'] = 'application/json'
                req = urllib.request.Request(f'http://127.0.0.1:{port}'+route, data=None if data is None else json.dumps(data).encode(), headers=headers)
                try:
                    with urllib.request.urlopen(req, timeout=15) as response:
                        return response.status, json.load(response)
                except urllib.error.HTTPError as e:
                    return e.code, e.read().decode()
            for _ in range(100):
                if child.poll() is not None: raise RuntimeError((work/'service.log').read_text())
                try:
                    if request('/v1/health')[0] == 200: break
                except urllib.error.URLError: time.sleep(.1)
            else: raise RuntimeError('Sidecar did not start')
            assert request('/v1/health', auth='bad')[0] == 403
            assert request('/v1/health', origin='https://example.com')[0] == 403
            status, session = request('/v1/session', {})
            assert status == 200 and session['schemaVersion'] == 1 and session['destinations']
            player = session['player']
            resident_ids = set()
            for _ in range(100):
                status, packet = request('/v1/focus', {'x': player['x'], 'z': player['z']})
                assert status == 200 and len(packet['chunks']) <= 4
                resident_ids.difference_update(packet['unloaded'])
                resident_ids.update(c['id'] for c in packet['chunks'])
                if packet['remaining'] == 0: break
            else: raise AssertionError('Focus did not settle')
            assert resident_ids
            plan = json.loads(Path(a.build_plan).read_text()) if a.build_plan else {
                'schemaVersion': 1, 'id': 'http-builder', 'actor': 'twin-builder', 'units': 'voxel',
                'origin': {'x': 0, 'y': 9, 'z': 0},
                'operations': [{'name': 'add_box', 'args': {'width': 2, 'height': 2, 'depth': 2, 'y': 1, 'color': '#5ad1c4'}}]}
            status, receipt = request('/v1/build', plan)
            assert status == 200 and receipt['voxels'] > 0, receipt
            assert request('/v1/build', {**plan, 'operations': plan['operations'] + [{'name': 'eval', 'args': {}}]})[0] == 400
            assert request('/v1/focus', {'x': player['x'], 'z': player['z']})[1]['builders'] == [receipt]
            player['selection'] = 'repoverse://snapkittywest/repoverse/README.md'
            assert request('/v1/save', player)[0] == 200
            assert request('/v1/load', {})[1] == player
            assert json.loads((work/'save.json').read_text())['builders'] == [receipt]
            status, source = request('/v1/source?repo=snapkittywest%2Frepoverse&path=README.md&offset=0')
            assert status == 200 and source['lines'], source
            assert request('/v1/source?repo=snapkittywest%2Frepoverse&path=.git%2Fconfig&offset=0')[0] == 400
            assert request('/v1/focus', {'x': 1e20, 'z': 0})[0] == 400
            # A newly connected renderer must receive the world again.
            assert request('/v1/session', {})[1]['builders'] == [receipt]
            assert request('/v1/focus', {'x': player['x'], 'z': player['z']})[1]['chunks']
            print('PASS: authentication, browser isolation, real mesh stream, save/load, source paging, invalid requests, builder placement/persistence, reconnect')
        finally:
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()
