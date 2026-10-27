param([switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

function Test-ComponentResources {
    $taskResources = @(
        @{Path='components/PresentMon-2.6.0.msi';Hash='0CB43D2622356C1E277A77F840887F9000CEA4E2D4236C0850922FC204C6F820';Signer='CN=Intel Corporation,'},
        @{Path='components/PawnIO_setup.exe';Hash='1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032';Signer='CN=namazso.eu,'},
        @{Path='.deps/AMDFamily17-0.2.11.bin';Hash='DAE74615761B78BDF064DFB3E136252DDCC6FC727D88F14738D0E5800D427A91';Signer=''}
    )
    # 所有离线资源先通过校验，才允许修改任何服务或驱动。
    foreach ($taskResource in $taskResources) {
        $taskFile = Join-Path $taskRoot $taskResource.Path
        if (!(Test-Path -LiteralPath $taskFile -PathType Leaf)) { throw "缺少离线组件 $($taskResource.Path)，请重新安装游戏仪表。" }
        if ((Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash -ne $taskResource.Hash) { throw "离线组件校验失败：$($taskResource.Path)" }
        if ($taskResource.Signer) {
            $taskSignature = Get-AuthenticodeSignature -LiteralPath $taskFile
            if ($taskSignature.Status -ne 'Valid' -or $taskSignature.SignerCertificate.Subject -notlike "*$($taskResource.Signer)*") {
                throw "离线组件签名不符：$($taskResource.Path)"
            }
        }
    }
}

if ($VerifyOnly) { Test-ComponentResources; Write-Host '离线采集组件校验通过。'; return }
$taskPrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '修复采集组件需要管理员权限。' }
$taskExpected = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'GameGauge'))
if ($taskRoot -ne $taskExpected) { throw '只能修复 Program Files 中已安装的游戏仪表。' }
foreach ($taskPath in @($taskRoot, $PSScriptRoot, (Join-Path $taskRoot 'components'), (Join-Path $taskRoot '.deps'))) {
    if ((Get-Item -LiteralPath $taskPath).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '修复目录不能是重解析点。' }
}
$taskLock = [Threading.Mutex]::new($false, 'Global\GameGauge.ComponentRepair')
$taskLocked = $false
$taskTranscript = $false
$taskExitCode = 1
try {
    try { $taskLocked = $taskLock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $taskLocked = $true }
    if (!$taskLocked) { throw '另一个组件修复正在运行，请稍后重试。' }
    Start-Transcript -LiteralPath (Join-Path $taskRoot 'component-repair.log') -Force | Out-Null
    $taskTranscript = $true
    Test-ComponentResources
    $taskReboot = $false
    $global:LASTEXITCODE = 0
    & (Join-Path $PSScriptRoot 'install-presentmon.ps1') -MsiPath (Join-Path $taskRoot 'components/PresentMon-2.6.0.msi')
    if ($LASTEXITCODE -eq 3010) { $taskReboot = $true }
    elseif ($LASTEXITCODE -ne 0) { throw "PresentMon 修复失败：$LASTEXITCODE" }
    $taskCpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    if ($taskCpu.Manufacturer -eq 'AuthenticAMD') {
        $taskPawnRegistry = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO' -ErrorAction SilentlyContinue
        $taskPawnDriver = Get-CimInstance Win32_SystemDriver -Filter "Name='PawnIO'"
        $taskPawnLibrary = if ($taskPawnRegistry.InstallLocation) { Join-Path $taskPawnRegistry.InstallLocation 'PawnIOLib.dll' } else { '' }
        $taskDriverPath = if ($taskPawnDriver -and $taskPawnDriver.PathName) { [Environment]::ExpandEnvironmentVariables($taskPawnDriver.PathName.Trim('"')) } else { '' }
        if ($taskDriverPath.StartsWith('\??\')) { $taskDriverPath = $taskDriverPath.Substring(4) }
        if ($taskDriverPath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
            $taskDriverPath = Join-Path ([Environment]::GetFolderPath('Windows')) $taskDriverPath.Substring(12)
        } elseif ($taskDriverPath.StartsWith('System32\', [StringComparison]::OrdinalIgnoreCase)) {
            $taskDriverPath = Join-Path ([Environment]::GetFolderPath('Windows')) $taskDriverPath
        }
        if (!$taskPawnLibrary -or !(Test-Path -LiteralPath $taskPawnLibrary -PathType Leaf) -or !$taskDriverPath -or !(Test-Path -LiteralPath $taskDriverPath -PathType Leaf)) {
            $taskInstall = Start-Process -FilePath (Join-Path $taskRoot 'components/PawnIO_setup.exe') -ArgumentList '-install -silent' -WindowStyle Hidden -PassThru -Wait
            if ($taskInstall.ExitCode -eq 3010) { $taskReboot = $true }
            elseif ($taskInstall.ExitCode -ne 0) { throw "PawnIO 修复失败：$($taskInstall.ExitCode)" }
        } elseif ($taskPawnDriver.State -ne 'Running') {
            Start-Service 'PawnIO'
        }
        $taskProbe = Join-Path $taskRoot 'GameGauge.CpuProbe.exe'
        if (!(Test-Path -LiteralPath $taskProbe -PathType Leaf)) { throw 'CPU 采集程序缺失，请重新安装游戏仪表。' }
        $taskServiceCommand = '"' + $taskProbe + '" --service'
        $taskSensor = Get-Service 'GameGauge.Sensor' -ErrorAction SilentlyContinue
        if (!$taskSensor) {
            New-Service -Name 'GameGauge.Sensor' -BinaryPathName $taskServiceCommand -DisplayName 'GameGauge CPU Temperature' -StartupType Automatic | Out-Null
        } else {
            $taskConfigured = Get-CimInstance Win32_Service -Filter "Name='GameGauge.Sensor'"
            if ($taskConfigured.PathName -ne $taskServiceCommand) {
                Stop-Service 'GameGauge.Sensor'
                $taskChange = Invoke-CimMethod -InputObject $taskConfigured -MethodName Change -Arguments @{PathName=$taskServiceCommand}
                if ($taskChange.ReturnValue -ne 0) { throw "温度服务路径修复失败：$($taskChange.ReturnValue)" }
            }
            Set-Service 'GameGauge.Sensor' -StartupType Automatic
        }
        $taskServiceControl = Join-Path ([Environment]::SystemDirectory) 'sc.exe'
        $taskResult = & $taskServiceControl failure GameGauge.Sensor reset= 86400 actions= restart/5000/restart/15000/restart/60000 2>&1
        if ($LASTEXITCODE -ne 0) { throw "温度服务恢复策略设置失败：$taskResult" }
        $taskResult = & $taskServiceControl failureflag GameGauge.Sensor 1 2>&1
        if ($LASTEXITCODE -ne 0) { throw "温度服务错误恢复设置失败：$taskResult" }
        Start-Service 'GameGauge.Sensor'
        (Get-Service 'GameGauge.Sensor').WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(15))
        if (!$taskReboot) {
            # 驱动存在不代表可用；用实际温度读取验证修复结果。
            & $taskProbe (Join-Path $taskRoot '.deps/AMDFamily17-0.2.11.bin')
            if ($LASTEXITCODE -ne 0) { throw 'CPU 温度探针仍无法读取，请查看上述具体错误。' }
        }
    }
    $taskExitCode = if ($taskReboot) { 3010 } else { 0 }
    Write-Host $(if ($taskReboot) { '需要重启 Windows 完成组件修复。' } else { '采集组件修复完成。' })
} catch { Write-Output $_ }
finally {
    if ($taskTranscript) { Stop-Transcript | Out-Null }
    if ($taskLocked) { $taskLock.ReleaseMutex() }
    $taskLock.Dispose()
}
exit $taskExitCode
