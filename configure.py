#!/usr/bin/env python3
import os
import sys
import argparse

sourcedir = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(sourcedir, "tools/enki"))
from enki import *

parser = argparse.ArgumentParser("configure.py", description="generates build files")
parser.add_argument("-o", "--out", default="build", help="build generation output directory")
parser.add_argument("-d", "--debug", action="store_true", help="compile with debug info")
parser.add_argument("--optimize", action="store_true", help="compile with optimizations")
parser.add_argument("-v", "--verbose", action="store_true", help="verbose print generation info")
args = parser.parse_args();

host_os   = sys.platform
target_os = host_os

build = Ninja("build", sourcedir, sys.argv[1:], target_os)
build.verbose = args.verbose

### default flags
build.flags("c", [ "-Wall" ])
build.flags("cxx", [
    "-std=c++20",

    "-fno-rtti",
    "-fno-exceptions",

    "-Wno-missing-braces",
    "-Wno-char-subscripts",
    "-Wno-c99-designator",
    "-Wno-missing-field-initializers",
    "-Wno-non-c-typedef-for-linkage",
    "-Wno-unused-function",

    "-Wno-initializer-overrides",
])

if args.debug:
    build.flags("c", "-g")
    build.flags("link", "-g")

if args.optimize:
    build.flags("c", "-O2")

### core
core = build.library("core", "$root/core")
core.meta = True
include_path(core, ["$root", "$root/external"], public=True)
if host_os == "win32": define(core, "_CRT_SECURE_NO_WARNINGS", public=True)

cxx(core, "core.cpp")
cxx(core, "file.cpp")
cxx(core, "lexer.cpp")
cxx(core, "memory.cpp")
cxx(core, "string.cpp")
cxx(core, "thread.cpp")

meta(core, "array.h")
meta(core, "map.h")

if target_os == "win32":
    lib(core, [ "user32", "shell32", "gdi32", "shlwapi", "Xinput" ], public=True)
if target_os == "linux":
    lib(core, [ "X11", "Xi" ], public=True)

### fsg 
fsg = build.executable("fsg", "$root")
dep(fsg, [ core ])

if host_os == "win32": define(fsg, "_CRT_SECURE_NO_WARNINGS", public=True)
include_path(fsg, ["$root/external"], public=True)

cxx(fsg, "fsg.cpp")

build.default = fsg
build.generate()
