$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBin = Join-Path $taskRoot 'build/core/bin/Release'
if (Get-Process GameGauge,GameGauge.Settings -ErrorAction SilentlyContinue) { throw '请先退出正在运行的游戏仪表，避免干扰真实会话。' }
$taskData = Join-Path $taskRoot ('build/runtime-test/history-ui-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$taskData/history" -Force | Out-Null
$taskStart = [uint64]([DateTimeOffset]::Now.ToUnixTimeMilliseconds() - 1800000)
$taskId = "$taskStart-777"
$taskSeries = @(); $taskStats = @{}
$taskIds = @('fps','cpu_load','cpu_temperature','gpu_load','gpu_temperature','memory_load','disk_temperature','frametime','gpu_power','vram')
for ($taskIndex = 0; $taskIndex -lt 120; $taskIndex++) {
    $taskWave = [Math]::Sin($taskIndex / 7)
    $taskSeries += ,@(($taskIndex * 15), (110 + $taskWave * 15), (23 + $taskWave * 6), (65 + $taskWave * 5), (76 + $taskWave * 10), (60 + $taskWave * 7), (39 + $taskWave * 2), (48 + $taskWave * 3), (9 + $taskWave), (190 + $taskWave * 20), (10 + $taskWave))
}
for ($taskIndex = 0; $taskIndex -lt $taskIds.Count; $taskIndex++) {
    $taskValues = @($taskSeries | ForEach-Object { $_[$taskIndex + 1] })
    $taskSummary = $taskValues | Measure-Object -Average -Minimum -Maximum
    $taskStats[$taskIds[$taskIndex]] = @{average=$taskSummary.Average;minimum=$taskSummary.Minimum;maximum=$taskSummary.Maximum}
}
$taskSeries[50] = @(750, $null, $null, $null, $null, $null, $null, $null, $null, $null, $null)
$taskRecord = @{version=1;id=$taskId;game='视觉验收样本.exe';started_ms=$taskStart;updated_ms=($taskStart+1800000);status='completed';active_seconds=1785;duration_seconds=1800;stats=$taskStats;series_metrics=$taskIds;series=$taskSeries;series_interval_seconds=15;events=@(@{at_ms=($taskStart+750000);type='background'},@{at_ms=($taskStart+765000);type='resumed'});latest_low1=82;latest_low01=68;hardware=@{cpu='视觉验收处理器';logical_processors=16;selected_gpu='test';gpus=@(@{id='test';name='视觉验收显卡'});memory_total=@{value=34359738368};disks=@(@{name='视觉验收 SSD'});displays=@(@{name='视觉验收显示器';width=2560;height=1440;dpi=144;hdr=$true})}}
[IO.File]::WriteAllText("$taskData/history/$taskId.json", ($taskRecord | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
foreach ($taskDayOffset in 1..6) {
    $taskOlder = $taskRecord.Clone(); $taskOlderStart = $taskStart - [uint64]($taskDayOffset * 86400000)
    $taskOlder.id = "$taskOlderStart-777"; $taskOlder.started_ms = $taskOlderStart; $taskOlder.updated_ms = $taskOlderStart + 1800000
    [IO.File]::WriteAllText("$taskData/history/$($taskOlder.id).json", ($taskOlder | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}
$taskHost = Start-Process -FilePath "$taskBin/GameGauge.exe" -ArgumentList @('--background','--data-dir',('"'+$taskData+'"')) -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Seconds 2
    $taskList = & "$taskBin/GameGauge.Diagnostics.exe" --request '{"command":"history","limit":2}' | ConvertFrom-Json
    if (!$taskList.ok -or $taskList.total -ne 7 -or $taskList.history.Count -ne 2 -or $taskList.dates.Count -ne 7) { throw '历史列表接口或分页失败' }
    $taskDay = $taskList.dates[0]
    $taskFilter = @{command='history';day=$taskDay} | ConvertTo-Json -Compress
    $taskFiltered = & "$taskBin/GameGauge.Diagnostics.exe" --request $taskFilter | ConvertFrom-Json
    if (!$taskFiltered.ok -or $taskFiltered.total -ne 1 -or $taskFiltered.active_seconds -ne 1785) { throw '按日期筛选或时长汇总失败' }
    if ($taskList.history[0].PSObject.Properties['series']) { throw '历史列表携带过大曲线数据' }
    $taskRequest = @{command='history_detail';id=$taskId} | ConvertTo-Json -Compress
    $taskDetail = & "$taskBin/GameGauge.Diagnostics.exe" --request $taskRequest | ConvertFrom-Json
    if (!$taskDetail.ok -or $taskDetail.record.series.Count -ne 120) { throw '历史详情接口失败' }
    foreach ($taskPage in 0..4) {
        $env:GAMEGAUGE_SETTINGS_PAGE = [string]$taskPage
        $env:GAMEGAUGE_SETTINGS_SNAPSHOT = "$taskData/page-$taskPage.png"
        $taskShot = Start-Process -FilePath "$taskBin/GameGauge.Settings.exe" -WindowStyle Hidden -PassThru -Wait
        if ($taskShot.ExitCode -ne 0 -or !(Test-Path $env:GAMEGAUGE_SETTINGS_SNAPSHOT)) { throw "页面 $taskPage 截图失败" }
    }
    $env:GAMEGAUGE_SETTINGS_PAGE = '3'; $env:GAMEGAUGE_SETTINGS_HISTORY_ID = $taskId
    foreach ($taskTab in 0..3) {
        $env:GAMEGAUGE_SETTINGS_HISTORY_TAB = [string]$taskTab
        $env:GAMEGAUGE_SETTINGS_SNAPSHOT = "$taskData/history-$taskTab.png"
        $taskShot = Start-Process -FilePath "$taskBin/GameGauge.Settings.exe" -WindowStyle Hidden -PassThru -Wait
        if ($taskShot.ExitCode -ne 0 -or !(Test-Path $env:GAMEGAUGE_SETTINGS_SNAPSHOT)) { throw "历史标签 $taskTab 截图失败" }
    }
    Remove-Item Env:GAMEGAUGE_SETTINGS_HISTORY_ID
    $env:GAMEGAUGE_SETTINGS_PAGE = '1'; $env:GAMEGAUGE_SETTINGS_DISKS = '1'
    $env:GAMEGAUGE_SETTINGS_SNAPSHOT = "$taskData/disks.png"
    $taskShot = Start-Process -FilePath "$taskBin/GameGauge.Settings.exe" -WindowStyle Hidden -PassThru -Wait
    if ($taskShot.ExitCode -ne 0) { throw '硬盘详情截图失败' }
    Remove-Item Env:GAMEGAUGE_SETTINGS_DISKS
    foreach ($taskDpi in @(96,120,144)) {
        $env:GAMEGAUGE_SETTINGS_DPI = [string]$taskDpi
        foreach ($taskPage in 0..4) {
            $env:GAMEGAUGE_SETTINGS_PAGE = [string]$taskPage
            $env:GAMEGAUGE_SETTINGS_SNAPSHOT = "$taskData/dpi-$taskDpi-page-$taskPage.png"
            $taskShot = Start-Process -FilePath "$taskBin/GameGauge.Settings.exe" -WindowStyle Hidden -PassThru -Wait
            if ($taskShot.ExitCode -ne 0) { throw "DPI $taskDpi 页面 $taskPage 截图失败" }
        }
        $env:GAMEGAUGE_SETTINGS_PAGE = '3'; $env:GAMEGAUGE_SETTINGS_HISTORY_ID = $taskId; $env:GAMEGAUGE_SETTINGS_HISTORY_TAB = '1'
        $env:GAMEGAUGE_SETTINGS_SNAPSHOT = "$taskData/dpi-$taskDpi-charts.png"
        $taskShot = Start-Process -FilePath "$taskBin/GameGauge.Settings.exe" -WindowStyle Hidden -PassThru -Wait
        if ($taskShot.ExitCode -ne 0) { throw "DPI $taskDpi 图表截图失败" }
        Remove-Item Env:GAMEGAUGE_SETTINGS_HISTORY_ID
    }
    Write-Host "历史列表与详情、全部设置页、报告/图表/事件/设备信息及硬盘详情：通过。样本截图：$taskData"
} finally {
    foreach ($taskName in @('GAMEGAUGE_SETTINGS_PAGE','GAMEGAUGE_SETTINGS_HISTORY_ID','GAMEGAUGE_SETTINGS_HISTORY_TAB','GAMEGAUGE_SETTINGS_DISKS','GAMEGAUGE_SETTINGS_SNAPSHOT','GAMEGAUGE_SETTINGS_DPI')) { Remove-Item "Env:$taskName" -ErrorAction SilentlyContinue }
    & "$taskBin/GameGauge.Diagnostics.exe" --request '{"command":"quit"}' | Out-Null
    if (!$taskHost.WaitForExit(5000)) { Stop-Process -Id $taskHost.Id }
}
