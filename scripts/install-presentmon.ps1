param([Parameter(Mandatory = $true)][string]$MsiPath, [switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'

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
if ($VerifyOnly) { Write-Host "安装包校验通过：$taskProduct；签名者 $($taskSignature.SignerCertificate.Subject)"; return }

$taskPrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '请在管理员 PowerShell 中运行此脚本；PresentMon 需要注册 Windows 服务和 ETW 组件。'
}

$taskExisting = Get-Service PresentMonSharedService -ErrorAction SilentlyContinue
if ($taskExisting) {
    Write-Host 'PresentMon Shared Service 已安装，未覆盖现有版本。'
    if ($taskExisting.Status -ne 'Running') { Start-Service PresentMonSharedService }
    return
}

$taskLog = Join-Path $env:TEMP 'GameGauge-PresentMon-install.log'
$taskQuotedMsi = '"' + $taskMsi + '"'
$taskQuotedLog = '"' + $taskLog + '"'
$taskArguments = @('/i', $taskQuotedMsi, '/qn', '/norestart', '/L*v', $taskQuotedLog)
$taskProcess = Start-Process -FilePath (Join-Path $env:WINDIR 'System32/msiexec.exe') -ArgumentList $taskArguments -WindowStyle Hidden -Wait -PassThru
if ($taskProcess.ExitCode -notin @(0, 3010)) { throw "PresentMon 安装失败，MSI 退出代码 $($taskProcess.ExitCode)。日志：$taskLog" }
$taskService = Get-Service PresentMonSharedService -ErrorAction Stop
if ($taskService.Status -ne 'Running') { Start-Service PresentMonSharedService }
Write-Host 'PresentMon Shared Service 已安装并启动。请重新启动游戏仪表。'
if ($taskProcess.ExitCode -eq 3010) { Write-Warning '安装器提示需要重启 Windows。' }
