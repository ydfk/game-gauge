param([string]$BinPath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBin = if ($BinPath) { (Resolve-Path -LiteralPath $BinPath).Path } else { Join-Path $taskRoot 'build/core/bin/Release' }
$taskHostExe = Join-Path $taskBin 'GameGauge.exe'
$taskDiagnostics = Join-Path $taskBin 'GameGauge.Diagnostics.exe'
$taskData = Join-Path $taskRoot 'build/runtime-test/smoke'

if (!(Test-Path -LiteralPath $taskHostExe) -or !(Test-Path -LiteralPath $taskDiagnostics)) {
    throw '请先运行 scripts/build-core.ps1。'
}
if (Get-Process GameGauge -ErrorAction SilentlyContinue) {
    throw '已有游戏仪表进程在运行；为避免修改其配置，请先退出后再执行冒烟测试。'
}
New-Item -ItemType Directory -Path $taskData -Force | Out-Null
$taskProcess = Start-Process -FilePath $taskHostExe `
    -ArgumentList @('--background', '--exit-after-ms', '15000', '--data-dir', '"' + $taskData + '"') `
    -WindowStyle Hidden -PassThru
try {
    $taskStatus = $null
    for ($taskTry = 0; $taskTry -lt 30; $taskTry++) {
        if ($taskProcess.HasExited) { throw "宿主程序提前退出，代码 $($taskProcess.ExitCode)。" }
        $taskRaw = & $taskDiagnostics --host-status 2>$null
        if ($LASTEXITCODE -eq 0) { $taskStatus = $taskRaw | ConvertFrom-Json; break }
        Start-Sleep -Milliseconds 150
    }
    if (!$taskStatus -or !$taskStatus.ok) { throw '宿主 IPC 在超时前未就绪。' }
    if (!$taskStatus.snapshot.PSObject.Properties['cpu']) { throw '诊断协议结构意外变化。' }

    $taskConfig = $taskStatus.config
    $taskConfig.font_size = 17
    $taskConfig.opacity = 0.75
    $taskConfig.metrics = @('fps', 'gpu_load')
    $taskRequest = @{ command = 'apply'; config = $taskConfig } | ConvertTo-Json -Compress -Depth 8
    $taskApplied = (& $taskDiagnostics --request $taskRequest | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or !$taskApplied.ok) { throw '配置应用失败。' }

    $taskAfter = (& $taskDiagnostics --host-status | ConvertFrom-Json)
    if ($taskAfter.config.font_size -ne 17 -or $taskAfter.config.opacity -ne 0.75 -or
        (@($taskAfter.config.metrics) -join ',') -ne 'fps,gpu_load') { throw 'IPC 配置读回与提交不一致。' }
    if (!(Test-Path -LiteralPath (Join-Path $taskData 'config.json'))) { throw '配置没有保存到隔离测试目录。' }
    & $taskDiagnostics --request '{"command":"settings"}' | Out-Null
    $taskSettings = $null
    for ($taskTry = 0; $taskTry -lt 30; $taskTry++) {
        $taskSettings = Get-Process GameGauge.Settings -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $taskBin 'GameGauge.Settings.exe') } | Select-Object -First 1
        if ($taskSettings) { break }
        Start-Sleep -Milliseconds 100
    }
    if (!$taskSettings) { throw '设置窗口没有随主程序启动' }
    Start-Sleep -Milliseconds 1500
    & $taskDiagnostics --request '{"command":"quit"}' | Out-Null
    if ($LASTEXITCODE -ne 0 -or !$taskProcess.WaitForExit(5000) -or $taskProcess.ExitCode -ne 0) {
        throw 'IPC 退出未能正常关闭宿主。'
    }
    if (!$taskSettings.WaitForExit(4000)) { throw '托盘退出后设置进程仍在运行' }
    Write-Host '宿主启动、连续 IPC、配置应用、持久化、托盘退出同时关闭设置：通过。'
} finally {
    if (!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    $taskProcess.WaitForExit(2000) | Out-Null
}
