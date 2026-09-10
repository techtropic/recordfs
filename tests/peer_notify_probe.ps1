# Does a PEER's save now reach a directory watcher, and does a reopen see the
# new bytes? Includes a local-disk control so a silent watcher bug cannot pass.
$rec = (Get-ChildItem S:\workorders | Where-Object Name -like "*Lot 22*" | Select-Object -First 1).FullName
$py  = "D:\Python311\python.exe"
$peer = "C:\Users\marcm\AppData\Local\Temp\claude\D--workspace-scheduler\7629f68e-4574-47b4-9c80-ce40da95400e\scratchpad\fsdtest\peer_change.py"
$p = Join-Path $rec "peerwatch.txt"

"seeding via the mount..."
Set-Content $p "seed-content" -NoNewline
Start-Sleep -Seconds 3
"seed size: $((Get-Item $p).Length)"

# keep the directory 'active' so the pump watches it, then start watching
Get-ChildItem $rec | Out-Null
Get-EventSubscriber | Unregister-Event -ErrorAction SilentlyContinue
Get-Event | Remove-Event -ErrorAction SilentlyContinue

$fsw = New-Object System.IO.FileSystemWatcher
$fsw.Path = $rec
$fsw.NotifyFilter = [System.IO.NotifyFilters]::FileName -bor `
                    [System.IO.NotifyFilters]::LastWrite -bor `
                    [System.IO.NotifyFilters]::Size
Register-ObjectEvent $fsw Changed -SourceIdentifier PeerChanged | Out-Null
Register-ObjectEvent $fsw Created -SourceIdentifier PeerCreated | Out-Null
Register-ObjectEvent $fsw Deleted -SourceIdentifier PeerDeleted | Out-Null
$fsw.EnableRaisingEvents = $true
Start-Sleep -Seconds 1

"peer replaces the content over the wire (never touches this machine's FS)..."
& $py $peer 2>&1 | ForEach-Object { "   $_" }

Start-Sleep -Seconds 14
$fsw.EnableRaisingEvents = $false
$evts = @(Get-Event -ErrorAction SilentlyContinue)
"watcher events from the PEER change: $($evts.Count)"
$evts | Select-Object -First 6 | ForEach-Object { "   $($_.SourceIdentifier) $($_.SourceEventArgs.Name)" }

"size after reopen: $((Get-Item $p).Length)"
$content = Get-Content $p -Raw
"reopen sees peer content: $($content.StartsWith('PEER REPLACED'))"

Get-EventSubscriber | Unregister-Event -ErrorAction SilentlyContinue
Get-Event | Remove-Event -ErrorAction SilentlyContinue
Remove-Item $p -Force -ErrorAction SilentlyContinue
