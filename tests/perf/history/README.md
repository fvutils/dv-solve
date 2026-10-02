# Committed performance history

Written only by `python3 -m tests.perf.consolidate`; never edited by hand.
Design: `docs/results_publishing_design.md` §4.

| Path | Content |
|---|---|
| `<year>.jsonl` | One trend line per run and build, sorted by `utc`. Plain text on purpose: git delta-compresses appended text, and a gzipped file would cost its full size on every change. |
| `releases/<tag>.json.gz` | The full run record of each release's perf run (the earliest valid one). Written once. |
| `machines/<id>.json` | Each machine class runs were measured on: CPU model, cores, memory, kernel. Never a host name. |

The full record of every other run is a Forgejo artifact
(`perf-<kind>-<utc>-<sha>`) that expires 90 days after upload. Consolidate
before then: at each release, and at least monthly.
