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

// for using libftdi.so
//#define USE_LIBFTDI

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <libusb.h>

#ifdef USE_LIBFTDI
#include "ftdi.h"
#else
#define MPSSE_WRITE_NEG 0x01   /* Write TDI/DO on negative TCK/SK edge*/
#define MPSSE_BITMODE   0x02   /* Write bits, not bytes */
#define MPSSE_READ_NEG  0x04   /* Sample TDO/DI on negative TCK/SK edge */
#define MPSSE_LSB       0x08   /* LSB first */
#define MPSSE_DO_WRITE  0x10   /* Write TDI/DO */
#define MPSSE_DO_READ   0x20   /* Read TDO/DI */
#define MPSSE_WRITE_TMS 0x40   /* Write TMS/CS */
#define SET_BITS_LOW    0x80
#define SET_BITS_HIGH   0x82
#define LOOPBACK_END    0x85
#define TCK_DIVISOR     0x86
#define DIS_DIV_5       0x8a
#define CLK_BYTES       0x8f
#define SEND_IMMEDIATE  0x87
struct ftdi_context;
#endif

#define USB_TIMEOUT     5000
#define ENDPOINT_IN     0x02
#define ENDPOINT_OUT    0x81
#define USB_CHUNKSIZE   4096
#define USB_INDEX          0

#define USBSIO_RESET                     0 /* Reset the port */
#define USBSIO_RESET_PURGE_RX            1
#define USBSIO_RESET_PURGE_TX            2
#define USBSIO_SET_BAUD_RATE             3 /* Set baud rate */
#define USBSIO_SET_LATENCY_TIMER_REQUEST 9
#define USBSIO_SET_BITMODE_REQUEST       11

static libusb_device_handle *usbhandle = NULL;
static FILE *logfile;
#if defined(USE_LOGGING)
static int logging = 1;
#else
static int logging;
#endif
static int trace;

static int dont_run_pciescan;
static int skip_penultimate_byte = 1;
static int logall = 1;
static int datafile_fd = -1;
static void openlogfile(void);

#include "dumpdata.h"

#define BUFFER_MAX_LEN      1000000
#define FILE_READSIZE          6464
#define MAX_SINGLE_USB_DATA    4045
#define DITEM(...) ((uint8_t[]){sizeof((uint8_t[]){ __VA_ARGS__ }), __VA_ARGS__})
#define M(A)               ((A) & 0xff)
#define INT16(A)           M(A), M((A) >> 8)
#define INT32(A)           INT16(A), INT16((A) >> 16)
#define BSWAP(A) ((((A) & 1) << 7) | (((A) & 2) << 5) | (((A) & 4) << 3) | (((A) & 8) << 1) \
         | (((A) & 0x10) >> 1) | (((A) & 0x20) >> 3) | (((A) & 0x40) >> 5) | (((A) & 0x80) >> 7))

/*
 * FTDI constants
 */
#define MREAD   (MPSSE_LSB|MPSSE_READ_NEG)
#define MWRITE  (MPSSE_LSB|MPSSE_WRITE_NEG)
#define DREAD   (MPSSE_DO_READ  | MREAD)
#define DWRITE  (MPSSE_DO_WRITE | MWRITE)

#define TMSW  (MPSSE_WRITE_TMS      |MWRITE|MPSSE_BITMODE)//4b
#define TMSRW (MPSSE_WRITE_TMS|DREAD|MWRITE|MPSSE_BITMODE)//6f

#define DATAWBIT  (DWRITE|MPSSE_BITMODE)       //1b
#define DATARBIT  (DREAD |MPSSE_BITMODE)       //2e
#define DATARWBIT (DREAD |DWRITE|MPSSE_BITMODE)//3f
#define DATAW(READA, A)    (DWRITE|(READA)), INT16((A)-1) //(0)->19 (DREAD)->3d
#define DATAR(A)           DREAD, INT16((A)-1) //2c

#define IDLE_TO_SHIFT_IR   TMSW, 0x03, 0x03  /* Idle -> Shift-IR */
#define IDLE_TO_SHIFT_DR   TMSW, 0x02, 0x01  /* Idle -> Shift-DR */
#define EXIT1_TO_IDLE      TMSW, 0x01, 0x01  /* Exit1/Exit2 -> Idle */
#define IDLE_TO_RESET      TMSW, 0x02, 0x07  /* Idle -> Reset */
#define RESET_TO_IDLE      TMSW, 0x00, 0x00  /* Reset -> Idle */
#define IN_RESET_STATE     TMSW, 0x00, 0x7f  /* Marker for Reset */
#define SHIFT_TO_EXIT1(READA, A) \
     TMSW | (READA), 0x00, ((A) | 0x01)             /* Shift-IR -> Exit1-IR */
#define SHIFT_TO_PAUSE(READA, A) \
     TMSW | (READA), 0x01, ((A) | 0x01)             /* Shift-IR -> Pause-IR */
#define SHIFT_TO_UPDATE(READA, A) \
     TMSW | (READA), 0x01, ((A) | 0x03)
#define SHIFT_TO_UPDATE_TO_IDLE(READA, A) \
     TMSW | (READA), 0x02, ((A) | 0x03)    /* Shift-DR -> Update-DR -> Idle */
#define FORCE_RETURN_TO_RESET TMSW, 0x04, 0x1f /* go back to TMS reset state */
#define RESET_TO_SHIFT_DR     TMSW, 0x03, 0x02  /* Reset -> Shift-DR */
#define TMS_WAIT \
         TMSW, 0x06, 0x00, TMSW, 0x06, 0x00, TMSW, 0x06, 0x00
#define TMSW_DELAY                                             \
         RESET_TO_IDLE,  /* Hang out in Idle for a while */ \
         TMS_WAIT, TMS_WAIT, TMS_WAIT, TMS_WAIT, \
         TMSW, 0x06, 0x00, TMSW, 0x06, 0x00, TMSW, 0x01, 0x00
#define PAUSE_TO_SHIFT       TMSW, 0x01, 0x01 /* Pause-DR -> Shift-DR */

#define EXTEND_EXTRA            0xc0
#define EXTRA_BIT_SHIFT         12
#define EXTRA_IRREG_BIT_SHIFT    8
#define EXTRA_BIT_MASK          (1<<EXTRA_BIT_SHIFT)
#define EXTRA_BIT_ADDITION(A)   (((A) >> (EXTRA_BIT_SHIFT - 7)) & 0x80)
#define EXTRA_BIT(READA, B)     DATAWBIT | (READA), 0x02, M((IRREG_BYPASS<<4) | (B)),

static struct libusb_context *usb_context;
static int number_of_devices = 1;
static uint8_t bitswap[256];
static int last_read_data_length;
static int found_232H;
static int found_zynq;
static uint8_t usbreadbuffer[USB_CHUNKSIZE];
static uint8_t *usbreadbuffer_ptr = usbreadbuffer;
static uint8_t *idle_to_reset = DITEM(IDLE_TO_RESET);
static uint8_t *shift_to_exit1 = DITEM(SHIFT_TO_EXIT1(0, 0));
static int opcode_bits = 4;
static int irreg_extrabit = 0;

static void openlogfile(void)
{
    if (!logfile)
        logfile = fopen("/tmp/xx.logfile2", "w");
    if (datafile_fd < 0)
        datafile_fd = creat("/tmp/xx.datafile2", 0666);
}

static void memdump(const uint8_t *p, int len, char *title)
{
int i;

    i = 0;
    while (len > 0) {
        if (title && !(i & 0xf)) {
            if (i > 0)
                printf("\n");
            printf("%s: ",title);
        }
        printf("0x%02x, ", *p++);
        i++;
        len--;
    }
    if (title)
        printf("\n");
}

/*
 * USB interface
 */
static void init_usb(const char *serialno)
{
    int cfg, type = 0, i = 0, baudrate = 9600;
    static const char frac_code[8] = {0, 3, 2, 4, 1, 5, 6, 7};
    int best_divisor = 12000000*8 / baudrate;
    unsigned long encdiv = (best_divisor >> 3) | (frac_code[best_divisor & 0x7] << 14);
    libusb_device **device_list, *dev, *usbdev = NULL;
    struct libusb_device_descriptor desc;
    struct libusb_config_descriptor *config_descrip;

    /*
     * Locate USB interface for JTAG
     */
    if (libusb_init(&usb_context) < 0
     || libusb_get_device_list(usb_context, &device_list) < 0) {
        printf("libusb_init failed\n");
        exit(-1);
    }
    while ((dev = device_list[i++]) ) {
        if (libusb_get_device_descriptor(dev, &desc) < 0)
            break;
        if ( desc.idVendor == 0x403 && (desc.idProduct == 0x6001 || desc.idProduct == 0x6010
         || desc.idProduct == 0x6011 || desc.idProduct == 0x6014)) {
            unsigned char serial[64], manuf[64], descrip[128];
            libusb_ref_device(dev);
            if (libusb_open(dev, &usbhandle) < 0
             || libusb_get_string_descriptor_ascii(usbhandle, desc.iManufacturer, manuf, sizeof(manuf)) < 0
             || libusb_get_string_descriptor_ascii(usbhandle, desc.iProduct, descrip, sizeof(descrip)) < 0
             || libusb_get_string_descriptor_ascii(usbhandle, desc.iSerialNumber, serial, sizeof(serial)) < 0)
                goto error;
            printf("[%s] %s:%s:%s\n", __FUNCTION__, manuf, descrip, serial);
            if (!serialno || !strcmp(serialno, (char *)serial)) {
                usbdev = dev;
                break;
            }
            libusb_close (usbhandle);
        }
    }
    libusb_free_device_list(device_list,1);
    if (!usbdev || libusb_get_config_descriptor(usbdev, 0, &config_descrip) < 0)
        goto error;
    int configv = config_descrip->bConfigurationValue;
    libusb_free_config_descriptor (config_descrip);
    libusb_detach_kernel_driver(usbhandle, 0);
#define USBCTRL(A,B,C) \
     libusb_control_transfer(usbhandle, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE \
           | LIBUSB_ENDPOINT_OUT, (A), (B), (C) | USB_INDEX, NULL, 0, USB_TIMEOUT)

    if (libusb_get_configuration (usbhandle, &cfg) < 0
     || (desc.bNumConfigurations > 0 && cfg != configv && libusb_set_configuration(usbhandle, configv) < 0)
     || libusb_claim_interface(usbhandle, 0) < 0
     || USBCTRL(USBSIO_RESET, USBSIO_RESET, 0) < 0
     || USBCTRL(USBSIO_SET_BAUD_RATE, (encdiv | 0x20000) & 0xFFFF, ((encdiv >> 8) & 0xFF00)) < 0
     || USBCTRL(USBSIO_SET_LATENCY_TIMER_REQUEST, 255, 0) < 0
     || USBCTRL(USBSIO_SET_BITMODE_REQUEST, 0, 0) < 0
     || USBCTRL(USBSIO_SET_BITMODE_REQUEST, 2 << 8, 0) < 0
     || USBCTRL(USBSIO_RESET, USBSIO_RESET_PURGE_RX, 0) < 0
     || USBCTRL(USBSIO_RESET, USBSIO_RESET_PURGE_TX, 0) < 0)
        goto error;
    //(desc.bcdDevice == 0x700) //kc       TYPE_2232H
    printf("[%s:%d] bcd %x type %d\n", __FUNCTION__, __LINE__, desc.bcdDevice, type);
    if (desc.bcdDevice == 0x900) //zedboard TYPE_232H
        found_232H = 1;
    return;
error:
    printf("Can't find usable usb interface\n");
    exit(-1);
}

#ifndef USE_LIBFTDI
static int ftdi_write_data(struct ftdi_context *ftdi, const unsigned char *buf, int size)
{
    int actual_length;
    if (logging)
        formatwrite(1, buf, size, "WRITE");
    if (libusb_bulk_transfer(usbhandle, ENDPOINT_IN, (unsigned char *)buf, size, &actual_length, USB_TIMEOUT) < 0)
        printf( "usb bulk write failed");
    return actual_length;
}
static int ftdi_read_data(struct ftdi_context *ftdi, unsigned char *buf, int size)
{
    int actual_length = 1;
    do {
        int ret = libusb_bulk_transfer (usbhandle, ENDPOINT_OUT, usbreadbuffer, USB_CHUNKSIZE, &actual_length, USB_TIMEOUT);
        if (ret < 0)
            printf( "usb bulk read failed");
        actual_length -= 2;
    } while (actual_length == 0);
    memcpy (buf, usbreadbuffer+2, actual_length);
    if (actual_length != size) {
        printf("[%s:%d] bozo actual_length %d size %d\n", __FUNCTION__, __LINE__, actual_length, size);
        exit(-1);
        }
    if (logging)
        memdumpfile(buf, actual_length, "READ");
    return actual_length;
}
#endif //end if not USE_LIBFTDI

/*
 * FTDI generic initialization
 */
struct ftdi_context *init_ftdi(void)
{
    static uint8_t illegal_command[] = { 0xaa, SEND_IMMEDIATE };
    static uint8_t command_ab[] = { 0xab, SEND_IMMEDIATE };
    static uint8_t errorcode_aa[] = { 0xfa, 0xaa };
    static uint8_t errorcode_ab[] = { 0xfa, 0xab };
    struct ftdi_context *ftdi = NULL;
    int i;
    uint8_t retcode[2];

#ifdef USE_LIBFTDI
    ftdi = ftdi_new();
    ftdi_set_usbdev(ftdi, usbhandle);
    ftdi->usb_ctx = usb_context;
    ftdi->max_packet_size = 512; //5000;
#endif
    /*
     * Generic command synchronization with ftdi chip
     */
    for (i = 0; i < 4; i++) {
        ftdi_write_data(ftdi, illegal_command, sizeof(illegal_command));
        ftdi_read_data(ftdi, retcode, sizeof(retcode));
        if (memcmp(retcode, errorcode_aa, sizeof(errorcode_aa)))
            memdump(retcode, sizeof(retcode), "RETaa");
    }
    ftdi_write_data(ftdi, command_ab, sizeof(command_ab));
    ftdi_read_data(ftdi, retcode, sizeof(retcode));
    if (memcmp(retcode, errorcode_ab, sizeof(errorcode_ab)))
        memdump(retcode, sizeof(retcode), "RETab");
    return ftdi;
}

#define MAX_ITEM_LENGTH 2000
static int read_size[MAX_ITEM_LENGTH];
static int read_size_ptr;
static void flush_write(struct ftdi_context *ftdi)
{
    int write_length = usbreadbuffer_ptr - usbreadbuffer;
    usbreadbuffer_ptr = usbreadbuffer;
    if (!write_length)
        return;
    ftdi_write_data(ftdi, usbreadbuffer, write_length);
    read_size_ptr = 0;

    const uint8_t *p = usbreadbuffer;
    while (write_length > 0) {
        int plen = 1;
        uint8_t ch = *p;
        unsigned tlen = (p[2] << 8 | p[1]) + 1;
        switch(ch) {
        case 0x85: case 0x87: case 0x8a: case 0xaa: case 0xab:
            break;
        case 0x2e:
            plen = 2;
            break;
        case 0x19: case 0x1b: case 0x2c: case 0x3d: case 0x3f: case 0x4b:
        case 0x6f: case 0x80: case 0x82: case 0x86: case 0x8f:
            plen = 3;
            break;
        default:
            memdump(p-1, write_length, "FOO");
            exit(-1);
        }
        if (ch & MPSSE_DO_READ) {
            if (ch & MPSSE_BITMODE) {
                int bitsize = *(p+1)+1;
                if (ch & MPSSE_WRITE_TMS)
                    bitsize = 1;
                read_size[read_size_ptr] = -bitsize; /* number of bits */
            }
            else if (ch == 0x2c || ch == 0x3d)       /* DATAR or DATARW */
                read_size[read_size_ptr] = tlen;     /* number of bytes */
            else
                read_size[read_size_ptr] = *(p+1);   /* number of bytes */
            read_size_ptr++;
        }
        p += plen;
        write_length -= plen;
        if (ch == 0x19 || ch == 0x3d) {
            p += tlen;
            write_length -= tlen;
        }
    }
}

static uint8_t *read_data(int linenumber, struct ftdi_context *ftdi, int size)
{
    static uint8_t last_read_data[10000];
    int i, expected_len = 0;
    flush_write(ftdi);
    last_read_data_length = 0;
    for (i = 0; i < read_size_ptr; i++) {
//printf("[%s:%d] %d\n", __FUNCTION__, linenumber, read_size[i]);
        if (read_size[i] < 0)
            expected_len++;
        else
            expected_len += read_size[i];
    }
    if (expected_len != size) {
printf("[%s:%d] expected len %d=0x%x size %d\n", __FUNCTION__, linenumber, expected_len, expected_len, size);
        exit(-1);
    }
    if (size) {
        last_read_data_length = ftdi_read_data(ftdi, last_read_data, size);
        uint8_t *p = last_read_data;
        int validbits = 0;
        for (i = 0; i < read_size_ptr; i++) {
            if (read_size[i] < 0) {
                validbits -= read_size[i];
                if (validbits < 0 || validbits > 8) {
                    printf("[%s:%d] validbits %d\n", __FUNCTION__, __LINE__, validbits);
                    exit(-1);
                }
                *p &= (0xff << (8-validbits));
                if (i > 0 && read_size[i-1] < 0) {
                    *(p-1) = *p;
                    *p = 0;
                }
                p++;
            }
            else {
                p += read_size[i];
                validbits = 0;
            }
        }
    }
    return last_read_data;
}

static void write_data(uint8_t *buf, int size)
{
    memcpy(usbreadbuffer_ptr, buf, size);
    usbreadbuffer_ptr += size;
}

static void write_item(uint8_t *buf)
{
    write_data(buf+1, buf[0]);
}

static uint64_t extract_int(uint8_t *bufp, int size)
{
    uint64_t ret = 0;
    uint8_t *backp = bufp + size;
    while (backp > bufp)
        ret = (ret << 8) | bitswap[*--backp];  //each byte is bitswapped
    return ret;
}

static uint64_t read_data_int(int linenumber, struct ftdi_context *ftdi, int size)
{
    return extract_int(read_data(linenumber, ftdi, size), size);
}

static uint8_t *check_read_data(int linenumber, struct ftdi_context *ftdi, uint8_t *buf)
{
    if (trace)
        printf("[%s:%d]\n", __FUNCTION__, linenumber);
    uint8_t *rdata = read_data(linenumber, ftdi, buf[0]);
    if (last_read_data_length != buf[0] || memcmp(buf+1, rdata, buf[0])) {
        printf("[%s] mismatch on line %d\n", __FUNCTION__, linenumber);
        memdump(buf+1, buf[0], "EXPECT");
        memdump(rdata, last_read_data_length, "ACTUAL");
    }
    return rdata;
}

static uint16_t fetch16(int linenumber, struct ftdi_context *ftdi)
{
    return read_data_int(linenumber, ftdi, 2);
}

static uint64_t fetch24(int linenumber, struct ftdi_context *ftdi)
{
    return read_data_int(linenumber, ftdi, 2 + found_zynq);
}

static void write_dataw(uint32_t value)
{
    write_item(DITEM(DATAW(0, value)));
}

static void swap32(uint32_t value)
{
#define MS(A)              BSWAP(M(A))
#define SWAP32(A)          MS((A) >> 24), MS((A) >> 16), MS((A) >> 8), MS(A)
    write_item(DITEM(SWAP32(value)));
}
static void write_dswap32(uint32_t value)
{
    write_dataw(4);
    swap32(value);
}

static void pulse_gpio(struct ftdi_context *ftdi, int delay)
{
#define GPIO_DONE            0x10
#define GPIO_01              0x01
#define SET_LSB_DIRECTION(A) SET_BITS_LOW, 0xe0, (0xea | (A))

    write_item(DITEM(SET_LSB_DIRECTION(GPIO_DONE | GPIO_01),
                     SET_LSB_DIRECTION(GPIO_DONE)));
    while(delay > 65536) {
        write_item(DITEM(CLK_BYTES, INT16(65536 - 1)));
        delay -= 65536;
    }
    write_item(DITEM(CLK_BYTES, INT16(delay-1)));
    write_item(DITEM(SET_LSB_DIRECTION(GPIO_DONE | GPIO_01),
                     SET_LSB_DIRECTION(GPIO_01)));
}

static void send_data_frame(struct ftdi_context *ftdi, uint8_t read_param,
    uint8_t *tail, uint8_t *ptrin, int size, int limit_len)
{
    while (size > 0) {
        int rlen = size-1;
        if (rlen > limit_len)
            rlen = limit_len;
        if (rlen < limit_len)
            rlen--;                   // last byte is actually loaded with DATAW command
        write_item(DITEM(DWRITE | read_param, INT16(rlen)));
        write_data(ptrin, rlen+1);
        ptrin += rlen+1;
        if (rlen < limit_len) {
            uint8_t temp[200];
            uint8_t ch = *ptrin++;
            write_item(DITEM(DATAWBIT | read_param, 0x06, ch)); // 7 bits of data here
            memcpy(temp, tail+1, tail[0]);
            *temp |= read_param; // this is a TMS instruction to shift state
            *(temp+2) |= 0x80 & ch; // insert 1 bit of data here
            write_data(temp, tail[0]);
        }
        flush_write(ftdi);
        size -= limit_len+1;
    }
}

/*
 * Xilinx constants
 */
//#define CLOCK_FREQUENCY      15000000
#define CLOCK_FREQUENCY      30000000
#define MAX_PACKET_STRING    10

#define COMBINE_IR_REG(FPGAREG, CORTEXREG) \
     (((CORTEXREG) << EXTRA_IRREG_BIT_SHIFT) | (irreg_extrabit | (FPGAREG)))

/* FPGA registers */
#define IRREG_USER2          COMBINE_IR_REG(0x03, 0xf)
#define IRREG_CFG_OUT        COMBINE_IR_REG(0x04, 0xf)
#define IRREG_CFG_IN         COMBINE_IR_REG(0x05, 0xf)
#define IRREG_USERCODE       COMBINE_IR_REG(0x08, 0xf)
#define IRREG_JPROGRAM       COMBINE_IR_REG(0x0b, 0xf)
#define IRREG_JSTART         COMBINE_IR_REG(0x0c, 0xf)
#define IRREG_ISC_NOOP       COMBINE_IR_REG(0x14, 0xf)
#define IRREG_BYPASS         COMBINE_IR_REG(((1<<EXTRA_BIT_SHIFT) | 0x3f), 0xf) // even on PCIE, this has an extra bit

/* ARM JTAG-DP registers */
#define IRREGA_ABORT         COMBINE_IR_REG(0xff, 0x8)   /* 35 bit register */
#define IRREGA_DPACC         COMBINE_IR_REG(0xff, 0xa)   /* Debug Port access, 35 bit register */
#define IRREGA_APACC         COMBINE_IR_REG(0xff, 0xb)   /* Access Port access, 35 bit register */
    #define AP_CSW           0
    #define AP_TAR           2
    #define AP_DRW           6
#define IRREGA_IDCODE        COMBINE_IR_REG(0xff, 0xe)   /* 32 bit register */
#define IRREGA_BYPASS        COMBINE_IR_REG(0xff, 0xf)

#define SMAP_DUMMY           0xffffffff
#define SMAP_SYNC            0xaa995566

// Type 1 Packet
#define SMAP_TYPE1(OPCODE,REG,COUNT) \
    (0x20000000 | ((OPCODE) << 27) | ((REG) << 13) | (COUNT))
#define SMAP_OP_NOP         0
#define SMAP_OP_READ        1
#define SMAP_OP_WRITE       2
#define SMAP_REG_CMD     0x04  // CMD register, Table 5-22
#define     SMAP_CMD_DESYNC 0x0000000d  // end of configuration
#define SMAP_REG_STAT    0x07  // STAT register, Table 5-25
#define SMAP_REG_BOOTSTS 0x16  // BOOTSTS register, Table 5-35

// Type 2 Packet
#define SMAP_TYPE2(LEN) (0x40000000 | (LEN))

#define PATTERN1 \
         INT32(0xff), INT32(0xff), INT32(0xff), INT32(0xff), INT32(0xff), \
         INT32(0xff), INT32(0xff), INT32(0xff), INT32(0xff), INT32(0xff), \
         INT32(0xff), INT32(0xff), INT32(0xff), INT32(0xff), INT32(0xff)

#define PATTERN2 \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff), \
         INT32(0xffffffff), INT32(0xffffffff), INT32(0xffffffff)

#define CORTEX_IDCODE 0x4ba00477
static uint8_t idcode_pattern1[] = DITEM(INT32(0), PATTERN1, 0x00); // starts with idcode
static uint8_t idcode_pattern2[] = DITEM(INT32(0), PATTERN2, 0x00); // starts with idcode
static uint8_t *command_ending;
static int idcode_setup;

#define SET_CLOCK_DIVISOR    TCK_DIVISOR, INT16(30000000/CLOCK_FREQUENCY - 1)

/****************** Cortex stuff ******************/
#define LOADDR(AREAD, A, B) \
    IDLE_TO_SHIFT_DR, DATAWBIT, 0x00, 0x00, \
    DATAW((AREAD), 4), INT32((((uint64_t)(A)) << 3) | (B)), \
    DATAWBIT | (AREAD), 0x01, (((A)>>29) & 0x3f),\
    SHIFT_TO_UPDATE_TO_IDLE((AREAD),(((A)>>24) & 0x80))

#define VAL1          0x15137030
#define VAL2          0x0310c002
#define VAL3          0x1f000200
#define VAL4          0x03008002
#define VAL5          0x00028000
#define VAL6          0xe001b400
#define SELECT_DEBUG  0x01000000

/* request */
#define DPACC_CTRL     (1 << 1)
    // Coresight: Figure 2-14
    #define CORTEX_DEFAULT_STATUS 0xf0000001
    // CSYSPWRUPACK,CSYSPWRUPREQ,CDBGPWRUPACK,CDBGPWRUPREQ,ORUNDETECT
#define DPACC_SELECT   (2 << 1)
#define DPACC_RDBUFF   (3 << 1)

#define DPACC_WRITE        0x1

/* response */
#define DPACC_RESPONSE_OK 0x2
#define DEFAULT_CSW   0xe0000042
                      // Coresight: Table 2-20
                      // DbgStatus=1 -> AHB transfers permitted
                      // Size=2      -> 32 bits

/* Addresses from ug585-Zynq-7000-TRM.pdf */
#define ADDRESS_DEVCFG_MCTRL      0xf8007080
#define ADDRESS_SLCR_ARM_PLL_CTRL 0xf8000100
#define ADDRESS_SLCR_ARM_CLK_CTRL 0xf8000120

static void read_rdbuff(void)
{
    write_item(DITEM(LOADDR(DREAD, 0, DPACC_RDBUFF | DPACC_WRITE)));
}

static void exit1_to_idle(void)
{
    write_item(DITEM(EXIT1_TO_IDLE));
}

static void write_jtag_irreg_extra(int read, int command, int goto_idle)
{
    if (trace)
        printf("[%s] read %x command %x goto %x\n", __FUNCTION__, read, command, goto_idle);
    write_item(DITEM(IDLE_TO_SHIFT_IR, DATAWBIT | (read), opcode_bits, M(command)));
    if (found_zynq)
        write_item(DITEM(EXTRA_BIT(read, ((command >> EXTRA_IRREG_BIT_SHIFT) & 0xf))));
    if (goto_idle == 2)
        write_item(DITEM(SHIFT_TO_UPDATE(0, 0x80)));
    else if (goto_idle)
        write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE((read), EXTRA_BIT_ADDITION(command))));
    else {
        write_item(DITEM(SHIFT_TO_EXIT1((read), EXTRA_BIT_ADDITION(command))));
        exit1_to_idle();
    }
}

static void write_jtag_irreg_short(int read, int command, int extra_bit)
{
    write_item(DITEM(IDLE_TO_SHIFT_IR, DATAWBIT | (read), 4, M(command)));
    if (found_zynq)
        write_item(DITEM(SHIFT_TO_PAUSE(DREAD, extra_bit)));
    else
        write_item(DITEM(SHIFT_TO_EXIT1((read), EXTRA_BIT_ADDITION(command))));
}

static uint16_t write_combo_irreg(struct ftdi_context *ftdi, int command, int extra_bit)
{
    write_jtag_irreg_short(DREAD, command, extra_bit);
    write_item(DITEM(SEND_IMMEDIATE));
    uint16_t ret16 = fetch16(__LINE__, ftdi);
    if (found_zynq) {
        write_item(DITEM(PAUSE_TO_SHIFT));
        write_item(DITEM(EXTRA_BIT(0, 0xf) SHIFT_TO_EXIT1(0, 0x80)));
    }
    exit1_to_idle();
    return ret16;
}

static uint32_t write_bypass(struct ftdi_context *ftdi)
{
    if (found_zynq)
        write_item(DITEM(IDLE_TO_SHIFT_IR, DATAW(DREAD, 1), 0xff,
                  DATARWBIT, 0x00, 0xff, SHIFT_TO_UPDATE_TO_IDLE(DREAD, 0x80)));
    else
        write_jtag_irreg_extra(DREAD, EXTEND_EXTRA | IRREG_BYPASS, 1);
    write_item(DITEM(SEND_IMMEDIATE));
    return fetch24(__LINE__, ftdi);
}

static uint8_t *check_read_cortex(int linenumber, struct ftdi_context *ftdi, uint32_t *buf, int load)
{
    uint8_t *rdata;
    int err = 0;
    uint32_t tempdata[100];
    uint32_t tempdata_index = 0;
    uint32_t *testp = buf+1;

    if (load)
        write_jtag_irreg_extra(0, IRREGA_DPACC, 2);
    write_item(DITEM(LOADDR(DREAD, 0, DPACC_CTRL | DPACC_WRITE)));
    read_rdbuff();
    write_item(DITEM(SEND_IMMEDIATE));
    rdata = read_data(linenumber, ftdi, buf[0] * 6);
    if (last_read_data_length != buf[0]*6) {
        err = 1;
        printf("[%s] mismatch on line %d act %d exp %d\n", __FUNCTION__, linenumber, last_read_data_length/6, buf[0]);
    }
    int i;
    uint8_t *bufp = rdata;
    for (i = 0; i < last_read_data_length/6; i++) {
        uint64_t ret = 0;
        *(bufp+4) >>= 5; // only 3 bits valid in MSB byte
        memcpy(&ret, bufp, 5);
        int bottom = ret & 0x7;
        if (bottom != DPACC_RESPONSE_OK) /* IHI0031C_debug_interface_as.pdf: 3.4.3 */
            printf("[%s:%d] Error in cortex response %x \n", __FUNCTION__, linenumber, bottom);
        ret >>= 3;
        uint32_t ret32 = ret;
        tempdata[tempdata_index++] = ret32;
        if (ret32 != *testp) {
            printf("[%s:%d] Error [%ld] actual %x expect %x \n", __FUNCTION__, linenumber, testp - buf, ret32, *testp);
            memdump(bufp, 6, "RX");
            err = 1;
        }
        testp++;
        bufp += 6;
    }
    if (err) {
        printf("[%s:%d]\n    cresp = (uint32_t[]){%d,", __FUNCTION__, linenumber, tempdata_index);
        for (i = 0; i < tempdata_index; i++)
            printf(" 0x%08x,", tempdata[i]);
        printf("};\n");
    }
    return rdata;
}

static void cortex_pair(uint32_t v)
{
    write_item(DITEM(LOADDR(0, v, AP_TAR), LOADDR(DREAD, 0x0300c002, AP_DRW), LOADDR(DREAD, VAL2, AP_DRW)));
}

static void write_select(int bus)
{
    // Coresight: Table 2-11
    static uint32_t selreq[] = {0,       /* main system bus */
                     SELECT_DEBUG};      /* dedicated Debug Bus */
    write_jtag_irreg_extra(0, IRREGA_DPACC, 2);
    write_item(DITEM(LOADDR(0, selreq[bus], DPACC_SELECT)));
    write_jtag_irreg_extra(0, IRREGA_APACC, 2);
}

static uint8_t *cortex_reset = DITEM(RESET_TO_IDLE, TMSW, 0x01, 0x00);
static uint8_t *waitreq[2] = {DITEM(RESET_TO_IDLE, TMS_WAIT, TMSW, 0x03, 0x00),
                              DITEM(RESET_TO_IDLE, TMS_WAIT, TMSW, 0x02, 0x00)};
static void cortex_csw(struct ftdi_context *ftdi, int wait, int clear_wait)
{
    uint32_t *cresp[2];
    int i;

    write_jtag_irreg_extra(0, IRREGA_ABORT, 2);
    write_item(DITEM(LOADDR(0, 1, 0)));
    write_jtag_irreg_extra(0, IRREGA_DPACC, 2);
    write_item(DITEM(LOADDR(0, 0x50000033, DPACC_CTRL)));
    // in Debug, 2.3.2: CTRL/STAT, Control/Status register
    // CSYSPWRUPREQ, CDBGPWRUPREQ, STICKYERR, STICKYCMP, STICKYORUN, ORUNDETECT
    if (!clear_wait)
        cresp[0] = (uint32_t[]){2, CORTEX_DEFAULT_STATUS, CORTEX_DEFAULT_STATUS,};
    else {
        write_jtag_irreg_extra(0, IRREGA_APACC, 2);
        cortex_pair(0x80092088);
        cresp[0] = (uint32_t[]){5, 0, 0, 0, CORTEX_DEFAULT_STATUS, CORTEX_DEFAULT_STATUS,};
    }
    cresp[1] = (uint32_t[]){3, 0, 0x00800042/*SPIStatus=High*/, CORTEX_DEFAULT_STATUS};
    write_jtag_irreg_extra(0, IRREGA_DPACC, 2);
    write_item(DITEM(LOADDR(clear_wait?DREAD:0, 0, DPACC_CTRL | DPACC_WRITE)));
    for (i = 0; i < 2; i++) {
        if (trace)
            printf("[%s:%d] wait %d i %d\n", __FUNCTION__, __LINE__, wait, i);
        check_read_cortex(__LINE__, ftdi, cresp[i], i);
        write_select(i);
        write_item(DITEM(LOADDR(DREAD, 0, AP_CSW | DPACC_WRITE)));
        if (wait)
           write_item(waitreq[i]);
    }
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, SELECT_DEBUG, DEFAULT_CSW, CORTEX_DEFAULT_STATUS,}, 1);
}

static void tar_read(uint32_t v)
{
    write_item(DITEM(LOADDR(DREAD, v, AP_TAR)));
    read_rdbuff();
}
static void tar_write(uint32_t v)
{
    write_jtag_irreg_extra(0, IRREGA_APACC, 2);
    write_item(DITEM(LOADDR(0, v, AP_TAR)));
    read_rdbuff();
}

static void read_csw(struct ftdi_context *ftdi, int wait, uint32_t *creturn1, uint32_t *creturn2)
{
int i;
static uint32_t cread[] = {2, 0x80000002};
uint32_t *cresp[] = {(uint32_t[]){3, 0, DEFAULT_CSW, CORTEX_DEFAULT_STATUS,},
          (uint32_t[]){3, SELECT_DEBUG, DEFAULT_CSW, CORTEX_DEFAULT_STATUS,}};
static uint32_t address_table[] = {ADDRESS_SLCR_ARM_PLL_CTRL, ADDRESS_SLCR_ARM_CLK_CTRL};

    for (i = 0; i < 2; i++) {
        write_select(i);
        write_item(DITEM(LOADDR(DREAD,cread[i], AP_CSW)));
        if (wait)
            write_item(waitreq[i]);
        check_read_cortex(__LINE__, ftdi, cresp[i], 1);
    }
    write_select(0);
    for (i = 0; i < 2; i++) {
        write_item(DITEM(LOADDR(DREAD, address_table[i], AP_TAR)));
        if (wait)
            write_item(waitreq[0]);
        read_rdbuff();
        if (wait)
            write_item(waitreq[0]);
        else
            write_item(cortex_reset);
    }
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){6, 0, DEFAULT_CSW,
          VAL5, VAL5, VAL3, CORTEX_DEFAULT_STATUS,}, 1);
    if (wait) {
        tar_write(ADDRESS_DEVCFG_MCTRL);
        write_item(cortex_reset);
        check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, VAL3, 0, CORTEX_DEFAULT_STATUS,}, 1);
    }
    write_select(1);
    tar_read(0x80090000);
    tar_read(0x80090314);
    tar_read(0x80090088);
    tar_read(0x80090028);
    check_read_cortex(__LINE__, ftdi, creturn1, 1);
    write_jtag_irreg_extra(0, IRREGA_APACC, 2);
    cortex_pair(0x80090088);
    tar_read(0x80092000);
    tar_read(0x80092314);
    tar_read(0x80092088);
    tar_read(0x80092028);
    check_read_cortex(__LINE__, ftdi, creturn2, 1);
}
static void cortex_bypass(struct ftdi_context *ftdi, int cortex_nowait)
{
    cortex_csw(ftdi, 1-cortex_nowait, 0);
    if (!cortex_nowait) {
        read_csw(ftdi, 1, (uint32_t[]){10, SELECT_DEBUG, 0,
                VAL1, VAL1, 1, 1, VAL4, VAL4, 0, CORTEX_DEFAULT_STATUS,},
            (uint32_t[]){12, 0, 0, 0, 0,
                VAL1, VAL1, 1, 1, VAL4, VAL4, 0, CORTEX_DEFAULT_STATUS,});
        int count = number_of_devices - 1;
        while (count-- > 0)
            cortex_csw(ftdi, 0, 1);
    }
    read_csw(ftdi, 0, (uint32_t[]){10, SELECT_DEBUG,
            VAL3, VAL1, VAL1, 1, 1, VAL2, VAL2, 0, CORTEX_DEFAULT_STATUS,},
        (uint32_t[]){12, 0, 0, 0, 0,
            VAL1, VAL1, 1, 1, VAL2, VAL2, 0, CORTEX_DEFAULT_STATUS,});
    write_jtag_irreg_extra(0, IRREGA_APACC, 2);
    cortex_pair(0x80092088);
    tar_read(0x80090314);
    tar_read(0x80090088);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){8, 0, 0, 0, 0, 1, 1,
            VAL2, CORTEX_DEFAULT_STATUS,}, 1);
    tar_write(0x80090084);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, VAL2, VAL6, CORTEX_DEFAULT_STATUS,}, 1);
    tar_write(0x80092314);
    tar_read(0x80092088);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){5, VAL6, 1, 1, VAL2, CORTEX_DEFAULT_STATUS,}, 1);
    tar_write(0x80092084);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, VAL2, VAL6, CORTEX_DEFAULT_STATUS,}, 1);
}

static void check_idcode(struct ftdi_context *ftdi, uint32_t idcode)
{
    static uint8_t patdata[] =  {INT32(0xff), PATTERN1};
    uint32_t returnedid;

    write_item(DITEM(TMSW, 0x04, 0x7f/*Reset?????*/, RESET_TO_SHIFT_DR));
    send_data_frame(ftdi, DREAD, DITEM(SHIFT_TO_UPDATE_TO_IDLE(0, 0), SEND_IMMEDIATE),
        patdata, sizeof(patdata), 9999);
    uint8_t *rdata = read_data(__LINE__, ftdi, idcode_pattern1[0]);
    if (!idcode_setup) {    // only setup idcode patterns on first call!
        memcpy(&returnedid, rdata, sizeof(returnedid));
        idcode |= 0xf0000000 & returnedid;
        memcpy(idcode_pattern2+1, rdata, sizeof(returnedid)); // copy returned idcode
        memcpy(idcode_pattern1+1, rdata, sizeof(returnedid));       // copy returned idcode
        if (memcmp(idcode_pattern1+1, rdata, idcode_pattern1[0])) {
            uint32_t anotherid;
            memcpy(&anotherid, rdata+4, sizeof(anotherid));
            printf("[%s] second device idcode found 0x%x\n", __FUNCTION__, anotherid);
            if (anotherid == CORTEX_IDCODE)
                found_zynq = 1;
            memcpy(idcode_pattern1+4+1, rdata+4, sizeof(anotherid));   // copy 2nd idcode
            memcpy(idcode_pattern2+4+1, rdata+4, sizeof(anotherid));   // copy 2nd idcode
            number_of_devices++;
        }
        idcode_setup = 1;
        if (idcode != returnedid) {
            printf("[%s] id %x from file does not match actual id %x\n", __FUNCTION__, idcode, returnedid);
            exit(1);
        }
    }
    if (memcmp(idcode_pattern1+1, rdata, idcode_pattern1[0])) {
        printf("[%s]\n", __FUNCTION__);
        memdump(idcode_pattern1+1, idcode_pattern1[0], "EXPECT");
        memdump(rdata, idcode_pattern1[0], "ACTUAL");
    }
}

static uint64_t fetch_config_word(int linenumber, struct ftdi_context *ftdi, uint32_t irreg, int i)
{
    write_jtag_irreg_extra(0, EXTEND_EXTRA | irreg, 1);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    if (i > 1)
        write_item(DITEM(DATAWBIT, opcode_bits, 0x0c, SHIFT_TO_UPDATE_TO_IDLE(0, 0), IDLE_TO_SHIFT_DR));
    if (i > 0) {
        write_item(DITEM(DATAW(0, 1), 0x69, DATAWBIT, 0x01, 0x00));
        if (found_zynq)
            write_item(DITEM(DATAWBIT, 0x00, 0x00));
    }
    write_item(command_ending);
    return read_data_int(linenumber, ftdi, 4+skip_penultimate_byte);
}

static void bypass_test(struct ftdi_context *ftdi, int j, int cortex_nowait)
{
    int i;
    uint64_t ret40;

    check_idcode(ftdi, 0); // idcode parameter ignored, since this is not the first invocation
    while (j-- > 0) {
        for (i = 0; i < 4; i++) {
            write_jtag_irreg_extra(0, EXTEND_EXTRA | IRREG_BYPASS, 1);
            if ((ret40 = fetch_config_word(__LINE__, ftdi, IRREG_USER2, i)) != 0)
                printf("[%s:%d] nonzero value %" PRIx64 "\n", __FUNCTION__, __LINE__, ret40);
        }
    }
    if (found_zynq)
        cortex_bypass(ftdi, cortex_nowait);
}

static void write_one_word(int short_format, int value)
{
    if (short_format)
        write_item(DITEM(INT32(value)));
    else
        write_item(DITEM(value, 0x00, 0x00, DATAWBIT, 0x06, 0x00));
}

static void write_eight_bytes(void)
{
    if (found_zynq) {
        write_item(DITEM(DATAW(0, 7), INT32(0)));
        write_one_word(0, 0);
    }
}

static void send_data_file(struct ftdi_context *ftdi, int inputfd)
{
    int size, i;
    uint8_t *tailp = DITEM(SHIFT_TO_PAUSE(0,0));
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    write_eight_bytes();
    write_dswap32(0);
    int limit_len = MAX_SINGLE_USB_DATA - (usbreadbuffer_ptr - usbreadbuffer);
    printf("Starting to send file\n");
    do {
        static uint8_t filebuffer[FILE_READSIZE];
        size = read(inputfd, filebuffer, FILE_READSIZE);
        for (i = 0; i < size; i++)
            filebuffer[i] = bitswap[filebuffer[i]];
        if (size < FILE_READSIZE) {
            if (found_zynq)
                tailp = DITEM(EXIT1_TO_IDLE, EXIT1_TO_IDLE, SHIFT_TO_EXIT1(0, 0x80), EXIT1_TO_IDLE);
            else
                tailp = DITEM(SHIFT_TO_EXIT1(0, 0), EXIT1_TO_IDLE);
        }
        send_data_frame(ftdi, 0, tailp, filebuffer, size, limit_len);
        if (size != FILE_READSIZE)
            break;
        write_item(DITEM(PAUSE_TO_SHIFT));
        limit_len = MAX_SINGLE_USB_DATA;
    } while(size == FILE_READSIZE);
    printf("Done sending file\n");
}

static void read_status(struct ftdi_context *ftdi, uint32_t expected)
{
    write_dataw(19 + found_zynq);
    swap32(SMAP_DUMMY);
    swap32(SMAP_SYNC);
    swap32(SMAP_TYPE2(0));
    swap32(SMAP_TYPE1(SMAP_OP_READ, SMAP_REG_STAT, 1));
    write_one_word(found_zynq, 0);
    write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE(0, 0)));
    uint64_t ret40 = fetch_config_word(__LINE__, ftdi, IRREG_CFG_OUT, 0);
    uint32_t status = ret40 >> 8;
    if (M(ret40) != 0x40 || status != expected)
        printf("[%s:%d] expect %x mismatch %" PRIx64 "\n", __FUNCTION__, __LINE__, expected, ret40);
    printf("STATUS %08x done %x release_done %x eos %x startup_state %x\n", status,
        status & 0x4000, status & 0x2000, status & 0x10, (status >> 18) & 7);
}
static uint64_t read_smap(struct ftdi_context *ftdi, uint32_t data)
{
    write_jtag_irreg_extra(0, IRREG_CFG_IN, 0);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    write_dswap32(SMAP_DUMMY);
    write_eight_bytes();
    write_dswap32(SMAP_SYNC);
    write_dswap32(SMAP_TYPE1(SMAP_OP_NOP, 0,0));
    write_dswap32(SMAP_TYPE1(SMAP_OP_READ, data, 1));
    write_dswap32(SMAP_TYPE1(SMAP_OP_NOP, 0,0));
    write_dswap32(SMAP_TYPE1(SMAP_OP_NOP, 0,0));
    write_dswap32(SMAP_TYPE1(SMAP_OP_WRITE, SMAP_REG_CMD, 1));
    write_dswap32(SMAP_CMD_DESYNC);
    write_dswap32(SMAP_TYPE1(SMAP_OP_NOP, 0,0));
    int temp = 0;
    write_item(DITEM(DATAW(0, 3+found_zynq)));
    if (found_zynq)
        temp = 0x80;
    write_one_word(found_zynq, 4);
    write_item(DITEM(SHIFT_TO_EXIT1(0, temp)));
    exit1_to_idle();
    write_jtag_irreg_extra(0, IRREG_CFG_OUT, 0);
    write_item(DITEM(IDLE_TO_SHIFT_DR, DATAW(DREAD, 3), 0x00, 0x00, 0x00, DATARWBIT, 0x06, 0x00));
    if (found_zynq)
        write_item(DITEM(SHIFT_TO_PAUSE(DREAD, 0)));
    else
        write_item(DITEM(SHIFT_TO_EXIT1(DREAD, 0)));
    write_item(DITEM(SEND_IMMEDIATE));
    uint64_t ret = read_data_int(__LINE__, ftdi, 5);
    exit1_to_idle();
    if (found_zynq) {
        write_item(DITEM(SHIFT_TO_EXIT1(0, 0x80)));
        exit1_to_idle();
    }
    return ret;
}

int main(int argc, char **argv)
{
    logfile = stdout;
    struct ftdi_context *ftdi;
    uint32_t idcode;
    uint16_t ret16;
    uint64_t ret40;
    int i = 1;
    int inputfd = 0;   /* default input for '-' is stdin */
    const char *serialno = NULL;

    if (argc < 2) {
        printf("%s: [ -s <serialno> ] <filename>\n", argv[0]);
        exit(1);
    }
    if (!strcmp(argv[i], "-s")) {
        serialno = argv[++i];
        i++;
    }
    if (strcmp(argv[i], "-")) {
        inputfd = open(argv[i], O_RDONLY);
        if (inputfd == -1) {
            printf("Unable to open file '%s'\n", argv[1]);
            exit(-1);
        }
    }

    for (i = 0; i < sizeof(bitswap); i++)
        bitswap[i] = BSWAP(i);
    lseek(inputfd, 0x80, SEEK_SET); /*** Read idcode from file to be programmed ***/
    read(inputfd, &idcode, sizeof(idcode));
    idcode = (M(idcode) << 24) | (M(idcode >> 8) << 16) | (M(idcode >> 16) << 8) | M(idcode >> 24);
    lseek(inputfd, 0, SEEK_SET);
    init_usb(serialno);             /*** Initialize USB interface ***/
    ftdi = init_ftdi();             /*** Generic initialization of FTDI chip ***/
    write_item(DITEM(LOOPBACK_END, DIS_DIV_5, SET_CLOCK_DIVISOR,
                     SET_BITS_LOW, 0xe8, 0xeb, SET_BITS_HIGH, 0x20, 0x30));
    if (!found_232H)
        write_item(DITEM(SET_BITS_HIGH, 0x30, 0x00, SET_BITS_HIGH, 0x00, 0x00));
    write_item(DITEM(FORCE_RETURN_TO_RESET));
    flush_write(ftdi);
    write_item(shift_to_exit1);
    check_idcode(ftdi, idcode);     /*** Check to see if idcode matches file and detect Zynq ***/
    if (found_zynq) {
        dont_run_pciescan = 1;
        skip_penultimate_byte = 0;
        command_ending = DITEM(DATAR(4), SHIFT_TO_UPDATE_TO_IDLE(0, 0), SEND_IMMEDIATE);
        opcode_bits = 0x05;
        irreg_extrabit = EXTRA_BIT_MASK;
    }
    else
        command_ending = DITEM(DATAR(3), DATARBIT, 0x06, SHIFT_TO_UPDATE_TO_IDLE(DREAD, 0), SEND_IMMEDIATE);

    /*
     * Step 5: Check Device ID
     */

    write_item(idle_to_reset);
    bypass_test(ftdi, 2 + number_of_devices, 0);
    write_item(idle_to_reset);
    bypass_test(ftdi, 3, 1);
    write_item(DITEM(IDLE_TO_RESET, IN_RESET_STATE));
    flush_write(ftdi);
    write_item(DITEM(SET_CLOCK_DIVISOR));
    flush_write(ftdi);

    static uint8_t iddata[] = {INT32(0xffffffff),  PATTERN2};
    write_item(DITEM(SHIFT_TO_EXIT1(0, 0), IN_RESET_STATE, SHIFT_TO_EXIT1(0, 0),
             IN_RESET_STATE, RESET_TO_IDLE, IDLE_TO_SHIFT_DR));
    send_data_frame(ftdi, DREAD, DITEM(PAUSE_TO_SHIFT, SEND_IMMEDIATE),
        iddata, sizeof(iddata), 9999);
    check_read_data(__LINE__, ftdi, idcode_pattern2);

    write_item(DITEM(FORCE_RETURN_TO_RESET, IN_RESET_STATE, RESET_TO_IDLE));
    if (found_zynq) {
        ret16 = write_bypass(ftdi);
        printf("[%s:%d] write_bypass return %x\n", __FUNCTION__, __LINE__, ret16);
    }
    if (((ret40 = fetch_config_word(__LINE__, ftdi, IRREG_USERCODE, 0)) & 0xffffffff) != 0xffffffff)
        printf("[%s:%d] mismatch %" PRIx64 "\n", __FUNCTION__, __LINE__, ret40);

    for (i = 0; i < 3; i++) {
        ret16 = write_bypass(ftdi);
        if (ret16 == 0x8 || (found_zynq && ret16 == 0x8a))
            printf("xjtag: bypass first time %x\n", ret16);
        else if (ret16 == 0x11)
            printf("xjtag: bypass next times %x\n", ret16);
        else if (ret16 == 0x2f)
            printf("xjtag: bypass already programmed %x\n", ret16);
        else
            printf("xjtag: bypass mismatch %x\n", ret16);
    }
    write_item(idle_to_reset);
    write_item(DITEM(RESET_TO_IDLE));
    write_jtag_irreg_extra(0, EXTEND_EXTRA | IRREG_CFG_IN, 1);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    read_status(ftdi, 0x301900);
    write_item(idle_to_reset);
    flush_write(ftdi);
    write_item(shift_to_exit1);
    bypass_test(ftdi, 3, 1);
    for (i = 0; i < 3; i++) {
        write_item(idle_to_reset);
        bypass_test(ftdi, 3, 1);
    }
    write_item(DITEM(IDLE_TO_RESET, IN_RESET_STATE, RESET_TO_IDLE));

    /*
     * Step 2: Initialization
     */
    write_jtag_irreg_extra(0, IRREG_JPROGRAM, 0);
    write_jtag_irreg_extra(0, IRREG_ISC_NOOP, 0);
    pulse_gpio(ftdi, CLOCK_FREQUENCY/80/* 12.5 msec */);
    ret16 = write_combo_irreg(ftdi, IRREG_ISC_NOOP, 0);
    if (ret16 != (found_zynq ? 0x04 : 0x22))
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret16);
    /*
     * Step 6: Load Configuration Data Frames
     */
    ret16 = write_combo_irreg(ftdi, IRREG_CFG_IN, 0);
    if (ret16 != (found_zynq ? 0x04 : 0x22))
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret16);
    send_data_file(ftdi, inputfd);
    /*
     * Step 8: Startup
     */
    pulse_gpio(ftdi, CLOCK_FREQUENCY/800/*1.25 msec*/);
    if ((ret40 = read_smap(ftdi, SMAP_REG_BOOTSTS)) != (found_zynq ? 0x03000000 : 0x01000000))
        printf("[%s:%d] mismatch %" PRIx64 "\n", __FUNCTION__, __LINE__, ret40);
    write_jtag_irreg_extra(0, IRREG_BYPASS, 0);
    write_jtag_irreg_extra(0, IRREG_JSTART, 0);
    write_item(DITEM(TMSW_DELAY));
    ret16 = write_combo_irreg(ftdi, IRREG_BYPASS, 0x80);
    if (ret16 != (found_zynq ? 0x17 : 0x2b))
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret16);
    if ((ret40 = read_smap(ftdi, SMAP_REG_STAT)) != (found_zynq ? 0xf87f1046 : 0xfc791040))
        printf("[%s:%d] mismatch %" PRIx64 "\n", __FUNCTION__, __LINE__, ret40);
    if (found_zynq)
        write_jtag_irreg_extra(0, IRREG_BYPASS, 0);
    else {
        write_jtag_irreg_short(0, IRREG_BYPASS, 0);
        exit1_to_idle();
        flush_write(ftdi);
    }
    write_item(DITEM(IDLE_TO_RESET, IN_RESET_STATE, RESET_TO_IDLE));
    ret16 = write_bypass(ftdi);
    if (ret16 != (found_zynq ? 0xae : 0x2f))
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret16);
    if (!found_zynq) {
        write_item(idle_to_reset);
        bypass_test(ftdi, 3, 1);
    }
    write_item(idle_to_reset);
    write_item(DITEM(IN_RESET_STATE, SHIFT_TO_EXIT1(0, 0)));
    write_item(DITEM(RESET_TO_IDLE));
    write_jtag_irreg_extra(0, EXTEND_EXTRA | IRREG_CFG_IN, 1);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    read_status(ftdi, 0xf07910);
    write_item(idle_to_reset);
    if (found_zynq) {
        write_item(shift_to_exit1);
        bypass_test(ftdi, 3, 1);
        write_item(idle_to_reset);
        bypass_test(ftdi, 3, 1);
    }
    flush_write(ftdi);
#ifdef USE_LIBFTDI
    ftdi_deinit(ftdi);
#else
    libusb_close (usbhandle);
    libusb_exit(usb_context);
#endif
    fflush(stdout);
    if (!dont_run_pciescan)
        execlp("/usr/local/bin/pciescanportal", "arg", (char *)NULL); /* rescan pci bus to discover device */
    return 0;
}
