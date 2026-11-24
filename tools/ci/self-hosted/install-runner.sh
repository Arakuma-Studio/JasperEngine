#!/usr/bin/env bash
# Installs this machine's overflow runner (docs/testing.md "Overflow runners") in ~/je-runner: one
# runner, labelled self-hosted, its OS and arch, and <label>, running as the user who runs this, as a
# service that starts with the machine (systemd, via sudo) or the user's login (a launchd agent) and
# reconnects by itself after sleep. Its jobs get their own HOME and git config inside ~/je-runner, and
# the hooks beside this script. Idempotent: a configured runner is kept, its settings rewritten.
#
#   JE_RUNNER_TOKEN=<token> tools/ci/self-hosted/install-runner.sh <je-mac|je-linux> [KEY=VALUE ...]
#
# The token: gh api -X POST repos/Arakuma-Studio/JasperEngine/actions/runners/registration-token --jq .token
# (an hour's use, registration only). KEY=VALUE pairs go into the jobs' environment (JOBS=10 CPUS=10).
set -euo pipefail
LABEL="${1:?usage: install-runner.sh <je-mac|je-linux> [KEY=VALUE ...]}"
shift
VERSION=2.338.2
URL=https://github.com/Arakuma-Studio/JasperEngine
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="$HOME/je-runner"
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64) pkg=osx-arm64 ;;
    Linux/x86_64) pkg=linux-x64 ;;
    *) echo "install-runner: no runner package for $(uname -s)/$(uname -m)" >&2; exit 2 ;;
esac
mkdir -p "$DIR/hooks" "$DIR/home"
cd "$DIR"
if [[ ! -x run.sh ]]; then
    curl -fsSL --retry 3 "https://github.com/actions/runner/releases/download/v$VERSION/actions-runner-$pkg-$VERSION.tar.gz" | tar xz
fi
cp "$HERE/job-started.sh" "$HERE/job-completed.sh" hooks/
chmod +x hooks/*.sh
# ci.yml's Windows legs set autocrlf with --global: here that is this file, never the user's
printf '[core]\n\tautocrlf = false\n' > gitconfig
if [[ ! -f .runner ]]; then
    ./config.sh --unattended --url "$URL" --token "${JE_RUNNER_TOKEN:?JE_RUNNER_TOKEN=<registration token>}" \
        --name "$LABEL" --labels "$LABEL" --work _work --replace
fi
# (config.sh wrote both from this shell; the jobs get these instead)
if [[ "$(uname -s)" == Darwin ]]; then
    echo "/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" > .path
else
    echo "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" > .path
fi
{
    echo "LANG=en_US.UTF-8"
    echo "HOME=$DIR/home"
    echo "GIT_CONFIG_GLOBAL=$DIR/gitconfig"
    echo "ACTIONS_RUNNER_HOOK_JOB_STARTED=$DIR/hooks/job-started.sh"
    echo "ACTIONS_RUNNER_HOOK_JOB_COMPLETED=$DIR/hooks/job-completed.sh"
    for pair in "$@"; do echo "$pair"; done
} > .env
# (svc.sh install writes .service; a restart rereads .env and .path)
svc=(./svc.sh)
[[ "$(uname -s)" == Linux ]] && svc=(sudo ./svc.sh)
if [[ -f .service ]]; then
    "${svc[@]}" stop
elif [[ "$(uname -s)" == Linux ]]; then
    "${svc[@]}" install "$USER"
else
    "${svc[@]}" install
fi
"${svc[@]}" start
"${svc[@]}" status | head -8
