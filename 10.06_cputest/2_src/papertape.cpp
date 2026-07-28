/* papertape.cpp: reader for DEC absolute loader paper tape images

 Copyright (c) 2026, Angelo Papenhoff, Joerg Hoppe

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 Ported from loadpt() in 10.02_devices/2_src/cpu20/pdp11-master/1120.c, the
 loader of Angelo Papenhoff's standalone PDP-11 emulator. Same tape format, but
 the words go into a testbus_c instead of a global array, and a botched tape is
 reported instead of printed.

 Tape format: a stream of blocks, each

	001 000  <count:16 lo,hi>  <load address:16 lo,hi>  <count-6 data bytes>  <checksum>

 with all bytes of the block summing to 0 mod 256. Anything before the leading
 001 000 is leader and skipped. A block whose load address is 1 is the end
 block, its address field is the start address (which we ignore: the tests set
 the PC themselves).
 */

#include <stdio.h>
#include <string>

#include "papertape.hpp"
#include "testbus.hpp"

std::string papertape_load(const char *filepath, testbus_c &bus)
{
    FILE *f = fopen(filepath, "rb");
    if (f == nullptr)
        return std::string("can not open tape \"") + filepath + "\"";

    uint8_t hi, lo, sum;
    uint16_t n, a, w;

    for (;;) {
        sum = 0;
        // scan for the 001 000 block start, skipping leader
        if (fread(&lo, 1, 1, f) < 1)
            break;
        if (lo != 1)
            continue;
        sum += lo;
        if (fread(&hi, 1, 1, f) < 1)
            break;
        sum += hi;
        w = (uint16_t)(hi << 8 | lo);
        if (w != 1)
            continue;

        // byte count of the whole block, including the 6 header bytes
        if (fread(&lo, 1, 1, f) < 1)
            goto botch;
        sum += lo;
        if (fread(&hi, 1, 1, f) < 1)
            goto botch;
        sum += hi;
        n = (uint16_t)(hi << 8 | lo);

        // load address
        if (fread(&lo, 1, 1, f) < 1)
            goto botch;
        sum += lo;
        if (fread(&hi, 1, 1, f) < 1)
            goto botch;
        sum += hi;
        a = (uint16_t)(hi << 8 | lo);

        if (a == 1)
            break;	// end block: rest is the start address
        if (n == 0)
            continue;
        n -= 6;

        /* Byte by byte, not word by word: a block may hold an odd number of
         data bytes, and may start at an odd address. The XXDP .BIC images of
         the 11/34 diagnostics do both - upstream's loadpt(), which this comes
         from, gave up on such a block ("paper tape botch"). */
        while (n) {
            if (fread(&lo, 1, 1, f) < 1)
                goto botch;
            sum += lo;
            if (!bus.mem_deposit_byte(a, lo)) {
                fclose(f);
                char buff[128];
                snprintf(buff, sizeof buff,
                        "tape loads a byte to %06o, outside of the %u words of memory", a,
                        bus.memory_words());
                return std::string(buff);
            }
            a++;
            n--;
        }

        if (fread(&lo, 1, 1, f) < 1)
            goto botch;
        sum += lo;
        if (sum)
            goto botch;
    }
    fclose(f);
    return "";

botch:
    fclose(f);
    return std::string("paper tape botch in \"") + filepath + "\"";
}
