"""A stand-in for the GitHub releases API, for testing the updater offline.

    python tests/update_feed_mock.py <packages-dir> [--port 8765] [--host 127.0.0.1]

Serves GitHub's "latest release" JSON shape at /<scenario>/latest and the
package bytes at /assets/<name>. Every RecordFS-<version>.msi in the
packages directory is published as scenario "v<version>" (e.g. /v0.2.1/latest)
with its real size and SHA-256 digest. Negative scenarios reuse the highest
package: /badsize, /baddigest, /prerelease, /noasset, /rctag, /old, and
/missing (HTTP 404, like a private or empty repository). Point the updater
at one with HKLM\\SOFTWARE\\RecordFS UpdateFeed, or `update-check --feed`.

Plain HTTP is accepted by the updater for loopback only; signatures are the
trust anchor either way.
"""
import argparse, hashlib, http.server, json, os, re, sys, threading

p = argparse.ArgumentParser()
p.add_argument("dir")
p.add_argument("--port", type=int, default=8765)
p.add_argument("--host", default="127.0.0.1")
p.add_argument("--ready-file", default=None, help="write the bound port here once listening")
args = p.parse_args()

packages = {}
for name in os.listdir(args.dir):
    m = re.fullmatch(r"RecordFS-(\d+\.\d+\.\d+)\.msi", name, re.I)
    if not m:
        continue
    path = os.path.join(args.dir, name)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    packages[m.group(1)] = {"name": name, "size": os.path.getsize(path), "digest": "sha256:" + h.hexdigest()}


def vkey(v):
    return tuple(int(x) for x in v.split("."))


def release(base, tag, asset=None, **extra):
    r = {"tag_name": tag, "name": tag, "draft": False, "prerelease": False, "assets": []}
    if asset:
        r["assets"].append({
            "name": asset["name"], "size": asset["size"], "digest": asset.get("digest"),
            "content_type": "application/x-msi",
            "browser_download_url": base + "/assets/" + asset["name"]})
    r.update(extra)
    return r


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        sys.stderr.write("mock: " + (fmt % a) + "\n")

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        base = "http://%s:%d" % (args.host, self.server.server_address[1])
        path = self.path.split("?")[0]
        if path.startswith("/assets/"):
            name = os.path.basename(path[len("/assets/"):])
            full = os.path.join(args.dir, name)
            if not os.path.isfile(full):
                return self.send_json({"message": "Not Found"}, 404)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(os.path.getsize(full)))
            self.end_headers()
            with open(full, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    self.wfile.write(chunk)
            return
        m = re.fullmatch(r"/([^/]+)/latest", path)
        if not m:
            return self.send_json({"message": "Not Found"}, 404)
        scenario = m.group(1)
        top = max(packages, key=vkey) if packages else None
        a = dict(packages[top]) if top else None
        if scenario.startswith("v") and scenario[1:] in packages:
            return self.send_json(release(base, scenario, packages[scenario[1:]]))
        if scenario == "missing" or not a:
            return self.send_json({"message": "Not Found"}, 404)
        if scenario == "badsize":
            a["size"] += 1
            return self.send_json(release(base, "v" + top, a))
        if scenario == "baddigest":
            a["digest"] = "sha256:" + "0" * 64
            return self.send_json(release(base, "v" + top, a))
        if scenario == "prerelease":
            return self.send_json(release(base, "v" + top, a, prerelease=True))
        if scenario == "noasset":
            return self.send_json(release(base, "v" + top))
        if scenario == "rctag":
            return self.send_json(release(base, "v" + top + "-rc1", a))
        if scenario == "old":
            return self.send_json(release(base, "v0.0.1", a))
        return self.send_json({"message": "Not Found"}, 404)


server = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
if args.ready_file:
    with open(args.ready_file, "w") as f:
        f.write(str(server.server_address[1]))
print("mock feed on http://%s:%d  packages: %s" % (args.host, server.server_address[1],
                                                    ", ".join(sorted(packages, key=vkey))), flush=True)
server.serve_forever()
