/* webdebug.hpp: what the processor holds, for the debug panel

   Copyright (c) 2026, Frits Jalvingh
   jal@etc.to
   MIT license, see webserver.hpp for the full text.

   One endpoint, GET /api/debug/cpu, answering the same document whichever kind
   of processor runs the machine. Where the answer comes from is in "source":

     emulated  a core of this board's own (CPU20, CPU34). Read where the
               registers lie, so it costs no bus cycle, disturbs nothing and
               may be polled.
     bus       a processor that puts its register file in the I/O page, as the
               11/45, 11/55 and 11/70 do at 777700..777717 with the status word
               at 777776. Read by DATI, so it costs a bus cycle per register
               and only says what the machine in front of the board answers.
     none      neither: the small machines (11/04, 11/20, 11/34, 11/40, ...)
               give their registers no bus address at all, and a J11 keeps its
               own. Those are reachable only through the front panel or the
               processor's console ODT, which this endpoint does not drive.

   Which one applies is never assumed: an emulated processor is asked directly,
   and everything else is settled by probing the window and reporting what
   answered.
*/
#ifndef _WEBDEBUG_HPP_
#define _WEBDEBUG_HPP_

#include "civetweb.h"

// GET /api/debug/cpu
void webdebug_register(struct mg_context *ctx);

#endif // _WEBDEBUG_HPP_
