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
"""Supplies the version, and a build_ext that builds the native code.

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

The build_ext override is for the other way in: `python setup.py build_ext
--inplace` in a workspace set up by `ivpm update -d dev`. The stock build_ext
returns at once when there are no ext_modules -- and there are none, since the
native library is loaded via ctypes -- so without the override that command
succeeds and builds nothing. It drives the same CmakeBuilder the backend uses,
leaving the libraries in build/lib and dv-solve-smt2 in build/bin, which is
where dv_solve looks in a source checkout.

Under the backend this runs cmake a second time: the backend reports
has_ext_modules() as True so the wheel is platform-tagged, which makes
bdist_wheel run build_ext. That pass reconfigures and finds nothing to
rebuild. ivpm_build is imported inside run() because it is only guaranteed to
be installed where a build happens, not wherever setup.py is evaluated.
"""

import os

from setuptools import setup
from setuptools.command.build_ext import build_ext as _build_ext

proj_dir = os.path.dirname(os.path.abspath(__file__))


def _get_version():
    version_file = os.path.join(proj_dir, "src", "dv_solve", "__version__.py")
    glb = {}
    with open(version_file) as f:
        exec(f.read(), glb)
    return glb["_pkg_version"]


class build_ext(_build_ext):

    def run(self):
        from ivpm_build.cmake.cmake_builder import CmakeBuilder
        CmakeBuilder(proj_dir, debug=bool(self.debug)).run()
        super().run()


setup(version=_get_version(), cmdclass={"build_ext": build_ext})
