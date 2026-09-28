# SPDX-License-Identifier: MIT
# Runs INSIDE Windows Sandbox (the LogonCommand of update_sandbox_e2e.py's
# .wsb). C:\t is the mapped test folder; everything observed lands in
# C:\t\out (results.json + logs), then the sandbox shuts itself down.
#
# The scenario: install the OLD RecordFS with site settings (drive R:, a
# custom label), mount it against the configured server, hold a file open on
# the drive, then have the updater service install the NEW version from a
# local mock feed. The update must wait for that file, install, bring the
# drive back, and keep the site's settings -- and uninstall must clean up.
$ErrorActionPreference = 'Continue'
$T = 'C:\t'
$out = "$T\out"
New-Item -ItemType Directory -Force $out | Out-Null
$log = "$out\run.log"
function Log($m) { "$(Get-Date -Format HH:mm:ss) $m" | Out-File -FilePath $log -Append -Encoding utf8 }
$results = [ordered]@{}
function Result($k, $v) { $results[$k] = $v; Log "RESULT $k = $v" }
function Uninstall-Entry { Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue | Where-Object DisplayName -eq 'RecordFS' | Select-Object -First 1 }

$cfg = Get-Content "$T\config.json" -Raw | ConvertFrom-Json
$exe = 'C:\Program Files\RecordFS\recordfs.exe'
try {
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  Log "user $(whoami) elevated=$admin"

  # --- prerequisites (what the setup bundle chains) ---------------------------
  $p = Start-Process "$T\vc_redist.x64.exe" -ArgumentList '/install', '/quiet', '/norestart' -Wait -PassThru
  Log "vc_redist exit $($p.ExitCode)"
  $p = Start-Process msiexec.exe -ArgumentList '/i', "`"$T\winfsp.msi`"", '/qn', '/norestart' -Wait -PassThru
  Log "winfsp exit $($p.ExitCode)"

  # --- test policy: a local feed, and unsigned test packages accepted --------
  New-Item -Path HKLM:\SOFTWARE\RecordFS -Force | Out-Null
  New-ItemProperty HKLM:\SOFTWARE\RecordFS -Name UpdateFeed -Value "http://127.0.0.1:8765/v$($cfg.to)/latest" -PropertyType String -Force | Out-Null
  New-ItemProperty HKLM:\SOFTWARE\RecordFS -Name UpdateAllowUnsigned -Value 1 -PropertyType DWord -Force | Out-Null

  # --- the OLD version, with site settings ------------------------------------
  $p = Start-Process msiexec.exe -ArgumentList '/i', "`"$T\RecordFS-$($cfg.from).msi`"", '/qn', '/norestart', 'DRIVELETTER=R:', 'VOLUMELABEL="Sandbox Records"', '/l*v', "`"$out\install-from.log`"" -Wait -PassThru
  Result 'install_from_exit' $p.ExitCode
  Result 'from_file_version' (Get-Item $exe).VersionInfo.FileVersion
  Result 'service_after_install' "$((Get-Service RecordFSUpdate -ErrorAction SilentlyContinue).Status)"

  # --- this user's credentials, then the agent the way logon starts it --------
  $tok = (Get-Content "$T\token.txt" -Raw).Trim()
  $p = Start-Process $exe -ArgumentList 'store-token', '--server', $cfg.server, '--token', $tok -Wait -PassThru -WindowStyle Hidden
  Remove-Item "$T\token.txt" -Force
  Log "store-token exit $($p.ExitCode)"
  Start-Process $exe -ArgumentList 'agent' -WindowStyle Hidden
  $mounted = $false
  for ($i = 0; $i -lt 30 -and -not $mounted; $i++) { Start-Sleep 2; $mounted = Test-Path 'R:\workorders' }
  Result 'mounted_on_fresh_machine' $mounted
  if (-not $mounted) {
    # A fresh Windows fetches root certificates on first use (lazily); give
    # it a reason to, then let the agent's retry pick it up.
    try { Invoke-WebRequest -UseBasicParsing "https://$(([Uri]$cfg.server).Host):$(([Uri]$cfg.server).Port)/" -TimeoutSec 20 | Out-Null } catch { Log "warm-up request: $($_.Exception.Message)" }
    for ($i = 0; $i -lt 45 -and -not $mounted; $i++) { Start-Sleep 2; $mounted = Test-Path 'R:\workorders' }
  }
  Result 'mounted_before_update' $mounted
  if ($mounted) { Result 'volume_label_before' ([IO.DriveInfo]::new('R')).VolumeLabel }

  # --- somebody has a document open on the drive -------------------------------
  $holdSeconds = 75
  if ($mounted) {
    $rec = (Get-ChildItem R:\workorders | Where-Object Name -like $cfg.record_like | Select-Object -First 1).FullName
    $file = Join-Path $rec $cfg.file
    $cmd = "`$f=[IO.File]::Open('$file','Open','Read','Read'); 'held' | Out-File '$out\held.txt'; Start-Sleep $holdSeconds; `$f.Close(); (Get-Date).ToString('HH:mm:ss') | Out-File '$out\released.txt'"
    Start-Process powershell -ArgumentList '-NoProfile', '-Command', $cmd -WindowStyle Hidden
    for ($i = 0; $i -lt 30 -and -not (Test-Path "$out\held.txt"); $i++) { Start-Sleep 1 }
    Result 'file_held_open' (Test-Path "$out\held.txt")
  }

  # --- the new release appears; an administrator says "check now" -------------
  Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$T\mock.ps1", '-Dir', "$T\feed", '-Port', '8765' -WindowStyle Hidden
  Start-Sleep 3
  $p = Start-Process $exe -ArgumentList 'update-check', '--apply' -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput "$out\apply.txt" -RedirectStandardError "$out\apply_err.txt"
  Result 'check_now_exit' $p.ExitCode

  $v = $null
  for ($i = 0; $i -lt 200; $i++) {
    Start-Sleep 3
    $v = (Uninstall-Entry).DisplayVersion
    if ($v -eq $cfg.to -and (Get-Item $exe).VersionInfo.FileVersion -eq $cfg.to) { break }
  }
  Result 'installed_version_after' $v
  Result 'file_version_after' (Get-Item $exe).VersionInfo.FileVersion
  if ($mounted) {
    $remounted = $false
    for ($i = 0; $i -lt 40 -and -not $remounted; $i++) { Start-Sleep 2; $remounted = Test-Path 'R:\workorders' }
    Result 'remounted_after_update' $remounted
    if ($remounted) { Result 'volume_label_after' ([IO.DriveInfo]::new('R')).VolumeLabel }
    if (Test-Path "$out\released.txt") { Result 'file_released_at' (Get-Content "$out\released.txt" -Raw).Trim() }
  }
  $agents = @(Get-Process recordfs -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })
  Result 'agents_running_after' $agents.Count
  Result 'service_after_update' "$((Get-Service RecordFSUpdate -ErrorAction SilentlyContinue).Status)"
  $pol = Get-ItemProperty HKLM:\SOFTWARE\RecordFS
  Result 'drive_letter_kept' $pol.DriveLetter
  Result 'volume_label_kept' $pol.VolumeLabel
  Result 'autoupdate_value' $pol.AutoUpdate
  Result 'updater_state' ((Get-ItemProperty HKLM:\SOFTWARE\RecordFS\Updater -ErrorAction SilentlyContinue | Select-Object * -ExcludeProperty PS* | ConvertTo-Json -Compress))

  Copy-Item 'C:\Program Files\RecordFS\updates\updater.log' "$out\updater.log" -ErrorAction SilentlyContinue
  Copy-Item 'C:\Program Files\RecordFS\updates\install-*.log' $out -ErrorAction SilentlyContinue
  Copy-Item "$env:LOCALAPPDATA\RecordFS\agent.log" "$out\agent.log" -ErrorAction SilentlyContinue
  Result 'updates_dir_acl' (((Get-Acl 'C:\Program Files\RecordFS\updates').Access | ForEach-Object { "$($_.IdentityReference):$($_.FileSystemRights)" }) -join '; ')

  # --- uninstall cleans up after itself -----------------------------------------
  Get-Process recordfs -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
  Start-Sleep 2
  $pc = (Uninstall-Entry).PSChildName
  $p = Start-Process msiexec.exe -ArgumentList '/x', $pc, '/qn', '/norestart', '/l*v', "`"$out\uninstall.log`"" -Wait -PassThru
  Result 'uninstall_exit' $p.ExitCode
  Result 'service_removed' (-not (Get-Service RecordFSUpdate -ErrorAction SilentlyContinue))
  Result 'updates_dir_removed' (-not (Test-Path 'C:\Program Files\RecordFS\updates'))
  Result 'updater_state_removed' (-not (Test-Path HKLM:\SOFTWARE\RecordFS\Updater))
  Result 'run_key_removed' (-not (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name RecordFS -ErrorAction SilentlyContinue))
} catch {
  Log "ERROR $($_.Exception.Message) at $($_.InvocationInfo.PositionMessage)"
} finally {
  $results | ConvertTo-Json | Out-File "$out\results.json" -Encoding utf8
  'done' | Out-File "$out\done.flag"
  Start-Sleep 3
  shutdown /s /t 0
}
