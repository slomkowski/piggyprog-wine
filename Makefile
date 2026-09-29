CC      := i686-w64-mingw32-gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-cast-function-type -municode -DUNICODE=0
LDFLAGS := -shared -static-libgcc -Wl,--enable-stdcall-fixup

PIGGYDIR := /home/michal/.wine/drive_c/Program Files (x86)/Ottomotive/PiggyProg v1.5

all: ftd2xx.dll shimtest.exe localeprobe.exe

ftd2xx.dll: ftd2xx_shim.c ftd2xx.def
	$(CC) -O2 -Wall -Wextra -shared -static-libgcc \
	      -o $@ ftd2xx_shim.c ftd2xx.def \
	      -Wl,--enable-stdcall-fixup -Wl,--exclude-all-symbols

shimtest.exe: shimtest.c
	$(CC) -O2 -Wall -Wextra -o $@ shimtest.c

# Prints the regional settings Wine hands the app; sDecimal must be "," or every
# comma-decimal in Config/*.ini raises "is not a valid floating point value".
localeprobe.exe: localeprobe.c
	$(CC) -O2 -Wall -o $@ localeprobe.c

# Verify the export ordinals still line up with FTDI's original DLL.
check: ftd2xx.dll
	winedump -j export ftd2xx.dll | head -60

# Install next to PiggyProg.exe, preserving the original once.
install: ftd2xx.dll
	@if [ -f "$(PIGGYDIR)/ftd2xx.dll" ] && [ ! -f "$(PIGGYDIR)/ftd2xx.dll.orig-windows" ]; then \
	    mv "$(PIGGYDIR)/ftd2xx.dll" "$(PIGGYDIR)/ftd2xx.dll.orig-windows"; \
	    echo "backed up original -> ftd2xx.dll.orig-windows"; \
	fi
	cp ftd2xx.dll "$(PIGGYDIR)/ftd2xx.dll"
	@echo "installed shim into $(PIGGYDIR)"

uninstall:
	@if [ -f "$(PIGGYDIR)/ftd2xx.dll.orig-windows" ]; then \
	    mv "$(PIGGYDIR)/ftd2xx.dll.orig-windows" "$(PIGGYDIR)/ftd2xx.dll"; \
	    echo "restored FTDI original"; \
	else echo "no backup found"; fi

clean:
	rm -f ftd2xx.dll shimtest.exe localeprobe.exe

.PHONY: all check install uninstall clean
