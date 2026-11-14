/*
 * Input for the island (struct lx_input, lx_abi.h), read with libounce's hid and pad APIs:
 *
 * - controllers: one PadState per Npad (No1..No8, Handheld). Buttons are ORed; each stick comes from the pad that
 *   pushes it furthest, so a centred handheld Joy-Con doesn't hide a Pro Controller's stick (HBLauncher finding).
 * - touch screen (1280x720 coordinates).
 * - six-axis (LX_INPUT_GYRO): sensors of the handheld Joy-Cons, a Pro Controller and a Joy-Con pair on player 1,
 *   started on the first request; the one turning fastest wins.
 * - USB keyboard and mouse (LX_INPUT_KBMOUSE): initialised on the first request; mouse movement is summed over the
 *   samples since the previous poll.
 * - Joy-Con 2 mouse (LX_INPUT_POINTER): libounce's PadPointer over Nmouse, a 1920x1080 pointer area.
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
static PadState g_all;
static int g_gyro_state; /* 0 not started, 1 running, -1 none found */
static HidSixAxisSensorHandle g_gyro[6];
static int g_gyro_count;
static int g_kbmouse_ready;
static u64 g_mouse_last;
static int g_mouse_last_valid;
static int g_pointer_state; /* 0 not started, 1 running, -1 failed */
static PadPointer g_pointer;

static void init(void) {
    static const HidNpadIdType ids[NPADS] = {
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4, HidNpadIdType_No5,
        HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8, HidNpadIdType_Handheld,
    };
    u64 all = 0;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    for (int i = 0; i < NPADS; i++) {
        padInitializeWithMask(&g_pads[i], BITL(ids[i]));
        all |= BITL(ids[i]);
    }
    padInitializeWithMask(&g_all, all);
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
        Result rc = padPointerInitialize(&g_pointer, 1920, 1080);
        g_pointer_state = R_SUCCEEDED(rc) ? 1 : -1;
        lx_logf("lx-input: Joy-Con 2 mouse (PadPointer 1920x1080): 0x%x", rc);
    }
    if (g_pointer_state != 1) return;
    padPointerUpdate(&g_pointer, &g_all);
    out->pointer_active = g_pointer.active;
    if (!g_pointer.active) return;
    out->pointer_x = g_pointer.x;
    out->pointer_y = g_pointer.y;
    out->pointer_wheel = g_pointer.wheel_delta_y;
    out->pointer_buttons = padPointerGetButtons(&g_pointer);
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
    padUpdate(&g_all);

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
