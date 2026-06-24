param(
    [switch]$Release,
    [switch]$Python,
    [switch]$Help,
    [string]$PythonExecutable = $env:PYTHON_SYS_EXECUTABLE
)

$ErrorActionPreference = "Stop"

function Write-Usage {
    @"
Usage: scripts/build-native-windows-arm64.ps1 [-Release] [-Python] [-PythonExecutable <path>] [-Help]

Build run-manager and a native psyche-solana-client for Windows ARM64 CPU
development. This helper does not enable DirectML, NPU, or GPU acceleration.
All native builds require a Python environment that can import torch because
the Rust client links against libtorch through PyTorch.
The Rust target is always aarch64-pc-windows-msvc.

Options:
  -Release           Build release binaries.
  -Python            Build the client with the python feature for HfAuto runs.
  -PythonExecutable  Python executable with torch installed.
  -Help              Show this help text.
"@
}

function ConvertTo-PowerShellSingleQuotedLiteral {
    param([string]$Value)
    return "'" + $Value.Replace("'", "''") + "'"
}

if ($PSBoundParameters.ContainsKey("Help")) {
    Write-Usage
    exit 0
}

$isWindows = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
    [System.Runtime.InteropServices.OSPlatform]::Windows
)
if (-not $isWindows) {
    throw "This helper must be run from Windows ARM64."
}

$osArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($osArch -ne [System.Runtime.InteropServices.Architecture]::Arm64) {
    throw "This helper must be run from native Windows ARM64. Current OS architecture: $osArch"
}

$rustTarget = "aarch64-pc-windows-msvc"
$rustupCommand = Get-Command rustup -ErrorAction SilentlyContinue
if ($null -eq $rustupCommand) {
    throw "rustup not found. Install Rust with rustup and add target $rustTarget."
}
$installedTargets = & rustup target list --installed 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "rustup target list failed. Install Rust with rustup and add target $rustTarget."
}
if ($installedTargets -notcontains $rustTarget) {
    throw "Rust target $rustTarget is not installed. Run: rustup target add $rustTarget"
}

if ([string]::IsNullOrWhiteSpace($PythonExecutable)) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $pythonCommand) {
        throw "python not found; pass -PythonExecutable or set PYTHON_SYS_EXECUTABLE"
    }
    $PythonExecutable = $pythonCommand.Source
}

$pythonInfoMarker = "PSYCHE_NATIVE_WINDOWS_ARM64_INFO="
$pythonInfoOutput = & $PythonExecutable -c @"
import json
import pathlib
import sys
import torch

print("$pythonInfoMarker" + json.dumps({
    "version": f"{sys.version_info.major}.{sys.version_info.minor}",
    "torch_lib": str(pathlib.Path(torch.__file__).parent / "lib"),
    "python_dir": str(pathlib.Path(sys.executable).parent),
    "machine": getattr(__import__("platform"), "machine")(),
}))
"@

if ($LASTEXITCODE -ne 0) {
    throw "Failed to import torch with $PythonExecutable. Install PyTorch for that Python or pass -PythonExecutable."
}

$pythonInfoLine = $pythonInfoOutput |
    Where-Object { $_ -like "$pythonInfoMarker*" } |
    Select-Object -Last 1
if ([string]::IsNullOrWhiteSpace($pythonInfoLine)) {
    throw "Could not parse Python/PyTorch information from $PythonExecutable."
}

$pythonInfoJson = $pythonInfoLine.Substring($pythonInfoMarker.Length)
$pythonInfo = $pythonInfoJson | ConvertFrom-Json
if ($pythonInfo.machine -notmatch "ARM64|AARCH64") {
    throw "Python reports machine '$($pythonInfo.machine)'. Install native Windows ARM64 Python with torch, or pass -PythonExecutable for one."
}

$env:PYTHON_SYS_EXECUTABLE = $PythonExecutable
if ([string]::IsNullOrWhiteSpace($env:LIBTORCH_USE_PYTORCH)) {
    $env:LIBTORCH_USE_PYTORCH = "1"
}
if ([string]::IsNullOrWhiteSpace($env:LIBTORCH_BYPASS_VERSION_CHECK)) {
    $env:LIBTORCH_BYPASS_VERSION_CHECK = "1"
}

$env:PATH = "$($pythonInfo.torch_lib);$($pythonInfo.python_dir);$env:PATH"

if ($Python) {
    $versionParts = $pythonInfo.version.Split(".") | ForEach-Object { [int]$_ }
    if ($versionParts[0] -gt 3 -or ($versionParts[0] -eq 3 -and $versionParts[1] -ge 14)) {
        if ([string]::IsNullOrWhiteSpace($env:PYO3_USE_ABI3_FORWARD_COMPATIBILITY)) {
            $env:PYO3_USE_ABI3_FORWARD_COMPATIBILITY = "1"
        }
    }
}

$buildMode = @()
if ($Release) {
    $buildMode += "--release"
}

$featureArgs = @()
if ($Python) {
    $featureArgs += @("--features", "python")
}

cargo build -p run-manager --target $rustTarget @buildMode
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

cargo build -p psyche-solana-client --target $rustTarget --no-default-features @featureArgs @buildMode
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$profile = if ($Release) { "release" } else { "debug" }
$targetDir = "target\$rustTarget\$profile"
$envScript = "$targetDir\native-windows-arm64-env.ps1"
$pythonLiteral = ConvertTo-PowerShellSingleQuotedLiteral $PythonExecutable
$torchLibLiteral = ConvertTo-PowerShellSingleQuotedLiteral $pythonInfo.torch_lib
$pythonDirLiteral = ConvertTo-PowerShellSingleQuotedLiteral $pythonInfo.python_dir
@"
`$env:PYTHON_SYS_EXECUTABLE = $pythonLiteral
`$env:LIBTORCH_USE_PYTORCH = "1"
`$env:LIBTORCH_BYPASS_VERSION_CHECK = "1"
`$env:PATH = $torchLibLiteral + ";" + $pythonDirLiteral + ";" + `$env:PATH
"@ | Set-Content -Encoding ASCII $envScript

@"
Built native Psyche binaries:
  $targetDir\run-manager.exe
  $targetDir\psyche-solana-client.exe

Python: $PythonExecutable
Python version: $($pythonInfo.version)
Torch libraries: $($pythonInfo.torch_lib)
Rust target: $rustTarget

Before running the binaries in a fresh PowerShell session:
  . .\$envScript
"@
