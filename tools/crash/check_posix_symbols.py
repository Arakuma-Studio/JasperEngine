#!/usr/bin/env python3
"""AC6: reject allocation, stdio and mutex imports from the POSIX crash backend object."""
import re
import subprocess
import sys

result = subprocess.run(["nm", "-u", "-C", sys.argv[1]], capture_output=True, text=True, check=True)
forbidden = re.compile(r"(?:\b\w*printf\b|\b(?:malloc|calloc|realloc|free|pthread_mutex_lock)\b|operator new|operator delete)")
bad = [line.strip() for line in result.stdout.splitlines() if forbidden.search(line)]
if bad:
    sys.exit("unsafe POSIX crash imports:\n" + "\n".join(bad))
print("PASS POSIX crash backend imports: no printf, heap allocation or mutex lock")
