/* simh_shim_console.c: the console side of the scp stand-in
 *
 * simh's sim_console.c owns a terminal: it puts the host tty in raw mode,
 * handles the ^E escape into the command interpreter, logs to files, and can
 * hand the console to a telnet session. An embedded core wants none of that.
 * It wants two bytes streams, and the embedding says where they come from.
 *
 * So the console here is the host's console_get() and console_put() of
 * simh_shim.h, plus the 7-bit and parity conversions of simh, which belong to
 * the console because the terminal device of every simulator applies them.
 */

#include "simh_shim.h"

#include <ctype.h>

#include "sim_defs.h"
#include "scp.h"
#include "sim_console.h"
#include "simh_shim_internal.h"

t_stat sim_poll_kbd (void)
{
int c = simh_shim_console_get ();

if (c < 0)
    return SCPE_OK;                                     /* nothing waiting */
return (t_stat) c | SCPE_KFLAG;
}

t_stat sim_putchar (int32 c)
{
simh_shim_console_put ((int) c);
return SCPE_OK;
}

/* The _s form tells the caller to retry when the output is blocked. The host
   channel never blocks, so it always succeeds. */
t_stat sim_putchar_s (int32 c)
{
simh_shim_console_put ((int) c);
return SCPE_OK;
}

/* Input conversion, TTUF_MODE_* of sim_console.h: 7P and 7B strip the eighth
   bit, and 7P additionally discards a character that is all-bits-clear after
   stripping. */
int32 sim_tt_inpcvt (int32 c, uint32 mode)
{
uint32 md = mode & TTUF_M_MODE;

if (md != TTUF_MODE_8B) {
    uint32 par_mode = (mode >> TTUF_W_MODE) & TTUF_M_PAR;
    static const int32 nibble_even_parity [] = {
        0x0000, 0x0100, 0x0100, 0x0000, 0x0100, 0x0000, 0x0000, 0x0100,
        0x0100, 0x0000, 0x0000, 0x0100, 0x0000, 0x0100, 0x0100, 0x0000 };

    c = c & 0177;
    if (md == TTUF_MODE_UC)
        c = toupper (c);
    switch (par_mode) {
        case TTUF_PAR_EVEN:
            c |= (nibble_even_parity [c & 0xF] ^ nibble_even_parity [(c >> 4) & 0xF]);
            break;
        case TTUF_PAR_ODD:
            c |= ((~nibble_even_parity [c & 0xF]) ^ nibble_even_parity [(c >> 4) & 0xF]) & 0x100;
            break;
        case TTUF_PAR_MARK:
            c = c | 0x100;
            break;
        }
    }
else
    c = c & 0377;
return c;
}

/* Output conversion. 7P discards a character that would print as nothing. */
int32 sim_tt_outcvt (int32 c, uint32 mode)
{
uint32 md = mode & TTUF_M_MODE;

if (md != TTUF_MODE_8B) {
    c = c & 0177;
    if (md == TTUF_MODE_UC)
        c = toupper (c);
    if ((md == TTUF_MODE_7P) &&
        ((c == 0177) || ((c < 040) && !((c == 007) || (c == 012) || (c == 015)))))
        return -1;
    return c;
    }
return c & 0377;
}
