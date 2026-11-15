param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskToolchain = [regex]::Match([IO.File]::ReadAllText("$taskRoot/saves/rust-toolchain.toml"), 'channel = "([^"]+)"').Groups[1].Value
$taskRustup = Join-Path $env:USERPROFILE '.cargo/bin/rustup.exe'
if (!(Test-Path -LiteralPath $taskRustup)) { $taskRustup = (Get-Command rustup.exe).Source }
Push-Location "$taskRoot/saves"
try {
    $taskMetadata = & $taskRustup run $taskToolchain cargo metadata --locked --format-version 1 --filter-platform x86_64-pc-windows-msvc
    if ($LASTEXITCODE) { throw '无法读取存档组件依赖授权' }
    $taskPackages = ($taskMetadata | ConvertFrom-Json).packages | Where-Object source | Sort-Object name,version
} finally { Pop-Location }
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$taskIndex = foreach ($taskPackage in $taskPackages) {
    $taskFolder = Split-Path -Parent $taskPackage.manifest_path
    $taskFiles = @(Get-ChildItem -LiteralPath $taskFolder -File | Where-Object Name -Match '^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE|UNLICENSE)')
    if ($taskPackage.license_file) {
        $taskLicense = Get-Item -LiteralPath (Join-Path $taskFolder $taskPackage.license_file)
        if ($taskLicense.FullName -notin $taskFiles.FullName) { $taskFiles += $taskLicense }
    }
    if (!$taskFiles) { throw "未找到授权文本：$($taskPackage.name) $($taskPackage.version)" }
    $taskTarget = Join-Path $Destination "$($taskPackage.name)-$($taskPackage.version)"
    New-Item -ItemType Directory -Path $taskTarget -Force | Out-Null
    $taskFiles | Copy-Item -Destination $taskTarget
    Get-ChildItem -LiteralPath $taskTarget -File | Where-Object { $_.LastWriteTime.Year -lt 1980 } |
        ForEach-Object { $_.LastWriteTime = [datetime]::new(1980, 1, 1) }
    "$($taskPackage.name) $($taskPackage.version) | $($taskPackage.license) | $($taskPackage.repository)"
}
$taskIndex | Set-Content -LiteralPath (Join-Path $Destination 'DEPENDENCIES.txt') -Encoding utf8
