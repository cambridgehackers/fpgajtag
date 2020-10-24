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
#define USER_PORT_IR       34
#define TRANSFER_WIDTH     4
#define MAX_TRACE_WIDTH    100000
#define DECODE_BUFFER_SIZE 100000
#define DOLLAR "$"

static uint8_t data[MAX_TRACE_WIDTH];
static uint8_t returnBuffer[TRANSFER_WIDTH];
static char decodeBuffer[DECODE_BUFFER_SIZE];

bool inline endswith(std::string str, std::string suffix)
{
    int skipl = str.length() - suffix.length();
    return skipl >= 0 && str.substr(skipl) == suffix;
}
bool inline startswith(std::string str, std::string suffix)
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
    FILE *inputData = fopen("../../../atomicc-examples/lib/generated/AxiTop.trace", "r");
    int traceCount = -1, traceWidth = -1, traceDepth = -1;
    while (fgets(decodeBuffer, sizeof(decodeBuffer), inputData)) {
        if (!strncmp(decodeBuffer, "-----", 5))
            break;
        char *len = strstr(decodeBuffer, " ");
        *len++ = 0;
        int temp = 0;
        while (1) {
            char *endptr = nullptr;
            temp += strtol(len, &endptr, 10);
            if (*endptr == '\n')
                break;
            if (*endptr != '+') {
                printf("[%s:%d] error in decode data expression %s\n", __FUNCTION__, __LINE__, len);
            }
            len = endptr+1;
        }
        if (temp <= 1) {
            printf("[%s:%d] unable to read decode data %d\n", __FUNCTION__, __LINE__, temp);
        }
        if (!strcmp(decodeBuffer, "COUNT"))
            traceCount = temp;
        else if (!strcmp(decodeBuffer, "WIDTH"))
            traceWidth = temp;
        else if (!strcmp(decodeBuffer, "DEPTH"))
            traceDepth = temp;
        else
            printf("[%s:%d] unknown tag when reading decode data %s\n", __FUNCTION__, __LINE__, decodeBuffer);
    }
printf("[%s:%d] width %d depth %d count %d\n", __FUNCTION__, __LINE__, traceWidth, traceDepth, traceCount);
    int traceWidthBytes = (traceWidth + 7)/8;
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
    for (int i = 0; i < traceDepth; i++) {
        uint8_t *bufferp = returnBuffer;
        for (int j = 0; j < traceWidthBytes; j += TRANSFER_WIDTH) {
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
        for (int j = 0; j < traceWidthBytes - 1; j++)
            printf(" %02x", returnBuffer[j]);
        printf("\n");
    }
    endVcdFile();
    return fpgajtag_finish(0);
}

