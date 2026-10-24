#!/usr/bin/env python3
"""countertest.py - do the processor's counters count floating-point operations right on this machine?

The compute monitor (tools/monitor/node_sampler.py) reports operations counted by the processor itself. This check
runs on a node, in its Linux, as any user:

    python3 tools/remote.py sh NODE -- python3 tools/monitor/countertest.py

Criterion (written before the first run, 2026-10-08): a loop y[i] = a*x[i] + y[i] over n = 10^6 values, 1000 times,
then one sum over y, does 2*n*1000 + n = 2.001e9 operations. The counted operations must lie within 1% of that, for a
scalar build and, where the processor has AVX2 and FMA, a vectorised FMA build (where each counted instruction does
four multiply-adds and the counter counts it twice).

First run, desktop node (i7-7740X, WSL2, 2026-10-08): 2.0011e9 counted in both builds, ratio 1.00003. Pass.
"""
import ctypes
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

KERNEL = r"""
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  long n = 1000000, reps = atol(argv[1]);
  double *x = malloc(n * sizeof *x), *y = malloc(n * sizeof *y), a = 1.000001;
  for (long i = 0; i < n; i++) { x[i] = (double)i; y[i] = 1.0; }
  for (long r = 0; r < reps; r++)
    for (long i = 0; i < n; i++) y[i] = a * x[i] + y[i];
  double s = 0; for (long i = 0; i < n; i++) s += y[i];
  printf("%.6e\n", s);
  return 0;
}
"""
FP_GROUPS = [(0x03, 1), (0x04, 2), (0x18, 4), (0x20, 8)]
FP_GROUPS_AVX512 = [(0x03, 1), (0x04, 2), (0x18, 4), (0x60, 8), (0x80, 16)]


def counter(libc, config):
    """this process and the children it starts later, user space only (allowed without root)"""
    attr = bytearray(64)
    flags = (1 << 1) | (1 << 5) | (1 << 6)  # inherit, exclude_kernel, exclude_hv
    struct.pack_into("<IIQQQQQIIQ", attr, 0, 4, 64, config, 0, 0, 3, flags, 0, 0, 0)
    buf = (ctypes.c_char * 64).from_buffer(attr)
    fd = libc.syscall(ctypes.c_long(298), buf, ctypes.c_int(0), ctypes.c_int(-1), ctypes.c_int(-1), ctypes.c_ulong(8))
    if fd < 0:
        e = ctypes.get_errno()
        raise OSError(e, "perf_event_open(config %#x): %s" % (config, os.strerror(e)))
    return fd


def count(libc, groups, exe, reps):
    fds = [(w, counter(libc, (umask << 8) | 0xC7)) for umask, w in groups]
    subprocess.run([exe, str(reps)], check=True, capture_output=True)
    total = 0.0
    for w, fd in fds:
        v, enabled, running = struct.unpack("<QQQ", os.read(fd, 24))
        total += w * (v * enabled / running if running else 0.0)
        os.close(fd)
    return total


def main():
    if platform.machine() != "x86_64":
        print("countertest: SKIP, written for x86_64")
        return 0
    flags = open("/proc/cpuinfo").read()
    pmu = open("/sys/bus/event_source/devices/cpu/caps/pmu_name").read().strip() if os.path.exists(
        "/sys/bus/event_source/devices/cpu/caps/pmu_name") else ""
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        print("countertest: SKIP, no C compiler")
        return 0
    groups = FP_GROUPS_AVX512 if " avx512f" in flags else FP_GROUPS
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    n, reps = 1000000, 1000
    expect = 2 * n * reps + n
    builds = [("scalar", ["-O2", "-fno-tree-vectorize", "-ffp-contract=off"] +
               (["-fno-vectorize", "-fno-slp-vectorize"] if "clang" in cc else []))]
    if " avx2" in flags and " fma" in flags:
        builds.append(("avx2-fma", ["-O3", "-mavx2", "-mfma", "-ffp-contract=fast"]))
    failed = 0
    with tempfile.TemporaryDirectory(prefix="countertest.") as d:
        src = os.path.join(d, "k.c")
        open(src, "w").write(KERNEL)
        for name, opts in builds:
            exe = os.path.join(d, name)
            subprocess.run([cc] + opts + ["-o", exe, src], check=True)
            got = count(libc, groups, exe, reps)
            ok = abs(got / expect - 1) < 0.01
            failed += not ok
            print("countertest %-8s counted %.4e, expected %.4e, ratio %.5f: %s (pmu %s)" %
                  (name, got, expect, got / expect, "pass" if ok else "FAIL", pmu or "?"))
    print("countertest: %s" % ("all passed" if not failed else "%d FAILED" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
