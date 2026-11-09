# Make repository modules importable in pytest on Windows and Python 3.14.
# Pytest invocation/import mode can leave the repository root off sys.path,
# Keep test imports isolated from local runtime setup.
from __future__ import annotations

import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
root_string = str(REPOSITORY_ROOT)
if root_string not in sys.path:
    sys.path.insert(0, root_string)
