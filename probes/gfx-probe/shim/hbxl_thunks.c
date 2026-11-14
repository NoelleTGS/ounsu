/* NativeAOT delegate thunks on hbxl: hands the prebuilt block from hbxl_thunks.S to the
 * runtime's thunk pool instead of letting it build one in non-executable memory.
 * Link with: hbxl_thunks.S, -Wl,--defsym=HBXL_THUNKS=1, -Wl,--wrap=RhAllocateThunksMapping,
 * and manifest "data_base": "after_code".  One block = 2040 thunks (one per marshalled
 * delegate instance); a second request fails with E_OUTOFMEMORY, which the managed side
 * turns into an OutOfMemoryException rather than a silent crash. */
#include <stdint.h>

void hbxl_log(const char *text);
extern char __hbxl_thunks_start[];
extern char __hbxl_thunk_data_start[];

int32_t __wrap_RhAllocateThunksMapping(void **out)
{
    static volatile int taken;
    if (__atomic_exchange_n(&taken, 1, __ATOMIC_ACQ_REL)) {
        hbxl_log("hbxl-thunks: a second thunk block was requested; none left (E_OUTOFMEMORY)");
        return (int32_t)0x8007000e;
    }
    if (__hbxl_thunk_data_start != __hbxl_thunks_start + 0x8000) {
        hbxl_log("hbxl-thunks: thunk data is not 0x8000 after the thunk code; refusing");
        return (int32_t)0x8007000e;
    }
    *out = __hbxl_thunks_start;
    hbxl_log("hbxl-thunks: delegate thunk block handed out (prebuilt, executable)");
    return 0;
}
