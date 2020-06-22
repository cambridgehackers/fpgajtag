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

//#define USER_PORT_IR    32
//#define USER_PORT_IR    34
#define USER_PORT_IR    35
#define QUIET_GUARD //if ((counter % 1000) == 0)

int main(int argc, char **argv)
{
    int counter = 0;
    fpgajtag_logfile = stdout;
    if (init_fpgajtag(NULL, 0, 0xffffffff, 0) < 0)
        return -1;      // error
    int op = 0x9;
    printf("[%s:%d] IR %x\n", __FUNCTION__, __LINE__, op);
    fpgajtag_write_ir(op);
    op = USER_PORT_IR;
    printf("[%s:%d] IR %x\n", __FUNCTION__, __LINE__, op);
    fpgajtag_write_ir(op);
    static uint32_t data = 0xbeefdead;
    int len = sizeof(data);
    for (int i = 0; i < LOOP_LIMIT; i++) {  // loop length defined in Makefile
        data += 0x10010105;
        uint8_t *rdata = fpgajtag_write_dr((uint8_t *)&data, len);
        counter++;
        QUIET_GUARD
            printf("%5d out %8x in %8x\n", counter, data, *(uint32_t *)rdata);
#ifdef SLOW
        sleep(1);
#endif
    }
    return fpgajtag_finish(0);
}

