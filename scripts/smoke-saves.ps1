param([string]$BinPath)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
if (!$BinPath) { $BinPath = Join-Path $taskRepo 'build/core/bin/Release' }
$taskBin = (Resolve-Path -LiteralPath $BinPath).Path
$taskHost = Join-Path $taskBin 'GameGauge.exe'
$taskDiag = Join-Path $taskBin 'GameGauge.Diagnostics.exe'
$taskWorker = Join-Path $taskBin 'GameGauge.Saves.exe'
if (Get-Process GameGauge,GameGauge.Settings,GameGauge.Saves -ErrorAction SilentlyContinue) {
    throw '已有游戏仪表进程，请在独立会话执行测试。'
}
$taskRoot = Join-Path $taskRepo ('build/runtime-test/saves-' + [guid]::NewGuid().ToString('N'))
$taskSlot = Join-Path $taskRoot '测试存档/76561198000000001'
$taskData = Join-Path $taskRoot 'data'
$taskSource = Join-Path $taskSlot 'PBZSaveGame.sav'
New-Item -ItemType Directory -Path $taskSlot,$taskData -Force | Out-Null
[IO.File]::WriteAllText($taskSource, 'synthetic save 1')
$taskTarget = @{game_id='steam-4115450';profile='76561198000000001'}

function Invoke-GameGauge([hashtable]$Request) {
    $taskJson = $Request | ConvertTo-Json -Compress -Depth 10
    if ($PSVersionTable.PSVersion.Major -lt 7 -or $PSNativeCommandArgumentPassing -eq 'Legacy') {
        $taskJson = $taskJson.Replace('"','\"')
    }
    $taskRaw = & $taskDiag --request $taskJson
    if (!$taskRaw) { throw '未收到 IPC 响应' }
    $taskRaw | ConvertFrom-Json
}
function Wait-SavesReady {
    for ($taskTry = 0; $taskTry -lt 80; ++$taskTry) {
        try {
            $taskStatus = Invoke-GameGauge @{command='saves.host.status'}
            if ($taskStatus.state -eq 'ready') { return }
        } catch { $taskLast = $_.Exception.Message }
        Start-Sleep -Milliseconds 150
    }
    throw "存档工作进程未就绪 $($taskStatus.error) $taskLast"
}
function Wait-SavesJob([string]$Id) {
    for ($taskTry = 0; $taskTry -lt 100; ++$taskTry) {
        $taskState = Invoke-GameGauge @{command='saves.job.status';job_id=$Id}
        if ($taskState.state -eq 'succeeded') { return $taskState.result }
        if (!$taskState.ok -or $taskState.state -in @('failed','interrupted')) { throw $taskState.error }
        Start-Sleep -Milliseconds 100
    }
    throw '存档任务未完成'
}
function Invoke-Saves([hashtable]$Request) {
    if (!$Request.request_id) { $Request.request_id = [guid]::NewGuid().ToString() }
    $taskQueued = Invoke-GameGauge $Request
    if (!$taskQueued.ok) { throw $taskQueued.error }
    Wait-SavesJob $taskQueued.job_id
}
function Start-FakeHost($Executable = $taskHost) {
    $taskArguments = @('--background','--exit-after-ms','60000','--data-dir',('"' + $taskData + '"'))
    Start-Process -FilePath $Executable -ArgumentList $taskArguments -WindowStyle Hidden -PassThru
}

$taskProcess = Start-FakeHost
try {
    Wait-SavesReady
    Invoke-Saves @{
        command='saves.configure';game_id=$taskTarget.game_id;profile='';save_dir=$taskSlot;backup_dir=''
        interval_minutes=10;keep_auto=3;hotkey='';enabled=$false;auto_enabled=$true
    } | Out-Null
    $taskRequest = $taskTarget + @{command='saves.backup.enqueue';request_id=[guid]::NewGuid().ToString()}
    $taskFirst = Invoke-GameGauge $taskRequest
    $taskDuplicate = Invoke-GameGauge $taskRequest
    if (!$taskFirst.ok -or $taskFirst.job_id -ne $taskDuplicate.job_id) { throw '重复请求创建了新任务' }
    if (!(Invoke-GameGauge @{command='status'}).ok) { throw '运行备份时监控 IPC 失败' }
    $taskRecord = Wait-SavesJob $taskFirst.job_id
    $taskAction = $taskTarget + @{record_id=$taskRecord.record_id}
    Invoke-Saves ($taskAction + @{command='saves.records.verify'}) | Out-Null
    Invoke-Saves ($taskAction + @{command='saves.records.edit';title='测试名称';note='测试中文备注'}) | Out-Null
    Invoke-Saves ($taskAction + @{command='saves.records.protect';locked=$true}) | Out-Null
    $taskDetail = Invoke-Saves ($taskAction + @{command='saves.records.detail'})
    if (!$taskDetail.record.locked -or $taskDetail.record.note -ne '测试中文备注') { throw '备份元数据写入失败' }
    [IO.File]::WriteAllText($taskSource, 'synthetic save 2')
    Invoke-Saves ($taskAction + @{command='saves.restore.enqueue';confirmed=$true}) | Out-Null
    if ([IO.File]::ReadAllText($taskSource) -ne 'synthetic save 1') { throw '合成存档恢复失败' }
    $taskPage = Invoke-Saves ($taskTarget + @{command='saves.records.list';limit=1})
    if ($taskPage.total -ne 2 -or $taskPage.next -ne 1 -or $taskPage.records[0].trigger -ne 'before_restore') {
        throw '恢复前保护和分页结果异常'
    }
    $taskChild = Get-Process GameGauge.Saves | Where-Object Path -eq $taskWorker | Select-Object -First 1
    if (!$taskChild) { throw '找不到本次测试的存档工作进程' }
    $taskOldPid = $taskChild.Id
    $taskChild.Kill()
    $taskChild.WaitForExit(2000) | Out-Null
    if (!(Invoke-GameGauge @{command='status'}).ok) { throw '工作进程异常时监控 IPC 失败' }
    for ($taskTry = 0; $taskTry -lt 80; ++$taskTry) {
        $taskChild = Get-Process GameGauge.Saves -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $taskWorker -and $_.Id -ne $taskOldPid } | Select-Object -First 1
        if ($taskChild) { break }
        Start-Sleep -Milliseconds 150
    }
    if (!$taskChild) { throw '异常退出后未重启工作进程' }
    Wait-SavesReady
    $taskPage = Invoke-Saves ($taskTarget + @{command='saves.records.list';limit=6})
    if ($taskPage.total -ne 2) { throw '工作进程重启后本地库丢失' }
    Invoke-GameGauge @{command='quit'} | Out-Null
    if (!$taskProcess.WaitForExit(17000) -or !$taskChild.WaitForExit(2000)) { throw '退出后宿主或工作进程仍在运行' }
    $taskProcess = Start-FakeHost
    Wait-SavesReady
    $taskPrevious = Wait-SavesJob $taskFirst.job_id
    if ($taskPrevious.record_id -ne $taskRecord.record_id) { throw '重启后无法查询持久化任务结果' }
    $taskReplay = Invoke-Saves $taskRequest
    if ($taskReplay.record_id -ne $taskRecord.record_id) { throw '重启后重复投递创建了新备份' }
    Invoke-GameGauge @{command='quit'} | Out-Null
    if (!$taskProcess.WaitForExit(17000)) { throw '重启后的宿主未退出' }
    $taskMissing = Join-Path $taskRoot 'no-worker'
    New-Item -ItemType Directory -Path $taskMissing | Out-Null
    Copy-Item -LiteralPath $taskHost -Destination $taskMissing
    $taskProcess = Start-FakeHost (Join-Path $taskMissing 'GameGauge.exe')
    for ($taskTry = 0; $taskTry -lt 40; ++$taskTry) {
        try { $taskStatus = Invoke-GameGauge @{command='saves.host.status'} } catch {}
        if ($taskStatus.state -eq 'unavailable') { break }
        Start-Sleep -Milliseconds 150
    }
    if ($taskStatus.state -ne 'unavailable' -or !(Invoke-GameGauge @{command='status'}).ok) { throw '组件缺失时宿主降级失败' }
    Invoke-GameGauge @{command='quit'} | Out-Null
    if (!$taskProcess.WaitForExit(5000)) { throw '缺少工作进程时未正常退出' }
    Write-Output '备份、重复投递、跨重启任务查询、中文元数据、保护恢复、分页、工作进程异常重启、缺失降级与关闭：通过'
} finally {
    if (!$taskProcess.HasExited) { $taskProcess.Kill(); $taskProcess.WaitForExit(5000) | Out-Null }
}
