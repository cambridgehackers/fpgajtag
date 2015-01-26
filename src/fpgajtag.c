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
// FTDI interface documented at:
//     http://www.ftdichip.com/Documents/AppNotes/AN2232C-01_MPSSE_Cmnd.pdf
// Xilinx Series7 Configuation documented at:
//     ug470_7Series_Config.pdf
// ARM JTAG-DP registers documented at:
//     DDI0314H_coresight_components_trm.pdf
// ARM DPACC/APACC programming documented at:
//     IHI0031C_debug_interface_as.pdf

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include "util.h"
#include "fpga.h"

#define FILE_READSIZE          6464
#define MAX_SINGLE_USB_DATA    4046
#define IDCODE_ARRAY_SIZE        20
#define SEGMENT_LENGTH   256 /* sizes above 256bytes seem to get more bytes back in response than were requested */

uint8_t *input_fileptr;
int input_filesize, found_cortex;
int not_last_id, jtag_index = -1, device_type, dcount, idcode_count;
int tracep ;//= 1;

static int verbose, trailing_count, skip_idcode;
static USB_INFO *uinfo;

static int first_time_idcode_read = 1;
static uint32_t idcode_array[IDCODE_ARRAY_SIZE];
static uint32_t idcode_len[IDCODE_ARRAY_SIZE];
static uint8_t *rstatus = DITEM(CONFIG_DUMMY,
            CONFIG_SYNC, CONFIG_TYPE2(0),
            CONFIG_TYPE1(CONFIG_OP_READ, CONFIG_REG_STAT, 1), SINT32(0));
static void write_fill(struct ftdi_context *ftdi, int read, int width, int tail);

#ifndef USE_MDM
void access_mdm(struct ftdi_context *ftdi, int version, int pre, int amatch)
{
    flush_write(ftdi, DITEM(TMSW, 2, 0xe7)); /* strange pattern, so we can find in trace log */
}
#endif

/*
 * Support for GPIOs from Digilent JTAG module to h/w design.
 *
 * SMT1 does not have any external GPIO connections (KC705).
 *
 * SMT2 has GPIO0/1/2 for user use.  In the datasheet for
 * the SMT2, it has an example connecting GPIO2 -> PS_SRST_B
 * on the Zynq-7000. (but the zedboard uses SMT1)
 */
static void pulse_gpio(struct ftdi_context *ftdi, int adelay)
{
    int delay;
#define GPIO_DONE            0x10
#define GPIO_01              0x01
#define SET_LSB_DIRECTION(A) SET_BITS_LOW, 0xe0, (0xea | (A))

    ENTER_TMS_STATE('I');
    switch (adelay) {
    case 1250:  delay = CLOCK_FREQUENCY/800; break;
    case 12500: delay = CLOCK_FREQUENCY/80; break;
    default:
           printf("pulse_gpio: unsupported time delay %d\n", adelay);
           exit(-1);
    }
    write_item(DITEM(SET_LSB_DIRECTION(GPIO_DONE | GPIO_01),
                     SET_LSB_DIRECTION(GPIO_DONE)));
    while(delay > 65536) {
        write_item(DITEM(CLK_BYTES, INT16(65536 - 1)));
        delay -= 65536;
    }
    write_item(DITEM(CLK_BYTES, INT16(delay-1)));
    flush_write(ftdi, DITEM(SET_LSB_DIRECTION(GPIO_DONE | GPIO_01),
                     SET_LSB_DIRECTION(GPIO_01)));
}
static void set_clock_divisor(struct ftdi_context *ftdi)
{
    flush_write(ftdi, DITEM(TCK_DIVISOR, INT16(30000000/CLOCK_FREQUENCY - 1)));
}

static char current_state, *lasttail;
static int match_state(char req)
{
    return req == 'X' || !current_state || current_state == req
       || (current_state == 'S' && req == 'D')
       || (current_state == 'D' && req == 'S');
}
void write_tms_transition(char *tail)
{
     char *p = tail+2;
     uint8_t temp[] = {TMSW, 0, 0};
     int len = 0;

     if (!match_state(tail[0])) {
         printf("%s: TMS Error: current %c target %s last %s\n", __FUNCTION__, current_state, tail, lasttail);
     }
     lasttail = tail;
     current_state = tail[1];
     while (*p) {
         len++;
         temp[2] = (temp[2] >> 1) | ((*p++ << 7) & 0x80);
     }
     temp[1] = len-1;
     temp[2] >>= 8 - len;
     write_data(temp, sizeof(temp));
}
void ENTER_TMS_STATE(char required)
{
    char temp = current_state == 'D' ? 'S' : current_state;
    static char *tail[] = {"PS10", /* Pause-DR -> Shift-DR */
        "EI10",  /* Exit1/Exit2 -> Update -> Idle */"RI0", /* Reset -> Idle */
        "SI110", /* Shift-DR -> Update-DR -> Idle */"SP10",/* Shift-IR -> Pause-IR */
        "SE1",   /* Shift-IR -> Exit1-IR */ "SU11", /* Shift-DR -> Update-DR */
        "UD100", /* Update -> Shift-DR */   "ID100",/* Idle -> Shift-DR */
        "RD0100",/* Reset -> Shift-DR */    "IR111",/* Idle -> Reset */
        "PR11111",/* Pause -> Reset */      "IS1100", NULL};/* Idle -> Shift-IR */
    char **p = tail; 
    while(*p) {
        if (temp == (*p)[0] && required == (*p)[1])
            write_tms_transition(*p);
        p++;
    }
    if (!match_state(required))
        printf("[%s:%d] %c should be %c\n", __FUNCTION__, __LINE__, current_state, required);
}
void tmsw_delay(struct ftdi_context *ftdi, int delay_time, int extra)
{
#define SEND_IDLE(A) write_item(DITEM(TMSW, (A), 0))
    int i;
    ENTER_TMS_STATE('I');
    if (extra)
        SEND_IDLE(0);
    for (i = 0; i < delay_time; i++)
        SEND_IDLE(6);
    if (extra)
        SEND_IDLE(extra);
}
static void marker_for_reset(struct ftdi_context *ftdi, int stay_reset)
{
    ENTER_TMS_STATE('R');
    flush_write(ftdi, DITEM(TMSW, stay_reset, 0x7f));
}
static void reset_mark_clock(struct ftdi_context *ftdi, int clock)
{
DPRINT("[%s:%d]\n", __FUNCTION__, __LINE__);
    marker_for_reset(ftdi, 0);
    if (clock)
        set_clock_divisor(ftdi);
    write_tms_transition("RR1");
    flush_write(ftdi, NULL);
}
void write_bit(int read, int bits, int data, char target_state)
{
    int extrabit = 0;
    ENTER_TMS_STATE('S');
    if (bits >= 0) {
        if (bits)
            write_item(DITEM(DATAWBIT | read, bits-1, M(data)));
        extrabit = (data << (7 - bits)) & 0x80;
    }
    if (target_state) {
        uint8_t *cptr = buffer_current_ptr();
        ENTER_TMS_STATE(target_state);
        cptr[0] |= read; // this is a TMS instruction to shift state
        cptr[2] |= extrabit; // insert 1 bit of data here
    }
}

static uint64_t read_data_int(uint8_t *bufp)
{
    uint64_t ret = 0;
    uint8_t *backp = bufp + last_read_data_length;
    while (backp > bufp)
        ret = (ret << 8) | bitswap[*--backp];  //each byte is bitswapped
    return ret;
}

void write_bytes(struct ftdi_context *ftdi, uint8_t read,
    char target_state, uint8_t *ptrin, int size, int max_frame_size, int opttail, int swapbits, int exchar)
{
    ENTER_TMS_STATE('S');
    while (size > 0) {
        int i, rlen = size;
        if (rlen > max_frame_size)
            rlen = max_frame_size;
        int tlen = rlen;
        if (rlen < max_frame_size && opttail > 0)
            tlen--;                   // last byte is actually loaded with DATAWBIT command
        write_item(DITEM(DATAW(read, tlen)));
        uint8_t *cptr = buffer_current_ptr();
        write_data(ptrin, tlen);
        if (swapbits)
            for (i = 0; i < tlen; i++)
                cptr[i] = bitswap[cptr[i]];
        ptrin += tlen;
        if (rlen < max_frame_size) {
            if (opttail > 0) {
                exchar = *ptrin++;
                if (swapbits)
                    exchar = bitswap[exchar];
                opttail = -7;
            }
            write_fill(ftdi, 0, target_state == 'E' && dcount == 2 && trailing_count == 0, 0);
            write_bit(read, -opttail, exchar, target_state);
        }
        size -= max_frame_size;
        if (size > 0)
            flush_write(ftdi, NULL);
    }
}
void idle_to_shift_dr(int idindex)
{
    ENTER_TMS_STATE('D');
    write_bit(0, idindex, 0xff, 0);
}
static uint8_t *write_pattern(struct ftdi_context *ftdi, int idindex, uint8_t *req, int target_state)
{
    idle_to_shift_dr(idindex);
    write_bytes(ftdi, DREAD, target_state, req+1, req[0], SEND_SINGLE_FRAME, 1, 0, 1);
    return read_data(ftdi);
}

static void write_int32(struct ftdi_context *ftdi, uint8_t *data, int size)
{
    while (size) {
        write_bytes(ftdi, 0, 0, data, sizeof(uint32_t), SEND_SINGLE_FRAME, 0, 0, 1);
        data += sizeof(uint32_t);
        size -= sizeof(uint32_t);
    }
}

/*
 * Read/validate IDCODE from device to be programmed
 */
#define REPEAT5(A) INT32(A), INT32(A), INT32(A), INT32(A), INT32(A)
#define REPEAT10(A) REPEAT5(A), REPEAT5(A)

#define IDCODE_PPAT INT32(0xff), REPEAT10(0xff), REPEAT5(0xff)
#define IDCODE_VPAT INT32(0xffffffff), REPEAT10(0xffffffff), REPEAT10(0xffffffff), \
            REPEAT10(0xffffffff), INT32(0xffffffff)

static uint8_t idcode_ppattern[] = DITEM(IDCODE_PPAT);
static uint8_t idcode_presult[] = DITEM(IDCODE_PPAT); // filled in with idcode
static uint8_t idcode_vpattern[] = DITEM(IDCODE_VPAT);
static uint8_t idcode_vresult[] = DITEM(IDCODE_VPAT); // filled in with idcode
void read_idcode(struct ftdi_context *ftdi, int prereset)
{
    int i, offset = 0;

    if (prereset)
        write_tms_transition("RR1");
    marker_for_reset(ftdi, 4);
    uint8_t *rdata = write_pattern(ftdi, 0, idcode_ppattern, 'I');
    if (first_time_idcode_read) {    // only setup idcode patterns on first call!
        first_time_idcode_read = 0;
        memcpy(&idcode_presult[1], idcode_ppattern+1, idcode_ppattern[0]);
        memcpy(&idcode_vresult[1], idcode_vpattern+1, idcode_vpattern[0]);
        while (memcmp(idcode_presult+1, rdata, idcode_presult[0]) && offset < sizeof(uint32_t) * (IDCODE_ARRAY_SIZE-1)) {
            memcpy(&idcode_array[idcode_count++], rdata+offset, sizeof(uint32_t));
            memcpy(idcode_presult+offset+1, rdata+offset, sizeof(uint32_t));   // copy 2nd idcode
            memcpy(idcode_vresult+offset+1, rdata+offset, sizeof(uint32_t));   // copy 2nd idcode
            offset += sizeof(uint32_t);
        }
    }
    if (memcmp(idcode_presult+1, rdata, idcode_presult[0])) {
        memdump(idcode_presult+1, idcode_presult[0], "READ_IDCODE: EXPECT");
        memdump(rdata, idcode_presult[0], "READ_IDCODE: ACTUAL");
    }
    for (i = 0; i < idcode_count; i++) {
        if (idcode_array[i] == CORTEX_IDCODE) {
            found_cortex = i;
            idcode_len[i] = CORTEX_IR_LENGTH;
        }
        else {
            idcode_array[i] &= 0x0fffffff;  /* Xilinx 7 Series: 4 MSB are 'version': UG470, Fig 5-8 */
            idcode_len[i] = XILINX_IR_LENGTH;
        }
    }
}

static void init_device(struct ftdi_context *ftdi, int extra)
{
    write_item(DITEM(LOOPBACK_END, DIS_DIV_5));
    set_clock_divisor(ftdi);
    write_item(DITEM(SET_BITS_LOW, 0xe8, 0xeb, SET_BITS_HIGH, 0x20, 0x30));
    if (extra)
        write_item(DITEM(SET_BITS_HIGH, 0x30, 0x00, SET_BITS_HIGH, 0x00, 0x00));
    write_tms_transition("XR11111");       /*** Force TAP controller to Reset state ***/
}
static struct ftdi_context *get_deviceid(int device_index)
{
    struct ftdi_context *ftdi = init_ftdi(device_index);
    /*
     * Set JTAG clock speed and GPIO pins for our i/f
     */
    idcode_count = 0;
    if (ftdi) {
        init_device(ftdi, uinfo[device_index].bcdDevice == 0x700); /* not a zedboard */
        first_time_idcode_read = 1;
        ENTER_TMS_STATE('R');
        read_idcode(ftdi, 1);
    }
    return ftdi;
}

static void write_fill(struct ftdi_context *ftdi, int read, int width, int tail)
{
    static uint8_t ones[] = {0xff, 0xff, 0xff, 0xff};
    if (width > 7) {
        int bytes = width/8;
        write_bytes(ftdi, read, 0, ones, bytes, SEND_SINGLE_FRAME, 0, 0, 1);
        width -= 8 * bytes;
    }
    write_bit(read, width, 0xff, tail);
}
/*
 * Functions for setting Instruction Register(IR)
 */
static int befbits, afterbits;
void write_irreg(struct ftdi_context *ftdi, int read, int command, int idindex, char tail)
{
    int i;
    befbits = 0;
    afterbits = 0;
    for (i = 0; i < idcode_count; i++) {
        if (i > idindex)
            afterbits += idcode_len[i];
        else if (i < idindex)
            befbits += idcode_len[i];
    }
if(tracep)
printf("[%s:%d] read %d command %x idindex %d bef %d aft %d\n", __FUNCTION__, __LINE__, read, command, idindex, befbits, afterbits);
    ENTER_TMS_STATE('I');
    ENTER_TMS_STATE('S');
    write_fill(ftdi, 0, befbits, 0);
    int trim = (read && idindex != -1);
    if (afterbits && !trim) {
        if (idindex != -1)
            write_bit(0, idcode_len[idindex], command, 0);
        write_fill(ftdi, read, afterbits - 1, tail);
    }
    else
        write_bit(read, idcode_len[idindex] - 1, command, tail);
}
static int write_cirreg(struct ftdi_context *ftdi, int read, int command)
{
    int ret = 0, target_state = (not_last_id && read ? 'P' : 'E');
    write_irreg(ftdi, read, command, jtag_index, target_state);
    if (read) {
        ret = read_data_int(read_data(ftdi));
        if (target_state == 'P')
            write_fill(ftdi, 0, afterbits - 1, 'E');
    }
    ENTER_TMS_STATE('I');
    return ret;
}
int write_cbypass(struct ftdi_context *ftdi, int read, int idindex)
{
    int ret = 0;
    write_irreg(ftdi, read, IRREG_BYPASS_EXTEND, idindex, 'I');
    if (read)
        ret = read_data_int(read_data(ftdi));
    ENTER_TMS_STATE('I');
    return ret;
}
void write_dirreg(struct ftdi_context *ftdi, int command, int idindex)
{
    write_irreg(ftdi, 0, EXTEND_EXTRA | command, idindex, 'I');
    idle_to_shift_dr(0);
    write_bit(0, idindex, 0, 0);
}
void write_creg(struct ftdi_context *ftdi, int regname)
{
    write_irreg(ftdi, 0, regname, found_cortex, 'U');
}

static void send_data_file(struct ftdi_context *ftdi, int read, int extra_shift,
    uint8_t *pdata, int psize, uint8_t *pre, uint8_t *post, int opttail, int swapbits)
{
    static uint8_t zerodata[7];
    flush_write(ftdi, NULL);
    write_cirreg(ftdi, read, IRREG_CFG_IN);
    idle_to_shift_dr(jtag_index);
    if (pre)
        write_int32(ftdi, pre+1, pre[0]);
    int tremain = trailing_count;
    while (idcode_count > 1) {
        int temp = (dcount == 2 && !tremain) ? -6 : -7;
        if (tremain == 1)
           temp = -(8 - jtag_index);
        write_bytes(ftdi, 0, 0, zerodata, 7, SEND_SINGLE_FRAME, temp, 0, 0);
        if (tremain-- <= 1)
            break;
    }
    if (post)
        write_int32(ftdi, post+1, post[0]);
    int limit_len = MAX_SINGLE_USB_DATA - buffer_current_size();
    while(psize) {
        int size = FILE_READSIZE, final = (psize <= FILE_READSIZE);
        if (final)
            size = psize;
        write_bytes(ftdi, 0, (final && !extra_shift) ? 'E' : 'P', pdata,
            size, limit_len, !final || opttail, swapbits, 1);
        flush_write(ftdi, NULL);
        limit_len = MAX_SINGLE_USB_DATA;
        psize -= size;
        pdata += size;
    };
    if (extra_shift)
        write_fill(ftdi, 0, 0, 'E');
    ENTER_TMS_STATE('I');
    flush_write(ftdi, NULL);
}

static void write_above2(int read, int idindex)
{
    write_bit(read, (!idindex && idcode_count > 1) * (idcode_count - 2), 0, 'I');
}
uint32_t fetch_result(struct ftdi_context *ftdi, int idindex, int command, int resp_len, int fd)
{
    int j, readitem = ((idindex == idcode_count - 1) && (idindex || !not_last_id)) * DREAD;
    uint32_t ret = 0;

    if (idindex >= 0 && resp_len) {
        write_dirreg(ftdi, command, idindex);
DPRINT("[%s:%d] idindex %d readitem %x\n", __FUNCTION__, __LINE__, idindex, readitem);
        write_bit(0, (dcount - 2) * (idindex != 0 && idindex != idcode_count - 1), 0, 0);
    }
DPRINT("[%s:%d]\n", __FUNCTION__, __LINE__);
    while (resp_len > 0) {
        int size = resp_len;
        if (size > SEGMENT_LENGTH)
            size = SEGMENT_LENGTH;
        resp_len -= size;
        if (readitem)
            write_item(DITEM(DATAR(size - 1), DATARBIT, 0x06));
        else
            write_item(DITEM(DATAR(size)));
        if (resp_len <= 0)
            write_above2(readitem, idindex);
        uint8_t *rdata = read_data(ftdi);
        uint8_t sdata[] = {SINT32(*(uint32_t *)rdata)};
        ret = *(uint32_t *)sdata;
        for (j = 0; j < size; j++)
            rdata[j] = bitswap[rdata[j]];
        if (fd != -1) {
            static int skipsize = BITFILE_ITEMSIZE; /* 1 framebuffer of delay until data is output */
            if (skipsize) {
                int skip = skipsize;
                if (skip > size)
                    skip = size;
                skipsize -= skip;
                size -= skip;
                rdata += skip;
                resp_len += skip;
            }
            if (size)
                write(fd, rdata, size);
        }
    }
    return ret;
}

/*
 * Read Xilinx configuration status register
 * In ug470_7Series_Config.pdf, see "Accessing Configuration Registers
 * through the JTAG Interface" and Table 6-3.
 */
static uint32_t readout_seq(struct ftdi_context *ftdi, int idindex, uint8_t *req, int resp_len, int fd)
{
    write_dirreg(ftdi, IRREG_CFG_IN, idindex);
    write_bytes(ftdi, 0, 0, req+1, req[0], SEND_SINGLE_FRAME,
        (idcode_count == 1) || (idcode_count - 1 == idindex && idindex), 0, 0/*weird!*/);
DPRINT("[%s:%d] idindex %d\n", __FUNCTION__, __LINE__, idindex);
    write_above2(0, idindex);
DPRINT("[%s:%d]\n", __FUNCTION__, __LINE__);
    return fetch_result(ftdi, idindex, IRREG_CFG_OUT, resp_len, fd);
}

static void readout_status0(struct ftdi_context *ftdi)
{
    int ret, idindex;

    for (idindex = idcode_count - 1; idindex >= 0; idindex--) {
DPRINT("[%s:%d] idindex %d/%d dcount %d trailing %d\n", __FUNCTION__, __LINE__, idindex, idcode_count, dcount, trailing_count);
        if (found_cortex && idindex == found_cortex) {
            write_cbypass(ftdi, DREAD, -1);
            continue; // skip Cortex element
        }
        if ((ret = fetch_result(ftdi, idindex, IRREG_USERCODE, sizeof(uint32_t), -1)) != 0xffffffff)
            printf("fpgajtag: USERCODE value %x\n", ret);
        write_cbypass(ftdi, DREAD, -1);
        write_cbypass(ftdi, DREAD, -1);
        write_cbypass(ftdi, DREAD, -1);
        ENTER_TMS_STATE('R');
DPRINT("[%s:%d] idindex %d/%d dcount %d trailing %d\n", __FUNCTION__, __LINE__, idindex, idcode_count, dcount, trailing_count);
        ret = readout_seq(ftdi, idindex, rstatus, sizeof(uint32_t), -1);
        uint32_t status = ret >> 8;
        if (verbose && (bitswap[M(ret)] != 2 || status != 0x301900))
            printf("[%s:%d] expect %x mismatch %x\n", __FUNCTION__, __LINE__, 0x301900, ret);
        printf("STATUS %08x done %x release_done %x eos %x startup_state %x\n", status,
            status & 0x4000, status & 0x2000, status & 0x10, (status >> 18) & 7);
        ENTER_TMS_STATE('R');
    }
}

/*
 * Configuration Register Read Procedure (JTAG), ug470_7Series_Config.pdf,
 * Table 6-4.
 */
static uint32_t read_config_reg(struct ftdi_context *ftdi, uint32_t data)
{
    uint8_t *req = DITEM(CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0),
        CONFIG_TYPE1(CONFIG_OP_READ, data, 1),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0),
        CONFIG_TYPE1(CONFIG_OP_WRITE, CONFIG_REG_CMD, CONFIG_CMD_WCFG),
        CONFIG_CMD_DESYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0));
    uint8_t constant4[] = {INT32(4)};

    send_data_file(ftdi, 0, 0, constant4, sizeof(constant4), DITEM(CONFIG_DUMMY), req, !not_last_id, 0);
    write_cirreg(ftdi, 0, IRREG_CFG_OUT);
    uint64_t ret = read_data_int(
        write_pattern(ftdi, jtag_index, DITEM(INT32(0)), not_last_id ? 'P' : 'E'));
    if (not_last_id)
        write_fill(ftdi, 0, dcount == 2 && trailing_count == 0, 'E');
    write_cirreg(ftdi, 0, IRREG_BYPASS);
    return ret;
}

static void read_config_memory(struct ftdi_context *ftdi, int fd, uint32_t size)
{
#if 0
    readout_seq(ftdi, 0, DITEM(CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_READ,CONFIG_REG_STAT,1),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)), sizeof(uint32_t), -1);
    readout_seq(ftdi, 0, DITEM(CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_CMD,1), CONFIG_CMD_RCRC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)), 0, -1);
#endif
    write_cirreg(ftdi, 0, IRREG_JSHUTDOWN);
    tmsw_delay(ftdi, 6, 0);
    readout_seq(ftdi, 0, DITEM( CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_CMD,1), CONFIG_CMD_RCFG,
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_FAR,1), 0,
        CONFIG_TYPE1(CONFIG_OP_READ,CONFIG_REG_FDRO,0),
        CONFIG_TYPE2(size),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)), size, fd);
}

struct ftdi_context *init_fpgajtag(const char *serialno, const char *filename)
{
    struct ftdi_context *ftdi;
    int i, j;

    /*
     * Initialize USB, FTDI
     */
    for (i = 0; i < sizeof(bitswap); i++)
        bitswap[i] = BSWAP(i);
    uinfo = fpgausb_init();   /*** Initialize USB interface ***/
    int usb_index = 0;
    for (i = 0; uinfo[i].dev; i++) {
        fprintf(stderr, "fpgajtag: %s:%s:%s; bcd:%x", uinfo[i].iManufacturer,
            uinfo[i].iProduct, uinfo[i].iSerialNumber, uinfo[i].bcdDevice);
        if (!filename) {
            idcode_count = 0;
            ftdi = get_deviceid(i);  /*** Generic initialization of FTDI chip ***/
            fpgausb_close(ftdi);
            if (idcode_count)
                fprintf(stderr, "; IDCODE:");
            for (j = 0; j < idcode_count; j++)
                fprintf(stderr, "  %x", idcode_array[j]);
        }
        fprintf(stderr, "\n");
    }
    if (!filename)
        exit(1);
    while (1) {
        if (!uinfo[usb_index].dev) {
            fprintf(stderr, "fpgajtag: Can't find usable usb interface\n");
            exit(-1);
        }
        if (!serialno || !strcmp(serialno, (char *)uinfo[usb_index].iSerialNumber))
            break;
        usb_index++;
    }

    /*
     * Read input file
     */
    uint32_t file_idcode = read_inputfile(filename);

    /*
     * Set JTAG clock speed and GPIO pins for our i/f
     */
    ftdi = get_deviceid(usb_index);          /*** Generic initialization of FTDI chip ***/
    for (i = 0; i < idcode_count; i++)       /*** look for device matching file idcode ***/
        if (idcode_array[i] == file_idcode) {
            jtag_index = i;
            if (skip_idcode-- <= 0)
                break;
        }
    if (jtag_index == -1) {
        printf("[%s] id %x from file does not match actual id %x\n", __FUNCTION__, file_idcode, idcode_array[0]);
        exit(-1);
    }
    uint32_t thisid = idcode_array[jtag_index];
    device_type = DEVICE_OTHER;
    if (thisid == DEVICE_AC701 || thisid == DEVICE_ZC706
     || thisid == DEVICE_VC707 || thisid == DEVICE_KC705)
        device_type = thisid;
    else if (thisid == DEVICE_ZC702) {       // zc702 and zedboard
        if (uinfo[usb_index].bcdDevice == 0x700)
            device_type = DEVICE_ZC702;
        else
            device_type = DEVICE_ZEDBOARD;
    }
    else {
        printf("[%s:%d]unknown device %x\n", __FUNCTION__, __LINE__, thisid);
        //exit(1);
    }
    return ftdi;
}

int main(int argc, char **argv)
{
    uint32_t ret;
    int i, rflag = 0, lflag = 0, cflag = 0, rescan = 0;
    const char *serialno = NULL;
    logfile = stdout;
    opterr = 0;
    while ((i = getopt (argc, argv, "trls:ci:")) != -1)
        switch (i) {
        case 't':
            trace = 1;
            break;
        case 'r':
            rflag = 1;
            break;
        case 'l':
            lflag = 1;
            break;
        case 'c':
            cflag = 1;
            break;
        case 's':
            serialno = optarg;
            break;
        case 'i':
            skip_idcode = atoi(optarg);
            break;
        default:
            goto usage;
        }

    if (optind != argc - 1 && !cflag && !lflag) {
usage:
        fprintf(stderr, "%s: [ -l ] [ -t ] [ -s <serialno> ] [ -r ] <filename>\n", argv[0]);
        exit(1);
    }

    struct ftdi_context *ftdi = init_fpgajtag(serialno, lflag ? NULL : argv[argc - 1]);

    dcount = idcode_count - (found_cortex != 0) - 1;
    not_last_id = jtag_index != idcode_count - 1;
    trailing_count = (jtag_index != 0) * (idcode_count - jtag_index);
printf("[%s:%d] count %d cortex %d jtag %d trailing_count %d\n", __FUNCTION__, __LINE__, idcode_count, found_cortex, jtag_index, trailing_count);
printf("[%s:%d] bef user2 jtagindex %d not_last_id %d dcount %d not_last_id %d\n", __FUNCTION__, __LINE__, jtag_index, not_last_id, dcount, not_last_id);

    /*
     * See if we are reading out data
     */
    if (rflag) {
        fprintf(stderr, "fpgajtag: readout fpga config into xx.bozo\n");
        /* this size was taken from the TYPE2 record in the original bin file
         * (and must be converted to bits)
         */
        int fd = creat("xx.bozo", 0666);
        uint32_t header = {CONFIG_TYPE2_RAW(0x000f6c78)};
        header = htonl(header);
        write(fd, &header, sizeof(header));
        read_config_memory(ftdi, fd, 0x000f6c78 * sizeof(uint32_t));
        close(fd);
        return 0;
    }
    /*
     * See if we are in 'command' mode with IR/DR info on command line
     */
    if (cflag) {
        process_command_list(ftdi);
        goto exit_label;
    }

    access_mdm(ftdi, 2, 0, 1);
    reset_mark_clock(ftdi, 1);
    marker_for_reset(ftdi, 0);
    write_tms_transition("RR1");
    marker_for_reset(ftdi, 0);
    ENTER_TMS_STATE('I');

    /*
     * Use a pattern of 0xffffffff to validate that we actually understand all the
     * devices in the JTAG chain.  (this list was set up in read_idcode()
     * on the first call
     */
    uint8_t *rdata = write_pattern(ftdi, 0, idcode_vpattern, 'P');
    if (last_read_data_length != idcode_vresult[0]
     || memcmp(idcode_vresult+1, rdata, idcode_vresult[0])) {
        memdump(idcode_vresult+1, idcode_vresult[0], "IDCODE_VALIDATE: EXPECT");
        memdump(rdata, last_read_data_length, "IDCODE_VALIDATE: ACTUAL");
    }

    marker_for_reset(ftdi, 0);
    readout_status0(ftdi);
    access_mdm(ftdi, 1, 0, 99999);
    marker_for_reset(ftdi, 0);

    /*
     * Step 2: Initialization
     */
    write_cirreg(ftdi, 0, IRREG_JPROGRAM);
    write_cirreg(ftdi, 0, IRREG_ISC_NOOP);
    pulse_gpio(ftdi, 12500 /*msec*/);
    if ((ret = write_cirreg(ftdi, DREAD, IRREG_ISC_NOOP)) != INPROGRAMMING)
        printf("[%s:%d] NOOP/INPROGRAMMING mismatch %x\n", __FUNCTION__, __LINE__, ret);

    /*
     * Step 6: Load Configuration Data Frames
     */
    printf("fpgajtag: Starting to send file\n");
    send_data_file(ftdi, DREAD, !dcount && not_last_id, input_fileptr, input_filesize,
        NULL, DITEM(INT32(0)), (trailing_count == 1) || dcount == 0, 1);
    printf("fpgajtag: Done sending file\n");

    /*
     * Step 8: Startup
     */
    pulse_gpio(ftdi, 1250 /*msec*/);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_BOOTSTS)) != (not_last_id ? 0x03000000 : 0x01000000))
        printf("[%s:%d] CONFIG_REG_BOOTSTS mismatch %x\n", __FUNCTION__, __LINE__, ret);
    write_cirreg(ftdi, 0, IRREG_JSTART);
    tmsw_delay(ftdi, 14, 1);
    flush_write(ftdi, NULL);
    if ((ret = write_cirreg(ftdi, DREAD, IRREG_BYPASS)) != FINISHED)
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_STAT)) !=
            (found_cortex ? 0xf87f1046 : 0xfc791040))
        if (verbose)
            printf("[%s:%d] CONFIG_REG_STAT mismatch %x\n", __FUNCTION__, __LINE__, ret);
    marker_for_reset(ftdi, 0);
    ret = write_cbypass(ftdi, DREAD, -1) & 0xff;
    if (ret == FIRST_TIME)
        printf("fpgajtag: bypass first time %x\n", ret);
    else if (ret == PROGRAMMED)
        printf("fpgajtag: bypass already programmed %x\n", ret);
    else
        printf("fpgajtag: bypass unknown %x\n", ret);
    access_mdm(ftdi, 0, 1, 0);

    reset_mark_clock(ftdi, 0);
    ret = readout_seq(ftdi, jtag_index, rstatus, sizeof(uint32_t), -1);
    int status = ret >> 8;
    if (verbose && (bitswap[M(ret)] != 2 || status != 0xf07910))
        printf("[%s:%d] expect %x mismatch %x\n", __FUNCTION__, __LINE__, 0xf07910, ret);
    printf("STATUS %08x done %x release_done %x eos %x startup_state %x\n", status,
        status & 0x4000, status & 0x2000, status & 0x10, (status >> 18) & 7);
    access_mdm(ftdi, 0, 0, 1);
    rescan = 1;

    /*
     * Cleanup and free USB device
     */
exit_label:
    fpgausb_close(ftdi);
    fpgausb_release();
    if (rescan)
        execlp("/usr/local/bin/pciescanportal", "arg", (char *)NULL); /* rescan pci bus to discover device */
    return 0;
}
