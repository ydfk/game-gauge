[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Install')][string]$MsiPath,
    [Parameter(ParameterSetName = 'Install')][switch]$VerifyOnly,
    [Parameter(Mandatory = $true, ParameterSetName = 'Recovery')][switch]$ConfigureRecoveryOnly
)
$ErrorActionPreference = 'Stop'

function Assert-PresentMonAdministrator {
    $taskPrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '请在管理员 PowerShell 中运行此脚本；PresentMon 安装和服务恢复配置需要管理员权限。'
    }
}

function Set-PresentMonRecovery {
    # 使用系统服务恢复，不依赖游戏仪表常驻，也不为普通用户放宽服务权限。
    Get-Service -Name PresentMonSharedService -ErrorAction Stop | Out-Null
    $taskServiceControl = Join-Path ([Environment]::SystemDirectory) 'sc.exe'
    $taskResult = & $taskServiceControl failure PresentMonSharedService reset= 86400 actions= restart/5000/restart/15000/restart/60000 2>&1
    if ($LASTEXITCODE -ne 0) { throw "设置 PresentMon 服务恢复策略失败：$taskResult" }
    $taskResult = & $taskServiceControl failureflag PresentMonSharedService 1 2>&1
    if ($LASTEXITCODE -ne 0) { throw "设置 PresentMon 非崩溃错误恢复失败：$taskResult" }
    # 最后一次动作会重复；正常停止不触发恢复，避免持续故障时快速重启。
    Write-Host 'PresentMon 意外退出后将在 5 秒、15 秒、之后每次 60 秒自动重启；无故障 24 小时后重置计数。'
    Write-Host '非崩溃错误退出恢复标志在下次 Windows 启动后生效；配置过程不重启正在运行的服务。'
}

if ($ConfigureRecoveryOnly) {
    Assert-PresentMonAdministrator
    Set-PresentMonRecovery
    return
}

$taskMsi = (Resolve-Path -LiteralPath $MsiPath).Path
$taskSignature = Get-AuthenticodeSignature -LiteralPath $taskMsi
if ($taskSignature.Status -ne 'Valid' -or $taskSignature.SignerCertificate.Subject -notmatch 'CN=Intel Corporation,') {
    throw '安装包未通过 Intel Corporation 数字签名校验。'
}

$taskInstaller = New-Object -ComObject WindowsInstaller.Installer
$taskDatabase = $taskInstaller.OpenDatabase($taskMsi, 0)
$taskView = $taskDatabase.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='ProductName'")
$taskView.Execute()
$taskProduct = $taskView.Fetch().StringData(1)
if ($taskProduct -notmatch '^Intel(?:\(R\))? PresentMon$') { throw "安装包产品名不符：$taskProduct" }
$taskCodeView = $taskDatabase.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='ProductCode'")
$taskCodeView.Execute()
$taskProductCode = $taskCodeView.Fetch().StringData(1)
if ($VerifyOnly) { Write-Host "安装包校验通过：$taskProduct；签名者 $($taskSignature.SignerCertificate.Subject)"; return }

Assert-PresentMonAdministrator

$taskExisting = Get-Service PresentMonSharedService -ErrorAction SilentlyContinue
$taskServiceRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Intel/PresentMonSharedService'
$taskFilesReady = (Test-Path -LiteralPath (Join-Path $taskServiceRoot 'PresentMonService.exe')) -and (Test-Path -LiteralPath (Join-Path $taskServiceRoot 'PresentMonAPI2.dll'))
if ($taskExisting -and $taskFilesReady) {
    Write-Host 'PresentMon Shared Service 已安装，未覆盖现有版本。'
    Set-PresentMonRecovery
    Set-Service PresentMonSharedService -StartupType Automatic
    if ($taskExisting.Status -ne 'Running') { Start-Service PresentMonSharedService }
    return
}

$taskLog = Join-Path $env:TEMP 'GameGauge-PresentMon-install.log'
$taskQuotedMsi = '"' + $taskMsi + '"'
$taskQuotedLog = '"' + $taskLog + '"'
$taskProductInstalled = $taskInstaller.ProductState($taskProductCode) -eq 5
if ($taskExisting -and !$taskProductInstalled) { throw '其他版本的 PresentMon 安装已损坏，请使用对应版本安装包修复，避免降级。' }
$taskMode = if ($taskProductInstalled) { '/fa' } else { '/i' }
$taskArguments = @($taskMode, $taskQuotedMsi, '/qn', '/norestart', '/L*v', $taskQuotedLog)
$taskProcess = Start-Process -FilePath (Join-Path ([Environment]::SystemDirectory) 'msiexec.exe') -ArgumentList $taskArguments -WindowStyle Hidden -Wait -PassThru
if ($taskProcess.ExitCode -notin @(0, 3010)) { throw "PresentMon 安装失败，MSI 退出代码 $($taskProcess.ExitCode)。日志：$taskLog" }
$taskService = Get-Service PresentMonSharedService -ErrorAction Stop
Set-PresentMonRecovery
Set-Service PresentMonSharedService -StartupType Automatic
if ($taskService.Status -ne 'Running') { Start-Service PresentMonSharedService }
Write-Host 'PresentMon Shared Service 已安装并启动。请重新启动游戏仪表。'
if ($taskProcess.ExitCode -eq 3010) { Write-Warning '安装器提示需要重启 Windows。'; exit 3010 }
