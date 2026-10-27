"""dv-solve-simbench: SystemVerilog randomization benchmarks for any simulator."""
import os

def share_dir() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "share")
