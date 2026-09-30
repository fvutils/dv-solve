# Sphinx configuration for the dv-solve documentation site.
#
# Only the pages under docs/site/ are published. The working notes in docs/
# sit outside this source tree on purpose, so they can never be published by
# accident.
import pathlib
import sys

_REPO = pathlib.Path(__file__).resolve().parents[2]

# Autodoc imports dv_solve from the source tree. The package loads its native
# library lazily, so no native build is needed to document it.
sys.path.insert(0, str(_REPO / "src"))

# The version comes from the package's single source of truth. exec() rather
# than import, for the same reason setup.py does it.
_ver = {}
exec((_REPO / "src" / "dv_solve" / "__version__.py").read_text(), _ver)

project = "dv-solve"
author = "Matthew Ballance and contributors"
copyright = "2019-2026, Matthew Ballance and contributors"
release = _ver["BASE"]
version = release

extensions = [
    "myst_parser",
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "sphinx.ext.intersphinx",
]

source_suffix = {".rst": "restructuredtext", ".md": "markdown"}
exclude_patterns = ["_build", "requirements.txt"]
templates_path = []

myst_enable_extensions = ["colon_fence", "deflist"]
myst_heading_anchors = 3

intersphinx_mapping = {"python": ("https://docs.python.org/3", None)}

autodoc_member_order = "bysource"
napoleon_google_docstring = True
napoleon_numpy_docstring = False

html_theme = "sphinx_rtd_theme"
html_title = f"dv-solve {release}"
html_static_path = ["_static"]
