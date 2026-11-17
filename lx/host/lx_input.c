/*
 * Input for the island (struct lx_input, lx_abi.h), read with libounce's hid and pad APIs:
 *
 * - controllers: one PadState per Npad (No1..No8, Handheld). Buttons are ORed; each stick comes from the pad that
 *   pushes it furthest, so a centred handheld Joy-Con doesn't hide a Pro Controller's stick (HBLauncher finding).
 * - touch screen (1920x1080 coordinates).
 * - six-axis (LX_INPUT_GYRO): sensors of the handheld Joy-Cons, a Pro Controller and a Joy-Con pair on player 1,
 *   started on the first request; the one turning fastest wins.
 * - USB keyboard and mouse (LX_INPUT_KBMOUSE): initialised on the first request; mouse movement is summed over the
 *   samples since the previous poll.
 * - Joy-Con 2 mouse (LX_INPUT_POINTER): hid Nmouse, read per Joy-Con and reported per side (left, right): movement,
 *   the stick as a wheel, buttons, and whether the sensor tracked. Not libounce's PadPointer: its `active` is set
 *   whenever a Joy-Con 2 is attached, and it merges both sides, so the island could not tell a Joy-Con sliding on a
 *   table from one held in the hand (the stick then moved the cursor and scrolled at once).
 *
 * Called from one island thread (osu-framework's window thread, through the island's SDL_PumpEvents wrapper).
 */
#include <switch.h>

#include <string.h>

#include "lx_host.h"

#define NPADS 9
#define DEADZONE 4915 /* 15% of full scale, radial, like the SDK */

static int g_ready;
static PadState g_pads[NPADS];
static int g_gyro_state; /* 0 not started, 1 running, -1 none found */
static HidSixAxisSensorHandle g_gyro[6];
static int g_gyro_count;
static int g_kbmouse_ready;
static u64 g_mouse_last;
static int g_mouse_last_valid;
static int g_pointer_state; /* 0 not started, 1 running, -1 failed */
#define JOYMICE 8
static struct {
    u64 handle;
    u64 last;      /* newest sampling number read */
    int side;      /* LX_INPUT_JOY_*, -1 not a Joy-Con */
    int last_valid;
    u32 flags;     /* for the log */
} g_joy[JOYMICE];
static int g_joy_count;

static void init(void) {
    static const HidNpadIdType ids[NPADS] = {
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4, HidNpadIdType_No5,
        HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8, HidNpadIdType_Handheld,
    };
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    for (int i = 0; i < NPADS; i++) {
        padInitializeWithMask(&g_pads[i], BITL(ids[i]));
    }
    hidSetTouchScreenOutputRanges(1920, 1080);
    hidInitializeTouchScreen();
    g_ready = 1;
    lx_logf("lx-input: controllers and touch screen ready");
}

static void gyro_start(void) {
    static const struct {
        HidNpadIdType id;
        HidNpadStyleTag style;
        int count;
    } sets[] = {
        { HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 1 },
        { HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 1 },
        { HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 2 },
    };
    g_gyro_count = 0;
    for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        HidSixAxisSensorHandle h[2];
        Result rc = hidGetSixAxisSensorHandles(h, sets[i].count, sets[i].id, sets[i].style);
        if (R_FAILED(rc)) {
            lx_logf("lx-input: six-axis handles (id %d, style %#x): 0x%x", (int)sets[i].id, (unsigned)sets[i].style,
                    rc);
            continue;
        }
        for (int k = 0; k < sets[i].count; k++) {
            rc = hidStartSixAxisSensor(h[k]);
            if (R_SUCCEEDED(rc)) g_gyro[g_gyro_count++] = h[k];
            else lx_logf("lx-input: hidStartSixAxisSensor: 0x%x", rc);
        }
    }
    g_gyro_state = g_gyro_count ? 1 : -1;
    lx_logf("lx-input: %d six-axis sensor(s) started", g_gyro_count);
}

static void gyro_read(struct lx_input *out) {
    float best = 0.0f;
    for (int i = 0; i < g_gyro_count; i++) {
        HidSixAxisSensorState st;
        if (hidGetSixAxisSensorStates(g_gyro[i], &st, 1) < 1) continue;
        float x = st.angular_velocity.x, y = st.angular_velocity.y, z = st.angular_velocity.z;
        float m = x * x + y * y + z * z;
        if (m > best) {
            best = m;
            out->gyro[0] = x;
            out->gyro[1] = y;
            out->gyro[2] = z;
        }
    }
    out->gyro_sensors = (uint32_t)g_gyro_count;
}

static void kbmouse_read(struct lx_input *out) {
    if (!g_kbmouse_ready) {
        hidInitializeKeyboard();
        hidInitializeMouse();
        g_kbmouse_ready = 1;
        lx_logf("lx-input: USB keyboard and mouse ready");
    }
    HidKeyboardState kb;
    if (hidGetKeyboardStates(&kb, 1) >= 1) {
        out->kb_modifiers = (uint32_t)kb.modifiers;
        memcpy(out->kb_keys, kb.keys, sizeof out->kb_keys);
    }
    HidMouseState ms[17];
    size_t n = hidGetMouseStates(ms, 17); /* newest first */
    if (n < 1) return;
    out->mouse_connected = (ms[0].attributes & HidMouseAttribute_IsConnected) != 0;
    out->mouse_buttons = out->mouse_connected ? ms[0].buttons : 0;
    if (g_mouse_last_valid && out->mouse_connected) {
        for (size_t i = n; i-- > 0;) {
            if (ms[i].sampling_number <= g_mouse_last) continue;
            out->mouse_dx += ms[i].delta_x;
            out->mouse_dy += ms[i].delta_y;
            out->mouse_wheel += ms[i].wheel_delta_y;
        }
    }
    g_mouse_last = ms[0].sampling_number;
    g_mouse_last_valid = 1;
}

static void pointer_read(struct lx_input *out) {
    if (g_pointer_state == 0) {
        Result rc = hidInitializeNmouse(NULL, 0, 1920, 1080, HidNmousePointerType_Default);
        g_pointer_state = R_SUCCEEDED(rc) ? 1 : -1;
        lx_logf("lx-input: Joy-Con 2 mouse (Nmouse 1920x1080): 0x%x", rc);
    }
    if (g_pointer_state != 1) return;

    HidNmouseHandle handles[JOYMICE];
    s32 n = 0;
    if (R_FAILED(hidGetAllNmouseHandles(handles, JOYMICE, &n)) || n < 0) n = 0;
    if (n > JOYMICE) n = JOYMICE;

    /* Carry each known handle's state over; new handles get their type looked up once. */
    __typeof__(g_joy) next;
    memset(next, 0, sizeof next);
    for (s32 i = 0; i < n; i++) {
        int k;
        for (k = 0; k < g_joy_count; k++)
            if (g_joy[k].handle == handles[i].handle) break;
        if (k < g_joy_count) {
            next[i] = g_joy[k];
            continue;
        }
        u32 types = 0;
        hidGetNmouseTypeSet(handles[i], &types);
        next[i].handle = handles[i].handle;
        next[i].side = (types & HidNmouseType_JoyLeft) ? LX_INPUT_JOY_LEFT
                       : (types & HidNmouseType_JoyRight) ? LX_INPUT_JOY_RIGHT : -1;
        lx_logf("lx-input: Nmouse device %#llx: types %#x (%s)", (unsigned long long)handles[i].handle, types,
                next[i].side == LX_INPUT_JOY_LEFT ? "left Joy-Con" : next[i].side == LX_INPUT_JOY_RIGHT ? "right Joy-Con"
                                                                                                : "ignored");
    }
    memcpy(g_joy, next, sizeof g_joy);
    g_joy_count = (int)n;

    for (int i = 0; i < g_joy_count; i++) {
        if (g_joy[i].side < 0) continue;
        HidNmouseHandle h = { g_joy[i].handle };
        HidNmouseStateWithDelta st[16];
        size_t total = 0;
        out->joymouse[g_joy[i].side].present++;
        if (R_FAILED(hidGetNmouseStatesWithDelta(h, st, 16, &total)) || total == 0) continue;
        if (total > 16) total = 16;
        __typeof__(out->joymouse[0]) *jm = &out->joymouse[g_joy[i].side];
        if (g_joy[i].last_valid) {
            for (size_t k = 0; k < total; k++) { /* newest first */
                if (st[k].sampling_number <= g_joy[i].last) break;
                jm->samples++;
                if (st[k].flags & 2u) jm->tracked++;
                jm->dx += st[k].delta_x;
                jm->dy += st[k].delta_y;
                jm->wheel += st[k].wheel_delta_y;
            }
        }
        jm->buttons |= st[0].buttons;
        jm->flags |= st[0].flags;
        if (st[0].flags != g_joy[i].flags) {
            static int log_left = 40;
            if (log_left > 0) {
                log_left--;
                lx_logf("lx-input: Nmouse %s flags %#x -> %#x", g_joy[i].side ? "right" : "left", g_joy[i].flags,
                        st[0].flags);
            }
            g_joy[i].flags = st[0].flags;
        }
        g_joy[i].last = st[0].sampling_number;
        g_joy[i].last_valid = 1;
    }
}

void lx_input_poll(struct lx_input *out, uint32_t flags) {
    memset(out, 0, sizeof *out);
    if (!g_ready) init();

    int64_t best_l = DEADZONE * (int64_t)DEADZONE, best_r = best_l;
    for (int i = 0; i < NPADS; i++) {
        padUpdate(&g_pads[i]);
        if (!padIsConnected(&g_pads[i])) continue;
        out->buttons |= padGetButtons(&g_pads[i]);
        HidAnalogStickState l = padGetStickPos(&g_pads[i], 0), r = padGetStickPos(&g_pads[i], 1);
        int64_t ml = (int64_t)l.x * l.x + (int64_t)l.y * l.y, mr = (int64_t)r.x * r.x + (int64_t)r.y * r.y;
        if (ml > best_l) {
            best_l = ml;
            out->stick_lx = l.x;
            out->stick_ly = l.y;
        }
        if (mr > best_r) {
            best_r = mr;
            out->stick_rx = r.x;
            out->stick_ry = r.y;
        }
    }

    HidTouchScreenState ts;
    if (hidGetTouchScreenStates(&ts, 1) >= 1) {
        uint32_t n = ts.count < 0 ? 0 : (uint32_t)ts.count;
        if (n > LX_INPUT_MAX_TOUCH) n = LX_INPUT_MAX_TOUCH;
        for (uint32_t i = 0; i < n; i++) {
            out->touch[i].id = ts.touches[i].finger_id;
            out->touch[i].x = ts.touches[i].x;
            out->touch[i].y = ts.touches[i].y;
        }
        out->touch_count = n;
    }

    if (flags & LX_INPUT_GYRO) {
        if (g_gyro_state == 0) gyro_start();
        if (g_gyro_state == 1) gyro_read(out);
    }
    if (flags & LX_INPUT_KBMOUSE) kbmouse_read(out);
    if (flags & LX_INPUT_POINTER) pointer_read(out);
}

/* At exit: hbmenu (or the next program) runs in this same process, so give back what lx_input_poll started. */
void lx_input_shutdown(void) {
    for (int i = 0; i < g_gyro_count; i++) hidStopSixAxisSensor(g_gyro[i]);
    g_gyro_count = 0;
    g_gyro_state = 0;
    if (g_pointer_state == 1) hidFinalizeNmouse();
    g_pointer_state = 0;
    g_joy_count = 0;
    g_ready = 0;
}
