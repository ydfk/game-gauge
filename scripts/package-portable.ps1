param([string]$Version)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (!$Version) { $Version = (Get-Content -LiteralPath "$taskRoot/VERSION" -Raw).Trim() }
if ($Version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') { throw '版本格式不正确' }
$taskBin = Join-Path $taskRoot 'build/core/bin/Release'
$taskPackage = Join-Path $taskRoot ('build/package/.staging-' + [guid]::NewGuid().ToString('N'))
$taskZip = Join-Path $taskRoot "build/package/GameGauge-$Version-win-x64.zip"

New-Item -ItemType Directory -Path $taskPackage -Force | Out-Null
foreach ($taskName in @('GameGauge.exe', 'GameGauge.Settings.exe', 'GameGauge.Diagnostics.exe', 'GameGauge.CpuProbe.exe')) {
    $taskSource = Join-Path $taskBin $taskName
    if (!(Test-Path -LiteralPath $taskSource)) { throw "缺少 Release 构建产物：$taskName" }
    Copy-Item -LiteralPath $taskSource -Destination (Join-Path $taskPackage $taskName) -Force
}
foreach ($taskName in @('README.md', 'plan.md', 'THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot $taskName) -Destination (Join-Path $taskPackage $taskName) -Force
}
foreach ($taskFolder in @('assets', 'docs', 'scripts', 'licenses', 'output/imagegen')) {
    New-Item -ItemType Directory -Path (Join-Path $taskPackage $taskFolder) -Force | Out-Null
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'assets/logo.svg') -Destination (Join-Path $taskPackage 'assets/logo.svg') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'assets/logo-concept.png') -Destination (Join-Path $taskPackage 'assets/logo-concept.png') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'output/imagegen/frameeave-logo-concept-v1.png') -Destination (Join-Path $taskPackage 'output/imagegen/frameeave-logo-concept-v1.png') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'docs/validation.md') -Destination (Join-Path $taskPackage 'docs/validation.md') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'scripts/install-presentmon.ps1') -Destination (Join-Path $taskPackage 'scripts/install-presentmon.ps1') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'third_party/nlohmann/LICENSE.MIT') -Destination (Join-Path $taskPackage 'licenses/nlohmann-json-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'third_party/presentmon/LICENSE.txt') -Destination (Join-Path $taskPackage 'licenses/PresentMon-SDK-MIT.txt') -Force

$taskManifest = [ordered]@{version=$Version;platform='Windows 11 x64';presentmon_service='separate signed Intel installation';files=@()}
foreach ($taskName in @('GameGauge.exe', 'GameGauge.Settings.exe', 'GameGauge.Diagnostics.exe', 'GameGauge.CpuProbe.exe')) {
    $taskFile = Join-Path $taskPackage $taskName
    $taskManifest.files += [ordered]@{name=$taskName;sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $taskFile).Hash;bytes=(Get-Item $taskFile).Length}
}
$taskManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskPackage 'release-manifest.json') -Encoding utf8
Compress-Archive -Path (Join-Path $taskPackage '*') -DestinationPath $taskZip -CompressionLevel Optimal -Force
Write-Host "已生成便携包：$taskZip"
