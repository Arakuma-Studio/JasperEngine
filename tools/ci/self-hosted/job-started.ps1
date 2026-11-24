# The Windows overflow runner's job-started hook (as job-started.sh): fails the job unless it is this
# repository's push, merge queue, dispatch or pull request from one of its own branches.
$ErrorActionPreference = 'Stop'
$repo = 'Arakuma-Studio/JasperEngine'
function Refuse($why) { Write-Output "::error::overflow runner ${env:COMPUTERNAME}: ${why}: not run here"; exit 1 }

if ($env:GITHUB_REPOSITORY -ne $repo) { Refuse "repository '$env:GITHUB_REPOSITORY'" }
switch ($env:GITHUB_EVENT_NAME) {
    { $_ -in 'push', 'merge_group', 'workflow_dispatch' } { break }
    'pull_request' {
        $head = $null
        try { $head = (Get-Content -Raw $env:GITHUB_EVENT_PATH | ConvertFrom-Json).pull_request.head.repo.full_name } catch { }
        if ($head -ne $repo) { Refuse "a pull request from '$head'" }
        break
    }
    default { Refuse "event '$env:GITHUB_EVENT_NAME'" }
}
$free = [math]::Floor((Get-PSDrive ($PSScriptRoot.Substring(0, 1))).Free / 1GB)
Write-Output "overflow runner ${env:COMPUTERNAME}: $env:GITHUB_EVENT_NAME of $env:GITHUB_REPOSITORY, $free GB free"
exit 0
