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
int input_filesize;
int found_cortex;

static int verbose;
static int jtag_index = -1, device_type, multiple_fpga;
static uint8_t zerodata[8];
static USB_INFO *uinfo;

static int first_time_idcode_read = 1;
static uint32_t idcode_array[IDCODE_ARRAY_SIZE];
static uint32_t idcode_len[IDCODE_ARRAY_SIZE];
static int idcode_count;

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

static char current_state;
static char *lasttail;
void write_tail(char *tail)
{
     char *p = tail+2;
     uint8_t *temp = DITEM(TMSW, 0, 0);
     int len = 0;

     if (current_state != tail[0] && tail[0] != 'X'
          && !(current_state == 'U' && tail[0] == 'I')
          && !(current_state == 'I' && tail[0] == 'U')) {
         printf("%s: current %c target %s last %s\n", __FUNCTION__, current_state, tail, lasttail);
     }
     lasttail = tail;
     current_state = tail[1];
     if (current_state == 'D')
         current_state = 'S';
     while (*p) {
         len++;
         temp[1+2] = (temp[1+2] >> 1) | ((*p++ << 7) & 0x80);
     }
     temp[1+1] = len-1;
     temp[1+2] >>= 8 - len;
     write_item(temp);
}
void reset_state(struct ftdi_context *ftdi, int goto_reset, int input_shift)
{
    char *prefix = IDLE_TO_RESET;
    if (input_shift == 1)
        prefix = "XR1";
    else if (input_shift == 2)
        prefix = FORCE_RETURN_TO_RESET;
    write_tail(prefix);
    uint8_t *temp = DITEM(TMSW, 0x00, 0x7f);
    current_state = 'I';
    if (goto_reset) {
        temp[1+1] = 4; // goto RESET instead of IDLE
        current_state = 'R';
    }
    flush_write(ftdi, temp);
}
void tmsw_delay(int delay_time)
{
    int i;
    for (i = 0; i < delay_time; i++)
        write_tail("II0000000");
}

void check_state(char required)
{
    if (current_state == 'P' && required == 'S')
        write_tail(PAUSE_TO_SHIFT);
    else if (current_state == 'E' && required == 'I')
        write_tail(EXIT1_TO_IDLE);
    else if (current_state == 'R' && required == 'I')
        write_tail(RESET_TO_IDLE);
    if (current_state != required) {
            printf("[%s:%d] %c should be %c\n", __FUNCTION__, __LINE__, current_state, required);
    }
}
void write_bit(int read, int bits, int data, char *tail)
{
    check_state('S');
    if (bits)
        write_item(DITEM(DATAWBIT | read, bits-1, M(data)));
    int extrabit = (data << (7 - bits)) & 0x80;
    if (tail) {
        uint8_t *cptr = buffer_current_ptr();
        write_tail(tail);
        cptr[0] |= read; // this is a TMS instruction to shift state
        cptr[2] |= extrabit; // insert 1 bit of data here
    }
}

void write_bytes(struct ftdi_context *ftdi, uint8_t read,
    char *tail, uint8_t *ptrin, int size, int max_frame_size, int opttail, int swapbits, int default_ext)
{
    uint8_t ch = 0;
    check_state('S');
    while (size > 0) {
        int i, rlen = size;
        size -= max_frame_size;
        if (rlen > max_frame_size)
            rlen = max_frame_size;
        int tlen = rlen;
        if (rlen < max_frame_size && opttail)
            tlen--;                   // last byte is actually loaded with DATAWBIT command
        write_item(DITEM(DATAW(read, tlen)));
        uint8_t *cptr = buffer_current_ptr();
        write_data(ptrin, tlen);
        if (swapbits)
            for (i = 0; i < tlen; i++)
                cptr[i] = bitswap[cptr[i]];
        ptrin += tlen;
        if (rlen < max_frame_size) {
            if (opttail) {
                ch = *ptrin++;
                if (swapbits)
                    ch = bitswap[ch];
                write_bit(read, 7, ch, tail);
            }
            else
                write_bit(read, 0, default_ext, tail); /* this is the 'bypass' bit value */
        }
        if (size > 0)
            flush_write(ftdi, NULL);
    }
}

static void write_one_byte(struct ftdi_context *ftdi, int read, uint8_t data)
{
    write_bytes(ftdi, read, NULL, &data, 1, SEND_SINGLE_FRAME, 0, 0, 1);
}

static void write_int32(struct ftdi_context *ftdi, uint8_t *data, int size)
{
    while (size) {
         write_bytes(ftdi, 0, NULL, data, sizeof(uint32_t), SEND_SINGLE_FRAME, 0, 0, 1);
         data += sizeof(uint32_t);
         size -= sizeof(uint32_t);
    }
}

void idle_to_shift_dr(int extra, int val)
{
    write_tail(IDLE_TO_SHIFT_DR);
    if (extra)
        write_bit(0, 1, val, NULL);
}

static void send_data_file(struct ftdi_context *ftdi, int extra_shift)
{
    char *tailp = SHIFT_TO_PAUSE;

    idle_to_shift_dr(jtag_index, 0xff);
    if (idcode_count > 1)
        write_bytes(ftdi, 0, NULL, zerodata, sizeof(zerodata), SEND_SINGLE_FRAME, 1, 0, 1);
    write_int32(ftdi, zerodata, sizeof(uint32_t));
    int limit_len = MAX_SINGLE_USB_DATA - buffer_current_size();
    printf("fpgajtag: Starting to send file\n");
    while(input_filesize) {
        int size = FILE_READSIZE;
        if (input_filesize <= FILE_READSIZE) {
            size = input_filesize;
            if (!extra_shift)
                tailp = SHIFT_TO_EXIT1;
        }
        input_filesize -= size;
        write_bytes(ftdi, 0, tailp, input_fileptr, size, limit_len, input_filesize != 0 || jtag_index || !multiple_fpga, 1, 1);
        flush_write(ftdi, NULL);
        limit_len = MAX_SINGLE_USB_DATA;
        input_fileptr += size;
    };
    if (extra_shift)
        write_bit(0, 0, 0xff, SHIFT_TO_EXIT1);
    check_state('I');
    printf("fpgajtag: Done sending file\n");
}

/*
 * Read/validate IDCODE from device to be programmed
 */
#define REPEAT5(A) INT32(A), INT32(A), INT32(A), INT32(A), INT32(A)
#define REPEAT10(A) REPEAT5(A), REPEAT5(A)

#define IDCODE_PROBE_PATTERN REPEAT10(0xff), REPEAT5(0xff)
#define PATTERN2 REPEAT10(0xffffffff), REPEAT10(0xffffffff), \
            REPEAT10(0xffffffff), INT32(0xffffffff)

static uint8_t idcode_probe_pattern[] =     {INT32(0xff), IDCODE_PROBE_PATTERN};
static uint8_t idcode_probe_result[] = DITEM(INT32(0xff), IDCODE_PROBE_PATTERN); // filled in with idcode
static uint8_t idcode_validate_pattern[] =     {INT32(0xffffffff),  PATTERN2};
static uint8_t idcode_validate_result[] = DITEM(INT32(0xffffffff), PATTERN2); // filled in with idcode
static void read_idcode(struct ftdi_context *ftdi, int input_shift)
{
    int i;
    if (first_time_idcode_read) {    // only setup idcode patterns on first call!
        memcpy(&idcode_probe_result[1], idcode_probe_pattern, sizeof(idcode_probe_pattern));
        memcpy(&idcode_validate_result[1], idcode_validate_pattern, sizeof(idcode_validate_pattern));
    }
    reset_state(ftdi, 1, input_shift); /* goto RESET */
    write_tail(RESET_TO_SHIFT_DR);
    write_bytes(ftdi, DREAD, SHIFT_TO_IDLE, idcode_probe_pattern, sizeof(idcode_probe_pattern), SEND_SINGLE_FRAME, 1, 0, 1);
    uint8_t *rdata = read_data(ftdi);
    int offset = 0;
    if (first_time_idcode_read) {    // only setup idcode patterns on first call!
        first_time_idcode_read = 0;
        while (memcmp(idcode_probe_result+1, rdata, idcode_probe_result[0]) && offset < sizeof(uint32_t) * (IDCODE_ARRAY_SIZE-1)) {
            memcpy(&idcode_array[idcode_count++], rdata+offset, sizeof(uint32_t));
            memcpy(idcode_probe_result+offset+1, rdata+offset, sizeof(uint32_t));   // copy 2nd idcode
            memcpy(idcode_validate_result+offset+1, rdata+offset, sizeof(uint32_t));   // copy 2nd idcode
            offset += sizeof(uint32_t);
        }
    }
    if (memcmp(idcode_probe_result+1, rdata, idcode_probe_result[0])) {
        printf("[%s]\n", __FUNCTION__);
        memdump(idcode_probe_result+1, idcode_probe_result[0], "EXPECT");
        memdump(rdata, idcode_probe_result[0], "ACTUAL");
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

static struct ftdi_context *get_deviceid(int device_index)
{
    struct ftdi_context *ftdi = init_ftdi(device_index);
    /*
     * Set JTAG clock speed and GPIO pins for our i/f
     */
    idcode_count = 0;
    if (ftdi) {
        write_item(DITEM(LOOPBACK_END, DIS_DIV_5));
        set_clock_divisor(ftdi);
        write_item(DITEM(SET_BITS_LOW, 0xe8, 0xeb, SET_BITS_HIGH, 0x20, 0x30));
        if (uinfo[device_index].bcdDevice == 0x700) /* not a zedboard */
            write_item(DITEM(SET_BITS_HIGH, 0x30, 0x00, SET_BITS_HIGH, 0x00, 0x00));
        write_tail(FORCE_RETURN_TO_RESET); /*** Force TAP controller to Reset state ***/
        first_time_idcode_read = 1;
        read_idcode(ftdi, 1);
    }
    return ftdi;
}

/*
 * Functions for setting Instruction Register(IR)
 */
void write_irreg(struct ftdi_context *ftdi, int read, int command, int flip, char *tail)
{
    int extralen = idcode_len[idcode_count - 1];
    check_state('I');
    write_tail(IDLE_TO_SHIFT_IR);
    if (idcode_count > 1 && read && command == IRREG_BYPASS_EXTEND) {
        write_one_byte(ftdi, read, 0xff);
        extralen -= 8 - idcode_len[0]; /* 2 extra bits sent with write byte*/
    }
    else if (idcode_count > 1 && flip)
        write_bit(0, idcode_len[0], 0xff, NULL);
    else if (idcode_count > 1 && !read) {
        write_bit(0, idcode_len[0], command, NULL);
        command = 0xff;
    }
    else
        extralen = idcode_len[0];
    write_bit(read, extralen - 1, command, tail);
}
static int write_cirreg(struct ftdi_context *ftdi, int read, int command)
{
    int ret = 0, extra = jtag_index != idcode_count - 1 && read;
    write_irreg(ftdi, read, command, jtag_index, extra ? SHIFT_TO_PAUSE : SHIFT_TO_EXIT1);
    if (read)
        ret = read_data_int(ftdi);
    if (extra)
        write_bit(0, idcode_len[idcode_count - 1] - 1, 0xff, SHIFT_TO_EXIT1);
    check_state('I');
    return ret;
}
static void write_dirreg(struct ftdi_context *ftdi, int command, int flip)
{
    write_irreg(ftdi, 0, EXTEND_EXTRA | command, flip, SHIFT_TO_IDLE);
    idle_to_shift_dr(flip, 0);
}
void write_creg(struct ftdi_context *ftdi, int regname)
{
    write_irreg(ftdi, 0, regname, 1, SHIFT_TO_UPDATE);
}

static void write_bypass(struct ftdi_context *ftdi, int read)
{
    write_irreg(ftdi, read, IRREG_BYPASS_EXTEND, jtag_index, SHIFT_TO_IDLE);
    if (read) {
        uint32_t ret = read_data_int(ftdi) & 0xfff;
        if (ret == FIRST_TIME)
            printf("fpgajtag: bypass first time %x\n", ret);
        else if (ret == PROGRAMMED)
            printf("fpgajtag: bypass already programmed %x\n", ret);
        else
            printf("fpgajtag: bypass unknown %x\n", ret);
    }
}

static uint32_t fetch_result(struct ftdi_context *ftdi, int resp_len, int fd, int readitem)
{
    int j;
    uint32_t ret = 0;

    while (resp_len > 0) {
        int size = resp_len;
        if (size > SEGMENT_LENGTH)
            size = SEGMENT_LENGTH;
        resp_len -= size;
        if (!readitem)
            write_item(DITEM(DATAR(size)));
        else
            write_item(DITEM(DATAR(size - 1), DATARBIT, 0x06));
        if (resp_len <= 0)
            write_bit(readitem, 0, 0, SHIFT_TO_IDLE);
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

static uint32_t readout_seq(struct ftdi_context *ftdi, uint8_t *req, int resp_len,
     int fd, int oneformat, int flip)
{
    uint32_t ret = 0;

    write_dirreg(ftdi, IRREG_CFG_IN, flip); /* Select CFG_IN so that we can send out our request */
    write_bytes(ftdi, 0, SHIFT_TO_IDLE, req+1, req[0], SEND_SINGLE_FRAME, oneformat, 0, 0/*weird!*/);
    if (resp_len) {
        write_dirreg(ftdi, IRREG_CFG_OUT, flip);
        ret = fetch_result(ftdi, resp_len, fd, (idcode_count == 1 || flip) * DREAD);
    }
    return ret;
}

static void access_user2(struct ftdi_context *ftdi, int argj, int cortex_nowait, int input_shift, int reset, int clock)
{
    int testi, flip;

    read_idcode(ftdi, input_shift);
    for (flip = 0; flip < 1 + multiple_fpga; flip++) {
        int j = argj;
        while (j-- > 0) {
            for (testi = 0; testi < 4; testi++) {
                write_bypass(ftdi, 0);
                write_dirreg(ftdi, IRREG_USER2, flip);
                if (testi) {
                    if (testi > 1) {
                        write_bit(0, idcode_len[0] - (idcode_count == 1) - flip,
                            IRREG_JSTART, SHIFT_TO_IDLE); /* DR data */
                        idle_to_shift_dr(flip, 0);
                    }
                    write_one_byte(ftdi, 0, 0x69);
                    write_bit(0, 2, 0, NULL);
                    if (idcode_count > 1)
                        write_bit(0, 1, 0, NULL);
                }
                uint32_t ret = fetch_result(ftdi, sizeof(uint32_t), -1, (idcode_count == 1 || flip) * DREAD);
                if (ret != 0)
                    printf("[%s:%d] nonzero USER2 %x\n", __FUNCTION__, __LINE__, ret);
            }
        }
    }
    if (found_cortex)
        cortex_bypass(ftdi, cortex_nowait);
    if (reset)
        reset_state(ftdi, 0, 0);
    if (clock)
        set_clock_divisor(ftdi);
    if (trace)
        printf("[%s:%d] end\n", __FUNCTION__, __LINE__);
}


static void readout_status(struct ftdi_context *ftdi, int btype, int upperbound, uint32_t checkval)
{
    int i, j, ret, statparam = found_cortex ? 1 : -(btype && jtag_index == 0);
    for (j = 0; j < upperbound; j++) {
        if (btype) {
            access_user2(ftdi, 3, 1, (j == 1), 0, 0);
            if (j)
                continue;
            reset_state(ftdi, 0, 0);
            write_tail("IR1");
        }
        else {
            write_dirreg(ftdi, IRREG_USERCODE, !j && multiple_fpga);
            ret = fetch_result(ftdi, sizeof(uint32_t), -1,
                  ((!j && multiple_fpga) || idcode_count == 1) * DREAD);
            if (ret != 0xffffffff)
                printf("fpgajtag: USERCODE value %x\n", ret);
            for (i = 0; i < 3; i++)
                write_bypass(ftdi, DREAD);
            write_tail(IDLE_TO_RESET);
        }
        /*
         * Read Xilinx configuration status register
         * In ug470_7Series_Config.pdf, see "Accessing Configuration Registers
         * through the JTAG Interface" and Table 6-3.
         */
        uint32_t ret = readout_seq(ftdi, DITEM(CONFIG_DUMMY,
            CONFIG_SYNC, CONFIG_TYPE2(0),
            CONFIG_TYPE1(CONFIG_OP_READ, CONFIG_REG_STAT, 1), SINT32(0)),
            sizeof(uint32_t), -1,
            !statparam || ((jtag_index || !multiple_fpga) && statparam == -1),
            multiple_fpga * (!statparam));
        write_tail(IDLE_TO_RESET);
        uint32_t status = ret >> 8;
        if (verbose && (bitswap[M(ret)] != 2 || status != checkval))
            printf("[%s:%d] expect %x mismatch %x\n", __FUNCTION__, __LINE__, checkval, ret);
        printf("STATUS %08x done %x release_done %x eos %x startup_state %x\n", status,
            status & 0x4000, status & 0x2000, status & 0x10, (status >> 18) & 7);
        statparam = 1;
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
    uint8_t dummy[] = {CONFIG_DUMMY};
    int extra = jtag_index != idcode_count - 1;

    write_cirreg(ftdi, 0, IRREG_CFG_IN);
    idle_to_shift_dr(jtag_index, 0xff);
    write_int32(ftdi, dummy, sizeof(uint32_t));
    if (idcode_count > 1)
        write_bytes(ftdi, 0, NULL, zerodata, sizeof(zerodata), SEND_SINGLE_FRAME, 1, 0, 1);
    write_int32(ftdi, req+1, req[0]);
    write_bytes(ftdi, 0, SHIFT_TO_EXIT1, constant4, sizeof(constant4), SEND_SINGLE_FRAME, !extra, 0, 1);
    write_cirreg(ftdi, 0, IRREG_CFG_OUT);
    idle_to_shift_dr(jtag_index, 0xff);
    write_bytes(ftdi, DREAD, extra ? SHIFT_TO_PAUSE : SHIFT_TO_EXIT1,
        zerodata, sizeof(uint32_t), SEND_SINGLE_FRAME, 1, 0, 1);
    uint64_t ret = read_data_int(ftdi);
    if (extra)
        write_bit(0, 0, 0xff, SHIFT_TO_EXIT1);
    //check_state('I');
    return ret;
}

static void read_config_memory(struct ftdi_context *ftdi, int fd, uint32_t size)
{
#if 0
    readout_seq(ftdi, DITEM(CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_READ,CONFIG_REG_STAT,1),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)), sizeof(uint32_t), -1, 1, 0);
    readout_seq(ftdi, DITEM(CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_CMD,1), CONFIG_CMD_RCRC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)), 0, -1, 1, 0);
#endif
    write_cirreg(ftdi, 0, IRREG_JSHUTDOWN);
    tmsw_delay(6);
    readout_seq(ftdi, DITEM( CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_CMD,1), CONFIG_CMD_RCFG,
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_FAR,1), 0,
        CONFIG_TYPE1(CONFIG_OP_READ,CONFIG_REG_FDRO,0),
        CONFIG_TYPE2(size),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)), size, fd, 1, 0);
}

int main(int argc, char **argv)
{
    logfile = stdout;
    struct ftdi_context *ftdi;
    uint32_t ret;
    int i, j, rflag = 0, lflag = 0, cflag = 0;
    const char *serialno = NULL;
    int rescan = 0;

    opterr = 0;
    while ((i = getopt (argc, argv, "trls:c")) != -1)
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
        default:
            goto usage;
        }

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
        if (lflag) {
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
    if (lflag)
        goto exit_label;
    while (1) {
        if (!uinfo[usb_index].dev) {
            fprintf(stderr, "fpgajtag: Can't find usable usb interface\n");
            exit(-1);
        }
        if (!serialno || !strcmp(serialno, (char *)uinfo[usb_index].iSerialNumber))
            break;
        usb_index++;
    }
    if (optind != argc - 1 && !cflag) {
usage:
        fprintf(stderr, "%s: [ -l ] [ -t ] [ -s <serialno> ] [ -r ] <filename>\n", argv[0]);
        exit(1);
    }

    /*
     * Read input file
     */
    uint32_t file_idcode = read_inputfile(argv[optind]);

    /*
     * Set JTAG clock speed and GPIO pins for our i/f
     */
    ftdi = get_deviceid(usb_index);          /*** Generic initialization of FTDI chip ***/
    for (i = 0; i < idcode_count; i++)       /*** look for device matching file idcode ***/
        if (idcode_array[i] == file_idcode)
            jtag_index = i;
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

    //(uinfo[device_index].bcdDevice == 0x700) //kc,vc,ac701,zc702  FT2232C
    //if (uinfo[device_index].bcdDevice == 0x900) //zedboard, zc706 FT232H
        //found_232H = 1;
    multiple_fpga = idcode_count > 1 && !found_cortex;
    int bypass_tc = 4;
    if (device_type == DEVICE_AC701)
        bypass_tc = 3;
    if (device_type == DEVICE_VC707 && idcode_count == 1)
        bypass_tc = 6;
    if (device_type == DEVICE_ZEDBOARD)
        bypass_tc = 2;
    if (device_type == DEVICE_ZC702)
        bypass_tc = 1;
    if (multiple_fpga && jtag_index == 0)
        bypass_tc += 8;
    int firstflag = device_type == DEVICE_ZC702 || (multiple_fpga && jtag_index == 0);
    int first_bypass_count = 3 + (device_type == DEVICE_VC707 || device_type == DEVICE_AC701 || idcode_count > 1);
    int extra_bypass_count = 1 + (device_type == DEVICE_VC707 || device_type == DEVICE_AC701 || (jtag_index != idcode_count - 1 && (device_type != DEVICE_ZEDBOARD)));

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

    access_user2(ftdi, first_bypass_count, 0, 0, firstflag, firstflag);
    access_user2(ftdi, 3, 1, firstflag, 1, !firstflag);
    for (i = 0; i < 1 + (firstflag == 0); i++)
        reset_state(ftdi, 0, 1);
    write_tail("II0");

    /*
     * Use a pattern of 0xffffffff to validate that we actually understand all the
     * devices in the JTAG chain.  (this list was set up in read_idcode()
     * on the first call
     */
    idle_to_shift_dr(0, 0);
    write_bytes(ftdi, DREAD, SHIFT_TO_PAUSE, idcode_validate_pattern, sizeof(idcode_validate_pattern), SEND_SINGLE_FRAME, 1, 0, 1);
    uint8_t *rdata = read_data(ftdi);
    if (last_read_data_length != idcode_validate_result[0]
     || memcmp(idcode_validate_result+1, rdata, idcode_validate_result[0])) {
        printf("fpgajtag: mismatch validate data\n");
        memdump(idcode_validate_result+1, idcode_validate_result[0], "EXPECT");
        memdump(rdata, last_read_data_length, "ACTUAL");
    }
    reset_state(ftdi, 0, 2);
    write_tail("II0");
    if (found_cortex)
        write_bypass(ftdi, DREAD);

    readout_status(ftdi, 0, 1 + multiple_fpga, 0x301900);
    for (i = 0; i < bypass_tc; i++)
        access_user2(ftdi, 3, 1, (i == 0), 0, 0);

    /*
     * Step 2: Initialization
     */
    reset_state(ftdi, 0, 0);
    write_tail("II0");
    write_cirreg(ftdi, 0, IRREG_JPROGRAM);
    write_cirreg(ftdi, 0, IRREG_ISC_NOOP);
    pulse_gpio(ftdi, 12500 /*msec*/);
    int rc = write_cirreg(ftdi, DREAD, IRREG_ISC_NOOP);
    if (rc != INPROGRAMMING)
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, rc);

    /*
     * Step 6: Load Configuration Data Frames
     */
    rc = write_cirreg(ftdi, DREAD, IRREG_CFG_IN);
    if (rc != INPROGRAMMING)
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, rc);
    send_data_file(ftdi, found_cortex);

    /*
     * Step 8: Startup
     */
    pulse_gpio(ftdi, 1250 /*msec*/);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_BOOTSTS)) != ((jtag_index != idcode_count - 1) ? 0x03000000 : 0x01000000))
        printf("[%s:%d] CONFIG_REG_BOOTSTS mismatch %x\n", __FUNCTION__, __LINE__, ret);
    write_cirreg(ftdi, 0, IRREG_BYPASS);
    write_cirreg(ftdi, 0, IRREG_JSTART);
    write_tail("II0");
    tmsw_delay(14);
    write_tail("II00");
    flush_write(ftdi, NULL);
    rc = write_cirreg(ftdi, DREAD, IRREG_BYPASS);
    if (rc != FINISHED)
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, rc);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_STAT)) != (found_cortex ? 0xf87f1046 : 0xfc791040))
        if (verbose)
            printf("[%s:%d] CONFIG_REG_STAT mismatch %x\n", __FUNCTION__, __LINE__, ret);
    write_cirreg(ftdi, 0, IRREG_BYPASS);
    reset_state(ftdi, 0, 0);
    write_tail("II0");
    write_bypass(ftdi, DREAD);

    readout_status(ftdi, 1, extra_bypass_count, 0xf07910);
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
