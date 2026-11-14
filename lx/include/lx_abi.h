/*
 * lx_abi.h - the boundary between the musl island and its libounce host.
 *
 * The island (osu! or a probe, plus musl libc, all built with the aarch64-linux-musl toolchain) is one relocatable
 * object whose only global symbol is lx_island_main. It reaches the host only through the function pointers in
 * struct lx_host. The host (built with devkitA64 against libounce/newlib) fills the struct and calls
 * lx_island_main. Nothing here may depend on either libc: only fixed-size integers and pointers.
 *
 * Syscall numbers, struct layouts and errno values are Linux aarch64's.
 */
#ifndef LX_ABI_H
#define LX_ABI_H

#include <stddef.h>
#include <stdint.h>

#define LX_ABI_VERSION 6

/* Private syscall numbers (above Linux's range), used by the island runtime only. */
#define LX_SYS_UNMAPSELF 0x10000 /* (base, size): unmap the calling thread's stack and end the thread */

/* Input, as libnx reads it (lx/host/lx_input.c). Plain fields only; the island turns them into SDL events. */
#define LX_INPUT_MAX_TOUCH 10
#define LX_INPUT_GYRO 1u    /* flag: start and read the six-axis sensors */
#define LX_INPUT_KBMOUSE 2u /* flag: read USB keyboard and mouse */
#define LX_INPUT_POINTER 4u /* flag: read the Joy-Con 2 mouse sensors (hid Nmouse), per side */
#define LX_INPUT_JOY_LEFT 0  /* joymouse[] index */
#define LX_INPUT_JOY_RIGHT 1

struct lx_input {
    uint64_t buttons;                                   /* HidNpadButton bits, every connected controller ORed */
    int32_t stick_lx, stick_ly, stick_rx, stick_ry;     /* -32767..32767, +y up; per stick, the pad pushed furthest */
    uint32_t touch_count;
    struct {
        uint32_t id, x, y;                              /* x, y in 1280x720 */
    } touch[LX_INPUT_MAX_TOUCH];
    float gyro[3];                                      /* angular velocity (turns/s) of the sensor turning fastest */
    uint32_t gyro_sensors;                              /* sensors started (0: none or not requested) */
    uint32_t kb_modifiers;                              /* HidKeyboardModifier */
    uint8_t kb_keys[32];                                /* held keys, bit per USB HID usage (= SDL_Scancode) */
    uint32_t mouse_connected;
    int32_t mouse_dx, mouse_dy, mouse_wheel;            /* summed since the previous poll */
    uint32_t mouse_buttons;                             /* HidMouseButton */
    struct {                                            /* Joy-Con 2 mouse, [LX_INPUT_JOY_LEFT] and [LX_INPUT_JOY_RIGHT] */
        uint32_t present;                               /* number of such Joy-Cons with a mouse sensor */
        uint32_t samples;                               /* new samples since the previous poll */
        uint32_t tracked;                               /* new samples with Nmouse flag bit 1 (sensor tracked) */
        uint32_t flags;                                 /* Nmouse flags of the newest sample */
        int32_t dx, dy;                                 /* movement in 1920x1080 pointer pixels, summed */
        int32_t wheel;                                  /* the Joy-Con's stick as a wheel, summed, positive upward */
        uint32_t buttons;                               /* HidMouseButton of the newest sample (L/ZL or R/ZR: left) */
    } joymouse[2];
};

struct lx_host {
    uint32_t version; /* LX_ABI_VERSION */
    uint32_t size;    /* sizeof(struct lx_host) */

    /* Every Linux syscall the island makes. Returns the result or -errno (Linux numbers). */
    long (*syscall)(long n, long a, long b, long c, long d, long e, long f);
    /* clone() as musl's __clone calls it: start fn(arg) on a new thread with sp = stack and tpidr_el0 = tls.
       Writes the new tid to *ptid before the thread starts; on thread exit stores 0 to *ctid and wakes it
       (CLONE_PARENT_SETTID / CLONE_CHILD_CLEARTID semantics). Returns the tid or -errno. */
    long (*clone)(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *tls, int *ctid);
    /* A log line from the island runtime itself (not the program's stdout). */
    void (*log)(const char *text, size_t len);

    /* Vulkan (OunceVK), NULL when the host is built without it: the loader entry point (a wrapper that tracks open
       instances and devices), and the console's default window for VK_NN_vi_surface (an NWindow *). */
    void *(*vk_get_instance_proc_addr)(void *instance, const char *name);
    void *vi_window;

    /* Name resolution through the console's resolver: IPv4 addresses (network byte order) for a host name. Returns
       how many, 0 if the name doesn't resolve, -1 if the resolver failed. */
    int (*resolve_ipv4)(const char *name, uint32_t *out, int max);

    /* Image layout of the final .nro, for musl's static TLS (PT_TLS) and the unwinder (PT_GNU_EH_FRAME). */
    uintptr_t tls_image, tls_filesz, tls_memsz, tls_align;
    uintptr_t eh_frame_hdr, eh_frame_hdr_size;
    uintptr_t text_start, text_end, data_start, data_end;

    /* The program's command line and environment (NULL-terminated arrays, owned by the host). */
    int argc;
    char *const *argv;
    char *const *envp;
    const uint8_t *random16; /* 16 random bytes for AT_RANDOM */

    /* Version 4. Controllers, touch screen, six-axis, USB keyboard/mouse and the Joy-Con 2 mouse. flags: LX_INPUT_*.
       Call it from one thread. */
    void (*input_poll)(struct lx_input *out, uint32_t flags);

    /* Version 5. Audio output (libnx audout, lx/host/lx_audio.c): 48 kHz, stereo, s16. audio_open allocates
       `buffers` host buffers of `period` frames and starts the output; returns 0 or a negative error. audio_submit
       copies up to one period into a free buffer and queues it: 1 queued, 0 every buffer is still playing, < 0 error.
       audio_queued: buffers still playing. One output at a time; calls must not overlap. */
    int (*audio_open)(uint32_t period, uint32_t buffers);
    int (*audio_submit)(const void *pcm, uint32_t bytes);
    int (*audio_queued)(void);
    void (*audio_close)(void);
};

/* Exported by the island. Runs the program on the given main stack and returns its exit status when the program
   exits (exit_group from the main thread). The host's callee-saved registers, sp and tpidr_el0 are restored. */
int lx_island_main(const struct lx_host *host, void *stack_lo, size_t stack_size);

/* Linux errno values used by the host (newlib's differ). */
#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_EINTR 4
#define LX_EIO 5
#define LX_EBADF 9
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EFAULT 14
#define LX_EINVAL 22
#define LX_ENOTTY 25
#define LX_ESPIPE 29
#define LX_ENOSYS 38
#define LX_ETIMEDOUT 110

#endif
