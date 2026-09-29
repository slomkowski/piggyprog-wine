/*
 * shimtest - exercises ftd2xx.dll exactly the way PiggyProg.exe does: every entry
 * point is resolved BY ORDINAL, never by name.  That makes this a real check of the
 * .def ordinal mapping, not just of the code.
 *
 * Build:  make shimtest.exe
 * Run:    FTD2XX_SHIM_LOG=/tmp/ftd2xx.log wine ./shimtest.exe [baud]
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef ULONG FT_STATUS;
typedef PVOID FT_HANDLE;

typedef struct {
    ULONG Flags, Type, ID;
    DWORD LocId;
    char  SerialNumber[16];
    char  Description[64];
    FT_HANDLE ftHandle;
} FT_DEVICE_LIST_INFO_NODE;

typedef FT_STATUS (WINAPI *pFT_CreateDeviceInfoList)(LPDWORD);
typedef FT_STATUS (WINAPI *pFT_GetDeviceInfoList)(FT_DEVICE_LIST_INFO_NODE *, LPDWORD);
typedef FT_STATUS (WINAPI *pFT_Open)(int, FT_HANDLE *);
typedef FT_STATUS (WINAPI *pFT_Read)(FT_HANDLE, LPVOID, DWORD, LPDWORD);
typedef FT_STATUS (WINAPI *pFT_Write)(FT_HANDLE, LPVOID, DWORD, LPDWORD);
typedef FT_STATUS (WINAPI *pFT_SetBaudRate)(FT_HANDLE, ULONG);
typedef FT_STATUS (WINAPI *pFT_SetDataCharacteristics)(FT_HANDLE, UCHAR, UCHAR, UCHAR);
typedef FT_STATUS (WINAPI *pFT_SetTimeouts)(FT_HANDLE, ULONG, ULONG);
typedef FT_STATUS (WINAPI *pFT_GetStatus)(FT_HANDLE, LPDWORD, LPDWORD, LPDWORD);
typedef FT_STATUS (WINAPI *pFT_Close)(FT_HANDLE);

#define ORD(n) MAKEINTRESOURCEA(n)

static void *need(HMODULE m, int ord, const char *what)
{
    void *p = (void *)GetProcAddress(m, ORD(ord));
    printf("  ordinal %-3d %-26s %s\n", ord, what, p ? "resolved" : "*** MISSING ***");
    return p;
}

int main(int argc, char **argv)
{
    HMODULE m;
    pFT_CreateDeviceInfoList   FT_CreateDeviceInfoList;
    pFT_GetDeviceInfoList      FT_GetDeviceInfoList;
    pFT_Open                   FT_Open;
    pFT_Read                   FT_Read;
    pFT_Write                  FT_Write;
    pFT_SetBaudRate            FT_SetBaudRate;
    pFT_SetDataCharacteristics FT_SetDataCharacteristics;
    pFT_SetTimeouts            FT_SetTimeouts;
    pFT_GetStatus              FT_GetStatus;
    pFT_Close                  FT_Close;

    DWORD n = 0, rx = 0, tx = 0, ev = 0, got = 0, put = 0;
    FT_DEVICE_LIST_INFO_NODE node;
    FT_HANDLE h = NULL;
    FT_STATUS st;
    ULONG baud = (argc > 1) ? strtoul(argv[1], NULL, 10) : 115200;
    unsigned char buf[256];

    m = LoadLibraryA("ftd2xx.dll");
    if (!m) { printf("LoadLibrary(ftd2xx.dll) failed: %lu\n", GetLastError()); return 1; }
    printf("ftd2xx.dll loaded at %p\n\nResolving by ordinal (as PiggyProg does):\n", (void *)m);

    FT_CreateDeviceInfoList   = need(m, 70, "FT_CreateDeviceInfoList");
    FT_GetDeviceInfoList      = need(m, 71, "FT_GetDeviceInfoList");
    FT_Open                   = need(m,  1, "FT_Open");
    FT_Read                   = need(m,  3, "FT_Read");
    FT_Write                  = need(m,  4, "FT_Write");
    FT_SetBaudRate            = need(m,  7, "FT_SetBaudRate");
    FT_SetDataCharacteristics = need(m,  8, "FT_SetDataCharacteristics");
    FT_SetTimeouts            = need(m, 17, "FT_SetTimeouts");
    FT_GetStatus              = need(m, 21, "FT_GetStatus");
    FT_Close                  = need(m,  2, "FT_Close");

    if (!FT_CreateDeviceInfoList || !FT_GetDeviceInfoList || !FT_Open || !FT_Read ||
        !FT_Write || !FT_SetBaudRate || !FT_SetDataCharacteristics ||
        !FT_SetTimeouts || !FT_GetStatus || !FT_Close) {
        printf("\nordinal mapping is broken - fix ftd2xx.def\n");
        return 2;
    }

    printf("\nFT_CreateDeviceInfoList: ");
    st = FT_CreateDeviceInfoList(&n);
    printf("status=%lu numDevs=%lu\n", st, n);
    if (st != 0 || n == 0) { printf("no device - is /dev/ttyUSB0 present and mapped?\n"); return 3; }

    n = 1;
    memset(&node, 0, sizeof(node));
    st = FT_GetDeviceInfoList(&node, &n);
    printf("FT_GetDeviceInfoList:    status=%lu numDevs=%lu\n", st, n);
    printf("  Flags=0x%lx Type=%lu ID=0x%08lx LocId=0x%lx\n",
           node.Flags, node.Type, node.ID, node.LocId);
    printf("  Serial=\"%s\"  Description=\"%s\"\n", node.SerialNumber, node.Description);

    st = FT_Open(0, &h);
    printf("FT_Open(0):              status=%lu handle=%p\n", st, h);
    if (st != 0) return 4;

    printf("FT_SetBaudRate(%lu):    status=%lu\n", baud, FT_SetBaudRate(h, baud));
    printf("FT_SetDataCharacteristics(8,N,1): status=%lu\n",
           FT_SetDataCharacteristics(h, 8, 0, 0));
    printf("FT_SetTimeouts(500,500): status=%lu\n", FT_SetTimeouts(h, 500, 500));

    st = FT_GetStatus(h, &rx, &tx, &ev);
    printf("FT_GetStatus:            status=%lu rx=%lu tx=%lu ev=%lu\n", st, rx, tx, ev);

    /* Harmless probe: one 0x00 out, then see whether anything comes back. */
    buf[0] = 0x00;
    st = FT_Write(h, buf, 1, &put);
    printf("FT_Write(1):             status=%lu written=%lu\n", st, put);

    st = FT_Read(h, buf, sizeof(buf), &got);
    printf("FT_Read(256):            status=%lu got=%lu", st, got);
    if (got) {
        DWORD i;
        printf("  data:");
        for (i = 0; i < got && i < 32; i++) printf(" %02x", buf[i]);
    }
    printf("\n");

    printf("FT_Close:                status=%lu\n", FT_Close(h));
    printf("\nAll ordinals resolved and the port was opened successfully.\n");
    return 0;
}
