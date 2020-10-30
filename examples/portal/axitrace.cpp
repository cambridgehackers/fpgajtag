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
#include <assert.h>
#include <string.h>
#include <libgen.h>
#include <map>
#include "fpgajtag.h"
#include "vcddump.h"

//#define USER_PORT_IR    35
#define USER_PORT_IR       34
#define TRANSFER_WIDTH     4
#define MAX_TRACE_WIDTH    100000
#define DECODE_BUFFER_SIZE 100000

static uint8_t data[MAX_TRACE_WIDTH];
static uint8_t returnBuffer[TRANSFER_WIDTH];

TraceDescription descr[10];

static bool isName(char ch)
{
    return isalpha(ch) || ch == '_' || ch == '$';
}
static int getValue(char *len, std::map<std::string, std::string> &mapValue)
{
    int temp = 0;
    while (1) {
        char *endptr = nullptr;
        int val = 0;
        if (isName(*len)) {
            char *start = len++;
            while (isName(*len) || isdigit(*len))
                len++;
            std::string name(start, len);
            std::string vstr = mapValue[name];
            assert(vstr != "");
            val = strtol(vstr.c_str(), &endptr, 10);
        }
        else {
            val = strtol(len, &endptr, 10);
            len = endptr;
        }
        temp += val;
        if (*len == '\n')
            break;
        if (*len != '+') {
            printf("[%s:%d] error in decode data expression %s\n", __FUNCTION__, __LINE__, len);
        }
        len++; // skip '+'
    }
    if (temp <= 0) {
        printf("[%s:%d] unable to read decode data %d\n", __FUNCTION__, __LINE__, temp);
    }
    return temp;
}

static int readTraceDescription(std::string directoryName)
{
    char decodeBuffer[DECODE_BUFFER_SIZE];
    int traceIndex = 0;

    FILE *inputDict = fopen(directoryName.c_str(), "r");
    assert(inputDict);
    char *dn = strdup(directoryName.c_str());
    std::string base = dirname(dn);
    base += "/";
    std::map<std::string, std::string> mapValue;
    while (fgets(decodeBuffer, sizeof(decodeBuffer), inputDict)) {
        char *filename = strstr(decodeBuffer, "\n");
        *filename = 0;
        filename = strstr(decodeBuffer, " ");
        *filename++ = 0;
        if (decodeBuffer[0] == ':') {   // module parameter value
            mapValue[decodeBuffer+1] = filename;
//printf("[%s:%d] mapval %s val %s\n", __FUNCTION__, __LINE__, decodeBuffer+1, filename);
            continue;
        }
        std::string prefix = decodeBuffer;
        prefix += "/";
        std::string temp = base + filename;
        FILE *inputData = fopen(temp.c_str(), "r");
        assert(inputData);
        descr[traceIndex].traceCount = -1;
        descr[traceIndex].traceWidth = -1;
        descr[traceIndex].traceDepth = -1;
        while (fgets(decodeBuffer, sizeof(decodeBuffer), inputData)) {
            if (!strncmp(decodeBuffer, "-----", 5))
                break;
            char *len = strstr(decodeBuffer, " ");
            *len++ = 0;
            int temp = getValue(len, mapValue);
            if (!strcmp(decodeBuffer, "COUNT"))
                descr[traceIndex].traceCount = temp;
            else if (!strcmp(decodeBuffer, "WIDTH"))
                descr[traceIndex].traceWidth = temp;
            else if (!strcmp(decodeBuffer, "DEPTH"))
                descr[traceIndex].traceDepth = temp;
            else
                printf("[%s:%d] unknown tag when reading decode data %s\n", __FUNCTION__, __LINE__, decodeBuffer);
        }
        printf("[%s:%d] width %d depth %d count %d\n", __FUNCTION__, __LINE__, descr[traceIndex].traceWidth, descr[traceIndex].traceDepth, descr[traceIndex].traceCount);
        descr[traceIndex].traceWidthBytes = (descr[traceIndex].traceWidth + 7)/8;
        int currentItem = 0;
        while (fgets(decodeBuffer, sizeof(decodeBuffer), inputData)) {
            if (currentItem >= descr[traceIndex].traceCount) {
                printf("[%s:%d] too many items in decode data %d max was %d\n", __FUNCTION__, __LINE__, currentItem, descr[traceIndex].traceCount);
                exit(-1);
            }
            char *len = strstr(decodeBuffer, " ");
            *len++ = 0;
            std::string name = decodeBuffer;
            if (name != "TIME")
                name = prefix + name;
            int ind;
            while ((ind = name.find(PERIOD)) > 0)
                name = name.substr(0, ind) + "_" + name.substr(ind+1);
            descr[traceIndex].fullname.push_back(name);
            int temp = getValue(len, mapValue);
            descr[traceIndex].width.push_back(temp);
            currentItem++;
        }
        fclose(inputData);
        traceIndex++;
        mapValue.clear();
    }
    fclose(inputDict);
    return traceIndex;
}

static void readTraceData(int traceIndex)
{
    for (int i = 0; i < descr[traceIndex].traceDepth; i++) {
        uint8_t *bufferp = returnBuffer;
        for (int j = 0; j < descr[traceIndex].traceWidthBytes; j += TRANSFER_WIDTH) {
            uint8_t *rdata = fpgajtag_write_dr((uint8_t *)&data, TRANSFER_WIDTH);
            for (int k = 0; k < TRANSFER_WIDTH; k++)
                *bufferp++ = rdata[TRANSFER_WIDTH-1 - k];
        }
        int iremain = 0;
        uint8_t *p = &returnBuffer[0];
        uint8_t idata;
        auto namep = descr[traceIndex].fullname.begin();
        for (auto fwidth : descr[traceIndex].width) {
            std::string name = *namep++;
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
            if (name == "TIME")
                outputTime(data);
            else
                outputValue(name, data);
        }
        for (int j = 0; j < descr[traceIndex].traceWidthBytes - 1; j++)
            printf(" %02x", returnBuffer[j]);
        printf("\n");
    }
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
    int traceLimit = readTraceDescription("../../../atomicc-examples/examples/gray/generated/ZynqTop.dict");
#if 0
    for (int index = 0; index < traceLimit; index++) {
printf("[%s:%d] index %d count %d width %d widthbytes %d depth %d\n", __FUNCTION__, __LINE__, index, descr[index].traceCount,
    descr[index].traceWidth, descr[index].traceWidthBytes, descr[index].traceDepth);
printf("[%s:%d] width", __FUNCTION__, __LINE__);
        for (auto item: descr[index].width)
            printf(" %d,", item);
printf("\n");
printf("[%s:%d] fullname", __FUNCTION__, __LINE__);
        for (auto item: descr[index].fullname)
            printf(" %s,", item.c_str());
printf("\n");
    }
#endif
    startVcdFile("xx.foo", traceLimit);
    readTraceData(0);
    endVcdFile();
    return fpgajtag_finish(0);
}

