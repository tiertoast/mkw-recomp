"""Run the three forbidden guest accesses in concurrent children. Run through CTest."""

import concurrent.futures
import signal
import subprocess
import sys


def fault(access):
    result = subprocess.run([sys.argv[1], access], capture_output=True, timeout=20)
    if f"armed {access}".encode() not in result.stderr.splitlines() or result.returncode not in (
        -signal.SIGBUS,
        -signal.SIGSEGV,
    ):
        raise RuntimeError(f"{access}: status {result.returncode}, {result.stderr.decode()}")
    print(f"{access}: {signal.Signals(-result.returncode).name}")


with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    list(pool.map(fault, ["write-ro", "read-none", "read-hole"]))
