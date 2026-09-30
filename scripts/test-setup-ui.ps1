$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskPreview = Join-Path $taskRoot 'build/core/bin/Release/GameGauge.SetupPreview.exe'
if (!(Test-Path -LiteralPath $taskPreview)) { throw '请先运行 scripts/build-core.ps1。' }
if (Get-Process GameGauge.SetupPreview -ErrorAction SilentlyContinue) { throw '请先关闭安装界面预览窗口。' }
$taskData = Join-Path $taskRoot ('build/runtime-test/setup-ui-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskData -Force | Out-Null
foreach ($taskDpi in @(96,120,144)) {
    foreach ($taskState in @('installing','success','failure')) {
        $taskImage = Join-Path $taskData "$taskState-$taskDpi.png"
        $taskProcess = Start-Process -FilePath $taskPreview -ArgumentList @('--snapshot',('"'+$taskImage+'"'),'--state',$taskState,'--progress','60','--dpi',$taskDpi,'--lock-close') -WindowStyle Hidden -PassThru -Wait
        if ($taskProcess.ExitCode -ne 0 -or !(Test-Path -LiteralPath $taskImage)) { throw "$taskState / DPI $taskDpi 渲染失败。" }
    }
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SetupNative {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr rect, IntPtr region, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out Rect r);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr GetProp(IntPtr h, string key);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    public static void Click(IntPtr h, int x, int y) {
        Rect r; GetClientRect(h, out r); var s = r.Right / 760.0;
        var p = new IntPtr(unchecked((int)(((uint)(y*s) & 65535) << 16 | ((uint)(x*s) & 65535))));
        SendMessage(h, 0x202, IntPtr.Zero, p);
    }
}
'@
function Wait-PreviewWindow {
    for ($taskTry = 0; $taskTry -lt 30; $taskTry++) {
        $taskHandle = [SetupNative]::FindWindow('GameGauge.Installer.Preview', [NullString]::Value)
        if ($taskHandle -ne [IntPtr]::Zero) {
            [SetupNative]::ShowWindow($taskHandle,4) | Out-Null
            [SetupNative]::RedrawWindow($taskHandle,[IntPtr]::Zero,[IntPtr]::Zero,0x101) | Out-Null
            return $taskHandle
        }
        if ($taskProcess.HasExited) { throw '预览程序提前退出。' }
        Start-Sleep -Milliseconds 100
    }
    throw '预览窗口未就绪。'
}
$taskPreviousDpi = [SetupNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
$taskProcess = $null
try {
    # 预览程序与真实安装器共用界面，不包含安装执行入口或驱动资源。
    $taskProcess = Start-Process -FilePath $taskPreview -ArgumentList @('--state','installing','--lock-close') -WindowStyle Hidden -PassThru
    $taskHandle = Wait-PreviewWindow
    [SetupNative]::SendMessage($taskHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    [SetupNative]::SendMessage($taskHandle,0x100,[IntPtr]27,[IntPtr]::Zero) | Out-Null
    [SetupNative]::Click($taskHandle,720,30)
    [SetupNative]::Click($taskHandle,660,450)
    if ($taskProcess.WaitForExit(200)) { throw '安装中可被关闭。' }
    if ([SetupNative]::GetProp($taskHandle,'GameGauge.Setup.Completed') -ne [IntPtr]::Zero) { throw '安装中被错误标记为完成。' }
    [SetupNative]::SendMessage($taskHandle,0x8001,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    if ([SetupNative]::GetProp($taskHandle,'GameGauge.Setup.Completed') -eq [IntPtr]::Zero) { throw '缺少安装完成标记。' }
    [SetupNative]::Click($taskHandle,660,450)
    if (!$taskProcess.WaitForExit(2000) -or $taskProcess.ExitCode -ne 0) { throw '完成按钮无效。' }

    $taskActionLog = Join-Path $taskData 'actions.txt'
    $taskProcess = Start-Process -FilePath $taskPreview -ArgumentList @('--state','installing','--lock-close','--action-log',('"'+$taskActionLog+'"')) -WindowStyle Hidden -PassThru
    $taskHandle = Wait-PreviewWindow
    [SetupNative]::SendMessage($taskHandle,0x8001,[IntPtr]1,[IntPtr]::Zero) | Out-Null
    [SetupNative]::Click($taskHandle,660,450)
    if (!(Test-Path -LiteralPath $taskActionLog) -or (Get-Content -LiteralPath $taskActionLog -Raw).Trim() -ne 'logs') { throw '失败状态日志按钮无效。' }
    if ($taskProcess.HasExited) { throw '打开日志错误地关闭了窗口。' }
    [SetupNative]::SendMessage($taskHandle,0x100,[IntPtr]9,[IntPtr]::Zero) | Out-Null
    [SetupNative]::SendMessage($taskHandle,0x100,[IntPtr]13,[IntPtr]::Zero) | Out-Null
    if (@(Get-Content -LiteralPath $taskActionLog).Count -ne 2) { throw '键盘日志入口无效。' }
    [SetupNative]::SendMessage($taskHandle,0x100,[IntPtr]9,[IntPtr]::Zero) | Out-Null
    [SetupNative]::SendMessage($taskHandle,0x100,[IntPtr]32,[IntPtr]::Zero) | Out-Null
    if (!$taskProcess.WaitForExit(2000) -or $taskProcess.ExitCode -ne 1) { throw '失败状态键盘关闭无效。' }
    Write-Host "三种安装状态、100%/125%/150% DPI、安装中关闭保护、完成按钮、日志和键盘交互：通过。截图：$taskData"
} finally {
    if ($taskProcess -and !$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    [SetupNative]::SetThreadDpiAwarenessContext($taskPreviousDpi) | Out-Null
}
