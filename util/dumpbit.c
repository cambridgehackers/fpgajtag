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
/* from ug470, pg 91:
 * The 7 series devices are divided into two halves, the top and the bottom. All frames in
 * 7 series devices have a fixed, identical length of 3,232 bits (101 32-bit words).
 */
#define LINESIZE 8

static int dump_flag = 1;
static int dump_file = 0;
static int fd_out = -1;

typedef struct {
    uint32_t value;
    char    *name;
    uint32_t mask;
} MAPTYPE;

static MAPTYPE map[] = {
    {    CONFIG_DUMMY, "CONFIG_DUMMY"},
    //{            0xbb, "WIDTHSYNC"},
    //{      0x11220044, "WIDTH"},
    {     CONFIG_SYNC, "CONFIG_SYNC"}, {}};

static char *opcodemap[] = {"CONFIG_OP_NOP", "CONFIG_OP_READ", "CONFIG_OP_WRITE", "CONFIG_OP_RESERVED"};

static MAPTYPE regmap[] = {
#define AA(A) {CONFIG_REG_ ## A, "CONFIG_REG_" # A}
    AA(CRC), AA(FAR), AA(FDRI), AA(FDRO), AA(CMD), AA(CTL0), AA(MASK), AA(STAT),
    AA(LOUT), AA(COR0), AA(MFWR), AA(CBC), AA(IDCODE), AA(AXSS), AA(COR1),
    AA(WBSTAR), AA(TIMER), AA(BOOTSTS), AA(CTL1), {}};
static MAPTYPE cmdmap[] = {
#define AB(A) {CONFIG_CMD_ ## A, "CONFIG_CMD_" # A}
    AB(NULL), AB(WCFG), AB(MFW), AB(DGHIGH), AB(RCFG), AB(START), AB(RCAP),
    AB(RCRC), AB(AGHIGH), AB(SWITCH), AB(GRESTORE), AB(SHUTDOWN),
    AB(GCAPTURE), AB(DESYNC), AB(IPROG), AB(CRCC), AB(LTIMER), {}};

static uint32_t buffer[BUFFER_SIZE];
static int skipped;

static void dump_data(uint32_t *pint, int size)
{
static int itemnumber;
    uint32_t t[BITFILE_ITEMSIZE], *p = t, nonzero = 0;
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
    if (nonzero || fd_out != -1) {
        if (skipped)
            printf("skipped %d\n", skipped);
        skipped = 0;
        if (fd_out == -1)
            printf("%s: ",title);
        while (size > 0) {
            if (i == LINESIZE) {
                printf("\n%s: ",title);
                i = 0;
            }
            if (fd_out >= 0) {
                write(fd_out, p, sizeof(*p));
                p++;
            }
            else {
                printf("%08x ", *p++);
                i++;
            }
            size--;
        }
        if (fd_out == -1)
            printf("\n");
    }
    else
        skipped++;
    itemnumber++;
}
static uint32_t *pint = buffer;
static uint32_t get_next()
{
    uint32_t ret = htonl(*pint);
    pint++;
    return ret;
}
int main(int argc, char **argv)
{
uint32_t *pend;
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
    if (dump_file)
        fd_out = creat("xx.orig.dump", 0666);
    while (pint < pend) {
        uint32_t val = get_next();
        switch(val & CONFIG_TYPE_MASK) {
        case CONFIG_TYPE1(0,0,0): { /* Type 1 */
            int opcode = (val >> CONFIG_TYPE1_OPCODE_SHIFT) & CONFIG_TYPE1_OPCODE_MASK;
            int regnum = (val >> CONFIG_TYPE1_REG_SHIFT) & CONFIG_TYPE1_REG_MASK;
            int wordcnt = val & CONFIG_TYPE1_WORDCNT_MASK;
            char cbuf[100], *pname = cbuf;
            int regindex = 0;
            sprintf(cbuf,"0x%02x ", regnum);
            while (regmap[regindex].name && regmap[regindex].value != regnum)
                regindex++;
            if (regmap[regindex].name)
                pname = regmap[regindex].name;

            if (opcode == CONFIG_OP_NOP) /* dont print NOOP */
                break;
            printf("CONFIG_TYPE1(%s,%s", opcodemap[opcode], pname);
            if (regnum == CONFIG_REG_CMD && wordcnt == 1 && (htonl(*pint) & 0xffffff00) == 0) { /* CMD */
                int cmdindex = 0;
                int cmdnum = get_next();
                sprintf(cbuf,"0x%02x ", cmdnum);
                while (cmdmap[cmdindex].name && cmdmap[cmdindex].value != cmdnum)
                    cmdindex++;
                if (cmdmap[cmdindex].name)
                    pname = cmdmap[cmdindex].name;
                printf(",1), %s,\n", pname);
                break;
            }
            //if (regnum == CONFIG_REG_FAR && wordcnt == 1) { /* FAR */
                //int cmdnum = get_next();
                //printf("), 0x08x", cmdnum);
                //printf(") type %x top %x row %x col %x minor %x\n",
                //    (cmdnum>>23) & 7, (cmdnum>>22) & 1, (cmdnum>>17) & 0x1f,
                //    (cmdnum>>7) & 0x3ff, cmdnum & 0x7f);
                //break;
            //}
            printf(",%d), ", wordcnt);
            while (wordcnt--)
                printf(" 0x%08x,", get_next());
            printf("\n");
            break;
            }
        case CONFIG_TYPE2(0): { /* Type 2 */
            int wordcnt = val & 0x7ffffff;
            printf("CONFIG_TYPE2(0x%08x)\n", wordcnt);
            while (wordcnt >= BITFILE_ITEMSIZE) {
                dump_data(pint, BITFILE_ITEMSIZE);
                wordcnt -= BITFILE_ITEMSIZE;
                pint += BITFILE_ITEMSIZE;
                if (wordcnt && pint >= pend) {
                    printf("[%s:%d] pint %p pend %p wordcnt %d\n", __FUNCTION__, __LINE__, pint, pend, wordcnt);
                    wordcnt = 0;
                    break;
                }
            }
            if (wordcnt)
                printf("residual wordcnt %d\n", wordcnt);
            dump_data(pint, wordcnt);
            pint += wordcnt;
            if (skipped)
                printf("skipped %d\n", skipped);
            break;
            }
        default: {
            int mapindex = 0;
            while (map[mapindex].name && val != map[mapindex].value)
                mapindex++;
            if (map[mapindex].name)
                printf("%s,\n", map[mapindex].name);
            else
                printf("0x%08x,\n", val);
            }
        }
    }
printf("[%s:%d] end\n", __FUNCTION__, __LINE__);
    return 0;
}
