// usb-probe: can a kaiHEN homebrew read a USB drawing tablet?
//
// 1. usbHsInitialize (is usb:hs reachable from the game process at all?)
// 2. Lists every USB interface (all / available / acquired) with VID:PID, class and endpoints.
// 3. Acquires each available interface that has an interrupt IN endpoint, reads its HID report
//    descriptor and prints every report that changes, for ~30 s (move the pen over the tablet).
// 4. Prints hid mouse state changes at the same time, to see whether hid already turns the tablet
//    into a mouse.
// Output goes to nxlink and to sdmc:/usb-probe.log (synced after every line). Press + to stop early.
// Each risky step waits for A first (B skips it), so a console crash points at one call.
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

static FILE *logf;

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
        fsync(fileno(logf)); // the console may die on the next call: commit the line to the card
    }
    fflush(stdout);
}

static void hexdump(const char *prefix, const u8 *p, u32 n)
{
    char line[3 * 64 + 1];
    for (u32 off = 0; off < n; off += 32) {
        u32 len = n - off < 32 ? n - off : 32;
        char *w = line;
        for (u32 i = 0; i < len; i++)
            w += sprintf(w, "%02x ", p[off + i]);
        out("%s%s\n", prefix, line);
    }
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

#define MAX_IFACES 16

static UsbHsInterface ifaces[MAX_IFACES];

static s32 query(const char *what, int kind, const UsbHsInterfaceFilter *f)
{
    s32 n = 0;
    Result rc;
    memset(ifaces, 0, sizeof(ifaces));
    if (kind == 0)
        rc = usbHsQueryAllInterfaces(f, ifaces, sizeof(ifaces), &n);
    else if (kind == 1)
        rc = usbHsQueryAvailableInterfaces(f, ifaces, sizeof(ifaces), &n);
    else
        rc = usbHsQueryAcquiredInterfaces(ifaces, sizeof(ifaces), &n);
    out("%s: rc=0x%x n=%d\n", what, rc, n);
    if (R_FAILED(rc))
        return -1;
    for (s32 i = 0; i < n && i < MAX_IFACES; i++)
        print_iface("  ", &ifaces[i]);
    return n;
}

typedef struct {
    bool open;
    UsbHsClientIfSession s;
    UsbHsClientEpSession ep;
    u8 *buf;
    u8 last[64];
    u32 last_len;
    u32 reports;
    char name[32];
} Reader;

static Reader readers[MAX_IFACES];
static int nreaders;

static PadState pad;

// Waits for A (true) or B (false); + also answers false.
static bool confirm(const char *what)
{
    out("next: %s  -- A = do it, B = skip\n", what);
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

static void open_reader(const UsbHsInterface *it)
{
    const struct usb_endpoint_descriptor *in = NULL;
    for (int i = 0; i < 15; i++) {
        const struct usb_endpoint_descriptor *e = &it->inf.input_endpoint_descs[i];
        if (e->bLength && (e->bmAttributes & 3) == 3) {
            in = e;
            break;
        }
    }
    if (!in || nreaders >= MAX_IFACES)
        return;
    Reader *r = &readers[nreaders];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%04x:%04x if%u", it->device_desc.idVendor, it->device_desc.idProduct,
             it->inf.interface_desc.bInterfaceNumber);
    char q[64];
    snprintf(q, sizeof(q), "acquire %s", r->name);
    if (!confirm(q))
        return;
    UsbHsInterface copy = *it;
    Result rc = usbHsAcquireUsbIf(&r->s, &copy);
    out("acquire %s: rc=0x%x\n", r->name, rc);
    if (R_FAILED(rc))
        return;

    r->buf = memalign(0x1000, 0x1000);
    u8 ifno = it->inf.interface_desc.bInterfaceNumber;

    // HID report descriptor: GET_DESCRIPTOR(report) to the interface. Its length is in the HID
    // descriptor, which we don't have here; ask for 1024 and take what comes.
    if (it->inf.interface_desc.bInterfaceClass == 3 && confirm("read the HID report descriptor")) {
        u32 got = 0;
        memset(r->buf, 0, 0x1000);
        rc = usbHsIfCtrlXfer(&r->s, 0x81, 0x06, 0x2200, ifno, 1024, r->buf, &got);
        out("  report descriptor: rc=0x%x len=%u\n", rc, got);
        if (R_SUCCEEDED(rc))
            hexdump("    ", r->buf, got);
    }

    if (!confirm("open the interrupt IN endpoint")) {
        usbHsIfClose(&r->s);
        free(r->buf);
        return;
    }
    rc = usbHsIfOpenUsbEp(&r->s, &r->ep, 1, in->wMaxPacketSize, (struct usb_endpoint_descriptor *)in);
    out("  open ep %02x: rc=0x%x\n", in->bEndpointAddress, rc);
    if (R_FAILED(rc)) {
        usbHsIfClose(&r->s);
        free(r->buf);
        return;
    }
    r->open = true;
    nreaders++;
}

// One blocking read per reader per call. usbHsEpPostBuffer waits for a report; a tablet that is not
// touched sends nothing, so use the async API with a short timeout instead.
typedef struct {
    bool pending;
    u32 xfer;
} Pending;
static Pending pend[MAX_IFACES];

static void poll_readers(void)
{
    for (int i = 0; i < nreaders; i++) {
        Reader *r = &readers[i];
        if (!r->open)
            continue;
        if (!pend[i].pending) {
            Result rc = usbHsEpPostBufferAsync(&r->ep, r->buf, r->ep.desc.wMaxPacketSize, 0, &pend[i].xfer);
            if (R_FAILED(rc)) {
                out("%s: post rc=0x%x, closing\n", r->name, rc);
                r->open = false;
                continue;
            }
            pend[i].pending = true;
        }
        Event *ev = usbHsEpGetXferEvent(&r->ep);
        if (R_FAILED(eventWait(ev, 2 * 1000 * 1000ULL)))
            continue;
        eventClear(ev);
        UsbHsXferReport rep[4];
        u32 count = 0;
        Result rc = usbHsEpGetXferReport(&r->ep, rep, 4, &count);
        pend[i].pending = false;
        if (R_FAILED(rc) || count == 0) {
            out("%s: report rc=0x%x count=%u\n", r->name, rc, count);
            continue;
        }
        if (R_FAILED(rep[0].res)) {
            out("%s: xfer res=0x%x\n", r->name, rep[0].res);
            continue;
        }
        u32 len = rep[0].transferredSize;
        if (len > 64)
            len = 64;
        r->reports++;
        if (len != r->last_len || memcmp(r->last, r->buf, len)) {
            char prefix[48];
            snprintf(prefix, sizeof(prefix), "%s #%u: ", r->name, r->reports);
            hexdump(prefix, r->buf, len);
            memcpy(r->last, r->buf, len);
            r->last_len = len;
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    socketInitializeDefault();
    nxlinkStdio();
    logf = fopen("sdmc:/usb-probe.log", "w");
    out("usb-probe start\n");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeMouse();

    // usbHsInitialize step by step. Run 4: smGetService and the domain conversion work, BindClientProcess
    // (cmd 0, pseudo process handle) gets 0xf601 = kernel ConnectionClosed. smGetService("usb:hs:a") then
    // blocked forever (sm waits for names that aren't registered): that name doesn't exist here, never ask for it.
    // Run 5: every CMIF request on usb:hs (bound or not, domain or not) gets 0xf601: the server closes the session.
    // That is what a TIPC server does with a CMIF request, and libounce talks TIPC to several Switch 2 services
    // (viddec, gsv, vi2). So try TIPC: a fresh session per request, since a refused one is closed.
    if (confirm("usb:hs over TIPC")) {
        TipcService t;
        Handle h;
        Result r;
        SmServiceName name = smEncodeName("usb:hs");
        Handle proc = envGetOwnProcessHandle();

#define TIPC_OPEN()                                                     \
    (R_SUCCEEDED(r = smGetServiceOriginal(&h, name)) ? (tipcCreate(&t, h), true) \
                                                     : (out("  smGetServiceOriginal rc=0x%x\n", r), false))

        if (TIPC_OPEN()) {
            r = tipcDispatch(&t, 0, .in_num_handles = 1, .in_handles = {proc});
            out("[T1] BindClientProcess(real) rc=0x%x\n", r);
            if (R_SUCCEEDED(r)) {
                Handle ev = INVALID_HANDLE;
                r = tipcDispatch(&t, 6, .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &ev);
                out("[T1] GetInterfaceStateChangeEvent rc=0x%x\n", r);
                if (R_SUCCEEDED(r))
                    svcCloseHandle(ev);
                static UsbHsInterface tmp[MAX_IFACES];
                UsbHsInterfaceFilter f0 = {0};
                s32 cnt = 0;
                r = tipcDispatchInOut(&t, 0 + 1, f0, cnt,
                                      .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                      .buffers = {{tmp, sizeof(tmp)}});
                out("[T1] cmd1 (QueryAllInterfaces?) rc=0x%x n=%d\n", r, cnt);
                for (s32 i = 0; R_SUCCEEDED(r) && i < cnt && i < MAX_IFACES; i++)
                    print_iface("   ", &tmp[i]);
            }
            tipcClose(&t);
        }

        // No-argument requests 0..31 on fresh sessions: which ids exist at all? A TIPC server answers a known id
        // with wrong arguments with an error code, an unknown id with another one; a CMIF server closes.
        for (u32 id = 0; id < 32; id++) {
            if (!TIPC_OPEN())
                break;
            r = tipcDispatch(&t, id);
            out("[T2] id %u rc=0x%x\n", id, r);
            tipcClose(&t);
        }
#undef TIPC_OPEN
    }

    // Run 6: TIPC is closed too. The server did answer the CMIF control request (ConvertToDomain), so the session is
    // live and only commands kill it. Newer sf servers close the session when a request's layout does not match the
    // command (missing pid, wrong handle/buffer counts), so scan CMIF ids with the pid sent, with and without a
    // copy handle (our process).
    if (confirm("usb:hs CMIF scan with pid")) {
        Handle proc = envGetOwnProcessHandle();
        // Control: does a working CMIF service (set) also close on a wrong layout? cmd 1 GetAvailableLanguageCodes
        // wants an out buffer; send none. Then an unknown id (999).
        {
            Service set;
            Result r = smGetService(&set, "set");
            out("[C] set: smGetService rc=0x%x\n", r);
            if (R_SUCCEEDED(r)) {
                r = serviceDispatch(&set, 1);
                out("[C] set cmd1 without its buffer rc=0x%x\n", r);
                serviceClose(&set);
            }
            if (R_SUCCEEDED(smGetService(&set, "set"))) {
                r = serviceDispatch(&set, 999);
                out("[C] set unknown id 999 rc=0x%x\n", r);
                serviceClose(&set);
            }
        }
        for (int variant = 0; variant < 3; variant++) {
            for (u32 id = 0; id < (variant == 2 ? 16u : 64u); id++) {
                Service srv;
                Result r = smGetService(&srv, "usb:hs");
                if (R_FAILED(r)) {
                    out("  smGetService rc=0x%x\n", r);
                    break;
                }
                if (variant == 0)
                    r = serviceDispatch(&srv, id, .in_send_pid = true);
                else if (variant == 1)
                    r = serviceDispatch(&srv, id, .in_send_pid = true, .in_num_handles = 1, .in_handles = {proc});
                else {
                    u64 pid = 0;
                    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
                    r = serviceDispatchIn(&srv, id, pid, .in_send_pid = true);
                }
                if (r != 0xf601)
                    out("[S%d] id %u rc=0x%x\n", variant, id, r);
                serviceClose(&srv);
            }
            out("[S%d] done (ids not listed: 0xf601)\n", variant);
        }
    }

    // Run 8 tried a blind CMIF layout scan here (ids x data size x pid x handle x buffers): it crashed the whole
    // console during id 0. Removed; don't scan usb:hs blindly (FEASIBILITY.md 2026-11-14).

    if (confirm("manual usb:hs init steps (CMIF)")) {
        Service srv;
        Result r;

        out("own process handle: has=%d handle=0x%x\n", (envGetOwnProcessHandle() != INVALID_HANDLE && envGetOwnProcessHandle() != CUR_PROCESS_HANDLE), envGetOwnProcessHandle());

        // 1. No domain, pseudo handle.
        r = smGetService(&srv, "usb:hs");
        out("[1] smGetService rc=0x%x\n", r);
        if (R_SUCCEEDED(r)) {
            r = serviceDispatch(&srv, 0, .in_num_handles = 1, .in_handles = {CUR_PROCESS_HANDLE});
            out("[1] no domain, BindClientProcess(pseudo) rc=0x%x\n", r);
            serviceClose(&srv);
        }

        // 2. No domain, real process handle from hbloader.
        if ((envGetOwnProcessHandle() != INVALID_HANDLE && envGetOwnProcessHandle() != CUR_PROCESS_HANDLE)) {
            r = smGetService(&srv, "usb:hs");
            if (R_SUCCEEDED(r)) {
                r = serviceDispatch(&srv, 0, .in_num_handles = 1, .in_handles = {envGetOwnProcessHandle()});
                out("[2] no domain, BindClientProcess(real) rc=0x%x\n", r);
                if (R_SUCCEEDED(r)) {
                    Handle h = INVALID_HANDLE;
                    r = serviceDispatch(&srv, 6, .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &h);
                    out("[2] GetInterfaceStateChangeEvent rc=0x%x\n", r);
                    if (R_SUCCEEDED(r))
                        svcCloseHandle(h);
                }
                serviceClose(&srv);
            }
        }

        // 3. Domain, real process handle.
        if ((envGetOwnProcessHandle() != INVALID_HANDLE && envGetOwnProcessHandle() != CUR_PROCESS_HANDLE)) {
            r = smGetService(&srv, "usb:hs");
            if (R_SUCCEEDED(r))
                r = serviceConvertToDomain(&srv);
            if (R_SUCCEEDED(r)) {
                r = serviceDispatch(&srv, 0, .in_num_handles = 1, .in_handles = {envGetOwnProcessHandle()});
                out("[3] domain, BindClientProcess(real) rc=0x%x\n", r);
            }
            serviceClose(&srv);
        }

        // 4. Without binding: does the service answer anything? QueryAcquiredInterfaces (cmd 3) and
        // GetInterfaceStateChangeEvent (cmd 6).
        r = smGetService(&srv, "usb:hs");
        if (R_SUCCEEDED(r)) {
            static UsbHsInterface tmp[4];
            s32 cnt = 0;
            r = serviceDispatchOut(&srv, 3, cnt, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                   .buffers = {{tmp, sizeof(tmp)}});
            out("[4] unbound QueryAcquiredInterfaces rc=0x%x n=%d\n", r, cnt);
            serviceClose(&srv);
        }
        r = smGetService(&srv, "usb:hs");
        if (R_SUCCEEDED(r)) {
            Handle h = INVALID_HANDLE;
            r = serviceDispatch(&srv, 6, .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &h);
            out("[4] unbound GetInterfaceStateChangeEvent rc=0x%x\n", r);
            if (R_SUCCEEDED(r))
                svcCloseHandle(h);
            serviceClose(&srv);
        }
    }

    Result rc = confirm("usbHsInitialize") ? usbHsInitialize() : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    out("usbHsInitialize: rc=0x%x\n", rc);
    if (R_SUCCEEDED(rc)) {
        UsbHsInterfaceFilter f0 = {0};
        UsbHsInterfaceFilter fhid = {.Flags = UsbHsInterfaceFilterFlags_bInterfaceClass, .bInterfaceClass = 3};
        UsbHsInterfaceFilter fven = {.Flags = UsbHsInterfaceFilterFlags_bInterfaceClass, .bInterfaceClass = 0xff};
        if (!confirm("query interfaces"))
            goto done;
        query("all (no filter)", 0, &f0);
        query("all (HID class)", 0, &fhid);
        query("acquired", 2, NULL);
        s32 n = query("available (no filter)", 1, &f0);
        if (n <= 0) {
            n = query("available (HID class)", 1, &fhid);
            if (n > 0)
                for (s32 i = 0; i < n && i < MAX_IFACES; i++)
                    open_reader(&ifaces[i]);
            n = query("available (vendor class)", 1, &fven);
        }
        if (n > 0) {
            UsbHsInterface copy[MAX_IFACES];
            memcpy(copy, ifaces, sizeof(copy));
            for (s32 i = 0; i < n && i < MAX_IFACES; i++)
                open_reader(&copy[i]);
        }
    done:
        out("%d reader(s) open; move the pen over the tablet, tap, press its buttons (30 s, + stops)\n", nreaders);
    }

    HidMouseState last_mouse = {0};
    u64 end = armGetSystemTick() + armNsToTicks(30ULL * 1000 * 1000 * 1000);
    while (appletMainLoop() && armGetSystemTick() < end) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        HidMouseState m;
        if (hidGetMouseStates(&m, 1) == 1 &&
            (m.x != last_mouse.x || m.y != last_mouse.y || m.buttons != last_mouse.buttons ||
             m.delta_x || m.delta_y || m.attributes != last_mouse.attributes)) {
            out("hid mouse: x=%d y=%d dx=%d dy=%d buttons=%x attr=%x\n", m.x, m.y, m.delta_x, m.delta_y,
                m.buttons, m.attributes);
            last_mouse = m;
        }
        if (nreaders)
            poll_readers();
        else
            svcSleepThread(5 * 1000 * 1000ULL);
    }

    for (int i = 0; i < nreaders; i++) {
        out("%s: %u reports\n", readers[i].name, readers[i].reports);
        if (readers[i].open) {
            usbHsEpClose(&readers[i].ep);
            usbHsIfClose(&readers[i].s);
        }
    }
    if (R_SUCCEEDED(rc))
        usbHsExit();
    out("usb-probe end\n");
    if (logf)
        fclose(logf);
    socketExit();
    return 0;
}
