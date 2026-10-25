#!/usr/bin/env bash
# Run the MCP tests (tools/mcp/tests/test_*.py) through the harness. Arguments go to
# run_tests.py: -k FILTER, -j N, --fake, --url URL, --xvfb editor|harness|none, --keep, files...
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UV="$(command -v uv || echo "$HOME/.local/bin/uv")"
exec "$UV" run --quiet --no-project --with mcp --with pyyaml --with jsonschema python "$HERE/run_tests.py" "$@"
