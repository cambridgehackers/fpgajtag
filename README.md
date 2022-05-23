

This is a simple jtag programmer for Xilinx FPGAs

On Ubuntu, before compiling be sure to:
    sudo apt-get install libusb-1.0-0-dev

On Centos, before compiling be sure to:
    sudo yum install libusb1-devel

On Mac OSX, before compiling be sure to:
    sudo port install libusb
or
    brew install libusb

To make android version:
    make android


In the util directory, there are 2 tools:
   1) dumpbit: formatted dump of xilinx bit file
   2) readll: an attempt to parse xilinx vivado 'll' files

=========================
Note that on OSX, the Apple driver captures all ftdi devices, prohibiting
other users (like fpgajtag) from opening them.  Before using fpgajtag,
the following command must be executed:
    sudo kextunload -b com.apple.driver.AppleUSBFTDI

=========================
To read out Zedboard AXI trace info:
    fpgajtag -c testdata/zedtrace.jtag >trace.xx.tempfile
