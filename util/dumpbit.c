/* Copyright (c) 2013 Quanta Research Cambridge, Inc
 * Original author John Ankcorn
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * This utility is used to dump the '.bin' Configuration file
 * for 7 Series Xilinx FPGAs.
 * The coding/etc is taken from UG470 (v1.7).
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include "fpga.h"

#define BUFFER_SIZE 20000000
#define ITEMSIZE 101
/* from ug470, pg 91:
 * The 7 series devices are divided into two halves, the top and the bottom. All frames in
 * 7 series devices have a fixed, identical length of 3,232 bits (101 32-bit words).
 */
#define LINESIZE 8

static struct {
    uint32_t mask;
    uint32_t value;
    uint32_t type;
    char *name;
} map[] = {
    {0xffffffff, 0xffffffff, 0, "DUMMY    "},
    {0xffffffff,       0xbb, 0, "WIDTHSYNC"},
    {0xffffffff, 0x11220044, 0, "WIDTH    "},
    {0xffffffff, 0xaa995566, 0, "SYNC     "},
    {0xe0000000, 0x20000000, 1, "TYPE1    "},
    {0xe0000000, 0x40000000, 2, "TYPE2    "},
    {0,                   0, 0, "UNKNOWN  "}};

typedef struct {
    int value;
    char *name;
} MAPTYPE;

static char *opcodemap[] = {"nop   ", "read  ", "write ", "reserv"};

#define AA(A) {CONFIG_REG_ ## A, #A}
#define AB(A) {CONFIG_CMD_ ## A, #A}
MAPTYPE regmap[] = {
    AA(CRC), AA(FAR), AA(FDRI), AA(FDRO), AA(CMD), AA(CTL0), AA(MASK), AA(STAT),
    AA(LOUT), AA(COR0), AA(MFWR), AA(CBC), AA(IDCODE), AA(AXSS), AA(COR1),
    AA(WBSTAR), AA(TIMER), AA(BOOTSTS), AA(CTL1), {}};
MAPTYPE cmdmap[] = {
    AB(NULL), AB(WCFG), AB(MFW), AB(DGHIGH), AB(RCFG), AB(START), AB(RCAP),
    AB(RCRC), AB(AGHIGH), AB(SWITCH), AB(GRESTORE), AB(SHUTDOWN),
    AB(GCAPTURE), AB(DESYNC), AB(IPROG), AB(CRCC), AB(LTIMER), {}};

uint32_t buffer[BUFFER_SIZE];
static int dump_flag = 1;

static void dump_data(uint32_t *pint, int size)
{
static int itemnumber;
static int skipped;
    uint32_t t[ITEMSIZE], *p = t, nonzero = 0;
    int i;
    char title[100];
    if (!dump_flag)
        return;
    sprintf(title, "   %2x:%03x:%02x", itemnumber>>17, (itemnumber>>7) & 0x3ff, itemnumber & 0x7f);
    for (i = 0; i < size; i++) {
         t[i] = htonl(*pint);
         nonzero |= *pint++;
    }
    i = 0;
    if (nonzero) {
        if (skipped)
            printf("skipped %d\n", skipped);
        printf("%s: ",title);
        while (size > 0) {
            if (i == LINESIZE) {
                printf("\n%s: ",title);
                i = 0;
            }
            printf("%08x ", *p++);
            i++;
            size--;
        }
        printf("\n");
        skipped = 0;
    }
    else
        skipped++;
    itemnumber++;
}
int main(int argc, char **argv)
{
uint32_t *pint = buffer, *pend;
int len;
printf("[%s:%d] start\n", __FUNCTION__, __LINE__);
    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        printf("dumpbit: open failed %s\n", argv[1]);
        exit(1);
    }
    len = read(fd, buffer, sizeof(buffer));
    if (len == -1) {
        printf("dumpbit: read failed %d\n", errno);
        exit(1);
    }
    pend = pint + len/sizeof(buffer[0]);
    while (pint < pend) {
        uint32_t val = *pint++;
        val = htonl(val);
        int mapindex = 0;
        while (map[mapindex].mask && (val & map[mapindex].mask) != map[mapindex].value)
            mapindex++;
        switch(map[mapindex].type) {
        case 1: { /* Type 1 */
            int opcode = val>>27 & 0x3;
            int regnum = val>>13 & 0x3fff;
            int wordcnt = val & 0x7ff;
            char cbuf[100], *pname = cbuf;
            if (val == 0x20000000) /* dont print NOOP */
                break;
            sprintf(cbuf,"UNK_%02x ", regnum);
            int regindex = 0;
            while (regmap[regindex].name && regmap[regindex].value != regnum)
                regindex++;
            int cmdnum = htonl(*pint);
            if (regnum == 0x4 && wordcnt == 1 && (cmdnum & 0xffffff00) == 0) { /* CMD */
                pint++;
                sprintf(cbuf,"UNK_%02x ", cmdnum);
                regindex = 0;
                while (cmdmap[regindex].name && cmdmap[regindex].value != cmdnum)
                    regindex++;
                if (cmdmap[regindex].name)
                    pname = cmdmap[regindex].name;
                printf("%s: opcode %s CMD %s\n", map[mapindex].name, opcodemap[opcode], pname);
                break;
            }
            if (regmap[regindex].name)
                pname = regmap[regindex].name;
            if (regnum == 0x1 && wordcnt == 1) { /* FAR */
                pint++;
                printf("%s: opcode %s %-7s type %x top %x row %x col %x minor %x\n",
                    map[mapindex].name, opcodemap[opcode], pname,
                    (cmdnum>>23) & 7, (cmdnum>>22) & 1, (cmdnum>>17) & 0x1f,
                    (cmdnum>>7) & 0x3ff, cmdnum & 0x7f);
                break;
            }
            else
                printf("%s: opcode %s %-7s", map[mapindex].name, opcodemap[opcode], pname);
            if (wordcnt)
                printf(" :");
            while (wordcnt--) {
                uint32_t t = htonl(*pint);
                printf(" %08x", t);
                pint++;
            }
            printf("\n");
            break;
            }
        case 2: { /* Type 2 */
            int wordcnt = val & 0x7ffffff;
            int skipped = 0;
            printf("%s: wordcnt %08x\n", map[mapindex].name, wordcnt);
            while (wordcnt >= ITEMSIZE) {
                dump_data(pint, ITEMSIZE);
                wordcnt -= ITEMSIZE;
                pint += ITEMSIZE;
            }
            if (wordcnt)
                printf("residual wordcnt %d\n", wordcnt);
            dump_data(pint, wordcnt);
            pint += wordcnt;
            break;
            }
        default:
            printf("%s: %08x\n", map[mapindex].name, val);
        }
    }
printf("[%s:%d] end\n", __FUNCTION__, __LINE__);
    return 0;
}
