param(
    [ValidateRange(1, 10)]
    [int]$RunsPerPreset = 3,
    [string]$ReleaseDirectory = (Join-Path $PSScriptRoot 'x64\Release')
)

$ErrorActionPreference = 'Stop'
$releaseDirectory = (Resolve-Path -LiteralPath $ReleaseDirectory).Path
$executable = Join-Path $releaseDirectory 'soh.exe'
$resultsDirectory = Join-Path $releaseDirectory 'benchmarks'

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Shipwright executable not found: $executable"
}

New-Item -ItemType Directory -Path $resultsDirectory -Force | Out-Null

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $middle = [math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

$records = [System.Collections.Generic.List[object]]::new()
$suiteStarted = Get-Date

Write-Host ''
Write-Host 'VR BENCHMARK SUITE'
Write-Host "Runs: $RunsPerPreset baseline + $RunsPerPreset optimized"
Write-Host 'Keep the headset worn and focused until all runs finish.'
Write-Host ''

foreach ($preset in @('baseline', 'optimized')) {
    for ($run = 1; $run -le $RunsPerPreset; $run++) {
        Write-Host "[$preset $run/$RunsPerPreset] Starting Hyrule Field introduction..."
        $started = Get-Date
        $process = Start-Process -FilePath $executable `
                                 -ArgumentList '--vr-benchmark', '--benchmark-name', $preset `
                                 -WorkingDirectory $releaseDirectory `
                                 -Wait -PassThru

        $summaryFile = Get-ChildItem -LiteralPath $resultsDirectory -Filter "*_${preset}-summary.json" |
            Where-Object { $_.LastWriteTime -ge $started } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -eq $summaryFile) {
            throw "Benchmark $preset run $run exited with code $($process.ExitCode) without producing a new summary."
        }
        if ($process.ExitCode -ne 0) {
            Write-Warning "Benchmark $preset run $run produced a complete summary but exited with code $($process.ExitCode); keeping the result."
        }

        $summary = Get-Content -LiteralPath $summaryFile.FullName -Raw | ConvertFrom-Json
        $records.Add([pscustomobject]@{
            preset = $preset
            run = $run
            file = $summaryFile.Name
            cadence_valid = [bool]$summary.cadence_valid
            target_rate_frame_percent = [double]$summary.target_rate_frame_percent
            average_submitted_fps = [double]$summary.average_submitted_fps
            one_percent_low_submitted_fps = [double]$summary.one_percent_low_submitted_fps
            frame_interval_p99_ms = [double]$summary.frame_interval_p99_ms
            xr_wait_average_ms = [double]$summary.xr_wait_average_ms
            xr_wait_p99_ms = [double]$summary.xr_wait_p99_ms
            cpu_active_average_ms = [double]$summary.cpu_active_average_ms
            cpu_active_p99_ms = [double]$summary.cpu_active_p99_ms
            gpu_eyes_average_ms = [double]$summary.gpu_eyes_average_ms
            gpu_eyes_p99_ms = [double]$summary.gpu_eyes_p99_ms
            gpu_total_average_ms = [double]$summary.gpu_total_average_ms
            gpu_total_p99_ms = [double]$summary.gpu_total_p99_ms
            cpu_total_average_ms = [double]$summary.cpu_total_average_ms
            cpu_total_p99_ms = [double]$summary.cpu_total_p99_ms
            cpu_eyes_average_ms = [double]$summary.cpu_eyes_average_ms
            cpu_eyes_p99_ms = [double]$summary.cpu_eyes_p99_ms
        })
        Write-Host ("[{0} {1}/{2}] Complete: {3:N2} FPS, eye CPU {4:N2} ms, cadence {5}" -f `
            $preset, $run, $RunsPerPreset, $summary.average_submitted_fps, $summary.cpu_eyes_average_ms, `
            $(if ($summary.cadence_valid) { 'VALID' } else { 'INVALID' }))
    }
}

$aggregates = foreach ($preset in @('baseline', 'optimized')) {
    $set = @($records | Where-Object preset -eq $preset)
    [pscustomobject]@{
        preset = $preset
        runs = $set.Count
        valid_cadence_runs = @($set | Where-Object cadence_valid).Count
        median_target_rate_frame_percent = Get-Median @($set.target_rate_frame_percent)
        median_submitted_fps = Get-Median @($set.average_submitted_fps)
        median_one_percent_low_fps = Get-Median @($set.one_percent_low_submitted_fps)
        median_frame_interval_p99_ms = Get-Median @($set.frame_interval_p99_ms)
        median_xr_wait_average_ms = Get-Median @($set.xr_wait_average_ms)
        median_xr_wait_p99_ms = Get-Median @($set.xr_wait_p99_ms)
        median_cpu_active_average_ms = Get-Median @($set.cpu_active_average_ms)
        median_cpu_active_p99_ms = Get-Median @($set.cpu_active_p99_ms)
        median_gpu_eyes_average_ms = Get-Median @($set.gpu_eyes_average_ms)
        median_gpu_eyes_p99_ms = Get-Median @($set.gpu_eyes_p99_ms)
        median_gpu_total_average_ms = Get-Median @($set.gpu_total_average_ms)
        median_gpu_total_p99_ms = Get-Median @($set.gpu_total_p99_ms)
        median_cpu_total_average_ms = Get-Median @($set.cpu_total_average_ms)
        median_cpu_total_p99_ms = Get-Median @($set.cpu_total_p99_ms)
        median_cpu_eyes_average_ms = Get-Median @($set.cpu_eyes_average_ms)
        median_cpu_eyes_p99_ms = Get-Median @($set.cpu_eyes_p99_ms)
    }
}

$baseline = $aggregates | Where-Object preset -eq 'baseline'
$optimized = $aggregates | Where-Object preset -eq 'optimized'
function Percent-Change([double]$Before, [double]$After) {
    if ([math]::Abs($Before) -lt 0.000001) { return 0.0 }
    return (($After - $Before) / $Before) * 100.0
}

$comparison = [pscustomobject]@{
    submitted_fps_percent = Percent-Change $baseline.median_submitted_fps $optimized.median_submitted_fps
    one_percent_low_fps_percent = Percent-Change $baseline.median_one_percent_low_fps $optimized.median_one_percent_low_fps
    frame_interval_p99_percent = Percent-Change $baseline.median_frame_interval_p99_ms $optimized.median_frame_interval_p99_ms
    cpu_active_average_percent = Percent-Change $baseline.median_cpu_active_average_ms $optimized.median_cpu_active_average_ms
    cpu_active_p99_percent = Percent-Change $baseline.median_cpu_active_p99_ms $optimized.median_cpu_active_p99_ms
    gpu_eyes_average_percent = Percent-Change $baseline.median_gpu_eyes_average_ms $optimized.median_gpu_eyes_average_ms
    gpu_total_average_percent = Percent-Change $baseline.median_gpu_total_average_ms $optimized.median_gpu_total_average_ms
    cpu_total_average_percent = Percent-Change $baseline.median_cpu_total_average_ms $optimized.median_cpu_total_average_ms
    cpu_total_p99_percent = Percent-Change $baseline.median_cpu_total_p99_ms $optimized.median_cpu_total_p99_ms
    cpu_eyes_average_percent = Percent-Change $baseline.median_cpu_eyes_average_ms $optimized.median_cpu_eyes_average_ms
    cpu_eyes_p99_percent = Percent-Change $baseline.median_cpu_eyes_p99_ms $optimized.median_cpu_eyes_p99_ms
}

$suite = [pscustomobject]@{
    started = $suiteStarted.ToString('o')
    completed = (Get-Date).ToString('o')
    runs_per_preset = $RunsPerPreset
    runs = $records
    medians = $aggregates
    optimized_change_percent = $comparison
}
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$suiteFile = Join-Path $resultsDirectory "suite-$timestamp-summary.json"
$suite | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $suiteFile -Encoding UTF8

Write-Host ''
Write-Host 'MEDIAN RESULTS'
$aggregates | Format-Table preset, valid_cadence_runs, median_submitted_fps, median_one_percent_low_fps, `
    median_cpu_eyes_average_ms, median_cpu_eyes_p99_ms -AutoSize
Write-Host "Suite summary: $suiteFile"

if ($aggregates.valid_cadence_runs -contains 0) {
    [System.Media.SystemSounds]::Exclamation.Play()
    Write-Warning 'At least one preset had no cadence-valid runs. Inspect target-rate percentages before comparing.'
} else {
    1..3 | ForEach-Object {
        [System.Media.SystemSounds]::Asterisk.Play()
        Start-Sleep -Milliseconds 250
    }
}
