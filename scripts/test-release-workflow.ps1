$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskWorkflow = Get-Content -LiteralPath "$taskRoot/.github/workflows/release.yml" -Raw -Encoding UTF8
$taskMatch = [regex]::Match($taskWorkflow, '(?ms)      - name: Create draft and upload assets.*?        run: \|\r?\n(?<body>(?:          [^\r\n]*(?:\r?\n|$))+)')
if (!$taskMatch.Success) { throw 'Release step was not found' }
$taskBody = [regex]::Replace($taskMatch.Groups['body'].Value, '(?m)^          ', '')
$taskStep = [scriptblock]::Create($taskBody)
$taskFolder = Join-Path $taskRoot ('build/runtime-test/release-workflow-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$taskFolder/build/package" -Force | Out-Null
[IO.File]::WriteAllText("$taskFolder/build/package/GameGauge-1.2.3-Setup.exe", 'isolated release test payload')
$taskHash = (Get-FileHash -LiteralPath "$taskFolder/build/package/GameGauge-1.2.3-Setup.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$taskFolder/build/package/SHA256SUMS.txt", "$taskHash  GameGauge-1.2.3-Setup.exe")
$taskPrevious = @{}; foreach ($taskKey in @('RELEASE_TAG','RELEASE_VERSION','GH_REPO')) { $taskPrevious[$taskKey] = [Environment]::GetEnvironmentVariable($taskKey) }
$taskLocation = Get-Location
function gh {
    $taskArguments = @($args)
    $script:taskCalls.Add(($taskArguments -join ' '))
    $global:LASTEXITCODE = 0
    if ($taskArguments[0] -eq 'release') {
        switch ($taskArguments[1]) {
            'view' {
                if ($taskArguments -contains 'databaseId') {
                    if ($script:taskScenario -eq 'invalid-id') { return '0' }
                    return '42'
                }
                if ($script:taskScenario -eq 'new-draft') { $global:LASTEXITCODE = 1; return }
                return (@{isDraft=($script:taskScenario -ne 'published')} | ConvertTo-Json -Compress)
            }
            'create' { return 'https://github.com/example/repo/releases/tag/untagged-test' }
            'upload' { return }
        }
    }
    if ($taskArguments[0] -eq 'api' -and $taskArguments -contains 'PATCH') { return '{}' }
    if ($taskArguments[0] -eq 'api' -and $taskArguments[1] -eq 'repos/example/repo/releases/42') {
        $taskDigest = if ($script:taskScenario -eq 'bad-digest') { 'sha256:invalid' } else { "sha256:$script:taskHash" }
        $taskTag = if ($script:taskScenario -eq 'wrong-tag') { 'v9.9.9' } else { 'v1.2.3' }
        return (@{id=42;draft=$true;tag_name=$taskTag;assets=@(@{name='GameGauge-1.2.3-Setup.exe';digest=$taskDigest})} | ConvertTo-Json -Depth 5 -Compress)
    }
    throw "Unexpected gh command: $($taskArguments -join ' ')"
}
try {
    Set-Location -LiteralPath $taskFolder
    $env:RELEASE_TAG='v1.2.3'; $env:RELEASE_VERSION='1.2.3'; $env:GH_REPO='example/repo'
    foreach ($taskCase in @('existing-draft','new-draft','published','bad-digest','wrong-tag','invalid-id')) {
        $script:taskScenario=$taskCase; $script:taskCalls=[Collections.Generic.List[string]]::new()
        $taskFailure=$null
        try { & $taskStep | Out-Null } catch { $taskFailure=$_.Exception.Message }
        $taskSuccess = $taskCase -in @('existing-draft','new-draft')
        if ($taskSuccess -eq [bool]$taskFailure) { throw "Unexpected workflow result for ${taskCase}: $taskFailure; commands: $($taskCalls -join '; ')" }
        $taskPublished=@($taskCalls | Where-Object { $_ -match '^api --method PATCH ' }).Count
        if ($taskPublished -ne [int]$taskSuccess) { throw "Unexpected publication count for $taskCase" }
        if (@($taskCalls | Where-Object { $_ -match '/releases/tags/' }).Count) { throw 'Draft verification used the published-only tag endpoint' }
        if ($taskCase -eq 'published' -and @($taskCalls | Where-Object { $_ -match '^release (create|upload) ' }).Count) { throw 'Published assets were changed' }
        if ($taskCase -eq 'existing-draft' -and @($taskCalls | Where-Object { $_ -match '^release create ' }).Count) { throw 'Existing draft was recreated' }
        if ($taskCase -eq 'new-draft' -and @($taskCalls | Where-Object { $_ -match '^release create ' }).Count -ne 1) { throw 'New draft was not created exactly once' }
        Write-Host "Release contract passed: $taskCase"
    }
} finally {
    Set-Location -LiteralPath $taskLocation.Path
    foreach ($taskKey in $taskPrevious.Keys) { [Environment]::SetEnvironmentVariable($taskKey,$taskPrevious[$taskKey]) }
}
