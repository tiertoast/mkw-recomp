"""Configure the native Mach-O foundations target with the active Apple SDK and build it with Ninja.

Invoked by build.py --target macos-x86_64. The fixture has no dependency on setup, guest files or a GPU.
"""

import hashlib
import subprocess


def xcrun(*args):
    return subprocess.check_output(["xcrun", "--sdk", "macosx", *args], text=True).strip()


def build_foundations(root, args, common_id, exports):
    translated = subprocess.run(["sysctl", "-in", "sysctl.proc_translated"], capture_output=True, text=True, check=True)
    if translated.stdout.strip() == "1":
        raise SystemExit("macos-x86_64 requires native execution (Rosetta detected)")
    out = args.out or root / "build" / "macos-x86_64-g2a"
    # Separate module caches by target and the host library's two-level namespace ABI.
    abi = root / "runtime/core/boot/darwin_abi.h"
    fingerprint = hashlib.sha256(b"macos-x86_64/macho/libmkw-host.1\0" + common_id.encode())
    fingerprint.update(abi.read_bytes().replace(b"\r\n", b"\n"))
    fingerprint.update(";".join(exports).encode())
    module_id = fingerprint.hexdigest()[:16]
    subprocess.run(
        [
            "cmake",
            "-S",
            root / "tests/macos",
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
        ],
        check=True,
    )
    cmd = ["cmake", "--build", out]
    if args.jobs:
        cmd += ["--parallel", str(args.jobs)]
    if args.dry_run:
        cmd += ["--", "-n"]
    subprocess.run(cmd, check=True)
