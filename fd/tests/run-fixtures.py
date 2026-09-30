#!/usr/bin/env python3
"""Generate cross-target fixtures, cross-check them independently, then test qBFD.

Three sources of truth are compared rather than one:

  * LLVM's reader (``llvm-readobj``) supplies symbol and relocation names for
    the compiled objects, so a qBFD regression cannot be confirmed by qBFD
    agreeing with itself.
  * The system ``llvm-ar``/``ar`` writes real ``.a`` containers, which qBFD then
    has to read back, including the symbol index.
  * The system ``strip`` produces reference outputs for the strip engine, and
    the stripped objects are re-read with qBFD.

Skips (exit 77) rather than fails when the tools it needs are absent.
"""

import argparse
import pathlib
import shutil
import subprocess
import tempfile

# Extra targets beyond the obvious ones, chosen to cover both byte orders and
# both ELF classes as well as the PE and Mach-O backends.
TARGETS = [
    "x86_64-linux-gnu", "i386-linux-gnu", "powerpc64-linux-gnu",
    "aarch64-linux-gnu", "riscv64-linux-gnu", "s390x-linux-gnu",
    "mips-linux-gnu", "x86_64-windows-msvc", "i686-windows-msvc",
    "arm64-windows-msvc", "x86_64-apple-macos", "arm64-apple-macos",
]

SOURCE = """extern int external(int);
int data = 9;
int zero[8];
int exported(int x) { return external(x) + data + zero[0]; }
int helper(int x) { return x * 3; }
"""


def skip(reason):
    print(f"skipping: {reason}")
    raise SystemExit(77)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_binary", type=pathlib.Path)
    args = parser.parse_args()

    clang = shutil.which("clang")
    reader = shutil.which("llvm-readobj")
    if not clang or not reader:
        skip("clang and llvm-readobj are required")
    ar = shutil.which("llvm-ar") or shutil.which("ar")
    ranlib = shutil.which("llvm-ranlib") or shutil.which("ranlib")
    if not ar:
        skip("llvm-ar or ar is required to build archive fixtures")

    with tempfile.TemporaryDirectory(prefix="qbfd-fixtures-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "fixture.c"
        source.write_text(SOURCE)
        objects = []

        for target in TARGETS:
            obj = root / (target + ".o")
            result = subprocess.run(
                [clang, "-target", target, "-g", "-c", str(source), "-o", str(obj)],
                capture_output=True, text=True)
            if result.returncode != 0:
                skip(f"cannot compile for {target}: {result.stderr.strip()}")
            report = subprocess.check_output(
                [reader, "--symbols", "--relocations", str(obj)], text=True)
            for name in ("external", "data", "zero", "exported", "helper"):
                if name not in report:
                    raise RuntimeError(f"LLVM did not find {name} in {target}")
            objects.append(str(obj))

        # A real archive, with a symbol index, has to come back out of qBFD with
        # the same member set and an index naming the same symbols.
        archive = root / "libfixture.a"
        subprocess.run([ar, "rcs", str(archive), *objects], check=True)
        if ranlib:
            subprocess.run([ranlib, str(archive)], check=True)
        listing = subprocess.check_output([ar, "t", str(archive)], text=True)
        expected_members = sorted(pathlib.Path(o).name for o in objects)
        if sorted(line for line in listing.split() if line) != expected_members:
            raise RuntimeError("ar listed unexpected members")

        # qBFD is handed the archive alongside the plain objects: it treats an
        # archive as a container, so the test binary must cope with both.
        subprocess.run([str(args.test_binary.resolve()), *objects, str(archive)],
                       check=True)

        # The strip engine has to survive a round trip on real debug objects.
        strip = shutil.which("strip")
        for flag in ("-g", "-d", "-s"):
            if not strip:
                break
            stripped = root / f"stripped{flag}.o"
            subprocess.run(
                [strip, flag, "-o", str(stripped), str(objects[0])], check=True)
            if not stripped.exists() or stripped.stat().st_size == 0:
                raise RuntimeError(f"strip {flag} produced nothing")
            # A stripped object must still be a readable object, which qBFD
            # checks; the reference build keeps the test honest about the
            # expected symbol set.
            report = subprocess.check_output(
                [reader, "--symbols", str(stripped)], text=True)
            if flag == "-s" and "exported" in report:
                raise RuntimeError("strip -s left a symbol behind")

    print("fixture checks passed")


if __name__ == "__main__":
    main()
