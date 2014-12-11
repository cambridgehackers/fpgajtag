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
#include <zlib.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include "util.h"
#include "fpga.h"

#define BUFFER_MAX_LEN    100000000
#define FILE_READSIZE          6464
#define MAX_SINGLE_USB_DATA    4046
#define IDCODE_ARRAY_SIZE        20
#define IDCODE_MASK      0x0fffffff
#define SEGMENT_LENGTH           64 /* sizes above 256bits seem to get more bytes back in response than were requested */

static int verbose;
static int found_multiple, use_first, use_second, corfirst, use_both, device_type;
int found_cortex;
static int opcode_bits = 4;
uint8_t *input_fileptr;
static int input_filesize;
static uint32_t file_idcode;
static USB_INFO *uinfo;

static int first_time_idcode_read = 1;
static uint32_t idcode_array[IDCODE_ARRAY_SIZE];
static int idcode_array_index;

static uint8_t bitfile_header[] = {
    0, 9, 0xf, 0xf0, 0xf, 0xf0, 0xf, 0xf0, 0xf, 0xf0, 0, 0, 1, 'a'};
static void read_inputfile(char *filename)
{
    static uint8_t filebuf[BUFFER_MAX_LEN];
    static uint8_t uncompressbuf[BUFFER_MAX_LEN];
    static uint8_t gzmagic[] = {0x1f, 0x8b};
    int inputfd = 0;   /* default input for '-' is stdin */

    if (strcmp(filename, "-")) {
        inputfd = open(filename, O_RDONLY);
        if (inputfd == -1) {
            printf("fpgajtag: Unable to open file '%s'\n", filename);
            exit(-1);
        }
    }
    input_filesize = read(inputfd, filebuf, sizeof(filebuf));
    input_fileptr = filebuf;
    close(inputfd);
    if (input_filesize <= 0 || input_filesize >= sizeof(filebuf) - 1)
        goto badlen;
    if (!memcmp(input_fileptr, gzmagic, sizeof(gzmagic))) {
        printf("fpgajtag: unzip input file, len %d\n", input_filesize);
        z_stream strm;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        int ret = inflateInit2(&strm, 16+MAX_WBITS); // inflate gzip'ed file
        if (ret != Z_OK)
            goto badlen;
        strm.next_in = input_fileptr;
        strm.avail_in = input_filesize;
        strm.next_out = uncompressbuf;
        strm.avail_out = sizeof(uncompressbuf);
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        input_filesize = sizeof(uncompressbuf) - strm.avail_out;
        if (ret != Z_STREAM_END)
            goto badlen;
        input_fileptr = uncompressbuf;
    }
    if (!memcmp(bitfile_header, input_fileptr, sizeof(bitfile_header))) {
        uint8_t *inputtemp = input_fileptr;
        input_fileptr += sizeof(bitfile_header) - 1;
        while(*input_fileptr++ < 'e') {
            int len = *input_fileptr++;
            len = (len << 8) | *input_fileptr++;
            input_fileptr += len;
        }
        if (*--input_fileptr == 'e')
            input_fileptr += 1 + sizeof(uint32_t); /* skip over 'e' and length */
        input_filesize -= input_fileptr - inputtemp;
    }

    /*
     * Step 5: Check Device ID
     */
    /*** Read device id from file to be programmed           ***/
    memcpy(&file_idcode, input_fileptr+0x80, sizeof(file_idcode));
    file_idcode = (M(file_idcode) << 24) | (M(file_idcode >> 8) << 16) | (M(file_idcode >> 16) << 8) | M(file_idcode >> 24);
    return;
badlen:
    printf("fpgajtag: Input file length exceeds static buffer size %ld.  You must recompile fpgajtag.\n", sizeof(filebuf));
    exit(-1);
}

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

void tmsw_delay(int delay_time)
{
    int i;
    for (i = 0; i < delay_time; i++)
        write_item(DITEM(TMSW, 0x06, 0x00));
}

static void write_dataw(uint32_t value)
{
    write_item(DITEM(DATAW(0, value)));
}

static uint32_t swap32i(uint32_t value)
{
    int i;
    union {
        uint32_t i;
        uint8_t c[4];
    } temp, tempo;
    temp.i = value;
    for (i = 0; i < 4; i++)
        tempo.c[i] = bitswap[temp.c[3-i]];
    return tempo.i;
}
static void swap32(uint32_t value)
{
    write_item(DITEM(INT32(swap32i(value))));
}
int write_bit(int read, int bits, int data)
{
    write_item(DITEM(DATAWBIT | read, bits-1, M(data)));
    return (data << (7 - bits)) & 0x80;
}

static int write_one_word(int dread, int short_format, uint32_t value)
{
    if (!short_format) {
        write_item(DITEM(M(value), M(value >> 8), M(value >> 16)));
        return write_bit(dread, 7, value >> 24);
    }
    write_item(DITEM(INT32(value)));
    return 0;
}

void send_data_frame(struct ftdi_context *ftdi, uint8_t read,
    uint8_t *tail, uint8_t *ptrin, int size, int max_frame_size, int opttail)
{
    while (size > 0) {
        int rlen = size;
        size -= max_frame_size;
        if (rlen > max_frame_size)
            rlen = max_frame_size;
        int tlen = rlen;
        if (rlen < max_frame_size && opttail)
            tlen--;                   // last byte is actually loaded with DATAWBIT command
        write_item(DITEM(DATAW(read, tlen)));
        write_data(ptrin, rlen);
        ptrin += rlen;
        uint8_t *cptr = buffer_current_ptr();
        if (rlen < max_frame_size) {
            uint8_t ch = cptr[-1];
            if (opttail) {
                cptr[-1] = DATAWBIT | read; /* replace last byte of data with DATAWBIT op */
                write_item(DITEM(6)); // 7 bits of data here
                write_data(&ch, 1);
                cptr += 2;
            }
            else
                ch = 0x80; /* this is the 'bypass' bit value */
            write_item(tail);
            if (tail[1] & MPSSE_WRITE_TMS) {
                cptr[0] |= read; // this is a TMS instruction to shift state
                cptr[2] |= 0x80 & ch; // insert 1 bit of data here
            }
        }
        if (size > 0)
            flush_write(ftdi, NULL);
    }
}

static void write_dswap32(uint32_t value)
{
    write_dataw(4);
    swap32(value);
}

static void write_eight_bytes()
{
    if (found_multiple) {
        write_item(DITEM(DATAW(0, 7), INT32(0)));
        write_one_word(0, 0, 0);
    }
}

void idle_to_shift_dr(int extra, int val)
{
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    if (extra)
        write_bit(0, 1, val);
}

static void send_data_file(struct ftdi_context *ftdi)
{
    uint8_t filebuffer[FILE_READSIZE];
    uint8_t *tailp = DITEM(SHIFT_TO_PAUSE(0,0));

    idle_to_shift_dr(use_second, 0xff);
    write_eight_bytes();
    write_dswap32(0);
    int limit_len = MAX_SINGLE_USB_DATA - buffer_current_size();
    printf("fpgajtag: Starting to send file\n");
    while(1) {
        int i, size = FILE_READSIZE;
        if (input_filesize < FILE_READSIZE) {
            size = input_filesize;
            if (!found_cortex)
                tailp = DITEM(SHIFT_TO_EXIT1(0, 0));
        }
        for (i = 0; i < size; i++)
            filebuffer[i] = bitswap[*input_fileptr++];
        send_data_frame(ftdi, 0, tailp, filebuffer, size, limit_len, size == FILE_READSIZE || !use_first);
        flush_write(ftdi, NULL);
        limit_len = MAX_SINGLE_USB_DATA;
        input_filesize -= size;
        if (size != FILE_READSIZE)
            break;
        write_item(DITEM(PAUSE_TO_SHIFT));
    };
    if (found_cortex)
        write_item(DITEM(PAUSE_TO_SHIFT, SHIFT_TO_EXIT1(0, 0x80)));
    write_item(DITEM(EXIT1_TO_IDLE));
    printf("fpgajtag: Done sending file\n");
}

/*
 * Functions for setting Instruction Register(IR)
 */
void write_irreg(struct ftdi_context *ftdi, int read, int command, int next_state, int flip, int combo, uint32_t expect)
{
    if (flip)
        command = ((command >> 8) & 0xff) | ((command & 0xff) << 8);
    int extrabit;
    write_item(DITEM(IDLE_TO_SHIFT_IR));
    if (combo == 1) {
        if (use_second)
            write_bit(0, opcode_bits+1, 0xff);
        extrabit = write_bit(read, 5, command);
        if (corfirst)
            write_item(DITEM(SHIFT_TO_PAUSE(read, extrabit)));
        else
            write_item(DITEM(SHIFT_TO_EXIT1(read, extrabit)));
        if (read) {
            uint16_t ret = read_data_int(__LINE__, ftdi, 1);
            if (found_cortex) {
                write_item(DITEM(PAUSE_TO_SHIFT));
                write_item(DITEM(SHIFT_TO_EXIT1(0, write_bit(0, 3, 0xff))));
            }
            if (ret != expect)
                printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret);
        }
        if (!use_first || !expect) {
            if (!use_first || expect)
                write_item(DITEM(EXIT1_TO_IDLE));
            return;
        }
        read = 0;
        write_item(DITEM(PAUSE_TO_SHIFT));
        extrabit = write_bit(0, 5, command>>8);
    }
    else if ((combo == 2) || (use_both && read && opcode_bits == 5 && (command & 0xffff) == 0xffff)) {
        write_item(DITEM(DATAW(read, 1), 0xff));
        extrabit = write_bit(read, 3 - combo, command);
    }
    else {
        extrabit = write_bit(read, opcode_bits+1, command);
        if (use_both)
            extrabit = write_bit(read, 5, command>>8);
        if (found_cortex)     /* 3 extra bits of IR are sent here */
            extrabit = write_bit(read, 3, command>>8);
    }
    if (next_state == 2)
        write_item(DITEM(SHIFT_TO_UPDATE(0, extrabit)));
    else if (next_state)
        write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE(read, extrabit)));
    else
        write_item(DITEM(SHIFT_TO_EXIT1(read, extrabit), EXIT1_TO_IDLE));
}

static void write_bypass(struct ftdi_context *ftdi)
{
    write_irreg(ftdi, DREAD, EXTEND_EXTRA | IRREG_BYPASS, 1, 0, found_cortex * 2, 0);
    uint32_t ret = read_data_int(__LINE__, ftdi, 1 + found_multiple) & 0xfff;
    if (ret == FIRST_TIME)
        printf("fpgajtag: bypass first time %x\n", ret);
    else if (ret == PROGRAMMED)
        printf("fpgajtag: bypass already programmed %x\n", ret);
    else
        printf("fpgajtag: bypass unknown %x\n", ret);
}

#define REPEAT5(A) INT32(A), INT32(A), INT32(A), INT32(A), INT32(A)
#define REPEAT10(A) REPEAT5(A), REPEAT5(A)

#define IDCODE_PROBE_PATTERN REPEAT10(0xff), REPEAT5(0xff)
#define PATTERN2 REPEAT10(0xffffffff), REPEAT10(0xffffffff), \
            REPEAT10(0xffffffff), INT32(0xffffffff)

static uint8_t idcode_probe_pattern[] =     {INT32(0xff), IDCODE_PROBE_PATTERN};
static uint8_t idcode_probe_result[] = DITEM(INT32(0xff), IDCODE_PROBE_PATTERN); // filled in with idcode
static uint8_t idcode_validate_pattern[] =     {INT32(0xffffffff),  PATTERN2};
static uint8_t idcode_validate_result[] = DITEM(INT32(0xffffffff), PATTERN2); // filled in with idcode
/*
 * Read/validate IDCODE from device to be programmed
 */
static void read_idcode(struct ftdi_context *ftdi, int input_shift)
{
    if (first_time_idcode_read) {    // only setup idcode patterns on first call!
        memcpy(&idcode_probe_result[1], idcode_probe_pattern, sizeof(idcode_probe_pattern));
        memcpy(&idcode_validate_result[1], idcode_validate_pattern, sizeof(idcode_validate_pattern));
    }
    if (input_shift)
        write_item(DITEM(SHIFT_TO_EXIT1(0, 0)));
    else
        write_item(DITEM(IDLE_TO_RESET));
    write_item(DITEM(TMSW, 4, 0x7f/*Reset?????*/, RESET_TO_SHIFT_DR));
    send_data_frame(ftdi, DREAD, DITEM(SHIFT_TO_UPDATE_TO_IDLE(DREAD, 0)),
        idcode_probe_pattern, sizeof(idcode_probe_pattern), SEND_SINGLE_FRAME, 1);
    uint8_t *rdata = read_data(__LINE__, ftdi, idcode_probe_result[0]);
    if (first_time_idcode_read) {    // only setup idcode patterns on first call!
        first_time_idcode_read = 0;
        memcpy(&idcode_array[idcode_array_index++], rdata, sizeof(idcode_array[0]));
        memcpy(idcode_validate_result+1, rdata, sizeof(idcode_array[1])); // copy returned idcode
        memcpy(idcode_probe_result+1, rdata, sizeof(idcode_array[1]));       // copy returned idcode
        if (memcmp(idcode_probe_result+1, rdata, idcode_probe_result[0])) {
            memcpy(&idcode_array[idcode_array_index++], rdata+4, sizeof(idcode_array[0]));
            memcpy(idcode_probe_result+4+1, rdata+4, sizeof(idcode_array[0]));   // copy 2nd idcode
            memcpy(idcode_validate_result+4+1, rdata+4, sizeof(idcode_array[0]));   // copy 2nd idcode
        }
    }
    if (memcmp(idcode_probe_result+1, rdata, idcode_probe_result[0])) {
        printf("[%s]\n", __FUNCTION__);
        memdump(idcode_probe_result+1, idcode_probe_result[0], "EXPECT");
        memdump(rdata, idcode_probe_result[0], "ACTUAL");
    }
}

static uint32_t fetch_result(struct ftdi_context *ftdi, uint32_t irreg, int variant, int resp_len, int bozostyle, int fd, int second)
{
    int j;
    uint32_t ret = 0, readitem = (second && second != 2 && second != 3) ? DREAD : 0;

    write_irreg(ftdi, 0, irreg, 1, readitem, 0, 0);
    idle_to_shift_dr(readitem, 0);
    if (variant > 1) {
        write_bit(0, opcode_bits + (second == 0), IRREG_JSTART);
        write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE(0, 0)));
        idle_to_shift_dr(second, 0);
    }
    if (variant > 0) {
        write_item(DITEM(DATAW(0, 1), 0x69));
        write_bit(0, 2, 0);
        if (found_multiple)
            write_bit(0, 1, 0);
    }
    while (resp_len > 0) {
        int size = resp_len;
        if (size > SEGMENT_LENGTH)
            size = SEGMENT_LENGTH;
        resp_len -= size;
        if (bozostyle && !readitem)
            write_item(DITEM(DATAR(size * sizeof(uint32_t))));
        else
            write_item(DITEM(DATAR(size * sizeof(uint32_t) - 1), DATARBIT, 0x06));
        if (resp_len <= 0)
            write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE(idcode_array_index == 1 ? DREAD : readitem, 0)));
        uint8_t *rdata = read_data(__LINE__, ftdi, size * sizeof(uint32_t));
        ret = swap32i(*(uint32_t *)rdata);
        for (j = 0; j < size * sizeof(uint32_t); j++)
            rdata[j] = bitswap[rdata[j]];
        if (fd != -1) {
            static int skipsize = BITFILE_ITEMSIZE; /* 1 framebuffer of delay until data is output */
            if (skipsize) {
                int skip = skipsize;
                if (skip > size)
                    skip = size;
                skipsize -= skip;
                size -= skip;
                rdata += skip * sizeof(uint32_t);
                resp_len += skip;
            }
            if (size)
                write(fd, rdata, size * sizeof(uint32_t));
        }
    }
    return ret;
}

static uint32_t readout_seq(struct ftdi_context *ftdi, uint32_t *req, int req_len, int resp_len, int fd, int extend, int oneformat, int fetchformat, int extra)
{
    int i;
    uint32_t ret = 0;

    write_irreg(ftdi, 0, extend | IRREG_CFG_IN, 1,
        use_first ? (extra == 4) : (use_second * (extra != 3)),
        0, 0); /* Select CFG_IN so that we can send out our request */
    idle_to_shift_dr(extra == 1 || extra == 4, 0); /* Shift in actual request into DR for CFG_IN */
    write_dataw(req_len * sizeof(uint32_t) - 1 + oneformat);
    for (i = 0; i < req_len - 1; i++)
        swap32(req[i]);
    write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE(0, write_one_word(0, oneformat, swap32i(req[req_len-1])))));
    if (resp_len)
        ret = fetch_result(ftdi, extend | IRREG_CFG_OUT, 0, resp_len, fetchformat, fd, extra);
    flush_write(ftdi, NULL);
    return ret;
}

/*
 * Configuration Register Read Procedure (JTAG), ug470_7Series_Config.pdf,
 * Table 6-4.
 */
static uint32_t read_config_reg(struct ftdi_context *ftdi, uint32_t data, int combo)
{
    uint32_t req[] = {CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0),
        CONFIG_TYPE1(CONFIG_OP_READ, data, 1),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0),
        CONFIG_TYPE1(CONFIG_OP_WRITE, CONFIG_REG_CMD, CONFIG_CMD_WCFG),
        CONFIG_CMD_DESYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0,0)};
    int i;

    write_irreg(ftdi, 0, IRREG_CFG_IN, 0, use_second, 0, 0);
    idle_to_shift_dr(use_second, 0xff);
    write_dswap32(CONFIG_DUMMY);
    write_eight_bytes();
    for (i = 0; i < sizeof(req)/sizeof(req[0]); i++)
        write_dswap32(req[i]);
    write_item(DITEM(DATAW(0, sizeof(uint32_t) - 1 +corfirst)));
    write_one_word(0, corfirst, 4);
    write_item(DITEM(SHIFT_TO_EXIT1(0, corfirst ? 0x80 : 0), EXIT1_TO_IDLE));
    write_irreg(ftdi, 0, IRREG_CFG_OUT, 0, use_second, 0, 0);
    idle_to_shift_dr(use_second, 0xff);
    write_item(DITEM(DATAW(DREAD, sizeof(uint32_t) - 1 )));
    write_one_word(DREAD, 0, 0);
    if (corfirst)
        write_item(DITEM(SHIFT_TO_PAUSE(DREAD, 0)));
    else
        write_item(DITEM(SHIFT_TO_EXIT1(DREAD, 0)));
    uint64_t ret = read_data_int(__LINE__, ftdi, 4);
    if (corfirst)
        write_item(DITEM(PAUSE_TO_SHIFT, SHIFT_TO_EXIT1(0, 0x80)));
    write_item(DITEM(EXIT1_TO_IDLE));
    write_irreg(ftdi, 0, IRREG_BYPASS, 0, use_second, combo, 0);
    flush_write(ftdi, NULL);
    return ret;
}

static void read_config_memory(struct ftdi_context *ftdi, int fd, uint32_t size)
{
#if 0
    static uint32_t req_stat[] = {
        CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_READ,CONFIG_REG_STAT,1),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)};
    static uint32_t req_rcrc[] = {
        CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_CMD,1), CONFIG_CMD_RCRC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)};
#endif
    uint32_t req_rcfg[] = {
        CONFIG_DUMMY, CONFIG_SYNC,
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_CMD,1), CONFIG_CMD_RCFG,
        CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_FAR,1), 0,
        CONFIG_TYPE1(CONFIG_OP_READ,CONFIG_REG_FDRO,0),
        CONFIG_TYPE2(size),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0),
        CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0)};

    //readout_seq(ftdi, req_stat, sizeof(req_stat)/sizeof(req_stat[0]), 1, -1, 0, 0, 1, 0);
    //readout_seq(ftdi, req_rcrc, sizeof(req_rcrc)/sizeof(req_rcrc[0]), 0, -1, 0, 0, 1, 0);
    write_irreg(ftdi, 0, IRREG_JSHUTDOWN, 0, 0, 0, 0);
    tmsw_delay(6);
    readout_seq(ftdi, req_rcfg, sizeof(req_rcfg)/sizeof(req_rcfg[0]), size, fd, 0, 0, 1, 0);
}

static void set_clock_divisor(struct ftdi_context *ftdi)
{
    flush_write(ftdi, DITEM(TCK_DIVISOR, INT16(30000000/CLOCK_FREQUENCY - 1)));
}
static void bypass_test(struct ftdi_context *ftdi, int argj, int cortex_nowait, int input_shift, int reset, int clock)
{
    int i, j = argj, second = 0;
    uint32_t ret;

    if (trace)
        printf("[%s:%d] start(%d, %d, %d)\n", __FUNCTION__, __LINE__, j, cortex_nowait, input_shift);
    read_idcode(ftdi, input_shift);
    while(j > 0) {
        while (j-- > 0) {
            for (i = 0; i < 4; i++) {
                //if (trace)
                //    printf("[%s:%d] j %d i %d\n", __FUNCTION__, __LINE__, j, i);
                write_irreg(ftdi, 0, EXTEND_EXTRA | IRREG_BYPASS, 1, 0, 0, 0);
                ret = fetch_result(ftdi, IRREG_USER2, i, 1, found_multiple, -1, second);
                if (ret != 0)
                    printf("[%s:%d] nonzero value %x\n", __FUNCTION__, __LINE__, ret);
            }
        }
        if (use_both)
            j = argj;
        second++;
        argj = 0;
    }
    if (found_cortex)
        cortex_bypass(ftdi, cortex_nowait);
    if (reset)
        flush_write(ftdi, DITEM(IDLE_TO_RESET, IN_RESET_STATE));
    if (clock)
        set_clock_divisor(ftdi);
    if (trace)
        printf("[%s:%d] end\n", __FUNCTION__, __LINE__);
}

/*
 * Read status register.
 * In ug470_7Series_Config.pdf, see "Accessing Configuration Registers through
 * the JTAG Interface" and Table 6-3.
 */
static void check_status(struct ftdi_context *ftdi, uint32_t expected, int after, int extra)
{
    static uint32_t req[] = {CONFIG_DUMMY, CONFIG_SYNC, CONFIG_TYPE2(0), CONFIG_TYPE1(CONFIG_OP_READ, CONFIG_REG_STAT, 1), 0};
    if (after)
        write_item(DITEM(IDLE_TO_RESET, IN_RESET_STATE, SHIFT_TO_EXIT1(0, 0), RESET_TO_IDLE));
    else
        write_item(DITEM(IDLE_TO_RESET, RESET_TO_IDLE));
    /*
     * Read Xilinx configuration status register
     * See: ug470_7Series_Config.pdf, Chapter 6
     */
    uint32_t ret = readout_seq(ftdi, req, sizeof(req)/sizeof(req[0]), 1, -1, EXTEND_EXTRA,
        (found_cortex || (use_first ? (extra != 4) : (extra == 3))),
        found_multiple, (use_first && extra == 4) ? extra : (use_second * extra));
    write_item(DITEM(IDLE_TO_RESET));
    uint32_t status = ret >> 8;
    if (verbose && (bitswap[M(ret)] != 2 || status != expected))
        printf("[%s:%d] expect %x mismatch %x\n", __FUNCTION__, __LINE__, expected, ret);
    printf("STATUS %08x done %x release_done %x eos %x startup_state %x\n", status,
        status & 0x4000, status & 0x2000, status & 0x10, (status >> 18) & 7);
}

static void bypass_status(struct ftdi_context *ftdi, int writeb, int btype, int upperbound, int statparam, uint32_t checkval)
{
    int i, j, ret;

    write_item(DITEM(IN_RESET_STATE, RESET_TO_IDLE));
    if (writeb)
        write_bypass(ftdi);
    for (j = 0; j < upperbound; j++) {
        if (!btype) {
            if (j)
                write_item(DITEM(RESET_TO_IDLE));
            ret = fetch_result(ftdi, IRREG_USERCODE, 0, 1,
                found_multiple, -1, (!j) ? use_both : (use_second * 2));
            if (verbose && ret != 0xffffffff)
                printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret);
            for (i = 0; i < 3; i++)
                write_bypass(ftdi);
        }
        else
            bypass_test(ftdi, 3, 1, (j == 1), 0, 0);
        if (!btype || j == 0)
            check_status(ftdi, checkval, btype, statparam);
        if (!btype)
            statparam = 3;
    }
}

static struct ftdi_context *get_deviceid(int device_index)
{
    fpgausb_open(device_index);            /*** Open selected USB interface ***/
    struct ftdi_context *ftdi = init_ftdi();
    /*
     * Set JTAG clock speed and GPIO pins for our i/f
     */
    idcode_array_index = 0;
    if (ftdi) {
        write_item(DITEM(LOOPBACK_END, DIS_DIV_5));
        set_clock_divisor(ftdi);
        write_item(DITEM(SET_BITS_LOW, 0xe8, 0xeb, SET_BITS_HIGH, 0x20, 0x30));
        if (use_both || uinfo[device_index].bcdDevice == 0x700) /* not a zedboard */
            write_item(DITEM(SET_BITS_HIGH, 0x30, 0x00, SET_BITS_HIGH, 0x00, 0x00));
        flush_write(ftdi, DITEM(FORCE_RETURN_TO_RESET)); /*** Force TAP controller to Reset state ***/
        first_time_idcode_read = 1;
        read_idcode(ftdi, 1);
    }
    return ftdi;
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
    while ((i = getopt (argc, argv, "trls:c12")) != -1)
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
        case '1':
            use_first = 1;
            use_both = 1;
            corfirst = 1;
            found_multiple = 1;
            opcode_bits = 5;
            break;
        case '2':
            use_second = 1;
            use_both = 1;
            found_multiple = 1;
            opcode_bits = 5;
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
            idcode_array_index = 0;
            ftdi = get_deviceid(i);  /*** Generic initialization of FTDI chip ***/
            fpgausb_close(ftdi);
            if (idcode_array_index > 0)
                fprintf(stderr, "; IDCODE:");
            for (j = 0; j < idcode_array_index; j++)
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
     * Set JTAG clock speed and GPIO pins for our i/f
     */
    ftdi = get_deviceid(usb_index);          /*** Generic initialization of FTDI chip ***/
    uint32_t thisid = idcode_array[use_second] & IDCODE_MASK;
    device_type = DEVICE_OTHER;
    if (thisid == DEVICE_AC701)         // ac701
        device_type = DEVICE_AC701;
    else if (thisid == DEVICE_ZC706)         // zc706
        device_type = DEVICE_ZC706;
    else if (thisid == DEVICE_VC707)         // vc707
        device_type = DEVICE_VC707;
    else if (thisid == DEVICE_KC705)         // vc707
        device_type = DEVICE_KC705;
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
    if (idcode_array[1] == CORTEX_IDCODE) {
        found_cortex = 1;
        corfirst = 1;
        found_multiple = 1;
    /*** Depending on the idcode read, change some default actions ***/
        opcode_bits = 5;
    }
    int bypass_tc = 4;
    if (device_type == DEVICE_AC701)
        bypass_tc = 3;
    if (device_type == DEVICE_VC707 && !found_multiple)
        bypass_tc = 6;
    if (device_type == DEVICE_ZEDBOARD)
        bypass_tc = 2;
    if (device_type == DEVICE_ZC702)
        bypass_tc = 1;
    if (use_first)
        bypass_tc += 8;
    int firstflag = device_type == DEVICE_ZC702 || use_first;
    int first_bypass_count = 3 + (device_type == DEVICE_VC707 || device_type == DEVICE_AC701 || device_type == DEVICE_ZC706 || found_multiple);
    int extra_bypass_count = (device_type == DEVICE_VC707 || device_type == DEVICE_AC701 || device_type == DEVICE_ZC706 || (corfirst && (device_type != DEVICE_ZEDBOARD))) + 1;

    /*
     * See if we are reading out data
     */
    if (rflag) {
        fprintf(stderr, "fpgajtag: readout fpga config into xx.bozo\n");
        /* this size was taken from the TYPE2 record in the original bin file
         * (and must be converted to bits)
         */
        int fd = creat("xx.bozo", 0666);
        uint32_t header = {CONFIG_TYPE2(0x000f6c78)};
        header = htonl(header);
        write(fd, &header, sizeof(header));
        read_config_memory(ftdi, fd, 0x000f6c78);
        close(fd);
        return 0;
    }

    /*
     * Read input file
     */
    read_inputfile(argv[optind]);

    /*
     * See if we are in 'command' mode with IR/DR info on command line
     */
    if (cflag) {
        process_command_list(ftdi);
        goto exit_label;
    }

    /*** Check to see if file_idcode matches file and detect Zynq ***/
    if (file_idcode != (idcode_array[use_second] & IDCODE_MASK)) {
        printf("[%s] id %x from file does not match actual id %x\n", __FUNCTION__, file_idcode, idcode_array[use_second]);
        exit(-1);
    }

    bypass_test(ftdi, first_bypass_count, 0, 0, firstflag, firstflag);
    bypass_test(ftdi, 3, 1, firstflag, 1, !firstflag);
    for (i = 0; i < 1 + (firstflag == 0); i++)
        write_item(DITEM(SHIFT_TO_EXIT1(0, 0), IN_RESET_STATE));

    /*
     * Use a pattern of 0xffffffff to validate that we actually understand all the
     * devices in the JTAG chain.  (this list was set up in read_idcode()
     * on the first call
     */
    write_item(DITEM(RESET_TO_IDLE, IDLE_TO_SHIFT_DR));
    send_data_frame(ftdi, DREAD, DITEM(SHIFT_TO_PAUSE(0, 0)),
        idcode_validate_pattern, sizeof(idcode_validate_pattern), SEND_SINGLE_FRAME, 1);
    uint8_t *rdata = read_data(__LINE__, ftdi, idcode_validate_result[0]);
    if (last_read_data_length != idcode_validate_result[0]
     || memcmp(idcode_validate_result+1, rdata, idcode_validate_result[0])) {
        printf("fpgajtag: mismatch validate data\n");
        memdump(idcode_validate_result+1, idcode_validate_result[0], "EXPECT");
        memdump(rdata, last_read_data_length, "ACTUAL");
    }
    write_item(DITEM(FORCE_RETURN_TO_RESET));

    bypass_status(ftdi, found_cortex, 0, 1 + use_both, 4, 0x301900);
    for (i = 0; i < bypass_tc; i++)
        bypass_test(ftdi, 3, 1, (i == 0), 0, 0);

    /*
     * Step 2: Initialization
     */
    write_item(DITEM(IDLE_TO_RESET, IN_RESET_STATE, RESET_TO_IDLE));
    write_irreg(ftdi, 0, IRREG_JPROGRAM, 0, use_second, 0, 0);
    write_irreg(ftdi, 0, IRREG_ISC_NOOP, 0, use_second, 0, 0);
    pulse_gpio(ftdi, 12500 /*msec*/);
    write_irreg(ftdi, DREAD, IRREG_ISC_NOOP, 0, 0, 1, INPROGRAMMING);

    /*
     * Step 6: Load Configuration Data Frames
     */
    write_irreg(ftdi, DREAD, IRREG_CFG_IN, 0, 0, 1, INPROGRAMMING);
    send_data_file(ftdi);

    /*
     * Step 8: Startup
     */
    pulse_gpio(ftdi, 1250 /*msec*/);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_BOOTSTS, 0)) != (corfirst ? 0x03000000 : 0x01000000))
        printf("[%s:%d] CONFIG_REG_BOOTSTS mismatch %x\n", __FUNCTION__, __LINE__, ret);
    write_irreg(ftdi, 0, IRREG_JSTART, 0, use_second, 0, 0);
    write_item(DITEM(RESET_TO_IDLE));
    tmsw_delay(14);
    flush_write(ftdi, DITEM(TMSW, 0x01, 0x00));
    write_irreg(ftdi, DREAD, IRREG_BYPASS, 0, 0, 1, FINISHED);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_STAT, !found_multiple)) != (found_cortex ? 0xf87f1046 : 0xfc791040))
        if (verbose)
            printf("[%s:%d] CONFIG_REG_STAT mismatch %x\n", __FUNCTION__, __LINE__, ret);
    write_item(DITEM(IDLE_TO_RESET));

    bypass_status(ftdi, 1, 1, extra_bypass_count, use_second, 0xf07910);
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
