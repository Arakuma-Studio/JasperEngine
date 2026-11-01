#!/usr/bin/env python3
"""tools/gpu/summary.py <result.json...> — every case's render size and its timed GPU passes (median
ms per frame) from the real-GPU benches' JSON (atmosphere_bench.py --gpu, gpu_campaign.py)."""
import json
import sys

for path in sys.argv[1:]:
    data = json.load(open(path))
    print(f"== {path}: {data.get('device', '')}")
    for case, result in data.get("results", {}).items():
        gpu = result.get("gpu", result)
        passes = {label: round(ms, 3) for label, ms in gpu.items()
                  if isinstance(ms, (int, float)) and not label.startswith("_") and ms >= 0.005}
        frame = gpu.get("_frame")
        size = result.get("render_size") or result.get("_size")
        print(f"  {case}: {size} frame {frame} {passes}")
