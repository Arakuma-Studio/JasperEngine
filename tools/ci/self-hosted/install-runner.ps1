# Installs the Windows overflow runner (docs/testing.md "Overflow runners") in C:\je-runner, as
# install-runner.sh: labelled self-hosted, Windows, X64 and je-win, run as the user who runs this (no
# stored password: a scheduled task at startup with an S4U logon, restarted when it stops), its jobs with
# their own HOME and git config in C:\je-runner and the hooks beside this script. After
# windows-tools.ps1, as an administrator. Idempotent.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File install-runner.ps1 -Token <token> [-JobEnv 'JOBS=3']
param([Parameter(Mandatory = $true)][string]$Token, [string[]]$JobEnv = @())
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$version = '2.338.2'
$dir = 'C:\je-runner'
$label = 'je-win'
$task = 'je-runner'
New-Item -ItemType Directory -Force "$dir\hooks", "$dir\home\AppData\Local", "$dir\home\AppData\Roaming", "$dir\ccache" | Out-Null
Set-Location $dir
if (-not (Test-Path "$dir\run.cmd")) {
    $zip = "$dir\downloads\actions-runner-win-x64-$version.zip"
    New-Item -ItemType Directory -Force "$dir\downloads" | Out-Null
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -UseBasicParsing "https://github.com/actions/runner/releases/download/v$version/actions-runner-win-x64-$version.zip" -OutFile $zip
    }
    Expand-Archive -Force $zip $dir
}
Copy-Item -Force "$PSScriptRoot\job-started.ps1", "$PSScriptRoot\job-completed.ps1" "$dir\hooks\"
Set-Content -Encoding ascii "$dir\gitconfig" "[core]`n`tautocrlf = false"
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $task
    Get-Process Runner.Listener, Runner.Worker -ErrorAction SilentlyContinue | Stop-Process -Force
}
if (-not (Test-Path "$dir\.runner")) {
    & "$dir\config.cmd" --unattended --url https://github.com/Arakuma-Studio/JasperEngine --token $Token `
        --name $label --labels $label --work _work --replace
    if ($LASTEXITCODE -ne 0) { throw "config.cmd exited $LASTEXITCODE" }
}
# the tools of windows-tools.ps1 first; Git's bin holds the bash ci.yml runs its steps with
$machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
Set-Content -Encoding ascii "$dir\.path" "$dir\tools\bin;C:\Program Files\Git\bin;$machine"
# (PSExecutionPolicyPreference: the runner's own processes may run its hooks; the machine's policy stays)
Set-Content -Encoding ascii "$dir\.env" (@(
    "PSExecutionPolicyPreference=Bypass",
    "HOME=$dir\home",
    # an S4U task has no LOCALAPPDATA or APPDATA, and ccache (on PATH, the compiler launcher of every leg)
    # needs a directory: the legs without a ccache-restore step share this capped one
    "LOCALAPPDATA=$dir\home\AppData\Local",
    "APPDATA=$dir\home\AppData\Roaming",
    "CCACHE_DIR=$dir\ccache",
    "CCACHE_MAXSIZE=5G",
    "GIT_CONFIG_GLOBAL=$dir\gitconfig",
    "ACTIONS_RUNNER_HOOK_JOB_STARTED=$dir\hooks\job-started.ps1",
    "ACTIONS_RUNNER_HOOK_JOB_COMPLETED=$dir\hooks\job-completed.ps1"
) + $JobEnv)
# (run.cmd does not read .path, as the Unix service wrapper does: start.cmd sets PATH from it)
Set-Content -Encoding ascii "$dir\start.cmd" "@echo off`r`nset /p PATH=<`"$dir\.path`"`r`ncd /d `"$dir`"`r`ncall run.cmd"
$action = New-ScheduledTaskAction -Execute "$dir\start.cmd" -WorkingDirectory $dir
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
Register-ScheduledTask -Force -TaskName $task -Action $action -Trigger $trigger -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep 15
Get-ScheduledTask -TaskName $task | Select-Object TaskName, State | Format-Table | Out-String
Get-Process Runner.Listener -ErrorAction SilentlyContinue | Select-Object Id, StartTime | Format-Table | Out-String
