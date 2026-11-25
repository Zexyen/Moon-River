# runtime/freestanding

The C library headers the player's PC compiles against: `string.h`, `math.h`, `stdio.h`, `stdlib.h` and `setjmp.h`,
cut down to what the lifted game code (`build/gen_eh/*.c`), `runtime/gen/sdkdata.c`, `runtime/gen/sdklift_gen.c` and
`runtime/mr_tables.c` use. `mrbuild` compiles those with `-nostdlibinc -isystem runtime/freestanding`, so the compiler
needs no libc development package: `stdint.h`, `stddef.h` and `stdarg.h` come from clang's own resource directory.

Each declaration is glibc's own, with the same attributes (`nothrow`, `const`, `pure`, `nonnull`), and the macros are
the ones glibc's `math.h` defines, so clang makes the same code from them: `mrbuild check-freestanding`
(`tools/mrbuild/src/freestanding_check.rs`) compiles stand-in lifted code with these headers and with the system's and checks that every object file is the
same, byte for byte. The rest of the runtime (prebuilt in CI, or by `mrbuild` on a developer's PC) uses the system's
headers as before.

`jmp_buf` is glibc's x86-64 layout (200 bytes): `SdkJmpRec` in `sdklift.h` holds one and is shared between
`sdklift_gen.c` (built here) and `sdklift_rt.c` (built against glibc), and the comparison checks its size too.

On Windows (`_WIN32`, the MinGW target `mrbuild` gives clang there) the headers follow mingw-w64's UCRT ones instead:
`jmp_buf` is its 256-byte layout and `setjmp` expands as mingw-w64's does (the UCRT's `_setjmp` with the frame from
`__builtin_sponentry`), and `ldexpf` / `frexpf`, which the UCRT lacks, are inline over the double versions as mingw-w64
gives them. `mrbuild check-freestanding --target windows` checks those objects against mingw-w64's headers.

On macOS (`__APPLE__`, Apple silicon) only `jmp_buf` differs: the system's 48 `int`s (192 bytes), with its `setjmp`. The
other declarations are the same C functions in libSystem.
