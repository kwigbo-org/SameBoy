// The tester requires low-level access to the GB struct to detect failures
#define GB_INTERNAL

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <signal.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define snprintf _snprintf
#else
#include <sys/wait.h>
#endif

#include <ctype.h>
#include <Core/gb.h>
#include <Core/random.h>
#include <Core/symbol_hash.h>

static bool running = false;
static char *filename;
static char *bmp_filename;
static char *log_filename;
static char *sav_filename;
static FILE *log_file;
static void replace_extension(const char *src, size_t length, char *dest, const char *ext);
static bool push_start_a, start_is_not_first, a_is_bad, b_is_confirm, push_faster, push_slower,
            do_not_stop, push_a_twice, start_is_bad, allow_weird_sp_values, large_stack, push_right,
            semi_random, limit_start, pointer_control, unsafe_speed_switch;
static unsigned int test_length = 60 * 40;
GB_gameboy_t gb;

static unsigned int frames = 0;
static bool use_tga = false;

/* kwigbo-org fork extensions: state-trace harness ----------------------- */
#define WATCH_MAX 32
#define WATCH_LABEL_MAX 64

typedef struct {
    char     label[WATCH_LABEL_MAX];
    uint16_t addr;
} watch_t;

typedef struct {
    unsigned frame;
    uint8_t  buttons;  /* bitmask: bit k set ⇒ event touches GB_KEY_k */
    bool     press;
} script_event_t;

static const char    *sym_filename;
static const char    *watch_arg;
static const char    *script_filename;
static const char    *trace_filename;
static FILE          *trace_file;
static watch_t        watches[WATCH_MAX];
static unsigned       n_watches;
static script_event_t *script_events;
static unsigned       n_script_events;
static unsigned       next_script_event;

static const char *button_names[GB_KEY_MAX] = {
    [GB_KEY_RIGHT]  = "RIGHT",
    [GB_KEY_LEFT]   = "LEFT",
    [GB_KEY_UP]     = "UP",
    [GB_KEY_DOWN]   = "DOWN",
    [GB_KEY_A]      = "A",
    [GB_KEY_B]      = "B",
    [GB_KEY_SELECT] = "SELECT",
    [GB_KEY_START]  = "START",
};

static int parse_button_token(const char *s, size_t len)
{
    for (int k = 0; k < GB_KEY_MAX; k++) {
        if (strlen(button_names[k]) == len && strncasecmp(s, button_names[k], len) == 0) {
            return k;
        }
    }
    return -1;
}

static bool parse_button_mask(const char *s, uint8_t *out)
{
    uint8_t mask = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        const char *plus = strchr(s, '+');
        size_t len = plus ? (size_t)(plus - s) : strlen(s);
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
        if (len == 0) return false;
        int key = parse_button_token(s, len);
        if (key < 0) return false;
        mask |= (uint8_t)(1u << key);
        if (!plus) break;
        s = plus + 1;
    }
    *out = mask;
    return true;
}

static bool load_script_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open script file '%s'\n", path);
        return false;
    }
    char line[256];
    unsigned cap = 64;
    script_events = malloc(cap * sizeof(*script_events));
    if (!script_events) { fclose(f); return false; }
    n_script_events = 0;
    unsigned lineno = 0;
    unsigned last_frame = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        unsigned frame;
        char action[16];
        char buttons[64];
        int matched = sscanf(p, "%u %15s %63s", &frame, action, buttons);
        if (matched != 3) {
            fprintf(stderr, "script '%s' line %u: expected '<frame> press|release <buttons>'\n", path, lineno);
            fclose(f); return false;
        }
        if (frame < last_frame) {
            fprintf(stderr, "script '%s' line %u: frame %u out of order (previous was %u)\n",
                    path, lineno, frame, last_frame);
            fclose(f); return false;
        }
        last_frame = frame;

        bool press;
        if (strcasecmp(action, "press") == 0) press = true;
        else if (strcasecmp(action, "release") == 0) press = false;
        else {
            fprintf(stderr, "script '%s' line %u: unknown action '%s' (expected press/release)\n",
                    path, lineno, action);
            fclose(f); return false;
        }

        uint8_t mask;
        if (!parse_button_mask(buttons, &mask)) {
            fprintf(stderr, "script '%s' line %u: bad button list '%s'\n", path, lineno, buttons);
            fclose(f); return false;
        }

        if (n_script_events >= cap) {
            cap *= 2;
            script_event_t *grown = realloc(script_events, cap * sizeof(*script_events));
            if (!grown) { fclose(f); return false; }
            script_events = grown;
        }
        script_events[n_script_events++] = (script_event_t){frame, mask, press};
    }
    fclose(f);
    return true;
}

/* Resolve a single token to a 16-bit address. Hex like "0xC100" or a symbol
   name resolved via the debugger's reversed symbol map. Returns true on success. */
static bool resolve_watch_token(const char *tok, watch_t *out)
{
    size_t len = strlen(tok);
    if (len == 0 || len >= WATCH_LABEL_MAX) return false;

    memcpy(out->label, tok, len + 1);

    if (len > 2 && (tok[0] == '0') && (tok[1] == 'x' || tok[1] == 'X')) {
        char *end;
        unsigned long v = strtoul(tok + 2, &end, 16);
        if (*end != '\0' || v > 0xFFFF) return false;
        out->addr = (uint16_t)v;
        return true;
    }

#ifndef GB_DISABLE_DEBUGGER
    const GB_symbol_t *sym = GB_reversed_map_find_symbol(&gb.reversed_symbol_map, tok);
    if (sym) {
        out->addr = sym->addr;
        return true;
    }
#endif
    fprintf(stderr, "watch: unknown symbol '%s' (no --sym file, or symbol not in map)\n", tok);
    return false;
}

static bool resolve_watches(const char *arg)
{
    n_watches = 0;
    const char *s = arg;
    while (*s) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
        while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
        if (len == 0) goto next;
        if (len >= WATCH_LABEL_MAX) {
            fprintf(stderr, "watch: token too long\n");
            return false;
        }
        if (n_watches >= WATCH_MAX) {
            fprintf(stderr, "watch: too many entries (max %d)\n", WATCH_MAX);
            return false;
        }
        char tok[WATCH_LABEL_MAX];
        memcpy(tok, s, len);
        tok[len] = '\0';
        if (!resolve_watch_token(tok, &watches[n_watches])) return false;
        n_watches++;
    next:
        if (!comma) break;
        s = comma + 1;
    }
    return true;
}

static void apply_scripted_input(GB_gameboy_t *_gb)
{
    while (next_script_event < n_script_events && script_events[next_script_event].frame == frames) {
        const script_event_t *ev = &script_events[next_script_event++];
        for (int k = 0; k < GB_KEY_MAX; k++) {
            if (ev->buttons & (1u << k)) {
                GB_set_key_state(_gb, (GB_key_t)k, ev->press);
            }
        }
    }
}

static void dump_trace_row(GB_gameboy_t *_gb)
{
    if (!trace_file) return;
    fprintf(trace_file, "%u\t", frames);
    bool first = true;
    for (int k = 0; k < GB_KEY_MAX; k++) {
        if (_gb->keys[0][k]) {
            if (!first) fputc('+', trace_file);
            fputs(button_names[k], trace_file);
            first = false;
        }
    }
    if (first) fputc('.', trace_file);
    for (unsigned w = 0; w < n_watches; w++) {
        fprintf(trace_file, "\t0x%02X", GB_safe_read_memory(_gb, watches[w].addr));
    }
    fputc('\n', trace_file);
}

/* Function-cycle profiler (--profile). Design: Tester/TAD.md D1-D12.
   Brackets are SP-keyed (entry SP + the return address read from it), so
   early returns, tail calls, recursion and IRQ preemption all close on
   "PC == return address with the stack unwound past the entry frame" rather
   than on any particular ret instruction. Reported unit is T-cycles
   (GB_run's 8MHz ticks / 2 — valid at single speed; the SDK consumer is
   DMG-only). */
#define PROFILE_MAX 8
#define PROFILE_FRAMES_MAX 16
#define PROFILE_IRQ_MAX 8

typedef struct {
    char     label[WATCH_LABEL_MAX];
    uint16_t addr;
    int      bank;         /* -1 = any (hex-literal token) */
    unsigned long long calls;
    unsigned long long total_excl;
    unsigned long long max_excl;
    unsigned long long max_incl;
} profile_sym_t;

typedef struct {
    unsigned sym_index;
    uint16_t sp_entry;     /* SP at entry: points at the return address */
    uint16_t return_addr;
    uint16_t entry_bank;
    unsigned entry_frame;
    uint64_t t0;           /* profile_ticks at entry (8MHz ticks) */
    uint64_t irq_sub;      /* ticks spent in IRQ handlers that preempted us */
    unsigned irq_count;
} profile_frame_t;

typedef struct {
    uint16_t sp_entry;
    uint16_t return_addr;
    uint64_t t0;
} irq_window_t;

static const char      *profile_arg;
static const char      *profile_filename;
static FILE            *profile_file;
static profile_sym_t    profile_syms[PROFILE_MAX];
static unsigned         n_profile_syms;
static profile_frame_t  profile_stack[PROFILE_FRAMES_MAX];
static unsigned         profile_depth;
static irq_window_t     irq_windows[PROFILE_IRQ_MAX];
static unsigned         irq_depth;
static uint64_t         profile_ticks;
static bool             profile_warned_missed, profile_warned_full, profile_warned_speed;

static bool resolve_profile_token(const char *tok, profile_sym_t *out)
{
    size_t len = strlen(tok);
    if (len == 0 || len >= WATCH_LABEL_MAX) return false;
    memcpy(out->label, tok, len + 1);
    out->calls = out->total_excl = out->max_excl = out->max_incl = 0;

    if (len > 2 && (tok[0] == '0') && (tok[1] == 'x' || tok[1] == 'X')) {
        char *end;
        unsigned long v = strtoul(tok + 2, &end, 16);
        if (*end != '\0' || v > 0xFFFF) return false;
        out->addr = (uint16_t)v;
        out->bank = -1;
        return true;
    }

#ifndef GB_DISABLE_DEBUGGER
    /* The reversed map keeps one entry per (name, bank) and its find function
       returns the first hit, so scan every chain: D11 makes cross-bank
       ambiguity a hard error rather than a silent first-match. */
    const GB_symbol_t *found = NULL;
    bool ambiguous = false;
    for (unsigned b = 0; b < sizeof(gb.reversed_symbol_map.buckets) / sizeof(gb.reversed_symbol_map.buckets[0]); b++) {
        for (const GB_symbol_t *sym = gb.reversed_symbol_map.buckets[b]; sym; sym = sym->next) {
            if (strcmp(sym->name, tok) != 0) continue;
            if (found && (found->bank != sym->bank || found->addr != sym->addr)) ambiguous = true;
            if (!found) found = sym;
        }
    }
    if (ambiguous) {
        fprintf(stderr, "profile: symbol '%s' is ambiguous across banks; no bank:name syntax in v1 — make .sym names unique\n", tok);
        return false;
    }
    if (found) {
        out->addr = found->addr;
        out->bank = found->bank;
        return true;
    }
#endif
    fprintf(stderr, "profile: unknown symbol '%s' (no --sym file, or symbol not in map)\n", tok);
    return false;
}

static bool resolve_profiles(const char *arg)
{
    n_profile_syms = 0;
    const char *s = arg;
    while (*s) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
        while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
        if (len == 0) goto next;
        if (len >= WATCH_LABEL_MAX) {
            fprintf(stderr, "profile: token too long\n");
            return false;
        }
        if (n_profile_syms >= PROFILE_MAX) {
            fprintf(stderr, "profile: too many symbols (max %d)\n", PROFILE_MAX);
            return false;
        }
        char tok[WATCH_LABEL_MAX];
        memcpy(tok, s, len);
        tok[len] = '\0';
        if (!resolve_profile_token(tok, &profile_syms[n_profile_syms])) return false;
        n_profile_syms++;
    next:
        if (!comma) break;
        s = comma + 1;
    }
    return true;
}

static uint16_t profile_read16(GB_gameboy_t *_gb, uint16_t addr)
{
    return GB_safe_read_memory(_gb, addr) | (GB_safe_read_memory(_gb, (uint16_t)(addr + 1)) << 8);
}

static uint16_t profile_mapped_bank(GB_gameboy_t *_gb, uint16_t addr)
{
    if (addr < 0x4000) return _gb->mbc_rom0_bank;
    if (addr < 0x8000) return _gb->mbc_rom_bank;
    return 0; /* RAM code: not bank-qualified (DMG scope) */
}

static void profile_emit(profile_frame_t *f)
{
    profile_sym_t *s = &profile_syms[f->sym_index];
    uint64_t incl_ticks = profile_ticks - f->t0;
    uint64_t excl_ticks = incl_ticks - f->irq_sub;
    /* D1/D9: T-cycles = 8MHz ticks / 2 at single speed */
    unsigned long long incl = incl_ticks / 2;
    unsigned long long excl = excl_ticks / 2;
    if (gb.cgb_double_speed && !profile_warned_speed) {
        profile_warned_speed = true;
        fprintf(stderr, "profile: CPU is in double-speed mode; the ticks/2 T-cycle conversion is wrong there (DMG-only feature)\n");
    }
    fprintf(profile_file, "%s,%u,%llu,%u,%llu,%llu,%u\n",
            s->label, f->entry_bank, s->calls, f->entry_frame, incl, excl, f->irq_count);
    s->calls++;
    s->total_excl += excl;
    if (excl > s->max_excl) s->max_excl = excl;
    if (incl > s->max_incl) s->max_incl = incl;
}

static void profile_step(GB_gameboy_t *_gb)
{
    uint16_t pc = _gb->pc;
    uint16_t sp = _gb->registers[GB_REGISTER_SP];

    /* Close completed IRQ windows (LIFO). A window's duration is charged to
       the brackets it preempted (entered before the window opened) — unless
       another window is still open around those brackets, in which case that
       outer window's eventual duration already contains this one. */
    while (irq_depth) {
        irq_window_t *w = &irq_windows[irq_depth - 1];
        if (pc != w->return_addr || sp < (uint16_t)(w->sp_entry + 2)) break;
        uint64_t d = profile_ticks - w->t0;
        uint64_t w_t0 = w->t0;
        irq_depth--;
        for (unsigned i = 0; i < profile_depth; i++) {
            profile_frame_t *f = &profile_stack[i];
            if (f->t0 <= w_t0 && (irq_depth == 0 || irq_windows[irq_depth - 1].t0 < f->t0)) {
                f->irq_sub += d;
                f->irq_count++;
            }
        }
    }

    /* Close the topmost completed bracket; anything stacked above it missed
       its return observation and is discarded (warned once). */
    for (unsigned i = profile_depth; i--;) {
        profile_frame_t *f = &profile_stack[i];
        if (pc == f->return_addr && sp >= (uint16_t)(f->sp_entry + 2)) {
            if (i + 1 < profile_depth && !profile_warned_missed) {
                profile_warned_missed = true;
                fprintf(stderr, "profile: discarded %u nested bracket(s) whose return was never observed\n",
                        profile_depth - i - 1);
            }
            profile_emit(f);
            profile_depth = i;
            break;
        }
    }

    /* IRQ dispatch detection (D4): only relevant while something is bracketed
       (or while inside a tracked handler, for correct nesting). */
    if ((profile_depth || irq_depth) &&
        (pc == 0x40 || pc == 0x48 || pc == 0x50 || pc == 0x58 || pc == 0x60)) {
        if (irq_depth < PROFILE_IRQ_MAX) {
            /* Suppress re-observation of the same dispatch (e.g. halt loops). */
            if (!irq_depth || irq_windows[irq_depth - 1].sp_entry != sp ||
                irq_windows[irq_depth - 1].return_addr != profile_read16(_gb, sp)) {
                irq_window_t *w = &irq_windows[irq_depth++];
                w->sp_entry = sp;
                w->return_addr = profile_read16(_gb, sp);
                w->t0 = profile_ticks;
            }
        }
    }

    /* Entry detection, bank-qualified (D3/D11). */
    for (unsigned s = 0; s < n_profile_syms; s++) {
        profile_sym_t *ps = &profile_syms[s];
        if (pc != ps->addr) continue;
        if (ps->bank >= 0 && pc < 0x8000 &&
            profile_mapped_bank(_gb, pc) != (uint16_t)ps->bank) {
            continue;
        }
        /* Same activation re-observed (halt/wait loop at the entry address, or
           a tail self-jump): SP unchanged ⇒ not a new call. */
        if (profile_depth && profile_stack[profile_depth - 1].sym_index == s &&
            profile_stack[profile_depth - 1].sp_entry == sp) {
            break;
        }
        if (profile_depth >= PROFILE_FRAMES_MAX) {
            if (!profile_warned_full) {
                profile_warned_full = true;
                fprintf(stderr, "profile: bracket stack full (%d); dropping entries — is the routine returning?\n",
                        PROFILE_FRAMES_MAX);
            }
            break;
        }
        profile_frame_t *f = &profile_stack[profile_depth++];
        f->sym_index = s;
        f->sp_entry = sp;
        f->return_addr = profile_read16(_gb, sp);
        f->entry_bank = ps->bank >= 0 ? (uint16_t)ps->bank : profile_mapped_bank(_gb, pc);
        f->entry_frame = frames;
        f->t0 = profile_ticks;
        f->irq_sub = 0;
        f->irq_count = 0;
        break;
    }
}

static void profile_finish(void)
{
    if (!profile_file) return;
    for (unsigned s = 0; s < n_profile_syms; s++) {
        profile_sym_t *ps = &profile_syms[s];
        fprintf(profile_file,
                "# summary symbol=%s calls=%llu total_t_cycles_excl=%llu max_t_cycles_excl=%llu max_t_cycles_incl=%llu\n",
                ps->label, ps->calls, ps->total_excl, ps->max_excl, ps->max_incl);
    }
    if (profile_depth) {
        fprintf(stderr, "profile: %u bracket(s) still open at end of run (not emitted)\n", profile_depth);
    }
    if (profile_file != stdout) fclose(profile_file);
    profile_file = NULL;
}
/* end fork extensions ---------------------------------------------------- */
static uint8_t bmp_header[] = {
    0x42, 0x4D, 0x48, 0x68, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x38, 0x00,
    0x00, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x70, 0xFF,
    0xFF, 0xFF, 0x01, 0x00, 0x20, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x02, 0x68, 0x01, 0x00, 0x12, 0x0B,
    0x00, 0x00, 0x12, 0x0B, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint8_t tga_header[] = {
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x90, 0x00,
    0x20, 0x28,
};

uint32_t bitmap[256*224];

static char *async_input_callback(GB_gameboy_t *gb)
{
    return NULL;
}

static void handle_buttons(GB_gameboy_t *gb)
{
    if (script_filename) {
        apply_scripted_input(gb);
        return;
    }
    if (!gb->cgb_double_speed && unsafe_speed_switch) {
        return;
    }
    /* Do not press any buttons during the last two seconds, this might cause a
     screenshot to be taken while the LCD is off if the press makes the game
     load graphics. */
    if (push_start_a && (frames < test_length - 120 || do_not_stop)) {
        unsigned combo_length = 40;
        if (start_is_not_first || push_a_twice) combo_length = 60; /* The start item in the menu is not the first, so also push down */
        else if (a_is_bad || start_is_bad) combo_length = 20; /* Pressing A has a negative effect (when trying to start the game). */
        
        if (semi_random) {
            if (frames % 10 == 0) {
                unsigned key = (((frames / 20) * 0x1337cafe) >> 29) & 7;
                gb->keys[0][key] = (frames % 20) == 0;
            }
        }
        else {
            switch ((push_faster ? frames * 2 :
                     push_slower ? frames / 2 :
                     push_a_twice? frames / 4:
                     frames) % combo_length + (start_is_bad? 20 : 0) ) {
                case 0:
                    if (!limit_start || frames < 20 * 60) {
                        GB_set_key_state(gb, push_right? GB_KEY_RIGHT: GB_KEY_START, true);
                    }
                    if (pointer_control) {
                        GB_set_key_state(gb, GB_KEY_LEFT, true);
                        GB_set_key_state(gb, GB_KEY_UP, true);
                    }
                    
                    break;
                case 10:
                    GB_set_key_state(gb, push_right? GB_KEY_RIGHT: GB_KEY_START, false);
                    if (pointer_control) {
                        GB_set_key_state(gb, GB_KEY_LEFT, false);
                        GB_set_key_state(gb, GB_KEY_UP, false);
                    }
                    break;
                case 20:
                    GB_set_key_state(gb, b_is_confirm? GB_KEY_B: GB_KEY_A, true);
                    break;
                case 30:
                    GB_set_key_state(gb, b_is_confirm? GB_KEY_B: GB_KEY_A, false);
                    break;
                case 40:
                    if (push_a_twice) {
                        GB_set_key_state(gb, b_is_confirm? GB_KEY_B: GB_KEY_A, true);
                    }
                    else if (gb->boot_rom_finished) {
                        GB_set_key_state(gb, GB_KEY_DOWN, true);
                    }
                    break;
                case 50:
                    GB_set_key_state(gb, b_is_confirm? GB_KEY_B: GB_KEY_A, false);
                    GB_set_key_state(gb, GB_KEY_DOWN, false);
                    break;
            }
        }
    }

}

static void vblank(GB_gameboy_t *gb, GB_vblank_type_t type)
{
    /* Detect common crashes and stop the test early */
    if (frames < test_length - 1) {
        if (gb->backtrace_size >= 0x200 + (large_stack? 0x80: 0) || (!allow_weird_sp_values && (gb->registers[GB_REGISTER_SP] >= 0xfe00 && gb->registers[GB_REGISTER_SP] < 0xff80))) {
            GB_log(gb, "A stack overflow has probably occurred. (SP = $%04x; backtrace size = %d) \n",
                   gb->registers[GB_REGISTER_SP], gb->backtrace_size);
            frames = test_length - 1;
        }
        if (gb->halted && !gb->interrupt_enable && gb->speed_switch_halt_countdown == 0) {
            GB_log(gb, "The game is deadlocked.\n");
            frames = test_length - 1;
        }
    }

    if (frames >= test_length && !gb->disable_rendering) {
        bool is_screen_blank = true;
        if (!gb->sgb) {
            for (unsigned i = 160 * 144; i--;) {
                if (bitmap[i] != bitmap[0]) {
                    is_screen_blank = false;
                    break;
                }
            }
        }
        else {
            if (gb->sgb->mask_mode == 0) {
                for (unsigned i = 160 * 144; i--;) {
                    if (gb->sgb->screen_buffer[i] != gb->sgb->screen_buffer[0]) {
                        is_screen_blank = false;
                        break;
                    }
                }
            }
        }
        
        /* Let the test run for extra four seconds if the screen is off/disabled */
        if (!is_screen_blank || frames >= test_length + 60 * 4) {
            FILE *f = fopen(bmp_filename, "wb");
            if (use_tga) {
                tga_header[0xC] = GB_get_screen_width(gb);
                tga_header[0xD] = GB_get_screen_width(gb) >> 8;
                tga_header[0xE] = GB_get_screen_height(gb);
                tga_header[0xF] = GB_get_screen_height(gb) >> 8;
                fwrite(&tga_header, 1, sizeof(tga_header), f);
            }
            else {
                (*(uint32_t *)&bmp_header[0x2]) = sizeof(bmp_header) + sizeof(bitmap[0]) * GB_get_screen_width(gb) * GB_get_screen_height(gb) + 2;
                (*(uint32_t *)&bmp_header[0x12]) = GB_get_screen_width(gb);
                (*(int32_t *)&bmp_header[0x16]) = -GB_get_screen_height(gb);
                (*(uint32_t *)&bmp_header[0x22]) = sizeof(bitmap[0]) * GB_get_screen_width(gb) * GB_get_screen_height(gb) + 2;
                fwrite(&bmp_header, 1, sizeof(bmp_header), f);
            }
            fwrite(&bitmap, 1, sizeof(bitmap[0]) * GB_get_screen_width(gb) * GB_get_screen_height(gb), f);
            fclose(f);
            if (!gb->boot_rom_finished) {
                GB_log(gb, "Boot ROM did not finish.\n");
            }
            if (is_screen_blank) {
                GB_log(gb, "Game probably stuck with blank screen. \n");
            }
            if (sav_filename) {
                GB_save_battery(gb, sav_filename);
            }
            running = false;
        }
    }
    else if (frames >= test_length - 1) {
        gb->disable_rendering = false;
    }
}

static void log_callback(GB_gameboy_t *gb, const char *string, GB_log_attributes_t attributes)
{
    if (!log_file) log_file = fopen(log_filename, "w");
    fprintf(log_file, "%s", string);
}

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static const char *executable_folder(void)
{
    static char path[1024] = {0,};
    if (path[0]) {
        return path;
    }
    /* Ugly unportable code! :( */
#ifdef __APPLE__
    uint32_t length = sizeof(path) - 1;
    _NSGetExecutablePath(&path[0], &length);
#else
#ifdef __linux__
    size_t __attribute__((unused)) length = readlink("/proc/self/exe", &path[0], sizeof(path) - 1);
    assert(length != -1);
#else
#ifdef _WIN32
    HMODULE hModule = GetModuleHandle(NULL);
    GetModuleFileName(hModule, path, sizeof(path) - 1);
#else
    /* No OS-specific way, assume running from CWD */
    getcwd(&path[0], sizeof(path) - 1);
    return path;
#endif
#endif
#endif
    size_t pos = strlen(path);
    while (pos) {
        pos--;
#ifdef _WIN32
        if (path[pos] == '\\') {
#else
        if (path[pos] == '/') {
#endif
            path[pos] = 0;
            break;
        }
    }
    return path;
}

static char *executable_relative_path(const char *filename)
{
    static char path[1024];
    snprintf(path, sizeof(path), "%s/%s", executable_folder(), filename);
    return path;
}

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
#ifdef GB_BIG_ENDIAN
    if (use_tga) {
        return (r << 8) | (g << 16) | (b << 24);
    }
    return (r << 0) | (g << 8) | (b << 16);
#else
    if (use_tga) {
        return (r << 16) | (g << 8) | (b);
    }
    return (r << 24) | (g << 16) | (b << 8);
#endif
}

static void replace_extension(const char *src, size_t length, char *dest, const char *ext)
{
    memcpy(dest, src, length);
    dest[length] = 0;

    /* Remove extension */
    for (size_t i = length; i--;) {
        if (dest[i] == '/') break;
        if (dest[i] == '.') {
            dest[i] = 0;
            break;
        }
    }

    /* Add new extension */
    strcat(dest, ext);
}


int main(int argc, char **argv)
{
    fprintf(stderr, "SameBoy Tester v" GB_VERSION "\n");

    if (argc == 1) {
        fprintf(stderr, "Usage: %s [--dmg] [--sgb] [--cgb] [--start] [--length seconds] [--sav] [--boot path to boot ROM]"
#ifndef _WIN32
                        " [--jobs number of tests to run simultaneously]"
#endif
                        " [--sym path to .sym] [--watch sym_or_addr[,...]] [--script path] [--trace-out path]"
                        " [--profile sym_or_addr[,...]] [--profile-out path]"
                        " rom ...\n", argv[0]);
        exit(1);
    }

#ifndef _WIN32
    unsigned int max_forks = 1;
    unsigned int current_forks = 0;
#endif

    bool dmg = false;
    bool sgb = false;
    bool sav = false;
    const char *boot_rom_path = NULL;
    
    GB_random_set_enabled(false);

    for (unsigned i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dmg") == 0) {
            fprintf(stderr, "Using DMG mode\n");
            dmg = true;
            sgb = false;
            continue;
        }
        
        if (strcmp(argv[i], "--sgb") == 0) {
            fprintf(stderr, "Using SGB mode\n");
            sgb = true;
            dmg = false;
            continue;
        }
        
        if (strcmp(argv[i], "--cgb") == 0) {
            fprintf(stderr, "Using CGB mode\n");
            dmg = false;
            sgb = false;
            continue;
        }
        
        if (strcmp(argv[i], "--tga") == 0) {
            fprintf(stderr, "Using TGA output\n");
            use_tga = true;
            continue;
        }

        if (strcmp(argv[i], "--start") == 0) {
            fprintf(stderr, "Pushing Start and A\n");
            push_start_a = true;
            continue;
        }
        
        if (strcmp(argv[i], "--length") == 0 && i != argc - 1) {
            test_length = atoi(argv[++i]) * 60;
            fprintf(stderr, "Test length is %d seconds\n", test_length / 60);
            continue;
        }
        
        if (strcmp(argv[i], "--boot") == 0 && i != argc - 1) {
            fprintf(stderr, "Using boot ROM %s\n", argv[i + 1]);
            boot_rom_path = argv[++i];
            continue;
        }
        
        if (strcmp(argv[i], "--sav") == 0) {
            fprintf(stderr, "Saving a battery save\n");
            sav = true;
            continue;
        }

        if (strcmp(argv[i], "--sym") == 0 && i != argc - 1) {
            sym_filename = argv[++i];
            fprintf(stderr, "Symbol file: %s\n", sym_filename);
            continue;
        }

        if (strcmp(argv[i], "--watch") == 0 && i != argc - 1) {
            watch_arg = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--script") == 0 && i != argc - 1) {
            script_filename = argv[++i];
            fprintf(stderr, "Input script: %s\n", script_filename);
            continue;
        }

        if (strcmp(argv[i], "--trace-out") == 0 && i != argc - 1) {
            trace_filename = argv[++i];
            fprintf(stderr, "Trace output: %s\n", trace_filename);
            continue;
        }

        if (strcmp(argv[i], "--profile") == 0 && i != argc - 1) {
            profile_arg = argv[++i];
            fprintf(stderr, "Profiling: %s\n", profile_arg);
            continue;
        }

        if (strcmp(argv[i], "--profile-out") == 0 && i != argc - 1) {
            profile_filename = argv[++i];
            fprintf(stderr, "Profile output: %s\n", profile_filename);
            continue;
        }

#ifndef _WIN32
        if (strcmp(argv[i], "--jobs") == 0 && i != argc - 1) {
            max_forks = atoi(argv[++i]);
            /* Make sure wrong input doesn't blow anything up. */
            if (max_forks < 1) max_forks = 1;
            if (max_forks > 16) max_forks = 16;
            fprintf(stderr, "Running up to %d tests simultaneously\n", max_forks);
            continue;
        }

        if (max_forks > 1) {
            while (current_forks >= max_forks) {
                int wait_out;
                while (wait(&wait_out) == -1);
                current_forks--;
            }
            
            current_forks++;
            if (fork() != 0) continue;
        }
#endif
        filename = argv[i];
        size_t path_length = strlen(filename);

        /* kwigbo-org fork: one-shot init for trace harness. Runs once per
           process (so each --jobs child reloads the script independently). */
        static bool harness_init_done = false;
        if (!harness_init_done) {
            harness_init_done = true;
#ifndef _WIN32
            if (trace_filename && max_forks > 1) {
                fprintf(stderr, "--trace-out is incompatible with --jobs > 1\n");
                exit(1);
            }
            /* D6 names --profile-out, but the default profile destination is
               stdout, which forked runs interleave just the same. */
            if (profile_arg && max_forks > 1) {
                fprintf(stderr, "--profile is incompatible with --jobs > 1\n");
                exit(1);
            }
#endif
            if (profile_filename && !profile_arg) {
                fprintf(stderr, "--profile-out requires --profile\n");
                exit(1);
            }
            if (profile_arg && !sym_filename) {
                /* Numeric tokens still work; only named symbols need --sym. */
                fprintf(stderr, "--profile without --sym: only hex addresses resolvable\n");
            }
            if (script_filename && push_start_a) {
                fprintf(stderr, "--script overrides --start; --start ignored\n");
                push_start_a = false;
            }
            if (watch_arg && !sym_filename) {
                /* Numeric tokens still work; only named symbols need --sym. */
                fprintf(stderr, "--watch without --sym: only hex addresses resolvable\n");
            }
            if (script_filename && !load_script_file(script_filename)) {
                exit(1);
            }
        }
        next_script_event = 0;

        char bitmap_path[path_length + 5]; /* At the worst case, size is strlen(path) + 4 bytes for .bmp + NULL */
        replace_extension(filename, path_length, bitmap_path, use_tga? ".tga" : ".bmp");
        bmp_filename = &bitmap_path[0];
        
        char log_path[path_length + 5];
        replace_extension(filename, path_length, log_path, ".log");
        log_filename = &log_path[0];
        
        char sav_path[path_length + 5];
        if (sav) {
            replace_extension(filename, path_length, sav_path, ".sav");
            sav_filename = &sav_path[0];
        }
        
        fprintf(stderr, "Testing ROM %s\n", filename);
        
        if (dmg) {
            GB_init(&gb, GB_MODEL_DMG_B);
            if (GB_load_boot_rom(&gb, boot_rom_path ?: executable_relative_path("dmg_boot.bin"))) {
                fprintf(stderr, "Failed to load boot ROM from '%s'\n", boot_rom_path ?: executable_relative_path("dmg_boot.bin"));
                exit(1);
            }
        }
        else if (sgb) {
            GB_init(&gb, GB_MODEL_SGB2);
            if (GB_load_boot_rom(&gb, boot_rom_path ?: executable_relative_path("sgb2_boot.bin"))) {
                fprintf(stderr, "Failed to load boot ROM from '%s'\n", boot_rom_path ?: executable_relative_path("sgb2_boot.bin"));
                exit(1);
            }
        }
        else {
            GB_init(&gb, GB_MODEL_CGB_E);
            if (GB_load_boot_rom(&gb, boot_rom_path ?: executable_relative_path("cgb_boot.bin"))) {
                fprintf(stderr, "Failed to load boot ROM from '%s'\n", boot_rom_path ?: executable_relative_path("cgb_boot.bin"));
                exit(1);
            }
        }
        
        GB_set_vblank_callback(&gb, (GB_vblank_callback_t) vblank);
        GB_set_pixels_output(&gb, &bitmap[0]);
        GB_set_rgb_encode_callback(&gb, rgb_encode);
        GB_set_log_callback(&gb, log_callback);
        GB_set_async_input_callback(&gb, async_input_callback);
        GB_set_color_correction_mode(&gb, GB_COLOR_CORRECTION_EMULATE_HARDWARE);
        GB_set_rtc_mode(&gb, GB_RTC_MODE_ACCURATE);
        GB_set_emulate_joypad_bouncing(&gb, false); // Adds too much noise
        
        if (GB_load_rom(&gb, filename)) {
            perror("Failed to load ROM");
            exit(1);
        }
        
        /* Game specific hacks for start attempt automations */
        /* It's OK. No overflow is possible here. */
        start_is_not_first = strcmp((const char *)(gb.rom + 0x134), "NEKOJARA") == 0 ||
                             strcmp((const char *)(gb.rom + 0x134), "GINGA") == 0;
        a_is_bad = strcmp((const char *)(gb.rom + 0x134), "DESERT STRIKE") == 0 ||
                    /* Restarting in Puzzle Boy/Kwirk (Start followed by A) leaks stack. */
                   strcmp((const char *)(gb.rom + 0x134), "KWIRK") == 0 ||
                   strcmp((const char *)(gb.rom + 0x134), "PUZZLE BOY") == 0;
        start_is_bad = strcmp((const char *)(gb.rom + 0x134), "BLUESALPHA") == 0 ||
                       strcmp((const char *)(gb.rom + 0x134), "ONI 5") == 0;
        b_is_confirm = strcmp((const char *)(gb.rom + 0x134), "ELITE SOCCER") == 0 ||
                       strcmp((const char *)(gb.rom + 0x134), "SOCCER") == 0 ||
                       strcmp((const char *)(gb.rom + 0x134), "GEX GECKO") == 0 ||
                       strcmp((const char *)(gb.rom + 0x134), "BABE") == 0;
        push_faster = strcmp((const char *)(gb.rom + 0x134), "MOGURA DE PON!") == 0 ||
                      strcmp((const char *)(gb.rom + 0x134), "HUGO2 1/2") == 0 ||
                      strcmp((const char *)(gb.rom + 0x134), "HUGO") == 0;
        push_slower = strcmp((const char *)(gb.rom + 0x134), "BAKENOU") == 0;
        do_not_stop = strcmp((const char *)(gb.rom + 0x134), "SPACE INVADERS") == 0;
        push_right = memcmp((const char *)(gb.rom + 0x134), "BOB ET BOB", strlen("BOB ET BOB")) == 0 ||
                     strcmp((const char *)(gb.rom + 0x134), "LITTLE MASTER") == 0 ||
                     /* M&M's Minis Madness Demo (which has no menu but the same title as the full game) */
                     (memcmp((const char *)(gb.rom + 0x134), "MINIMADNESSBMIE", strlen("MINIMADNESSBMIE")) == 0 &&
                      gb.rom[0x14e] == 0x6c);
        /* This game has some terrible menus. */
        semi_random = strcmp((const char *)(gb.rom + 0x134), "KUKU GAME") == 0;
        

        
        /* This game temporarily sets SP to OAM RAM */
        allow_weird_sp_values = strcmp((const char *)(gb.rom + 0x134), "WDL:TT") == 0 ||
        /* Some mooneye-gb tests abuse the stack */
                                strcmp((const char *)(gb.rom + 0x134), "mooneye-gb test") == 0;
        
        /* This game uses some recursive algorithms and therefore requires quite a large call stack */
        large_stack = memcmp((const char *)(gb.rom + 0x134), "MICRO EPAK1BM", strlen("MICRO EPAK1BM")) == 0 ||
                      strcmp((const char *)(gb.rom + 0x134), "TECMO BOWL") == 0;
        /* High quality game that leaks stack whenever you open the menu (with start),
         but requires pressing start to play it. */
        limit_start = strcmp((const char *)(gb.rom + 0x134), "DIVA STARS") == 0;
        large_stack |= limit_start;

        /* Pressing start while in the map in Tsuri Sensei will leak an internal screen-stack which
           will eventually overflow, override an array of jump-table indexes, jump to a random
           address, execute an invalid opcode, and crash. Pressing A twice while slowing down
           will prevent this scenario. */
        push_a_twice = strcmp((const char *)(gb.rom + 0x134), "TURI SENSEI V1") == 0;

        /* Yes, you should totally use a cursor point & click interface for the language select menu. */
        pointer_control = memcmp((const char *)(gb.rom + 0x134), "LEGO ATEAM BLPP", strlen("LEGO ATEAM BLPP")) == 0;
        push_faster |= pointer_control;
        
        /* Games that perform an unsafe speed switch, don't input until in double speed */
        unsafe_speed_switch = strcmp((const char *)(gb.rom + 0x134), "GBVideo") == 0 || // lulz this is my fault
                              strcmp((const char *)(gb.rom + 0x134), "POKEMONGOLD 2") == 0; // Pokemon Adventure

        
        /* kwigbo-org fork: per-ROM trace harness setup. Symbols and trace file
           depend on the live GB state, so they're (re)bound after each load. */
        if (sym_filename) {
            GB_debugger_load_symbol_file(&gb, sym_filename);
        }
        n_watches = 0;
        if (watch_arg && !resolve_watches(watch_arg)) {
            exit(1);
        }
        if (trace_filename) {
            trace_file = fopen(trace_filename, "w");
            if (!trace_file) {
                fprintf(stderr, "Failed to open trace file '%s'\n", trace_filename);
                exit(1);
            }
            fprintf(trace_file, "# rom: %s\n", filename);
            fputs("frame\tbuttons", trace_file);
            for (unsigned w = 0; w < n_watches; w++) {
                fprintf(trace_file, "\t%s", watches[w].label);
            }
            fputc('\n', trace_file);
        }
        n_profile_syms = 0;
        if (profile_arg) {
            /* Resolution needs the symbol map, so it runs after
               GB_debugger_load_symbol_file — like --watch above. */
            if (!resolve_profiles(profile_arg)) {
                exit(1);
            }
            profile_file = profile_filename ? fopen(profile_filename, "w") : stdout;
            if (!profile_file) {
                fprintf(stderr, "Failed to open profile file '%s'\n", profile_filename);
                exit(1);
            }
            profile_depth = irq_depth = 0;
            profile_ticks = 0;
            profile_warned_missed = profile_warned_full = profile_warned_speed = false;
            fprintf(profile_file, "# rom: %s\n", filename);
            fputs("# unit: T-cycles (GB_run 8MHz ticks / 2, single-speed)\n", profile_file);
            fputs("symbol,bank,call_index,entry_frame,t_cycles_incl,t_cycles_excl,irq_count\n", profile_file);
        }

        /* Run emulation */
        running = true;
        gb.turbo = gb.turbo_dont_skip = gb.disable_rendering = true;
        frames = 0;
        unsigned cycles = 0;
        while (running) {
            unsigned step_ticks = GB_run(&gb);
            cycles += step_ticks;
            if (profile_file) { /* D8: inert unless --profile was passed */
                profile_ticks += step_ticks;
                profile_step(&gb);
            }
            if (cycles >= 139810) { /* Approximately 1/60 a second. Intentionally not the actual length of a frame. */
                dump_trace_row(&gb);
                handle_buttons(&gb);
                cycles -= 139810;
                frames++;
            }
            /* This early crash test must not run in vblank because PC might not point to the next instruction. */
            if (gb.pc == 0x38 && frames < test_length - 1 && GB_read_memory(&gb, 0x38) == 0xFF) {
                GB_log(&gb, "The game is probably stuck in an FF loop.\n");
                frames = test_length - 1;
            }
        }
        
        
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }

        if (trace_file) {
            fclose(trace_file);
            trace_file = NULL;
        }

        profile_finish();

        GB_free(&gb);
#ifndef _WIN32
        if (max_forks > 1) {
            exit(0);
        }
#endif
    }
#ifndef _WIN32
    int wait_out;
    while (wait(&wait_out) != -1);
#endif
    return 0;
}

