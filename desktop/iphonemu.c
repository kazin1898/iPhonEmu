/*
 * iPhonEmu — desktop frontend.
 *
 * One window that takes an iPhone 3G iPhone OS 3.1.3 IPSW and boots it:
 *
 *   1. IMPORT     the IPSW (dropped on the window, picked with O, or passed on
 *                 the command line) becomes kernel.macho, devicetree.bin and
 *                 rootfs.img, each verified against its known-good hash.
 *   2. PROVISION  rootfs.img is copied once into the machine's writable disk.
 *   3. RUN        the emulator thread boots Apple's kernel and runs it; this
 *                 thread draws the panel and turns mouse and keys into touch
 *                 and button reports.
 *
 * Everything that knows about firmware, the board or the guest lives in vm/
 * and core/. This file is only the window, the threads and the input.
 *
 * Copyright (c) 2026 Caique V (kazin1898). MIT licensed.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "soc.h"
#include "VMAudioBuffer.h"
#include "VMBootOptions.h"
#include "VMButtonQueue.h"
#include "VMFirmwareBoot.h"
#include "VMFirmwareImport.h"
#include "VMFirmwareKeys.h"
#include "VMGuest.h"
#include "VMOptions.h"
#include "VMTouchMap.h"
#include "VMTouchQueue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define APP_NAME     "iPhonEmu"
#define PANEL_W      320
#define PANEL_H      480
#define RUN_CHUNK    100000u          /* instructions per emulator slice  */
#define PUBLISH_NS   (16u * 1000000u) /* framebuffer copy, ~60 Hz         */
#define TEXT_COLS    (PANEL_W / 8)
#if defined(_WIN32)
#define SEP "\\"
#else
#define SEP "/"
#endif

/* ------------------------------------------------------------------------ */
/* Shared state                                                              */
/* ------------------------------------------------------------------------ */

typedef enum {
    PHASE_WAITING = 0,   /* no firmware yet: asking for an IPSW            */
    PHASE_IMPORTING,
    PHASE_PROVISIONING,
    PHASE_BOOTING,
    PHASE_RUNNING,
    PHASE_FAILED
} phase_t;

typedef struct {
    SDL_Mutex *lock;

    /* Written by worker threads, read by the window. Under `lock`. */
    phase_t  phase;
    char     status[512];
    uint64_t progress_done, progress_total;

    /* The latest published frame. Under `lock`. */
    uint8_t  frame[PANEL_W * PANEL_H * 4];
    uint32_t frame_w, frame_h;
    vm_pixel_order_t frame_order;
    uint64_t frame_seq;
    bool     display_on;

    /* Input from the window to the emulator thread. Under `lock`. */
    vm_touch_queue_t  touch;
    vm_button_queue_t buttons;

    SDL_AtomicInt quit;
    SDL_AtomicInt insn_millions;

    vm_firmware_boot_paths_t paths;
    char ipsw[1024];
    char data_dir[1024];

    vm_audio_buffer_t audio;
} app_t;

static void set_phase(app_t *app, phase_t phase, const char *fmt, ...) {
    va_list ap;
    SDL_LockMutex(app->lock);
    app->phase = phase;
    app->progress_done = app->progress_total = 0;
    va_start(ap, fmt);
    (void)vsnprintf(app->status, sizeof app->status, fmt, ap);
    va_end(ap);
    SDL_UnlockMutex(app->lock);
    SDL_Log("[%d] %s", (int)phase, app->status);
}

static void set_progress(app_t *app, uint64_t done, uint64_t total) {
    SDL_LockMutex(app->lock);
    app->progress_done = done;
    app->progress_total = total;
    SDL_UnlockMutex(app->lock);
}

/* ------------------------------------------------------------------------ */
/* Boot options                                                              */
/* ------------------------------------------------------------------------ */

/* The settings a new machine gets in S5LBox's app: MBX graphics on, the CPU
 * renderer override off, networking on. */
static unsigned boot_options(bool values[VM_BOOT_OPTION_MAX]) {
    unsigned count = vm_option_count();
    if (count > VM_BOOT_OPTION_MAX) count = VM_BOOT_OPTION_MAX;
    for (unsigned i = 0; i < count; i++)
        values[i] = vm_option_new_machine_value(i, false, false);
    return count;
}

/* ------------------------------------------------------------------------ */
/* IPSW import                                                               */
/* ------------------------------------------------------------------------ */

static size_t ipsw_pread(void *ctx, uint64_t off, uint8_t *buf, size_t len) {
    SDL_IOStream *io = ctx;
    if (SDL_SeekIO(io, (Sint64)off, SDL_IO_SEEK_SET) < 0) return 0;
    return SDL_ReadIO(io, buf, len);
}

typedef struct { app_t *app; } sink_t;

typedef struct {
    SDL_IOStream *io;
    char path[1200];
} sink_file_t;

static void *sink_open(void *ctx, const char *name) {
    sink_t *s = ctx;
    if (!name || !*name || name[0] == '.' || strpbrk(name, "/\\:")) return NULL;
    sink_file_t *f = SDL_calloc(1, sizeof *f);
    if (!f) return NULL;
    (void)snprintf(f->path, sizeof f->path, "%s%s",
                   s->app->paths.firmware, name);
    /* The encrypted DMG scratch file is read back, so open for both. */
    f->io = SDL_IOFromFile(f->path, "w+b");
    if (!f->io) { SDL_free(f); return NULL; }
    return f;
}

static bool sink_write(void *ctx, void *h, const uint8_t *d, size_t n) {
    (void)ctx;
    sink_file_t *f = h;
    return f && SDL_WriteIO(f->io, d, n) == n;
}

static size_t sink_pread(void *ctx, void *h, uint64_t off, uint8_t *b, size_t n) {
    (void)ctx;
    sink_file_t *f = h;
    if (!f || !SDL_FlushIO(f->io)) return 0;
    if (SDL_SeekIO(f->io, (Sint64)off, SDL_IO_SEEK_SET) < 0) return 0;
    size_t got = SDL_ReadIO(f->io, b, n);
    (void)SDL_SeekIO(f->io, 0, SDL_IO_SEEK_END);
    return got;
}

static void sink_close(void *ctx, void *h, bool keep) {
    (void)ctx;
    sink_file_t *f = h;
    if (!f) return;
    SDL_CloseIO(f->io);
    /* Intermediates and half-written outputs are deleted: a partial
     * rootfs.img left behind would look like a finished import. */
    if (!keep) (void)SDL_RemovePath(f->path);
    SDL_free(f);
}

static void import_progress(void *ctx, vm_fw_artefact_t which,
                            vm_fw_stage_t stage, uint64_t done, uint64_t total) {
    app_t *app = ctx;
    SDL_LockMutex(app->lock);
    (void)snprintf(app->status, sizeof app->status, "Importing %s\n%s",
                   vm_fw_artefact_title(which), vm_fw_stage_name(stage));
    app->progress_done = done;
    app->progress_total = total;
    SDL_UnlockMutex(app->lock);
}

static bool import_cancel(void *ctx) {
    app_t *app = ctx;
    return SDL_GetAtomicInt(&app->quit) != 0;
}

static bool run_import(app_t *app) {
    set_phase(app, PHASE_IMPORTING, "Opening the IPSW");
    SDL_IOStream *io = SDL_IOFromFile(app->ipsw, "rb");
    if (!io) {
        set_phase(app, PHASE_FAILED, "Could not open the IPSW:\n%s",
                  SDL_GetError());
        return false;
    }
    Sint64 size = SDL_GetIOSize(io);

    sink_t sink = { app };
    vm_fw_files_t files = {
        .open = sink_open, .write = sink_write,
        .pread = sink_pread, .close = sink_close, .ctx = &sink,
    };
    vm_fw_import_t in;
    SDL_memset(&in, 0, sizeof in);
    in.pread = ipsw_pread;
    in.pread_ctx = io;
    in.size = size > 0 ? (uint64_t)size : 0;
    in.files = &files;
    in.progress = import_progress;
    in.progress_ctx = app;
    in.cancel = import_cancel;
    in.cancel_ctx = app;
    in.resolve_keys = vm_fw_resolve_public_keys;
    in.require_supported = true;

    vm_fw_report_t *rep = SDL_calloc(1, sizeof *rep);
    if (!rep) { SDL_CloseIO(io); return false; }
    vm_fw_status_t st = vm_fw_import_run(&in, rep);
    SDL_CloseIO(io);

    char text[4096];
    (void)vm_fw_report_render(rep, text, sizeof text);
    SDL_Log("import report:\n%s", text);

    bool ok = st == VM_FW_OK && vm_fw_report_is_verified(rep);
    if (!ok) {
        const char *why = rep->detail[0] ? rep->detail : vm_fw_strerror(st);
        if (st == VM_FW_ERR_UNSUPPORTED_BUILD ||
            st == VM_FW_ERR_UNSUPPORTED_DEVICE)
            why = vm_fw_strerror(st);
        set_phase(app, PHASE_FAILED, "This IPSW cannot be used.\n%s", why);
    }
    SDL_free(rep);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Provisioning the writable disk                                            */
/* ------------------------------------------------------------------------ */

static void provision_progress(void *ctx, uint64_t done, uint64_t total) {
    set_progress(ctx, done, total);
}

static bool run_provision(app_t *app) {
    set_phase(app, PHASE_PROVISIONING, "Preparing the writable disk\n"
              "(about 450 MB, only once)");
    bool values[VM_BOOT_OPTION_MAX];
    unsigned count = boot_options(values);
    char detail[VM_FW_BOOT_DETAIL_CAPACITY] = "";
    if (vm_firmware_boot_provision(&app->paths, values, count,
                                   provision_progress, app,
                                   detail, sizeof detail))
        return true;
    vm_firmware_boot_state_t after;
    vm_firmware_boot_probe(&app->paths, &after);
    if (after.work_present) return true;   /* made by an earlier run */
    set_phase(app, PHASE_FAILED, "Could not prepare the disk.\n%s", detail);
    return false;
}

/* ------------------------------------------------------------------------ */
/* The emulator thread                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    vm_button_power_hold_t      power;
    vm_button_momentary_holds_t momentary;
} button_holds_t;

/* One queued touch report into the Z2 controller per slice; it holds a single
 * report until the guest has clocked it out. Same policy as S5LBox's app. */
static void drain_touch(app_t *app, s5l8900_t *m) {
    s5l_mt_contact_t c;
    SDL_LockMutex(app->lock);
    if (!vm_touch_queue_peek(&app->touch, &c)) {
        SDL_UnlockMutex(app->lock);
        return;
    }
    if (s5l8900_set_contacts(m, &c, 1u)) {
        vm_touch_queue_pop(&app->touch);
        SDL_UnlockMutex(app->lock);
        s5l8900_tick(m, 0);
        return;
    }
    if (!s5l_mtz2_irq(&m->mtz2))     /* not backpressure: the part is not  */
        vm_touch_queue_pop(&app->touch);   /* able to report; do not wedge */
    SDL_UnlockMutex(app->lock);
}

/* One button transition per slice, held long enough for the guest's debounce
 * to see it. A refusal keeps it queued: the guest arms its button interrupts a
 * few hundred million instructions into the boot. */
static void drain_button(app_t *app, s5l8900_t *m, button_holds_t *holds) {
    vm_button_event_t e;
    SDL_LockMutex(app->lock);
    bool have = vm_button_queue_peek(&app->buttons, &e);
    SDL_UnlockMutex(app->lock);
    if (!have) return;

    uint64_t now = SDL_GetTicksNS();
    bool display = s5l_clcd_running(&m->clcd);
    if (!vm_button_momentary_release_ready(&e, &holds->momentary, now, display))
        return;
    if (!vm_button_power_release_ready(&e, &holds->power, now, m->cpu.cycles,
                                       display))
        return;

    bool waking_home = e.which == S5L_BUTTON_MENU && e.pressed &&
        s5l_pcf50635_in_hibernation(&m->pmu) &&
        !s5l_pcf50635_in_standby(&m->pmu);
    if (!s5l8900_set_button(m, e.which, e.pressed)) return;

    SDL_LockMutex(app->lock);
    vm_button_queue_pop(&app->buttons);
    SDL_UnlockMutex(app->lock);
    vm_button_momentary_note_accepted(&e, &holds->momentary, now, waking_home);
    if (e.which == S5L_BUTTON_HOLD) {
        if (e.pressed) {
            holds->power.active = true;
            holds->power.display_running_at_press = display;
            holds->power.delivered_ns = now;
            holds->power.delivered_cycles = m->cpu.cycles;
        } else {
            SDL_memset(&holds->power, 0, sizeof holds->power);
        }
    }
}

static void publish_frame(app_t *app, const s5l8900_t *m) {
    uint32_t w = 0, h = 0, stride = 0;
    vm_pixel_order_t order = VM_ORDER_BGRA;
    const uint8_t *px = vm_guest_display(m, &w, &h, &stride, &order);
    bool on = px && s5l_clcd_running(&m->clcd);

    SDL_LockMutex(app->lock);
    app->display_on = on;
    if (on && w <= PANEL_W && h <= PANEL_H) {
        for (uint32_t y = 0; y < h; y++)
            SDL_memcpy(app->frame + (size_t)y * PANEL_W * 4,
                       px + (size_t)y * stride, (size_t)w * 4);
        app->frame_w = w;
        app->frame_h = h;
        app->frame_order = order;
        app->frame_seq++;
    }
    SDL_UnlockMutex(app->lock);
}

static int SDLCALL emulator_thread(void *arg) {
    app_t *app = arg;

    vm_firmware_boot_state_t state;
    vm_firmware_boot_probe(&app->paths, &state);
    if (state.readiness == VM_FW_BOOT_INCOMPLETE) {
        if (!run_import(app)) return 1;
        vm_firmware_boot_probe(&app->paths, &state);
    }
    if (state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE) {
        if (!run_provision(app)) return 1;
        vm_firmware_boot_probe(&app->paths, &state);
    }
    if (state.readiness != VM_FW_BOOT_READY) {
        set_phase(app, PHASE_FAILED, "The firmware is incomplete.\n%s",
                  state.detail);
        return 1;
    }

    set_phase(app, PHASE_BOOTING, "Starting iPhone OS");
    s5l8900_t *m = SDL_calloc(1, sizeof *m);
    vm_firmware_boot_t *boot = vm_firmware_boot_create();
    if (!m || !boot || !s5l8900_init(m, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE)) {
        set_phase(app, PHASE_FAILED, "Not enough memory for the machine.");
        vm_firmware_boot_destroy(&boot);
        SDL_free(m);
        return 1;
    }

    bool values[VM_BOOT_OPTION_MAX];
    unsigned count = boot_options(values);
    vm_firmware_boot_report_t report;
    if (!vm_firmware_boot_start(boot, m, &app->paths, values, count, &report)) {
        set_phase(app, PHASE_FAILED, "iPhone OS could not start.\n%s",
                  report.detail);
        vm_firmware_boot_destroy(&boot);
        s5l8900_free(m);
        SDL_free(m);
        return 1;
    }
    SDL_Log("boot: %s; cmdline \"%s\"", report.summary, report.bringup.cmdline);
    if (report.options.summary[0])
        SDL_Log("settings: %s", report.options.summary);

    m->i2s[0].frame = vm_audio_guest_frame;
    m->i2s[0].frame_ctx = &app->audio;
    vm_audio_buffer_active(&app->audio, true);

    set_phase(app, PHASE_RUNNING, "%s", report.summary);
    button_holds_t holds;
    SDL_memset(&holds, 0, sizeof holds);
    uint64_t retired = 0, next_publish = 0;
    arm_status_t status = ARM_OK;

    while (!SDL_GetAtomicInt(&app->quit)) {
        drain_touch(app, m);
        drain_button(app, m, &holds);

        retired += s5l8900_run(m, RUN_CHUNK, &status);

        if (m->i2s[0].frame != vm_audio_guest_frame) {
            /* A watchdog reboot replaced the board's callbacks. */
            m->i2s[0].frame = vm_audio_guest_frame;
            m->i2s[0].frame_ctx = &app->audio;
        }

        uint64_t now = SDL_GetTicksNS();
        if (now >= next_publish || status != ARM_OK) {
            publish_frame(app, m);
            SDL_SetAtomicInt(&app->insn_millions, (int)(retired / 1000000u));
            next_publish = now + PUBLISH_NS;
        }
        if (status != ARM_OK && status != ARM_RESTART) {
            set_phase(app, PHASE_FAILED,
                      "The guest stopped (%s) at pc 0x%08x\nafter %.1f M "
                      "instructions.",
                      status == ARM_UNDEFINED ? "unsupported instruction" :
                      status == ARM_HALT ? "halt" : "error",
                      (unsigned)m->cpu.r[15], retired / 1.0e6);
            break;
        }
    }

    vm_audio_buffer_active(&app->audio, false);
    m->i2s[0].frame = NULL;
    m->i2s[0].frame_ctx = NULL;
    vm_firmware_boot_destroy(&boot);
    s5l8900_free(m);
    SDL_free(m);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Audio                                                                     */
/* ------------------------------------------------------------------------ */

static void SDLCALL audio_pull(void *ctx, SDL_AudioStream *stream,
                               int additional, int total) {
    (void)total;
    app_t *app = ctx;
    float left[512], right[512], mixed[1024];
    int frames = additional / (int)(2 * sizeof(float));
    while (frames > 0) {
        unsigned n = frames > 512 ? 512u : (unsigned)frames;
        vm_audio_render(&app->audio, left, right, n);
        for (unsigned i = 0; i < n; i++) {
            mixed[2 * i] = left[i];
            mixed[2 * i + 1] = right[i];
        }
        SDL_PutAudioStreamData(stream, mixed, (int)(n * 2 * sizeof(float)));
        frames -= (int)n;
    }
}

/* ------------------------------------------------------------------------ */
/* The window                                                                */
/* ------------------------------------------------------------------------ */

static SDL_Thread *start_machine(app_t *app) {
    SDL_SetAtomicInt(&app->quit, 0);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           (void *)emulator_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "emulator");
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, app);
    /* The boot path keeps large reports on the stack. */
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER,
                          8 * 1024 * 1024);
    SDL_Thread *t = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    return t;
}

/* The file dialog answers on whatever thread it likes; the choice is handed
 * to the window thread, which owns starting the machine. */
static Uint32 picked_event;

static void SDLCALL picked_ipsw(void *ctx, const char *const *files, int filter) {
    (void)ctx; (void)filter;
    if (!files || !files[0]) return;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = picked_event;
    ev.user.data1 = SDL_strdup(files[0]);
    if (!SDL_PushEvent(&ev)) SDL_free(ev.user.data1);
}

static void open_ipsw(app_t *app, SDL_Thread **machine, const char *path);

static void queue_button(app_t *app, unsigned app_button, bool pressed) {
    unsigned which;
    bool guest_pressed;
    if (!vm_button_to_guest(app_button, pressed, &which, &guest_pressed)) return;
    SDL_LockMutex(app->lock);
    if (app->phase == PHASE_RUNNING)
        (void)vm_button_queue_push(&app->buttons, which, guest_pressed);
    SDL_UnlockMutex(app->lock);
}

static void queue_touch(app_t *app, vm_touch_tracker_t *tracker,
                        vm_touch_phase_t phase, float x, float y) {
    vm_touch_point_t p = vm_touch_map(PANEL_W, PANEL_H, S5L_MT_PANEL_W,
                                      S5L_MT_PANEL_H, x, y);
    int gx, gy;
    if (!vm_touch_track(tracker, phase, p, &gx, &gy)) return;
    s5l_mt_contact_t c;
    if (!vm_touch_contact_from_ui(phase, gx, gy, &c)) return;
    SDL_LockMutex(app->lock);
    if (app->phase == PHASE_RUNNING)
        (void)vm_touch_queue_push(&app->touch, &c);
    SDL_UnlockMutex(app->lock);
}

static int key_button(SDL_Keycode key) {
    switch (key) {
        case SDLK_H: case SDLK_HOME: case SDLK_BACKSPACE:
            return (int)VM_BUTTON_HOME;
        case SDLK_P: case SDLK_END:
            return (int)VM_BUTTON_POWER;
        case SDLK_PAGEUP: case SDLK_EQUALS: case SDLK_KP_PLUS:
            return (int)VM_BUTTON_VOLUME_UP;
        case SDLK_PAGEDOWN: case SDLK_MINUS: case SDLK_KP_MINUS:
            return (int)VM_BUTTON_VOLUME_DOWN;
        default:
            return -1;
    }
}

/* Debug text is 8x8; wrap at the panel width and centre each line. */
static void draw_text_block(SDL_Renderer *r, const char *text, float top) {
    char line[TEXT_COLS + 1];
    float y = top;
    const char *p = text;
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n < TEXT_COLS) n++;
        if (p[n] && p[n] != '\n' && n == TEXT_COLS) {
            size_t cut = n;
            while (cut > 0 && p[cut] != ' ') cut--;
            if (cut > 0) n = cut;
        }
        SDL_memcpy(line, p, n);
        line[n] = '\0';
        float x = (PANEL_W - (float)n * 8.0f) / 2.0f;
        SDL_RenderDebugText(r, x, y, line);
        y += 12.0f;
        p += n;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
}

static void draw_overlay(SDL_Renderer *r, app_t *app) {
    char status[512];
    phase_t phase;
    uint64_t done, total;
    SDL_LockMutex(app->lock);
    phase = app->phase;
    SDL_strlcpy(status, app->status, sizeof status);
    done = app->progress_done;
    total = app->progress_total;
    SDL_UnlockMutex(app->lock);

    SDL_SetRenderDrawColor(r, 235, 235, 235, 255);
    SDL_RenderDebugText(r, (PANEL_W - 8.0f * 8) / 2.0f, 150, APP_NAME);

    if (phase == PHASE_RUNNING) {
        char line[96];
        (void)snprintf(line, sizeof line, "Starting iPhone OS\n%d M instructions",
                       SDL_GetAtomicInt(&app->insn_millions));
        draw_text_block(r, line, 200);
    } else if (phase == PHASE_WAITING) {
        draw_text_block(r,
            "Drop an iPhone 3G IPSW here\n"
            "(iPhone1,2 3.1.3, 7E18)\n\n"
            "or press O to choose one", 200);
    } else {
        if (phase == PHASE_FAILED) SDL_SetRenderDrawColor(r, 255, 120, 110, 255);
        draw_text_block(r, status, 200);
        if (phase == PHASE_FAILED) {
            SDL_SetRenderDrawColor(r, 200, 200, 200, 255);
            draw_text_block(r, "Press O to choose another IPSW", 400);
        }
    }

    if (total > 0) {
        SDL_FRect bar = { 40, 300, PANEL_W - 80, 8 };
        SDL_SetRenderDrawColor(r, 70, 70, 70, 255);
        SDL_RenderFillRect(r, &bar);
        bar.w *= (float)((double)done / (double)total);
        SDL_SetRenderDrawColor(r, 235, 235, 235, 255);
        SDL_RenderFillRect(r, &bar);
    }
}

static void redirect_log(const char *data_dir) {
    char path[1200];
    (void)snprintf(path, sizeof path, "%siphonemu.log", data_dir);
    /* The core reports through stdio; a GUI program has nowhere to show it,
     * so it goes to a log next to the machine. */
    if (freopen(path, "w", stdout)) {
        (void)setvbuf(stdout, NULL, _IOLBF, 0);
#if defined(_WIN32)
        (void)_dup2(_fileno(stdout), _fileno(stderr));
#else
        (void)dup2(fileno(stdout), fileno(stderr));
#endif
    }
}

static void SDLCALL log_to_stdout(void *ctx, int category,
                                  SDL_LogPriority priority, const char *msg) {
    (void)ctx; (void)category; (void)priority;
    fprintf(stdout, "%s\n", msg);
}

/* F12, or every IPHONEMU_AUTOSHOT seconds: the guest panel as a BMP in the
 * data folder's shots/, named by how far the guest had run. */
static void save_screenshot(app_t *app) {
    SDL_Surface *s = SDL_CreateSurface(PANEL_W, PANEL_H,
                                       SDL_PIXELFORMAT_XRGB8888);
    if (!s) return;
    SDL_LockMutex(app->lock);
    bool argb = app->frame_order == VM_ORDER_ARGB;
    for (int y = 0; y < PANEL_H; y++)
        SDL_memcpy((uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch,
                   app->frame + (size_t)y * PANEL_W * 4, PANEL_W * 4);
    SDL_UnlockMutex(app->lock);
    if (argb) {
        for (int y = 0; y < PANEL_H; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)s->pixels +
                                         (size_t)y * (size_t)s->pitch);
            for (int x = 0; x < PANEL_W; x++) row[x] = SDL_Swap32(row[x]);
        }
    }
    char path[1200];
    (void)snprintf(path, sizeof path, "%sshots/", app->data_dir);
    (void)SDL_CreateDirectory(path);
    (void)snprintf(path, sizeof path, "%sshots/shot-%05dM.bmp", app->data_dir,
                   SDL_GetAtomicInt(&app->insn_millions));
    if (SDL_SaveBMP(s, path)) SDL_Log("screenshot %s", path);
    SDL_DestroySurface(s);
}

/* Portable by default: a data folder beside the executable. The per-user
 * folder is the fallback for an install that cannot be written to. Beside the
 * executable also avoids folders Windows virtualizes for packaged parents,
 * which the disk preparation rightly refuses: the file it opens there is not
 * at the path it asked for. */
static bool choose_data_dir(app_t *app) {
    const char *base = SDL_GetBasePath();
    if (base) {
        char dir[1024], probe[1100];
        (void)snprintf(dir, sizeof dir, "%sdata" SEP, base);
        (void)snprintf(probe, sizeof probe, "%s.write-test", dir);
        if (SDL_CreateDirectory(dir)) {
            SDL_IOStream *io = SDL_IOFromFile(probe, "wb");
            if (io) {
                SDL_CloseIO(io);
                (void)SDL_RemovePath(probe);
                SDL_strlcpy(app->data_dir, dir, sizeof app->data_dir);
                return true;
            }
        }
    }
    char *pref = SDL_GetPrefPath(NULL, APP_NAME);
    if (!pref) return false;
    SDL_strlcpy(app->data_dir, pref, sizeof app->data_dir);
    SDL_free(pref);
    return true;
}

static bool setup_paths(app_t *app) {
    if (!choose_data_dir(app)) return false;

    char firmware[1024], work[1024];
    (void)snprintf(firmware, sizeof firmware, "%sfirmware" SEP "iPhone1,2_7E18" SEP,
                   app->data_dir);
    (void)snprintf(work, sizeof work, "%smachine" SEP, app->data_dir);
    if (!SDL_CreateDirectory(firmware) || !SDL_CreateDirectory(work))
        return false;
    return vm_firmware_boot_paths_split(&app->paths, firmware, work);
}

static bool is_ipsw(const char *path) {
    size_t n = path ? SDL_strlen(path) : 0;
    return n > 5 && SDL_strcasecmp(path + n - 5, ".ipsw") == 0;
}

/* A new IPSW is taken only while no machine is running. */
static void open_ipsw(app_t *app, SDL_Thread **machine, const char *path) {
    SDL_LockMutex(app->lock);
    phase_t phase = app->phase;
    SDL_UnlockMutex(app->lock);
    if ((phase != PHASE_WAITING && phase != PHASE_FAILED) || !is_ipsw(path))
        return;
    if (*machine) { SDL_WaitThread(*machine, NULL); *machine = NULL; }
    SDL_strlcpy(app->ipsw, path, sizeof app->ipsw);
    *machine = start_machine(app);
}

int main(int argc, char **argv) {
    static app_t app;
    SDL_SetAppMetadata(APP_NAME, "0.1", "io.github.kazin1898.iphonemu");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    app.lock = SDL_CreateMutex();
    vm_touch_queue_reset(&app.touch);
    vm_button_queue_reset(&app.buttons);
    if (!app.lock || !vm_audio_buffer_init(&app.audio) || !setup_paths(&app)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME,
                                 "Could not create the data folder.", NULL);
        return 1;
    }
    redirect_log(app.data_dir);
    SDL_SetLogOutputFunction(log_to_stdout, NULL);
    SDL_Log("%s data in %s", APP_NAME, app.data_dir);

    for (int i = 1; i < argc; i++)
        if (is_ipsw(argv[i]))
            SDL_strlcpy(app.ipsw, argv[i], sizeof app.ipsw);

    /* Size the window to the largest whole-number scale that fits. */
    int scale = 1;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
        while ((scale + 1) * PANEL_H + 60 <= usable.h &&
               (scale + 1) * PANEL_W <= usable.w)
            scale++;
    }
    SDL_Window *win;
    SDL_Renderer *ren;
    if (!SDL_CreateWindowAndRenderer(APP_NAME, PANEL_W * scale, PANEL_H * scale,
                                     SDL_WINDOW_RESIZABLE, &win, &ren)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME,
                                 SDL_GetError(), NULL);
        return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, PANEL_W, PANEL_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(ren, 1);
    SDL_Texture *tex_bgra = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
    SDL_Texture *tex_argb = SDL_CreateTexture(ren, SDL_PIXELFORMAT_BGRX8888,
        SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
    SDL_SetTextureScaleMode(tex_bgra, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(tex_argb, SDL_SCALEMODE_LINEAR);

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, VM_AUDIO_RATE };
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_pull, &app);
    if (audio) {
        SDL_ResumeAudioStreamDevice(audio);
        atomic_store(&app.audio.host_running, true);
    } else {
        SDL_Log("no audio output: %s", SDL_GetError());
    }

    picked_event = SDL_RegisterEvents(1);
    SDL_Thread *machine = NULL;
    vm_firmware_boot_state_t probe;
    vm_firmware_boot_probe(&app.paths, &probe);
    if (probe.readiness != VM_FW_BOOT_INCOMPLETE || app.ipsw[0])
        machine = start_machine(&app);
    else
        set_phase(&app, PHASE_WAITING, "%s", "waiting for an IPSW");

    static const SDL_DialogFileFilter filters[] = {
        { "iPhone firmware (*.ipsw)", "ipsw" },
    };
    vm_touch_tracker_t tracker;
    vm_touch_tracker_reset(&tracker);
    bool mouse_down = false;
    uint64_t shown_seq = 0;
    bool have_frame = false;
    char title[128];
    uint64_t next_title = 0;
    int last_millions = -1;
    const char *autoshot_env = SDL_getenv("IPHONEMU_AUTOSHOT");
    uint64_t autoshot_ns = autoshot_env
        ? (uint64_t)SDL_strtoul(autoshot_env, NULL, 10) * 1000000000u : 0;
    uint64_t next_autoshot = SDL_GetTicksNS() + autoshot_ns;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_DROP_FILE:
                open_ipsw(&app, &machine, ev.drop.data);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = ev.type == SDL_EVENT_KEY_DOWN;
                if (down && ev.key.key == SDLK_O) {
                    SDL_LockMutex(app.lock);
                    phase_t phase = app.phase;
                    SDL_UnlockMutex(app.lock);
                    if (phase == PHASE_WAITING || phase == PHASE_FAILED)
                        SDL_ShowOpenFileDialog(picked_ipsw, &app, win, filters,
                                               1, NULL, false);
                    break;
                }
                if (down && ev.key.key == SDLK_F12) {
                    save_screenshot(&app);
                    break;
                }
                if (down && ev.key.key == SDLK_F11) {
                    bool full = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(win, !full);
                    break;
                }
                int b = key_button(ev.key.key);
                if (b >= 0 && !ev.key.repeat)
                    queue_button(&app, (unsigned)b, down);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION: {
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                } else if (!mouse_down) {
                    break;
                }
                SDL_ConvertEventToRenderCoordinates(ren, &ev);
                float x = ev.type == SDL_EVENT_MOUSE_MOTION ? ev.motion.x
                                                            : ev.button.x;
                float y = ev.type == SDL_EVENT_MOUSE_MOTION ? ev.motion.y
                                                            : ev.button.y;
                vm_touch_phase_t ph =
                    ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? VM_TOUCH_BEGAN :
                    ev.type == SDL_EVENT_MOUSE_BUTTON_UP   ? VM_TOUCH_ENDED :
                                                             VM_TOUCH_MOVED;
                mouse_down = ph != VM_TOUCH_ENDED;
                queue_touch(&app, &tracker, ph, x, y);
                break;
            }
            default:
                if (ev.type == picked_event) {
                    open_ipsw(&app, &machine, ev.user.data1);
                    SDL_free(ev.user.data1);
                }
                break;
            }
        }

        SDL_LockMutex(app.lock);
        bool display_on = app.display_on;
        phase_t phase = app.phase;
        vm_pixel_order_t order = app.frame_order;
        if (app.frame_seq != shown_seq) {
            SDL_Texture *t = order == VM_ORDER_ARGB ? tex_argb : tex_bgra;
            SDL_UpdateTexture(t, NULL, app.frame, PANEL_W * 4);
            shown_seq = app.frame_seq;
            have_frame = true;
        }
        SDL_UnlockMutex(app.lock);

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        bool show_screen = have_frame && display_on &&
            (phase == PHASE_RUNNING || phase == PHASE_FAILED);
        if (show_screen)
            SDL_RenderTexture(ren, order == VM_ORDER_ARGB ? tex_argb : tex_bgra,
                              NULL, NULL);
        if (!show_screen) draw_overlay(ren, &app);
        SDL_RenderPresent(ren);

        uint64_t now = SDL_GetTicksNS();
        if (autoshot_ns && now >= next_autoshot) {
            if (have_frame) save_screenshot(&app);
            next_autoshot = now + autoshot_ns;
        }
        int millions = SDL_GetAtomicInt(&app.insn_millions);
        if (now >= next_title && millions != last_millions) {
            (void)snprintf(title, sizeof title, "%s - %d M instructions",
                           APP_NAME, millions);
            SDL_SetWindowTitle(win, title);
            last_millions = millions;
            next_title = now + 1000000000u;
        }
    }

    SDL_SetAtomicInt(&app.quit, 1);
    if (machine) SDL_WaitThread(machine, NULL);
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_DestroyTexture(tex_bgra);
    SDL_DestroyTexture(tex_argb);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_DestroyMutex(app.lock);
    SDL_Quit();
    return 0;
}
