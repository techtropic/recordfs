"""Install -> automatic update -> uninstall, end to end, in Windows Sandbox.

    python tests/update_sandbox_e2e.py --pkgs <dir> --from 0.2.0 --to 0.2.1
                                       [--server wss://...] [--work <dir>]

A disposable VM, so nothing is installed on the machine running the test.
<dir> holds RecordFS-<from>.msi and RecordFS-<to>.msi (build them with
`-p:RecordFSVersion=...`; unsigned is fine -- the sandbox sets
UpdateAllowUnsigned). The VC++ redistributable and WinFsp come from
SetupRecordFSBundle\\redist. A one-day mount token for the testing login is
minted on --server and handed to the sandbox, which stores it with
store-token and deletes the file.

What run.ps1 does inside, and what is asserted here: see
tests/update_sandbox/run.ps1. Needs Windows Sandbox enabled; the sandbox
window is visible while it runs (~5-10 minutes) and closes itself.
"""
import argparse, json, os, shutil, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(here)
p = argparse.ArgumentParser()
p.add_argument("--pkgs", required=True)
p.add_argument("--from", dest="frm", required=True)
p.add_argument("--to", required=True)
p.add_argument("--server", default="wss://testing.scheduler.techtropic.ca:7243/")
p.add_argument("--work", default=os.path.join(os.environ.get("TEMP", "."), "recordfs-sandbox"))
p.add_argument("--record-like", default="*Lot 22*")
p.add_argument("--file", default="workorder-signed.pdf")
p.add_argument("--timeout-min", type=int, default=25)
args = p.parse_args()

running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq WindowsSandbox*"], capture_output=True, text=True).stdout
if "WindowsSandbox" in running:
    sys.exit("a Windows Sandbox is already running (only one can run at a time)")

# Windows Sandbox wants a plain Windows path in HostFolder.
args.work = os.path.normpath(os.path.abspath(args.work))
sb = os.path.join(args.work, "sb")
shutil.rmtree(args.work, ignore_errors=True)
os.makedirs(os.path.join(sb, "feed"))
for name in ("run.ps1", "mock.ps1"):
    shutil.copy(os.path.join(here, "update_sandbox", name), sb)
redist = os.path.join(repo, "SetupRecordFSBundle", "redist")
shutil.copy(os.path.join(redist, "vc_redist.x64.exe"), sb)
shutil.copy(os.path.join(redist, "winfsp.msi"), sb)
shutil.copy(os.path.join(args.pkgs, f"RecordFS-{args.frm}.msi"), sb)
shutil.copy(os.path.join(args.pkgs, f"RecordFS-{args.to}.msi"), os.path.join(sb, "feed"))
with open(os.path.join(sb, "config.json"), "w") as f:
    json.dump({"from": args.frm, "to": args.to, "server": args.server,
               "record_like": args.record_like, "file": args.file}, f)
token_file = os.path.join(sb, "token.txt")
r = subprocess.run([sys.executable, os.path.join(here, "mint_profile.py"), args.server, "sandbox", "R:",
                    "--machine", "update-sandbox-test", "--token-file", token_file])
if r.returncode:
    sys.exit("could not mint a mount token")

wsb = os.path.join(args.work, "update-test.wsb")
with open(wsb, "w") as f:
    f.write(f"""<Configuration>
  <MappedFolders>
    <MappedFolder><HostFolder>{sb}</HostFolder><SandboxFolder>C:\\t</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>powershell -NoProfile -ExecutionPolicy Bypass -File C:\\t\\run.ps1</Command></LogonCommand>
  <Networking>Enable</Networking>
  <MemoryInMB>4096</MemoryInMB>
</Configuration>
""")

print("starting Windows Sandbox:", wsb, flush=True)
os.startfile(wsb)
done = os.path.join(sb, "out", "done.flag")
deadline = time.time() + args.timeout_min * 60
while time.time() < deadline and not os.path.exists(done):
    time.sleep(10)
if os.path.exists(token_file):
    os.remove(token_file)
if not os.path.exists(done):
    sys.exit("timed out; see " + os.path.join(sb, "out"))
time.sleep(3)

res = json.load(open(os.path.join(sb, "out", "results.json"), encoding="utf-8-sig"))
print(json.dumps(res, indent=2))
agent_log = open(os.path.join(sb, "out", "agent.log"), encoding="utf-8", errors="replace").read() \
    if os.path.exists(os.path.join(sb, "out", "agent.log")) else ""
upd_log = open(os.path.join(sb, "out", "updater.log"), encoding="utf-8", errors="replace").read() \
    if os.path.exists(os.path.join(sb, "out", "updater.log")) else ""

passed, failed, skipped = [], [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("PASS " if cond else "FAIL ") + name + (("  -- " + str(detail)[:300]) if not cond else ""))


check("installs_old_version", res.get("install_from_exit") == 0 and res.get("from_file_version") == args.frm, res)
check("updater_service_running", res.get("service_after_install") == "Running", res.get("service_after_install"))
mounted = res.get("mounted_before_update") is True
# Windows Sandbox does not load WinFsp's kernel driver (STATUS_NO_SUCH_DEVICE,
# 0xc000000e): the update itself is fully testable there, a live drive is
# not. Anything else that stops the mount is a real failure.
no_driver = "0xc000000e" in agent_log.lower() or "3221225486" in agent_log
if not mounted and no_driver:
    print("SKIP drive_mounted -- the WinFsp driver does not run in this sandbox; "
          "the open-file deferral and remount checks need a real (or Hyper-V) machine")
else:
    check("drive_mounted", mounted, "mount failed; the deferral and remount checks are skipped")
check("UPDATED_TO_NEW_VERSION", res.get("installed_version_after") == args.to and
      res.get("file_version_after") == args.to, (res.get("installed_version_after"), res.get("file_version_after")))
check("service_running_after_update", res.get("service_after_update") == "Running", res.get("service_after_update"))
check("SITE_DRIVE_LETTER_KEPT", res.get("drive_letter_kept") == "R:", res.get("drive_letter_kept"))
check("SITE_VOLUME_LABEL_KEPT", res.get("volume_label_kept") == "Sandbox Records", res.get("volume_label_kept"))
check("autoupdate_defaulted_on", res.get("autoupdate_value") == 1, res.get("autoupdate_value"))
check("staging_dir_admin_only", "BUILTIN\\Users:ReadAndExecute" in str(res.get("updates_dir_acl")) and
      "Users:FullControl" not in str(res.get("updates_dir_acl")) and
      "Users:Write" not in str(res.get("updates_dir_acl")), res.get("updates_dir_acl"))
if mounted:
    check("UPDATE_WAITED_FOR_THE_OPEN_FILE", "waiting for 1 open file" in agent_log, agent_log[-600:])
    released = res.get("file_released_at", "")
    installing = [l[:8] for l in upd_log.splitlines() if " installing " in l]
    check("INSTALL_STARTED_ONLY_AFTER_THE_FILE_CLOSED",
          bool(released) and bool(installing) and installing[-1] >= released, (released, installing))
    check("DRIVE_BACK_AFTER_UPDATE", res.get("remounted_after_update") is True, res.get("remounted_after_update"))
    check("label_after_update", res.get("volume_label_after") == "Sandbox Records", res.get("volume_label_after"))
check("agent_restarted", (res.get("agents_running_after") or 0) >= 1, res.get("agents_running_after"))
check("update_completion_logged", "complete" in upd_log, upd_log[-600:])
check("uninstall_ok", res.get("uninstall_exit") == 0, res.get("uninstall_exit"))
check("uninstall_removes_service", res.get("service_removed") is True)
check("uninstall_removes_staging", res.get("updates_dir_removed") is True)
check("uninstall_removes_updater_state", res.get("updater_state_removed") is True)
check("uninstall_removes_autostart", res.get("run_key_removed") is True)

print()
print(f"{len(passed)} passed, {len(failed)} failed  (logs: {os.path.join(sb, 'out')})")
sys.exit(1 if failed else 0)
