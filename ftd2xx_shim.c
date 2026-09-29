/*
 * ftd2xx_shim - a drop-in replacement for FTDI's ftd2xx.dll that works under Wine.
 *
 * Why this exists
 * ---------------
 * The stock ftd2xx.dll reaches the hardware through FTDI's Windows kernel driver
 * (ftdibus.sys).  Wine has no such driver, so the real DLL can only ever answer
 * FT_DEVICE_NOT_FOUND.  On Linux the very same chip is already driven by the kernel's
 * ftdi_sio module and exposed as /dev/ttyUSB* - which Wine surfaces as a normal COM
 * port.  This shim therefore re-implements the D2XX entry points on top of the Win32
 * serial API, and Wine does the rest.
 *
 * It is a pure PE DLL: no Unix calls, nothing to build against libftdi.  Drop it next
 * to PiggyProg.exe (or any other D2XX client that only needs a UART) and it just works.
 *
 * Configuration (environment variables, read once at load)
 * -------------------------------------------------------
 *   FTD2XX_SHIM_PORT    COM port(s) to use, comma separated, first one that opens wins.
 *                       Default: "COM2,COM1,COM3,COM4".  "\\\\.\\COM12" style also fine.
 *   FTD2XX_SHIM_DESC    Description reported by FT_GetDeviceInfoList.
 *                       Default: "Digital ECU Cheater".
 *   FTD2XX_SHIM_SERIAL  Serial number reported by FT_GetDeviceInfoList.
 *                       Default: "A7BGR844".
 *   FTD2XX_SHIM_VIDPID  ID field of the info node.  Default: 0x04036001.
 *   FTD2XX_SHIM_DTR     1 = assert DTR on open (default), 0 = leave deasserted.
 *   FTD2XX_SHIM_RTS     1 = assert RTS on open (default), 0 = leave deasserted.
 *   FTD2XX_SHIM_LOG     Path to a trace log.  Unset = no logging.
 *   FTD2XX_SHIM_TRACE   1 = also hex-dump every FT_Read/FT_Write payload.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ D2XX types */

typedef ULONG  FT_STATUS;
typedef PVOID  FT_HANDLE;

#define FT_OK                          0
#define FT_INVALID_HANDLE              1
#define FT_DEVICE_NOT_FOUND            2
#define FT_DEVICE_NOT_OPENED           3
#define FT_IO_ERROR                    4
#define FT_INSUFFICIENT_RESOURCES      5
#define FT_INVALID_PARAMETER           6
#define FT_INVALID_BAUD_RATE           7
#define FT_NOT_SUPPORTED              16
#define FT_OTHER_ERROR                18

#define FT_DEVICE_232R                 5

#define FT_BITS_8                      8
#define FT_BITS_7                      7
#define FT_STOP_BITS_1                 0
#define FT_STOP_BITS_2                 2
#define FT_PARITY_NONE                 0
#define FT_PARITY_ODD                  1
#define FT_PARITY_EVEN                 2
#define FT_PARITY_MARK                 3
#define FT_PARITY_SPACE                4

#define FT_FLOW_NONE              0x0000
#define FT_FLOW_RTS_CTS           0x0100
#define FT_FLOW_DTR_DSR           0x0200
#define FT_FLOW_XON_XOFF          0x0400

#define FT_PURGE_RX                    1
#define FT_PURGE_TX                    2

#define FT_OPEN_BY_SERIAL_NUMBER       1
#define FT_OPEN_BY_DESCRIPTION         2
#define FT_OPEN_BY_LOCATION            4

#define FT_LIST_NUMBER_ONLY   0x80000000
#define FT_LIST_BY_INDEX      0x40000000
#define FT_LIST_ALL           0x20000000

typedef struct _ft_device_list_info_node {
    ULONG     Flags;
    ULONG     Type;
    ULONG     ID;
    DWORD     LocId;
    char      SerialNumber[16];
    char      Description[64];
    FT_HANDLE ftHandle;
} FT_DEVICE_LIST_INFO_NODE;

/* The one handle we hand out.  Never dereferenced by the caller, only compared. */
#define SHIM_HANDLE ((FT_HANDLE)(ULONG_PTR)0x0D2110C0)

/* ------------------------------------------------------------------ shim state */

static CRITICAL_SECTION g_lock;
static BOOL             g_lock_ready = FALSE;

static HANDLE g_port    = INVALID_HANDLE_VALUE;
static char   g_port_open_name[64];

static char   g_cfg_ports[256]  = "COM2,COM1,COM3,COM4";
static char   g_cfg_desc[64]    = "Digital ECU Cheater";
static char   g_cfg_serial[16]  = "A7BGR844";
static DWORD  g_cfg_vidpid      = 0x04036001;
static BOOL   g_cfg_dtr         = TRUE;
static BOOL   g_cfg_rts         = TRUE;
static char   g_cfg_log[MAX_PATH];
static BOOL   g_cfg_trace       = FALSE;

/* Probe cache, so a polling caller does not open/close the tty in a tight loop. */
static DWORD  g_probe_stamp     = 0;
static BOOL   g_probe_result    = FALSE;
static BOOL   g_probe_valid     = FALSE;

/* Last DCB/timeouts, so we can re-apply them coherently. */
static DCB           g_dcb;
static COMMTIMEOUTS  g_cto;

/* ------------------------------------------------------------------ logging */

static void shim_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;

    if (!g_cfg_log[0]) return;
    f = fopen(g_cfg_log, "a");
    if (!f) return;

    fprintf(f, "[%8lu %04lx] ", (unsigned long)GetTickCount(),
            (unsigned long)GetCurrentThreadId());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void log_hex(const char *tag, const unsigned char *p, DWORD n)
{
    FILE *f;
    DWORD i;

    if (!g_cfg_log[0] || !g_cfg_trace || !n) return;
    f = fopen(g_cfg_log, "a");
    if (!f) return;

    fprintf(f, "[%8lu %04lx]   %s %lu:", (unsigned long)GetTickCount(),
            (unsigned long)GetCurrentThreadId(), tag, (unsigned long)n);
    for (i = 0; i < n && i < 256; i++) fprintf(f, " %02x", p[i]);
    if (n > 256) fprintf(f, " ...");
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------------ helpers */

static void env_str(const char *name, char *dst, DWORD cb)
{
    DWORD n = GetEnvironmentVariableA(name, dst, cb);
    if (n == 0 || n >= cb) return;              /* keep default on miss/overflow */
}

static BOOL env_bool(const char *name, BOOL dflt)
{
    char buf[16];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return dflt;
    return !(buf[0] == '0' || buf[0] == 'n' || buf[0] == 'N' ||
             buf[0] == 'f' || buf[0] == 'F');
}

static void load_config(void)
{
    char buf[32];

    GetEnvironmentVariableA("FTD2XX_SHIM_LOG", g_cfg_log, sizeof(g_cfg_log));
    env_str("FTD2XX_SHIM_PORT",   g_cfg_ports,  sizeof(g_cfg_ports));
    env_str("FTD2XX_SHIM_DESC",   g_cfg_desc,   sizeof(g_cfg_desc));
    env_str("FTD2XX_SHIM_SERIAL", g_cfg_serial, sizeof(g_cfg_serial));
    g_cfg_dtr   = env_bool("FTD2XX_SHIM_DTR",   TRUE);
    g_cfg_rts   = env_bool("FTD2XX_SHIM_RTS",   TRUE);
    g_cfg_trace = env_bool("FTD2XX_SHIM_TRACE", FALSE);

    if (GetEnvironmentVariableA("FTD2XX_SHIM_VIDPID", buf, sizeof(buf)))
        g_cfg_vidpid = (DWORD)strtoul(buf, NULL, 0);

    shim_log("---- ftd2xx_shim loaded: ports=\"%s\" desc=\"%s\" serial=\"%s\" "
         "id=0x%08lx dtr=%d rts=%d trace=%d",
         g_cfg_ports, g_cfg_desc, g_cfg_serial,
         (unsigned long)g_cfg_vidpid, g_cfg_dtr, g_cfg_rts, g_cfg_trace);
}

/* "COM2" -> "\\.\COM2"; anything already prefixed is passed through. */
static void dos_path(const char *name, char *dst, size_t cb)
{
    if (name[0] == '\\')
        _snprintf(dst, cb, "%s", name);
    else
        _snprintf(dst, cb, "\\\\.\\%s", name);
    dst[cb - 1] = 0;
}

static void trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
}

static HANDLE open_port_raw(const char *name)
{
    char path[64];
    dos_path(name, path, sizeof(path));
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
}

/* Try every configured port in order; returns an open handle or INVALID_HANDLE_VALUE. */
static HANDLE open_first_port(char *chosen, size_t chosen_cb)
{
    char list[256], *p, *next;

    _snprintf(list, sizeof(list), "%s", g_cfg_ports);
    list[sizeof(list) - 1] = 0;

    for (p = list; p && *p; p = next) {
        HANDLE h;
        next = strchr(p, ',');
        if (next) *next++ = 0;
        trim(p);
        if (!*p) continue;

        h = open_port_raw(p);
        if (h != INVALID_HANDLE_VALUE) {
            _snprintf(chosen, chosen_cb, "%s", p);
            chosen[chosen_cb - 1] = 0;
            shim_log("port \"%s\" opened", p);
            return h;
        }
        shim_log("port \"%s\" not available (err %lu)", p, (unsigned long)GetLastError());
    }
    return INVALID_HANDLE_VALUE;
}

static void apply_defaults(HANDLE h)
{
    memset(&g_dcb, 0, sizeof(g_dcb));
    g_dcb.DCBlength = sizeof(g_dcb);
    if (!GetCommState(h, &g_dcb)) {
        memset(&g_dcb, 0, sizeof(g_dcb));
        g_dcb.DCBlength = sizeof(g_dcb);
    }
    g_dcb.BaudRate     = CBR_9600;          /* D2XX power-on default */
    g_dcb.ByteSize     = 8;
    g_dcb.Parity       = NOPARITY;
    g_dcb.StopBits     = ONESTOPBIT;
    g_dcb.fBinary      = TRUE;
    g_dcb.fParity      = FALSE;
    g_dcb.fOutxCtsFlow = FALSE;
    g_dcb.fOutxDsrFlow = FALSE;
    g_dcb.fDsrSensitivity = FALSE;
    g_dcb.fOutX        = FALSE;
    g_dcb.fInX         = FALSE;
    g_dcb.fNull        = FALSE;
    g_dcb.fAbortOnError = FALSE;
    g_dcb.fDtrControl  = g_cfg_dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    g_dcb.fRtsControl  = g_cfg_rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    SetCommState(h, &g_dcb);

    /* D2XX defaults to infinite timeouts.  A hung GUI is worse than a short read,
     * so start at 5 s; every real client calls FT_SetTimeouts anyway. */
    g_cto.ReadIntervalTimeout         = 0;
    g_cto.ReadTotalTimeoutMultiplier  = 0;
    g_cto.ReadTotalTimeoutConstant    = 5000;
    g_cto.WriteTotalTimeoutMultiplier = 0;
    g_cto.WriteTotalTimeoutConstant   = 5000;
    SetCommTimeouts(h, &g_cto);

    SetupComm(h, 65536, 65536);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
}

static BOOL probe_device(void)
{
    DWORD now;
    HANDLE h;
    char chosen[64];

    if (g_port != INVALID_HANDLE_VALUE) return TRUE;

    now = GetTickCount();
    if (g_probe_valid && (now - g_probe_stamp) < 1000)
        return g_probe_result;

    h = open_first_port(chosen, sizeof(chosen));
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        g_probe_result = TRUE;
    } else {
        g_probe_result = FALSE;
    }
    g_probe_stamp = now;
    g_probe_valid = TRUE;
    return g_probe_result;
}

static BOOL handle_ok(FT_HANDLE h)
{
    return (h == SHIM_HANDLE) && (g_port != INVALID_HANDLE_VALUE);
}

/* Synchronous wrapper over an overlapped handle, so a blocking read in one thread
 * never stalls a write in another (which is exactly how real D2XX behaves). */
static BOOL sync_io(BOOL is_read, void *buf, DWORD len, DWORD *done)
{
    OVERLAPPED ov;
    BOOL ok;

    *done = 0;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return FALSE;

    ok = is_read ? ReadFile(g_port, buf, len, done, &ov)
                 : WriteFile(g_port, buf, len, done, &ov);

    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(g_port, &ov, done, TRUE);

    CloseHandle(ov.hEvent);
    return ok;
}

/* ------------------------------------------------------------------ D2XX API */

FT_STATUS WINAPI FT_CreateDeviceInfoList(LPDWORD lpdwNumDevs)
{
    BOOL present;

    if (!lpdwNumDevs) return FT_INVALID_PARAMETER;

    EnterCriticalSection(&g_lock);
    present = probe_device();
    LeaveCriticalSection(&g_lock);

    *lpdwNumDevs = present ? 1 : 0;
    shim_log("FT_CreateDeviceInfoList -> %lu", (unsigned long)*lpdwNumDevs);
    return FT_OK;
}

FT_STATUS WINAPI FT_GetDeviceInfoList(FT_DEVICE_LIST_INFO_NODE *dest, LPDWORD lpdwNumDevs)
{
    BOOL present;

    if (!lpdwNumDevs) return FT_INVALID_PARAMETER;

    EnterCriticalSection(&g_lock);
    present = probe_device();

    if (!present) {
        *lpdwNumDevs = 0;
        LeaveCriticalSection(&g_lock);
        shim_log("FT_GetDeviceInfoList -> 0 devices");
        return FT_OK;
    }

    if (dest) {
        memset(dest, 0, sizeof(*dest));
        dest->Flags = (g_port != INVALID_HANDLE_VALUE) ? 1 : 0;  /* FT_FLAGS_OPENED */
        dest->Type  = FT_DEVICE_232R;
        dest->ID    = g_cfg_vidpid;
        dest->LocId = 0;
        _snprintf(dest->SerialNumber, sizeof(dest->SerialNumber), "%s", g_cfg_serial);
        _snprintf(dest->Description,  sizeof(dest->Description),  "%s", g_cfg_desc);
        dest->SerialNumber[sizeof(dest->SerialNumber) - 1] = 0;
        dest->Description[sizeof(dest->Description) - 1]   = 0;
        dest->ftHandle = (g_port != INVALID_HANDLE_VALUE) ? SHIM_HANDLE : NULL;
    }
    *lpdwNumDevs = 1;
    LeaveCriticalSection(&g_lock);

    shim_log("FT_GetDeviceInfoList -> 1 device (\"%s\" / \"%s\")", g_cfg_desc, g_cfg_serial);
    return FT_OK;
}

FT_STATUS WINAPI FT_GetDeviceInfoDetail(DWORD index, LPDWORD flags, LPDWORD type,
                                        LPDWORD id, LPDWORD loc, PCHAR serial,
                                        PCHAR desc, FT_HANDLE *handle)
{
    BOOL present;

    EnterCriticalSection(&g_lock);
    present = probe_device();
    LeaveCriticalSection(&g_lock);

    if (index != 0 || !present) return FT_DEVICE_NOT_FOUND;

    if (flags)  *flags  = (g_port != INVALID_HANDLE_VALUE) ? 1 : 0;
    if (type)   *type   = FT_DEVICE_232R;
    if (id)     *id     = g_cfg_vidpid;
    if (loc)    *loc    = 0;
    if (serial) strcpy(serial, g_cfg_serial);
    if (desc)   strcpy(desc,   g_cfg_desc);
    if (handle) *handle = (g_port != INVALID_HANDLE_VALUE) ? SHIM_HANDLE : NULL;

    shim_log("FT_GetDeviceInfoDetail(%lu) -> ok", (unsigned long)index);
    return FT_OK;
}

FT_STATUS WINAPI FT_Open(int deviceNumber, FT_HANDLE *pHandle)
{
    HANDLE h;
    char chosen[64];

    if (!pHandle) return FT_INVALID_PARAMETER;
    *pHandle = NULL;

    if (deviceNumber != 0) {
        shim_log("FT_Open(%d) -> FT_DEVICE_NOT_FOUND (only index 0 exists)", deviceNumber);
        return FT_DEVICE_NOT_FOUND;
    }

    EnterCriticalSection(&g_lock);

    /* PiggyProg never calls FT_Close, so a reconnect must recycle the handle. */
    if (g_port != INVALID_HANDLE_VALUE) {
        shim_log("FT_Open: recycling previously open port \"%s\"", g_port_open_name);
        CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
    }
    g_probe_valid = FALSE;

    h = open_first_port(chosen, sizeof(chosen));
    if (h == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_lock);
        shim_log("FT_Open(0) -> FT_DEVICE_NOT_FOUND");
        return FT_DEVICE_NOT_FOUND;
    }

    g_port = h;
    _snprintf(g_port_open_name, sizeof(g_port_open_name), "%s", chosen);
    apply_defaults(g_port);

    LeaveCriticalSection(&g_lock);

    *pHandle = SHIM_HANDLE;
    shim_log("FT_Open(0) -> FT_OK on \"%s\"", g_port_open_name);
    return FT_OK;
}

FT_STATUS WINAPI FT_OpenEx(PVOID arg1, DWORD flags, FT_HANDLE *pHandle)
{
    shim_log("FT_OpenEx(flags=0x%lx, arg=\"%s\") -> delegating to FT_Open(0)",
         (unsigned long)flags,
         (flags & (FT_OPEN_BY_SERIAL_NUMBER | FT_OPEN_BY_DESCRIPTION)) && arg1
             ? (const char *)arg1 : "");
    return FT_Open(0, pHandle);
}

FT_STATUS WINAPI FT_Close(FT_HANDLE ftHandle)
{
    EnterCriticalSection(&g_lock);
    if (ftHandle == SHIM_HANDLE && g_port != INVALID_HANDLE_VALUE) {
        CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
        g_probe_valid = FALSE;
    }
    LeaveCriticalSection(&g_lock);
    shim_log("FT_Close -> FT_OK");
    return FT_OK;
}

FT_STATUS WINAPI FT_Read(FT_HANDLE ftHandle, LPVOID lpBuffer, DWORD dwBytesToRead,
                         LPDWORD lpBytesReturned)
{
    DWORD got = 0;
    BOOL ok;

    if (!lpBytesReturned) return FT_INVALID_PARAMETER;
    *lpBytesReturned = 0;
    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    if (!dwBytesToRead) return FT_OK;
    if (!lpBuffer) return FT_INVALID_PARAMETER;

    ok = sync_io(TRUE, lpBuffer, dwBytesToRead, &got);
    *lpBytesReturned = got;

    if (!ok) {
        DWORD e = GetLastError();
        shim_log("FT_Read(%lu) FAILED err=%lu", (unsigned long)dwBytesToRead,
             (unsigned long)e);
        return FT_IO_ERROR;
    }
    /* A short read is a timeout, not an error - that is D2XX semantics. */
    if (got != dwBytesToRead)
        shim_log("FT_Read(%lu) -> %lu (timeout)", (unsigned long)dwBytesToRead,
             (unsigned long)got);
    log_hex("RX", (const unsigned char *)lpBuffer, got);
    return FT_OK;
}

FT_STATUS WINAPI FT_Write(FT_HANDLE ftHandle, LPVOID lpBuffer, DWORD dwBytesToWrite,
                          LPDWORD lpBytesWritten)
{
    DWORD put = 0;
    BOOL ok;

    if (!lpBytesWritten) return FT_INVALID_PARAMETER;
    *lpBytesWritten = 0;
    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    if (!dwBytesToWrite) return FT_OK;
    if (!lpBuffer) return FT_INVALID_PARAMETER;

    ok = sync_io(FALSE, lpBuffer, dwBytesToWrite, &put);
    *lpBytesWritten = put;

    if (!ok) {
        shim_log("FT_Write(%lu) FAILED err=%lu", (unsigned long)dwBytesToWrite,
             (unsigned long)GetLastError());
        return FT_IO_ERROR;
    }
    log_hex("TX", (const unsigned char *)lpBuffer, put);
    return FT_OK;
}

FT_STATUS WINAPI FT_SetBaudRate(FT_HANDLE ftHandle, ULONG baud)
{
    BOOL ok;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;

    EnterCriticalSection(&g_lock);
    g_dcb.DCBlength = sizeof(g_dcb);
    g_dcb.BaudRate  = baud;
    ok = SetCommState(g_port, &g_dcb);
    LeaveCriticalSection(&g_lock);

    shim_log("FT_SetBaudRate(%lu) -> %s", (unsigned long)baud, ok ? "FT_OK" : "FAILED");
    if (!ok) {
        shim_log("  !! Wine refused this rate (err %lu).  If the device is silent, this "
             "is the reason - see the non-standard-baud fallback in the README.",
             (unsigned long)GetLastError());
        return FT_INVALID_BAUD_RATE;
    }
    return FT_OK;
}

FT_STATUS WINAPI FT_SetDivisor(FT_HANDLE ftHandle, USHORT divisor)
{
    /* FTDI base clock is 3 MHz; the two top bits encode the fractional part. */
    static const int frac[8] = { 0, 4, 2, 1, 3, 5, 6, 7 };   /* eighths */
    ULONG whole = divisor & 0x3FFF;
    ULONG baud;

    if (!whole) baud = 3000000;
    else baud = (ULONG)((3000000.0 * 8.0) / (whole * 8.0 + frac[(divisor >> 14) & 7]));

    shim_log("FT_SetDivisor(0x%04x) -> ~%lu baud", divisor, (unsigned long)baud);
    return FT_SetBaudRate(ftHandle, baud);
}

FT_STATUS WINAPI FT_SetDataCharacteristics(FT_HANDLE ftHandle, UCHAR wordLength,
                                           UCHAR stopBits, UCHAR parity)
{
    BOOL ok;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;

    EnterCriticalSection(&g_lock);
    g_dcb.DCBlength = sizeof(g_dcb);
    g_dcb.ByteSize  = wordLength;
    g_dcb.StopBits  = (stopBits == FT_STOP_BITS_2) ? TWOSTOPBITS : ONESTOPBIT;
    switch (parity) {
        case FT_PARITY_ODD:   g_dcb.Parity = ODDPARITY;   break;
        case FT_PARITY_EVEN:  g_dcb.Parity = EVENPARITY;  break;
        case FT_PARITY_MARK:  g_dcb.Parity = MARKPARITY;  break;
        case FT_PARITY_SPACE: g_dcb.Parity = SPACEPARITY; break;
        default:              g_dcb.Parity = NOPARITY;    break;
    }
    g_dcb.fParity = (parity != FT_PARITY_NONE);
    ok = SetCommState(g_port, &g_dcb);
    LeaveCriticalSection(&g_lock);

    shim_log("FT_SetDataCharacteristics(bits=%u stop=%u parity=%u) -> %s",
         wordLength, stopBits, parity, ok ? "FT_OK" : "FAILED");
    return ok ? FT_OK : FT_INVALID_PARAMETER;
}

FT_STATUS WINAPI FT_SetFlowControl(FT_HANDLE ftHandle, USHORT flowControl,
                                   UCHAR xon, UCHAR xoff)
{
    BOOL ok;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;

    EnterCriticalSection(&g_lock);
    g_dcb.DCBlength = sizeof(g_dcb);
    g_dcb.fOutxCtsFlow = (flowControl & FT_FLOW_RTS_CTS) ? TRUE : FALSE;
    g_dcb.fRtsControl  = (flowControl & FT_FLOW_RTS_CTS)
                         ? RTS_CONTROL_HANDSHAKE
                         : (g_cfg_rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE);
    g_dcb.fOutxDsrFlow = (flowControl & FT_FLOW_DTR_DSR) ? TRUE : FALSE;
    g_dcb.fDtrControl  = (flowControl & FT_FLOW_DTR_DSR)
                         ? DTR_CONTROL_HANDSHAKE
                         : (g_cfg_dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE);
    g_dcb.fOutX = g_dcb.fInX = (flowControl & FT_FLOW_XON_XOFF) ? TRUE : FALSE;
    g_dcb.XonChar  = (char)xon;
    g_dcb.XoffChar = (char)xoff;
    ok = SetCommState(g_port, &g_dcb);
    LeaveCriticalSection(&g_lock);

    shim_log("FT_SetFlowControl(0x%04x) -> %s", flowControl, ok ? "FT_OK" : "FAILED");
    return ok ? FT_OK : FT_INVALID_PARAMETER;
}

FT_STATUS WINAPI FT_SetTimeouts(FT_HANDLE ftHandle, ULONG readTimeout, ULONG writeTimeout)
{
    BOOL ok;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;

    EnterCriticalSection(&g_lock);
    /* D2XX uses a total timeout per call; 0 means "wait forever", which maps onto
     * an all-zero COMMTIMEOUTS.  Deliberately NOT an inter-character timeout. */
    g_cto.ReadIntervalTimeout         = 0;
    g_cto.ReadTotalTimeoutMultiplier  = 0;
    g_cto.ReadTotalTimeoutConstant    = readTimeout;
    g_cto.WriteTotalTimeoutMultiplier = 0;
    g_cto.WriteTotalTimeoutConstant   = writeTimeout;
    ok = SetCommTimeouts(g_port, &g_cto);
    LeaveCriticalSection(&g_lock);

    shim_log("FT_SetTimeouts(rd=%lu wr=%lu) -> %s", (unsigned long)readTimeout,
         (unsigned long)writeTimeout, ok ? "FT_OK" : "FAILED");
    return ok ? FT_OK : FT_INVALID_PARAMETER;
}

FT_STATUS WINAPI FT_GetStatus(FT_HANDLE ftHandle, LPDWORD rxBytes, LPDWORD txBytes,
                              LPDWORD eventStatus)
{
    COMSTAT cs;
    DWORD errs = 0;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;

    memset(&cs, 0, sizeof(cs));
    if (!ClearCommError(g_port, &errs, &cs)) {
        shim_log("FT_GetStatus: ClearCommError failed err=%lu", (unsigned long)GetLastError());
        return FT_IO_ERROR;
    }
    if (rxBytes)     *rxBytes     = cs.cbInQue;
    if (txBytes)     *txBytes     = cs.cbOutQue;
    if (eventStatus) *eventStatus = 0;
    return FT_OK;
}

FT_STATUS WINAPI FT_GetQueueStatus(FT_HANDLE ftHandle, LPDWORD rxBytes)
{
    DWORD tx, ev;
    return FT_GetStatus(ftHandle, rxBytes, &tx, &ev);
}

FT_STATUS WINAPI FT_GetEventStatus(FT_HANDLE ftHandle, LPDWORD eventStatus)
{
    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    if (eventStatus) *eventStatus = 0;
    return FT_OK;
}

FT_STATUS WINAPI FT_Purge(FT_HANDLE ftHandle, ULONG mask)
{
    DWORD flags = 0;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    if (mask & FT_PURGE_RX) flags |= PURGE_RXCLEAR | PURGE_RXABORT;
    if (mask & FT_PURGE_TX) flags |= PURGE_TXCLEAR | PURGE_TXABORT;
    PurgeComm(g_port, flags);
    shim_log("FT_Purge(0x%lx)", (unsigned long)mask);
    return FT_OK;
}

FT_STATUS WINAPI FT_ResetDevice(FT_HANDLE ftHandle)
{
    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    PurgeComm(g_port, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    shim_log("FT_ResetDevice -> purge only");
    return FT_OK;
}

FT_STATUS WINAPI FT_ResetPort(FT_HANDLE ftHandle)   { return FT_ResetDevice(ftHandle); }
FT_STATUS WINAPI FT_CyclePort(FT_HANDLE ftHandle)   { return FT_ResetDevice(ftHandle); }

FT_STATUS WINAPI FT_SetDtr(FT_HANDLE h)
{
    if (!handle_ok(h)) return FT_INVALID_HANDLE;
    EscapeCommFunction(g_port, SETDTR);
    shim_log("FT_SetDtr");
    return FT_OK;
}
FT_STATUS WINAPI FT_ClrDtr(FT_HANDLE h)
{
    if (!handle_ok(h)) return FT_INVALID_HANDLE;
    EscapeCommFunction(g_port, CLRDTR);
    shim_log("FT_ClrDtr");
    return FT_OK;
}
FT_STATUS WINAPI FT_SetRts(FT_HANDLE h)
{
    if (!handle_ok(h)) return FT_INVALID_HANDLE;
    EscapeCommFunction(g_port, SETRTS);
    shim_log("FT_SetRts");
    return FT_OK;
}
FT_STATUS WINAPI FT_ClrRts(FT_HANDLE h)
{
    if (!handle_ok(h)) return FT_INVALID_HANDLE;
    EscapeCommFunction(g_port, CLRRTS);
    shim_log("FT_ClrRts");
    return FT_OK;
}
FT_STATUS WINAPI FT_SetBreakOn(FT_HANDLE h)
{
    if (!handle_ok(h)) return FT_INVALID_HANDLE;
    SetCommBreak(g_port);
    return FT_OK;
}
FT_STATUS WINAPI FT_SetBreakOff(FT_HANDLE h)
{
    if (!handle_ok(h)) return FT_INVALID_HANDLE;
    ClearCommBreak(g_port);
    return FT_OK;
}

FT_STATUS WINAPI FT_GetModemStatus(FT_HANDLE ftHandle, LPDWORD modemStatus)
{
    DWORD st = 0;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    if (!GetCommModemStatus(g_port, &st)) return FT_IO_ERROR;

    /* D2XX packs the modem status into the low byte, line status into the next. */
    if (modemStatus) {
        DWORD v = 0;
        if (st & MS_CTS_ON)  v |= 0x10;
        if (st & MS_DSR_ON)  v |= 0x20;
        if (st & MS_RING_ON) v |= 0x40;
        if (st & MS_RLSD_ON) v |= 0x80;
        *modemStatus = v;
    }
    return FT_OK;
}

FT_STATUS WINAPI FT_GetDeviceInfo(FT_HANDLE ftHandle, LPDWORD devType, LPDWORD devID,
                                  PCHAR serial, PCHAR desc, LPVOID dummy)
{
    (void)dummy;
    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    if (devType) *devType = FT_DEVICE_232R;
    if (devID)   *devID   = g_cfg_vidpid;
    if (serial)  strcpy(serial, g_cfg_serial);
    if (desc)    strcpy(desc,   g_cfg_desc);
    return FT_OK;
}

FT_STATUS WINAPI FT_ListDevices(PVOID arg1, PVOID arg2, DWORD flags)
{
    BOOL present;

    EnterCriticalSection(&g_lock);
    present = probe_device();
    LeaveCriticalSection(&g_lock);

    if (flags & FT_LIST_NUMBER_ONLY) {
        if (arg1) *(LPDWORD)arg1 = present ? 1 : 0;
        shim_log("FT_ListDevices(NUMBER_ONLY) -> %d", present ? 1 : 0);
        return FT_OK;
    }
    if (!present) return FT_DEVICE_NOT_FOUND;

    if (flags & FT_LIST_BY_INDEX) {
        if ((ULONG_PTR)arg1 != 0) return FT_DEVICE_NOT_FOUND;
        if (arg2) {
            if (flags & FT_OPEN_BY_DESCRIPTION) strcpy((char *)arg2, g_cfg_desc);
            else                                strcpy((char *)arg2, g_cfg_serial);
        }
        return FT_OK;
    }
    if (flags & FT_LIST_ALL) {
        char **tbl = (char **)arg1;
        if (arg2) *(LPDWORD)arg2 = 1;
        if (tbl && tbl[0]) {
            if (flags & FT_OPEN_BY_DESCRIPTION) strcpy(tbl[0], g_cfg_desc);
            else                                strcpy(tbl[0], g_cfg_serial);
            tbl[1] = NULL;
        }
        return FT_OK;
    }
    return FT_INVALID_PARAMETER;
}

FT_STATUS WINAPI FT_GetComPortNumber(FT_HANDLE ftHandle, LPLONG comPortNumber)
{
    long n = -1;
    const char *p;

    if (!handle_ok(ftHandle)) return FT_INVALID_HANDLE;
    for (p = g_port_open_name; *p; p++)
        if (*p >= '0' && *p <= '9') { n = strtol(p, NULL, 10); break; }
    if (comPortNumber) *comPortNumber = n;
    return FT_OK;
}

FT_STATUS WINAPI FT_GetDriverVersion(FT_HANDLE ftHandle, LPDWORD ver)
{
    (void)ftHandle;
    if (ver) *ver = 0x00020C10;      /* pretend 2.12.16 */
    return FT_OK;
}

FT_STATUS WINAPI FT_GetLibraryVersion(LPDWORD ver)
{
    if (ver) *ver = 0x00020C10;
    return FT_OK;
}

/* Accepted and ignored: meaningless once the kernel driver owns the chip. */
FT_STATUS WINAPI FT_SetLatencyTimer(FT_HANDLE h, UCHAR t)       { (void)t; return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_GetLatencyTimer(FT_HANDLE h, PUCHAR t)      { if (t) *t = 16; return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_SetUSBParameters(FT_HANDLE h, ULONG i, ULONG o) { (void)i; (void)o; return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_SetResetPipeRetryCount(FT_HANDLE h, ULONG c){ (void)c; return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_SetDeadmanTimeout(FT_HANDLE h, ULONG t)     { (void)t; return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_SetChars(FT_HANDLE h, UCHAR a, UCHAR b, UCHAR c, UCHAR d) { (void)a;(void)b;(void)c;(void)d; return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_StopInTask(FT_HANDLE h)                     { return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_RestartInTask(FT_HANDLE h)                  { return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_SetEventNotification(FT_HANDLE h, DWORD m, PVOID p) { (void)m; (void)p; return handle_ok(h) ? FT_OK : FT_INVALID_HANDLE; }
FT_STATUS WINAPI FT_Rescan(void)                                { EnterCriticalSection(&g_lock); g_probe_valid = FALSE; LeaveCriticalSection(&g_lock); return FT_OK; }
FT_STATUS WINAPI FT_Reload(WORD vid, WORD pid)                  { (void)vid; (void)pid; return FT_Rescan(); }

/* Anything EEPROM- or bitbang-related cannot work through the kernel VCP driver. */
FT_STATUS WINAPI FT_SetBitMode(FT_HANDLE h, UCHAR m, UCHAR e)   { (void)h;(void)m;(void)e; return FT_NOT_SUPPORTED; }
FT_STATUS WINAPI FT_GetBitMode(FT_HANDLE h, PUCHAR m)           { (void)h; if (m) *m = 0; return FT_NOT_SUPPORTED; }
FT_STATUS WINAPI FT_EraseEE(FT_HANDLE h)                        { (void)h; return FT_NOT_SUPPORTED; }
FT_STATUS WINAPI FT_ReadEE(FT_HANDLE h, DWORD a, PWORD v)       { (void)h;(void)a; if (v) *v = 0; return FT_NOT_SUPPORTED; }
FT_STATUS WINAPI FT_WriteEE(FT_HANDLE h, DWORD a, WORD v)       { (void)h;(void)a;(void)v; return FT_NOT_SUPPORTED; }

/* ------------------------------------------------------------------ entry point */

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        InitializeCriticalSection(&g_lock);
        g_lock_ready = TRUE;
        load_config();
        break;
    case DLL_PROCESS_DETACH:
        if (g_port != INVALID_HANDLE_VALUE) {
            CloseHandle(g_port);
            g_port = INVALID_HANDLE_VALUE;
        }
        if (g_lock_ready) {
            DeleteCriticalSection(&g_lock);
            g_lock_ready = FALSE;
        }
        break;
    }
    return TRUE;
}
