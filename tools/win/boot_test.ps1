# Starts the PC build for a few seconds, stops it, and prints the end of its newest log plus what it reached.
# With -VideoAt N it also records a video from N seconds on (peggle1_video_s) and makes the MP4 (tools/make_video.sh).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 [-Seconds 45] [-Exe out\pc\peggle1.exe]
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -VideoAt 10 -VideoFrames 600
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -VideoAt 20 -VideoFrames 1200 -InputScript "25s:A,30s:A"
param(
    [int]$Seconds = 45,
    [string]$Exe = 'out\pc\peggle1.exe',
    [int]$Tail = 40,
    [int]$VideoAt = 0,
    [int]$VideoFrames = 300,
    # Peggle 2 presents 30 frames per second (D3DDevice_Swap, measured on the first boot); the run must last long
    # enough for the recording.
    [int]$GameFps = 30,
    # Scripted controller (peggle1_input_script, app/src/peggle1_input_script.h), e.g. "25s:A,30s:A,35s:A".
    [string]$InputScript = '',
    # Record from this game frame on instead of -VideoAt seconds (peggle1_video_from_frame): runs with the same input
    # script record the same game frames, for tools/compare_raw.py.
    [int]$VideoFromFrame = 0
)
if ($InputScript) { $env:REX_PEGGLE1_INPUT_SCRIPT = $InputScript } else { Remove-Item env:REX_PEGGLE1_INPUT_SCRIPT -ErrorAction SilentlyContinue }
if ($VideoFromFrame -gt 0) {
    $env:REX_PEGGLE1_VIDEO_FROM_FRAME = "$VideoFromFrame"
    $env:REX_PEGGLE1_VIDEO_FRAMES = "$VideoFrames"
    $Seconds = [math]::Max($Seconds, [int][math]::Ceiling(($VideoFromFrame + $VideoFrames) / [double]$GameFps * 1.25) + 60)
} else { $env:REX_PEGGLE1_VIDEO_FROM_FRAME = $null }
if ($VideoAt -gt 0) {
    $env:REX_PEGGLE1_VIDEO_S = "$VideoAt"
    $env:REX_PEGGLE1_VIDEO_FRAMES = "$VideoFrames"
    # Room for the recording (at the game's frame rate) and for writing it out before the run is stopped.
    $Seconds = [math]::Max($Seconds, $VideoAt + [int][math]::Ceiling($VideoFrames / [double]$GameFps) + 15)
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exePath = (Resolve-Path (Join-Path $root $Exe)).Path
$dir = Split-Path $exePath
# A test instance left over from an interrupted run keeps the exe locked (the next link fails with "permission
# denied") and competes for the GPU: stop instances of this very executable only (other trees are left alone).
Get-CimInstance Win32_Process -Filter "Name='$(Split-Path $exePath -Leaf)'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $exePath } | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $_.ProcessId -Timeout 20 -ErrorAction SilentlyContinue
    }
$started = Get-Date
# Test runs are always muted and minimized (environment beats peggle1.toml), so they never take over the desktop.
$env:REX_AUDIO_MUTE = 'true'
$env:REX_WINDOW_START_MINIMIZED = 'true'
$p = Start-Process -FilePath $exePath -WorkingDirectory $dir -PassThru -WindowStyle Minimized
$p.WaitForExit($Seconds * 1000) | Out-Null
if ($p.HasExited) {
    '{0} exited after {1:n1} s with code 0x{2:X8}' -f (Split-Path $exePath -Leaf), ((Get-Date) - $started).TotalSeconds, $p.ExitCode
} else {
    $p.Refresh()
    $status = 'still running after {0} s: cpu {1:n1} s, working set {2} MB; stopped' -f $Seconds, $p.TotalProcessorTime.TotalSeconds, [math]::Round($p.WorkingSet64 / 1MB)
    # Stop it BEFORE printing anything: a caller that pipes this script into Select-Object -First N closes the pipe
    # after its first line, which ends this script there and would leave the game running.
    Stop-Process -Id $p.Id -Force
    # Wait until it is really gone: a process still tearing down keeps peggle1.exe locked and the next link fails.
    if (-not $p.WaitForExit(20000)) { $status += " (warning: process $($p.Id) did not exit within 20 s)" }
    $status
}
$log = Get-ChildItem (Join-Path $dir 'logs') -Filter *.log -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) {
    "== $($log.FullName) ($($log.Length) bytes)"
    Get-Content $log.FullName -Tail $Tail
} else {
    'no log written'
}
if ($VideoAt -gt 0 -or $VideoFromFrame -gt 0) {
    $env:REX_PEGGLE1_VIDEO_S = $null; $env:REX_PEGGLE1_VIDEO_FRAMES = $null; $env:REX_PEGGLE1_VIDEO_FROM_FRAME = $null
    $raw = if ($VideoFromFrame -gt 0) { Join-Path $dir "captures\video_f${VideoFromFrame}.rgba" } else { Join-Path $dir "captures\video_${VideoAt}s.rgba" }
    if ((Test-Path $raw) -and (Get-Item $raw).LastWriteTime -ge $started) {
        $bash = 'C:\devkitPro\msys2\usr\bin\bash.exe'
        if (-not (Test-Path $bash)) { $bash = 'bash' }
        # 16 frames on the contact sheet, spread over the whole recording.
        $step = [math]::Max(1, [math]::Floor($VideoFrames / 16))
        & $bash (Join-Path $root 'tools/make_video.sh').Replace('\', '/') $raw.Replace('\', '/') 16 $step
    } else {
        "no video written to $raw (see the log for [capture] lines)"
    }
}
