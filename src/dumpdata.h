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

enum {
    state_unknown,
    state_reset,
    state_idle,
    state_selectd,
    state_capture,
    state_shift,
    state_exit1,
    state_pause,
    state_exit2,
    state_update,
    state_selecti};

static const char *stateName[] = {
    "",
    " reset ",
    " idle ",
    " selectd ",
    " capture%s ",
    " shift%s ",
    " exit1%s ",
    " pause%s ",
    " exit2%s ",
    " update%s ",
    " selecti "};

//0   1
static int nextState[][2] = {
    {state_unknown, state_unknown}, //state_unknown
    {state_idle, state_reset}, //state_reset
    {state_idle, state_selectd}, //state_idle
    {state_capture, state_selecti}, //state_selectd
    {state_shift, state_exit1}, //state_capture
    {state_shift, state_exit1}, //state_shift
    {state_pause, state_update}, //state_exit1
    {state_pause, state_exit2}, //state_pause
    {state_shift, state_update}, //state_exit2
    {state_idle, state_selectd}, //state_update
    {state_capture, state_reset}}; //state_selecti
static int currentState;
const char *selectType = "";

static void memdumpfile(const unsigned char *p, int len, const char *title)
{
int i;

    openlogfile();
    i = 0;
    while (len > 0) {
        if (!(i & 0xf)) {
            if (i > 0)
                fprintf(fpgajtag_logfile, "\n");
            fprintf(fpgajtag_logfile, "%s ",title);
        }
        fprintf(fpgajtag_logfile, "0x%02x, ", *p++);
        i++;
        len--;
    }
    fprintf(fpgajtag_logfile, "\n");
}
static int started = 0;
static void formatwrite(int submit, const unsigned char *p, int len, const char *title)
{
   static unsigned char bitswap[256];
   static int once = 1;
   static const char *header = "    ";
   static char header_data[200];
   if (logall)
       header = "WRITE";
   strcpy(header_data, header);
   strcat(header_data, "   ");
    openlogfile();
   
    while (len > 0) {
        const unsigned char *pstart = p;
        int plen = 1;
        unsigned char ch = *p;
#define GET_LEN() ((*(p+1) + (*(p+2) << 8))+1)
        int dataLen = 0;
        int writeTDI = (ch & 0x10);
        int writeTMS = (ch & 0x40);
        printf("COMMAND %02x", ch);
        if (!(ch & 0x80)) {  // Data shifting command
            if (ch & 0x2) { // bit mode
               dataLen = *(p+1) + 1;
               plen++;
               printf(": bit datalen %d: ", dataLen);
               if (writeTDI || writeTMS) { // write
                   int cdata = *(p+2);
                   if (writeTMS)
                       printf("TDI %x TMS ", cdata >> 7);
                   for (int i = 0; i < dataLen; i++) {
                       int data = cdata & 1;
                       printf("%x", data);
                       cdata >>= 1;
                       currentState = nextState[currentState][data];
                       if (writeTMS) {
                           if (currentState == state_selectd)
                               selectType = "D";
                           else if (currentState == state_selecti)
                               selectType = "I";
                       }
                   }
                   if (writeTMS && dataLen == 5 && (*(p+2) & 0x1f) == 0x1f)
                       currentState = state_reset;
                   if (writeTMS)
                       printf(stateName[currentState], selectType);
                   plen++;
               }
               printf("\n");
            }
            else { // byte mode
               dataLen = GET_LEN();
               plen += 2;
               printf(": byte datalen %d%s%s", dataLen, writeTDI != 0 ? " TDI" : "", writeTMS != 0 ? " TMS" : "");
               if (writeTDI || writeTMS) { // write
                   plen += dataLen;
                   memdump(p+3, dataLen, "");
               }
               else
                   printf("\n");
            }
        }
        else
        switch(ch) {
        case 0x80: case 0x82: case 0x86: case 0x8f:
        case 0x91: case 0x92:
            plen = 3;
            memdump(p+1, 2, "");
            break;
        case 0x93:
            dataLen = GET_LEN();
            plen = 3; // bytelen
            printf(" bytelen %d\n", dataLen);
            break;
        case 0x81: case 0x83: case 0x84: case 0x85: case 0x87:
        case 0x88: case 0x89: case 0x90: case 0xfa:
        case 0x8a: case 0xaa: case 0xab:
            plen = 1;
            printf("\n");
            break;
        default:
            printf("[%s:%d] UNKNOWNCOMMM %x\n", __FUNCTION__, __LINE__, ch);
            exit(-1);
            break;
        }
        if (started && p[0] == 0x1b && p[1] == 6)
            write(datafile_fd, &bitswap[p[2]], 1);
        p += plen;
        len -= plen;
        if (ch == 0x19 || ch == 0x3d) {
            unsigned tlen = (pstart[2] << 8 | pstart[1]) + 1;
if (tlen > 1500) {
    started = 1;
}
else
    started = 0;    // shutdown before final writes
            if (started) {
                int i;
                for (i = 0; once && i < sizeof(bitswap); i++)
                    bitswap[i] = ((i &    1) << 7) | ((i &    2) << 5)
                       | ((i &    4) << 3) | ((i &    8) << 1)
                       | ((i & 0x10) >> 1) | ((i & 0x20) >> 3)
                       | ((i & 0x40) >> 5) | ((i & 0x80) >> 7);
                unsigned char *pbuf = (unsigned char *)malloc(tlen);
                for (i = 0; i < tlen; i++)
                    pbuf[i] = bitswap[p[i]];
                write(datafile_fd, pbuf, tlen);
                free(pbuf);
                once = 0;
            }
            p += tlen;
            len -= tlen;
        }
    }
    if (len != 0)
        printf("[%s] ending length %d\n", __FUNCTION__, len);
}
