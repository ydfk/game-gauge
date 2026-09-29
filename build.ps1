param(
    [ValidateSet('Build','Run','Package')][string]$Task = 'Build',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [string]$Version,
    [string]$Repository = ''
)
$ErrorActionPreference = 'Stop'
if (!$Version) { $Version = (Get-Content -LiteralPath "$PSScriptRoot/VERSION" -Raw).Trim() }
if ($Task -eq 'Package') {
    if ($Configuration -ne 'Release') { throw '安装包仅支持 Release 配置' }
    & "$PSScriptRoot/scripts/fetch-release-deps.ps1"
    & "$PSScriptRoot/scripts/package-setup.ps1" -Version $Version -Repository $Repository
} else {
    & "$PSScriptRoot/scripts/build-core.ps1" -Configuration $Configuration -Version $Version -Repository $Repository
    if ($Task -eq 'Run') {
        Start-Process -FilePath "$PSScriptRoot/build/core/bin/$Configuration/GameGauge.exe" -WorkingDirectory $PSScriptRoot
    }
}
