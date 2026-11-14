/*
 * BASS device list for the console build (see tools/gen-bass-stubs.py for the rest).
 *
 * osu-framework asserts that BASS always lists its two internal devices, "No sound" (0)
 * and "Default" (1) (AudioManager.syncAudioDevices), so report exactly those.
 * BASS_Init still fails for both, so the framework logs it and runs silent.
 */
#include <stdint.h>

typedef struct {
    const char *name;
    const char *driver;
    uint32_t flags;          /* BASS_DEVICE_ENABLED 1, _DEFAULT 2, _INIT 4 */
} bass_device_info;

int BASS_GetDeviceInfo(uint32_t device, bass_device_info *info);
int BASS_GetDeviceInfo(uint32_t device, bass_device_info *info) {
    if (device > 1 || !info) return 0;
    info->name = device == 0 ? "No sound" : "Default";
    info->driver = 0;
    info->flags = device == 0 ? 1u : 3u;
    return 1;
}
