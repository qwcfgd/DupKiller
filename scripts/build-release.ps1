param(
    [ValidateSet('5','6','Both')][string]$Qt = 'Both',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$taskProjectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $taskProjectRoot
try {
    $taskVersions = if ($Qt -eq 'Both') { @('5','6') } else { @($Qt) }
    foreach ($taskVersion in $taskVersions) {
        $taskPreset = "qt$taskVersion-release"
        $taskKit = if ($taskVersion -eq '5') { 'C:\Qt\5.15.19\mingw810_64' } else { 'C:\Qt\6.8.4\mingw1200_64' }
        $taskCompiler = if ($taskVersion -eq '5') { 'C:\MinGW\8.1.0\mingw64\bin' } else { 'C:\MinGW\12.0.0\mingw64\bin' }
        $taskOldPath = $env:PATH
        try {
            $env:PATH = "$taskKit\bin;$taskCompiler;$taskOldPath"
            cmake --preset $taskPreset
            if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $taskPreset" }
            cmake --build --preset $taskPreset
            if ($LASTEXITCODE -ne 0) { throw "CMake build failed: $taskPreset" }
            if (-not $SkipTests) {
                ctest --preset $taskPreset
                if ($LASTEXITCODE -ne 0) { throw "Tests failed. See ../build/QtDupKiller-qt$taskVersion/test-results.txt" }
            }
            $taskOutput = [IO.Path]::GetFullPath((Join-Path $taskProjectRoot "..\build\QtDupKiller-qt$taskVersion"))
            & "$taskKit\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --compiler-runtime (Join-Path $taskOutput 'QtDupKiller.exe')
            if ($LASTEXITCODE -ne 0) { throw "windeployqt failed: $taskPreset" }
            # These local Qt builds do not include MinGW runtime DLLs in their bin folders.
            foreach ($taskLibrary in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
                $taskRuntimeSource = Join-Path $taskCompiler $taskLibrary
                $taskRuntimeDestination = Join-Path $taskOutput $taskLibrary
                # An unchanged runtime may be loaded by a running task. Avoid reopening it for writing.
                if (-not (Test-Path -LiteralPath $taskRuntimeDestination) -or
                    (Get-FileHash -LiteralPath $taskRuntimeSource -Algorithm SHA256).Hash -ne
                    (Get-FileHash -LiteralPath $taskRuntimeDestination -Algorithm SHA256).Hash) {
                    Copy-Item -LiteralPath $taskRuntimeSource -Destination $taskRuntimeDestination -Force
                }
            }
            Copy-Item -LiteralPath (Join-Path $taskProjectRoot 'README.md') -Destination $taskOutput -Force
            Copy-Item -LiteralPath (Join-Path $taskProjectRoot 'LICENSE') -Destination $taskOutput -Force
            Write-Output "Ready: $taskOutput\QtDupKiller.exe"
        } finally { $env:PATH = $taskOldPath }
    }
} finally { Pop-Location }
