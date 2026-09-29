$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskFolder = Join-Path $taskRoot ('build/runtime-test/locks-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskFolder -Force | Out-Null
$taskFile = Join-Path $taskFolder 'GameGauge.exe'
[IO.File]::WriteAllText($taskFile, 'test')
$taskLock = [IO.File]::Open($taskFile, 'Open', 'ReadWrite', 'None')
$taskJob = $null
try {
    $taskJob = Start-Job -ArgumentList $taskFolder,(Join-Path $PSScriptRoot 'setup-processes.ps1') -ScriptBlock {
        param($folder,$helper)
        $ErrorActionPreference = 'Stop'
        . $helper
        Stop-GameGaugeProcesses -Directory $folder
        [IO.File]::WriteAllText((Join-Path $folder 'GameGauge.exe'), 'replaced')
    }
    Start-Sleep -Seconds 3
    if ($taskJob.State -eq 'Completed') { throw '安装必须等待文件解锁' }
    $taskLock.Dispose()
    $taskJob | Wait-Job -Timeout 10 | Out-Null
    $taskJob | Receive-Job -ErrorAction Stop
    if ($taskJob.State -ne 'Completed' -or [IO.File]::ReadAllText($taskFile) -ne 'replaced') { throw '文件解锁后的替换失败' }
    Write-Output '安装文件锁等待与替换：通过'
} finally {
    $taskLock.Dispose()
    if ($taskJob) { $taskJob | Stop-Job; $taskJob | Remove-Job }
}
