
CFLAGS= $(VARIANT) -Wall -fPIC -I/usr/include/libusb-1.0 -I/opt/local/include/libusb-1.0 -g

fpgajtag: fpgajtag.o Makefile
	gcc -g -o fpgajtag fpgajtag.o -lpthread -lusb-1.0

mac: fpgajtag.o Makefile
	gcc -g -o fpgajtag fpgajtag.o -lpthread /opt/local/lib/libusb-1.0.dylib
#-lusb-1.0

install: fpgajtag
	cp fpgajtag /usr/local/bin

clean:
	rm -f fpgajtag.o fpgajtag
