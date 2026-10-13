/* RetroFlatbed Setup - GUI installer / uninstaller.
 *
 * Embeds the signed driver packages, the TWAIN data sources and the TWAIN managers
 * (see payload.rc) and installs either WIA, TWAIN or both.
 *
 *   RetroFlatbedSetup.exe                      GUI
 *   RetroFlatbedSetup.exe /install:wia|twain|both [/quiet] [/log:file]
 *   RetroFlatbedSetup.exe /uninstall [/quiet] [/log:file]
 */
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <setupapi.h>
#include <newdev.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"

#define WM_OP_DONE      (WM_APP + 1)
#define WM_AUTO_START   (WM_APP + 2)

enum { MODE_WIA = 1, MODE_TWAIN = 2, MODE_BOTH = 3 };
enum { OP_INSTALL = 1, OP_UNINSTALL = 2 };

static const wchar_t *APP_NAME      = L"RetroFlatbed";
static const wchar_t *APP_VERSION   = L"0.1.0";
static const wchar_t *UNINSTALL_KEY = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RetroFlatbed";
static const wchar_t *CLSID_KEY     = L"SOFTWARE\\Classes\\CLSID\\{c6d0a3e2-5b0f-4c11-9a52-7d1f0b8e4a21}";
static const wchar_t *CERT_NAMES[]  = { L"RetroFlatbed driver (self-signed)", L"LiDE20 WinUSB driver (self-signed)" };
static const GUID IF_GUID = { 0xf3efabc5, 0xb637, 0x4d66, { 0x91, 0xbb, 0x38, 0x46, 0x3a, 0xcb, 0x94, 0x63 } };

static HINSTANCE g_inst;
static HWND      g_dlg, g_log, g_progress;
static FILE     *g_logfile;
static int       g_quiet;
static int       g_failed;
static wchar_t   g_tmp[MAX_PATH];
static int       g_op, g_mode;

/* ------------------------------------------------------------------ */
/* logging / progress                                                  */
/* ------------------------------------------------------------------ */

static int g_avgw;
static int g_hext;

static void LOG(const wchar_t *fmt, ...)
{
    wchar_t buf[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_logfile) { fputws(buf, g_logfile); fputws(L"\n", g_logfile); fflush(g_logfile); }
    if (g_log) {
        LRESULT n = SendMessageW(g_log, LB_ADDSTRING, 0, (LPARAM)buf);
        SendMessageW(g_log, LB_SETTOPINDEX, (WPARAM)n, 0);
        if ((int)wcslen(buf) * g_avgw > g_hext) {
            g_hext = (int)wcslen(buf) * g_avgw;
            SendMessageW(g_log, LB_SETHORIZONTALEXTENT, (WPARAM)g_hext, 0);
        }
    }
}

static void FAIL(const wchar_t *what)
{
    DWORD e = GetLastError();
    g_failed = 1;
    LOG(L"  ERROR: %ls (Windows error %lu)", what, e);
}

static void PROGRESS(int pos)
{
    if (g_progress) SendMessageW(g_progress, PBM_SETPOS, (WPARAM)pos, 0);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void win_path(wchar_t *out, const wchar_t *rel)
{
    wchar_t w[MAX_PATH];
    GetWindowsDirectoryW(w, MAX_PATH);
    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%ls\\%ls", w, rel);
}

static void install_dir(wchar_t *out)
{
    wchar_t pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH) && !GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH))
        wcscpy_s(pf, MAX_PATH, L"C:\\Program Files");
    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%ls\\RetroFlatbed", pf);
}

static const void *res_data(int id, DWORD *size)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g;
    if (!r) return NULL;
    g = LoadResource(NULL, r);
    if (!g) return NULL;
    *size = SizeofResource(NULL, r);
    return LockResource(g);
}

static BOOL extract(int id, const wchar_t *path)
{
    DWORD size = 0, written = 0;
    const void *p = res_data(id, &size);
    HANDLE f;
    BOOL ok;
    if (!p) return FALSE;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(f, p, size, &written, NULL) && written == size;
    CloseHandle(f);
    return ok;
}

static void rmtree(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    HANDLE h;
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static ULONGLONG file_version(const wchar_t *path)
{
    DWORD h, sz = GetFileVersionInfoSizeW(path, &h);
    void *b;
    VS_FIXEDFILEINFO *fi;
    UINT l;
    ULONGLONG v = 0;
    if (!sz) return 0;
    b = malloc(sz);
    if (b && GetFileVersionInfoW(path, 0, sz, b) && VerQueryValueW(b, L"\\", (void **)&fi, &l))
        v = ((ULONGLONG)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS;
    free(b);
    return v;
}

static BYTE *read_file(const wchar_t *path, DWORD *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER li;
    BYTE *b = NULL;
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(f, &li) && li.QuadPart > 0 && li.QuadPart < (8 << 20)) {
        b = (BYTE *)malloc((size_t)li.QuadPart + 2);
        if (b && ReadFile(f, b, (DWORD)li.QuadPart, &got, NULL)) { *size = got; b[got] = b[got + 1] = 0; }
        else { free(b); b = NULL; }
    }
    CloseHandle(f);
    return b;
}

/* ASCII or UTF-16LE occurrence of a pattern inside an INF file image */
static int contains(const BYTE *buf, DWORD n, const char *pat)
{
    size_t l = strlen(pat), i, j;
    for (i = 0; i + l <= n; i++)
        if (!memcmp(buf + i, pat, l)) return 1;
    for (i = 0; i + 2 * l <= n; i++) {
        for (j = 0; j < l; j++)
            if (buf[i + 2 * j] != (BYTE)pat[j] || buf[i + 2 * j + 1] != 0) break;
        if (j == l) return 1;
    }
    return 0;
}

static void stop_wia_service(void)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), s;
    SERVICE_STATUS st;
    int i;
    if (!m) return;
    s = OpenServiceW(m, L"stisvc", SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (s) {
        if (QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED) {
            LOG(L"Stopping the Windows Image Acquisition service ...");
            ControlService(s, SERVICE_CONTROL_STOP, &st);
            for (i = 0; i < 50 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(200);
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(m);
}

/* ------------------------------------------------------------------ */
/* device state                                                        */
/* ------------------------------------------------------------------ */

static int scanner_attached(void)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA dd;
    DWORD i;
    int found = 0;
    if (set == INVALID_HANDLE_VALUE) return 0;
    dd.cbSize = sizeof(dd);
    for (i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dd); i++) {
        wchar_t ids[1024];
        DWORD need;
        if (SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_HARDWAREID, NULL, (PBYTE)ids, sizeof(ids) - 4, &need)) {
            wchar_t *p;
            ids[_countof(ids) - 1] = ids[_countof(ids) - 2] = 0;
            for (p = ids; *p; p += wcslen(p) + 1)
                if (wcsstr(p, L"VID_04A9&PID_220D")) { found = 1; break; }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static int interface_present(void)
{
    HDEVINFO s = SetupDiGetClassDevsW(&IF_GUID, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d;
    int ok;
    if (s == INVALID_HANDLE_VALUE) return 0;
    d.cbSize = sizeof(d);
    ok = SetupDiEnumDeviceInterfaces(s, NULL, &IF_GUID, 0, &d);
    SetupDiDestroyDeviceInfoList(s);
    return ok ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* driver packages                                                     */
/* ------------------------------------------------------------------ */

typedef struct { wchar_t path[MAX_PATH]; int wia; } oem_inf_t;

static int find_our_infs(oem_inf_t *out, int max)
{
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    win_path(dir, L"INF");
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\oem*.inf", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t p[MAX_PATH];
        DWORD sz = 0;
        BYTE *b;
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);
        b = read_file(p, &sz);
        if (!b) continue;
        if (n < max && (contains(b, sz, "ProviderName = \"RetroFlatbed\"") ||
                        contains(b, sz, "ProviderName = \"LiDE20 user-space driver\""))) {
            wcscpy_s(out[n].path, MAX_PATH, p);
            out[n].wia = contains(b, sz, "6bdd1fc6");
            n++;
        }
        free(b);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void remove_packages(void)
{
    oem_inf_t infs[16];
    int n = find_our_infs(infs, 16), i;
    for (i = 0; i < n; i++) {
        BOOL reboot = FALSE;
        if (DiUninstallDriverW(NULL, infs[i].path, 0, &reboot)) LOG(L"  removed driver package %ls", wcsrchr(infs[i].path, L'\\') + 1);
        else FAIL(L"could not remove the previous driver package");
    }
    if (!n) LOG(L"  no earlier driver package found");
}

static BOOL install_package(int infId, int catId, int dllId, const wchar_t *infName, const wchar_t *catName, const wchar_t *dllName)
{
    wchar_t inf[MAX_PATH], cat[MAX_PATH], dll[MAX_PATH];
    BOOL reboot = FALSE;
    _snwprintf_s(inf, MAX_PATH, _TRUNCATE, L"%ls\\%ls", g_tmp, infName);
    _snwprintf_s(cat, MAX_PATH, _TRUNCATE, L"%ls\\%ls", g_tmp, catName);
    if (!extract(infId, inf) || !extract(catId, cat)) { FAIL(L"could not unpack the driver package"); return FALSE; }
    if (dllId) {
        _snwprintf_s(dll, MAX_PATH, _TRUNCATE, L"%ls\\%ls", g_tmp, dllName);
        if (!extract(dllId, dll)) { FAIL(L"could not unpack the WIA driver"); return FALSE; }
    }
    if (!DiInstallDriverW(NULL, inf, DIIRFLAG_FORCE_INF, &reboot)) {
        FAIL(L"Windows refused the driver package");
        return FALSE;
    }
    LOG(L"  driver package %ls installed", infName);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* certificate                                                         */
/* ------------------------------------------------------------------ */

static void import_cert(void)
{
    DWORD size = 0;
    const void *p = res_data(IDR_CERT, &size);
    PCCERT_CONTEXT ctx;
    const wchar_t *stores[] = { L"Root", L"TrustedPublisher" };
    int i;
    if (!p) { FAIL(L"certificate missing from installer"); return; }
    ctx = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE *)p, size);
    if (!ctx) { FAIL(L"certificate unreadable"); return; }
    for (i = 0; i < 2; i++) {
        HCERTSTORE s = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, stores[i]);
        if (s && CertAddCertificateContextToStore(s, ctx, CERT_STORE_ADD_REPLACE_EXISTING, NULL))
            LOG(L"  certificate added to %ls", stores[i]);
        else FAIL(L"could not add the certificate");
        if (s) CertCloseStore(s, 0);
    }
    CertFreeCertificateContext(ctx);
}

static void remove_certs(void)
{
    const wchar_t *stores[] = { L"Root", L"TrustedPublisher", L"My" };
    int i, k, removed = 0;
    for (i = 0; i < 3; i++) {
        HCERTSTORE s = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, stores[i]);
        if (!s) continue;
        for (k = 0; k < 2; k++) {
            PCCERT_CONTEXT c;
            while ((c = CertFindCertificateInStore(s, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, CERT_NAMES[k], NULL)) != NULL) {
                CertDeleteCertificateFromStore(c);       /* frees c */
                removed++;
            }
        }
        CertCloseStore(s, 0);
    }
    LOG(L"  removed %d certificate(s) from the trust stores", removed);
}

/* ------------------------------------------------------------------ */
/* TWAIN                                                               */
/* ------------------------------------------------------------------ */

static void install_dsm(int id, const wchar_t *relDst, const wchar_t *label)
{
    wchar_t tmp[MAX_PATH], dst[MAX_PATH];
    ULONGLONG nv, cv;
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%ls\\dsm-%d.dll", g_tmp, id);
    if (!extract(id, tmp)) { FAIL(L"could not unpack the TWAIN manager"); return; }
    win_path(dst, relDst);
    nv = file_version(tmp);
    cv = (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) ? file_version(dst) : 0;
    if (cv && cv >= nv) { LOG(L"  %ls TWAIN manager already present (%u.%u) - kept", label, (unsigned)(cv >> 48), (unsigned)((cv >> 32) & 0xffff)); return; }
    if (CopyFileW(tmp, dst, FALSE)) LOG(L"  %ls TWAIN manager %u.%u installed", label, (unsigned)(nv >> 48), (unsigned)((nv >> 32) & 0xffff));
    else FAIL(L"could not install the TWAIN manager");
}

static void install_ds(int id, const wchar_t *relRoot, const wchar_t *label)
{
    wchar_t root[MAX_PATH], dir[MAX_PATH], file[MAX_PATH];
    win_path(root, relRoot);
    CreateDirectoryW(root, NULL);
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%ls\\RetroFlatbed", root);
    CreateDirectoryW(dir, NULL);
    _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\RetroFlatbed.ds", dir);
    if (extract(id, file)) LOG(L"  %ls TWAIN data source installed", label);
    else FAIL(L"could not install the TWAIN data source");
}

static void install_twain(void)
{
    install_ds(IDR_DS64, L"twain_64", L"64-bit");
    install_ds(IDR_DS32, L"twain_32", L"32-bit");
    install_dsm(IDR_DSM64, L"System32\\twaindsm.dll", L"64-bit");
    install_dsm(IDR_DSM32, L"SysWOW64\\twaindsm.dll", L"32-bit");
}

static void remove_dsm(int id, const wchar_t *relDst, const wchar_t *label)
{
    wchar_t dst[MAX_PATH];
    DWORD rs = 0, fs = 0;
    const void *r = res_data(id, &rs);
    BYTE *f;
    win_path(dst, relDst);
    f = read_file(dst, &fs);
    if (f && r && fs == rs && !memcmp(f, r, rs)) {
        if (DeleteFileW(dst)) LOG(L"  removed the bundled %ls TWAIN manager", label);
        else FAIL(L"could not remove the TWAIN manager");
    } else if (f) {
        LOG(L"  %ls TWAIN manager is not ours (or newer) - left in place", label);
    }
    free(f);
}

static void remove_twain(int alsoManagers)
{
    const wchar_t *roots[] = { L"twain_64", L"twain_32" };
    const wchar_t *names[] = { L"RetroFlatbed", L"LiDE20" };     /* current + former name */
    int i, k, n = 0;
    for (i = 0; i < 2; i++)
        for (k = 0; k < 2; k++) {
            wchar_t p[MAX_PATH], rel[MAX_PATH];
            _snwprintf_s(rel, MAX_PATH, _TRUNCATE, L"%ls\\%ls", roots[i], names[k]);
            win_path(p, rel);
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { rmtree(p); n++; }
        }
    LOG(L"  removed %d TWAIN data source folder(s)", n);
    if (alsoManagers) {
        remove_dsm(IDR_DSM64, L"System32\\twaindsm.dll", L"64-bit");
        remove_dsm(IDR_DSM32, L"SysWOW64\\twaindsm.dll", L"32-bit");
    }
}

/* ------------------------------------------------------------------ */
/* application registration                                            */
/* ------------------------------------------------------------------ */

static void reg_str(HKEY k, const wchar_t *name, const wchar_t *v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

static void register_app(void)
{
    wchar_t dir[MAX_PATH], self[MAX_PATH], dst[MAX_PATH], un[MAX_PATH + 32];
    HKEY k;
    DWORD one = 1;
    install_dir(dir);
    CreateDirectoryW(dir, NULL);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%ls\\RetroFlatbedSetup.exe", dir);
    if (_wcsicmp(self, dst) && !CopyFileW(self, dst, FALSE)) { FAIL(L"could not copy the uninstaller"); return; }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        _snwprintf_s(un, _countof(un), _TRUNCATE, L"\"%ls\" /uninstall", dst);
        reg_str(k, L"DisplayName", L"RetroFlatbed (Canon CanoScan LiDE 20 driver)");
        reg_str(k, L"DisplayVersion", APP_VERSION);
        reg_str(k, L"Publisher", L"RetroFlatbed");
        reg_str(k, L"UninstallString", un);
        reg_str(k, L"InstallLocation", dir);
        reg_str(k, L"DisplayIcon", dst);
        RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
        RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
        RegCloseKey(k);
        LOG(L"  registered in Apps & features");
    }
}

static void schedule_dir_removal(const wchar_t *dir)
{
    /* A temp copy of this GUI-subsystem exe deletes the folder once we exit: no console window. */
    wchar_t self[MAX_PATH], tmp[MAX_PATH], copy[MAX_PATH], cmd[MAX_PATH * 3];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!GetModuleFileNameW(NULL, self, MAX_PATH) || !GetTempPathW(MAX_PATH, tmp)) return;
    _snwprintf_s(copy, _countof(copy), _TRUNCATE, L"%lsrf-cleanup-%lu.exe", tmp, GetTickCount());
    if (!CopyFileW(self, copy, FALSE)) return;
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%ls\" \"/cleanup:%lu:%ls\"", copy, GetCurrentProcessId(), dir);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, tmp, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

static int run_cleanup(const wchar_t *arg)
{
    wchar_t self[MAX_PATH];
    DWORD pid = wcstoul(arg, NULL, 10);
    const wchar_t *dir = wcschr(arg, L':');
    HANDLE h;
    int i;
    if (!dir || !pid) return 1;
    dir++;
    h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) { WaitForSingleObject(h, 60000); CloseHandle(h); }
    for (i = 0; i < 20; i++) {
        rmtree(dir);
        if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) break;
        Sleep(500);
    }
    if (GetModuleFileNameW(NULL, self, MAX_PATH)) MoveFileExW(self, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static void do_install(int mode)
{
    int waited;
    LOG(L"Installing RetroFlatbed (%ls) ...", mode == MODE_WIA ? L"WIA" : mode == MODE_TWAIN ? L"TWAIN" : L"WIA + TWAIN");
    PROGRESS(5);

    LOG(L"Removing earlier installation ...");
    stop_wia_service();
    remove_packages();
    remove_twain(0);
    PROGRESS(25);

    LOG(L"Trusting the RetroFlatbed driver certificate ...");
    import_cert();
    PROGRESS(40);

    LOG(L"Installing the %ls driver package ...", mode == MODE_TWAIN ? L"WinUSB" : L"WIA");
    if (mode == MODE_TWAIN) install_package(IDR_USB_INF, IDR_USB_CAT, 0, L"retroflatbed-winusb.inf", L"retroflatbed-winusb.cat", NULL);
    else                    install_package(IDR_WIA_INF, IDR_WIA_CAT, IDR_WIA_DLL, L"retroflatbed-wia.inf", L"retroflatbed-wia.cat", L"RetroFlatbedWIA.dll");
    PROGRESS(70);

    if (mode == MODE_TWAIN || mode == MODE_BOTH) {
        LOG(L"Installing TWAIN ...");
        install_twain();
    }
    PROGRESS(85);

    register_app();
    PROGRESS(92);

    if (scanner_attached()) {
        LOG(L"Waiting for Windows to start the scanner ...");
        for (waited = 0; waited < 15 && !interface_present(); waited++) Sleep(1000);
        if (interface_present()) LOG(L"Scanner ready.");
        else {
            LOG(L"Windows could not restart the scanner yet. Please UNPLUG AND REPLUG its USB cable.");
            LOG(L"(Close programs that use the scanner first if this keeps happening.)");
        }
    } else {
        LOG(L"The scanner is not connected right now. Plug it in and it will be set up automatically.");
    }
    PROGRESS(100);
    LOG(g_failed ? L"Finished with errors - see above." : L"Done.");
}

static void do_uninstall(void)
{
    wchar_t dir[MAX_PATH], local[MAX_PATH], cache[MAX_PATH];
    LOG(L"Uninstalling RetroFlatbed ...");
    PROGRESS(5);

    stop_wia_service();
    LOG(L"Removing driver packages ...");
    remove_packages();
    PROGRESS(35);

    LOG(L"Removing TWAIN data sources and managers ...");
    remove_twain(1);
    PROGRESS(55);

    LOG(L"Removing the certificate ...");
    remove_certs();
    PROGRESS(70);

    LOG(L"Cleaning up registry and files ...");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, CLSID_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY);
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        _snwprintf_s(cache, MAX_PATH, _TRUNCATE, L"%ls\\RetroFlatbed", local);
        if (GetFileAttributesW(cache) != INVALID_FILE_ATTRIBUTES) { rmtree(cache); LOG(L"  removed calibration cache"); }
    }
    install_dir(dir);
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) { schedule_dir_removal(dir); LOG(L"  uninstaller folder will be deleted in a moment"); }
    PROGRESS(100);
    LOG(g_failed ? L"Finished with errors - see above." : L"RetroFlatbed has been removed. A scanner that is still plugged in may need to be replugged.");
}

/* ------------------------------------------------------------------ */
/* GUI                                                                 */
/* ------------------------------------------------------------------ */

static void make_tmp(void)
{
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    _snwprintf_s(g_tmp, MAX_PATH, _TRUNCATE, L"%lsRetroFlatbedSetup-%lu", t, GetCurrentProcessId());
    CreateDirectoryW(g_tmp, NULL);
}

static void RunOp(void)
{
    g_failed = 0;
    make_tmp();
    if (g_op == OP_INSTALL) do_install(g_mode);
    else do_uninstall();
    rmtree(g_tmp);
}

static DWORD WINAPI op_thread(LPVOID unused)
{
    (void)unused;
    RunOp();
    PostMessageW(g_dlg, WM_OP_DONE, 0, 0);
    return 0;
}

static void set_busy(int busy)
{
    EnableWindow(GetDlgItem(g_dlg, IDC_INSTALL), !busy);
    EnableWindow(GetDlgItem(g_dlg, IDC_UNINSTALL), !busy);
    EnableWindow(GetDlgItem(g_dlg, IDC_CLOSE), !busy);
    EnableWindow(GetDlgItem(g_dlg, IDC_RADIO_WIA), !busy);
    EnableWindow(GetDlgItem(g_dlg, IDC_RADIO_TWAIN), !busy);
    EnableWindow(GetDlgItem(g_dlg, IDC_RADIO_BOTH), !busy);
}

static void refresh_state(void)
{
    oem_inf_t infs[16];
    int n = find_our_infs(infs, 16), wia = 0, usb = 0, i, mode = MODE_WIA;
    wchar_t twain[MAX_PATH], msg[400], parts[200] = L"";
    int hasTwain;
    win_path(twain, L"twain_64\\RetroFlatbed\\RetroFlatbed.ds");
    hasTwain = GetFileAttributesW(twain) != INVALID_FILE_ATTRIBUTES;
    for (i = 0; i < n; i++) { if (infs[i].wia) wia = 1; else usb = 1; }

    if (wia) wcscat_s(parts, _countof(parts), L"WIA driver");
    if (usb) wcscat_s(parts, _countof(parts), L"WinUSB driver");
    if (hasTwain) { if (parts[0]) wcscat_s(parts, _countof(parts), L" + "); wcscat_s(parts, _countof(parts), L"TWAIN"); }
    _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Installed now: %ls.   Scanner: %ls.",
                 parts[0] ? parts : L"nothing",
                 scanner_attached() ? L"connected" : L"not connected");
    SetDlgItemTextW(g_dlg, IDC_STATE, msg);

    if (wia && hasTwain) mode = MODE_BOTH;
    else if (!wia && hasTwain) mode = MODE_TWAIN;
    CheckRadioButton(g_dlg, IDC_RADIO_WIA, IDC_RADIO_BOTH,
                     mode == MODE_BOTH ? IDC_RADIO_BOTH : mode == MODE_TWAIN ? IDC_RADIO_TWAIN : IDC_RADIO_WIA);
}

static void start_op(int op)
{
    g_op = op;
    if (op == OP_INSTALL)
        g_mode = IsDlgButtonChecked(g_dlg, IDC_RADIO_BOTH) ? MODE_BOTH :
                 IsDlgButtonChecked(g_dlg, IDC_RADIO_TWAIN) ? MODE_TWAIN : MODE_WIA;
    SendMessageW(g_log, LB_RESETCONTENT, 0, 0);
    g_hext = 0;
    SendMessageW(g_log, LB_SETHORIZONTALEXTENT, 0, 0);
    PROGRESS(0);
    set_busy(1);
    if (!CreateThread(NULL, 0, op_thread, NULL, 0, NULL)) { set_busy(0); LOG(L"Could not start the worker thread."); }
}

/* ------------------------------------------------------------------ */
/* DPI-aware layout: all metrics are computed from the monitor DPI and */
/* the measured text, so the window adapts to scaling and resolution   */
/* ------------------------------------------------------------------ */

#define SC(x) MulDiv((x), g_dpi, 96)

static const int TEXT_IDS[] = { IDC_SUBTITLE, IDC_GROUP, IDC_RADIO_WIA, IDC_RADIO_TWAIN, IDC_RADIO_BOTH, IDC_HINT,
                                IDC_STATE, IDC_LOG, IDC_NOTE, IDC_UNINSTALL, IDC_INSTALL, IDC_CLOSE };

static int   g_dpi = 96;
static int   g_lineh;
static HFONT g_font, g_font_title;
static HICON g_ico_big, g_ico_small, g_ico_logo;

static HICON load_icon(int px)
{
    return (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
}

static void apply_dpi_resources(void)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    LOGFONTW lf;
    HFONT old_font = g_font, old_title = g_font_title;
    HICON o1 = g_ico_big, o2 = g_ico_small, o3 = g_ico_logo;
    TEXTMETRICW tm;
    HDC dc;
    size_t i;

    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi)) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    }
    lf = ncm.lfMessageFont;
    g_font = CreateFontIndirectW(&lf);
    lf.lfHeight = MulDiv(lf.lfHeight, 17, 10);
    lf.lfWeight = FW_SEMIBOLD;
    g_font_title = CreateFontIndirectW(&lf);

    for (i = 0; i < _countof(TEXT_IDS); i++) SendDlgItemMessageW(g_dlg, TEXT_IDS[i], WM_SETFONT, (WPARAM)g_font, FALSE);
    SendDlgItemMessageW(g_dlg, IDC_TITLE, WM_SETFONT, (WPARAM)g_font_title, FALSE);
    if (old_font) DeleteObject(old_font);
    if (old_title) DeleteObject(old_title);

    dc = GetDC(g_dlg);
    SelectObject(dc, g_font);
    GetTextMetricsW(dc, &tm);
    ReleaseDC(g_dlg, dc);
    g_lineh = tm.tmHeight;
    g_avgw = tm.tmAveCharWidth;

    g_ico_big   = load_icon(GetSystemMetricsForDpi(SM_CXICON, g_dpi));
    g_ico_small = load_icon(GetSystemMetricsForDpi(SM_CXSMICON, g_dpi));
    g_ico_logo  = load_icon(SC(48));
    SendMessageW(g_dlg, WM_SETICON, ICON_BIG, (LPARAM)g_ico_big);
    SendMessageW(g_dlg, WM_SETICON, ICON_SMALL, (LPARAM)g_ico_small);
    SendDlgItemMessageW(g_dlg, IDC_LOGO, STM_SETICON, (WPARAM)g_ico_logo, 0);
    if (o1) DestroyIcon(o1);
    if (o2) DestroyIcon(o2);
    if (o3) DestroyIcon(o3);
}

static int text_h(int id, int width)
{
    wchar_t t[800];
    HWND c = GetDlgItem(g_dlg, id);
    HDC dc = GetDC(c);
    HGDIOBJ o = SelectObject(dc, (HFONT)SendMessageW(c, WM_GETFONT, 0, 0));
    RECT r = { 0, 0, width > 1 ? width : 1, 0 };
    GetWindowTextW(c, t, _countof(t));
    if (!t[0]) wcscpy_s(t, _countof(t), L"Ag");
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS);
    SelectObject(dc, o);
    ReleaseDC(c, dc);
    return r.bottom;
}

static int text_w(int id)
{
    wchar_t t[200];
    HWND c = GetDlgItem(g_dlg, id);
    HDC dc = GetDC(c);
    HGDIOBJ o = SelectObject(dc, (HFONT)SendMessageW(c, WM_GETFONT, 0, 0));
    RECT r = { 0, 0, 0, 0 };
    GetWindowTextW(c, t, _countof(t));
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, o);
    ReleaseDC(c, dc);
    return r.right;
}

static void place(int id, int x, int y, int w, int h, int apply)
{
    if (apply) SetWindowPos(GetDlgItem(g_dlg, id), NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Lays everything out for a client area of W x H and returns the height needed with a log of
   `log_lines` lines. With apply == 0 nothing is moved. */
static int do_layout(int W, int H, int log_lines, int apply)
{
    const int M = SC(16), G = SC(8), P = SC(12);
    int cw = W - 2 * M, y = M, x, tw, th, sh, logo = SC(48), gtop, gy, rw, rh, i, ph;
    int bh, bw[3], note_h, state_h, log_h, need, bx;
    static const int radios[3] = { IDC_RADIO_WIA, IDC_RADIO_TWAIN, IDC_RADIO_BOTH };
    static const int btns[3]   = { IDC_UNINSTALL, IDC_INSTALL, IDC_CLOSE };
    const int glyph = SC(13) + SC(9);

    /* header */
    x = M + logo + SC(12);
    tw = W - M - x;
    th = text_h(IDC_TITLE, tw);
    sh = text_h(IDC_SUBTITLE, tw);
    place(IDC_LOGO, M, y, logo, logo, apply);
    if (th + SC(2) + sh >= logo) {
        place(IDC_TITLE, x, y, tw, th, apply);
        place(IDC_SUBTITLE, x, y + th + SC(2), tw, sh, apply);
        y += th + SC(2) + sh;
    } else {
        int off = (logo - th - SC(2) - sh) / 2;
        place(IDC_TITLE, x, y + off, tw, th, apply);
        place(IDC_SUBTITLE, x, y + off + th + SC(2), tw, sh, apply);
        y += logo;
    }
    y += G + SC(6);

    /* group box with the three modes */
    gtop = y;
    gy = gtop + g_lineh + SC(8);
    rw = cw - 2 * P;
    for (i = 0; i < 3; i++) {
        rh = text_h(radios[i], rw - glyph) + SC(6);
        if (rh < SC(22)) rh = SC(22);
        place(radios[i], M + P, gy, rw, rh, apply);
        gy += rh + SC(2);
    }
    rh = text_h(IDC_HINT, rw);
    place(IDC_HINT, M + P, gy + SC(2), rw, rh, apply);
    gy += SC(2) + rh + P;
    place(IDC_GROUP, M, gtop, cw, gy - gtop, apply);
    y = gy + G;

    state_h = text_h(IDC_STATE, cw);
    place(IDC_STATE, M, y, cw, state_h, apply);
    y += state_h + G;

    ph = SC(10);
    place(IDC_PROGRESS, M, y, cw, ph, apply);
    y += ph + G;

    /* bottom: buttons, note */
    bh = text_h(IDC_CLOSE, 1000) + SC(14);
    if (bh < SC(28)) bh = SC(28);
    for (i = 0; i < 3; i++) {
        bw[i] = text_w(btns[i]) + SC(32);
        if (bw[i] < SC(92)) bw[i] = SC(92);
    }
    note_h = text_h(IDC_NOTE, cw);

    log_h = log_lines * g_lineh + SC(8);
    need = y + log_h + G + note_h + G + bh + M;
    if (apply) {
        int avail = H - M - bh - G - note_h - G - y;   /* room for the log */
        if (avail < g_lineh) avail = g_lineh;
        place(IDC_LOG, M, y, cw, avail, 1);
        place(IDC_NOTE, M, H - M - bh - G - note_h, cw, note_h, 1);
        place(IDC_UNINSTALL, M, H - M - bh, bw[0], bh, 1);
        bx = W - M - bw[2];
        place(IDC_CLOSE, bx, H - M - bh, bw[2], bh, 1);
        bx -= SC(8) + bw[1];
        place(IDC_INSTALL, bx, H - M - bh, bw[1], bh, 1);
        RedrawWindow(g_dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    return need;
}

static void layout_now(void)
{
    RECT rc;
    GetClientRect(g_dlg, &rc);
    do_layout(rc.right, rc.bottom, 2, 1);
}

static void window_size_for_client(int cw, int ch, int *ww, int *wh)
{
    RECT r = { 0, 0, cw, ch };
    LONG style = GetWindowLongW(g_dlg, GWL_STYLE), ex = GetWindowLongW(g_dlg, GWL_EXSTYLE);
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g_dpi);
    *ww = r.right - r.left;
    *wh = r.bottom - r.top;
}

static void initial_placement(void)
{
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon = MonitorFromWindow(g_dlg, MONITOR_DEFAULTTOPRIMARY);
    int cw = SC(520), ch, ww, wh, aw, ah;
    GetMonitorInfoW(mon, &mi);
    aw = mi.rcWork.right - mi.rcWork.left;
    ah = mi.rcWork.bottom - mi.rcWork.top;
    ch = do_layout(cw, 0, 9, 0);
    window_size_for_client(cw, ch, &ww, &wh);
    if (ww > aw - SC(24)) ww = aw - SC(24);
    if (wh > ah - SC(24)) wh = ah - SC(24);
    SetWindowPos(g_dlg, NULL, mi.rcWork.left + (aw - ww) / 2, mi.rcWork.top + (ah - wh) / 2, ww, wh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK dlg_proc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG: {
        wchar_t sub[300];
        g_dlg = d;
        g_dpi = (int)GetDpiForWindow(d);
        g_log = GetDlgItem(d, IDC_LOG);
        g_progress = GetDlgItem(d, IDC_PROGRESS);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        _snwprintf_s(sub, _countof(sub), _TRUNCATE,
                     L"Version %ls  -  user-space driver for the Canon CanoScan LiDE 20 (USB 04A9:220D) on Windows 11.", APP_VERSION);
        SetDlgItemTextW(d, IDC_SUBTITLE, sub);
        apply_dpi_resources();
        refresh_state();
        initial_placement();
        layout_now();
        if (l == OP_UNINSTALL) PostMessageW(d, WM_AUTO_START, 0, 0);
        return TRUE;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && g_font) layout_now();
        return TRUE;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        MONITORINFO mi = { sizeof(mi) };
        int ww, wh, minw = SC(440);
        if (!g_font) return FALSE;
        GetMonitorInfoW(MonitorFromWindow(d, MONITOR_DEFAULTTONEAREST), &mi);
        window_size_for_client(minw, do_layout(minw, 0, 3, 0), &ww, &wh);
        mm->ptMinTrackSize.x = min(ww, mi.rcWork.right - mi.rcWork.left);
        mm->ptMinTrackSize.y = min(wh, mi.rcWork.bottom - mi.rcWork.top);
        return TRUE;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        g_dpi = HIWORD(w);
        apply_dpi_resources();
        SetWindowPos(d, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_now();
        return TRUE;
    }
    case WM_DESTROY:
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        if (g_font_title) { DeleteObject(g_font_title); g_font_title = NULL; }
        return FALSE;
    case WM_AUTO_START:
        if (MessageBoxW(d, L"Remove RetroFlatbed completely?\n\nThis removes the driver, the TWAIN/WIA components and the certificate.",
                        APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) start_op(OP_UNINSTALL);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_INSTALL:   start_op(OP_INSTALL); return TRUE;
        case IDC_UNINSTALL:
            if (MessageBoxW(d, L"Remove RetroFlatbed completely?\n\nThis removes the driver, the TWAIN/WIA components and the certificate.",
                            APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) start_op(OP_UNINSTALL);
            return TRUE;
        case IDC_CLOSE: case IDCANCEL:
            if (IsWindowEnabled(GetDlgItem(d, IDC_CLOSE))) EndDialog(d, 0);
            return TRUE;
        }
        break;
    case WM_OP_DONE:
        set_busy(0);
        refresh_state();
        layout_now();
        if (g_op == OP_INSTALL && !g_failed) {
            if (!interface_present() && scanner_attached())
                MessageBoxW(d, L"Installed. Please unplug and replug the scanner's USB cable now.", APP_NAME, MB_OK | MB_ICONINFORMATION);
        }
        if (g_failed) MessageBoxW(d, L"Finished with errors. See the log in the window for details.", APP_NAME, MB_OK | MB_ICONWARNING);
        return TRUE;
    case WM_CLOSE:
        if (IsWindowEnabled(GetDlgItem(d, IDC_CLOSE))) EndDialog(d, 0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    int argc, i, cli = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    (void)prev; (void)cmdline; (void)show;
    g_inst = inst;
    InitCommonControlsEx(&icc);

    if (argv && argc > 1 && !_wcsnicmp(argv[1], L"/cleanup:", 9)) return run_cleanup(argv[1] + 9);

    for (i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/quiet") || !_wcsicmp(argv[i], L"/silent")) g_quiet = 1;
        else if (!_wcsnicmp(argv[i], L"/log:", 5)) g_logfile = _wfopen(argv[i] + 5, L"w, ccs=UTF-8");
        else if (!_wcsicmp(argv[i], L"/uninstall")) { g_op = OP_UNINSTALL; cli = 1; }
        else if (!_wcsnicmp(argv[i], L"/install:", 9)) {
            g_op = OP_INSTALL; cli = 1;
            g_mode = !_wcsicmp(argv[i] + 9, L"twain") ? MODE_TWAIN : !_wcsicmp(argv[i] + 9, L"both") ? MODE_BOTH : MODE_WIA;
        }
    }

    if (cli && g_quiet) {                 /* unattended */
        RunOp();
        if (g_logfile) fclose(g_logfile);
        return g_failed ? 1 : 0;
    }
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETUP), NULL, dlg_proc, (cli && g_op == OP_UNINSTALL) ? OP_UNINSTALL : 0);
    if (g_logfile) fclose(g_logfile);
    return g_failed ? 1 : 0;
}
