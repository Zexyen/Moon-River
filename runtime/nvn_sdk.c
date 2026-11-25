/* NVN texture storage rules answered by the SDK's own code: supports both nnSdk / NVN 3.6.0 (Bayonetta 2) and
 * nnSdk / NVN 13.3.2 (API 55.12). Same API (nvn_sdk.h); the SDK code is lifted from targets/exefs/sdk.elf by
 * tools/mrbuild/src/sdklift.rs. Every address comes from runtime/gen/nvn_sdk_addrs.h, which it writes from the player's
 * own sdk: NVN entry points by name from the proc table, the vtables by their typeinfo names, the format table and
 * chip descriptor by shape (tools/mrbuild/src/nvn_procs.rs gpu_model). Nothing here is a hard-coded sdk address.
 *
 * NVN 3.6.0 vs 13.3.2 is detected from SDKA_nvnTextureBuilderGetRawStorageClass (0 in NVN 3.6.0, which has no
 * GetRawStorageClass; non-zero in NVN 13.3.2).
 *
 * NVN 3.6.0 (Bayonetta 2):
 *   NvRmNvgpuDevice: vtable at +0, HAL object pointer at +0x370 (HAL size 0x28: [0] vtable, [1] chip descriptor)
 *   NVNdevice: +0x80 big page size 0x10000, +0x918 GPU device, +0x1998/+0x199c compression switches 0, +0x1ab0 comp=1
 *   Format table: 0x34-byte records
 *   NVNtextureBuilder: +0 dev, +0x18 flags, +0x1c target, +0x20 w, +0x24 h, +0x28 d, +0x2c levels, +0x30 fmt, +0x34 samples,
 *                      +0x58 u16 layout, +0x5c stride, +0x60 glname
 *   NVNtexture (0xb0): +8 dev, +0x20..+0x53 copy of builder +0x18..+0x4b, +0x54 format flags, +0x58 layout block
 *
 * NVN 13.3.2:
 *   NvRmNvgpuDevice: vtable at +0, HAL object pointer at +0xf0 (HAL size 0x38: [0] vtable, [1] chip descriptor)
 *   NVNdevice: +0xcc big page size 0x10000, +0x998 GPU device, +0x1a10/+0x1a14 comp switches 0, +0x1b30 linear kind 0, +0x1b34 comp=1
 *   Format table: 0x30-byte records
 *   NVNtextureBuilder: +8 dev, +0x20..+0x3c flags..samples, +0x54 u16 layout, +0x58 stride, +0x5c glname
 *   NVNtexture (0xc0): packed record, +0x48 layout block */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"
#include "nvn_c5.h"
#include "nvn_sdk_addrs.h"

#define SDK(a) (SDK_BASE + (uint64_t)(a))
#define P(v) ((uint64_t)(uintptr_t)(v))

static int is_nvn_360(void) { return SDKA_nvnTextureBuilderGetRawStorageClass == 0; }

static uint8_t g_dev[0x1c00];                 /* NVNdevice model */
static uint64_t g_gpu_dev[0x380 / 8];         /* NvRmNvgpuDevice: vtable, HAL object at +0x370 (3.6.0) or +0xf0 (13.3.2) */
static uint64_t g_hal[0x38 / 8];              /* NvRmGpu_HwHalFunctions_GM200 */
static volatile LONG g_state;                 /* 0 not built, 1 building, 2 ready */

static void build_model(void) {
    if (!sdk_image_ready()) {
        printf("\nnvn_sdk: sdk.elf is not mapped; the texture storage rules are the SDK's own code and cannot be answered without it.\n");
        report_and_exit(3);
    }
    g_hal[0] = SDK(SDKA_vt_hal_gm200);
    g_hal[1] = SDK(SDKA_chip_descriptor);
    g_gpu_dev[0] = SDK(SDKA_vt_nvgpu_device);
    if (is_nvn_360()) {
        g_gpu_dev[0x370 / 8] = P(g_hal);
        *(uint32_t *)(g_dev + 0x80) = 0x10000;
        *(uint64_t *)(g_dev + 0x918) = P(g_gpu_dev);
        *(uint32_t *)(g_dev + 0x1998) = 0;
        *(uint32_t *)(g_dev + 0x199c) = 0;
        g_dev[0x1ab0] = 1;
    } else {
        g_gpu_dev[0xf0 / 8] = P(g_hal);
        *(uint32_t *)(g_dev + 0xcc) = 0x10000;
        *(uint64_t *)(g_dev + 0x998) = P(g_gpu_dev);
        *(uint32_t *)(g_dev + 0x1a10) = 0;
        *(uint32_t *)(g_dev + 0x1a14) = 0;
        *(uint32_t *)(g_dev + 0x1b30) = 0;
        g_dev[0x1b34] = 1;
    }
}
static void model(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_state, 1, 0);
        if (s == 2) return;
        if (s == 0) { build_model(); InterlockedExchange(&g_state, 2); return; }
        Sleep(0);
    }
}
static const uint8_t *fmt_rec(uint32_t format) {
    uint64_t stride = is_nvn_360() ? 0x34ull : 0x30ull;
    return (const uint8_t *)(uintptr_t)(SDK(SDKA_format_table) + stride * (format < SDKA_format_count ? format : 0));
}

static void real_builder(uint8_t *rb, const NvnSdkTexBuilder *b) {
    memset(rb, 0, 0x80);
    if (is_nvn_360()) {
        *(uint64_t *)(rb + 0) = P(g_dev);
        *(uint32_t *)(rb + 0x18) = b->flags;
        *(uint32_t *)(rb + 0x1c) = b->target;
        *(uint32_t *)(rb + 0x20) = b->width;
        *(uint32_t *)(rb + 0x24) = b->height;
        *(uint32_t *)(rb + 0x28) = b->depth;
        *(uint32_t *)(rb + 0x2c) = b->levels;
        *(uint32_t *)(rb + 0x30) = b->format;
        *(uint32_t *)(rb + 0x34) = b->samples;
        if (b->has_layout) *(uint16_t *)(rb + 0x58) = (uint16_t)(1u | ((b->layout & 0xfffu) << 4));
        *(uint32_t *)(rb + 0x5c) = b->stride;
        *(uint32_t *)(rb + 0x60) = b->glname;
    } else {
        *(uint64_t *)(rb + 8) = P(g_dev);
        *(uint32_t *)(rb + 0x20) = b->flags;
        *(uint32_t *)(rb + 0x24) = b->target;
        *(uint32_t *)(rb + 0x28) = b->width;
        *(uint32_t *)(rb + 0x2c) = b->height;
        *(uint32_t *)(rb + 0x30) = b->depth;
        *(uint32_t *)(rb + 0x34) = b->levels;
        *(uint32_t *)(rb + 0x38) = b->format;
        *(uint32_t *)(rb + 0x3c) = b->samples;
        if (b->has_layout) *(uint16_t *)(rb + 0x54) = (uint16_t)(1u | ((b->layout & 0xfffu) << 4));
        *(uint32_t *)(rb + 0x58) = b->stride;
        *(uint32_t *)(rb + 0x5c) = b->glname;
    }
}

static uint64_t run(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b, uint64_t x1) {
    uint8_t rb[0x80];
    model();
    real_builder(rb, b);
    c->x[0] = P(rb); c->x[1] = x1;
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
uint64_t nvn_sdk_texture_storage_alignment(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, SDKA_nvnTextureBuilderGetStorageAlignment, b, 0); }
uint64_t nvn_sdk_texture_storage_size(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, SDKA_nvnTextureBuilderGetStorageSize, b, 0); }
uint32_t nvn_sdk_texture_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { return (uint32_t)run(c, SDKA_nvnTextureBuilderGetStorageClass, b, 0); }
uint32_t nvn_sdk_texture_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b) {
    if (is_nvn_360()) { (void)b; guest_trap("nvnTextureBuilderGetRawStorageClass does not exist in NVN 3.6.0", c->x[30]); return 0; }
    return (uint32_t)run(c, SDKA_nvnTextureBuilderGetRawStorageClass, b, 0);
}
void nvn_sdk_texture_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { run(c, SDKA_nvnTextureBuilderGetSparseTileLayout, b, P(out)); }

static int real_texture(Ctx *c, uint8_t *rt, uint8_t *rb, const NvnSdkTexBuilder *b) {
    if (b->glname) return 0;
    model();
    real_builder(rb, b);
    memset(rt, 0, 0xc0);
    const uint8_t *fr = fmt_rec(b->format);
    if (is_nvn_360()) {
        memcpy(rt + 8, rb + 0, 8);
        memcpy(rt + 0x20, rb + 0x18, 0x34);
        rt[0x54] = (uint8_t)((fr[0xe] ? 1 : 0) | (fr[0xf] ? 2 : 0));
        c->x[0] = P(rt + 0x58); c->x[1] = P(rb);
        sdk_dispatch(c, SDK(SDKA_texture_layout));
    } else {
        *(uint64_t *)(rt + 8) = P(g_dev);
        *(uint64_t *)(rt + 0x18) = (uint64_t)(b->levels & 0xff) << 48 | (uint64_t)(b->samples & 0xff) << 56;
        *(uint64_t *)(rt + 0x20) = b->width | (uint64_t)(b->height & 0xffff) << 32 | (uint64_t)(b->depth & 0xffff) << 48;
        uint64_t h = (b->flags & 0xffffu) | (uint64_t)(b->format & 0x1ff) << 16 | (uint64_t)(b->target & 0x7f) << 25 |
                     (uint64_t)(fr[0xe] != 0) << 46 | (uint64_t)(fr[0xf] != 0) << 47;
        memcpy(rt + 0x28, &h, 6);
        c->x[0] = P(rt + 0x48); c->x[1] = P(rb);
        sdk_dispatch(c, SDK(SDKA_texture_layout));
    }
    return 1;
}
static uint64_t run_tex(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b, uint64_t x1, int *ok) {
    uint8_t rb[0x80], rt[0xc0];
    *ok = real_texture(c, rt, rb, b);
    if (!*ok) return 0;
    c->x[0] = P(rt); c->x[1] = x1;
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
int nvn_sdk_tex_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, SDKA_nvnTextureGetStorageClass, b, 0, &ok); return ok; }
int nvn_sdk_tex_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) {
    if (is_nvn_360()) { (void)b; (void)out; guest_trap("nvnTextureGetRawStorageClass does not exist in NVN 3.6.0", c->x[30]); return 0; }
    int ok; *out = (uint32_t)run_tex(c, SDKA_nvnTextureGetRawStorageClass, b, 0, &ok); return ok;
}
int nvn_sdk_tex_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { int ok; run_tex(c, SDKA_nvnTextureGetSparseTileLayout, b, P(out), &ok); return ok; }
int nvn_sdk_tex_storage_size(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, SDKA_nvnTextureGetStorageSize, b, 0, &ok); return ok; }

/* Format table: bit 0 of the first word = "this format exists" (nvnTextureBuilderGetStorageSize). */
int nvn_sdk_format_known(uint32_t format) {
    model();
    return format < SDKA_format_count && (fmt_rec(format)[0] & 1);
}

/* Self-test (MR_SDK_SELFTEST): answers read in our sdk's disassembly, plus a sweep that must not stop anywhere
 * (every method the choosers reach must have been lifted). MR_NVN_STORAGE_TABLE=<file> writes the sweep. */
int nvn_sdk_selftest(Ctx *c) {
    int bad = 0, n = 0;
    NvnSdkTexBuilder b;
    const char *out = getenv("MR_NVN_STORAGE_TABLE");
    FILE *f = out && *out ? fopen(out, "w") : NULL;
#define SET(fl, tg, w_, h_, d_, lv, fm, sm) do { memset(&b, 0, sizeof b); b.flags = (fl); b.target = (tg); b.width = (w_); b.height = (h_); b.depth = (d_); b.levels = (lv); b.format = (fm); b.samples = (sm); } while (0)
#define WANT(what, got, want) do { uint64_t g_ = (got), w_ = (want); n++; if (g_ != w_) { bad++; printf("  FAIL nvn storage %s: 0x%llx, expected 0x%llx\n", what, (unsigned long long)g_, (unsigned long long)w_); } } while (0)
    SET(0x40, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 6", nvn_sdk_texture_storage_alignment(c, &b), 0x80);
    SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 4", nvn_sdk_texture_storage_alignment(c, &b), 0x20);
    b.stride = 256; WANT("size, flag bit 4 = stride x height", nvn_sdk_texture_storage_size(c, &b), 256 * 64);
    for (uint32_t fm = 1; fm < SDKA_format_count; fm++) {
        if (!nvn_sdk_format_known(fm)) continue;
        uint32_t bpe = fmt_rec(fm)[0x10];
        SET(0, 10, 100, 1, 1, 1, fm, 0);
        WANT("alignment, buffer target = bytes per element, 12 counts as 4", nvn_sdk_texture_storage_alignment(c, &b), bpe == 12 ? 4 : bpe);
        WANT("size, buffer target = width x bytes per element", nvn_sdk_texture_storage_size(c, &b), 100ull * bpe);
        static const uint32_t flags[] = {0, 8, 0x100, 0x108, 0x1008};
        for (int tg = 0; tg < 10; tg++) for (int fi = 0; fi < 5; fi++) for (uint32_t sm = 0; sm <= 4; sm += 2) {
            if ((tg == 5 || tg == 6) != (sm != 0)) continue;
            SET(flags[fi], (uint32_t)tg, 128, tg == 0 || tg == 3 ? 1 : 64, tg == 8 ? 1 : tg == 9 ? 6 : tg == 2 || tg == 4 || tg == 6 || tg == 3 ? 4 : 1, 1, fm, sm);
            uint64_t al = nvn_sdk_texture_storage_alignment(c, &b), sz = nvn_sdk_texture_storage_size(c, &b);
            uint32_t cl = nvn_sdk_texture_storage_class(c, &b);
            n++;
            if (al != 0x200 && al != 0x10000) { bad++; printf("  FAIL nvn storage alignment 0x%llx for format 0x%x target %d flags 0x%x: neither 0x200 nor the page size\n", (unsigned long long)al, fm, tg, flags[fi]); }
            if (al == 0x10000 && (sz & 0xffff)) { bad++; printf("  FAIL nvn storage size 0x%llx not a multiple of the page size for format 0x%x target %d\n", (unsigned long long)sz, fm, tg); }
            if (f) fprintf(f, "%x\t%d\t%x\t%u\t%llx\t%llx\t%x\n", fm, tg, flags[fi], sm, (unsigned long long)al, (unsigned long long)sz, cl);
            uint32_t tsz = 0, tcl = 0;
            n++;
            if (!nvn_sdk_tex_storage_size(c, &b, &tsz) || (sz && tsz != (uint32_t)sz)) { bad++; printf("  FAIL nvn texture storage size: format 0x%x target %d flags 0x%x: texture 0x%x, builder 0x%llx\n", fm, tg, flags[fi], tsz, (unsigned long long)sz); }
            n++;
            if (!nvn_sdk_tex_storage_class(c, &b, &tcl) || tcl != cl) { bad++; printf("  FAIL nvn texture storage class: format 0x%x target %d flags 0x%x: texture 0x%x, builder 0x%x\n", fm, tg, flags[fi], tcl, cl); }
        }
    }
    if (f) fclose(f);
    {
        uint32_t v, sp[4];
        SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); b.stride = 256; v = 0;
        WANT("texture class, flag bit 4: 0x100 | linear kind 0", (nvn_sdk_tex_storage_class(c, &b, &v), v), 0x100);
        WANT("builder class, flag bits 4/6: 0x100 | linear kind 0", nvn_sdk_texture_storage_class(c, &b), 0x100);
        if (!is_nvn_360()) {
            WANT("builder raw class, flag bits 4/6: 1", nvn_sdk_texture_raw_storage_class(c, &b), 1);
        }
        SET(0, 10, 100, 1, 1, 1, 0x25, 0);
        WANT("builder class, buffer target: 0x100 | linear kind 0", nvn_sdk_texture_storage_class(c, &b), 0x100);
        SET(0, 1, 256, 256, 1, 1, 0x25, 0); sp[0] = sp[1] = sp[2] = sp[3] = 0xdeadbeef;
        nvn_sdk_texture_sparse_layout(c, &b, sp);
        WANT("sparse layout without the sparse flag: zero words 0,1", (uint64_t)sp[0] | sp[1], 0);
        WANT("sparse layout without the sparse flag: zero words 2,3", (uint64_t)sp[2] | sp[3], 0);
        b.glname = 7; v = 0x55;
        WANT("texture made from an OpenGL name is not modelled", nvn_sdk_tex_storage_class(c, &b, &v), 0);
    }
    printf("  nvn_sdk: %d texture storage cases, %d failed\n", n, bad);
    bad += nvn_c4_selftest();
    bad += nvn_c5_selftest(c);
    return bad;
}
