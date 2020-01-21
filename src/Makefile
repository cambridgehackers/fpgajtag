
OS := $(shell uname)

#USE_LIBFTDI = 1
# make VARIANT="-DUSE_LOGGING"
ifeq ($(USE_LIBFTDI),1)
CFLAGS += -DUSE_LIBFTDI -I/home/$(USER)/git/system-tools/librarywrap/libftdi1-1.1/src/
FTDILIB = /scratch/Xilinx/Vivado/2013.2/lib/lnx64.o/libxftdi.so
endif
CFLAGS += $(VARIANT) -Wall -fPIC -I/usr/include/libusb-1.0 -I/opt/local/include/libusb-1.0 -I/usr/local//Cellar/libusb/1.0.18/include/libusb-1.0/ -g
ifeq ($(OS), Darwin)
CFLAGS += -DDARWIN
endif

PREFIX?=/usr/local
BINDIR?=$(PREFIX)/bin

OBJS = fpgajtag.o process.o cortex.o util.o
LIB = fpgajtag.a

HEADERS = dumpdata.h fpga.h util.h

ifeq ($(OS), Darwin)
    USBLIB = /opt/local/lib/libusb-1.0.dylib
    USBLIB = /usr/local/lib/libusb-1.0.dylib
else
    USBLIB = -lusb-1.0
endif

fpgajtag: $(LIB) Makefile main.c
	gcc -g -o fpgajtag main.c $(LIB) -lpthread $(FTDILIB) $(USBLIB) -lz

$(LIB): $(OBJS)
	rm -f $(LIB)
	ar -cr $(LIB) $(OBJS)

fpgajtag.o: $(HEADERS) Makefile
process.o: $(HEADERS) Makefile
cortex.o: $(HEADERS) Makefile
mdm.o: $(HEADERS) Makefile
util.o: $(HEADERS) Makefile

install: fpgajtag
	install -d -m755 $(DESTDIR)$(BINDIR)
	install -m4755 fpgajtag $(DESTDIR)$(BINDIR)/fpgajtag

android:
	ndk-build

clean:
	rm -rf $(OBJS) $(LIB) fpgajtag obj libs
