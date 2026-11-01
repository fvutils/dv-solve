"""The solve statistics' closed name lists (docs/solve_stats_capture_plan.md).

The stats file may hold only names from fixed lists. The routing reasons are
strings set across the SMT-LIB front end, the shape rule and the problem
handle; dvs_stats_reason_of maps them onto the list in dvs_stats.c, and an
unlisted one is counted as "other". These checks keep the two in step: every
reason string in the sources is listed, and each name table has one entry per
enum value.
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src" / "c"


def _table(text: str, name: str) -> list:
    m = re.search(r"static const char \*const " + name + r"\[[^\]]*\] = \{(.*?)\};",
                  text, re.S)
    assert m, name
    return re.findall(r'"([^"]*)"|NULL', m.group(1))


def _enum(text: str, typedef: str) -> list:
    m = re.search(r"typedef enum \{([^{}]*)\} " + typedef + ";", text)
    assert m, typedef
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return [n for n in re.findall(r"\b(DVS_STATS_\w+)", body) if "_N_" not in n]


def test_tables_match_enums():
    h = (SRC / "dvs_stats.h").read_text()
    c = (SRC / "dvs_stats.c").read_text()
    for table, typedef in (("k_path", "dvs_stats_path_t"),
                           ("k_engine", "dvs_stats_engine_t"),
                           ("k_result", "dvs_stats_result_t"),
                           ("k_via", "dvs_stats_via_t"),
                           ("k_reason", "dvs_stats_reason_t")):
        names = _table(c, table)
        assert len(names) == len(_enum(h, typedef)), table
        assert len(set(names)) == len(names), table + " has a duplicate name"


def test_every_reason_string_is_listed():
    listed = set(_table((SRC / "dvs_stats.c").read_text(), "k_reason"))
    found = {}
    fe = (SRC / "smt2" / "smt2_frontend.c").read_text()
    for m in re.finditer(r'(?:nb_why|qfbv_why)\s*=\s*"([^"]+)"', fe):
        found[m.group(1)] = "smt2_frontend.c"
    shape = (SRC / "dvs_shape.c").read_text()
    route = shape[shape.index("const char *dvs_shape_route("):]
    route = route[:route.index("\n}\n")]
    for m in re.finditer(r'return\s+"([^"]+)"', route):
        found[m.group(1)] = "dvs_shape.c"
    for m in re.finditer(r'route_why\s*=\s*"([^"]+)"', (SRC / "dvs_handle.c").read_text()):
        found[m.group(1)] = "dvs_handle.c"
    assert len(found) >= 10, found       # the scan still finds the sites
    missing = {s: f for s, f in found.items() if s not in listed}
    assert not missing, f"reason strings not in k_reason: {missing}"
