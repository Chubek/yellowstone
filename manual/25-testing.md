# Chapter 25: Testing and Diagnostics

Run the qbfd regression tests, CTest command checks, script validation, and small object link smoke tests. Errors carry context and source labels.

## ISA checks

Build the project and run the complete suite:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

For a focused ISA smoke test, assemble a small object and inspect it:

```sh
cc -c sample.s -o sample.o
build/qobjdump -m --isa-dir infobank/isa sample.o
build/qobjdump -d --isa-dir infobank/isa sample.o
```

A missing or malformed `.isa` file is reported with its path while object inspection continues far enough to show the unavailable architecture. This lets format tests distinguish an object-reader failure from an ISA-database failure.
