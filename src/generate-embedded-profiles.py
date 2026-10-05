# ZuluSCSI™ - Copyright (c) 2026 Rabbit Hole Computing™
#
# ZuluSCSI™ file is licensed under the GPL version 3 or any later version.
#
# https://www.gnu.org/licenses/gpl-3.0.html
# ----
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Compiles the profile definitions named by `profile_definitions` in
# platformio.ini into src/embedded_profiles_generated.h: a ZPDB store as a
# const array, which zpdb_profiles.cpp serves as the fallback behind the custom
# store built in flash from the SD card.
#
# The header is written here, while SCons is still setting up, rather than as a
# pre-link action: zpdb_profiles.cpp includes it, so it has to be on disk before
# that file is compiled -- a pre-link action runs only after every object is
# already built, which on a clean checkout (the header is .gitignored) means
# compiling against a header that does not exist yet. Same reasoning as in
# inject_build_info.py.

import filecmp
import os
import subprocess

Import("env")

_project_dir = env.subst("$PROJECT_DIR")
_definitions = os.path.join(_project_dir, env.GetProjectOption("profile_definitions"))
_header = os.path.join(_project_dir, "src", "embedded_profiles_generated.h")
_header_tmp = _header + ".tmp"

# Both linker scripts place .flashdata* in flash. Without it the RP2350 script
# would copy the array into RAM along with the rest of the .cpp.o .rodata.
_section = ".flashdata.zpdb_profiles"


def _fail(msg):
    print("generate-embedded-profiles.py: ERROR: " + msg)
    if os.path.exists(_header_tmp):
        os.remove(_header_tmp)
    env.Exit(1)


if not os.path.isfile(_definitions):
    _fail("profile_definitions file %r does not exist" % _definitions)

# zpdb_build.py stamps the store with the current time unless told otherwise,
# which would change the header -- and recompile zpdb_profiles.cpp -- on every
# build. The definitions file's own mtime is what the stamp should date anyway.
_build_env = dict(os.environ)
_build_env["SOURCE_DATE_EPOCH"] = str(int(os.path.getmtime(_definitions)))

_cmd = [
    env.subst("$PYTHONEXE"),
    os.path.join(_project_dir, "utils", "zpdb_build.py"),
    "--header", _header_tmp,
    "--section", _section,
    _definitions,
]

try:
    _result = subprocess.run(_cmd, cwd=_project_dir, env=_build_env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
except OSError as e:
    _fail("could not run %r: %s" % (_cmd, e))

_output = _result.stdout.decode("utf-8", "replace").strip()
if _output:
    print(_output)

if _result.returncode != 0:
    _fail("zpdb_build.py exited with %d building %s" % (_result.returncode, _definitions))

# Only replace the header when its contents changed, so an unchanged
# definitions file does not force a recompile.
if os.path.exists(_header) and filecmp.cmp(_header_tmp, _header, shallow=False):
    os.remove(_header_tmp)
    print("generate-embedded-profiles.py: %s is up to date" % _header)
else:
    os.replace(_header_tmp, _header)
    print("generate-embedded-profiles.py: wrote %s from %s" % (_header, _definitions))
