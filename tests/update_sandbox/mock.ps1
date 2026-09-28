# SPDX-License-Identifier: MIT
# Minimal GitHub-release-shaped feed for the sandbox test (Windows PowerShell
# 5.1, no Python in a fresh sandbox). Serves /v<version>/latest and
# /assets/<name> for every RecordFS-<version>.msi in -Dir.
param([string]$Dir, [int]$Port = 8765)

$l = [System.Net.HttpListener]::new()
$l.Prefixes.Add("http://127.0.0.1:$Port/")
$l.Start()
while ($l.IsListening) {
  $ctx = $l.GetContext()
  $res = $ctx.Response
  try {
    $path = $ctx.Request.Url.AbsolutePath
    if ($path -match '^/v(\d+\.\d+\.\d+)/latest$') {
      $v = $Matches[1]
      $f = Join-Path $Dir "RecordFS-$v.msi"
      if (Test-Path $f) {
        $h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
        $obj = [ordered]@{
          tag_name = "v$v"; draft = $false; prerelease = $false
          assets = @([ordered]@{
            name = "RecordFS-$v.msi"; size = (Get-Item $f).Length; digest = "sha256:$h"
            browser_download_url = "http://127.0.0.1:$Port/assets/RecordFS-$v.msi" })
        }
        $bytes = [Text.Encoding]::UTF8.GetBytes(($obj | ConvertTo-Json -Depth 5))
        $res.ContentType = 'application/json'
      } else {
        $res.StatusCode = 404
        $bytes = [Text.Encoding]::UTF8.GetBytes('{"message":"Not Found"}')
      }
      $res.ContentLength64 = $bytes.Length
      $res.OutputStream.Write($bytes, 0, $bytes.Length)
    } elseif ($path -match '^/assets/(.+)$') {
      $f = Join-Path $Dir ([IO.Path]::GetFileName($Matches[1]))
      if (Test-Path $f) {
        $res.ContentType = 'application/octet-stream'
        $res.ContentLength64 = (Get-Item $f).Length
        $fs = [IO.File]::OpenRead($f)
        $fs.CopyTo($res.OutputStream)
        $fs.Close()
      } else {
        $res.StatusCode = 404
      }
    } else {
      $res.StatusCode = 404
    }
  } catch {
  } finally {
    $res.Close()
  }
}
