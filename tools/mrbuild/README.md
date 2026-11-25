# mrbuild

The Moon River build in one program: from your own dump to `build/moon-river-game`. It began as a port of the Python
build (`scripts/setup.py` and the scripts it ran, removed from the repository on 2026-11-02):

| step | was (Python, removed) | here |
|---|---|---|
| deps | setup.py `step_deps` | `src/setup.rs` (also checks the libraries the runtime needs) |
| verify, convert | setup.py, `tools/extract/nso2elf.py` | `src/setup.rs`, `src/nso.rs` |
| mods | `scripts/pchtxt.py` | `src/pchtxt.rs` ("Mods" below) |
| tables | `runtime/gen/make_gen.py`, `lifter/sdklift.py`, `scripts/nvn_procs.py` | `src/tables.rs`, `src/sdklift.rs`, `src/nvn_procs.rs` (+ `runtime/gen_static/`) |
| lift | `lifter/recomp.py`, `lifter/recomp_eh.py` | `src/lifter/` |
| compile | `scripts/compile_main.py` | `src/compile.rs` |
| link | `scripts/build_runtime.py` | `src/link.rs` (split differently: "Prebuilt runtime" below) |
| check | `lifter/test_ext.py`, `lifter/test_mr.py` (Unicorn) | skipped: a developer's check, `tools/lifter-check` (below) |

Every file those steps wrote, it writes byte for byte the same, and its output lines, `build/setup_state.json` and
`build/logs/` are the same; the regression tests below keep it so. Two things differed on purpose: the lifted code is
compiled against `runtime/freestanding` instead of the system's C library headers (the same objects, checked by
`mrbuild check-freestanding`; a build made with `setup.py` was compiled once more because the compile flags changed),
and the link step splits the runtime from the player's tables (below).

```
cargo build --release --manifest-path tools/mrbuild/Cargo.toml
tools/mrbuild/target/release/mrbuild setup --dump <folder|.nsp|.xci> [--update <.nsp|.xci>] [--keys <prod.keys>]
                                           [--jobs N] [--only <step> | --from <step>] [--status] [--accept-modified]
                                           [--move-dump] [--mods <folder>] [--llvm <folder>]
```

`--jobs N` defaults to half the logical CPUs (at most 24), for `compile`.

The launcher runs `build/mrbuild` (a release) or `tools/mrbuild/target/release/mrbuild` (a checkout); in a checkout
where it was never built, the launcher says to run the `cargo build` line above.

## Steps

Your dump can be either your own Switch **NSP / XCI** packages (base + Update 1.2, a merged XCI/NSP, or a folder holding
both) together with your own console `prod.keys` (`src/switch/`; no keys or game data are ever shipped), or an already-extracted
folder holding `exefs/` (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`, plain NSO files) and `romfs/`. Every step derives its
files from the dump into `targets/` and `build/` (both ignored by git; never upload them). A finished step is skipped;
`--from <step>` repeats it and the rest.

| step | what |
|---|---|
| deps | the compiler (`deps/llvm` or the system's clang and lld) and, when the runtime is not prebuilt, its libraries (always runs) |
| verify | the dump is the exact version this port handles (SHA-256, NSO build ID, RomFS manifest; `src/setup.rs`); explains a mismatch |
| convert | extracts ExeFS (`targets/exefs`) and RomFS (`targets/romfs`) when given NSP/XCI, or places an extracted folder into `targets/`; converts `main`, `sdk`, `subsdk0` (NSO) -> `targets/exefs/*.elf` (LZ4, segment hashes checked) |
| mods | optional: code mods (`*.pchtxt`, `*.ips`) patched into `targets/exefs/main.elf` ("Mods" below; always runs, to notice a changed mods folder) |
| tables | `runtime/gen/` from your sdk and main (layout, imports, lifted SDK functions, SDK constants), plus the copies of `runtime/gen_static/` |
| lift | your main.elf -> `build/gen_eh` (C) |
| compile | `build/gen_eh` -> `build/obj` (clang -O2; the longest step). For this PC's own processor: `-march=x86-64-v3` with AVX2 and FMA, `-march=x86-64-v2` with SSE4.2, neither on an older one (`Target::cpu_level`; `MR_CPU_LEVEL=1\|2\|3` forces one). A build copied to an older processor must be compiled again there |
| link | `build/game.so` with the runtime (prebuilt, or built here); `game.dll` and `moon-river-game.exe` on Windows |
| check | skipped: the lifter's check against Unicorn is `tools/lifter-check` ("Regression tests" below) |

## Other commands

Mostly for developers; `--repo <folder>` names the checkout (default: the current folder).

- `extract <base.nsp|.xci|folder> [<update.nsp>] [--keys <prod.keys>] --out <folder>`: standalone extraction of `exefs/` and `romfs/` from your own Switch packages.
- `find-keys [--dump <path>] [--update <path>]`: prints the path to an auto-discovered `prod.keys` file if one exists.
- `nso2elf <in> <out>`, `lift <main.elf> <outdir> [--no-patches]`, `tables <repo>`, `compile [--jobs N]`, `link`: one step on its own.
- `lift-words`: lines `<word> <pc> <entry> <end>` (hex) on stdin, one lifted statement a line out.
- `runtime --out <dir>` (the prebuilt runtime, CI).
- `smoke [--prebuilt <dir>] [--lib <file>]... [--target windows] [--wine <program>]`: the link step on the committed
  stand-in modules, run until the image is mapped (CI's smoke test, below).
- `check-freestanding [--words N] [--seed S] [--clang <c>] [--gen <dir>] [--tables <dir>] [--target windows] [--jobs N]`:
  lifted code (random words by default, 60000, seed 1, or your `--gen build/gen_eh`, plus `--tables runtime/gen`)
  compiled with `runtime/freestanding` and with the system headers, which must give identical objects (CI).
- `fake-nso <out.nso> [functions] [--seed N] [--extra <file.c|.s>]... [--ld <flag>]...`: the stand-in modules of
  `testdata/fake_main`, made with clang and ld.lld for AArch64 (`src/fake.rs` has the commands for the committed
  `main.nso` and `sdk.nso`).
- `mods-tree <mods folder> [<main NSO>] [--ini=<mods.ini>]` (the launcher's Mods window),
  `mods-apply <mods folder> <main NSO> <in.elf> <out.elf> [--ini=<mods.ini>]`.
- `sha256 <file>` (the launcher's updater).
- `nvn-procs [<sdk.elf>] [name...]`: the sdk's NVN entry points, for `games/standin/lifter/sdklift_roots.txt`.
- `fps-census <census at 60> <census above 60> [<fps_sites.txt>] [--all]` (`Witch Time's docs/STATUS.md`, "Frame rates above 60").
- `lprof <lprof.txt> [threads (6)] [functions per list (25)]`: an `MR_LPROF` sample file (Linux).

## Mods

The `mods` step (`src/pchtxt.rs`; `mods-tree` and `mods-apply` run it on its own) applies emulator-style code mods:
IPSwitch text patches (`.pchtxt`) and IPS / IPS32 patches (`.ips`), the formats Yuzu / Ryujinx mods use, to
`targets/exefs/main.elf` before the lift. The repository ships only this mechanism, never anyone's patches.

- **Where**: files are read from the mods folder (`mods/`, or `--mods <folder>`) and every folder under it (an emulator
  mod pack's `<mod>/exefs/` layout works as it is), in path order; a later file wins where two patch the same bytes.
- **Switches**, like the emulators' per-mod check boxes: `config/mods.ini` (the default `--ini`; the launcher's Mods
  window writes it), section `[mods]`, lines `<key> = off` or `<key> = on`. A key is a mod (the first folder below the
  mods folder, or a file lying directly in it), a file (its path below the mods folder, with `/`), or one block of a
  `.pchtxt` (`<file> :: <block title>`, or `<file> :: #<n>` for an untitled or repeated title; `on` also applies a
  block written under `@disabled`). Keys are compared without case. Anything not listed keeps its default: mods and
  files on, blocks as their `@enabled` / `@disabled` line says. A mod or file switched off is not read at all, so one
  for another version of the game does not stop the build. Renaming a file (e.g. to `.pchtxt.off`) also leaves it out.
- **Offsets**, as the emulators apply them: the patched buffer is the 0x100-byte NSO header followed by the program
  image (text, rodata, data at their memory offsets). So a `.pchtxt` offset X means image offset X + offset_shift -
  0x100 (mods written with `@flag offset_shift 0x100` give image offsets directly), and an `.ips` offset means offset -
  0x100. Image offset = the address minus the module base; `mrbuild nso2elf` writes the image at file offset 0x1000 +
  image offset (found through the ELF's PT_LOAD headers).
- **Build ID**: every file must name the build ID of your main (`@nsobid-<id>` / `<id>.ips`); a file for another build
  or module is refused.

## Prebuilt runtime

The runtime (`libmoonriver.so`, which the launcher loads, `moon-river-game`, which runs it as a program of its own,
and `libmr_glslang.so`) holds nothing from the dump: the numbers it needs from the player's `main.elf` and `sdk.elf`
are `extern const` variables there (`runtime/split`), defined in `game.so` by `runtime/mr_tables.c` together with the
dump's sdk tables (`runtime/gen/sdkdata.c`, `sdklift_gen.c`). So the release workflow builds it once
(`mrbuild runtime --out prebuilt`, against its own SDL3, FFmpeg 7.1 and a static glslang) and ships it in `prebuilt/`
with a `manifest.json` that records the hash of the sources it was built from (`runtime/` but `gen/`, and
`games/standin/lifter/sdklift_install.txt`). The link step copies it into `build/` when that hash matches and builds the runtime
itself otherwise (a checkout, or a release whose `runtime/` was edited), so changing the runtime works as before.

What the player's PC compiles (the lifted code and those three files) uses only `runtime/freestanding` and clang's own
headers, and `game.so` links with `-nostdlib` and the compiler's builtins: no libc development package, no system
libraries. With the bundled compiler (`deps/llvm`, clang 23 and lld from LLVM's release, found before the system's)
a player installs nothing to build. `mrbuild smoke` runs the tables, lift, compile and link steps on the stand-in
modules (`testdata/fake_main`), checks that every symbol resolves (the game functions `games/standin/runtime/game_shim_eh.c` declares
may stay undefined) and runs `moon-river-game` until it has mapped the image; the release workflow runs it on the
runtime it ships.

### Windows

The same split, with Windows' files: the runtime is one program, `moon-river-game.exe`, built with llvm-mingw
(`mrbuild runtime --target windows --out prebuilt`, which CI runs on Linux), and the player's PC links `game.dll`. The
program imports the tables from `game.dll` through `libgame.dll.a` (made by llvm-dlltool from the export list,
`GAME_EXPORTS` in `src/link.rs`); `game.dll` imports nothing from the program, and its calls back into the runtime go
through the function pointers `game_bind` hands it (`runtime/game_shim.c`). `game.dll` links with `-nostdlib` and its
own entry point, takes its C functions from `ucrtbase.dll` (mingw-w64's `libucrtbase.a`, shipped in `prebuilt/`) and
llvm-mingw's builtins. Every clang gets `--target=x86_64-w64-mingw32` (`src/target.rs`), so the bundled compiler is
LLVM's own Windows release plus those builtins in `lib/clang/23/lib/windows/`. The `deps` step checks the DLLs the
program imports (its PE import table) against `build\`, `prebuilt\` and System32. `mrbuild smoke --target windows`
runs the smoke test there, or on Linux under Wine; `mrbuild check-freestanding --target windows` checks the freestanding
headers against mingw-w64's.

### macOS

Apple silicon only, built on a Mac (`src/target.rs`, `Target::MacOS`; there is no cross build and no prebuilt runtime
yet). Linux's split with Mach-O's means: `libmoonriver.dylib` and `game.dylib` are both linked with `-undefined
dynamic_lookup`, so what each takes from the other is looked up when the game starts, and the runtime names
`game.dylib` (`@rpath`, beside it) as a library it needs. Apple's clang and linker are used (no `-fuse-ld=lld`, no
`-mcx16`); the runtime's libraries are Homebrew's (`-I` / `-L` of `HOMEBREW_PREFIX`, else `/opt/homebrew`), with
`runtime/macos` standing in for the two headers glibc and Mesa have and macOS lacks (`malloc.h`, `GL/gl.h`). Apple's
linker refuses a library without libSystem, so `game.dylib` is not `-nostdlib`: the player's Mac needs the Command Line
Tools' SDK. The lifted code is compiled with `runtime/freestanding` as elsewhere (`jmp_buf` is the system's there).
`mrbuild smoke` runs as on Windows, with stand-ins for the game functions the runtime replaces (dyld looks up every
symbol when it loads the library).

## Regression tests

`cargo test --release --manifest-path tools/mrbuild/Cargo.toml` needs no dump; CI runs it on Linux
(`.github/workflows/mrbuild.yml`) and on Windows (`release.yml`). The tests that pin output compare it with the golden
files in `testdata/golden`, made from stand-in programs with no game or sdk code. When the Python was removed
(2026-11-02), each golden was shown to be equal to what the Python wrote for the same input; before that, the
`compare_*.py` scripts had checked the port against it (5 x 1M lifter words, 900 mod folders, 50 text archives and the
14 files of the tables step, all identical).

| test | what | golden |
|---|---|---|
| `lifter::tests::lift_words_digest` | 4,000,000 random words through `lift-words` (35 % FP / SIMD, 20 % load / store, 10 % data processing with registers, the rest any word) | `lift_words.sha256`, the output's sha256 |
| `lifter::tests::exclusive_pairs` | `ldaxp` / `stlxp`, every register and both sizes (2,162,688 words), against the statements they must give | none (asserted in the test) |
| `lifter::eh::tests::lift_fake_main` | `nso2elf` and `lift --no-patches` of `testdata/fake_main/main.nso`: the conversion report, the sha256 of the ELF and of each C file, and the lift's log (510 functions, 24,850 instructions, 16 landing pads) | `lift_main.txt` |
| `tables::tests::stand_ins` | the tables step on the stand-ins (`main.nso`, `sdk.nso`, `roots.txt`) in a tree with no `runtime/gen/`: the 8 files made from the dump and the printed lines, `sdk.elf` by its sha256, the 5 `runtime/gen_static` copies byte for byte | `tables/` |
| `pchtxt::tests::random_mod_folders` | 60 random mod folders against a synthetic NSO and ELF: tree lines with and without the main, apply output and patched ELF, errors included (shown equal to `pchtxt.py` on 300 folders); plus unit tests of unescape, IPS, `mods.ini` keys and build IDs | `pchtxt_mods.txt` |

Other tests pin the compile and table flags (`compile::tests::lifted_flags_unchanged`, `link::tests::table_flags_unchanged`)
and check the smaller commands one by one.

- **After a deliberate change** of the output, `MRBUILD_BLESS=1 cargo test --release` rewrites the goldens; commit them
  with the change. A failing test prints the first line that differs and leaves the new output in the temporary
  folder (`mrbuild-golden-<pid>-<name>`, `mrbuild-lift_words-<pid>.in` / `.txt`, `mrbuild-lift-<pid>/`) to diff
  against; an older build's `mrbuild lift-words < mrbuild-lift_words-<pid>.in` or `mrbuild lift <ELF> <dir> --no-patches`
  gives the other side.
  `MRBUILD_CASES=<dir>` keeps the random cases of the pchtxt and text tests in `<dir>/pchtxt` and `<dir>/text`.
- **The stand-ins** are `testdata/fake_main/main.nso` and `sdk.nso`, built by `mrbuild fake-nso` from the sources beside
  them (the exact commands are in `src/fake.rs`'s module comment). Regenerating them needs clang and ld.lld with the
  AArch64 target; another clang makes other code, so re-bless the goldens in the same commit. The root
  `.gitattributes` keeps `testdata` byte for byte (`-text`) in Windows checkouts.
- **Freestanding headers**: CI runs `mrbuild check-freestanding --words 200000 --seed 1 --jobs 4`;
  `mrbuild check-freestanding --gen build/gen_eh --tables runtime/gen` checks your own lift and tables on your PC.
- **Link step**: `mrbuild smoke` ("Prebuilt runtime" above), in release.yml on Linux and on a Windows runner.
- **Your own dump**: the stand-ins never reach what only the real main has (the Options-row patches, `UI_SIZE_SITES`,
  `UI_MARK_SITES`, the frame counters) or the sdk paths their code does not take, and nothing derived from a dump is
  committed. The compare scripts' dump modes (`compare_lift.py --main`, `compare_tables.py --exefs`,
  `compare_text.py --romfs --dump`) went with the Python. Their replacement is to compare two builds on your PC: run
  `mrbuild lift targets/exefs/main.elf <dir>` (with the patches) and `mrbuild tables .` (it writes `runtime/gen`)
  with the old and the new mrbuild, then `diff -r` the outputs; the lift is deterministic.

### The lifter against Unicorn (tools/lifter-check)

The lifter's semantic check is a crate of its own, for developers only: Unicorn is GPL-2.0, so it is never built for or
shipped to players. It lifts each word through `mrbuild lift-words` (build mrbuild first), runs it in Unicorn and its
statement in C with the real runtime helpers (`runtime/runtime.h`, `rt_ext.h`), and compares all 32 vector registers,
X0..X30, SP and a buffer:

```
cargo run --release --manifest-path tools/lifter-check/Cargo.toml -- [--main <main.elf> | --synthetic N] [--trials N] [--seed N] [--keep] [--mrbuild <path>]
```

`--main` (default `targets/exefs/main.elf`) takes every word of your main in the forms B2recomp and Witch Time added to
the lifter (those `lifter/test_ext.py` and `test_mr.py` checked), `--synthetic N` random words of those forms with no
dump (CI: `--synthetic 2000 --seed 1`, in `mrbuild.yml`). It needs clang, cmake and libclang, and runs on Linux.
**Open**: on B3's main it exits 1. `sqrdmulh` / `sqdmulh` by element is wrong when both 32-bit lanes are INT32_MIN:
`v_ielem` in `runtime/rt_ext.h` overflows its 64-bit product. Not fixed yet: any change to `rt_ext.h`
recompiles every player's game. `--synthetic 2000` passes.
