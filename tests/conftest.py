"""Root conftest: ensure local packages/zuspec-dataclasses and src/ take priority."""
import sys
from pathlib import Path

_REPO = Path(__file__).parent.parent

# Our solver src must come before any editable install of another zuspec-solver
_LOCAL_SOLVER = str(_REPO / "src")
if _LOCAL_SOLVER not in sys.path:
    sys.path.insert(0, _LOCAL_SOLVER)

# Local zuspec-dataclasses before any external copy
_LOCAL_ZDC = str(_REPO / "packages" / "zuspec-dataclasses" / "src")
if _LOCAL_ZDC not in sys.path:
    sys.path.insert(0, _LOCAL_ZDC)

# Local dv-flow-libhdlsim before any external copy
_LOCAL_HDLSIM = str(_REPO / "packages" / "dv-flow-libhdlsim" / "src")
if _LOCAL_HDLSIM not in sys.path:
    sys.path.insert(0, _LOCAL_HDLSIM)
