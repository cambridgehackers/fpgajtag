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
#include <string.h>
#include <string>
#include "fpgajtag.h"
#include "vcddump.h"

//#define USER_PORT_IR    35
#define USER_PORT_IR    34
#define DOLLAR "$"

#define TRACE_WIDTH       245
#define TRACE_WIDTH_BYTES ((TRACE_WIDTH + 7)/8)
#define TRACE_DEPTH        1024
#define TRANSFER_WIDTH     4

static uint8_t data[TRACE_WIDTH_BYTES];
static uint8_t returnBuffer[TRACE_WIDTH_BYTES];

bool inline endswith(std::string str, std::string suffix)
{
    int skipl = str.length() - suffix.length();
    return skipl >= 0 && str.substr(skipl) == suffix;
}
bool inline startswith(std::string str, std::string suffix)
{
    return str.substr(0, suffix.length()) == suffix;
}

#define DECODE_BUFFER_SIZE 2000
static char decodeBuffer[DECODE_BUFFER_SIZE];

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
    FILE *inputData = fopen("../../../atomicc-examples/lib/generated/AxiTop.trace", "r");
    fgets(decodeBuffer, sizeof(decodeBuffer), inputData);
    char *len = strstr(decodeBuffer, " ");
    *len++ = 0;
    int traceCount = atoi(len);
    if (traceCount <= 1) {
        printf("[%s:%d] unable to read decode data %d\n", __FUNCTION__, __LINE__, traceCount);
    }
    int *width = (int *)malloc(sizeof(int) * (traceCount + 2));
    memset(width, 0, sizeof(int) * (traceCount + 2));
    const char **fullname = (const char **)malloc(sizeof(const char *) * (traceCount + 2));
    memset(fullname, 0, sizeof(const char *) * (traceCount + 2));
    int currentItem = 0;
    while (fgets(decodeBuffer, sizeof(decodeBuffer), inputData)) {
        if (currentItem >= traceCount) {
            printf("[%s:%d] too many items in decode data %d max was %d\n", __FUNCTION__, __LINE__, currentItem, traceCount);
            exit(-1);
        }
        char *len = strstr(decodeBuffer, " ");
        *len++ = 0;
        fullname[currentItem] = strdup(decodeBuffer);
        width[currentItem] = atoi(len);
        currentItem++;
    }
    fullname[currentItem] = nullptr;
    width[currentItem] = -1;
    startVcdFile("xx.foo", fullname, width);
    for (int i = 0; i < TRACE_DEPTH; i++) {
        uint8_t *bufferp = returnBuffer;
        for (int j = 0; j < TRACE_WIDTH_BYTES; j += TRANSFER_WIDTH) {
            uint8_t *rdata = fpgajtag_write_dr((uint8_t *)&data, TRANSFER_WIDTH);
            for (int k = 0; k < TRANSFER_WIDTH; k++)
                *bufferp++ = rdata[TRANSFER_WIDTH-1 - k];
        }
        int iremain = 0;
        uint8_t *p = &returnBuffer[0];
        uint8_t idata;
        int index = 0;
        while (width[index] != -1) {
            int fwidth = width[index];
            int oremain = fwidth;
            uint32_t data = 0;
            while (oremain--) {
                if (!iremain) {
                    idata = *p;
                    p++;
                    iremain = 8;
                }
                data = (data << 1) | ((idata & 0x80) >> 7);
                idata <<= 1;
                iremain--;
            }
            std::string name = fullname[index];
            if (name == "TIME")
                outputTime(data);
            else
                outputValue(name, data);
            index++;
        }
        for (int j = 0; j < TRACE_WIDTH_BYTES - 1; j++)
            printf(" %02x", returnBuffer[j]);
        printf("\n");
    }
    endVcdFile();
    return fpgajtag_finish(0);
}

