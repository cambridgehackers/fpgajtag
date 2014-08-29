
OS := $(shell uname)

CFLAGS= $(VARIANT) -Wall -fPIC -I/usr/include/libusb-1.0 -I/opt/local/include/libusb-1.0 -I/usr/local//Cellar/libusb/1.0.18/include/libusb-1.0/ -g

OBJS = fpgajtag.o util.o

HEADERS = dumpdata.h fpga.h util.h

ifeq ($(OS), Darwin)
    USBLIB = /opt/local/lib/libusb-1.0.dylib
    USBLIB = /usr/local/lib/libusb-1.0.dylib
else
    USBLIB = -lusb-1.0
endif

fpgajtag: $(OBJS) Makefile
	gcc -g -o fpgajtag $(OBJS) -lpthread $(USBLIB) -lz

fpgajtag.o: $(HEADERS) Makefile

util.o: $(HEADERS) Makefile

install: fpgajtag
	cp fpgajtag /usr/local/bin

clean:
	rm -f $(OBJS) fpgajtag
