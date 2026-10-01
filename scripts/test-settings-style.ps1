$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBin = Join-Path $taskRoot 'build/core/bin/Release'
$taskOutput = Join-Path $taskRoot ('build/runtime-test/settings-style-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
function New-StyleMetric([double]$Value) { return @{state='valid';value=$Value;source='界面验收样本'} }
$taskIds = @('fps','frametime','low1','low01','cpu_temperature','cpu_load','cpu_clock','process_cpu','gpu_temperature','gpu_load','gpu_clock','gpu_power','gpu_fan','vram','memory_load','memory_used','process_memory','session','disk_temperature')
$taskSnapshot = @{obs_state='idle';selected_gpu='preview';session_seconds=3725;cpu='界面验收处理器';logical_processors=16;
    target=@{name='界面验收游戏.exe';pid=777};cpu_load=(New-StyleMetric 82);cpu_clock=(New-StyleMetric 5200);cpu_temperature=(New-StyleMetric 86);
    fps=(New-StyleMetric 118);frametime=(New-StyleMetric 8.5);low1=(New-StyleMetric 54);low01=(New-StyleMetric 24);
    memory_load=(New-StyleMetric 94);memory_used=(New-StyleMetric (30*1GB));memory_total=(New-StyleMetric (32*1GB));
    process_cpu=(New-StyleMetric 35);process_memory=(New-StyleMetric (5*1GB));disk_temperature=(New-StyleMetric 62);
    gpus=@(@{id='preview';name='界面验收显卡';load=(New-StyleMetric 100);temperature=(New-StyleMetric 72);clock=(New-StyleMetric 2600);power=(New-StyleMetric 285);fan=(New-StyleMetric 45);memory_used=(New-StyleMetric (15.5*1GB));memory_total=(New-StyleMetric (16*1GB))});
    disks=@(@{name='NVMe SSD 1';temperature=(New-StyleMetric 43)},@{name='NVMe SSD 2';temperature=(New-StyleMetric 62)},@{name='NVMe SSD 3';temperature=(New-StyleMetric 73)});
    displays=@(@{name='界面验收显示器';width=2560;height=1440;dpi=144;hdr=$true})}
$taskStart = [uint64]([DateTimeOffset]::Now.ToUnixTimeMilliseconds()-3600000)
$taskRecord = @{id="$taskStart-777";version=1;game='界面验收游戏.exe';started_ms=$taskStart;updated_ms=($taskStart+3600000);active_seconds=3525;duration_seconds=3600;status='completed';latest_low1=54;latest_low01=24;
    hardware=$taskSnapshot;events=@(@{at_ms=($taskStart+1200000);type='background'},@{at_ms=($taskStart+1250000);type='resumed'});stats=@{};series_metrics=@('fps','cpu_load','cpu_temperature','gpu_load','gpu_temperature','memory_load','disk_temperature','frametime','gpu_power','vram');series=@()}
$taskValues = @(118,82,86,100,72,94,62,8.5,285,15.5)
for ($taskIndex=0; $taskIndex -lt $taskValues.Count; $taskIndex++) {
    $taskRecord.stats[$taskRecord.series_metrics[$taskIndex]]=@{average=$taskValues[$taskIndex];minimum=($taskValues[$taskIndex]*.8);maximum=($taskValues[$taskIndex]*1.05)}
    if ($taskRecord.series_metrics[$taskIndex] -in @('cpu_load','gpu_load','memory_load')) {
        $taskRecord.stats[$taskRecord.series_metrics[$taskIndex]].maximum=[Math]::Min(100,$taskRecord.stats[$taskRecord.series_metrics[$taskIndex]].maximum)
    }
}
foreach ($taskIndex in 0..119) {
    $taskPoint = @(($taskIndex*30))
    foreach ($taskValue in $taskValues) { $taskPoint += ($taskValue*(.95+.05*[Math]::Sin($taskIndex/6))) }
    if ($taskIndex -eq 40) { $taskPoint = @(($taskIndex*30),$null,$null,$null,$null,$null,$null,$null,$null,$null,$null) }
    $taskRecord.series += ,$taskPoint
}
$taskFixture = @{status=@{config=@{version=1;metrics=$taskIds;show_obs=$true};snapshot=$taskSnapshot;
    update=@{current='0.1.10';message='当前已是最新版本';state='latest';repository='ydfk/game-gauge'};
    hud_preview=@(@{label='FPS';value='118';group='帧率';color=@(.459,.875,.69)},@{label='CPU';value='86°C  82%';group='CPU';color=@(1,.667,.408)},@{label='GPU';value='72°C  100%';group='GPU';color=@(1,.667,.408)},@{label='OBS';value='● 未录制';group='OBS';color=@(1,.667,.408)})};
    targets=@(@{name='界面验收游戏.exe';path='C:\Preview\game.exe';pid=777});history=@($taskRecord);history_detail=$taskRecord;active_seconds=3525;dates=@([DateTime]::Now.ToString('yyyy-MM-dd'))}
$taskFixturePath = Join-Path $taskOutput 'fixture.json'
$taskEnvNames = @('GAMEGAUGE_SETTINGS_FIXTURE','GAMEGAUGE_SETTINGS_SNAPSHOT','GAMEGAUGE_SETTINGS_PAGE','GAMEGAUGE_SETTINGS_DPI','GAMEGAUGE_SETTINGS_HISTORY_ID','GAMEGAUGE_SETTINGS_HISTORY_TAB','GAMEGAUGE_SETTINGS_DISKS')
$taskPrevious = @{}; foreach ($taskName in $taskEnvNames) { $taskPrevious[$taskName] = [Environment]::GetEnvironmentVariable($taskName) }
function Save-StyleFixture { [IO.File]::WriteAllText($taskFixturePath,($taskFixture | ConvertTo-Json -Depth 15),[Text.UTF8Encoding]::new($false)) }
function Invoke-StyleShot([string]$Name,[int]$Page) {
    $env:GAMEGAUGE_SETTINGS_PAGE=[string]$Page; $env:GAMEGAUGE_SETTINGS_SNAPSHOT=Join-Path $taskOutput "$Name.png"
    $taskShot = Start-Process -FilePath "$taskBin/GameGauge.Settings.exe" -WindowStyle Hidden -PassThru -Wait
    if ($taskShot.ExitCode -ne 0 -or !(Test-Path -LiteralPath $env:GAMEGAUGE_SETTINGS_SNAPSHOT)) { throw "界面渲染失败：$Name" }
}
try {
    foreach ($taskName in $taskEnvNames) { [Environment]::SetEnvironmentVariable($taskName,$null) }
    $env:GAMEGAUGE_SETTINGS_FIXTURE=$taskFixturePath; Save-StyleFixture
    foreach ($taskDpi in @(96,120,144)) {
        $env:GAMEGAUGE_SETTINGS_DPI=[string]$taskDpi
        foreach ($taskPage in 0..4) { Invoke-StyleShot "dpi-$taskDpi-page-$taskPage" $taskPage }
    }
    $env:GAMEGAUGE_SETTINGS_DPI='96'; $env:GAMEGAUGE_SETTINGS_HISTORY_ID=$taskRecord.id
    foreach ($taskTab in 0..3) { $env:GAMEGAUGE_SETTINGS_HISTORY_TAB=[string]$taskTab; Invoke-StyleShot "history-$taskTab" 3 }
    Remove-Item Env:GAMEGAUGE_SETTINGS_HISTORY_ID
    foreach ($taskState in @('recording','paused','idle','disconnected')) {
        $taskSnapshot.obs_state=$taskState; Save-StyleFixture; Invoke-StyleShot "obs-$taskState" 1
    }
    $env:GAMEGAUGE_SETTINGS_DISKS='1'; Invoke-StyleShot 'disks' 1
    Remove-Item Env:GAMEGAUGE_SETTINGS_DISKS
    $taskRecord.stats.fps.minimum=$null; $taskRecord.legacy_values_hidden=$true; Save-StyleFixture
    $env:GAMEGAUGE_SETTINGS_HISTORY_ID=$taskRecord.id; $env:GAMEGAUGE_SETTINGS_HISTORY_TAB='0'
    Invoke-StyleShot 'legacy-no-minimum' 3
    Write-Host "全部设置页、历史详情、OBS 四种状态与硬盘温度在隔离样本中渲染通过：$taskOutput"
} finally {
    foreach ($taskName in $taskEnvNames) { [Environment]::SetEnvironmentVariable($taskName,$taskPrevious[$taskName]) }
}
