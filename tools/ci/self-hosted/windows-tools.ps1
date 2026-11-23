# What the windows lanes need on an overflow runner (docs/testing.md "Overflow runners"), as an
# administrator: Git for Windows (bash, ci.yml's shell), Python, CMake, VS 2026 Build Tools (the
# MSVC that windows-latest has: VCToolsVersion 14.51), and Ninja and ccache in C:\je-runner\tools\bin
# (the runner's .path puts them first). Idempotent: an installed tool is skipped.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows-tools.ps1
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = 'C:\je-runner'
$dl = "$root\downloads"
$bin = "$root\tools\bin"
New-Item -ItemType Directory -Force $dl, $bin | Out-Null

function Fetch($url) {
    $out = Join-Path $dl ([IO.Path]::GetFileName(([Uri]$url).LocalPath))
    if (-not (Test-Path $out)) {
        for ($i = 1; $i -le 3; $i++) {
            try { Invoke-WebRequest -UseBasicParsing $url -OutFile "$out.part"; Move-Item -Force "$out.part" $out; break }
            catch { if ($i -eq 3) { throw }; Start-Sleep (10 * $i) }
        }
    }
    return $out
}
function Run($exe, $argList, [int[]]$ok = @(0)) {
    Write-Host "run: $exe $argList"
    $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru -NoNewWindow
    if ($ok -notcontains $p.ExitCode) { throw "$exe exited $($p.ExitCode)" }
}

if (-not (Test-Path 'C:\Program Files\Git\bin\bash.exe')) {
    Run (Fetch 'https://github.com/git-for-windows/git/releases/download/v2.56.1.windows.1/Git-2.56.1-64-bit.exe') `
        '/VERYSILENT /NORESTART /NOCANCEL /SP- /SUPPRESSMSGBOXES /o:PathOption=Cmd'
}
if (-not (Test-Path 'C:\Program Files\Python312\python.exe')) {
    Run (Fetch 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe') `
        '/quiet InstallAllUsers=1 PrependPath=1 Include_test=0 Include_launcher=1'
}
if (-not (Test-Path 'C:\Program Files\CMake\bin\cmake.exe')) {
    Run 'msiexec.exe' "/i `"$(Fetch 'https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-windows-x86_64.msi')`" /qn /norestart ADD_CMAKE_TO_PATH=System"
}
if (-not (Test-Path "$bin\ninja.exe")) {
    Expand-Archive -Force (Fetch 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip') $bin
}
if (-not (Test-Path "$bin\ccache.exe")) {
    $zip = Fetch 'https://github.com/ccache/ccache/releases/download/v4.14.3/ccache-4.14.3-windows-x86_64.zip'
    $tmp = "$dl\ccache"
    Expand-Archive -Force $zip $tmp
    Copy-Item (Get-ChildItem -Recurse $tmp -Filter ccache.exe | Select-Object -First 1).FullName $bin
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vc = if (Test-Path $vswhere) { & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { '' }
if (-not $vc) {
    # 3010: installed, a reboot pending (not done here)
    Run (Fetch 'https://aka.ms/vs/18/stable/vs_buildtools.exe') `
        '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' @(0, 3010)
}
& $vswhere -products * -property installationVersion
& "$bin\ninja.exe" --version
& "$bin\ccache.exe" --version | Select-Object -First 1
& 'C:\Program Files\CMake\bin\cmake.exe' --version | Select-Object -First 1
& 'C:\Program Files\Python312\python.exe' --version
& 'C:\Program Files\Git\bin\bash.exe' --version | Select-Object -First 1
