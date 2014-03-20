
CFLAGS= $(VARIANT) -Wall -fPIC -I/usr/include/libusb-1.0 -g

fpgajtag: fpgajtag.o Makefile
	gcc -g -o fpgajtag fpgajtag.o -lpthread -lusb-1.0

install: fpgajtag
	cp fpgajtag /usr/local/bin

clean:
	rm -f fpgajtag.o fpgajtag
