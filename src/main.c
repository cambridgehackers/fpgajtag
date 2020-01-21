// Copyright (c) 2014 Quanta Research Cambridge, Inc.
// Original author: John Ankcorn

// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include "util.h"
#include "fpgajtag.h"

int main(int argc, char **argv)
{
    uint32_t ret;
    int i, rflag = 0, lflag = 0, mflag = 0, cflag = 0, xflag = 0;
    int skip_idcode = 0, match_any_idcode = 0, interface = 0;
    const char *serialno = NULL;
    logfile = stdout;
    opterr = 0;
    while ((i = getopt (argc, argv, "atrxlms:ci:I:")) != -1)
        switch (i) {
        case 'a':
	    match_any_idcode = 1;
            break;
        case 'c':
            cflag = 1;
            break;
        case 'i':
            skip_idcode = atoi(optarg);
            break;
        case 'I':
            interface = atoi(optarg);
            break;
        case 'l':
            lflag = 1;
            break;
        case 'm':
            mflag = 1;
            break;
        case 'r':
            rflag = 1;
            break;
        case 's':
            serialno = optarg;
            break;
        case 't':
            trace = 1;
            break;
        case 'x':
            xflag = 1;
            break;
        default:
            goto usage;
        }

    if (optind != argc - 1 && !cflag && !lflag) {
usage:
        fprintf(stderr, "Usage %s [ -a ][ -x ] [ -l ] [ -m ] [ -t ] [ -s <serialno> ] [ -i <index> ] [ -I interface ] [ -r ] <filename>\n", argv[0]);
	fprintf(stderr, "\n"
		        "Programs Xilinx FPGA from a bitstream. The bitstream may be compressed and it may be contained an ELF executable.\n"
                        "\n"
                        "If filename is an ELF executable, reads the data from the fpgajtag section of the file, otherwise it reads the whole file.\n"
                        "\n"
                        "If the data is compressed with gzip, it is uncompressed.\n"
		        "\n"
                        "If the data has a .bit header, the header is removed.\n"
                        "\n"
                        "Unless using /dev/xdevcfg, scans USB for devices whose IDCODE matches the bitstream\n"
                        "and programs the device whose position matches index. Index defaults to 0.\n"
                        "\n"
                        "When using /dev/xdevcfg, programs the device by writing the bitstream to /dev/xdevcfg."
                        "\n"
                        "A bitstream may be embedded into ELF executable application.elf via the following command:\n"
                        "    objcopy --add-section fpgadata=system.bin.gz application.elf\n"
                        "\n"
		);
        fprintf(stderr, "Optional arguments:\n"
		        "  -a             Match any idcode\n"
                        "  -x             Write input file to /dev/xdevcfg on Zynq devices\n"
                        "  -l             Display a list of all FPGA jtag interfaces discovered on USB\n"
                        "  -m             Use FPGA Manager\n"
                        "  -s <serialno>  Use the jtag interface with the given serial number\n"
                        "  -i <index>     Program the 'index' device in the jtag chain that matches the IDCODE in the input file\n"
		        "  -I <0|1>       Which interface of FT2232 to use\n"
                        "  -t             Trace usb programming traffic\n");
        exit(1);
    }
    const char *filename = lflag ? NULL : argv[argc - 1];
    return fpgajtag_main(filename, serialno, rflag, mflag, cflag, xflag, skip_idcode, match_any_idcode, interface);
}

