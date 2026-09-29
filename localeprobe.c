/*
 * localeprobe - print the regional settings Wine reports to a Windows app.
 *
 * Delphi/C++Builder's StrToFloat reads LOCALE_SDECIMAL from LOCALE_USER_DEFAULT once
 * at startup.  PiggyProg Config/MP ini files stores values like "12,8", so it only parses
 * when that separator is a comma.  This tells us which knob actually moves it.
 *
 * Build: i686-w64-mingw32-gcc -O2 -Wall -o localeprobe.exe localeprobe.c
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void show(LCID lcid, LCTYPE type, const char *name)
{
    char buf[128] = "";
    if (GetLocaleInfoA(lcid, type, buf, sizeof(buf)))
        printf("  %-22s \"%s\"\n", name, buf);
    else
        printf("  %-22s <error %lu>\n", name, GetLastError());
}

int main(void)
{
    printf("GetUserDefaultLCID()    = 0x%04x\n", (unsigned)GetUserDefaultLCID());
    printf("GetSystemDefaultLCID()  = 0x%04x\n", (unsigned)GetSystemDefaultLCID());
    printf("GetThreadLocale()       = 0x%04x\n\n", (unsigned)GetThreadLocale());

    printf("LOCALE_USER_DEFAULT:\n");
    show(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL,      "sDecimal");
    show(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND,     "sThousand");
    show(LOCALE_USER_DEFAULT, LOCALE_SLIST,         "sList");
    show(LOCALE_USER_DEFAULT, LOCALE_SENGLANGUAGE,  "sEngLanguage");
    show(LOCALE_USER_DEFAULT, LOCALE_SENGCOUNTRY,   "sEngCountry");

    printf("\nThis is what StrToFloat will use: sDecimal above must be \",\" for\n"
           "PiggyProg's \"12,8\" style config values to parse.\n");
    return 0;
}
