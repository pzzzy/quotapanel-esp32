# SPDX-License-Identifier: GPL-3.0-only
"""Keep local build-machine paths out of assertion strings and debug metadata."""
Import("env")  # PlatformIO/SCons injects the build environment.

mappings = [
    ("$PROJECT_DIR", "firmware"),
    ("$PROJECT_PACKAGES_DIR", "platformio/packages"),
    ("$PROJECT_LIBDEPS_DIR", "platformio/libdeps"),
]
for variable, label in mappings:
    source = env.subst(variable)
    if not source or source.startswith("$"):
        raise RuntimeError("Missing PlatformIO path: " + variable)
    env.Append(CCFLAGS=["-ffile-prefix-map=" + source + "=" + label])
