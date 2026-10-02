"""Performance and distribution results: collection, history, rendering.

Design: docs/results_publishing_design.md. A run writes one gzipped JSON
record (collect.py), CI uploads it as an artifact, consolidate.py turns
artifacts into the committed trend history under tests/perf/history/, and
the docs build renders from that history plus the artifacts newer than it.
"""
