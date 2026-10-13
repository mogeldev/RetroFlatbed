/* WIA 2.0 user-mode driver (IStiUSD + IWiaMiniDrv) for the Canon CanoScan
 * LiDE 20. The scanner is driven through lide20_core / WinUSB.
 *
 * One flatbed item below the root item; stream (TYMED_FILE) BMP transfers.
 * The Windows WIA common UI provides the scan settings dialog from the
 * properties published here.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <propvarutil.h>
#include <stdio.h>
#include <string.h>

#include <sti.h>
#include <stiusd.h>
#include <wiamindr.h>
#include <wiamdef.h>

extern "C" {
#include "lide20_core.h"
}

static HINSTANCE g_hInst;

// {c6d0a3e2-5b0f-4c11-9a52-7d1f0b8e4a21}
static const CLSID CLSID_Lide20Wia =
    { 0xc6d0a3e2, 0x5b0f, 0x4c11, { 0x9a, 0x52, 0x7d, 0x1f, 0x0b, 0x8e, 0x4a, 0x21 } };

#define TRACE(...)  do { char b_[256]; _snprintf_s(b_, sizeof(b_), _TRUNCATE, __VA_ARGS__); OutputDebugStringA(b_); } while (0)

// Scan bed in thousandths of an inch (2550 x 3508 @ 300 dpi)
#define BED_W_1000   8500
#define BED_H_1000  11693
#define OPTICAL_DPI   600
#define LOCK_TIMEOUT 1000

#define ITEM_ROOT     L"Root"
#define ITEM_FLATBED  L"Flatbed"

struct DRV_ITEM_CTX { GUID category; };

/* ------------------------------------------------------------------ */
/* small WIA property builder                                          */
/* ------------------------------------------------------------------ */

class PropSet {
    enum { MAXP = 48, MAXL = 16 };
    PROPID            m_id[MAXP];
    LPOLESTR          m_name[MAXP];
    PROPSPEC          m_spec[MAXP];
    PROPVARIANT       m_var[MAXP];
    WIA_PROPERTY_INFO m_info[MAXP];
    LONG              m_lists[MAXP][MAXL];
    GUID              m_glists[MAXP][MAXL];
    int               m_n;

    int Next(PROPID id, LPOLESTR name, ULONG access, VARTYPE vt)
    {
        int i = m_n++;
        m_id[i] = id;
        m_name[i] = name;
        m_spec[i].ulKind = PRSPEC_PROPID;
        m_spec[i].propid = id;
        PropVariantInit(&m_var[i]);
        memset(&m_info[i], 0, sizeof(m_info[i]));
        m_info[i].lAccessFlags = access;
        m_info[i].vt = vt;
        return i;
    }
public:
    PropSet() : m_n(0) { }
    ~PropSet() { for (int i = 0; i < m_n; i++) PropVariantClear(&m_var[i]); }

    void Long(PROPID id, LPOLESTR name, ULONG access, LONG v)
    {
        int i = Next(id, name, access, VT_I4);
        m_var[i].vt = VT_I4; m_var[i].lVal = v;
        m_info[i].ValidVal.None.Dummy = 0;
    }
    void LongFlag(PROPID id, LPOLESTR name, ULONG access, LONG v, LONG validBits)
    {
        int i = Next(id, name, access, VT_I4);
        m_var[i].vt = VT_I4; m_var[i].lVal = v;
        m_info[i].ValidVal.Flag.Nom = v;
        m_info[i].ValidVal.Flag.ValidBits = validBits;
    }
    void LongRange(PROPID id, LPOLESTR name, ULONG access, LONG v, LONG nom, LONG mn, LONG mx, LONG inc)
    {
        int i = Next(id, name, access, VT_I4);
        m_var[i].vt = VT_I4; m_var[i].lVal = v;
        m_info[i].ValidVal.Range.Min = mn; m_info[i].ValidVal.Range.Nom = nom;
        m_info[i].ValidVal.Range.Max = mx; m_info[i].ValidVal.Range.Inc = inc;
    }
    void LongList(PROPID id, LPOLESTR name, ULONG access, LONG v, LONG nom, const LONG *list, int n)
    {
        int i = Next(id, name, access, VT_I4);
        if (n > MAXL) n = MAXL;
        memcpy(m_lists[i], list, sizeof(LONG) * n);
        m_var[i].vt = VT_I4; m_var[i].lVal = v;
        m_info[i].ValidVal.List.cNumList = n;
        m_info[i].ValidVal.List.Nom = nom;
        m_info[i].ValidVal.List.pList = (BYTE *)m_lists[i];
    }
    void Guid(PROPID id, LPOLESTR name, ULONG access, const GUID &g)
    {
        int i = Next(id, name, access, VT_CLSID);
        m_var[i].vt = VT_CLSID;
        m_var[i].puuid = (GUID *)CoTaskMemAlloc(sizeof(GUID));
        if (m_var[i].puuid) *m_var[i].puuid = g;
        m_info[i].ValidVal.None.Dummy = 0;
    }
    void GuidList(PROPID id, LPOLESTR name, ULONG access, const GUID *list, int n)
    {
        int i = Next(id, name, access, VT_CLSID);
        if (n > MAXL) n = MAXL;
        for (int k = 0; k < n; k++) m_glists[i][k] = list[k];
        m_var[i].vt = VT_CLSID;
        m_var[i].puuid = (GUID *)CoTaskMemAlloc(sizeof(GUID));
        if (m_var[i].puuid) *m_var[i].puuid = list[0];
        m_info[i].ValidVal.ListGuid.cNumList = n;
        m_info[i].ValidVal.ListGuid.Nom = list[0];
        m_info[i].ValidVal.ListGuid.pList = m_glists[i];
    }
    void Str(PROPID id, LPOLESTR name, ULONG access, const wchar_t *s)
    {
        int i = Next(id, name, access, VT_BSTR);
        m_var[i].vt = VT_BSTR;
        m_var[i].bstrVal = SysAllocString(s);
        m_info[i].ValidVal.None.Dummy = 0;
    }

    HRESULT Apply(BYTE *ctx)
    {
        HRESULT hr = wiasSetItemPropNames(ctx, m_n, m_id, m_name);
        if (SUCCEEDED(hr)) hr = wiasSetItemPropAttribs(ctx, m_n, m_spec, m_info);
        if (SUCCEEDED(hr)) hr = wiasWriteMultiple(ctx, m_n, m_spec, m_var);
        if (FAILED(hr)) TRACE("lide20wia: PropSet::Apply failed 0x%08lx\n", hr);
        return hr;
    }
};

#define RN    (WIA_PROP_READ | WIA_PROP_NONE)
#define RF    (WIA_PROP_READ | WIA_PROP_FLAG)
#define RWL   (WIA_PROP_READ | WIA_PROP_WRITE | WIA_PROP_LIST)
#define RWLC  (WIA_PROP_READ | WIA_PROP_WRITE | WIA_PROP_LIST  | WIA_PROP_CACHEABLE)
#define RWRC  (WIA_PROP_READ | WIA_PROP_WRITE | WIA_PROP_RANGE | WIA_PROP_CACHEABLE)
#define RWF   (WIA_PROP_READ | WIA_PROP_WRITE | WIA_PROP_FLAG)

/* ------------------------------------------------------------------ */
/* driver object                                                       */
/* ------------------------------------------------------------------ */

class INonDelegatingUnknown {
public:
    virtual STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, LPVOID *ppv) = 0;
    virtual STDMETHODIMP_(ULONG) NonDelegatingAddRef() = 0;
    virtual STDMETHODIMP_(ULONG) NonDelegatingRelease() = 0;
};

class CLide20Driver : public INonDelegatingUnknown, public IStiUSD, public IWiaMiniDrv {
public:
    CLide20Driver(LPUNKNOWN outer) : m_cRef(1), m_outer(NULL), m_stiDevice(NULL), m_root(NULL),
        m_clients(0), m_deviceId(NULL), m_formats(NULL)
    {
        m_outer = outer ? outer : reinterpret_cast<IUnknown *>(static_cast<INonDelegatingUnknown *>(this));
        InitializeCriticalSection(&m_scanLock);
    }
    ~CLide20Driver()
    {
        DestroyTree();
        if (m_deviceId) SysFreeString(m_deviceId);
        delete[] m_formats;
        DeleteCriticalSection(&m_scanLock);
    }

    /* IUnknown (delegating) */
    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv) { return m_outer->QueryInterface(riid, ppv); }
    STDMETHODIMP_(ULONG) AddRef()  { return m_outer->AddRef(); }
    STDMETHODIMP_(ULONG) Release() { return m_outer->Release(); }

    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, LPVOID *ppv)
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown))         *ppv = static_cast<INonDelegatingUnknown *>(this);
        else if (IsEqualIID(riid, IID_IStiUSD))     *ppv = static_cast<IStiUSD *>(this);
        else if (IsEqualIID(riid, IID_IWiaMiniDrv)) *ppv = static_cast<IWiaMiniDrv *>(this);
        else return E_NOINTERFACE;
        reinterpret_cast<IUnknown *>(*ppv)->AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) NonDelegatingAddRef()  { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) NonDelegatingRelease()
    {
        ULONG r = InterlockedDecrement(&m_cRef);
        if (!r) delete this;
        return r;
    }

    /* ---------------- IStiUSD ---------------- */
    STDMETHOD(Initialize)(PSTIDEVICECONTROL, DWORD, HKEY) { return S_OK; }
    STDMETHOD(GetCapabilities)(PSTI_USD_CAPS caps)
    {
        if (!caps) return E_INVALIDARG;
        memset(caps, 0, sizeof(*caps));
        caps->dwVersion     = STI_VERSION_3;
        caps->dwGenericCaps = STI_GENCAP_WIA;
        return S_OK;
    }
    STDMETHOD(GetStatus)(PSTI_DEVICE_STATUS st)
    {
        if (!st) return E_INVALIDARG;
        if (st->StatusMask & STI_DEVSTATUS_ONLINE_STATE)
            st->dwOnlineState = STI_ONLINESTATE_OPERATIONAL;
        if (st->StatusMask & STI_DEVSTATUS_EVENTS_STATE)
            st->dwEventHandlingState &= ~STI_EVENTHANDLING_PENDING;
        return S_OK;
    }
    STDMETHOD(DeviceReset)() { return S_OK; }
    STDMETHOD(Diagnostic)(LPDIAG d) { if (d) { d->sErrorInfo.dwGenericError = NOERROR; d->sErrorInfo.dwVendorError = 0; } return S_OK; }
    STDMETHOD(Escape)(STI_RAW_CONTROL_CODE, LPVOID, DWORD, LPVOID, DWORD, LPDWORD) { return STIERR_UNSUPPORTED; }
    STDMETHOD(GetLastError)(LPDWORD e) { if (e) *e = 0; return S_OK; }
    STDMETHOD(LockDevice)() { return S_OK; }
    STDMETHOD(UnLockDevice)() { return S_OK; }
    STDMETHOD(RawReadData)(LPVOID, LPDWORD, LPOVERLAPPED) { return STIERR_UNSUPPORTED; }
    STDMETHOD(RawWriteData)(LPVOID, DWORD, LPOVERLAPPED) { return STIERR_UNSUPPORTED; }
    STDMETHOD(RawReadCommand)(LPVOID, LPDWORD, LPOVERLAPPED) { return STIERR_UNSUPPORTED; }
    STDMETHOD(RawWriteCommand)(LPVOID, DWORD, LPOVERLAPPED) { return STIERR_UNSUPPORTED; }
    STDMETHOD(SetNotificationHandle)(HANDLE) { return S_OK; }
    STDMETHOD(GetNotificationData)(LPSTINOTIFY) { return STIERR_NOEVENTS; }
    STDMETHOD(GetLastErrorInfo)(STI_ERROR_INFO *e)
    {
        if (!e) return E_INVALIDARG;
        e->dwGenericError = NOERROR; e->dwVendorError = 0; e->szExtendedErrorText[0] = 0;
        return S_OK;
    }

    /* ---------------- IWiaMiniDrv ---------------- */
    STDMETHOD(drvInitializeWia)(BYTE *ctx, LONG, BSTR deviceId, BSTR rootFullName, IUnknown *stiDevice,
                                IUnknown *, IWiaDrvItem **ppRoot, IUnknown **ppInner, LONG *devErr)
    {
        if (!ctx || !ppRoot || !devErr || !ppInner) return E_INVALIDARG;
        *devErr = 0; *ppRoot = NULL; *ppInner = NULL;

        if (!m_deviceId) m_deviceId = SysAllocString(deviceId);
        m_stiDevice = reinterpret_cast<IStiDevice *>(stiDevice);

        HRESULT hr = S_OK;
        if (!m_root) hr = BuildTree(rootFullName);
        if (SUCCEEDED(hr)) {
            *ppRoot = m_root;
            InterlockedIncrement(&m_clients);
        }
        return hr;
    }

    STDMETHOD(drvAcquireItemData)(BYTE *ctx, LONG flags, PMINIDRV_TRANSFER_CONTEXT pmdtc, LONG *devErr)
    {
        if (!ctx || !pmdtc || !devErr) return E_INVALIDARG;
        *devErr = 0;
        if (!(flags & WIA_MINIDRV_TRANSFER_DOWNLOAD))
            return E_NOTIMPL;
        if (!pmdtc->pIWiaMiniDrvCallBack) return E_UNEXPECTED;

        IWiaMiniDrvTransferCallback *cb = NULL;
        HRESULT hr = pmdtc->pIWiaMiniDrvCallBack->QueryInterface(IID_IWiaMiniDrvTransferCallback, (void **)&cb);
        if (FAILED(hr)) return hr;
        hr = Download(ctx, pmdtc, cb, devErr);
        cb->Release();
        return hr;
    }

    STDMETHOD(drvInitItemProperties)(BYTE *ctx, LONG, LONG *devErr)
    {
        if (!ctx || !devErr) return E_INVALIDARG;
        *devErr = 0;
        LONG type = 0;
        HRESULT hr = wiasGetItemType(ctx, &type);
        if (FAILED(hr)) return hr;
        if (type & WiaItemTypeRoot) return InitRootProps(ctx);
        return InitFlatbedProps(ctx);
    }

    STDMETHOD(drvValidateItemProperties)(BYTE *ctx, LONG, ULONG nSpec, const PROPSPEC *spec, LONG *devErr)
    {
        if (!ctx || !spec || !devErr) return E_INVALIDARG;
        *devErr = 0;
        LONG type = 0;
        HRESULT hr = wiasGetItemType(ctx, &type);
        if (FAILED(hr)) return hr;
        if (!(type & WiaItemTypeRoot)) {
            hr = ValidateFlatbed(ctx, nSpec, spec);
            if (FAILED(hr)) return hr;
        }
        return wiasValidateItemProperties(ctx, nSpec, spec);
    }

    STDMETHOD(drvWriteItemProperties)(BYTE *, LONG, PMINIDRV_TRANSFER_CONTEXT, LONG *devErr)
    { if (devErr) *devErr = 0; return S_OK; }
    STDMETHOD(drvReadItemProperties)(BYTE *, LONG, ULONG, const PROPSPEC *, LONG *devErr)
    { if (devErr) *devErr = 0; return S_OK; }

    STDMETHOD(drvLockWiaDevice)(BYTE *, LONG, LONG *devErr)
    {
        if (devErr) *devErr = 0;
        return m_stiDevice ? m_stiDevice->LockDevice(LOCK_TIMEOUT) : S_OK;
    }
    STDMETHOD(drvUnLockWiaDevice)(BYTE *, LONG, LONG *devErr)
    {
        if (devErr) *devErr = 0;
        return m_stiDevice ? m_stiDevice->UnLockDevice() : S_OK;
    }

    STDMETHOD(drvAnalyzeItem)(BYTE *, LONG, LONG *) { return E_NOTIMPL; }
    STDMETHOD(drvGetDeviceErrorStr)(LONG, LONG, LPOLESTR *, LONG *) { return E_NOTIMPL; }
    STDMETHOD(drvDeviceCommand)(BYTE *, LONG, const GUID *, IWiaDrvItem **ppItem, LONG *devErr)
    {
        if (ppItem) *ppItem = NULL;
        if (devErr) *devErr = 0;
        return E_NOTIMPL;
    }
    STDMETHOD(drvGetCapabilities)(BYTE *, LONG, LONG *pcelt, WIA_DEV_CAP_DRV **ppCaps, LONG *devErr)
    {
        if (!pcelt || !ppCaps || !devErr) return E_INVALIDARG;
        *pcelt = 0; *ppCaps = NULL; *devErr = 0;
        return S_OK;
    }
    STDMETHOD(drvDeleteItem)(BYTE *, LONG, LONG *devErr) { if (devErr) *devErr = 0; return S_OK; }
    STDMETHOD(drvFreeDrvItemContext)(LONG, BYTE *, LONG *devErr) { if (devErr) *devErr = 0; return S_OK; }

    STDMETHOD(drvGetWiaFormatInfo)(BYTE *ctx, LONG, LONG *pcelt, WIA_FORMAT_INFO **ppwfi, LONG *devErr)
    {
        if (!pcelt || !ppwfi || !devErr) return E_INVALIDARG;
        *devErr = 0; *pcelt = 0; *ppwfi = NULL;

        BOOL image = TRUE;
        if (ctx) {
            LONG type = 0;
            if (SUCCEEDED(wiasGetItemType(ctx, &type))) image = (type & WiaItemTypeImage) != 0;
        }
        delete[] m_formats;
        m_formats = new WIA_FORMAT_INFO[1];
        if (!m_formats) return E_OUTOFMEMORY;
        m_formats[0].guidFormatID = image ? WiaImgFmt_BMP : WiaImgFmt_UNDEFINED;
        m_formats[0].lTymed       = TYMED_FILE;
        *pcelt = 1;
        *ppwfi = m_formats;
        return S_OK;
    }

    STDMETHOD(drvNotifyPnpEvent)(const GUID *, BSTR, ULONG) { return S_OK; }

    STDMETHOD(drvUnInitializeWia)(BYTE *ctx)
    {
        if (!ctx) return E_INVALIDARG;
        if (InterlockedDecrement(&m_clients) <= 0) {
            m_clients = 0;
            DestroyTree();
        }
        return S_OK;
    }

private:
    LONG              m_cRef;
    LPUNKNOWN         m_outer;
    IStiDevice       *m_stiDevice;
    IWiaDrvItem      *m_root;
    LONG              m_clients;
    BSTR              m_deviceId;
    WIA_FORMAT_INFO  *m_formats;
    CRITICAL_SECTION  m_scanLock;

    void DestroyTree()
    {
        if (m_root) {
            IWiaDrvItem *r = m_root;
            m_root = NULL;
            r->UnlinkItemTree(WiaItemTypeDisconnected);
            r->Release();
        }
    }

    HRESULT BuildTree(BSTR rootFullName)
    {
        BSTR rootName = SysAllocString(ITEM_ROOT);
        if (!rootName) return E_OUTOFMEMORY;
        HRESULT hr = wiasCreateDrvItem(WiaItemTypeFolder | WiaItemTypeDevice | WiaItemTypeRoot,
                                       rootName, rootFullName, (IWiaMiniDrv *)this, 0, NULL, &m_root);
        SysFreeString(rootName);
        if (FAILED(hr)) return hr;

        BSTR name = SysAllocString(ITEM_FLATBED);
        wchar_t full[MAX_PATH];
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", rootFullName, ITEM_FLATBED);
        BSTR fullName = SysAllocString(full);
        if (!name || !fullName) {
            SysFreeString(name); SysFreeString(fullName);
            return E_OUTOFMEMORY;
        }

        DRV_ITEM_CTX *ictx = NULL;
        IWiaDrvItem *flat = NULL;
        hr = wiasCreateDrvItem(WiaItemTypeImage | WiaItemTypeTransfer | WiaItemTypeFile |
                               WiaItemTypeProgrammableDataSource,
                               name, fullName, (IWiaMiniDrv *)this,
                               sizeof(DRV_ITEM_CTX), (BYTE **)&ictx, &flat);
        SysFreeString(name); SysFreeString(fullName);
        if (FAILED(hr)) return hr;

        ictx->category = WIA_CATEGORY_FLATBED;
        hr = flat->AddItemToFolder(m_root);
        flat->Release();
        return hr;
    }

    /* ---------------- property setup ---------------- */

    HRESULT InitRootProps(BYTE *ctx)
    {
        PropSet p;
        p.Guid(WIA_IPA_ITEM_CATEGORY, WIA_IPA_ITEM_CATEGORY_STR, RN, WIA_CATEGORY_ROOT);
        p.Long(WIA_IPA_ACCESS_RIGHTS, WIA_IPA_ACCESS_RIGHTS_STR, RF, WIA_ITEM_READ);
        p.Long(WIA_DPS_DOCUMENT_HANDLING_CAPABILITIES, WIA_DPS_DOCUMENT_HANDLING_CAPABILITIES_STR, RN, FLAT);
        p.Long(WIA_DPS_DOCUMENT_HANDLING_STATUS, WIA_DPS_DOCUMENT_HANDLING_STATUS_STR, RN, FLAT_READY);
        p.Str(WIA_DPA_FIRMWARE_VERSION, WIA_DPA_FIRMWARE_VERSION_STR, RN, L"1.0");
        p.Long(WIA_DPA_CONNECT_STATUS, WIA_DPA_CONNECT_STATUS_STR, RN, WIA_DEVICE_CONNECTED);
        p.Long(WIA_DPS_HORIZONTAL_BED_SIZE, WIA_DPS_HORIZONTAL_BED_SIZE_STR, RN, BED_W_1000);
        p.Long(WIA_DPS_VERTICAL_BED_SIZE, WIA_DPS_VERTICAL_BED_SIZE_STR, RN, BED_H_1000);
        return p.Apply(ctx);
    }

    static void ImageFormatFor(LONG dataType, LONG *depth, LONG *channels, LONG *bitsPerChannel)
    {
        switch (dataType) {
        case WIA_DATA_THRESHOLD: *depth = 1;  *channels = 1; *bitsPerChannel = 1; break;
        case WIA_DATA_GRAYSCALE: *depth = 8;  *channels = 1; *bitsPerChannel = 8; break;
        default:                 *depth = 24; *channels = 3; *bitsPerChannel = 8; break;
        }
    }

    HRESULT InitFlatbedProps(BYTE *ctx)
    {
        const LONG dpiList[]  = { 50, 75, 100, 150, 200, 300, 400, 600, 800, 1200 };
        const LONG dtypes[]   = { WIA_DATA_COLOR, WIA_DATA_GRAYSCALE, WIA_DATA_THRESHOLD };
        const LONG depths[]   = { 24, 8, 1 };
        const LONG previews[] = { WIA_FINAL_SCAN, WIA_PREVIEW_SCAN };
        const LONG comp[]     = { WIA_COMPRESSION_NONE };
        const LONG tymed[]    = { TYMED_FILE };
        const GUID formats[]  = { WiaImgFmt_BMP };
        const LONG dpi = 150;
        const LONG wpx = BED_W_1000 * dpi / 1000, hpx = BED_H_1000 * dpi / 1000;
        LONG stride = ((wpx * 24 + 31) / 32) * 4;

        PropSet p;
        p.Guid(WIA_IPA_ITEM_CATEGORY, WIA_IPA_ITEM_CATEGORY_STR, RN, WIA_CATEGORY_FLATBED);
        p.Long(WIA_IPA_ACCESS_RIGHTS, WIA_IPA_ACCESS_RIGHTS_STR, RF, WIA_ITEM_READ | WIA_ITEM_WRITE);
        p.Long(WIA_IPS_OPTICAL_XRES, WIA_IPS_OPTICAL_XRES_STR, RN, OPTICAL_DPI);
        p.Long(WIA_IPS_OPTICAL_YRES, WIA_IPS_OPTICAL_YRES_STR, RN, OPTICAL_DPI);
        p.LongList(WIA_IPS_PREVIEW, WIA_IPS_PREVIEW_STR, RWLC, WIA_FINAL_SCAN, WIA_FINAL_SCAN, previews, 2);
        p.Long(WIA_IPS_SHOW_PREVIEW_CONTROL, WIA_IPS_SHOW_PREVIEW_CONTROL_STR, RN, WIA_SHOW_PREVIEW_CONTROL);
        p.Long(WIA_IPS_MAX_HORIZONTAL_SIZE, WIA_IPS_MAX_HORIZONTAL_SIZE_STR, RN, BED_W_1000);
        p.Long(WIA_IPS_MAX_VERTICAL_SIZE, WIA_IPS_MAX_VERTICAL_SIZE_STR, RN, BED_H_1000);
        p.Long(WIA_IPS_MIN_HORIZONTAL_SIZE, WIA_IPS_MIN_HORIZONTAL_SIZE_STR, RN, 100);
        p.Long(WIA_IPS_MIN_VERTICAL_SIZE, WIA_IPS_MIN_VERTICAL_SIZE_STR, RN, 100);
        p.Long(WIA_IPS_SEGMENTATION, WIA_IPS_SEGMENTATION_STR, RN, WIA_DONT_USE_SEGMENTATION_FILTER);
        p.Long(WIA_IPS_SUPPORTS_CHILD_ITEM_CREATION, WIA_IPS_SUPPORTS_CHILD_ITEM_CREATION_STR, RN, FALSE);
        p.LongFlag(WIA_IPS_CUR_INTENT, WIA_IPS_CUR_INTENT_STR, RWF, WIA_INTENT_NONE,
                   WIA_INTENT_IMAGE_TYPE_COLOR | WIA_INTENT_IMAGE_TYPE_GRAYSCALE | WIA_INTENT_IMAGE_TYPE_TEXT |
                   WIA_INTENT_MINIMIZE_SIZE | WIA_INTENT_MAXIMIZE_QUALITY);
        p.LongList(WIA_IPS_XRES, WIA_IPS_XRES_STR, RWLC, dpi, dpi, dpiList, 10);
        p.LongList(WIA_IPS_YRES, WIA_IPS_YRES_STR, RWLC, dpi, dpi, dpiList, 10);
        p.LongRange(WIA_IPS_XPOS, WIA_IPS_XPOS_STR, RWRC, 0, 0, 0, wpx - 1, 1);
        p.LongRange(WIA_IPS_YPOS, WIA_IPS_YPOS_STR, RWRC, 0, 0, 0, hpx - 1, 1);
        p.LongRange(WIA_IPS_XEXTENT, WIA_IPS_XEXTENT_STR, RWRC, wpx, wpx, 1, wpx, 1);
        p.LongRange(WIA_IPS_YEXTENT, WIA_IPS_YEXTENT_STR, RWRC, hpx, hpx, 1, hpx, 1);
        p.LongRange(WIA_IPS_BRIGHTNESS, WIA_IPS_BRIGHTNESS_STR, RWRC, 0, 0, -1000, 1000, 1);
        p.LongRange(WIA_IPS_CONTRAST, WIA_IPS_CONTRAST_STR, RWRC, 0, 0, -1000, 1000, 1);
        p.LongList(WIA_IPA_DATATYPE, WIA_IPA_DATATYPE_STR, RWL, WIA_DATA_COLOR, WIA_DATA_COLOR, dtypes, 3);
        p.LongList(WIA_IPA_DEPTH, WIA_IPA_DEPTH_STR, RWLC, 24, 24, depths, 3);
        p.Long(WIA_IPA_CHANNELS_PER_PIXEL, WIA_IPA_CHANNELS_PER_PIXEL_STR, RN, 3);
        p.Long(WIA_IPA_BITS_PER_CHANNEL, WIA_IPA_BITS_PER_CHANNEL_STR, RN, 8);
        p.Long(WIA_IPA_PLANAR, WIA_IPA_PLANAR_STR, RN, WIA_PACKED_PIXEL);
        p.Long(WIA_IPA_PIXELS_PER_LINE, WIA_IPA_PIXELS_PER_LINE_STR, RN, wpx);
        p.Long(WIA_IPA_BYTES_PER_LINE, WIA_IPA_BYTES_PER_LINE_STR, RN, stride);
        p.Long(WIA_IPA_NUMBER_OF_LINES, WIA_IPA_NUMBER_OF_LINES_STR, RN, hpx);
        p.Long(WIA_IPA_ITEM_SIZE, WIA_IPA_ITEM_SIZE_STR, RN, 54 + stride * hpx);
        p.Guid(WIA_IPA_PREFERRED_FORMAT, WIA_IPA_PREFERRED_FORMAT_STR, RN, WiaImgFmt_BMP);
        p.GuidList(WIA_IPA_FORMAT, WIA_IPA_FORMAT_STR, RWL, formats, 1);
        p.LongList(WIA_IPA_COMPRESSION, WIA_IPA_COMPRESSION_STR, RWL, WIA_COMPRESSION_NONE, WIA_COMPRESSION_NONE, comp, 1);
        p.LongList(WIA_IPA_TYMED, WIA_IPA_TYMED_STR, RWL, TYMED_FILE, TYMED_FILE, tymed, 1);
        p.Long(WIA_IPA_BUFFER_SIZE, WIA_IPA_BUFFER_SIZE_STR, RN, 128 * 1024);
        p.Str(WIA_IPA_FILENAME_EXTENSION, WIA_IPA_FILENAME_EXTENSION_STR, RN, L"BMP");
        return p.Apply(ctx);
    }

    HRESULT ValidateFlatbed(BYTE *ctx, ULONG nSpec, const PROPSPEC *spec)
    {
        WIA_PROPERTY_CONTEXT pc;
        memset(&pc, 0, sizeof(pc));
        HRESULT hr = wiasCreatePropContext(nSpec, (PROPSPEC *)spec, 0, NULL, &pc);
        if (FAILED(hr)) return hr;

        hr = wiasUpdateValidFormat(ctx, &pc, (IWiaMiniDrv *)this);
        if (SUCCEEDED(hr)) hr = wiasUpdateScanRect(ctx, &pc, BED_W_1000, BED_H_1000);
        wiasFreePropContext(&pc);
        if (FAILED(hr)) return hr;

        /* keep the dependent image properties consistent with data type + extents */
        LONG dtype = WIA_DATA_COLOR, w = 0, h = 0;
        if (FAILED(wiasReadPropLong(ctx, WIA_IPA_DATATYPE, &dtype, NULL, TRUE))) return E_FAIL;
        if (FAILED(wiasReadPropLong(ctx, WIA_IPS_XEXTENT, &w, NULL, TRUE))) return E_FAIL;
        if (FAILED(wiasReadPropLong(ctx, WIA_IPS_YEXTENT, &h, NULL, TRUE))) return E_FAIL;

        LONG depth, ch, bpc;
        ImageFormatFor(dtype, &depth, &ch, &bpc);
        LONG stride = ((w * depth + 31) / 32) * 4;
        LONG palette = (depth == 1) ? 8 : (depth == 8 ? 1024 : 0);

        wiasWritePropLong(ctx, WIA_IPA_DEPTH, depth);
        wiasWritePropLong(ctx, WIA_IPA_CHANNELS_PER_PIXEL, ch);
        wiasWritePropLong(ctx, WIA_IPA_BITS_PER_CHANNEL, bpc);
        wiasWritePropLong(ctx, WIA_IPA_PIXELS_PER_LINE, w);
        wiasWritePropLong(ctx, WIA_IPA_BYTES_PER_LINE, stride);
        wiasWritePropLong(ctx, WIA_IPA_NUMBER_OF_LINES, h);
        wiasWritePropLong(ctx, WIA_IPA_ITEM_SIZE, 54 + palette + stride * h);
        return S_OK;
    }

    /* ---------------- transfer ---------------- */

    struct BeginArgs {
        lide20_t *s; lide20_params_t p; lide20_image_t img; int rc;
    };
    static DWORD WINAPI BeginThread(LPVOID a)
    {
        BeginArgs *b = (BeginArgs *)a;
        b->rc = lide20_scan_begin(b->s, &b->p, &b->img);
        return 0;
    }

    HRESULT Download(BYTE *ctx, PMINIDRV_TRANSFER_CONTEXT pmdtc, IWiaMiniDrvTransferCallback *cb, LONG *devErr)
    {
        UNREFERENCED_PARAMETER(pmdtc);
        LONG xres = 150, xpos = 0, ypos = 0, xext = 0, yext = 0, dtype = WIA_DATA_COLOR, bright = 0, contrast = 0;
        wiasReadPropLong(ctx, WIA_IPS_XRES, &xres, NULL, TRUE);
        wiasReadPropLong(ctx, WIA_IPS_XPOS, &xpos, NULL, TRUE);
        wiasReadPropLong(ctx, WIA_IPS_YPOS, &ypos, NULL, TRUE);
        wiasReadPropLong(ctx, WIA_IPS_XEXTENT, &xext, NULL, TRUE);
        wiasReadPropLong(ctx, WIA_IPS_YEXTENT, &yext, NULL, TRUE);
        wiasReadPropLong(ctx, WIA_IPA_DATATYPE, &dtype, NULL, TRUE);
        wiasReadPropLong(ctx, WIA_IPS_BRIGHTNESS, &bright, NULL, FALSE);
        wiasReadPropLong(ctx, WIA_IPS_CONTRAST, &contrast, NULL, FALSE);
        if (xres <= 0 || xext <= 0 || yext <= 0) return E_INVALIDARG;

        BSTR itemName = NULL, fullName = NULL;
        HRESULT hr = wiasReadPropStr(ctx, WIA_IPA_ITEM_NAME, &itemName, NULL, TRUE);
        if (SUCCEEDED(hr)) hr = wiasReadPropStr(ctx, WIA_IPA_FULL_ITEM_NAME, &fullName, NULL, TRUE);
        IStream *out = NULL;
        if (SUCCEEDED(hr)) hr = cb->GetNextStream(0, itemName, fullName, &out);
        SysFreeString(itemName); SysFreeString(fullName);
        if (hr != S_OK) return hr;

        EnterCriticalSection(&m_scanLock);
        hr = ScanToStream(out, cb, xres, xpos, ypos, xext, yext, dtype, bright, contrast, devErr);
        LeaveCriticalSection(&m_scanLock);
        out->Release();
        return hr;
    }

    static HRESULT Progress(IWiaMiniDrvTransferCallback *cb, LONG percent, ULONG bytes)
    {
        WiaTransferParams tp;
        memset(&tp, 0, sizeof(tp));
        tp.lMessage = WIA_TRANSFER_MSG_STATUS;
        tp.lPercentComplete = percent;
        tp.ulTransferredBytes = bytes;
        return cb->SendMessage(0, &tp);
    }

    HRESULT ScanToStream(IStream *out, IWiaMiniDrvTransferCallback *cb, LONG xres, LONG xpos, LONG ypos,
                         LONG xext, LONG yext, LONG dtype, LONG bright, LONG contrast, LONG *devErr)
    {
        lide20_t *s = NULL;
        int rc = lide20_open(0, &s);
        if (rc != LIDE20_OK) {
            TRACE("lide20wia: lide20_open failed: %s\n", lide20_strerror(rc));
            *devErr = WIA_ERROR_OFFLINE;
            return WIA_ERROR_OFFLINE;
        }

        BeginArgs ba;
        memset(&ba, 0, sizeof(ba));
        ba.s = s;
        ba.p.mode  = (dtype == WIA_DATA_THRESHOLD) ? LIDE20_MODE_LINEART :
                     (dtype == WIA_DATA_GRAYSCALE) ? LIDE20_MODE_GRAY : LIDE20_MODE_COLOR;
        ba.p.depth = (dtype == WIA_DATA_THRESHOLD) ? 1 : 8;
        ba.p.dpi   = xres;
        ba.p.left_mm   = xpos * 25.4 / xres;
        ba.p.top_mm    = ypos * 25.4 / xres;
        ba.p.width_mm  = xext * 25.4 / xres;
        ba.p.height_mm = yext * 25.4 / xres;
        const lide20_caps_t *caps = lide20_get_caps(s);
        if (ba.p.left_mm + ba.p.width_mm  > caps->max_width_mm)  ba.p.width_mm  = caps->max_width_mm  - ba.p.left_mm;
        if (ba.p.top_mm  + ba.p.height_mm > caps->max_height_mm) ba.p.height_mm = caps->max_height_mm - ba.p.top_mm;
        ba.p.brightness = bright / 10;
        ba.p.contrast   = contrast / 10;

        /* calibration/warm-up blocks for a while: run it aside and keep the UI informed */
        HANDLE th = CreateThread(NULL, 0, BeginThread, &ba, 0, NULL);
        if (!th) { lide20_close(s); return E_FAIL; }
        DWORD t0 = GetTickCount();
        HRESULT hr = S_OK;
        BOOL cancelled = FALSE;
        while (WaitForSingleObject(th, 400) == WAIT_TIMEOUT) {
            LONG pct = (LONG)((GetTickCount() - t0) / 1000);   /* ~1 % per second, capped below */
            if (pct > 9) pct = 9;
            if (Progress(cb, pct, 0) == S_FALSE && !cancelled) {
                cancelled = TRUE;
                lide20_cancel(s);
            }
        }
        CloseHandle(th);

        if (ba.rc != LIDE20_OK) {
            lide20_close(s);
            if (ba.rc == LIDE20_E_CANCELLED) return S_FALSE;
            *devErr = WIA_ERROR_GENERAL_ERROR;
            return WIA_ERROR_GENERAL_ERROR;
        }

        const lide20_image_t &im = ba.img;
        LONG depth = im.depth * im.channels;
        size_t stride = (((size_t)im.pixels_per_line * depth + 31) / 32) * 4;
        size_t palette = (depth == 1) ? 8 : (depth == 8 ? 1024 : 0);
        size_t dataSize = stride * (size_t)im.lines;
        size_t total = 54 + palette + dataSize;

        unsigned char *pix = (unsigned char *)VirtualAlloc(NULL, dataSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        unsigned char *line = (unsigned char *)malloc((size_t)im.bytes_per_line + 64);
        if (!pix || !line) {
            if (pix) VirtualFree(pix, 0, MEM_RELEASE);
            free(line);
            lide20_scan_end(s); lide20_close(s);
            return E_OUTOFMEMORY;
        }

        /* read the image; BMP rows are stored bottom-up */
        for (int y = 0; y < im.lines && SUCCEEDED(hr) && !cancelled; y++) {
            rc = lide20_scan_read_line(s, line);
            if (rc != LIDE20_OK) {
                hr = (rc == LIDE20_E_CANCELLED) ? S_FALSE : WIA_ERROR_GENERAL_ERROR;
                break;
            }
            unsigned char *dst = pix + stride * (size_t)(im.lines - 1 - y);
            if (im.channels == 3) {
                for (int x = 0; x < im.pixels_per_line; x++) {
                    dst[x * 3 + 0] = line[x * 3 + 2];
                    dst[x * 3 + 1] = line[x * 3 + 1];
                    dst[x * 3 + 2] = line[x * 3 + 0];
                }
            } else {
                memcpy(dst, line, (size_t)im.bytes_per_line < stride ? (size_t)im.bytes_per_line : stride);
            }
            if ((y & 15) == 0) {
                LONG pct = 10 + (LONG)((long long)y * 85 / (im.lines ? im.lines : 1));
                if (Progress(cb, pct, 0) == S_FALSE) { cancelled = TRUE; lide20_cancel(s); }
            }
        }
        free(line);
        lide20_scan_end(s);
        lide20_close(s);

        if (cancelled) hr = S_FALSE;
        if (hr == S_OK) {
            BITMAPFILEHEADER fh;
            BITMAPINFOHEADER ih;
            memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
            fh.bfType = 0x4D42;
            fh.bfSize = (DWORD)total;
            fh.bfOffBits = (DWORD)(54 + palette);
            ih.biSize = sizeof(ih);
            ih.biWidth = im.pixels_per_line; ih.biHeight = im.lines;
            ih.biPlanes = 1; ih.biBitCount = (WORD)depth; ih.biCompression = BI_RGB;
            ih.biSizeImage = (DWORD)dataSize;
            ih.biXPelsPerMeter = ih.biYPelsPerMeter = (LONG)(xres / 0.0254 + 0.5);
            ih.biClrUsed = (depth == 1) ? 2 : (depth == 8 ? 256 : 0);

            ULONG written = 0, sent = 0;
            LARGE_INTEGER zero; zero.QuadPart = 0;
            out->Seek(zero, STREAM_SEEK_END, NULL);
            hr = out->Write(&fh, sizeof(fh), &written);
            if (SUCCEEDED(hr)) hr = out->Write(&ih, sizeof(ih), &written);
            if (SUCCEEDED(hr) && palette) {
                RGBQUAD pal[256];
                int n = (depth == 1) ? 2 : 256;
                for (int i = 0; i < n; i++) {
                    BYTE v = (depth == 1) ? (i ? 0 : 255) : (BYTE)i;     /* 1-bit data: 1 = black */
                    pal[i].rgbBlue = pal[i].rgbGreen = pal[i].rgbRed = v; pal[i].rgbReserved = 0;
                }
                hr = out->Write(pal, (ULONG)palette, &written);
            }
            for (size_t off = 0; SUCCEEDED(hr) && off < dataSize; ) {
                ULONG chunk = (ULONG)((dataSize - off) > 131072 ? 131072 : (dataSize - off));
                hr = out->Write(pix + off, chunk, &written);
                off += chunk;
                sent += chunk;
                LONG pct = 95 + (LONG)((long long)off * 5 / (dataSize ? dataSize : 1));
                HRESULT pr = Progress(cb, pct, sent);
                if (pr == S_FALSE) { hr = S_FALSE; break; }
            }
        }
        VirtualFree(pix, 0, MEM_RELEASE);
        if (FAILED(hr)) *devErr = WIA_ERROR_GENERAL_ERROR;
        return hr;
    }
};

/* ------------------------------------------------------------------ */
/* class factory + DLL entry points                                    */
/* ------------------------------------------------------------------ */

class CFactory : public IClassFactory {
    LONG m_cRef;
public:
    CFactory() : m_cRef(1) { }
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID *ppv)
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  { return InterlockedIncrement(&m_cRef); }
    STDMETHOD_(ULONG, Release)() { ULONG r = InterlockedDecrement(&m_cRef); if (!r) delete this; return r; }
    STDMETHOD(CreateInstance)(IUnknown *outer, REFIID riid, void **ppv)
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = NULL;
        if (outer && !IsEqualIID(riid, IID_IUnknown)) return CLASS_E_NOAGGREGATION;
        CLide20Driver *d = new CLide20Driver(outer);
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->NonDelegatingQueryInterface(riid, ppv);
        d->NonDelegatingRelease();
        return hr;
    }
    STDMETHOD(LockServer)(BOOL) { return S_OK; }
};

extern "C" BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow(void) { return S_OK; }
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (!IsEqualCLSID(clsid, CLSID_Lide20Wia)) return CLASS_E_CLASSNOTAVAILABLE;
    CFactory *f = new CFactory;
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}
extern "C" HRESULT __stdcall DllRegisterServer()   { return S_OK; }
extern "C" HRESULT __stdcall DllUnregisterServer() { return S_OK; }
