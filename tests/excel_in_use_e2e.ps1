# Real-application check of cross-machine "file in use" (protocol 4.5):
# Excel on mount A opens a workbook for editing; Excel on mount B -- a
# different session, i.e. a different machine as far as the server knows --
# must get it READ-ONLY, see who holds it, and get write access once A closes.
#
#   powershell -File tests\excel_in_use_e2e.ps1 -A S: -B T: -Record "<record dir>" -File book.xlsx
#
# Needs two mounts of a lease-capable server and a workbook both can open.
# Excel instances are invisible (DisplayAlerts off: the "File in Use" prompt
# resolves to its default, Read-Only). Only Excel processes this script
# started are ever stopped.
param(
  [string]$A = "S:",
  [string]$B = "T:",
  [string]$Table = "workorders",
  [Parameter(Mandatory)] [string]$Record,
  [Parameter(Mandatory)] [string]$File,
  [string]$ExpectA1 = ""     # optional: what cell A1 of the first sheet holds
)

$pa = "$A\$Table\$Record\$File"
$pb = "$B\$Table\$Record\$File"
$owner = "$B\$Table\$Record\~`$$File"
$pre = @(Get-Process EXCEL -ErrorAction SilentlyContinue | ForEach-Object Id)
$fail = 0
function Check($name, $cond, $detail = "") {
  if ($cond) { "PASS $name" } else { "FAIL $name  -- $detail"; $script:fail++ }
}

$job = Start-Job -ArgumentList $pa, $pb, $owner -ScriptBlock {
  param($pa, $pb, $owner)
  $r = [ordered]@{}
  $x1 = New-Object -ComObject Excel.Application; $x1.Visible = $false; $x1.DisplayAlerts = $false
  $x2 = New-Object -ComObject Excel.Application; $x2.Visible = $false; $x2.DisplayAlerts = $false
  try {
    $w1 = $x1.Workbooks.Open($pa)
    $r.a_readonly = $w1.ReadOnly
    Start-Sleep -Milliseconds 500
    $w2 = $x2.Workbooks.Open($pb)
    $r.b_readonly = $w2.ReadOnly
    $r.b_a1 = [string]$w2.Worksheets.Item(1).Range("A1").Value2
    if (Test-Path -LiteralPath $owner) {
      $bytes = [IO.File]::ReadAllBytes($owner)
      $r.owner = [Text.Encoding]::ASCII.GetString($bytes, 1, [Math]::Min($bytes[0], $bytes.Length - 1)).Trim()
    } else { $r.owner = "" }
    $w1.Close($false)
    Start-Sleep -Milliseconds 500
    try { $w2.ChangeFileAccess(2) | Out-Null; $r.b_after = $w2.ReadOnly } catch { $r.b_after = "error: $_" }
    $w2.Close($false)
  } catch { $r.error = "$_" }
  finally { $x1.Quit(); $x2.Quit() }
  [pscustomobject]$r
}
if (-not (Wait-Job $job -Timeout 150)) { "FAIL timed out (an Excel prompt?)"; $fail++ }
$res = Receive-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force
Start-Sleep -Seconds 3
Get-Process EXCEL -ErrorAction SilentlyContinue | Where-Object { $pre -notcontains $_.Id } | Stop-Process -Force

if ($res) {
  if ($res.error) { "FAIL excel error -- $($res.error)"; $fail++ }
  Check "A_opens_read_write" ($res.a_readonly -eq $false) $res.a_readonly
  Check "B_gets_read_only_while_A_edits" ($res.b_readonly -eq $true) $res.b_readonly
  if ($ExpectA1) { Check "B_reads_the_content" ($res.b_a1 -eq $ExpectA1) $res.b_a1 }
  Check "B_sees_who_holds_it" ($res.owner.Length -gt 0) "owner file empty/absent"
  "     (owner file names: '$($res.owner)')"
  Check "B_gets_write_access_after_A_closes" ($res.b_after -eq $false) $res.b_after
}
""
if ($fail) { "$fail failed"; exit 1 } else { "all passed"; exit 0 }
