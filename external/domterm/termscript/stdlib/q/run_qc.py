#!/usr/bin/env python3
"""run_qc.py -- regenerate Termscript stdlib headers from domqlib Q templates.

Q itself ships only as sourceless .pyc in domlibs/domqlib/q/__pycache__
(no .py sources are vendored), so this driver stages those bytecode files
into a temp package dir and drives QCompiler from there.  Requires CPython
matching the bytecode (3.14) plus tkinter (stdlib).

Usage:
  python3 run_qc.py --qcache <dir-with-__pycache__> --q tsvec.q --out <dir>

Outputs (written atomically-ish, only when changed):
  <out>/ts_valvec.h   T=TS_Value    PREFIX=ts_valvec  FREE=ts_value_free
  <out>/ts_strvec.h   T="char *"    PREFIX=ts_strvec  FREE=ts_std_free_cstr
"""
import argparse
import shutil
import sys
import tempfile
from pathlib import Path


def load_q(qcache: Path):
    staged = Path(tempfile.mkdtemp(prefix="ts_qc_"))
    pkg = staged / "q"
    pkg.mkdir()
    for pyc in qcache.glob("*.pyc"):
        stem = pyc.name
        if stem.endswith(".cpython-314.pyc"):
            stem = stem[: -len(".cpython-314.pyc")] + ".pyc"
        shutil.copy(pyc, pkg / stem)
    sys.path.insert(0, str(staged))
    from q import QCompiler  # noqa: E402

    return QCompiler


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qcache", required=True)
    ap.add_argument("--q", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    QCompiler = load_q(Path(args.qcache))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    specs = [
        ("ts_valvec.h", {"T": "TS_Value", "PREFIX": "ts_valvec",
                         "FREE": "ts_value_free"},
         ["<termscript/termscript.h>"]),
        ("ts_strvec.h", {"T": "char *", "PREFIX": "ts_strvec",
                         "FREE": "ts_std_free_cstr"},
         ['"termscript/stdlib/common/ts_std_common.h"']),
    ]
    for fname, params, extra_includes in specs:
        qc = QCompiler(search_path=[str(Path(args.q).parent)])
        header = qc.compile(args.q, **params)
        # Q's @include lines come from the template; prepend the
        # instantiation-specific dependency after the generated comment.
        lines = header.splitlines(keepends=True)
        insert_at = 0
        for i, line in enumerate(lines):
            if line.startswith("#ifndef"):
                insert_at = i
                break
        includes = "".join(f"#include {inc}\n" for inc in extra_includes)
        header = "".join(lines[:insert_at]) + includes + "".join(lines[insert_at:])
        dest = out / fname
        old = dest.read_text() if dest.exists() else None
        if old != header:
            dest.write_text(header)
            print(f"run_qc: wrote {dest}")
        else:
            print(f"run_qc: {dest} up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
