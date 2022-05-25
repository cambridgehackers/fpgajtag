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
#include <time.h>
#include <map>
#include "vcddump.h"

typedef struct {
    std::string code;
    int         width;
    bool        seen;
} SymbolInfo;

static int symbolIndex;
static std::map<std::string, SymbolInfo> symbolMap;
static std::map<std::string, uint32_t> currentValue;
static FILE *outputFile;

static inline std::string autostr(uint64_t X, bool isNeg = false) {
  char Buffer[21];
  char *BufPtr = std::end(Buffer);

  if (X == 0) *--BufPtr = '0';  // Handle special case...

  while (X) {
    *--BufPtr = '0' + char(X % 10);
    X /= 10;
  }

  if (isNeg) *--BufPtr = '-';   // Add negative sign...
  return std::string(BufPtr, std::end(Buffer));
}

static std::string createCode(int identifier)
{
    std::string ret;
    bool first = true;
    while (first || identifier--) {
        ret += ('!' + identifier % 94);
        identifier /= 94;
        first = false;
    }
    return ret;
}

static std::string createBinary(uint32_t value, int width)
{
    std::string ret;
    while (width--) {
        ret = (char)('0' + (value & 1)) + ret;
        value >>= 1;
    }
    return ret;
}

void outputTime(uint32_t value)
{
    fprintf(outputFile, "#%d\n", value);
}

void outputValue(std::string name, uint32_t value)
{
    auto &lookup = symbolMap[name];
    if (!lookup.seen || currentValue[name] != value) {
        lookup.seen = true;
        currentValue[name] = value;
        if (lookup.width == 1)
            fprintf(outputFile, "%x%s\n", value & 1, lookup.code.c_str());
        else {
            std::string binary = createBinary(value, lookup.width);
            fprintf(outputFile, "b%s %s\n", binary.c_str(), lookup.code.c_str());
        }
    }
}

#define PREFIX_DEPTH    1000
static std::string prefixStack[PREFIX_DEPTH];
static int prefixIndex;
void startVcdFile(std::string filename, int maxDescr)
{
    time_t rawtime;
    time (&rawtime);
    struct tm *timeinfo = localtime(&rawtime);

    outputFile = fopen(filename.c_str(), "w");
    fprintf(outputFile, "$version Generated by fpgajtag $end\n");
    fprintf(outputFile, "$date %s $end\n", asctime(timeinfo));
    fprintf(outputFile, "$timescale   1ps $end\n\n");
    fprintf(outputFile, " $scope module TOP $end\n");
    for (int i = 0; i < maxDescr; i++) {
        auto names = descr[i].fullname.begin();
        for (auto sWidth: descr[i].width) {
            std::string sName = *names++;
            std::string orig = sName;
            if (symbolMap[sName].width) {
printf("[%s:%d] SYMBOLDEF %s\n", __FUNCTION__, __LINE__, sName.c_str());
                continue;
            }
            int ind;
//printf("[%s:%d]checkprefix %s sname %s\n", __FUNCTION__, __LINE__, prefix.c_str(), sName.c_str());
            while(prefixIndex > 0) { // handle popping prefix stack
                int len = prefixStack[prefixIndex].length();
                if (sName.substr(0, len) == prefixStack[prefixIndex]) {
                    sName = sName.substr(len+1);
                    break;
                }
                fprintf(outputFile, "    $upscope $end\n");
                prefixIndex--;
            }
            while ((ind = sName.find(PERIOD)) > 0) { // handle pushing prefix stack
                std::string component = sName.substr(0, ind);
                sName = sName.substr(ind+1);
                std::string prefix;
                if (prefixIndex)
                    prefix = prefixStack[prefixIndex] + PERIOD;
                prefixStack[++prefixIndex] = prefix + component;
                fprintf(outputFile, "    $scope module  %s $end\n", component.c_str());
            }
            std::string code = createCode(++symbolIndex);
            std::string vecCount;
            if (sWidth > 1)
                vecCount = " [" + autostr(sWidth-1) + ":0]";
            symbolMap[orig] = {code, sWidth, false};
            fprintf(outputFile, "    $var wire  %d %s %s%s $end\n", sWidth, code.c_str(), sName.c_str(), vecCount.c_str());
        }
    }
    while (prefixIndex-- > 0) {
        fprintf(outputFile, "    $upscope $end\n");
    }
    fprintf(outputFile, "$enddefinitions $end\n\n\n");
}

void endVcdFile()
{
    fclose(outputFile);
    outputFile = nullptr;
}