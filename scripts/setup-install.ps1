$ErrorActionPreference = 'Stop'
Start-Transcript -LiteralPath (Join-Path $PSScriptRoot 'install.log') -Force | Out-Null
try {
    $taskPayload = Join-Path $PSScriptRoot 'payload'
    Expand-Archive -LiteralPath (Join-Path $PSScriptRoot 'payload.zip') -DestinationPath $taskPayload -Force
    $taskDestination = Join-Path $env:ProgramFiles 'GameGauge'
    $taskRelease = Get-Content -LiteralPath (Join-Path $taskPayload 'app/release.json') -Raw | ConvertFrom-Json
    # 安装目录固定，服务程序不从用户可写目录运行。
    New-Item -ItemType Directory -Path $taskDestination -Force | Out-Null
    if ((Get-Item -LiteralPath $taskDestination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '安装目录不能是重解析点' }
    $taskSensor = Get-Service 'GameGauge.Sensor' -ErrorAction SilentlyContinue
    if ($taskSensor) { Stop-Service 'GameGauge.Sensor' -Force }
    Get-Process GameGauge,GameGauge.Settings -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$taskDestination\*" } | Stop-Process
    Copy-Item -Path (Join-Path $taskPayload 'app/*') -Destination $taskDestination -Recurse -Force
    & (Join-Path $taskDestination 'scripts/install-presentmon.ps1') -MsiPath (Join-Path $taskPayload 'PresentMon-2.6.0.msi')
    $taskPawn = Join-Path $taskPayload 'PawnIO_setup.exe'
    if ((Get-FileHash -LiteralPath $taskPawn -Algorithm SHA256).Hash -ne '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032') { throw 'PawnIO 安装器哈希不符' }
    if ((Get-AuthenticodeSignature -LiteralPath $taskPawn).Status -ne 'Valid') { throw 'PawnIO 签名无效' }
    if (!(Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO')) {
        $taskProcess = Start-Process -FilePath $taskPawn -ArgumentList '-install -silent' -WindowStyle Hidden -PassThru -Wait
        if ($taskProcess.ExitCode -ne 0) { throw "PawnIO 安装失败：$($taskProcess.ExitCode)" }
    }
    $taskCpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    if ($taskCpu.Manufacturer -eq 'AuthenticAMD') {
        $taskServiceCommand = '"' + (Join-Path $taskDestination 'GameGauge.CpuProbe.exe') + '" --service'
        if (!$taskSensor) { New-Service -Name 'GameGauge.Sensor' -BinaryPathName $taskServiceCommand -DisplayName 'GameGauge CPU Temperature' -StartupType Automatic | Out-Null }
        else { & sc.exe config GameGauge.Sensor binPath= $taskServiceCommand start= auto | Out-Null; if ($LASTEXITCODE) { throw '更新温度服务失败' } }
        & sc.exe failure GameGauge.Sensor reset= 86400 actions= restart/5000/restart/15000 | Out-Null
        Start-Service 'GameGauge.Sensor'
    }
    $taskShell = New-Object -ComObject WScript.Shell
    $taskPrograms = [Environment]::GetFolderPath('CommonPrograms')
    $taskShortcut = $taskShell.CreateShortcut((Join-Path $taskPrograms '游戏仪表.lnk'))
    $taskShortcut.TargetPath = Join-Path $taskDestination 'GameGauge.exe'; $taskShortcut.WorkingDirectory = $taskDestination; $taskShortcut.Save()
    $taskStartup = $taskShell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('CommonStartup')) 'GameGauge.lnk'))
    $taskStartup.TargetPath = $taskShortcut.TargetPath; $taskStartup.Arguments = '--background'; $taskStartup.WorkingDirectory = $taskDestination; $taskStartup.Save()
    $taskRegistry = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\GameGauge'
    New-Item $taskRegistry -Force | Out-Null
    $taskUninstall = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $taskDestination 'scripts/setup-uninstall.ps1') + '"'
    @{DisplayName='游戏仪表';DisplayVersion=$taskRelease.version;Publisher='GameGauge';InstallLocation=$taskDestination;UninstallString=$taskUninstall;DisplayIcon=$taskShortcut.TargetPath}.GetEnumerator() | ForEach-Object { New-ItemProperty $taskRegistry -Name $_.Key -Value $_.Value -Force | Out-Null }
    # 通过交互用户的普通权限计划任务启动，避免让主程序继承安装器权限。
    $taskUser = (Get-CimInstance Win32_ComputerSystem).UserName
    if ($taskUser) {
        $taskAction = New-ScheduledTaskAction -Execute $taskShortcut.TargetPath -WorkingDirectory $taskDestination
        $taskPrincipal = New-ScheduledTaskPrincipal -UserId $taskUser -LogonType Interactive -RunLevel Limited
        Register-ScheduledTask -TaskName 'GameGauge.StartAfterInstall' -Action $taskAction -Principal $taskPrincipal -Force | Out-Null
        Start-ScheduledTask -TaskName 'GameGauge.StartAfterInstall'
    }
    Stop-Transcript | Out-Null
    exit 0
} catch {
    Write-Output $_
    Stop-Transcript | Out-Null
    exit 1
}
