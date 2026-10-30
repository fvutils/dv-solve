"""The results pages render from the committed history alone."""
from tests.perf import render, svg


def test_pages_render(tmp_path):
    render.render(None, tmp_path)
    for p in ("index", "sat", "randomization", "trends", "releases"):
        assert (tmp_path / f"{p}.md").exists()
    idx = (tmp_path / "index.md").read_text()
    assert "trends" in idx and "releases" in idx
    rel = render.release_records()
    if rel:
        assert rel[-1][0] in (tmp_path / "releases.md").read_text()
        assert rel[-1][0] in idx


def test_trend_svg():
    s = svg.trend({"a": [(0, 1.0), (1, 2.0)], "b": [(0, 3.0), (2, None)]},
                  [(0, "10-01")], [(1, "v1.0")], "y", log_y=True)
    assert s.startswith("<svg") and "v1.0" in s and "10-01" in s
    assert svg.trend({"a": []}, [], [], "y") == ""
