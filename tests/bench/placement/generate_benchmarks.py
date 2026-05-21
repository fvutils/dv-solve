"""Synthetic macro placement benchmark generator.

Generates JSON benchmark files parameterized by macro count, canvas ratio,
size distribution, net topology, halos, spacing rules, etc.

Usage:
    python generate_benchmarks.py --output-dir benchdata/
    python generate_benchmarks.py --suite tier_a --output-dir benchdata/
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def generate_macro_placement_benchmark(
    n_macros: int = 50,
    canvas_ratio: float = 2.0,
    size_distribution: str = "uniform",
    aspect_ratio_range: Tuple[float, float] = (1.0, 2.0),
    n_nets: int = 100,
    net_degree_range: Tuple[int, int] = (2, 5),
    halo_fraction: float = 0.1,
    n_identical_groups: int = 0,
    n_spacing_rules: int = 5,
    n_forbidden_regions: int = 0,
    n_preplaced: int = 0,
    min_macro_size: int = 10,
    max_macro_size: int = 100,
    seed: int = 42,
) -> Dict[str, Any]:
    """Generate a synthetic macro placement problem.

    Returns a JSON-serializable dict describing the placement problem.
    """
    rng = random.Random(seed)

    # -- Generate macros --
    macros = []
    total_area = 0

    for i in range(n_macros):
        if size_distribution == "uniform":
            base = rng.randint(min_macro_size, max_macro_size)
        elif size_distribution == "bimodal":
            if rng.random() < 0.3:
                base = rng.randint(max_macro_size // 2, max_macro_size)
            else:
                base = rng.randint(min_macro_size, max_macro_size // 3)
        elif size_distribution == "power_law":
            # Inverse CDF of power-law: smaller macros much more common
            u = rng.random()
            base = int(min_macro_size + (max_macro_size - min_macro_size) * (1 - u**2))
        else:
            base = rng.randint(min_macro_size, max_macro_size)

        ar_lo, ar_hi = aspect_ratio_range
        aspect = ar_lo + rng.random() * (ar_hi - ar_lo)
        width = max(1, int(base * math.sqrt(aspect)))
        height = max(1, int(base / math.sqrt(aspect)))

        group_id = -1
        macros.append({
            "id": i,
            "width": width,
            "height": height,
            "group_id": group_id,
            "preplaced": False,
            "x": None,
            "y": None,
        })
        total_area += width * height

    # -- Identical groups --
    if n_identical_groups > 0 and n_macros >= n_identical_groups * 2:
        group_size = max(2, n_macros // (n_identical_groups * 3))
        idx = 0
        for g in range(n_identical_groups):
            template = macros[idx]
            for k in range(group_size):
                if idx + k < n_macros:
                    macros[idx + k]["width"] = template["width"]
                    macros[idx + k]["height"] = template["height"]
                    macros[idx + k]["group_id"] = g
            idx += group_size

    # -- Canvas sizing --
    canvas_area = total_area * canvas_ratio
    canvas_side = int(math.sqrt(canvas_area))
    canvas_w = canvas_side
    canvas_h = canvas_side
    # Ensure canvas is at least as wide/tall as the largest macro
    max_w = max(m["width"] for m in macros)
    max_h = max(m["height"] for m in macros)
    canvas_w = max(canvas_w, max_w + 10)
    canvas_h = max(canvas_h, max_h + 10)

    canvas = {"width": canvas_w, "height": canvas_h}

    # -- Preplaced macros --
    n_pre = min(n_preplaced, n_macros)
    if n_pre > 0:
        pre_indices = rng.sample(range(n_macros), n_pre)
        for pi in pre_indices:
            m = macros[pi]
            m["preplaced"] = True
            m["x"] = rng.randint(0, max(0, canvas_w - m["width"]))
            m["y"] = rng.randint(0, max(0, canvas_h - m["height"]))

    # -- Nets --
    nets = []
    for ni in range(n_nets):
        deg_lo, deg_hi = net_degree_range
        degree = rng.randint(deg_lo, min(deg_hi, n_macros))
        members = rng.sample(range(n_macros), degree)

        pin_offsets = []
        for mid in members:
            m = macros[mid]
            px = rng.randint(0, m["width"])
            py = rng.randint(0, m["height"])
            pin_offsets.append({"macro_id": mid, "pin_x": px, "pin_y": py})

        nets.append({
            "id": ni,
            "pins": pin_offsets,
        })

    # -- Halos --
    halos = []
    for i in range(n_macros):
        m = macros[i]
        h = int(halo_fraction * min(m["width"], m["height"]))
        halos.append({
            "macro_id": i,
            "left": h, "right": h, "top": h, "bottom": h,
        })

    # -- Spacing rules --
    spacing_rules = []
    groups = list(set(m["group_id"] for m in macros if m["group_id"] >= 0))
    for _ in range(n_spacing_rules):
        if groups and rng.random() < 0.5:
            fg = rng.choice(groups)
            tg = rng.choice(groups)
        else:
            fg = rng.randint(-1, max(0, n_identical_groups - 1))
            tg = rng.randint(-1, max(0, n_identical_groups - 1))

        spacing_rules.append({
            "from_group": fg,
            "to_group": tg,
            "min_distance": rng.randint(5, 50),
            "direction": rng.choice(["x", "y", "both"]),
        })

    # -- Forbidden regions --
    forbidden_regions = []
    for _ in range(n_forbidden_regions):
        fw = rng.randint(canvas_w // 10, canvas_w // 4)
        fh = rng.randint(canvas_h // 10, canvas_h // 4)
        fx = rng.randint(0, max(0, canvas_w - fw))
        fy = rng.randint(0, max(0, canvas_h - fh))
        forbidden_regions.append({"x": fx, "y": fy, "width": fw, "height": fh})

    return {
        "macros": macros,
        "nets": nets,
        "canvas": canvas,
        "halos": halos,
        "spacing_rules": spacing_rules,
        "forbidden_regions": forbidden_regions,
        "parameters": {
            "n_macros": n_macros,
            "canvas_ratio": canvas_ratio,
            "size_distribution": size_distribution,
            "seed": seed,
        },
    }


# ------------------------------------------------------------------ #
# Benchmark suite definitions                                         #
# ------------------------------------------------------------------ #

TIER_A = {
    "A1": dict(n_macros=20,  canvas_ratio=3.0, halo_fraction=0.0,
               n_spacing_rules=0, n_nets=30,  net_degree_range=(2,4), seed=1001),
    "A2": dict(n_macros=50,  canvas_ratio=2.0, halo_fraction=0.05,
               n_spacing_rules=5, n_nets=80,  net_degree_range=(2,5), seed=1002),
    "A3": dict(n_macros=100, canvas_ratio=1.8, halo_fraction=0.10,
               n_spacing_rules=10, n_nets=200, net_degree_range=(2,5),
               n_forbidden_regions=3, seed=1003),
    "A4": dict(n_macros=200, canvas_ratio=1.5, halo_fraction=0.15,
               n_spacing_rules=15, n_nets=300, net_degree_range=(2,5),
               n_forbidden_regions=5, n_preplaced=40, seed=1004),
    "A5": dict(n_macros=100, canvas_ratio=1.3, halo_fraction=0.20,
               n_spacing_rules=20, n_nets=150, net_degree_range=(2,5), seed=1005),
}

TIER_B = {
    "B1": dict(n_macros=20,  canvas_ratio=3.0, halo_fraction=0.0,
               n_spacing_rules=0, n_nets=80,   net_degree_range=(3,6), seed=2001),
    "B2": dict(n_macros=50,  canvas_ratio=2.0, halo_fraction=0.05,
               n_spacing_rules=5, n_nets=200,  net_degree_range=(3,6), seed=2002),
    "B3": dict(n_macros=100, canvas_ratio=1.8, halo_fraction=0.10,
               n_spacing_rules=10, n_nets=400, net_degree_range=(3,6),
               n_forbidden_regions=3, seed=2003),
    "B4": dict(n_macros=200, canvas_ratio=1.5, halo_fraction=0.15,
               n_spacing_rules=15, n_nets=600, net_degree_range=(3,6),
               n_forbidden_regions=5, n_preplaced=40, seed=2004),
}

TIER_C = {
    "C1": dict(n_macros=100, canvas_ratio=1.8, halo_fraction=0.10,
               n_spacing_rules=10, n_nets=200, net_degree_range=(2,5),
               n_identical_groups=0, seed=3001),
    "C2": dict(n_macros=100, canvas_ratio=1.8, halo_fraction=0.10,
               n_spacing_rules=10, n_nets=200, net_degree_range=(2,5),
               n_identical_groups=3, seed=3002),
    "C3": dict(n_macros=200, canvas_ratio=1.8, halo_fraction=0.10,
               n_spacing_rules=10, n_nets=300, net_degree_range=(2,5),
               n_identical_groups=5, seed=3003),
    "C4": dict(n_macros=200, canvas_ratio=1.4, halo_fraction=0.15,
               n_spacing_rules=20, n_nets=400, net_degree_range=(2,5),
               n_identical_groups=5, n_forbidden_regions=3,
               n_preplaced=20, seed=3004),
}

ALL_SUITES = {"tier_a": TIER_A, "tier_b": TIER_B, "tier_c": TIER_C}


def generate_suite(suite_name: str, output_dir: Path) -> List[Path]:
    """Generate all benchmarks for a suite, writing JSON files to output_dir."""
    output_dir.mkdir(parents=True, exist_ok=True)
    suite = ALL_SUITES.get(suite_name)
    if suite is None:
        raise ValueError(f"Unknown suite: {suite_name}. Available: {list(ALL_SUITES)}")

    paths = []
    for name, params in sorted(suite.items()):
        bench = generate_macro_placement_benchmark(**params)
        bench["name"] = name
        path = output_dir / f"{name}.json"
        with open(path, "w") as f:
            json.dump(bench, f, indent=2)
        paths.append(path)
    return paths


def generate_all_suites(output_dir: Path) -> List[Path]:
    """Generate all benchmark suites."""
    all_paths = []
    for suite_name in ALL_SUITES:
        all_paths.extend(generate_suite(suite_name, output_dir / suite_name))
    return all_paths


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate placement benchmarks")
    parser.add_argument("--output-dir", type=Path, default=Path("benchdata"))
    parser.add_argument("--suite", choices=list(ALL_SUITES) + ["all"], default="all")
    args = parser.parse_args()

    if args.suite == "all":
        paths = generate_all_suites(args.output_dir)
    else:
        paths = generate_suite(args.suite, args.output_dir)

    print(f"Generated {len(paths)} benchmark files:")
    for p in paths:
        print(f"  {p}")
