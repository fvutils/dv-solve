"""Fail an MSVC build whose cadical.lib still exports an un-renamed C name.

kissat and CaDiCaL both define kitten_* and a few other C names. On MSVC,
cmake/cadical_msvc_rename.h prefixes CaDiCaL's with dvs_cdk_ (see there for
why). A name the header misses would not necessarily fail the link -- the
CaDiCaL code calling it could bind to kissat's incompatible definition -- so
this lists the library's symbols with dumpbin and rejects any defined,
external, non-COMDAT symbol that is neither C++ (MSVC-mangled names start
with '?'), nor ccadical_* (the API dv-solve calls), nor dvs_cdk_*.

COMDAT symbols are skipped: those are inline functions and constants from the
CRT and C++ headers (printf, __real@..., ...), which the linker merges by
design.

Usage: python check_cadical_symbols.py <path/to/cadical.lib>
"""
import re
import subprocess
import sys

ALLOWED_PREFIXES = ("?", "ccadical_", "dvs_cdk_")

# 008 00000000 SECT3  notype ()    External     | kitten_solve
SYM = re.compile(r"^[0-9A-F]{3,} [0-9A-F]{8} (SECT[0-9A-F]+|UNDEF|ABS|DEBUG)"
                 r"\s+.*?\b(External|Static|WeakExternal|Label|Section)\s+\|\s+(\S+)")


def main(lib):
    out = subprocess.run(["dumpbin", "/nologo", "/symbols", lib],
                         capture_output=True, text=True, check=True).stdout
    bad = set()
    comdat = set()          # SECTn holding a COMDAT, in the current object
    last_section = None
    for line in out.splitlines():
        if "COFF SYMBOL TABLE" in line:
            comdat, last_section = set(), None
            continue
        if last_section and "selection" in line:
            comdat.add(last_section)
            last_section = None
            continue
        m = SYM.match(line)
        if not m:
            continue
        sect, storage, name = m.groups()
        last_section = sect if storage == "Static" and name.startswith(".") else None
        if (storage == "External" and sect.startswith("SECT")
                and sect not in comdat and not name.startswith(ALLOWED_PREFIXES)):
            bad.add(name)
    if bad:
        print("check_cadical_symbols: %s defines C names that are not renamed;"
              " add them to cmake/cadical_msvc_rename.h:" % lib)
        for name in sorted(bad):
            print("    " + name)
        return 1
    print("check_cadical_symbols: OK, only ccadical_* and dvs_cdk_* C names")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
