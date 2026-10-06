# Builds both release artifacts into _packages/:
#   Ship-<soh version>-win64-ship-<Version>.zip   (Release build + cpack, same as every past release)
#   Ship-<soh version>-quest-<Version>.apk        (Gradle assembleRelease, arm64-v8a)
#
# Usage (repo root):  powershell -ExecutionPolicy Bypass -File tools\build-release.ps1 -Version 1.4
# Options:            -SkipWindows / -SkipAndroid to build only one of them.
# Logs:               _packages\logs\  (one file per step)
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$SkipWindows,
    [switch]$SkipAndroid
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$packages = Join-Path $root '_packages'
$logs = Join-Path $packages 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null

function Invoke-Step([string]$name, [string]$workDir, [scriptblock]$cmd) {
    $log = Join-Path $logs "$name.log"
    Write-Host "== $name (log: $log)"
    Push-Location $workDir
    # Windows PowerShell turns every stderr line of a native tool (Gradle's javac notes) into an
    # error record; under 'Stop' that aborts the step. Judge the tool by its exit code instead.
    $prevPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $cmd *> $log
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevPref
        Pop-Location
    }
    if ($code -ne 0) {
        Get-Content $log -Tail 30
        throw "$name failed (exit $code). Full log: $log"
    }
}

# soh.o2r goes into both packages; it only changes with soh/assets/custom or the shaders.
$sohO2r = Join-Path $root 'build\x64\soh\soh.o2r'
if (-not (Test-Path $sohO2r)) {
    Invoke-Step 'soh-o2r' $root { cmake --build build/x64 --config Release --target GenerateSohOtr }
}

if (-not $SkipWindows) {
    # Capped parallelism: with many Claude sessions open, full /MP exhausts commit memory.
    Invoke-Step 'windows-build' $root {
        # Quoted: unquoted, PowerShell splits "-nodeReuse:false" at the colon into two arguments.
        cmake --build build/x64 --config Release --target soh --parallel 4 -- '-nodeReuse:false' '-p:CL_MPCount=4'
    }
    $exe = Get-Item (Join-Path $root 'x64\Release\soh.exe')
    if ($exe.LastWriteTime -lt (Get-Date).AddHours(-12)) {
        Write-Warning "x64\Release\soh.exe is from $($exe.LastWriteTime): nothing was relinked (already up to date?)."
    }

    Get-ChildItem $packages -Filter 'Ship-*-win64-ship.zip' | Remove-Item
    Invoke-Step 'windows-cpack' (Join-Path $root 'build\x64') { cpack -C Release }
    $zip = Get-ChildItem $packages -Filter 'Ship-*-win64-ship.zip' | Select-Object -First 1
    if (-not $zip) { throw "cpack ran but produced no Ship-*-win64-ship.zip in $packages" }
    $zipOut = Join-Path $packages ($zip.BaseName + "-$Version.zip")
    Move-Item -Force $zip.FullName $zipOut
    Write-Host "   -> $zipOut"
}

if (-not $SkipAndroid) {
    if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr' }
    $android = Join-Path $root 'Android'
    Invoke-Step 'android-assembleRelease' $android { .\gradlew.bat :app:assembleRelease --console=plain }
    $apk = Join-Path $android 'app\build\outputs\apk\release\app-release.apk'
    if (-not (Test-Path $apk)) { throw "Gradle finished but $apk is missing" }

    # Same "Ship-<soh version>" prefix as the Windows zip (CMake project version).
    $sohVersion = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^project\(Ship VERSION\s+(\d+\.\d+\.\d+)' |
        Select-Object -First 1).Matches[0].Groups[1].Value
    $apkOut = Join-Path $packages "Ship-$sohVersion-quest-$Version.apk"
    Copy-Item -Force $apk $apkOut
    Write-Host "   -> $apkOut"
}

Write-Host "== done"
Get-ChildItem $packages -File -Filter "*-$Version.*" | ForEach-Object {
    '{0,10:N1} MB  {1}' -f ($_.Length / 1MB), $_.Name
}
