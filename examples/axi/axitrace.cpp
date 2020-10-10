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
#include <string>
#include "fpgajtag.h"
#include "vcddump.h"

//#define USER_PORT_IR    35
#define USER_PORT_IR    34
#define DOLLAR "$"

// skip 3 bits from beginning
static bool comma[200];
static int width[] = {3, 32, 1, 1, 32, 12, 4, 1, 1, 32, 12, 4, 1, 1, 32, 12, 1, 1, 1, 32, 12, 1, 2, 1, 1, 12, 2, 1, -1};
const char *fullname[] = {"NULL", "TIME", "interrupt", "MAXIGP0_O.AR__ENA", "MAXIGP0_O.AR$addr", "MAXIGP0_O.AR$id", "MAXIGP0_O.AR$len", "MAXIGP0_O.AR__RDY", "MAXIGP0_O.AW__ENA", "MAXIGP0_O.AW$addr", "MAXIGP0_O.AW$id", "MAXIGP0_O.AW$len", "MAXIGP0_O.AW__RDY", "MAXIGP0_O.W__ENA", "MAXIGP0_O.W$data", "MAXIGP0_O.W$id", "MAXIGP0_O.W$last", "MAXIGP0_O.W__RDY", "MAXIGP0_I.R__ENA", "MAXIGP0_I.R$data", "MAXIGP0_I.R$id", "MAXIGP0_I.R$last", "MAXIGP0_I.R$resp", "MAXIGP0_I.R__RDY", "MAXIGP0_I.B__ENA", "MAXIGP0_I.B$id", "MAXIGP0_I.B$resp", "MAXIGP0_I.B__RDY", nullptr};
const char *name[] = {"", "TIME", "interrupt",
     "AR__ENA", "AR$addr", "AR$id", "AR$len", "AR__RDY",
     "AW__ENA", "AW$addr", "AW$id", "AW$len", "AW__RDY",
     "W__ENA", "W$data", "W$id", "W$last", "W__RDY",
     "R__ENA", "R$data", "R$id", "R$last", "R$resp", "R__RDY",
     "B__ENA", "B$id", "B$resp", "B__RDY", nullptr};
#define TRACE_WIDTH       245
#define TRACE_WIDTH_BYTES ((TRACE_WIDTH + 7)/8)
#define TRACE_DEPTH        1024

static uint8_t data[TRACE_WIDTH_BYTES];

static bool inline endswith(std::string str, std::string suffix)
{
    int skipl = str.length() - suffix.length();
    return skipl >= 0 && str.substr(skipl) == suffix;
}
static bool inline startswith(std::string str, std::string suffix)
{
    return str.substr(0, suffix.length()) == suffix;
}

int main(int argc, char **argv)
{
    fpgajtag_logfile = stdout;
    if (init_fpgajtag(NULL, 0, 0xffffffff, 0) < 0)
        return -1;      // error
    int op = 0x9;
    printf("[%s:%d] IR %x\n", __FUNCTION__, __LINE__, op);
    fpgajtag_write_ir(op);
    op = USER_PORT_IR;
    printf("[%s:%d] IR %x\n", __FUNCTION__, __LINE__, op);
    fpgajtag_write_ir(op);
    std::string prefix, sep;
    int index = 0;
    startVcdFile("xx.foo", fullname, width);
    printf("            ");
    comma[1] = true; // TIME
    while (name[index] != nullptr) {
        std::string fname = name[index];
        if (endswith(fname, "__ENA")) {
            fname = fname.substr(0, fname.length()-5);
            prefix = fname + DOLLAR;
            printf(" %s(", fname.c_str());
            sep = "";
            if (index > 0)
                comma[index-1] = true;
        }
        if (startswith(fname, prefix)) {
            fname = fname.substr(prefix.length());
            printf("%s", (sep + fname).c_str());
            sep = ",";
        }
        if (endswith(fname, "__RDY")) {
            printf("),      ");
        }
        index++;
    }
    printf("\n");
    for (int i = 0; i < TRACE_DEPTH; i++) {
        uint8_t *rdata = fpgajtag_write_dr((uint8_t *)&data, sizeof(data));
        for (int j = 0; j < TRACE_WIDTH_BYTES; j++)
            if (rdata[j])
                goto outlab;
        continue;
outlab:;
        printf("[%3d] ", i);
#if 1
        int iremain = 0;
        uint8_t *p = &rdata[TRACE_WIDTH_BYTES - 1];
        uint8_t idata;
        int index = 0;
        while (width[index] != -1) {
            int fwidth = width[index];
            int oremain = fwidth;
            uint32_t data = 0;
            while (oremain--) {
                if (!iremain) {
                    idata = *p--;
                    iremain = 8;
                }
                data = (data << 1) | ((idata & 0x80) >> 7);
                idata <<= 1;
                iremain--;
            }
            if (fwidth == 32)
                printf(" %8x", data);
            else if (fwidth > 8)
                printf(" %3x", data);
            else
                printf(" %x", data);
            if (comma[index])
                printf(",   ");
            std::string name = fullname[index];
            if (name == "TIME")
                outputTime(data);
            else
                outputValue(name, data);
            index++;
        }
#else
        for (int j = TRACE_WIDTH_BYTES - 1 - 4; j >= 0; j--)
            printf(" %02x", rdata[j]);
#endif
        printf("\n");
    }
    endVcdFile();
    return fpgajtag_finish(0);
}

