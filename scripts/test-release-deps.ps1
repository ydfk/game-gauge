$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskScript = Join-Path $taskRoot 'scripts/fetch-release-deps.ps1'
$taskTokens = $null
$taskParseErrors = $null
$taskAst = [Management.Automation.Language.Parser]::ParseFile($taskScript, [ref]$taskTokens, [ref]$taskParseErrors)
if ($taskParseErrors.Count) { throw '依赖下载脚本语法错误' }
# 直接执行生产脚本的下载循环，隔离网络和等待时间。
$taskLoop = $taskAst.Find({
    param($node)
    $node -is [Management.Automation.Language.ForEachStatementAst] -and $node.Variable.VariablePath.UserPath -eq 'taskAsset'
}, $true)
if (!$taskLoop) { throw '未找到依赖下载循环' }
$taskDownload = [scriptblock]::Create($taskLoop.Extent.Text)
$taskFolder = Join-Path $taskRoot ('build/runtime-test/release-deps-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskFolder -Force | Out-Null
$taskPayload = 'verified dependency fixture'
$taskHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($taskPayload)))
$taskAssets = @(@{Name='fixture.bin';Url='https://example.invalid/fixture';Hash=$taskHash})
function Invoke-WebRequest {
    param($Uri, $OutFile, $ConnectionTimeoutSeconds, $OperationTimeoutSeconds)
    $script:taskCalls++
    [IO.File]::WriteAllText($OutFile, 'partial response')
    if ($script:taskCase -eq 'unavailable' -or ($script:taskCase -eq 'transient' -and $script:taskCalls -lt 3)) {
        throw '504 Gateway Time-out'
    }
    if ($script:taskCase -ne 'bad-hash') { [IO.File]::WriteAllText($OutFile, $taskPayload) }
}
function Start-Sleep {
    param($Seconds)
    $script:taskDelays.Add([int]$Seconds)
}
foreach ($taskScenario in @('transient','cached','corrupt-cache','unavailable','bad-hash')) {
    $script:taskCase = $taskScenario
    $script:taskCalls = 0
    $script:taskDelays = [Collections.Generic.List[int]]::new()
    $taskDeps = Join-Path $taskFolder $taskScenario
    New-Item -ItemType Directory -Path $taskDeps -Force | Out-Null
    $taskFile = Join-Path $taskDeps 'fixture.bin'
    if ($taskScenario -eq 'cached') { [IO.File]::WriteAllText($taskFile, $taskPayload) }
    if ($taskScenario -eq 'corrupt-cache') { [IO.File]::WriteAllText($taskFile, 'interrupted old download') }
    $taskFailure = $null
    try { & $taskDownload } catch { $taskFailure = $_.Exception.Message }
    $taskShouldFail = $taskScenario -in @('unavailable','bad-hash')
    if ([bool]$taskFailure -ne $taskShouldFail) { throw "下载结果不符合预期：$taskScenario $taskFailure" }
    $taskExpectedCalls = switch ($taskScenario) { 'cached' {0} 'transient' {3} 'corrupt-cache' {1} default {4} }
    if ($taskCalls -ne $taskExpectedCalls) { throw "下载尝试次数错误：$taskScenario $taskCalls" }
    $taskExpectedDelays = switch ($taskExpectedCalls) { 3 {'3,6'} 4 {'3,6,12'} default {''} }
    if (($taskDelays -join ',') -ne $taskExpectedDelays) { throw "重试间隔错误：$taskScenario" }
    if (Test-Path -LiteralPath "$taskFile.download") { throw "下载临时文件未清理：$taskScenario" }
    if ($taskShouldFail) {
        if ((Test-Path -LiteralPath $taskFile) -or $taskFailure -notmatch 'fixture.bin') { throw "下载失败后误保留依赖或缺少错误上下文：$taskScenario" }
    } elseif ((Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash -ne $taskHash) {
        throw "未保存校验通过的依赖：$taskScenario"
    }
    Write-Host "依赖下载测试通过：$taskScenario"
}
