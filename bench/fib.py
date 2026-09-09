#!/usr/bin/env python3
"""Recursive fibonacci, for comparison against bench/fib.c.

Same algorithm and therefore the same call count, so ns/call lines up directly
with what the shibe bench reports.

    python3 bench/fib.py [n] [repeat]
"""

import sys
import time

FIB_MAX_N = 47


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def fib_ref(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a


def fib_calls(n):
    """calls(0) = calls(1) = 1, calls(n) = 1 + calls(n - 1) + calls(n - 2)"""
    if n <= 1:
        return 1
    prev, cur = 1, 1
    for _ in range(2, n + 1):
        prev, cur = cur, 1 + cur + prev
    return cur


def usage(file):
    print("usage: python3 bench/fib.py [n] [repeat]", file=file)
    print(file=file)
    print(f"  n       fibonacci index, 0 to {FIB_MAX_N} (default 30)", file=file)
    print("  repeat  timed runs to take the best of (default 3)", file=file)


def main(argv):
    n = 30
    repeat = 3

    positional = []
    for arg in argv[1:]:
        if arg in ("-h", "--help"):
            usage(sys.stdout)
            return 0
        positional.append(arg)

    if len(positional) > 2:
        print("too many arguments", file=sys.stderr)
        usage(sys.stderr)
        return 1
    try:
        if len(positional) >= 1:
            n = int(positional[0])
        if len(positional) >= 2:
            repeat = int(positional[1])
    except ValueError:
        usage(sys.stderr)
        return 1

    if n < 0 or n > FIB_MAX_N:
        print(f"n must be between 0 and {FIB_MAX_N}, got {n}", file=sys.stderr)
        return 1
    if repeat < 1:
        print(f"repeat must be at least 1, got {repeat}", file=sys.stderr)
        return 1

    # The recursion goes n deep, well under the default limit, but say so
    sys.setrecursionlimit(max(sys.getrecursionlimit(), n + 100))

    expected = fib_ref(n)
    calls = fib_calls(n)
    best = None

    for i in range(repeat):
        start = time.perf_counter()
        result = fib(n)
        elapsed = time.perf_counter() - start

        if result != expected:
            print(f"run {i} returned {result}, expected {expected}", file=sys.stderr)
            return 1

        if best is None or elapsed < best:
            best = elapsed

    print(f"python   fib({n}) = {expected}")
    print(f"  runs       {repeat}")
    print(f"  best       {best:.4f} s")
    print(f"  calls      {calls}")
    if best > 0:
        print(f"  ns/call    {best * 1e9 / calls:.2f}")
        print(f"  calls/s    {calls / best / 1e6:.2f} M")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
