/* WinUSB implementation of the sanei_usb subset used by the LM983x code.
 *
 * Devices are located through SetupAPI by VID/PID and must be bound to the
 * WinUSB driver (see driver\lide20.inf or Zadig). The device interface GUID
 * is read from the device's "DeviceInterfaceGUIDs" registry value so both
 * the bundled INF and Zadig-installed drivers work.
 */
#include <initguid.h>
#include <setupapi.h>
#include <winusb.h>
#include <cfgmgr32.h>

#include "../third_party/sane/include/sane/sane.h"
#include "../third_party/sane/include/sane/sanei_usb.h"
#include "../third_party/sane/include/sane/sanei_debug.h"

#define MAX_HANDLES     8
#define BULK_TIMEOUT_MS 30000

typedef struct {
    int                      used;
    HANDLE                   file;
    WINUSB_INTERFACE_HANDLE  wu;
    UCHAR                    ep_in;
    UCHAR                    ep_out;
    SANE_Word                vid;
    SANE_Word                pid;
} usb_slot_t;

static usb_slot_t g_slots[MAX_HANDLES];

void sanei_usb_init(void) { }

void sanei_usb_exit(void)
{
    int i;
    for (i = 0; i < MAX_HANDLES; i++)
        if (g_slots[i].used)
            sanei_usb_close(i);
}

static int parse_vid_pid(const char *s, SANE_Word *vid, SANE_Word *pid)
{
    const char *v = NULL, *p = NULL, *q;
    char lower[512];
    size_t i, n = strlen(s);

    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (i = 0; i < n; i++)
        lower[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    lower[n] = 0;

    for (q = lower; *q; q++) {
        if (!v && !strncmp(q, "vid_", 4)) v = q + 4;
        if (!p && !strncmp(q, "pid_", 4)) p = q + 4;
    }
    if (!v || !p) return 0;
    *vid = (SANE_Word)strtol(v, NULL, 16);
    *pid = (SANE_Word)strtol(p, NULL, 16);
    return 1;
}

static int read_interface_guid(HDEVINFO set, SP_DEVINFO_DATA *dd, GUID *out)
{
    HKEY  key;
    char  buf[512];
    DWORD type = 0, len = sizeof(buf) - 1;
    int   ok = 0;

    key = SetupDiOpenDevRegKey(set, dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE)
        return 0;

    if (RegQueryValueExA(key, "DeviceInterfaceGUIDs", NULL, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS
        && (type == REG_MULTI_SZ || type == REG_SZ) && buf[0]) {
        unsigned long d1; unsigned int d2, d3, b[8];
        buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = 0;
        if (sscanf(buf, "{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3,
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) == 11) {
            int k;
            out->Data1 = d1; out->Data2 = (USHORT)d2; out->Data3 = (USHORT)d3;
            for (k = 0; k < 8; k++) out->Data4[k] = (UCHAR)b[k];
            ok = 1;
        }
    }
    RegCloseKey(key);
    return ok;
}

SANE_Status sanei_usb_find_devices(SANE_Int vendor, SANE_Int product,
                                   SANE_Status (*attach)(SANE_String_Const devname))
{
    HDEVINFO        set;
    SP_DEVINFO_DATA dd;
    DWORD           idx;
    char            want[64];
    char            seen[16][512];
    int             nseen = 0;

    _snprintf(want, sizeof(want), "VID_%04X&PID_%04X", vendor & 0xffff, product & 0xffff);

    set = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return SANE_STATUS_IO_ERROR;

    dd.cbSize = sizeof(dd);
    for (idx = 0; SetupDiEnumDeviceInfo(set, idx, &dd); idx++) {
        char  ids[1024];
        DWORD need = 0;
        const char *p;
        int   match = 0;
        GUID  guid;
        HDEVINFO iset;
        SP_DEVICE_INTERFACE_DATA ifd;
        DWORD ii;

        if (!SetupDiGetDeviceRegistryPropertyA(set, &dd, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)ids, sizeof(ids) - 2, &need))
            continue;
        ids[sizeof(ids) - 1] = ids[sizeof(ids) - 2] = 0;
        for (p = ids; *p; p += strlen(p) + 1) {
            char up[128];
            size_t k, n = strlen(p);
            if (n >= sizeof(up)) n = sizeof(up) - 1;
            for (k = 0; k < n; k++) up[k] = (char)((p[k] >= 'a' && p[k] <= 'z') ? p[k] - 32 : p[k]);
            up[n] = 0;
            if (strstr(up, want)) { match = 1; break; }
        }
        if (!match)
            continue;

        if (!read_interface_guid(set, &dd, &guid)) {
            DBG(1, "winusb: %s present but no DeviceInterfaceGUIDs (WinUSB driver not bound?)\n", want);
            continue;
        }

        /* interfaces live in a separate set, built from the interface class GUID */
        iset = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (iset == INVALID_HANDLE_VALUE)
            continue;

        ifd.cbSize = sizeof(ifd);
        for (ii = 0; SetupDiEnumDeviceInterfaces(iset, NULL, &guid, ii, &ifd); ii++) {
            DWORD sz = 0;
            SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail;

            SetupDiGetDeviceInterfaceDetailA(iset, &ifd, NULL, 0, &sz, NULL);
            if (!sz) continue;
            detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(sz);
            if (!detail) continue;
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailA(iset, &ifd, detail, sz, NULL, NULL)) {
                SANE_Word v, pr;
                int dup = 0, k;
                if (parse_vid_pid(detail->DevicePath, &v, &pr) &&
                    v == (vendor & 0xffff) && pr == (product & 0xffff)) {
                    for (k = 0; k < nseen; k++)
                        if (!_stricmp(seen[k], detail->DevicePath)) dup = 1;
                    if (!dup) {
                        if (nseen < 16) { strncpy(seen[nseen], detail->DevicePath, 511); seen[nseen++][511] = 0; }
                        DBG(5, "winusb: found %s\n", detail->DevicePath);
                        attach(detail->DevicePath);
                    }
                }
            }
            free(detail);
        }
        SetupDiDestroyDeviceInfoList(iset);
    }
    SetupDiDestroyDeviceInfoList(set);
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_open(SANE_String_Const devname, SANE_Int *dn)
{
    int        i;
    usb_slot_t *s = NULL;
    USB_INTERFACE_DESCRIPTOR idesc;
    WINUSB_PIPE_INFORMATION  pipe;
    UCHAR      k;
    ULONG      timeout = BULK_TIMEOUT_MS;

    for (i = 0; i < MAX_HANDLES; i++)
        if (!g_slots[i].used) { s = &g_slots[i]; break; }
    if (!s) return SANE_STATUS_NO_MEM;

    memset(s, 0, sizeof(*s));
    if (!parse_vid_pid(devname, &s->vid, &s->pid))
        s->vid = s->pid = 0;

    s->file = CreateFileA(devname, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (s->file == INVALID_HANDLE_VALUE) {
        DBG(1, "winusb: CreateFile failed (%lu)\n", GetLastError());
        return SANE_STATUS_ACCESS_DENIED;
    }
    if (!WinUsb_Initialize(s->file, &s->wu)) {
        DBG(1, "winusb: WinUsb_Initialize failed (%lu)\n", GetLastError());
        CloseHandle(s->file);
        return SANE_STATUS_IO_ERROR;
    }
    if (!WinUsb_QueryInterfaceSettings(s->wu, 0, &idesc)) {
        WinUsb_Free(s->wu); CloseHandle(s->file);
        return SANE_STATUS_IO_ERROR;
    }
    for (k = 0; k < idesc.bNumEndpoints; k++) {
        if (!WinUsb_QueryPipe(s->wu, 0, k, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)  && !s->ep_in)  s->ep_in  = pipe.PipeId;
        if (USB_ENDPOINT_DIRECTION_OUT(pipe.PipeId) && !s->ep_out) s->ep_out = pipe.PipeId;
    }
    if (!s->ep_in || !s->ep_out) {
        DBG(1, "winusb: bulk endpoints not found\n");
        WinUsb_Free(s->wu); CloseHandle(s->file);
        return SANE_STATUS_IO_ERROR;
    }
    WinUsb_SetPipePolicy(s->wu, s->ep_in,  PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    WinUsb_SetPipePolicy(s->wu, s->ep_out, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    WinUsb_ResetPipe(s->wu, s->ep_in);
    WinUsb_ResetPipe(s->wu, s->ep_out);
    WinUsb_FlushPipe(s->wu, s->ep_in);

    s->used = 1;
    *dn = (SANE_Int)(s - g_slots);
    DBG(5, "winusb: opened handle %d (in=0x%02x out=0x%02x)\n", *dn, s->ep_in, s->ep_out);
    return SANE_STATUS_GOOD;
}

void sanei_usb_close(SANE_Int dn)
{
    usb_slot_t *s;
    if (dn < 0 || dn >= MAX_HANDLES || !g_slots[dn].used) return;
    s = &g_slots[dn];
    WinUsb_Free(s->wu);
    CloseHandle(s->file);
    memset(s, 0, sizeof(*s));
}

SANE_Status sanei_usb_get_vendor_product(SANE_Int dn, SANE_Word *vendor, SANE_Word *product)
{
    if (dn < 0 || dn >= MAX_HANDLES || !g_slots[dn].used) return SANE_STATUS_INVAL;
    if (!g_slots[dn].vid) return SANE_STATUS_UNSUPPORTED;
    *vendor  = g_slots[dn].vid;
    *product = g_slots[dn].pid;
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_read_bulk(SANE_Int dn, SANE_Byte *buffer, size_t *size)
{
    ULONG got = 0;
    usb_slot_t *s;
    if (dn < 0 || dn >= MAX_HANDLES || !g_slots[dn].used) return SANE_STATUS_INVAL;
    s = &g_slots[dn];

    if (!WinUsb_ReadPipe(s->wu, s->ep_in, buffer, (ULONG)*size, &got, NULL)) {
        DBG(1, "winusb: ReadPipe failed (%lu)\n", GetLastError());
        WinUsb_ResetPipe(s->wu, s->ep_in);
        *size = 0;
        return SANE_STATUS_IO_ERROR;
    }
    *size = got;
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_write_bulk(SANE_Int dn, const SANE_Byte *buffer, size_t *size)
{
    ULONG put = 0;
    usb_slot_t *s;
    if (dn < 0 || dn >= MAX_HANDLES || !g_slots[dn].used) return SANE_STATUS_INVAL;
    s = &g_slots[dn];

    if (!WinUsb_WritePipe(s->wu, s->ep_out, (PUCHAR)buffer, (ULONG)*size, &put, NULL)) {
        DBG(1, "winusb: WritePipe failed (%lu)\n", GetLastError());
        WinUsb_ResetPipe(s->wu, s->ep_out);
        *size = 0;
        return SANE_STATUS_IO_ERROR;
    }
    *size = put;
    return SANE_STATUS_GOOD;
}
