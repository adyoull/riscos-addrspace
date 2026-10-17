# AddrSpace

AddrSpace is a small RISC OS desktop program that shows what is using the
**logical address space**: the 4GB of addresses all memory has to fit into.
When it runs out, programs stop with *"Unable to allocate logical address
space"*, however much memory is free.

Every dynamic area reserves addresses for the most it can ever grow to, when
it is made. AddrSpace lists the areas with what each reserves, who it
probably belongs to and what changed, and can measure how much address space
is really left.

- **The list:** Name, Owner, Size, Reserved, Base and Notes, read again every
  few seconds. Click a heading to sort.
- **Owners:** the running task whose name the area's starts with, RISC OS for
  its own areas, or a module: ARMEABISupport for `mmap#N` and `UnixLib
  stacks`, SOManager for `Shared Libraries`, or the module whose name the
  area's starts with (StreamManager's `StreamManager heap`). A UnixLib
  program's heap (`<program> Heap`, `Heap 2`...) whose program isn't running
  is marked "left behind?".
- **Changes:** new areas, ones that grew or shrank, and ones that have gone,
  coloured for 30 seconds.
- **Measure free space:** makes empty areas, each as big as RISC OS allows,
  until no more fit, adds up their sizes and removes them. This gives the free
  space and the largest free piece (the biggest area that would fit).
- **Save list** and **Log changes:** plain text, to send with a report.

AddrSpace makes no dynamic area of its own (its memory is in its WimpSlot),
so it doesn't change what it shows.

## Needs

- RISC OS 5 on an ARMv7 machine (Raspberry Pi 2, 3, 4, 400...).
- SharedUnixLibrary 1.16 or later and ARMEABISupport, from PackMan.

## Building

Cross-compiled with GCCSDK's GCC 10.2 (`arm-riscos-gnueabihf`) and UnixLib
5.0.3.3, most easily in [riscos-crossdev](https://github.com/adyoull/riscos-crossdev)'s
container image (the version `.riscos-deps` names):

```
docker run --rm -v "$PWD":/src ghcr.io/adyoull/riscos-crossdev:1.8.1 \
  sh -c 'sh build/build.sh && sh build/package.sh && sh tests/host/run.sh'
```

- `build/build.sh` makes `build/out/!RunImage,ff8` and checks it.
- `build/package.sh` makes `dist/AddrSpace-<version>.zip`.
- `tests/host/run.sh` runs the host tests (arm-linux under qemu-arm-aligntrap, or qemu-arm).

`build/env.sh` finds the toolchain:
- riscos-crossdev's (`RISCOS_TOOLCHAIN`), else `~/gccsdk/env`;
- or set `GCCSDK_ENV`, `CROSS` and `ELF2AIF`.

`build.sh` checks that every large stack frame is probed, that the program was
linked with UnixLib 5.0.3.3, and that it has no heap dynamic area. Where
riscos-crossdev is installed, it also runs its `checks/check-all.sh`.

GitHub builds every push in that image (`.github/workflows/riscos-build.yml`).
Every day, `riscos-deps-update.yml` looks for a newer riscos-crossdev release
and opens a pull request to move to it, built and tested.

## Source

- `src/areas.c`: reading the areas and tasks, changes, sorting, measuring
  the free space, the text written. Every SWI goes through `as_swi()`.
- `src/sys.c`: `as_swi()` on RISC OS.
- `src/main.c`: the desktop front end.
- `tests/host`: `areas_test` and `ui_test` against fake areas and a fake
  Wimp.

## Licence

GNU General Public License, version 2 or later (`COPYING`).
