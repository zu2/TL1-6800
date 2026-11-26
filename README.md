# TL1-6800

This is a TL/1 cross-compiler for the MC6800 that runs on Linux.
It is in early development, and now it can compile only simple test programs.

It uses `as6800` and `ld6800` from Fuzix-Bintools to assemble and link.

```sh
make
./TL1 < program.tl1 > program.s
as6800 program.s
ld6800 -b -C 0x2222 -o program.bin program.o
```

The TL/1 runtime needed to run generated code is not included yet.

With `make test`, you can compare generated code with byte sequences taken from the original version.
