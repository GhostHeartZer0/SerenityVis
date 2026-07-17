# Silicon Phase 5 Integration Test Suite
# Runs compilation, checks native server socket binding, and verifies DOM elements.

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Resolve-Path (Join-Path $ScriptDir "..")
$HtmlPath = Join-Path $ProjectDir "SerenityVis.html"
$ExePath = Join-Path $ProjectDir "AudioServer.exe"
$CppPath = Join-Path $ProjectDir "AudioServer\AudioServer.cpp"
$BuildBatPath = Join-Path $ProjectDir "build.bat"

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "     RUNNING SILICON PHASE 5 INTEGRATION TESTS     " -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

# 1. Verify HTML DOM Elements
Write-Host "[1/4] Verifying HTML DOM Elements..." -ForegroundColor Yellow
if (-not (Test-Path $HtmlPath)) {
    throw "SerenityVis.html not found!"
}
$HtmlContent = Get-Content $HtmlPath -Raw

$RequiredElements = @(
    "id=`"gpuCanvas`"",
    "id=`"telemetryCanvas`"",
    "id=`"settings-save-top-btn`"",
    "id=`"settings-save-bottom-btn`"",
    "id=`"settings-close-btn`"",
    "id=`"ctrl-show-telemetry`"",
    "id=`"ctrl-async-compute`"",
    "id=`"ctrl-video-mask`"",
    "id=`"ctrl-gpu-noise`"",
    "id=`"ctrl-cpu-buoyancy`"",
    "id=`"ctrl-audio-sensitivity`"",
    "id=`"ctrl-audio-threshold`"",
    "id=`"btn-auto-adjust`"",
    "id=`"ctrl-dynamic-thermo`"",
    "id=`"ctrl-show-overlays`"",
    "id=`"ctrl-living-bg`"",
    "id=`"btn-fullscreen`""
)

foreach ($Element in $RequiredElements) {
    if ($HtmlContent -match $Element) {
        Write-Host "  [OK] Found element: $Element" -ForegroundColor Green
    }
    else {
        throw "Missing critical HTML element: $Element"
    }
}

# 2. Check CSS Collision Fix
Write-Host "`n[2/4] Verifying CSS Selector Collision Fix..." -ForegroundColor Yellow
if ($HtmlContent -match "#gpuCanvas\s*\{\s*position:\s*absolute;") {
    Write-Host "  [OK] Found isolated #gpuCanvas styling." -ForegroundColor Green
}
else {
    throw "Generic 'canvas' styling collision might still exist. Verify #gpuCanvas selector is present."
}

# 3. Test C++ Server Compilation
Write-Host "`n[3/4] Testing Native Compilation..." -ForegroundColor Yellow
if (-not (Test-Path $BuildBatPath)) {
    throw "build.bat not found!"
}
& $BuildBatPath
if ($LASTEXITCODE -eq 0 -and (Test-Path $ExePath)) {
    Write-Host "  [OK] Native server compiled successfully." -ForegroundColor Green
}
else {
    throw "Native compilation failed."
}

# 4. Verify Server Socket Binding & Telemetry
Write-Host "`n[4/4] Verifying Server Port Binding..." -ForegroundColor Yellow

# Kill any existing server tasks to prevent port collision
Get-Process | Where-Object { $_.Name -eq "AudioServer" } | ForEach-Object {
    Stop-Process $_.Id -Force -ErrorAction SilentlyContinue
}

# Start AudioServer.exe in the background
$ProcessInfo = New-Object System.Diagnostics.ProcessStartInfo
$ProcessInfo.FileName = $ExePath
$ProcessInfo.WorkingDirectory = $ProjectDir
$ProcessInfo.RedirectStandardInput = $true
$ProcessInfo.RedirectStandardOutput = $true
$ProcessInfo.UseShellExecute = $false
$Process = [System.Diagnostics.Process]::Start($ProcessInfo)

# Send Choice 2 (System Audio) and 1 (Normal Focus) to standard input
Start-Sleep -Milliseconds 500
$Process.StandardInput.WriteLine("2")
Start-Sleep -Milliseconds 200
$Process.StandardInput.WriteLine("1")
Start-Sleep -Seconds 2

# Check TCP port 8080 binding
$Connections = Get-NetTCPConnection -LocalPort 8080 -State Listen -ErrorAction SilentlyContinue
if ($Connections) {
    Write-Host "  [OK] AudioServer successfully listening on ws://localhost:8080" -ForegroundColor Green
}
else {
    $Process.Kill()
    throw "AudioServer failed to bind to port 8080."
}

# Shutdown background process cleanly
$Process.Kill()
Write-Host "  [OK] Server cleaned up successfully." -ForegroundColor Green

Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "         ALL INTEGRATION TESTS PASSED            " -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Cyan
