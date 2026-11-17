/*
 * osu! on kaiHEN: touch screen, controller, six-axis, USB keyboard/mouse and Joy-Con 2 mouse input.
 *
 * SDL runs on the offscreen video driver, which has no event source. This shim wraps SDL_PumpEvents
 * (-Wl,--wrap=SDL_PumpEvents; osu-framework calls it once per event loop pass, then reads the queue with
 * SDL_PeepEvents), reads the input through the host (lx_input_poll: libounce hid/pad, lx/host/lx_input.c) and pushes
 * ordinary SDL events through the public SDL_PushEvent API. SDL itself is not modified.
 *
 *   touchscreen     -> SDL_EVENT_FINGER_* (osu-framework's TouchHandler; taps click, drags scroll)
 *   left stick      -> mouse cursor        right stick -> mouse wheel
 *   A               -> left click          Y           -> right click
 *   Plus            -> Escape (back)       Minus       -> F9           X -> Enter
 *   ZL / ZR         -> Z / X (osu!'s hit keys)          L / R -> C / V  L3 / R3 -> A / S
 *   B (held)        -> gyro cursor (the "Switch 2" settings move it; B is then the key D)
 *   D-pad           -> arrow keys
 *   USB keyboard    -> keys + text (US layout)   USB mouse -> cursor, buttons, wheel
 *   Joy-Con 2 mouse -> cursor, its buttons and wheel (the Joy-Con's L/ZL or R/ZR click), per side
 *
 * Joy-Con 2 mouse: a Joy-Con is "in mouse mode" from the moment its sensor tracks movement on a surface until
 * JOYMOUSE_HOLD_NS after the last tracked sample, stick push or mouse button. In mouse mode its stick only scrolls
 * (at the right stick's rate) and its shoulder buttons only click; otherwise its stick and buttons work as on any controller
 * and the Nmouse wheel/buttons are ignored. So the stick never moves the cursor and scrolls at once. The "Switch 2"
 * settings choose which sides may be a mouse (none, left, right, both) and the speed.
 *
 * SDL's internal mouse/keyboard state doesn't see pushed events. osu-framework only reads the global mouse state
 * while the cursor is outside the window, so one WINDOW_MOUSE_ENTER (and FOCUS_GAINED) is pushed at the start.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "lx_abi.h"

int lx_input_poll(struct lx_input *out, uint32_t flags); /* island runtime */
extern void ounsu_log(const char *text);                   /* lx_osu.c */

void __real_SDL_PumpEvents(void);
void __wrap_SDL_PumpEvents(void);

/* HidNpadButton */
#define BTN_A       (1u << 0)
#define BTN_B       (1u << 1)
#define BTN_X       (1u << 2)
#define BTN_Y       (1u << 3)
#define BTN_STICK_L (1u << 4)
#define BTN_STICK_R (1u << 5)
#define BTN_L       (1u << 6)
#define BTN_R       (1u << 7)
#define BTN_ZL      (1u << 8)
#define BTN_ZR      (1u << 9)
#define BTN_PLUS    (1u << 10)
#define BTN_MINUS   (1u << 11)
#define BTN_LEFT    (1u << 12)
#define BTN_UP      (1u << 13)
#define BTN_RIGHT   (1u << 14)
#define BTN_DOWN    (1u << 15)

#define TOUCH_ID      ((SDL_TouchID)0x4C580001)
#define MOUSE_ID      ((SDL_MouseID)1)
#define KEYBOARD_ID   ((SDL_KeyboardID)1)
#define HID_W         1280.0f /* USB mouse coordinates */
#define HID_H         720.0f
#define TOUCH_W       1920.0f /* touch screen coordinates (lx_abi.h) */
#define TOUCH_H       1080.0f
#define POINTER_W     1920.0f /* the Joy-Con 2 mouse area (lx_input.c) */
#define POINTER_H     1080.0f
#define JOYMOUSE_HOLD_NS 750000000ull
#define CURSOR_SPEED  1800.0f /* window pixels per second at full deflection */
#define WHEEL_SPEED   12.0f   /* wheel notches per second at full deflection */
#define GYRO_PIXELS_PER_TURN 3600.0f /* default: 10 window pixels per degree */
#define KEY_REPEAT_DELAY_NS 500000000ull
#define KEY_REPEAT_RATE_NS  40000000ull

/* The gyro button is masked out when this table is used: it sends nothing to osu!. */
static const struct {
    uint32_t buttons;
    SDL_Scancode scancode;
} key_map[] = {
    { BTN_PLUS, SDL_SCANCODE_ESCAPE }, { BTN_MINUS, SDL_SCANCODE_F9 }, { BTN_X, SDL_SCANCODE_RETURN },
    { BTN_UP, SDL_SCANCODE_UP },       { BTN_DOWN, SDL_SCANCODE_DOWN }, { BTN_LEFT, SDL_SCANCODE_LEFT },
    { BTN_RIGHT, SDL_SCANCODE_RIGHT }, { BTN_ZL, SDL_SCANCODE_Z },      { BTN_ZR, SDL_SCANCODE_X },
    { BTN_L, SDL_SCANCODE_C },         { BTN_R, SDL_SCANCODE_V },       { BTN_STICK_L, SDL_SCANCODE_A },
    { BTN_STICK_R, SDL_SCANCODE_S },   { BTN_B, SDL_SCANCODE_D },
};

static const struct {
    uint32_t buttons;
    uint8_t button;
} mouse_map[] = {
    { BTN_A, SDL_BUTTON_LEFT },
    { BTN_Y, SDL_BUTTON_RIGHT },
};

static struct lx_input input;
static bool ready, host_input = true;
static uint64_t last_ns;
static float cursor_x, cursor_y; /* window coordinates */
static float wheel;              /* fractional notches */
static uint32_t keys_down;       /* bit per key_map entry */
static uint32_t mouse_down;      /* SDL_BUTTON_MASK bits pushed as down */
static uint32_t extra_buttons;   /* SDL_BUTTON_MASK bits held on a USB mouse or the Joy-Con 2 mouse */
static uint32_t prev_ids[LX_INPUT_MAX_TOUCH];
static float prev_x[LX_INPUT_MAX_TOUCH], prev_y[LX_INPUT_MAX_TOUCH];
static uint32_t prev_count;

/* Settings (Switch2Settings.cs), from the update thread: plain words the window thread reads on its next pump. */
static volatile uint32_t gyro_button = BTN_B;
static volatile float gyro_speed_x = 2.5f, gyro_speed_y = 1.5f, gyro_sign_x = 1.0f, gyro_sign_y = 1.0f;
static volatile bool kbmouse_on = true;
static volatile float mouse_speed = 1.0f;
static volatile uint32_t joymouse_sides = 3u; /* bit LX_INPUT_JOY_LEFT, bit LX_INPUT_JOY_RIGHT */
static volatile float joymouse_speed = 1.0f;
static bool joymouse_on[2];      /* in mouse mode */
static uint64_t joymouse_until[2];
static float gyro_carry_x, gyro_carry_y;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* -32767..32767 with a squared response: fine control near the centre, speed at the edge. */
static float curve(int32_t v) {
    float f = (float)v / 32767.0f;
    return f * (f < 0 ? -f : f);
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ounsu_log(line);
}

static void push_window(SDL_WindowID window, SDL_EventType type) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.window.type = type;
    e.window.timestamp = SDL_GetTicksNS();
    e.window.windowID = window;
    SDL_PushEvent(&e);
}

static void push_finger(SDL_WindowID window, SDL_EventType type, uint32_t id, float x, float y, float pressure) {
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

static void push_motion(SDL_WindowID window, float dx, float dy) {
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
    e.motion.state = mouse_down;
    SDL_PushEvent(&e);
}

static void push_button(SDL_WindowID window, uint8_t button, bool down) {
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

static void push_wheel(SDL_WindowID window, float notches) {
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
    SDL_PushEvent(&e);
}

static void push_key(SDL_WindowID window, SDL_Scancode sc, SDL_Keymod mod, bool down, bool repeat) {
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

static void move_cursor(SDL_WindowID window, int width, int height, float dx, float dy) {
    if (dx == 0.0f && dy == 0.0f) return;
    cursor_x = clampf(cursor_x + dx, 0.0f, (float)(width - 1));
    cursor_y = clampf(cursor_y + dy, 0.0f, (float)(height - 1));
    push_motion(window, dx, dy);
}

/* ---- touch screen --------------------------------------------------------------------------------------------- */

static void touch(SDL_WindowID window, int width, int height) {
    uint32_t ids[LX_INPUT_MAX_TOUCH];
    float xs[LX_INPUT_MAX_TOUCH], ys[LX_INPUT_MAX_TOUCH];
    uint32_t count = input.touch_count, i, j;

    if (count > LX_INPUT_MAX_TOUCH) count = LX_INPUT_MAX_TOUCH;
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
        if (!known) push_finger(window, SDL_EVENT_FINGER_DOWN, ids[i], xs[i], ys[i], 1.0f);
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
        if (!still) push_finger(window, SDL_EVENT_FINGER_UP, prev_ids[j], prev_x[j], prev_y[j], 0.0f);
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

/* ---- gyro cursor ----------------------------------------------------------------------------------------------
 * While the gyro button is held, the controller's rotation moves the cursor: yaw -> x, pitch -> y. The host starts
 * the sensors on the first request (LX_INPUT_GYRO) and reports the one turning fastest. */

static const char *button_name(uint32_t bit) {
    switch (bit) {
    case BTN_B: return "B";
    case BTN_L: return "L";
    case BTN_R: return "R";
    case BTN_ZL: return "ZL";
    case BTN_ZR: return "ZR";
    case BTN_STICK_L: return "L3";
    case BTN_STICK_R: return "R3";
    default: return "nothing (off)";
    }
}

/* button: 0 none, 1 B, 2 L, 3 R, 4 ZL, 5 ZR, 6 L3, 7 R3. */
void ounsu_input_set_gyro(int button, float speed_x, float speed_y, int invert_x, int invert_y) {
    static const uint32_t bits[] = { 0, BTN_B, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_STICK_L, BTN_STICK_R };
    if (button >= 0 && button < (int)(sizeof bits / sizeof bits[0])) gyro_button = bits[button];
    if (speed_x > 0.05f && speed_x < 20.0f) gyro_speed_x = speed_x;
    if (speed_y > 0.05f && speed_y < 20.0f) gyro_speed_y = speed_y;
    gyro_sign_x = invert_x ? -1.0f : 1.0f;
    gyro_sign_y = invert_y ? -1.0f : 1.0f;
    logf_("lx-input: gyro cursor on %s (speed x %.2f y %.2f%s%s) [settings]", button_name(gyro_button),
          (double)gyro_speed_x, (double)gyro_speed_y, gyro_sign_x < 0 ? ", x inverted" : "",
          gyro_sign_y < 0 ? ", y inverted" : "");
}

static bool gyro_held(void) {
    return gyro_button && (input.buttons & gyro_button);
}

static void gyro(SDL_WindowID window, int width, int height, float dt) {
    static int log_left = 8;
    if (!gyro_held() || !input.gyro_sensors) {
        gyro_carry_x = gyro_carry_y = 0.0f;
        return;
    }
    float wx = input.gyro[0], wz = input.gyro[2];
    if (log_left > 0 && wx * wx + input.gyro[1] * input.gyro[1] + wz * wz > 0.01f) {
        log_left--;
        logf_("lx-input: gyro angular velocity %.3f %.3f %.3f (turns/s)", (double)wx, (double)input.gyro[1],
              (double)wz);
    }
    gyro_carry_x += -wz * GYRO_PIXELS_PER_TURN * gyro_speed_x * gyro_sign_x * dt;
    gyro_carry_y += -wx * GYRO_PIXELS_PER_TURN * gyro_speed_y * gyro_sign_y * dt;
    float dx = (float)(int)gyro_carry_x, dy = (float)(int)gyro_carry_y;
    gyro_carry_x -= dx;
    gyro_carry_y -= dy;
    move_cursor(window, width, height, dx, dy);
}

/* ---- controller ------------------------------------------------------------------------------------------------ */

static void pad(SDL_WindowID window, int width, int height, float dt) {
    uint32_t buttons = (uint32_t)input.buttons;
    size_t i;

    /* The right stick scrolls; the left one moves the cursor, or scrolls while the left Joy-Con is a mouse. The Nmouse
       wheel isn't used: one small stick push gives a large value (volume straight to 0 or 100%). */
    bool left_scrolls = joymouse_on[LX_INPUT_JOY_LEFT];
    if (!left_scrolls)
        move_cursor(window, width, height, curve(input.stick_lx) * CURSOR_SPEED * dt,
                    -curve(input.stick_ly) * CURSOR_SPEED * dt); /* hid: +y is up */

    bool scrolling = input.stick_ry || (left_scrolls && input.stick_ly);
    wheel += (curve(input.stick_ry) + (left_scrolls ? curve(input.stick_ly) : 0.0f)) * WHEEL_SPEED * dt;
    if (wheel >= 1.0f || wheel <= -1.0f) {
        float notches = (float)(int)wheel;
        push_wheel(window, notches);
        wheel -= notches;
    } else if (!scrolling) {
        wheel = 0.0f;
    }

    /* Pad clicks, USB mouse and Joy-Con 2 mouse buttons drive the same SDL buttons (bit b-1 = SDL button b). */
    uint32_t want = extra_buttons;
    for (i = 0; i < sizeof mouse_map / sizeof mouse_map[0]; i++)
        if (buttons & mouse_map[i].buttons) want |= SDL_BUTTON_MASK(mouse_map[i].button);
    for (uint8_t b = 1; b <= 5; b++) {
        bool down = (want & SDL_BUTTON_MASK(b)) != 0;
        if (down != ((mouse_down & SDL_BUTTON_MASK(b)) != 0)) {
            mouse_down ^= SDL_BUTTON_MASK(b);
            push_button(window, b, down);
        }
    }

    /* The gyro button sends no key, nor do a Joy-Con's shoulder buttons while it is a mouse and clicks with them. */
    uint32_t mask = gyro_button | (joymouse_on[LX_INPUT_JOY_LEFT] ? BTN_L | BTN_ZL : 0) |
                    (joymouse_on[LX_INPUT_JOY_RIGHT] ? BTN_R | BTN_ZR : 0);
    for (i = 0; i < sizeof key_map / sizeof key_map[0]; i++) {
        bool down = (buttons & key_map[i].buttons & ~mask) != 0;
        if (down != ((keys_down >> i) & 1u)) {
            push_key(window, key_map[i].scancode, SDL_KMOD_NONE, down, false);
            keys_down ^= 1u << i;
        }
    }
}

/* ---- USB keyboard and mouse, Joy-Con 2 mouse ------------------------------------------------------------------- */

static uint8_t keys_prev[32];
static bool keyboard_seen, mouse_seen;
static SDL_Scancode repeat_key;
static uint64_t repeat_at;

/* "Switch 2" settings: USB keyboard/mouse on or off, and the mouse speed. LX_NO_KBMOUSE=1 keeps them off. */
void ounsu_input_set_kbmouse(int enabled, float speed) {
    const char *e = getenv("LX_NO_KBMOUSE");
    kbmouse_on = enabled != 0 && !(e && *e == '1');
    if (speed > 0.05f && speed < 20.0f) mouse_speed = speed;
    logf_("lx-input: USB keyboard/mouse %s, mouse speed %.2f [settings]", kbmouse_on ? "on" : "off",
          (double)mouse_speed);
}

/* "Switch 2" settings: which Joy-Cons may be a mouse (sides: bit 0 left, bit 1 right) and the mouse speed. */
void ounsu_input_set_joymouse(int sides, float speed) {
    static const char *names[] = { "off", "left Joy-Con only", "right Joy-Con only", "both Joy-Cons" };
    joymouse_sides = (uint32_t)sides & 3u;
    if (speed > 0.05f && speed < 20.0f) joymouse_speed = speed;
    logf_("lx-input: Joy-Con mouse %s, speed %.2f [settings]", names[joymouse_sides], (double)joymouse_speed);
}

/* US layout. Returns 0 for keys that don't type anything. */
static char key_char(SDL_Scancode sc, bool shift, bool caps, bool num) {
    static const char digits[] = "1234567890", shifted[] = "!@#$%^&*()";
    static const char punct[] = "-=[]\\#;'`,./", punct_shift[] = "_+{}|~:\"~<>?";

    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        char c = (char)('a' + (sc - SDL_SCANCODE_A));
        return (shift != caps) ? (char)(c - 32) : c;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return (shift ? shifted : digits)[sc - SDL_SCANCODE_1];
    if (sc == SDL_SCANCODE_SPACE) return ' ';
    if (sc >= SDL_SCANCODE_MINUS && sc <= SDL_SCANCODE_SLASH)
        return (shift ? punct_shift : punct)[sc - SDL_SCANCODE_MINUS];
    switch (sc) {
    case SDL_SCANCODE_KP_DIVIDE: return '/';
    case SDL_SCANCODE_KP_MULTIPLY: return '*';
    case SDL_SCANCODE_KP_MINUS: return '-';
    case SDL_SCANCODE_KP_PLUS: return '+';
    default: break;
    }
    if (num && sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0) return digits[sc - SDL_SCANCODE_KP_1];
    if (num && sc == SDL_SCANCODE_KP_PERIOD) return '.';
    return 0;
}

static void push_text(SDL_Window *win, SDL_WindowID window, char c) {
    /* SDL leaves pushed text alone unless it came from its own temporary pool (internal API), so point at a ring of
       static strings: osu-framework drains the queue right after each pump. */
    static char ring[64][2];
    static unsigned next;
    SDL_Event e;
    char *s;
    if (!c || !SDL_TextInputActive(win)) return;
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

static void keyboard(SDL_Window *win, SDL_WindowID window, uint64_t now) {
    uint8_t keys[32];
    uint32_t mods = kbmouse_on ? input.kb_modifiers : 0;
    SDL_Keymod mod = SDL_KMOD_NONE;

    if (kbmouse_on) memcpy(keys, input.kb_keys, sizeof keys);
    else memset(keys, 0, sizeof keys); /* switched off: release everything */

#define KEY_ON(sc) ((keys[(sc) >> 3] >> ((sc) & 7)) & 1)
#define KEY_SET(sc) (keys[(sc) >> 3] |= (uint8_t)(1u << ((sc) & 7)))
    /* HidKeyboardModifier: 0 Control, 1 Shift, 2 LeftAlt, 3 RightAlt, 4 Gui, 8 CapsLock, 10 NumLock. A held modifier
       also shows up as a key, whether or not the key set carries it. */
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
    bool shift = (mod & SDL_KMOD_SHIFT) != 0, caps = (mod & SDL_KMOD_CAPS) != 0, num = (mod & SDL_KMOD_NUM) != 0;

    /* Usage 0-3 are error codes, not keys. */
    for (int sc = 4; sc < 256; sc++) {
        bool down = KEY_ON(sc), was = (keys_prev[sc >> 3] >> (sc & 7)) & 1;
        if (down == was) continue;
        if (down && !keyboard_seen) {
            keyboard_seen = true;
            ounsu_log("lx-input: USB keyboard in use");
        }
        push_key(window, (SDL_Scancode)sc, mod, down, false);
        if (down) {
            /* Ctrl/Alt/Gui shortcuts don't type. */
            if (!(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
                push_text(win, window, key_char((SDL_Scancode)sc, shift, caps, num));
            repeat_key = (SDL_Scancode)sc;
            repeat_at = now + KEY_REPEAT_DELAY_NS;
        } else if (repeat_key == (SDL_Scancode)sc) {
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

/* HidMouseButton: 0 left, 1 right, 2 middle, 3 forward, 4 back. */
static uint32_t sdl_buttons(uint32_t hid) {
    uint32_t m = 0;
    if (hid & 1u) m |= SDL_BUTTON_LMASK;
    if (hid & 2u) m |= SDL_BUTTON_RMASK;
    if (hid & 4u) m |= SDL_BUTTON_MMASK;
    if (hid & 8u) m |= SDL_BUTTON_X2MASK;
    if (hid & 16u) m |= SDL_BUTTON_X1MASK;
    return m;
}

static void mice(SDL_WindowID window, int width, int height, uint64_t now) {
    extra_buttons = 0;

    if (kbmouse_on && input.mouse_connected) {
        if (!mouse_seen) {
            mouse_seen = true;
            ounsu_log("lx-input: USB mouse connected");
        }
        /* hid works in 1280x720 screen space. */
        move_cursor(window, width, height, (float)input.mouse_dx * mouse_speed * (float)width / HID_W,
                    (float)input.mouse_dy * mouse_speed * (float)height / HID_H);
        float notches = (float)input.mouse_wheel;
        if (notches >= 60.0f || notches <= -60.0f) notches /= 120.0f; /* some stacks report 120 per notch */
        if (notches != 0.0f) push_wheel(window, notches);
        extra_buttons |= sdl_buttons(input.mouse_buttons);
    }

    for (int side = 0; side < 2; side++) {
        static const char *names[] = { "left", "right" };
        const __typeof__(input.joymouse[0]) *jm = &input.joymouse[side];
        int32_t stick_x = side ? input.stick_rx : input.stick_lx, stick_y = side ? input.stick_ry : input.stick_ly;
        bool was = joymouse_on[side];

        if (!(joymouse_sides & (1u << side)) || !jm->present) {
            joymouse_on[side] = false;
        } else {
            bool moved = jm->tracked && (jm->dx || jm->dy);
            if (moved) joymouse_on[side] = true;
            /* Still on the surface, scrolling or clicking keeps a mouse a mouse. */
            if (joymouse_on[side] && (moved || jm->tracked || stick_x || stick_y || jm->buttons))
                joymouse_until[side] = now + JOYMOUSE_HOLD_NS;
            if (joymouse_on[side] && now >= joymouse_until[side]) joymouse_on[side] = false;
        }
        if (joymouse_on[side] != was) logf_("lx-input: %s Joy-Con %s", names[side], joymouse_on[side] ? "is a mouse" : "is a controller");
        if (!joymouse_on[side]) continue;

        move_cursor(window, width, height, (float)jm->dx * joymouse_speed * (float)width / POINTER_W,
                    (float)jm->dy * joymouse_speed * (float)height / POINTER_H);
        extra_buttons |= sdl_buttons(jm->buttons);
    }
}

/* ---- the pump -------------------------------------------------------------------------------------------------- */

void __wrap_SDL_PumpEvents(void) {
    SDL_Window **windows, *win;
    SDL_WindowID window;
    int count = 0, width = 0, height = 0;

    __real_SDL_PumpEvents();
    if (!host_input) return;

    windows = SDL_GetWindows(&count);
    if (!windows) return;
    if (count < 1) {
        SDL_free(windows);
        return;
    }
    win = windows[0];
    window = SDL_GetWindowID(win);
    SDL_GetWindowSize(win, &width, &height);
    SDL_free(windows);
    if (width < 1 || height < 1) return;

    uint64_t now = SDL_GetTicksNS();
    if (!ready) {
        push_window(window, SDL_EVENT_WINDOW_FOCUS_GAINED);
        push_window(window, SDL_EVENT_WINDOW_MOUSE_ENTER);
        cursor_x = (float)width / 2.0f;
        cursor_y = (float)height / 2.0f;
        push_motion(window, 0.0f, 0.0f);
        last_ns = now;
        ready = true;
    }
    float dt = (float)(now - last_ns) / 1e9f;
    if (dt > 0.1f) dt = 0.1f;
    last_ns = now;

    uint32_t flags = (joymouse_sides ? LX_INPUT_POINTER : 0) | (kbmouse_on ? LX_INPUT_KBMOUSE : 0);
    if (gyro_button) flags |= LX_INPUT_GYRO; /* sensors start on the first held press (cheap after that) */
    if ((flags & LX_INPUT_GYRO) && !gyro_held() && !input.gyro_sensors) flags &= ~LX_INPUT_GYRO;
    if (!lx_input_poll(&input, flags)) {
        host_input = false;
        ounsu_log("lx-input: the host has no input entry (ABI < 4): no input");
        return;
    }
    static bool announced;
    if (!announced) {
        announced = true;
        ounsu_log("lx-input: touch screen + controllers (stick = cursor, A = click, Plus = back), USB keyboard/mouse "
                 "and Joy-Con 2 mouse active");
    }

    touch(window, width, height);
    mice(window, width, height, now);
    pad(window, width, height, dt);
    gyro(window, width, height, dt);
    keyboard(win, window, now);
}
