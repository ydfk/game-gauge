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
    if ((Test-Path -LiteralPath $taskFile) -and
        (Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash -eq $taskAsset.Hash) {
        Write-Host "使用已校验依赖：$($taskAsset.Name)"
        continue
    }
    $taskPartial = "$taskFile.download"
    try {
        foreach ($taskAttempt in 1..4) {
            try {
                Write-Host "下载依赖 $($taskAsset.Name)，第 $taskAttempt/4 次：$($taskAsset.Url)"
                Invoke-WebRequest -Uri $taskAsset.Url -OutFile $taskPartial -ConnectionTimeoutSeconds 30 -OperationTimeoutSeconds 180
                if ((Get-FileHash -LiteralPath $taskPartial -Algorithm SHA256).Hash -ne $taskAsset.Hash) {
                    throw "依赖 SHA256 校验失败：$($taskAsset.Name)"
                }
                Move-Item -LiteralPath $taskPartial -Destination $taskFile -Force
                break
            } catch {
                if ($taskAttempt -eq 4) {
                    throw "依赖下载失败（已尝试 4 次）：$($taskAsset.Name)，$($taskAsset.Url)。$($_.Exception.Message)"
                }
                $taskDelay = 3 * [Math]::Pow(2, $taskAttempt - 1)
                Write-Warning "下载失败：$($_.Exception.Message)。$taskDelay 秒后重试。"
                Start-Sleep -Seconds $taskDelay
            }
        }
    } finally {
        Remove-Item -LiteralPath $taskPartial -Force -ErrorAction SilentlyContinue
    }
}
Expand-Archive -LiteralPath "$taskDeps/PawnIO.Modules-0.2.11.zip" -DestinationPath "$taskDeps/modules-0.2.11" -Force
$taskModule = @(Get-ChildItem "$taskDeps/modules-0.2.11" -Filter AMDFamily17.bin -Recurse)
if ($taskModule.Count -ne 1 -or (Get-FileHash -LiteralPath $taskModule[0].FullName -Algorithm SHA256).Hash -ne 'dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91') { throw 'AMD 模块校验失败' }
Copy-Item -LiteralPath $taskModule[0].FullName -Destination "$taskDeps/AMDFamily17-0.2.11.bin" -Force
