"""The updater's decisions, through `recordfs update-check` (a dry run: it
downloads and verifies exactly as the service does, and changes nothing).

    python tests/update_check_e2e.py <packages-dir> --exe <recordfs.exe> [--signed-other X.msi]

<packages-dir> holds RecordFS-<version>.msi test packages (unsigned builds
made with -p:RecordFSVersion=...) and is served by update_feed_mock.py,
started here. --exe is a recordfs.exe OLDER than the newest package, with
its OpenSSL DLLs beside it. --signed-other is any MSI validly signed by the
RecordFS publisher that is NOT RecordFS (a Scheduler++ installer, say): it
is published as version 9.9.9 to prove that a good signature alone does not
get a package installed.

Run on a machine where RecordFS is not installed through its MSI and
HKLM\\SOFTWARE\\RecordFS UpdateAllowUnsigned is not set.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

p = argparse.ArgumentParser()
p.add_argument("dir")
p.add_argument("--exe", required=True)
p.add_argument("--signed-other", default=None)
args = p.parse_args()

here = os.path.dirname(os.path.abspath(__file__))
passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + (("  -- " + str(detail)[-300:]) if not cond else ""))


def run(feed):
    r = subprocess.run([args.exe, "update-check", "--feed", feed], capture_output=True, text=True,
                       timeout=600)
    return r.returncode, r.stdout + r.stderr


serve_dir = tempfile.mkdtemp(prefix="rfs-feed-")
for n in os.listdir(args.dir):
    if n.lower().endswith(".msi"):
        shutil.copy(os.path.join(args.dir, n), serve_dir)
if args.signed_other:
    shutil.copy(args.signed_other, os.path.join(serve_dir, "RecordFS-9.9.9.msi"))
ready = os.path.join(serve_dir, "port.txt")
mock = subprocess.Popen([sys.executable, os.path.join(here, "update_feed_mock.py"), serve_dir,
                         "--port", "0", "--ready-file", ready],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        if os.path.exists(ready) and open(ready).read().strip():
            break
        time.sleep(0.1)
    base = "http://127.0.0.1:" + open(ready).read().strip()
    versions = sorted((n[9:-4] for n in os.listdir(serve_dir)
                       if n.startswith("RecordFS-") and n.endswith(".msi") and n != "RecordFS-9.9.9.msi"),
                      key=lambda v: tuple(map(int, v.split("."))))
    newest = versions[-1]

    code, out = run(base + "/v" + newest + "/latest")
    check("offers_the_newer_release", "update available" in out and newest in out, out)
    check("UNSIGNED_PACKAGE_REFUSED", "refusing" in out and "not signed" in out and code != 0, out)

    if args.signed_other:
        code, out = run(base + "/v9.9.9/latest")
        check("SIGNED_BY_PUBLISHER_BUT_NOT_RECORDFS_REFUSED",
              "not a RecordFS package" in out and code != 0, out)

    code, out = run(base + "/badsize/latest")
    check("SIZE_MISMATCH_REFUSED", "does not match the published" in out and code != 0, out)
    code, out = run(base + "/baddigest/latest")
    check("DIGEST_MISMATCH_REFUSED", "SHA-256 does not match" in out and code != 0, out)
    code, out = run(base + "/old/latest")
    check("older_release_is_not_an_update", "up to date" in out and code == 0, out)
    code, out = run(base + "/prerelease/latest")
    check("prerelease_ignored", "no published release" in out and code == 0, out)
    code, out = run(base + "/rctag/latest")
    check("suffixed_tag_ignored", "not a plain version" in out and code == 0, out)
    code, out = run(base + "/noasset/latest")
    check("release_without_msi_ignored", "carries no RecordFS .msi" in out and code == 0, out)
    code, out = run(base + "/missing/latest")
    check("feed_404_explained", "HTTP 404" in out and "not public" in out and code != 0, out)
    code, out = run("http://example.com/releases/latest")
    check("PLAIN_HTTP_REFUSED_OFF_LOOPBACK", "non-HTTPS" in out and code != 0, out)
finally:
    mock.terminate()
    mock.wait()
    shutil.rmtree(serve_dir, ignore_errors=True)

print()
print(f"{len(passed)} passed, {len(failed)} failed")
sys.exit(1 if failed else 0)
