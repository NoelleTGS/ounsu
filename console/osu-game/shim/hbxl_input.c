/*
 * osu! on HBLauncher (Switch 2): touchscreen and controller input.
 *
 * SDL runs on the offscreen video driver, which has no event source, so osu-framework only ever
 * saw the raw pad through SDL's hbx joystick driver (osu!'s menus don't use joysticks). This
 * shim wraps SDL_PumpEvents (-Wl,--wrap=SDL_PumpEvents; osu-framework calls it once per event
 * loop pass, then reads the queue with SDL_PeepEvents) and pushes ordinary SDL events through the
 * public SDL_PushEvent API. SDL itself is not modified.
 *
 *   touchscreen     -> SDL_EVENT_FINGER_* (osu-framework's TouchHandler; taps click, drags scroll)
 *   left stick      -> mouse cursor        right stick -> mouse wheel
 *   A               -> left click          Y           -> right click
 *   Plus            -> Escape (back)       Minus       -> F9           X -> Enter
 *   ZL / ZR         -> Z / X (osu!'s hit keys)          L / R -> C / V  L3 / R3 -> A / S
 *   B (held)        -> gyro cursor (HBXL_GYRO_BUTTON moves it; B is then the key D)
 *   D-pad           -> arrow keys
 *   (Plus is a normal button under kaiHEN: no SmileBASIC to hide it from.)
 *   USB keyboard    -> keys + text (US layout)   USB mouse -> cursor, buttons, wheel (see below)
 *
 * SDL's internal mouse/keyboard state doesn't see pushed events. osu-framework only reads the
 * global mouse state while the cursor is outside the window, so one WINDOW_MOUSE_ENTER (and
 * FOCUS_GAINED) is pushed at the start.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "hbx_sdk.h"
#include "hbx_app.h"

extern const struct hbx_ctx *hbxl_ctx;
extern void hbxl_log(const char *text);
extern int hbxl_thread_backtrace(int tid, uint64_t *out, int max);
extern void hbxl_emergency_log(const char *text);
extern long hbxl_vfs_lock_owner(void);

void __real_SDL_PumpEvents(void);
void __wrap_SDL_PumpEvents(void);

#define TOUCH_ID      ((SDL_TouchID)0x48425801)
#define MOUSE_ID      ((SDL_MouseID)1)
#define KEYBOARD_ID   ((SDL_KeyboardID)1)
#define TOUCH_W       1280.0f /* SDK touch coordinates */
#define TOUCH_H       720.0f
#define CURSOR_SPEED  1800.0f /* window pixels per second at full deflection */
#define WHEEL_SPEED   12.0f   /* wheel notches per second at full deflection */

/* The gyro button (osu!'s "Switch 2" settings, default B) is masked out when this table is used: it sends
 * nothing to osu!. */
static struct {
    uint32_t buttons;
    SDL_Scancode scancode;
} key_map[] = {
    { HBX_PLUS, SDL_SCANCODE_ESCAPE },
    { HBX_MINUS, SDL_SCANCODE_F9 },
    { HBX_X, SDL_SCANCODE_RETURN },
    { HBX_UP, SDL_SCANCODE_UP },
    { HBX_DOWN, SDL_SCANCODE_DOWN },
    { HBX_LEFT, SDL_SCANCODE_LEFT },
    { HBX_RIGHT, SDL_SCANCODE_RIGHT },
    { HBX_ZL, SDL_SCANCODE_Z },
    { HBX_ZR, SDL_SCANCODE_X },
    { HBX_L, SDL_SCANCODE_C },
    { HBX_R, SDL_SCANCODE_V },
    { HBX_STICK_L, SDL_SCANCODE_A },
    { HBX_STICK_R, SDL_SCANCODE_S },
    { HBX_B, SDL_SCANCODE_D },
};

static const struct {
    uint32_t buttons;
    uint8_t button;
} mouse_map[] = {
    { HBX_A, SDL_BUTTON_LEFT },
    { HBX_Y, SDL_BUTTON_RIGHT },
};

static uint32_t usb_mouse_buttons;  /* SDL_BUTTON_MASK bits held on a USB mouse */

static struct hbx_input_state input;
static bool ready;
static uint64_t last_ns;
static float cursor_x, cursor_y;    /* window coordinates */
static float wheel;                 /* fractional notches */
static uint32_t keys_down;          /* bit per key_map entry */
static uint32_t mouse_down;         /* SDL_BUTTON_MASK bits pushed as down */
static uint32_t prev_ids[HBX_MAX_TOUCH];
static float prev_x[HBX_MAX_TOUCH], prev_y[HBX_MAX_TOUCH];
static uint32_t prev_count;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* -100..100 with a squared response: fine control near the centre, speed at the edge. */
static float curve(int32_t v)
{
    float f = (float)v / 100.0f;
    return f * (f < 0 ? -f : f);
}

static void push_window(SDL_WindowID window, SDL_EventType type)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.window.type = type;
    e.window.timestamp = SDL_GetTicksNS();
    e.window.windowID = window;
    SDL_PushEvent(&e);
}

static void push_finger(SDL_WindowID window, SDL_EventType type, uint32_t id, float x, float y, float pressure)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.tfinger.type = type;
    e.tfinger.timestamp = SDL_GetTicksNS();
    e.tfinger.touchID = TOUCH_ID;
    e.tfinger.fingerID = (SDL_FingerID)id + 1; /* non-zero */
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.pressure = pressure;
    e.tfinger.windowID = window;
    SDL_PushEvent(&e);
}

static void push_motion(SDL_WindowID window, float dx, float dy)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.motion.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.timestamp = SDL_GetTicksNS();
    e.motion.windowID = window;
    e.motion.which = MOUSE_ID;
    e.motion.x = cursor_x;
    e.motion.y = cursor_y;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    SDL_PushEvent(&e);
}

static void push_button(SDL_WindowID window, uint8_t button, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.timestamp = SDL_GetTicksNS();
    e.button.windowID = window;
    e.button.which = MOUSE_ID;
    e.button.button = button;
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = cursor_x;
    e.button.y = cursor_y;
    SDL_PushEvent(&e);
}

static void push_wheel(SDL_WindowID window, float notches)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.wheel.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.timestamp = SDL_GetTicksNS();
    e.wheel.windowID = window;
    e.wheel.which = MOUSE_ID;
    e.wheel.y = notches;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.mouse_x = cursor_x;
    e.wheel.mouse_y = cursor_y;
    e.wheel.integer_y = (Sint32)notches;
    SDL_PushEvent(&e);
}

static void push_key(SDL_WindowID window, SDL_Scancode scancode, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = window;
    e.key.which = KEYBOARD_ID;
    e.key.scancode = scancode;
    e.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    e.key.mod = SDL_KMOD_NONE;
    e.key.down = down;
    SDL_PushEvent(&e);
}

static void touch(SDL_WindowID window, int width, int height)
{
    uint32_t ids[HBX_MAX_TOUCH];
    float xs[HBX_MAX_TOUCH], ys[HBX_MAX_TOUCH];
    uint32_t count = input.touch_count, i, j;

    if (count > HBX_MAX_TOUCH)
        count = HBX_MAX_TOUCH;

    for (i = 0; i < count; i++) {
        bool known = false;
        ids[i] = input.touch[i].id;
        xs[i] = clampf((float)input.touch[i].x / (TOUCH_W - 1.0f), 0.0f, 1.0f);
        ys[i] = clampf((float)input.touch[i].y / (TOUCH_H - 1.0f), 0.0f, 1.0f);

        for (j = 0; j < prev_count; j++) {
            if (prev_ids[j] == ids[i]) {
                known = true;
                break;
            }
        }

        if (!known)
            push_finger(window, SDL_EVENT_FINGER_DOWN, ids[i], xs[i], ys[i], 1.0f);
        else if (xs[i] != prev_x[j] || ys[i] != prev_y[j])
            push_finger(window, SDL_EVENT_FINGER_MOTION, ids[i], xs[i], ys[i], 1.0f);
    }

    for (j = 0; j < prev_count; j++) {
        bool still = false;
        for (i = 0; i < count; i++) {
            if (ids[i] == prev_ids[j]) {
                still = true;
                break;
            }
        }
        if (!still)
            push_finger(window, SDL_EVENT_FINGER_UP, prev_ids[j], prev_x[j], prev_y[j], 0.0f);
    }

    /* Keep the stick cursor where the last touch was, so switching between them doesn't jump. */
    if (count > 0) {
        cursor_x = xs[0] * (float)(width - 1);
        cursor_y = ys[0] * (float)(height - 1);
    }

    for (i = 0; i < count; i++) {
        prev_ids[i] = ids[i];
        prev_x[i] = xs[i];
        prev_y[i] = ys[i];
    }
    prev_count = count;
}

/* ---- gyro cursor ---------------------------------------------------------------------------
 * While the gyro button is held, the controller's rotation moves the cursor: yaw -> x, pitch -> y, through the SDK's
 * six-axis sensor API at audited offsets from the same module as the pad functions (sdk-symbols.json):
 * GetSixAxisSensorHandles / StartSixAxisSensor / GetSixAxisSensorState. Sensors are started the first time the button
 * is pressed (nobody else is affected), for the handheld Joy-Cons, a Pro Controller and a Joy-Con pair; each frame the
 * one turning fastest wins (like the sticks). SixAxisSensorState: delta time, sampling number, acceleration[3],
 * angularVelocity[3] (1.0 = one turn per second), angle[3], direction[3][3], attributes. */
#ifndef SDK_OFF_PAD_HANDHELD
#define SDK_OFF_PAD_HANDHELD     4845408
#define SDK_OFF_PAD_FULL_KEY     4845184
#endif
#define SDK_OFF_SIXAXIS_HANDLES 4850560  /* nn::hid::GetSixAxisSensorHandles(SixAxisSensorHandle*, int, uint const&, NpadStyleSet) */
#define SDK_OFF_SIXAXIS_START   4850640  /* nn::hid::StartSixAxisSensor(SixAxisSensorHandle const&) */
#define SDK_OFF_SIXAXIS_STATE   4851232  /* nn::hid::GetSixAxisSensorState(SixAxisSensorState*, SixAxisSensorHandle const&) */
#define GYRO_MAX_SENSORS 6
#define GYRO_PIXELS_PER_TURN 3600.0f    /* default: 10 window pixels per degree */

typedef int (*sixaxis_handles_fn)(uint32_t *out, int count, const uint32_t *npad_id, uint32_t style);
typedef void (*sixaxis_start_fn)(const uint32_t *handle);
typedef void (*sixaxis_state_fn)(void *state, const uint32_t *handle);

static uint32_t gyro_button = HBX_B;
static float gyro_speed_x = 2.5f, gyro_speed_y = 1.5f, gyro_sign_x = 1.0f, gyro_sign_y = 1.0f;
static int gyro_state;                  /* 0 not started, 1 running, -1 unavailable */
static uint32_t gyro_handles[GYRO_MAX_SENSORS];
static int gyro_count, gyro_log_left = 8;
static sixaxis_state_fn gyro_read;
static float gyro_carry_x, gyro_carry_y;

static const char *button_name(uint32_t bit)
{
    switch (bit) {
    case HBX_B: return "B";
    case HBX_L: return "L";
    case HBX_R: return "R";
    case HBX_ZL: return "ZL";
    case HBX_ZR: return "ZR";
    case HBX_STICK_L: return "L3";
    case HBX_STICK_R: return "R3";
    default: return "nothing (off)";
    }
}

static void gyro_log(const char *source)
{
    char line[160];
    snprintf(line, sizeof line, "hbxl-input: gyro cursor on %s (speed x %.2f y %.2f%s%s)%s",
             button_name(gyro_button), (double)gyro_speed_x, (double)gyro_speed_y,
             gyro_sign_x < 0 ? ", x inverted" : "", gyro_sign_y < 0 ? ", y inverted" : "", source);
    hbxl_log(line);
}

/* osu!'s "Switch 2" settings (console/osu-game/Switch2Settings.cs), called from the update thread; the window thread
 * reads these plain words on its next pump.  button: 0 none, 1 B, 2 L, 3 R, 4 ZL, 5 ZR, 6 L3, 7 R3. */
void hbxl_input_set_gyro(int button, float speed_x, float speed_y, int invert_x, int invert_y)
{
    static const uint32_t bits[] = { 0, HBX_B, HBX_L, HBX_R, HBX_ZL, HBX_ZR, HBX_STICK_L, HBX_STICK_R };
    if (button >= 0 && button < (int)(sizeof bits / sizeof bits[0]))
        gyro_button = bits[button];
    if (speed_x > 0.05f && speed_x < 20.0f)
        gyro_speed_x = speed_x;
    if (speed_y > 0.05f && speed_y < 20.0f)
        gyro_speed_y = speed_y;
    gyro_sign_x = invert_x ? -1.0f : 1.0f;
    gyro_sign_y = invert_y ? -1.0f : 1.0f;
    gyro_log(" [settings]");
}

static void gyro_start(void)
{
    uint64_t handheld = hbxl_ctx->input.pad_handheld, full_key = hbxl_ctx->input.pad_full_key;
    uint64_t base = handheld - SDK_OFF_PAD_HANDHELD;
    static const struct { uint32_t npad, style; } sets[] = {
        { 0x20, 1u << 1 },  /* handheld: attached Joy-Cons */
        { 0, 1u << 0 },     /* player 1: Pro Controller (full key) */
        { 0, 1u << 2 },     /* player 1: Joy-Con pair */
    };
    char line[128];
    gyro_state = -1;
    if (!handheld || !full_key || full_key - base != SDK_OFF_PAD_FULL_KEY) {
        hbxl_log("hbxl-input: gyro unavailable (SDK module doesn't match the audited offsets)");
        return;
    }
    sixaxis_handles_fn get_handles = (sixaxis_handles_fn)(uintptr_t)(base + SDK_OFF_SIXAXIS_HANDLES);
    sixaxis_start_fn start = (sixaxis_start_fn)(uintptr_t)(base + SDK_OFF_SIXAXIS_START);
    gyro_read = (sixaxis_state_fn)(uintptr_t)(base + SDK_OFF_SIXAXIS_STATE);
    gyro_count = 0;
    for (size_t i = 0; i < sizeof sets / sizeof sets[0] && gyro_count < GYRO_MAX_SENSORS; i++) {
        uint32_t handles[2] = { 0, 0 };
        int n = get_handles(handles, 2, &sets[i].npad, sets[i].style);
        for (int k = 0; k < n && k < 2 && gyro_count < GYRO_MAX_SENSORS; k++) {
            start(&handles[k]);
            gyro_handles[gyro_count++] = handles[k];
        }
    }
    gyro_state = gyro_count ? 1 : -1;
    snprintf(line, sizeof line, "hbxl-input: gyro: %d six-axis sensor(s) started", gyro_count);
    hbxl_log(line);
}

static void gyro(SDL_WindowID window, int width, int height, float dt)
{
    uint8_t state[256] __attribute__((aligned(16)));
    float best = 0.0f, wx = 0.0f, wz = 0.0f;
    if (!gyro_button || !(input.buttons & gyro_button)) {
        gyro_carry_x = gyro_carry_y = 0.0f;
        return;
    }
    if (gyro_state == 0)
        gyro_start();
    if (gyro_state != 1)
        return;
    for (int i = 0; i < gyro_count; i++) {
        float w[3];
        memset(state, 0, sizeof state);
        gyro_read(state, &gyro_handles[i]);
        memcpy(w, state + 28, sizeof w);         /* angularVelocity: after deltaTime(8), samplingNumber(8), acceleration(12) */
        float m = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
        if (m > best) {
            best = m;
            wx = w[0];
            wz = w[2];
        }
        if (gyro_log_left > 0 && m > 0.01f) {
            char line[160];
            gyro_log_left--;
            snprintf(line, sizeof line, "hbxl-input: gyro sensor %d raw angular velocity %.3f %.3f %.3f (turns/s)", i,
                     (double)w[0], (double)w[1], (double)w[2]);
            hbxl_log(line);
        }
    }
    gyro_carry_x += -wz * GYRO_PIXELS_PER_TURN * gyro_speed_x * gyro_sign_x * dt;
    gyro_carry_y += -wx * GYRO_PIXELS_PER_TURN * gyro_speed_y * gyro_sign_y * dt;
    float dx = (float)(int)gyro_carry_x, dy = (float)(int)gyro_carry_y;
    if (dx != 0.0f || dy != 0.0f) {
        gyro_carry_x -= dx;
        gyro_carry_y -= dy;
        cursor_x = clampf(cursor_x + dx, 0.0f, (float)(width - 1));
        cursor_y = clampf(cursor_y + dy, 0.0f, (float)(height - 1));
        push_motion(window, dx, dy);
    }
}

static void pad(SDL_WindowID window, int width, int height, float dt)
{
    uint32_t buttons = input.buttons;
    float dx = curve(input.stick_lx) * CURSOR_SPEED * dt;
    float dy = -curve(input.stick_ly) * CURSOR_SPEED * dt; /* SDK: +y is up */
    size_t i;

    if (dx != 0.0f || dy != 0.0f) {
        cursor_x = clampf(cursor_x + dx, 0.0f, (float)(width - 1));
        cursor_y = clampf(cursor_y + dy, 0.0f, (float)(height - 1));
        push_motion(window, dx, dy);
    }

    wheel += curve(input.stick_ry) * WHEEL_SPEED * dt;
    if (wheel >= 1.0f || wheel <= -1.0f) {
        float notches = (float)(int)wheel;
        push_wheel(window, notches);
        wheel -= notches;
    } else if (input.stick_ry == 0) {
        wheel = 0.0f;
    }

    /* Pad clicks and USB mouse buttons drive the same SDL buttons (bit b-1 = SDL button b). */
    uint32_t want = usb_mouse_buttons;
    for (i = 0; i < sizeof mouse_map / sizeof mouse_map[0]; i++)
        if (buttons & mouse_map[i].buttons)
            want |= SDL_BUTTON_MASK(mouse_map[i].button);
    for (uint8_t b = 1; b <= 5; b++) {
        bool down = (want & SDL_BUTTON_MASK(b)) != 0;
        if (down != ((mouse_down & SDL_BUTTON_MASK(b)) != 0)) {
            push_button(window, b, down);
            mouse_down ^= SDL_BUTTON_MASK(b);
        }
    }

    for (i = 0; i < sizeof key_map / sizeof key_map[0]; i++) {
        bool down = (buttons & key_map[i].buttons & ~gyro_button) != 0;   /* the gyro button sends no key */
        if (down != ((keys_down >> i) & 1u)) {
            push_key(window, key_map[i].scancode, down);
            keys_down ^= 1u << i;
        }
    }
}

/* ---- hang watchdog ------------------------------------------------------------------------
 * The main thread pumps events every frame. If the pump count stops for 10 s, dump every
 * thread's backtrace (hbxl_thread_backtrace: pc, lr, frame-pointer chain) through hbxl_log.
 * Nothing here allocates: hangs so far left musl's malloc lock held, which also stops
 * managed code (osu-console's own status thread). Symbolise with tools/symbolize.py. */
static volatile uint64_t pump_count;

static void hex(char **p, uint64_t v)
{
    char t[16];
    int k = 0;
    do {
        t[k++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    *(*p)++ = ' ';
    while (k)
        *(*p)++ = t[--k];
}

static void dump_threads(int round)
{
    static char line[32 * 17 + 64];
    uint64_t frames[32];
    char *p;

    p = line;
    memcpy(p, "threads: dump (main loop stalled, watchdog round", 48);
    p += 48;
    hex(&p, (uint64_t)round);
    memcpy(p, ", fs lock owner tid", 19);
    p += 19;
    hex(&p, (uint64_t)hbxl_vfs_lock_owner());
    *p++ = ')';
    *p = 0;
    hbxl_emergency_log(line);

    for (int tid = 1; tid < 160; tid++) {
        int n = hbxl_thread_backtrace(tid, frames, 32);
        if (n <= 0)
            continue;
        p = line;
        memcpy(p, "threads: tid", 12);
        p += 12;
        {
            /* decimal tid */
            char t[4];
            int k = 0, v = tid;
            do {
                t[k++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            *p++ = ' ';
            while (k)
                *p++ = t[--k];
        }
        *p++ = ':';
        for (int i = 0; i < n; i++)
            hex(&p, frames[i]);
        *p = 0;
        hbxl_emergency_log(line);
    }
    hbxl_emergency_log("threads: end of dump");
}

/* ---- stall detector -----------------------------------------------------------------------
 * Three heartbeats: 0 = window/input thread (each SDL_PumpEvents), 1 = draw thread (each Vulkan
 * present, hbxl_vk_shim.c), 2 = update thread (each update frame, ConsoleOsuGame.Update). A beat after
 * a gap of 250 ms or more logs "hbxl-stall: <thread> gap N ms". While a gap is still open past 400 ms,
 * the watchdog dumps every thread's backtrace over UDP (udp-diag.log), at most once per 5 s, so the
 * stall is caught in the act. The old 10 s main-loop dump (hangs) still runs. HBXL_STALL_DUMP=0 keeps
 * the gap lines but skips the dumps. */
extern uint64_t hbxl_gc_ticks(void);
static const char *const stall_names[3] = { "window/input", "draw", "update" };
static volatile uint64_t stall_last[3];   /* ticks of the last beat, 0 = never */
static volatile int stall_dumped[3];

static uint64_t ticks_to_ms(uint64_t t)
{
    uint64_t hz = hbxl_ctx && hbxl_ctx->tick_hz ? hbxl_ctx->tick_hz : 19200000ull;
    return t * 1000ull / hz;
}

void hbxl_stall_beat(int which)
{
    uint64_t now, prev;
    if (which < 0 || which > 2)
        return;
    now = hbxl_gc_ticks();
    prev = __atomic_exchange_n(&stall_last[which], now, __ATOMIC_RELAXED);
    if (prev && ticks_to_ms(now - prev) >= 250) {
        char line[96];
        snprintf(line, sizeof line, "hbxl-stall: %s thread gap %llu ms%s", stall_names[which],
                 (unsigned long long)ticks_to_ms(now - prev), stall_dumped[which] ? " (threads dumped to udp-diag.log)" : "");
        hbxl_log(line);
    }
    stall_dumped[which] = 0;
}

static void *watchdog(void *arg)
{
    uint64_t last = 0, last_dump = 0;
    int stalled_ms = 0, rounds = 0;
    const char *e = getenv("HBXL_STALL_DUMP");
    bool stall_dumps = !(e && *e == '0');
    (void)arg;
    hbxl_emergency_log("watchdog: armed (stalls > 400 ms and main-loop stalls > 10 s dump all threads here)");
    for (;;) {
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, 0);
        uint64_t now = hbxl_gc_ticks();

        if (stall_dumps) {
            for (int k = 0; k < 3; k++) {
                uint64_t beat = __atomic_load_n(&stall_last[k], __ATOMIC_RELAXED);
                if (!beat || stall_dumped[k] || ticks_to_ms(now - beat) < 400)
                    continue;
                if (last_dump && ticks_to_ms(now - last_dump) < 5000)
                    continue;
                char line[96];
                snprintf(line, sizeof line, "stall: %s thread silent for %llu ms; dumping threads", stall_names[k],
                         (unsigned long long)ticks_to_ms(now - beat));
                hbxl_emergency_log(line);
                stall_dumped[k] = 1;
                last_dump = now;
                dump_threads(100 + k);
            }
        }

        uint64_t pumps = __atomic_load_n(&pump_count, __ATOMIC_RELAXED);
        stalled_ms = pumps == last ? stalled_ms + 50 : 0;
        last = pumps;
        if (stalled_ms >= 10000 && rounds < 3) {
            dump_threads(++rounds);
            stalled_ms = -20000; /* again 30 s later if still stuck */
        }
    }
    return 0;
}

/* hbx_input_poll ORs every pad source's buttons but takes the sticks from the FIRST connected
 * source (handheld Joy-Con before the Pro Controller), so attached Joy-Con with centred sticks
 * overrode a Pro Controller's.  Re-poll the sources the same way (sdk/src/hbx_app.c
 * poll_sources) and use, per stick, whichever source is pushed furthest. */
static int32_t stick_scale(int32_t raw)
{
    int64_t v = raw;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return (int32_t)(v * 100 / 32767);
}

static void sticks_from_all_pads(void)
{
    const uint64_t fns[5] = {
        hbxl_ctx->input.pad_handheld, hbxl_ctx->input.pad_full_key, hbxl_ctx->input.pad_joy_dual,
        hbxl_ctx->input.pad_joy_left, hbxl_ctx->input.pad_joy_right,
    };
    const uint32_t ids[5] = { 32u, 0u, 0u, 0u, 0u };
    struct hbx_pad_state scratch;
    struct hbx_pad_state *pad = &scratch;
    int64_t best_l = 0, best_r = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;

    /* Same scratch block as the SDK (the host's, when aligned): the pad entrypoints write into it. */
    if (hbxl_ctx->input.pad_state && (hbxl_ctx->input.pad_state & 3u) == 0)
        pad = (struct hbx_pad_state *)(uintptr_t)hbxl_ctx->input.pad_state;

    for (int i = 0; i < 5; i++) {
        uint32_t id = ids[i];
        if (!fns[i])
            continue;
        memset(pad, 0, sizeof *pad);
        ((hbx_fn_pad)(uintptr_t)fns[i])(pad, &id);
        if (!(pad->connected & 1u))
            continue;
        int64_t ml = (int64_t)pad->stick_lx * pad->stick_lx + (int64_t)pad->stick_ly * pad->stick_ly;
        int64_t mr = (int64_t)pad->stick_rx * pad->stick_rx + (int64_t)pad->stick_ry * pad->stick_ry;
        if (ml > best_l) { best_l = ml; lx = pad->stick_lx; ly = pad->stick_ly; }
        if (mr > best_r) { best_r = mr; rx = pad->stick_rx; ry = pad->stick_ry; }
    }

    /* Radial deadzone: 15% of full scale, like the SDK. */
    if (best_l <= (int64_t)4915 * 4915) { lx = 0; ly = 0; }
    if (best_r <= (int64_t)4915 * 4915) { rx = 0; ry = 0; }
    input.stick_lx = stick_scale(lx);
    input.stick_ly = stick_scale(ly);
    input.stick_rx = stick_scale(rx);
    input.stick_ry = stick_scale(ry);
}

/* ---- USB keyboard and mouse ---------------------------------------------------------------
 * SmileBASIC 4.4.9 links nn::hid's keyboard and mouse API into the system SDK module, but the
 * launcher only hands apps the Npad and touch entrypoints. Their offsets inside that module come
 * from the same audit as the Npad ones (~/HBLauncher/sdk/sdk-symbols.json), so the module base is
 * the handheld entrypoint minus its offset, cross-checked against the full-key entrypoint (a
 * different SDK build fails the check and keyboard/mouse stay off).
 *
 * State layouts (yuzu's nn::hid::KeyboardState / MouseState, libnx HidKeyboardState/HidMouseState):
 *   KeyboardState 0x30: +0 s64 sampling, +8 u32 modifiers, +12 u32 attributes (bit 0 connected),
 *                       +16 256-bit key set indexed by USB HID usage (= SDL_Scancode)
 *   MouseState    0x28: +0 s64 sampling, +8 x, +12 y, +16 dx, +20 dy, +24 wheel (vertical),
 *                       +28 wheel (horizontal), +32 u32 buttons, +36 u32 attributes (bit 1 connected)
 * The calls read hid shared memory (no IPC) into the host's 4 KiB pad scratch block.
 * HBXL_NO_KBMOUSE=1 turns this off; HBXL_MOUSE_SPEED=<float> scales mouse motion (default 1). */
#define SDK_OFF_INIT_KEYBOARD    4843296
#define SDK_OFF_GET_KEYBOARD     4843344
#define SDK_OFF_INIT_MOUSE       4843504
#define SDK_OFF_GET_MOUSE_STATES 4843600
#define MOUSE_SAMPLES            16
#define KEY_REPEAT_DELAY_NS      500000000ull
#define KEY_REPEAT_RATE_NS       40000000ull

typedef void (*hid_init_fn)(void);
typedef void (*hid_keyboard_fn)(void *state);
typedef int (*hid_mouse_states_fn)(void *states, int count);

static hid_keyboard_fn get_keyboard;
static hid_mouse_states_fn get_mouse_states;
static float mouse_speed = 1.0f;
static bool kbmouse_on = true;      /* the in-game setting, unless HBXL_NO_KBMOUSE=1 */
static bool kbmouse_ready;          /* nn::hid keyboard + mouse initialised (once, on the window thread) */
static uint8_t keys_prev[32];
static bool keyboard_seen, mouse_seen;
static int64_t mouse_last_sampling = -1;
static int mouse_log_left = 12;     /* raw samples logged once, to confirm the layout on hardware */
static SDL_Scancode repeat_key;
static uint64_t repeat_at;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int64_t rd64(const uint8_t *p) { int64_t v; memcpy(&v, p, 8); return v; }

/* HBXL_NO_KBMOUSE=1 in hbxl-env, read once: an escape hatch that keeps nn::hid keyboard/mouse untouched whatever the
 * in-game setting says (the setting can't be reached if that init is what breaks). */
static bool kbmouse_forced_off;

static void kbmouse_config(void)
{
    static bool done;
    const char *e = getenv("HBXL_NO_KBMOUSE");
    if (done)
        return;
    done = true;
    if (e && *e == '1') {
        kbmouse_forced_off = true;
        kbmouse_on = false;
        hbxl_log("hbxl-input: USB keyboard/mouse off (HBXL_NO_KBMOUSE)");
    }
}

/* osu!'s "Switch 2" settings, from the update thread. nn::hid is only touched on the window thread (kbmouse_init). */
void hbxl_input_set_kbmouse(int enabled, float speed)
{
    char line[96];
    kbmouse_config();
    kbmouse_on = enabled != 0 && !kbmouse_forced_off;
    if (speed > 0.05f && speed < 20.0f)
        mouse_speed = speed;
    snprintf(line, sizeof line, "hbxl-input: USB keyboard/mouse %s, mouse speed %.2f [settings]", kbmouse_on ? "on" : "off",
             (double)mouse_speed);
    hbxl_log(line);
}

/* Window thread, on the first pump with USB keyboard/mouse enabled. */
static void kbmouse_init(void)
{
    uint64_t handheld = hbxl_ctx->input.pad_handheld, full_key = hbxl_ctx->input.pad_full_key;
    uint64_t base = handheld - SDK_OFF_PAD_HANDHELD;

    kbmouse_ready = true;
    if (!handheld || !full_key || full_key - base != SDK_OFF_PAD_FULL_KEY) {
        hbxl_log("hbxl-input: USB keyboard/mouse off (SDK module doesn't match the audited offsets)");
        return;
    }
    ((hid_init_fn)(uintptr_t)(base + SDK_OFF_INIT_KEYBOARD))();
    ((hid_init_fn)(uintptr_t)(base + SDK_OFF_INIT_MOUSE))();
    get_keyboard = (hid_keyboard_fn)(uintptr_t)(base + SDK_OFF_GET_KEYBOARD);
    get_mouse_states = (hid_mouse_states_fn)(uintptr_t)(base + SDK_OFF_GET_MOUSE_STATES);
    hbxl_log("hbxl-input: USB keyboard/mouse enabled (nn::hid initialised)");
}

static uint8_t *hid_scratch(void)
{
    if (hbxl_ctx->input.pad_state && (hbxl_ctx->input.pad_state & 7u) == 0)
        return (uint8_t *)(uintptr_t)hbxl_ctx->input.pad_state;
    return 0;
}

/* US layout. Returns 0 for keys that don't type anything. */
static char key_char(SDL_Scancode sc, bool shift, bool caps, bool num)
{
    static const char digits[] = "1234567890", shifted[] = "!@#$%^&*()";
    static const char punct[] = "-=[]\\#;'`,./", punct_shift[] = "_+{}|~:\"~<>?";

    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        char c = (char)('a' + (sc - SDL_SCANCODE_A));
        return (shift != caps) ? (char)(c - 32) : c;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0)
        return (shift ? shifted : digits)[sc - SDL_SCANCODE_1];
    if (sc == SDL_SCANCODE_SPACE)
        return ' ';
    if (sc >= SDL_SCANCODE_MINUS && sc <= SDL_SCANCODE_SLASH)
        return (shift ? punct_shift : punct)[sc - SDL_SCANCODE_MINUS];
    switch (sc) {
    case SDL_SCANCODE_KP_DIVIDE: return '/';
    case SDL_SCANCODE_KP_MULTIPLY: return '*';
    case SDL_SCANCODE_KP_MINUS: return '-';
    case SDL_SCANCODE_KP_PLUS: return '+';
    default: break;
    }
    if (num && sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0)
        return digits[sc - SDL_SCANCODE_KP_1];
    if (num && sc == SDL_SCANCODE_KP_PERIOD)
        return '.';
    return 0;
}

static void push_text(SDL_Window *win, SDL_WindowID window, char c)
{
    /* SDL leaves pushed text alone unless it came from its own temporary pool (internal API), so
     * point at a ring of static strings: osu-framework drains the queue right after each pump. */
    static char ring[64][2];
    static unsigned next;
    SDL_Event e;
    char *s;
    if (!c || !SDL_TextInputActive(win))
        return;
    s = ring[next++ % 64];
    s[0] = c;
    s[1] = 0;
    memset(&e, 0, sizeof e);
    e.text.type = SDL_EVENT_TEXT_INPUT;
    e.text.timestamp = SDL_GetTicksNS();
    e.text.windowID = window;
    e.text.text = s;
    SDL_PushEvent(&e);
}

static void push_usb_key(SDL_WindowID window, SDL_Scancode sc, SDL_Keymod mod, bool down, bool repeat)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = window;
    e.key.which = KEYBOARD_ID;
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    e.key.mod = mod;
    e.key.down = down;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
}

static void keyboard(SDL_Window *win, SDL_WindowID window, uint64_t now)
{
    uint8_t *st = hid_scratch(), keys[32];
    uint32_t mods;
    SDL_Keymod mod = SDL_KMOD_NONE;
    bool shift, caps, num;

    if (!get_keyboard || !st)
        return;
    memset(st, 0, 0x40);
    get_keyboard(st);
    if (!kbmouse_on || !(rd32(st + 12) & 1u)) {
        memset(keys, 0, sizeof keys);   /* unplugged or switched off: release everything */
    } else {
        if (!keyboard_seen) {
            keyboard_seen = true;
            hbxl_log("hbxl-input: USB keyboard connected");
        }
        memcpy(keys, st + 16, sizeof keys);
    }
    mods = rd32(st + 8);

    /* nn::hid::KeyboardModifier: 0 Control, 1 Shift, 2 LeftAlt, 3 RightAlt, 4 Gui, 8 CapsLock, 10 NumLock.
     * Make sure a held modifier also shows up as a key, whether or not the key set carries it. */
#define KEY_ON(sc) ((keys[(sc) >> 3] >> ((sc) & 7)) & 1)
#define KEY_SET(sc) (keys[(sc) >> 3] |= (uint8_t)(1u << ((sc) & 7)))
    if ((mods & 1u) && !KEY_ON(SDL_SCANCODE_LCTRL) && !KEY_ON(SDL_SCANCODE_RCTRL)) KEY_SET(SDL_SCANCODE_LCTRL);
    if ((mods & 2u) && !KEY_ON(SDL_SCANCODE_LSHIFT) && !KEY_ON(SDL_SCANCODE_RSHIFT)) KEY_SET(SDL_SCANCODE_LSHIFT);
    if ((mods & 4u) && !KEY_ON(SDL_SCANCODE_LALT)) KEY_SET(SDL_SCANCODE_LALT);
    if ((mods & 8u) && !KEY_ON(SDL_SCANCODE_RALT)) KEY_SET(SDL_SCANCODE_RALT);
    if ((mods & 16u) && !KEY_ON(SDL_SCANCODE_LGUI) && !KEY_ON(SDL_SCANCODE_RGUI)) KEY_SET(SDL_SCANCODE_LGUI);
    if (KEY_ON(SDL_SCANCODE_LCTRL)) mod |= SDL_KMOD_LCTRL;
    if (KEY_ON(SDL_SCANCODE_RCTRL)) mod |= SDL_KMOD_RCTRL;
    if (KEY_ON(SDL_SCANCODE_LSHIFT)) mod |= SDL_KMOD_LSHIFT;
    if (KEY_ON(SDL_SCANCODE_RSHIFT)) mod |= SDL_KMOD_RSHIFT;
    if (KEY_ON(SDL_SCANCODE_LALT)) mod |= SDL_KMOD_LALT;
    if (KEY_ON(SDL_SCANCODE_RALT)) mod |= SDL_KMOD_RALT;
    if (KEY_ON(SDL_SCANCODE_LGUI)) mod |= SDL_KMOD_LGUI;
    if (KEY_ON(SDL_SCANCODE_RGUI)) mod |= SDL_KMOD_RGUI;
    if (mods & 0x100u) mod |= SDL_KMOD_CAPS;
    if (mods & 0x400u) mod |= SDL_KMOD_NUM;
    shift = (mod & SDL_KMOD_SHIFT) != 0;
    caps = (mod & SDL_KMOD_CAPS) != 0;
    num = (mod & SDL_KMOD_NUM) != 0;

    /* Usage 0-3 are error codes, not keys. */
    for (int sc = 4; sc < 256; sc++) {
        bool down = KEY_ON(sc), was = (keys_prev[sc >> 3] >> (sc & 7)) & 1;
        if (down == was)
            continue;
        push_usb_key(window, (SDL_Scancode)sc, mod, down, false);
        if (down) {
            /* Ctrl/Alt/Gui shortcuts don't type. */
            if (!(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
                push_text(win, window, key_char((SDL_Scancode)sc, shift, caps, num));
            repeat_key = (SDL_Scancode)sc;
            repeat_at = now + KEY_REPEAT_DELAY_NS;
        } else if (repeat_key == sc) {
            repeat_key = SDL_SCANCODE_UNKNOWN;
        }
    }
#undef KEY_ON
#undef KEY_SET

    /* Text repeat for a held key (osu-framework repeats key-down actions such as Backspace itself). */
    if (repeat_key != SDL_SCANCODE_UNKNOWN && now >= repeat_at &&
        !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
        push_text(win, window, key_char(repeat_key, shift, caps, num));
        repeat_at = now + KEY_REPEAT_RATE_NS;
    }
    memcpy(keys_prev, keys, sizeof keys);
}

static void mouse(SDL_WindowID window, int width, int height)
{
    uint8_t *st = hid_scratch();
    int n;
    float dx = 0, dy = 0, wheel_notches = 0;
    int64_t newest;
    uint32_t buttons_raw, attributes;

    if (!get_mouse_states || !st)
        return;
    if (!kbmouse_on) {
        usb_mouse_buttons = 0;          /* switched off in the settings: release held buttons */
        return;
    }
    memset(st, 0, MOUSE_SAMPLES * 0x28 + 0x40);
    n = get_mouse_states(st, MOUSE_SAMPLES);
    if (n < 1 || n > MOUSE_SAMPLES)
        return;
    /* Newest sample first. */
    newest = rd64(st);
    buttons_raw = rd32(st + 32);
    attributes = rd32(st + 36);
    if (!(attributes & 2u)) {
        usb_mouse_buttons = 0;
        mouse_last_sampling = newest;
        return;
    }
    if (!mouse_seen) {
        mouse_seen = true;
        hbxl_log("hbxl-input: USB mouse connected");
    }
    for (int i = n - 1; i >= 0; i--) {
        const uint8_t *s = st + i * 0x28;
        if (mouse_last_sampling < 0 || rd64(s) <= mouse_last_sampling)
            continue;
        dx += (float)(int32_t)rd32(s + 16);
        dy += (float)(int32_t)rd32(s + 20);
        wheel_notches += (float)(int32_t)rd32(s + 24);
        if (mouse_log_left > 0 && (rd32(s + 16) || rd32(s + 20) || rd32(s + 24) || rd32(s + 32))) {
            char line[160];
            mouse_log_left--;
            snprintf(line, sizeof line, "hbxl-input: mouse raw sampling %lld x %d y %d dx %d dy %d wheel %d/%d buttons %#x attr %#x",
                     (long long)rd64(s), (int)rd32(s + 8), (int)rd32(s + 12), (int)rd32(s + 16), (int)rd32(s + 20),
                     (int)rd32(s + 24), (int)rd32(s + 28), rd32(s + 32), rd32(s + 36));
            hbxl_log(line);
        }
    }
    mouse_last_sampling = newest;

    /* hid works in 1280x720 screen space. */
    dx *= mouse_speed * (float)width / TOUCH_W;
    dy *= mouse_speed * (float)height / TOUCH_H;
    if (dx != 0.0f || dy != 0.0f) {
        cursor_x = clampf(cursor_x + dx, 0.0f, (float)(width - 1));
        cursor_y = clampf(cursor_y + dy, 0.0f, (float)(height - 1));
        push_motion(window, dx, dy);
    }
    if (wheel_notches != 0.0f) {
        /* Some stacks report 120 per notch. */
        if (wheel_notches >= 60.0f || wheel_notches <= -60.0f)
            wheel_notches /= 120.0f;
        push_wheel(window, wheel_notches);
    }

    /* nn::hid::MouseButton: 0 left, 1 right, 2 middle, 3 forward, 4 back. */
    usb_mouse_buttons = 0;
    if (buttons_raw & 1u) usb_mouse_buttons |= SDL_BUTTON_LMASK;
    if (buttons_raw & 2u) usb_mouse_buttons |= SDL_BUTTON_RMASK;
    if (buttons_raw & 4u) usb_mouse_buttons |= SDL_BUTTON_MMASK;
    if (buttons_raw & 8u) usb_mouse_buttons |= SDL_BUTTON_X2MASK;
    if (buttons_raw & 16u) usb_mouse_buttons |= SDL_BUTTON_X1MASK;
}

void __wrap_SDL_PumpEvents(void)
{
    SDL_Window **windows, *win;
    SDL_WindowID window;
    int count = 0, width = 0, height = 0;
    uint64_t now;
    float dt;
    static bool watchdog_started;

    __atomic_add_fetch(&pump_count, 1, __ATOMIC_RELAXED);
    hbxl_stall_beat(0);
    if (!watchdog_started) {
        pthread_t t;
        watchdog_started = true;
        if (pthread_create(&t, 0, watchdog, 0) == 0)
            pthread_detach(t);
    }

    __real_SDL_PumpEvents();

    if (!hbxl_ctx)
        return;

    windows = SDL_GetWindows(&count);
    if (!windows)
        return;
    if (count < 1) {
        SDL_free(windows);
        return;
    }
    win = windows[0];
    window = SDL_GetWindowID(win);
    SDL_GetWindowSize(win, &width, &height);
    SDL_free(windows);
    if (width < 1 || height < 1)
        return;

    now = SDL_GetTicksNS();

    if (!ready) {
        push_window(window, SDL_EVENT_WINDOW_FOCUS_GAINED);
        push_window(window, SDL_EVENT_WINDOW_MOUSE_ENTER);
        cursor_x = (float)width / 2.0f;
        cursor_y = (float)height / 2.0f;
        push_motion(window, 0.0f, 0.0f);
        last_ns = now;
        ready = true;
        hbxl_log("hbxl-input: touchscreen + controller (stick = cursor, A = click, Plus = back) active");
        kbmouse_config();
    }
    if (kbmouse_on && !kbmouse_ready)
        kbmouse_init();

    dt = (float)(now - last_ns) / 1e9f;
    if (dt > 0.1f)
        dt = 0.1f;
    last_ns = now;

    hbx_input_poll(hbxl_ctx, &input);
    sticks_from_all_pads();
    touch(window, width, height);
    mouse(window, width, height);
    pad(window, width, height, dt);
    gyro(window, width, height, dt);
    keyboard(win, window, now);
}
