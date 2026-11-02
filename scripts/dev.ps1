param(
    [ValidateSet('configure', 'build', 'test', 'check')]
    [string]$Action = 'build',
    [string[]]$Targets = @(),
    [string]$Preset = 'dev',
    [string]$QtDir = '',
    [int]$Jobs = 3,
    [string]$TestFilter = ''
)

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo "build\$Preset"

(Get-Process -Id $PID).PriorityClass = 'BelowNormal'

$lock = Join-Path $repo 'build\.dev-lock'
New-Item -ItemType Directory -Force (Join-Path $repo 'build') | Out-Null
$waited = 0
while ($true) {
    try {
        New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null
        break
    } catch {
        $age = (Get-Date) - (Get-Item $lock).LastWriteTime
        if ($age.TotalMinutes -gt 45) { Remove-Item $lock -Force -Recurse -ErrorAction SilentlyContinue; continue }
        Start-Sleep -Seconds 3
        $waited += 3
        if ($waited -gt 1800) { throw 'Timed out waiting for another build' }
    }
}

try {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $envFile = Join-Path $repo 'build\.vcvars-env.txt'
    if (-not (Test-Path $envFile) -or ((Get-Date) - (Get-Item $envFile).LastWriteTime).TotalHours -gt 24) {
        cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | Set-Content -Encoding utf8 $envFile
    }
    Get-Content $envFile | ForEach-Object {
        $parts = $_ -split '=', 2
        if ($parts.Count -eq 2 -and $parts[0] -notmatch '^(PROMPT|=)') { Set-Item -Path "env:$($parts[0])" -Value $parts[1] }
    }

    if (-not $QtDir) {
        $QtDir = (Get-ChildItem 'C:\Qt' -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and (Test-Path "$($_.FullName)\msvc2022_64\bin\qmake.exe") } |
            Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).FullName
        if ($QtDir) { $QtDir = "$QtDir\msvc2022_64" }
    }
    if ($QtDir) {
        $env:CMAKE_PREFIX_PATH = $QtDir
        $env:PATH = "$QtDir\bin;$env:PATH"
    }

    if ($Action -eq 'configure' -or -not (Test-Path "$buildDir\build.ninja")) {
        cmake --preset $Preset 2>&1 | Select-Object -Last 15
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if ($Action -eq 'configure') { exit 0 }
    }

    $buildArgs = @('--build', $buildDir, '--parallel', "$Jobs")
    if ($Targets.Count -gt 0) { $buildArgs += '--target'; $buildArgs += $Targets }
    if ($Action -in @('build', 'test', 'check')) {
        $output = & cmake @buildArgs 2>&1
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            $output | Select-String -Pattern 'error|FAILED|fatal' -CaseSensitive:$false | Select-Object -First 40 | ForEach-Object { $_.Line }
            exit $code
        }
        $output | Select-Object -Last 2
    }
    if ($Action -in @('test', 'check')) {
        $env:QT_QPA_PLATFORM = 'offscreen'
        $testArgs = @('--test-dir', $buildDir, '--output-on-failure', '--timeout', '600')
        if ($TestFilter) { $testArgs += '-R'; $testArgs += $TestFilter }
        & ctest @testArgs 2>&1 | Select-Object -Last 40
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
} finally {
    Remove-Item $lock -Force -Recurse -ErrorAction SilentlyContinue
}
