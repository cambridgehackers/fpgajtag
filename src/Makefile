
CFLAGS= $(VARIANT) -Wall -fPIC -I/usr/include/libusb-1.0 -I/opt/local/include/libusb-1.0 -g

OBJS = fpgajtag.o util.o

HEADERS = dumpdata.h fpga.h util.h

fpgajtag: $(OBJS) Makefile
	gcc -g -o fpgajtag $(OBJS) -lpthread -lusb-1.0 -lz

fpgajtag.o: $(HEADERS) Makefile

util.o: $(HEADERS) Makefile

mac: $(OBJS) Makefile
	gcc -g -o fpgajtag $(OBJS) -lpthread /opt/local/lib/libusb-1.0.dylib -lz
#-lusb-1.0

install: fpgajtag
	cp fpgajtag /usr/local/bin

clean:
	rm -f $(OBJS) fpgajtag
