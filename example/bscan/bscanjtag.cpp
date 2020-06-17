// Copyright (c) 2020 The Connectal Project

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
#include <unistd.h>
#include <inttypes.h>
#include "fpgajtag.h"

int main(int argc, char **argv)
{
    fpgajtag_logfile = stdout;
    if (init_fpgajtag(NULL, 0, 0xffffffff, 0) < 0)
        return -1;      // error
    int op = 0x9;
    printf("[%s:%d] IR %x\n", __FUNCTION__, __LINE__, op);
    fpgajtag_write_ir(op);
    op = 0x22;
    printf("[%s:%d] IR %x\n", __FUNCTION__, __LINE__, op);
    fpgajtag_write_ir(op);
//static uint8_t data[] = {0xef, 0xbe, 0x1d, 0xd0, 0xef, 0xbe, 0xad, 0xde};
static uint32_t data = 0xbeefdead;
    int len = sizeof(data);
    for (int i = 0; i < 20; i++) {
        data += 5;
        uint8_t *rdata = fpgajtag_write_dr((uint8_t *)&data, len);
        sleep(1);
        printf("%5d %8x\n", i, *(uint32_t *)rdata);
        //memdump(rdata, len, "           RVAL");
    }
    return fpgajtag_finish(0);
}

