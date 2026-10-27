"""Parse the `SIMBENCH_RESULT {json}` line printed by simbench_pkg."""
import dataclasses as dc
import json
from typing import Optional

TAG = "SIMBENCH_RESULT"


@dc.dataclass
class Result:
    bench: str
    n: int
    n_req: int
    warmup: int
    failures: int
    check_errors: int
    elapsed_ns: int
    first_call_ns: int
    timed_out: bool

    @property
    def ns_per_call(self) -> Optional[float]:
        return self.elapsed_ns / self.n if self.n else None

    @property
    def calls_per_s(self) -> Optional[float]:
        return self.n * 1e9 / self.elapsed_ns if self.elapsed_ns else None

    @property
    def ok(self) -> bool:
        return (self.failures == 0 and self.check_errors == 0
                and not self.timed_out and self.n == self.n_req)


def parse_result(text: str) -> Optional[Result]:
    """Return the last SIMBENCH_RESULT in the sim output, or None if absent.
    Raises ValueError if the line is present but malformed."""
    line = None
    for ln in text.splitlines():
        i = ln.find(TAG)
        if i >= 0:
            line = ln[i + len(TAG):].strip()
    if line is None:
        return None
    try:
        d = json.loads(line)
        fields = {f.name for f in dc.fields(Result)}
        missing = fields - d.keys()
        if missing:
            raise ValueError("missing keys %s" % sorted(missing))
        d = {k: d[k] for k in fields}
        d["timed_out"] = bool(d["timed_out"])
        return Result(**d)
    except (json.JSONDecodeError, TypeError) as e:
        raise ValueError("bad %s line: %s (%s)" % (TAG, line, e))
