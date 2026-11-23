# The Windows overflow runner's job-completed hook (as job-completed.sh): deletes the job's workspace,
# only a folder under this runner's _work. Never fails a job.
$dir = Split-Path -Parent $PSScriptRoot
$ws = $env:GITHUB_WORKSPACE
if ($ws -and $ws.StartsWith("$dir\_work\", [StringComparison]::OrdinalIgnoreCase) -and (Test-Path $ws)) {
    Get-ChildItem -Force $ws | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Write-Output "job-completed: emptied $ws"
} else {
    Write-Output "job-completed: workspace '$ws' is not under $dir\_work: left alone"
}
$free = [math]::Floor((Get-PSDrive ($dir.Substring(0, 1))).Free / 1GB)
Write-Output "job-completed: $free GB free"
exit 0
