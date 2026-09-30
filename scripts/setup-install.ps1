$ErrorActionPreference = 'Stop'
Start-Transcript -LiteralPath (Join-Path $PSScriptRoot 'install.log') -Force | Out-Null
function Set-InstallProgress([int]$Value) { [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'progress.txt'), [string]$Value) }
try {
    Set-InstallProgress 5
    $taskPayload = Join-Path $PSScriptRoot 'payload'
    Expand-Archive -LiteralPath (Join-Path $PSScriptRoot 'payload.zip') -DestinationPath $taskPayload -Force
    $taskDestination = Join-Path $env:ProgramFiles 'GameGauge'
    $taskRelease = Get-Content -LiteralPath (Join-Path $taskPayload 'app/release.json') -Raw | ConvertFrom-Json
    # 安装目录固定，服务程序不从用户可写目录运行。
    New-Item -ItemType Directory -Path $taskDestination -Force | Out-Null
    if ((Get-Item -LiteralPath $taskDestination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '安装目录不能是重解析点' }
    $taskSensor = Get-Service 'GameGauge.Sensor' -ErrorAction SilentlyContinue
    if ($taskSensor) {
        Stop-Service 'GameGauge.Sensor' -Force
        $taskSensor.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(15))
    }
    . (Join-Path $taskPayload 'app/scripts/setup-processes.ps1')
    Stop-GameGaugeProcesses -Directory $taskDestination
    Set-InstallProgress 25
    Copy-Item -Path (Join-Path $taskPayload 'app/*') -Destination $taskDestination -Recurse -Force
    Set-InstallProgress 40
    & (Join-Path $taskDestination 'scripts/install-presentmon.ps1') -MsiPath (Join-Path $taskPayload 'PresentMon-2.6.0.msi')
    $taskPawn = Join-Path $taskPayload 'PawnIO_setup.exe'
    if ((Get-FileHash -LiteralPath $taskPawn -Algorithm SHA256).Hash -ne '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032') { throw 'PawnIO 安装器哈希不符' }
    if ((Get-AuthenticodeSignature -LiteralPath $taskPawn).Status -ne 'Valid') { throw 'PawnIO 签名无效' }
    if (!(Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO')) {
        $taskProcess = Start-Process -FilePath $taskPawn -ArgumentList '-install -silent' -WindowStyle Hidden -PassThru -Wait
        if ($taskProcess.ExitCode -ne 0) { throw "PawnIO 安装失败：$($taskProcess.ExitCode)" }
    }
    $taskCpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    Set-InstallProgress 60
    if ($taskCpu.Manufacturer -eq 'AuthenticAMD') {
        $taskServiceCommand = '"' + (Join-Path $taskDestination 'GameGauge.CpuProbe.exe') + '" --service'
        if (!$taskSensor) { New-Service -Name 'GameGauge.Sensor' -BinaryPathName $taskServiceCommand -DisplayName 'GameGauge CPU Temperature' -StartupType Automatic | Out-Null }
        else {
            # 使用结构化参数，避免 Windows PowerShell 5.1 转发带空格路径时丢失引号。
            $taskService = Get-CimInstance Win32_Service -Filter "Name='GameGauge.Sensor'"
            $taskChange = Invoke-CimMethod -InputObject $taskService -MethodName Change -Arguments @{PathName=$taskServiceCommand;StartMode='Automatic'}
            if ($taskChange.ReturnValue -ne 0) { throw "更新温度服务失败，Win32_Service.Change 返回 $($taskChange.ReturnValue)" }
        }
        $taskRecovery = & "$env:WINDIR/System32/sc.exe" failure GameGauge.Sensor reset= 86400 actions= restart/5000/restart/15000 2>&1
        if ($LASTEXITCODE) { throw "设置温度服务恢复策略失败：$taskRecovery" }
        $taskConfigured = Get-CimInstance Win32_Service -Filter "Name='GameGauge.Sensor'"
        if ($taskConfigured.PathName -ne $taskServiceCommand -or $taskConfigured.StartMode -ne 'Auto') { throw '温度服务配置读回不一致' }
        Start-Service 'GameGauge.Sensor'
    }
    $taskShell = New-Object -ComObject WScript.Shell
    Set-InstallProgress 75
    $taskPrograms = [Environment]::GetFolderPath('CommonPrograms')
    . (Join-Path $taskDestination 'scripts/setup-shortcuts.ps1')
    Remove-Item -LiteralPath (Join-Path $taskPrograms '游戏仪表.lnk') -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $taskPrograms '游戏仪表 GameGauge.lnk') -ErrorAction SilentlyContinue
    $taskShortcutPath = Join-Path $taskPrograms 'GameGauge.lnk'
    $taskShortcut = $taskShell.CreateShortcut($taskShortcutPath)
    $taskShortcut.Description = '游戏仪表 GameGauge：游戏性能与硬件监控'
    $taskShortcut.TargetPath = Join-Path $taskDestination 'GameGauge.exe'; $taskShortcut.WorkingDirectory = $taskDestination; $taskShortcut.Save()
    Set-GameGaugeShortcutName -Path $taskShortcutPath -Executable $taskShortcut.TargetPath
    Remove-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) '游戏仪表 GameGauge.lnk') -ErrorAction SilentlyContinue
    $taskDesktopPath = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) '游戏仪表.lnk'
    $taskDesktop = $taskShell.CreateShortcut($taskDesktopPath)
    $taskDesktop.TargetPath = $taskShortcut.TargetPath; $taskDesktop.WorkingDirectory = $taskDestination; $taskDesktop.Save()
    $taskPowerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $taskUninstallArgs = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $taskDestination 'scripts/setup-uninstall.ps1') + '"'
    Remove-Item -LiteralPath (Join-Path $taskPrograms '卸载游戏仪表 GameGauge.lnk') -ErrorAction SilentlyContinue
    $taskUninstallLink = $taskShell.CreateShortcut((Join-Path $taskPrograms '卸载游戏仪表.lnk'))
    $taskUninstallLink.TargetPath = $taskPowerShell; $taskUninstallLink.Arguments = $taskUninstallArgs
    $taskUninstallLink.WindowStyle = 7; $taskUninstallLink.Save()
    $taskStartup = $taskShell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('CommonStartup')) 'GameGauge.lnk'))
    $taskStartup.TargetPath = $taskShortcut.TargetPath; $taskStartup.Arguments = '--background'; $taskStartup.WorkingDirectory = $taskDestination; $taskStartup.Save()
    $taskRegistry = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\GameGauge'
    New-Item $taskRegistry -Force | Out-Null
    $taskUninstall = '"' + $taskPowerShell + '" ' + $taskUninstallArgs
    @{DisplayName='游戏仪表';DisplayVersion=$taskRelease.version;Publisher='GameGauge';InstallLocation=$taskDestination;UninstallString=$taskUninstall;DisplayIcon=('"' + $taskShortcut.TargetPath + '",0');InstallDate=(Get-Date -Format 'yyyyMMdd')}.GetEnumerator() | ForEach-Object { New-ItemProperty $taskRegistry -Name $_.Key -Value $_.Value -PropertyType String -Force | Out-Null }
    foreach ($taskName in @('NoModify','NoRepair')) { New-ItemProperty $taskRegistry -Name $taskName -Value 1 -PropertyType DWord -Force | Out-Null }
    $taskSize = [int][Math]::Ceiling((Get-ChildItem -LiteralPath $taskDestination -Recurse -File | Measure-Object Length -Sum).Sum / 1024)
    New-ItemProperty $taskRegistry -Name EstimatedSize -Value $taskSize -PropertyType DWord -Force | Out-Null
    if (!(Test-Path -LiteralPath $taskDesktopPath) -or !(Get-ItemProperty $taskRegistry).UninstallString) { throw '快捷方式或卸载注册未完成' }
    # 通过交互用户的普通权限计划任务启动，避免让主程序继承安装器权限。
    $taskUser = (Get-CimInstance Win32_ComputerSystem).UserName
    Set-InstallProgress 90
    if ($taskUser) {
        $taskAction = New-ScheduledTaskAction -Execute $taskShortcut.TargetPath -WorkingDirectory $taskDestination
        $taskPrincipal = New-ScheduledTaskPrincipal -UserId $taskUser -LogonType Interactive -RunLevel Limited
        Register-ScheduledTask -TaskName 'GameGauge.StartAfterInstall' -Action $taskAction -Principal $taskPrincipal -Force | Out-Null
        Start-ScheduledTask -TaskName 'GameGauge.StartAfterInstall'
    }
    Set-InstallProgress 100
    Stop-Transcript | Out-Null
    exit 0
} catch {
    Write-Output $_
    # 安装中途失败时恢复已有采集服务，防止旧安装停留在停止状态。
    if ($taskSensor) { Start-Service 'GameGauge.Sensor' -ErrorAction Continue }
    Stop-Transcript | Out-Null
    exit 1
}
