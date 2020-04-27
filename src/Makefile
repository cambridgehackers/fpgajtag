
OS := $(shell uname)

#USE_LIBFTDI = 1
#VARIANT=-DUSE_LOGGING
ifeq ($(USE_LIBFTDI),1)
CFLAGS += -DUSE_LIBFTDI -I/home/$(USER)/git/system-tools/librarywrap/libftdi1-1.1/src/
FTDILIB = /scratch/Xilinx/Vivado/2013.2/lib/lnx64.o/libxftdi.so
endif
#MACUSB=/usr/local/Cellar/libusb/1.0.18
MACUSB=/usr/local/Cellar/libusb/1.0.22
CFLAGS += $(VARIANT) -Wall -fPIC -I/usr/include/libusb-1.0 -I/opt/local/include/libusb-1.0 \
    -I$(MACUSB)/include/libusb-1.0/ -g
ifeq ($(OS), Darwin)
CFLAGS += -DDARWIN
endif

PREFIX?=/usr/local
BINDIR?=$(PREFIX)/bin

OBJS = fpgajtag.o process.o cortex.o util.o
LIB = fpgajtag.a

HEADERS = dumpdata.h fpga.h util.h

ifeq ($(OS), Darwin)
    USBLIB = $(MACUSB)/lib/libusb-1.0.dylib
else
    USBLIB = -lusb-1.0
endif

fpgajtag: $(LIB) Makefile main.c
	gcc -g $(CFLAGS) -o fpgajtag main.c $(LIB) -lpthread $(FTDILIB) $(USBLIB) -lz

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

mac: fpgajtag

clean:
	rm -rf $(OBJS) $(LIB) fpgajtag obj libs
