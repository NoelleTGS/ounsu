// SPDX-License-Identifier: MIT
// usbhs-probe: does Switch 2's usb:hs open in two steps, like usb:ds does since 11.0.0?
//
// usb:ds on 11.0.0+ (and on Switch 2, as ouncemtp and libounce issue #2 show): root cmd 0 takes no arguments and
// returns the real service object; BindDevice etc. go to that object. Our first usb:hs probe (FEASIBILITY.md
// 2026-11-14) sent BindClientProcess straight to the root session and never tried a plain argument-free cmd 0.
//
// A handful of well-formed requests, no scan: the usb sysmodule crashes the console on some bad requests (our run 8;
// kaihen#12's RegisterInterface loop past 23). Each step waits for A (B or + skips the rest), every line is
// committed to sdmc:/usbhs-probe.log before the next request, and the probe stops at the first refusal.
//   1. root: smGetService, ConvertToDomain, cmd 0 (no arguments) -> object
//   2. object cmd 0: BindClientProcess(pseudo process handle); if refused, once more on a fresh session with the
//      real process handle from hbloader
//   3. object cmd 6: GetInterfaceStateChangeEvent
//   4. object cmd 1 / 2: QueryAllInterfaces / QueryAvailableInterfaces (empty filter), printed
// Nothing is acquired or read: that is the next probe, once these commands are known.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

static FILE *logf;
static PadState pad;

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (logf) {
        va_start(ap, fmt);
        vfprintf(logf, fmt, ap);
        va_end(ap);
        fflush(logf);
        fsync(fileno(logf)); // the console may die on the next request: commit the line to the card first
    }
    fflush(stdout);
}

// "ok", "closed" (0xF601: the server refused the request's id or layout), or module-description.
static const char *rcs(Result rc)
{
    static char buf[4][32];
    static int n;
    char *b = buf[n++ & 3];
    if (R_SUCCEEDED(rc))
        snprintf(b, 32, "ok");
    else if (rc == 0xF601)
        snprintf(b, 32, "closed (0xf601)");
    else
        snprintf(b, 32, "0x%x (%04u-%04u)", rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return b;
}

// Waits for A (true) or B / + (false).
static bool confirm(const char *what)
{
    out("next: %s  -- A = do it, B = stop\n", what);
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & HidNpadButton_A)
            return true;
        if (d & (HidNpadButton_B | HidNpadButton_Plus))
            return false;
        svcSleepThread(10 * 1000 * 1000ULL);
    }
    return false;
}

static void print_iface(const char *what, const UsbHsInterface *it)
{
    const struct usb_interface_descriptor *id = &it->inf.interface_desc;
    out("%s id=%d path=%.64s vid:pid=%04x:%04x dev class=%02x/%02x/%02x  if#%u alt%u class=%02x/%02x/%02x eps=%u\n",
        what, it->inf.ID, it->pathstr, it->device_desc.idVendor, it->device_desc.idProduct,
        it->device_desc.bDeviceClass, it->device_desc.bDeviceSubClass, it->device_desc.bDeviceProtocol,
        id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass,
        id->bInterfaceProtocol, id->bNumEndpoints);
    for (int i = 0; i < 15; i++) {
        const struct usb_endpoint_descriptor *e = &it->inf.input_endpoint_descs[i];
        if (e->bLength)
            out("    IN  ep=%02x attr=%02x maxpkt=%u interval=%u\n", e->bEndpointAddress, e->bmAttributes,
                e->wMaxPacketSize, e->bInterval);
        e = &it->inf.output_endpoint_descs[i];
        if (e->bLength)
            out("    OUT ep=%02x attr=%02x maxpkt=%u interval=%u\n", e->bEndpointAddress, e->bmAttributes,
                e->wMaxPacketSize, e->bInterval);
    }
}

// Step 1 on a fresh session: root + the object from root cmd 0.
static Result open_root(Service *root, Service *obj)
{
    Result rc = smGetService(root, "usb:hs");
    out("[1] smGetService(usb:hs): %s\n", rcs(rc));
    if (R_FAILED(rc))
        return rc;
    rc = serviceConvertToDomain(root);
    out("[1] ConvertToDomain: %s\n", rcs(rc));
    if (R_SUCCEEDED(rc)) {
        serviceAssumeDomain(root);
        rc = serviceDispatch(root, 0, .out_num_objects = 1, .out_objects = obj);
        out("[1] root cmd 0 (no arguments, object out): %s\n", rcs(rc));
    }
    if (R_FAILED(rc))
        serviceClose(root);
    return rc;
}

static void close_root(Service *root, Service *obj)
{
    serviceAssumeDomain(obj);
    serviceClose(obj);
    serviceClose(root);
}

static Result bind(Service *obj, Handle proc, const char *what)
{
    serviceAssumeDomain(obj);
    Result rc = serviceDispatch(obj, 0, .in_num_handles = 1, .in_handles = {proc});
    out("[2] object cmd 0 BindClientProcess(%s): %s\n", what, rcs(rc));
    return rc;
}

#define MAX_IFACES 16
static UsbHsInterface ifaces[MAX_IFACES];

static void query(Service *obj, u32 cmd, const char *what)
{
    UsbHsInterfaceFilter filter = {0};
    s32 n = 0;
    memset(ifaces, 0, sizeof(ifaces));
    serviceAssumeDomain(obj);
    Result rc = serviceDispatchInOut(obj, cmd, filter, n, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                     .buffers = {{ifaces, sizeof(ifaces)}});
    out("[4] object cmd %u %s: %s, %d interface(s)\n", cmd, what, rcs(rc), R_SUCCEEDED(rc) ? n : 0);
    for (s32 i = 0; R_SUCCEEDED(rc) && i < n && i < MAX_IFACES; i++)
        print_iface("   ", &ifaces[i]);
}

static void run(void)
{
    Service root, obj;
    Result rc;

    if (!confirm("step 1: root cmd 0 with no arguments"))
        return;
    if (R_FAILED(open_root(&root, &obj))) {
        out("=> usb:hs has no argument-free root cmd 0 (or wants another shape). Stopping.\n");
        return;
    }
    out("=> usb:hs opens in two steps, like usb:ds.\n");

    if (!confirm("step 2: BindClientProcess on the returned object")) {
        close_root(&root, &obj);
        return;
    }
    rc = bind(&obj, CUR_PROCESS_HANDLE, "pseudo handle");
    Handle real = envGetOwnProcessHandle();
    if (R_FAILED(rc) && real != INVALID_HANDLE && real != CUR_PROCESS_HANDLE) {
        close_root(&root, &obj); // a refused request closes the session: start over once
        if (!confirm("step 2b: fresh session, BindClientProcess with the real process handle"))
            return;
        if (R_FAILED(open_root(&root, &obj)))
            return;
        rc = bind(&obj, real, "real handle");
    }
    if (R_FAILED(rc)) {
        out("=> bind refused. Stopping.\n");
        if (rc != 0xF601)
            close_root(&root, &obj);
        else
            serviceClose(&root);
        return;
    }

    if (!confirm("step 3: GetInterfaceStateChangeEvent (cmd 6)")) {
        close_root(&root, &obj);
        return;
    }
    Handle ev = INVALID_HANDLE;
    serviceAssumeDomain(&obj);
    rc = serviceDispatch(&obj, 6, .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &ev);
    out("[3] object cmd 6 GetInterfaceStateChangeEvent: %s\n", rcs(rc));
    if (R_SUCCEEDED(rc))
        svcCloseHandle(ev);
    else if (rc == 0xF601) {
        out("=> cmd 6 moved. Stopping (session is closed).\n");
        serviceClose(&root);
        return;
    }

    if (!confirm("step 4: QueryAllInterfaces (cmd 1), then QueryAvailableInterfaces (cmd 2)")) {
        close_root(&root, &obj);
        return;
    }
    query(&obj, 1, "QueryAllInterfaces");
    query(&obj, 2, "QueryAvailableInterfaces");
    close_root(&root, &obj);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    socketInitializeDefault(); // nxlinkStdio needs sockets first
    nxlinkStdio();
    logf = fopen("sdmc:/usbhs-probe.log", "w");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    out("usbhs-probe start (hos %u.%u.%u); plug the tablet in now if it isn't\n", HOSVER_MAJOR(hosversionGet()),
        HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()));
    run();
    out("usbhs-probe end\n");
    if (logf)
        fclose(logf);
    socketExit();
    return 0;
}
