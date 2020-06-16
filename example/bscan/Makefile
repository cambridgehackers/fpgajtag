
OS := $(shell uname)
LIBDIR = ../../src

#USE_LIBFTDI = 1
#VARIANT=-DUSE_LOGGING
ifeq ($(USE_LIBFTDI),1)
CFLAGS += -DUSE_LIBFTDI
FTDILIB = /scratch/Xilinx/Vivado/2013.2/lib/lnx64.o/libxftdi.so
endif
#MACUSB=/usr/local/Cellar/libusb/1.0.18
MACUSB=/usr/local/Cellar/libusb/1.0.22
CFLAGS += $(VARIANT) -Wall -fPIC -g -I$(LIBDIR)
ifeq ($(OS), Darwin)
CFLAGS += -DDARWIN
endif

LIB = $(LIBDIR)/fpgajtag.a

ifeq ($(OS), Darwin)
    USBLIB = $(MACUSB)/lib/libusb-1.0.dylib
else
    USBLIB = -lusb-1.0
endif

bscan: $(LIB) Makefile bscanjtag.cpp
	gcc -g $(CFLAGS) -o $@ bscanjtag.cpp $(LIB) -lpthread $(FTDILIB) $(USBLIB) -lz

android:
	ndk-build

clean:
	rm -rf bscan
