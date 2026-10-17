#!/usr/bin/env bash
# The exported game against the editor's render of each scene: see check.py.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UV="$(command -v uv || echo "$HOME/.local/bin/uv")"
exec "$UV" run --quiet --no-project --with mcp --with pyyaml --with pillow --with numpy python "$HERE/check.py" "$@"
