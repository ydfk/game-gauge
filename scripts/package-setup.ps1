$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBuild = Join-Path $taskRoot 'build/setup'
$taskStage = Join-Path $taskBuild ('stage-' + [guid]::NewGuid().ToString('N'))
$taskApp = Join-Path $taskStage 'app'
New-Item -ItemType Directory -Path "$taskApp/.deps","$taskApp/scripts","$taskApp/licenses" -Force | Out-Null
foreach ($taskName in @('GameGauge.exe','GameGauge.Settings.exe','GameGauge.CpuProbe.exe','GameGauge.Diagnostics.exe')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot "build/core/bin/Release/$taskName") -Destination $taskApp
}
foreach ($taskName in @('install-presentmon.ps1','setup-uninstall.ps1')) {
    [IO.File]::WriteAllText((Join-Path "$taskApp/scripts" $taskName), [IO.File]::ReadAllText((Join-Path $PSScriptRoot $taskName)), [Text.UTF8Encoding]::new($true))
}
Copy-Item -LiteralPath "$taskRoot/.deps/AMDFamily17-0.2.11.bin" -Destination "$taskApp/.deps"
Copy-Item -LiteralPath "$taskRoot/third_party/nlohmann/LICENSE.MIT" -Destination "$taskApp/licenses/nlohmann.txt"
Copy-Item -LiteralPath "$taskRoot/third_party/presentmon/LICENSE.txt" -Destination "$taskApp/licenses/PresentMon.txt"
Copy-Item -LiteralPath "$taskRoot/THIRD_PARTY_NOTICES.md" -Destination $taskApp
$taskDependencies = @(
    @{Source='PresentMon-2.6.0.msi';Name='PresentMon-2.6.0.msi';Hash='0CB43D2622356C1E277A77F840887F9000CEA4E2D4236C0850922FC204C6F820'},
    @{Source='PawnIO_setup-2.2.0.exe';Name='PawnIO_setup.exe';Hash='1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032'}
)
foreach ($taskDependency in $taskDependencies) {
    $taskSource = Join-Path "$taskRoot/.deps" $taskDependency.Source
    if ((Get-FileHash -LiteralPath $taskSource -Algorithm SHA256).Hash -ne $taskDependency.Hash) { throw "依赖校验失败：$taskSource" }
    if ((Get-AuthenticodeSignature -LiteralPath $taskSource).Status -ne 'Valid') { throw "依赖签名无效：$taskSource" }
    Copy-Item -LiteralPath $taskSource -Destination (Join-Path $taskStage $taskDependency.Name)
}
Compress-Archive -Path "$taskStage/*" -DestinationPath "$taskBuild/payload.zip" -Force
$taskPayload = (Join-Path $taskBuild 'payload.zip').Replace('\','/')
$taskInstall = (Join-Path $taskBuild 'install.ps1').Replace('\','/')
[IO.File]::WriteAllText($taskInstall, [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'setup-install.ps1')), [Text.UTF8Encoding]::new($true))
$taskManifest = (Join-Path $taskRoot 'src/setup/setup.manifest').Replace('\','/')
"#include <windows.h>`n1 RT_MANIFEST `"$taskManifest`"`n101 RCDATA `"$taskPayload`"`n102 RCDATA `"$taskInstall`"" | Set-Content -LiteralPath "$taskBuild/payload.rc" -Encoding utf8
& "$PSScriptRoot/build-core.ps1"
if ($LASTEXITCODE) { throw '安装器构建失败' }
New-Item -ItemType Directory -Path "$taskRoot/build/package" -Force | Out-Null
Copy-Item -LiteralPath "$taskRoot/build/core/bin/Release/GameGauge.Setup.exe" -Destination "$taskRoot/build/package/GameGauge-0.1.0-Setup.exe" -Force
Write-Host "已生成：$taskRoot/build/package/GameGauge-0.1.0-Setup.exe"
