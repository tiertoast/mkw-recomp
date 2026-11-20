"""Build the Mach-O foundations or runtime with the active Apple SDK and Ninja.

Run via build.py --target macos-x86_64. The runtime-only build uses native dependencies and no generated game code.
"""

import hashlib
import os
import subprocess
import sys


def xcrun(*args):
    return subprocess.check_output(["xcrun", "--sdk", "macosx", *args], text=True).strip()


def game_module_id(root, common_id, exports):
    # Separate module caches by target and the host library's two-level namespace ABI.
    abi = root / "runtime/core/boot/darwin_abi.h"
    fingerprint = hashlib.sha256(b"macos-x86_64/macho/libmkw-host.1\0" + common_id.encode())
    fingerprint.update(abi.read_bytes().replace(b"\r\n", b"\n"))
    fingerprint.update(";".join(exports).encode())
    return fingerprint.hexdigest()[:16]


def build(root, args, common_id, exports, source, out, extra=()):
    translated = subprocess.run(["sysctl", "-in", "sysctl.proc_translated"], capture_output=True, text=True, check=True)
    if translated.stdout.strip() == "1":
        raise SystemExit("macos-x86_64 requires native execution (Rosetta detected)")
    module_id = game_module_id(root, common_id, exports)
    subprocess.run(
        [
            "cmake",
            "-S",
            source,
            "-B",
            out,
            "-G",
            "Ninja",
            "-DCMAKE_OSX_ARCHITECTURES=x86_64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0",
            "-DCMAKE_OSX_SYSROOT=" + xcrun("--show-sdk-path"),
            "-DCMAKE_C_COMPILER=" + xcrun("--find", "clang"),
            "-DCMAKE_CXX_COMPILER=" + xcrun("--find", "clang++"),
            "-DMKW_GAME_ID=" + module_id,
            "-DMKW_HOST_EXPORTS=" + ";".join(exports),
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DMKW_FRAME_POINTERS=" + ("ON" if args.frame_pointers else "OFF"),
            *extra,
        ],
        check=True,
    )
    cmd = ["cmake", "--build", out]
    if args.runtime_only:
        cmd += ["--target", "mkw"]
    if args.jobs:
        cmd += ["--parallel", str(args.jobs)]
    options = (["-n"] if args.dry_run else []) + (["-k", "0"] if args.runtime_only else [])
    if options:
        cmd += ["--", *options]
    result = subprocess.run(cmd, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)


def build_foundations(root, args, common_id, exports):
    out = args.out or root / "build" / "macos-x86_64-g2a"
    build(root, args, common_id, exports, root / "tests/macos", out)


def build_runtime(root, args, common_id, exports, sources, defaults, generate_catalog):
    out = args.out or root / "build/dist/macos-x86_64"
    generated = out / "rt"
    generated.mkdir(parents=True, exist_ok=True)
    path = generated / "defaults.c"
    if not path.exists() or path.read_text() != defaults:
        path.write_text(defaults)
    generate_catalog(generated / "i18n")
    extra = [
        "-DMKW_RUNTIME_SOURCES=" + ";".join(str(p) for p in sources),
        "-DMKW_PYTHON=" + sys.executable,
        "-DMKW_PYTHON_PREFIX=" + sys.prefix,
        *[f"-D{name}={os.environ[name]}" for name in ("CMAKE_PREFIX_PATH", "VULKAN_PREFIX") if name in os.environ],
    ]
    build(root, args, common_id, exports, root / "tools/macos-runtime", out, extra)
