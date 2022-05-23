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

#ifdef __cplusplus
extern "C" {
#endif
extern FILE *fpgajtag_logfile;

int fpgajtag_main(const char *bitstream, const char *serialport,
    int rflag, int mflag, int cflag, int xflag,
    int askip_idcode, int amatch_any_idcode, int interface, int adevice, int nflag);
int init_fpgajtag(const char *serialno, int read_idcode_only, uint32_t file_idcode, int interface);
int fpgajtag_finish(int rescan);
void fpgajtag_write_ir(int t);
uint8_t *fpgajtag_write_dr(uint8_t *tempbuf, int len);
void memdump(const uint8_t *p, int len, const char *title);
#ifdef __cplusplus
}
#endif
