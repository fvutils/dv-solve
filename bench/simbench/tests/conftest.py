import os

import pytest


def pytest_addoption(parser):
    parser.addoption("--simbench-out", metavar="PATH", default=None,
                     help="append one JSONL run record per bench run to PATH")


@pytest.fixture(scope="session")
def simbench_out(request):
    """Path to append run records to, or None when not recording."""
    path = request.config.getoption("--simbench-out")
    if path:
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
    return path
