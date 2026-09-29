# 安装和卸载共用：仅停止指定安装目录内的程序，并等待映像文件释放。
function Stop-GameGaugeProcesses {
    param([Parameter(Mandatory=$true)][string]$Directory)
    $taskRoot = [IO.Path]::GetFullPath($Directory).TrimEnd('\') + '\'
    $taskNames = @('GameGauge','GameGauge.Settings','GameGauge.Diagnostics','GameGauge.CpuProbe')
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $taskProcesses = @(Get-Process -Name $taskNames -ErrorAction SilentlyContinue | Where-Object {
            $_.Path -and [IO.Path]::GetFullPath($_.Path).StartsWith($taskRoot, [StringComparison]::OrdinalIgnoreCase)
        })
        foreach ($taskProcess in $taskProcesses) {
            Write-Output "等待程序退出：$($taskProcess.ProcessName) PID $($taskProcess.Id)"
            try {
                if (!$taskProcess.HasExited) { $taskProcess.Kill() }
                if (!$taskProcess.WaitForExit(5000)) { throw "程序未退出：$($taskProcess.ProcessName)" }
            } finally { $taskProcess.Dispose() }
        }
        $taskLocked = @()
        foreach ($taskName in $taskNames) {
            $taskFile = Join-Path $taskRoot ($taskName + '.exe')
            if (!(Test-Path -LiteralPath $taskFile)) { continue }
            try {
                $taskStream = [IO.File]::Open($taskFile, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                $taskStream.Dispose()
            } catch [IO.IOException] { $taskLocked += $taskFile }
        }
        if (!$taskLocked.Count) { return }
        if ([DateTime]::UtcNow -ge $taskDeadline) { throw "程序文件仍被占用，尚未覆盖文件：$($taskLocked -join ', ')" }
        Start-Sleep -Milliseconds 200
    } while ($true)
}
