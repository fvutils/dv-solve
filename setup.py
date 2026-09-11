#****************************************************************************
# Copyright 2019-2025 Matthew Ballance and contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#****************************************************************************
"""Supplies the one field pyproject.toml declares dynamic: the version.

Everything else about this package -- name, dependencies, packages,
package-data, the cmake and extra-data configuration the ivpm_build backend
reads -- stays in pyproject.toml. This file exists only so that the version
can live in src/dv_solve/__version__.py and still reach setuptools.

The mechanism is the same one pssparser uses (see its setup.py): exec() the
version file in an empty namespace and read _pkg_version back out. exec()ing
it treats it as a standalone source file, so nothing has to be importable and
sys.path is never consulted.

That distinction is the reason this file exists rather than a
`[tool.setuptools.dynamic] version = {attr = "dv_solve.__version__..."}`
entry, which is the more obvious spelling and does not work here: setuptools
only reads an attr: statically when it is a literal, and _pkg_version is
computed from BASE + SUFFIX, so setuptools falls back to importing dv_solve.
Under the ivpm_build backend the package is not importable during
get_requires_for_build_wheel, and the build dies with

    ModuleNotFoundError: No module named 'dv_solve'

inside the manylinux container while succeeding under a local `uv build`.

build-backend is ivpm_build.backend, which wraps setuptools.build_meta; that
wrapper runs cmake and stages the native libraries, then delegates to
setuptools, which picks up this setup.py.
"""

import os

from setuptools import setup

proj_dir = os.path.dirname(os.path.abspath(__file__))


def _get_version():
    version_file = os.path.join(proj_dir, "src", "dv_solve", "__version__.py")
    glb = {}
    with open(version_file) as f:
        exec(f.read(), glb)
    return glb["_pkg_version"]


setup(version=_get_version())
