param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$Configuration,
    [Parameter(Mandatory=$true)][string]$Version,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskToolchain = [regex]::Match([IO.File]::ReadAllText("$taskRoot/saves/rust-toolchain.toml"), 'channel = "([^"]+)"').Groups[1].Value
if (!$taskToolchain) { throw '未找到固定版本的 Rust 工具链' }
$taskRustup = Join-Path $env:USERPROFILE '.cargo/bin/rustup.exe'
if (!(Test-Path -LiteralPath $taskRustup)) { $taskRustup = (Get-Command rustup.exe).Source }
$taskCargo = & $taskRustup which --toolchain $taskToolchain cargo
if ($LASTEXITCODE -or !$taskCargo) { throw "需要安装 Rust $taskToolchain MSVC 工具链" }
$env:GAMEGAUGE_VERSION = $Version
$env:CARGO_TARGET_DIR = Join-Path $BuildDir 'rust'
$taskCommand = if ($Test) { 'test' } else { 'build' }
$taskArgs = @($taskCommand, '--locked', '--target', 'x86_64-pc-windows-msvc')
$taskProfile = 'debug'
if ($Configuration -ne 'Debug') { $taskArgs += '--release'; $taskProfile = 'release' }
$env:PATH = "$(Split-Path -Parent $taskCargo);$env:PATH"
Push-Location "$taskRoot/saves"
try {
    & $taskCargo @taskArgs
    if ($LASTEXITCODE) { throw "存档工作进程 Cargo $taskCommand 失败" }
} finally { Pop-Location }
if (!$Test) {
    $taskDestination = Join-Path $BuildDir "bin/$Configuration"
    New-Item -ItemType Directory -Path $taskDestination -Force | Out-Null
    $taskExe = "$env:CARGO_TARGET_DIR/x86_64-pc-windows-msvc/$taskProfile/gamegauge-saves.exe"
    Copy-Item -LiteralPath $taskExe -Destination (Join-Path $taskDestination 'GameGauge.Saves.exe') -Force
}
