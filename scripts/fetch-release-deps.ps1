$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskDeps = Join-Path $taskRoot '.deps'
New-Item -ItemType Directory -Path $taskDeps -Force | Out-Null
$taskAssets = @(
    @{Name='PresentMon-2.6.0.msi';Url='https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0.msi';Hash='0cb43d2622356c1e277a77f840887f9000cea4e2d4236c0850922fc204c6f820'},
    @{Name='PawnIO_setup-2.2.0.exe';Url='https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe';Hash='1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032'},
    @{Name='PawnIO.Modules-0.2.11.zip';Url='https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip';Hash='43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4'}
)
foreach ($taskAsset in $taskAssets) {
    $taskFile = Join-Path $taskDeps $taskAsset.Name
    if (!(Test-Path -LiteralPath $taskFile)) { Invoke-WebRequest -Uri $taskAsset.Url -OutFile $taskFile }
    if ((Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash -ne $taskAsset.Hash) { throw "依赖校验失败：$taskFile" }
}
Expand-Archive -LiteralPath "$taskDeps/PawnIO.Modules-0.2.11.zip" -DestinationPath "$taskDeps/modules-0.2.11" -Force
$taskModule = @(Get-ChildItem "$taskDeps/modules-0.2.11" -Filter AMDFamily17.bin -Recurse)
if ($taskModule.Count -ne 1 -or (Get-FileHash -LiteralPath $taskModule[0].FullName -Algorithm SHA256).Hash -ne 'dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91') { throw 'AMD 模块校验失败' }
Copy-Item -LiteralPath $taskModule[0].FullName -Destination "$taskDeps/AMDFamily17-0.2.11.bin" -Force
