# Test run: tools/run_game.ps1 <log> <seconds> [KEY=VALUE ...]
# Starts build/lasrecomp.exe on private/game from the repo root with those environment variables, waits until it
# exits or <seconds> pass, then stops it. stderr goes to <log> (a path, or a name: build/<name>.log).
# Run test sessions from PowerShell: Git Bash with the MSYS2 bin directories first on PATH starts the game
# without the LARECOMP_* variables (no error, they just have no effect).
# Example (intro replay, A every 300 frames, debug frame at 9000):
#   $in = (2500..5500 | ? { ($_ - 2500) % 300 -eq 0 } | % { "${_}:A:8" }) -join ","
#   tools/run_game.ps1 intro 300 "LARECOMP_INPUT=$in" LARECOMP_DEBUG_FRAME=9000 LARECOMP_WINDOW_SHOT=9005 LARECOMP_EXIT_AFTER_SHOT=1
param([string]$Log, [int]$Seconds, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Vars)
$root = Split-Path $PSScriptRoot -Parent
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
$env:LARECOMP_NO_LAUNCHER = "1"
foreach ($v in $Vars) { $k, $val = $v -split '=', 2; Set-Item -Path "env:$k" -Value $val }
if (-not [System.IO.Path]::IsPathRooted($Log) -and $Log -notmatch '[\\/]') { $Log = Join-Path $root "build\$Log.log" }
Start-Process -FilePath (Join-Path $root "build\lasrecomp.exe") -ArgumentList "--play", "private/game" `
    -WorkingDirectory $root -RedirectStandardError $Log -NoNewWindow | Out-Null
# (the game may re-execute itself for a free guest address range: wait by process name, not by the first PID)
$t = 0
while ($t -lt $Seconds) {
    Start-Sleep 2; $t += 2
    if (-not (Get-Process lasrecomp -ErrorAction SilentlyContinue)) { break }
}
Get-Process lasrecomp -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
"ran $t s, log $Log"
