#!/usr/bin/env bash
# Run the MCP tests (tools/mcp/tests/test_*.py) through the harness. Arguments go to
# run_tests.py: -k FILTER, -j N, --suite ci, --json FILE, --fake, --url URL, --xvfb editor|harness|none,
# --keep, files... The harness's packages are pinned in tools/mcp/requirements.txt.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UV="$(command -v uv || echo "$HOME/.local/bin/uv")"
exec "$UV" run --quiet --no-project --with-requirements "$HERE/requirements.txt" python "$HERE/run_tests.py" "$@"
