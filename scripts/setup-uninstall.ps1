$ErrorActionPreference = 'Stop'
$taskPrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"') -WindowStyle Hidden
    exit
}
$taskDestination = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'GameGauge'))
if ([IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)) -ne $taskDestination) { throw '卸载路径校验失败' }
if ((Get-Item -LiteralPath $taskDestination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '拒绝卸载重解析目录' }
Get-Service GameGauge.Sensor -ErrorAction SilentlyContinue | Stop-Service -Force
& sc.exe delete GameGauge.Sensor | Out-Null
Get-Process GameGauge,GameGauge.Settings -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$taskDestination\*" } | Stop-Process
Unregister-ScheduledTask -TaskName 'GameGauge.StartAfterInstall' -Confirm:$false -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('CommonPrograms')) '游戏仪表.lnk') -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('CommonStartup')) 'GameGauge.lnk') -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\GameGauge' -Recurse -ErrorAction SilentlyContinue
# 保留用户历史，以及可能被其他程序使用的 PresentMon / PawnIO。
Remove-Item -LiteralPath $taskDestination -Recurse -Force
