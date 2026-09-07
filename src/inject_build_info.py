# ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
#
# ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
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

# Bakes the exact git commit and a real build timestamp into the firmware,
# independent of the compiler's __DATE__/__TIME__ macros. Those macros only
# refresh when the specific translation unit that uses them actually gets
# recompiled -- if a build cache (e.g. ccache with time_macros sloppiness)
# serves a stale object for that file, the reported build time silently
# stays frozen even though "clean" dependency-checking (touch, mtime) says
# it should be current. This script runs on every PlatformIO invocation
# regardless of any object-level caching, and passes its output as compiler
# defines, so a differing value here always forces a real recompile+relink.

import datetime
import subprocess

Import("env")

_project_dir = env.subst("$PROJECT_DIR")
print("inject_build_info.py: PROJECT_DIR = %r" % _project_dir)


def _run(cmd):
    try:
        out = subprocess.check_output(
            cmd, cwd=_project_dir, stderr=subprocess.STDOUT
        )
        return out.decode("utf-8", "replace").strip()
    except Exception as e:
        print("inject_build_info.py: %r failed: %r" % (cmd, e))
        return ""


git_rev = _run(["git", "rev-parse", "--short", "HEAD"])
git_status = _run(["git", "status", "--porcelain"])
git_ident = (git_rev or "unknown") + ("-dirty" if git_rev and git_status else "")

# No spaces: a raw space in a -D value has repeatedly been a source of
# command-line quoting breakage across scons/compiler/shell combinations.
build_timestamp = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")

print("inject_build_info.py: git_ident=%s build_timestamp=%s" % (git_ident, build_timestamp))

env.Append(CPPDEFINES=[
    ("ZULU_BUILD_GIT_REV", '\\"%s\\"' % git_ident),
    ("ZULU_BUILD_TIMESTAMP", '\\"%s\\"' % build_timestamp),
])
