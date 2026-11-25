/* runtime/nnaudio.c: per-guest-object AudioOut isolation (Wwise + CRI simultaneous playback,
 * independent queues/released lists/states/events, SDK 13 no-event open, reopen, and AO_MAX limit).
 *   clang -O2 -Wall -D_GNU_SOURCE -Iruntime/posix -Iruntime/split -Iruntime -D__declspec\(x\)= \
 *     tests/nnaudio_test.c runtime/plat_posix.c -lm -lpthread -o /tmp/nnaudio_test && /tmp/nnaudio_test */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

/* Stubs for runtime symbols referenced by nnaudio.c */
const DynSym *game_dyn_syms_p;
const int *game_dyn_sym_count_p;
const DynSym dyn_syms[1];
const int dyn_sym_count = 0;

static uint64_t g_last_init_events[16];
static int g_init_event_count;
static uint64_t g_last_signalled[64];
static int g_signal_count;

void os_init_event(uint64_t key, int autoclear) {
    (void)autoclear;
    if (g_init_event_count < 16) g_last_init_events[g_init_event_count++] = key;
}
void os_signal_event(uint64_t key) {
    if (g_signal_count < 64) g_last_signalled[g_signal_count++] = key;
}
const char *guest_thread_name_of(DWORD tid) { (void)tid; return "test-thread"; }
void register_imports(const ImportDef *defs, int n) { (void)defs; (void)n; }
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)name; (void)fn; return 0x1000; }
const char *hostfn_current_name(void) { return "stub"; }
void call_guest(Ctx *c, uint64_t addr) { (void)c; (void)addr; }
void guest_trap(const char *msg, uint64_t pc) { fprintf(stderr, "trap: %s (%llx)\n", msg, (unsigned long long)pc); abort(); }
void trace_printf(const char *fmt, ...) { (void)fmt; }

/* Minimal fake waveOut so tests never touch real audio hardware */
typedef struct {
    int paused;
    int closed;
    int reset_count;
    WAVEHDR *last_hdr;
    unsigned written;
} FakeWave;

static FakeWave g_fake_waves[16];
static int g_fake_wave_count;

MMRESULT waveOutOpen(HWAVEOUT *out, UINT dev, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    (void)dev; (void)f; (void)cb; (void)inst; (void)flags;
    assert(g_fake_wave_count < 16);
    FakeWave *w = &g_fake_waves[g_fake_wave_count++];
    memset(w, 0, sizeof *w);
    *out = w;
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n) { (void)h; (void)n; hdr->dwFlags &= ~WHDR_DONE; return MMSYSERR_NOERROR; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n) { (void)h; (void)hdr; (void)n; return MMSYSERR_NOERROR; }
MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT n) {
    (void)n;
    FakeWave *w = (FakeWave *)h;
    w->last_hdr = hdr;
    w->written++;
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPause(HWAVEOUT h) { ((FakeWave *)h)->paused = 1; return MMSYSERR_NOERROR; }
MMRESULT waveOutRestart(HWAVEOUT h) { ((FakeWave *)h)->paused = 0; return MMSYSERR_NOERROR; }
MMRESULT waveOutReset(HWAVEOUT h) { ((FakeWave *)h)->reset_count++; return MMSYSERR_NOERROR; }
MMRESULT waveOutSetVolume(HWAVEOUT h, DWORD v) { (void)h; (void)v; return MMSYSERR_NOERROR; }
MMRESULT waveOutGetPosition(HWAVEOUT h, MMTIME *t, UINT n) {
    (void)h; (void)n;
    t->wType = TIME_SAMPLES;
    t->u.sample = 0;
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutClose(HWAVEOUT h) { ((FakeWave *)h)->closed = 1; return MMSYSERR_NOERROR; }

#include "nnaudio.c"

static void setup_param(uint8_t param[8], uint32_t rate, uint16_t ch) {
    memset(param, 0, 8);
    memcpy(param, &rate, 4);
    memcpy(param + 4, &ch, 2);
}

static void setup_buf(uint8_t buf[0x28], int16_t *pcm, size_t bytes) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)buf;
    c.x[1] = (uint64_t)(uintptr_t)pcm;
    c.x[2] = bytes;
    c.x[3] = bytes;
    audio_SetAudioOutBufferInfo(&c);
}

int main(void) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* Two guest AudioOut objects (simulating Wwise + CRI during a movie) */
    uint8_t ao_wwise[0x40] = {0}, ev_wwise[0x40] = {0}, p_wwise[8];
    uint8_t ao_cri[0x40] = {0}, ev_cri[0x40] = {0}, p_cri[8];
    setup_param(p_wwise, 48000, 2);
    setup_param(p_cri, 32000, 2);

    Ctx c;
    /* Open Wwise */
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise;
    c.x[1] = (uint64_t)(uintptr_t)ev_wwise;
    c.x[2] = (uint64_t)(uintptr_t)p_wwise;
    audio_OpenDefaultAudioOut(&c);
    assert(c.x[0] == 0);

    /* Open CRI while Wwise is open */
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;
    c.x[1] = (uint64_t)(uintptr_t)ev_cri;
    c.x[2] = (uint64_t)(uintptr_t)p_cri;
    audio_OpenDefaultAudioOut(&c);
    assert(c.x[0] == 0);

    /* Sample rates stay independent */
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; audio_GetAudioOutSampleRate(&c); assert(c.x[0] == 48000);
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_GetAudioOutSampleRate(&c); assert(c.x[0] == 32000);

    /* State is per-AudioOut: start Wwise only */
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; audio_StartAudioOut(&c);
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; audio_GetAudioOutState(&c); assert(c.x[0] == 0); /* started */
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_GetAudioOutState(&c); assert(c.x[0] == 1); /* stopped */
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_StartAudioOut(&c);
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_GetAudioOutState(&c); assert(c.x[0] == 0); /* started */

    /* Append separate buffers to Wwise and CRI */
    int16_t pcm_w[1024] = {1000, -1000}, pcm_c[1024] = {2000, -2000};
    uint8_t buf_w[0x28], buf_c[0x28];
    setup_buf(buf_w, pcm_w, sizeof pcm_w);
    setup_buf(buf_c, pcm_c, sizeof pcm_c);

    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; c.x[1] = (uint64_t)(uintptr_t)buf_w; audio_AppendAudioOutBuffer(&c); assert(c.x[0] == 1);
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   c.x[1] = (uint64_t)(uintptr_t)buf_c; audio_AppendAudioOutBuffer(&c); assert(c.x[0] == 1);

    /* Mark only CRI's waveOut buffer done: Wwise must NOT receive CRI's buffer, CRI must receive its own */
    g_fake_waves[1].last_hdr->dwFlags |= WHDR_DONE;
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; audio_GetReleasedAudioOutBuffer(&c); assert(c.x[0] == 0);
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_GetReleasedAudioOutBuffer(&c); assert(c.x[0] == (uint64_t)(uintptr_t)buf_c);
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_GetReleasedAudioOutBuffer(&c); assert(c.x[0] == 0);

    /* Now mark Wwise's buffer done */
    g_fake_waves[0].last_hdr->dwFlags |= WHDR_DONE;
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; audio_GetReleasedAudioOutBuffer(&c); assert(c.x[0] == (uint64_t)(uintptr_t)buf_w);

    /* Stopping CRI leaves Wwise running */
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_StopAudioOut(&c);
    c.x[0] = (uint64_t)(uintptr_t)ao_cri;   audio_GetAudioOutState(&c); assert(c.x[0] == 1);
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; audio_GetAudioOutState(&c); assert(c.x[0] == 0);

    /* SDK 13 no-event opens get distinct per-device own_event addresses */
    uint8_t ao_s13a[0x40] = {0}, ao_s13b[0x40] = {0}, ao_extra[0x40] = {0};
    g_init_event_count = 0;
    c.x[0] = (uint64_t)(uintptr_t)ao_s13a; c.x[1] = (uint64_t)(uintptr_t)p_wwise; audio_OpenDefaultAudioOut13(&c); assert(c.x[0] == 0);
    c.x[0] = (uint64_t)(uintptr_t)ao_s13b; c.x[1] = (uint64_t)(uintptr_t)p_wwise; audio_OpenDefaultAudioOut13(&c); assert(c.x[0] == 0);
    assert(g_init_event_count == 2 && g_last_init_events[0] != 0 && g_last_init_events[1] != 0);
    assert(g_last_init_events[0] != g_last_init_events[1]);

    /* 5th simultaneous AudioOut is refused (AO_MAX == 4) */
    c.x[0] = (uint64_t)(uintptr_t)ao_extra; c.x[1] = (uint64_t)(uintptr_t)ev_cri; c.x[2] = (uint64_t)(uintptr_t)p_cri;
    audio_OpenDefaultAudioOut(&c);
    assert(c.x[0] != 0);

    /* Closing CRI frees its slot so a new open succeeds and closes old waveOut */
    c.x[0] = (uint64_t)(uintptr_t)ao_cri; audio_CloseAudioOut(&c);
    assert(g_fake_waves[1].closed == 1);
    c.x[0] = (uint64_t)(uintptr_t)ao_extra; c.x[1] = (uint64_t)(uintptr_t)ev_cri; c.x[2] = (uint64_t)(uintptr_t)p_cri;
    audio_OpenDefaultAudioOut(&c);
    assert(c.x[0] == 0);

    /* Clean up */
    c.x[0] = (uint64_t)(uintptr_t)ao_wwise; audio_CloseAudioOut(&c);
    c.x[0] = (uint64_t)(uintptr_t)ao_s13a;  audio_CloseAudioOut(&c);
    c.x[0] = (uint64_t)(uintptr_t)ao_s13b;  audio_CloseAudioOut(&c);
    c.x[0] = (uint64_t)(uintptr_t)ao_extra; audio_CloseAudioOut(&c);

    printf("nnaudio_test: PASS\n");
    return 0;
}
