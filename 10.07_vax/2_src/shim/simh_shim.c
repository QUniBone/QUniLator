/* simh_shim.c: scp stand-in for the vendored simh VAX core
 *
 * What simh's command interpreter supplies to a simulator, supplied instead by
 * a few hundred lines that an embedding program can host. simh_shim.h says what
 * the seam is and why it is here.
 *
 * The parts an executing processor needs are real: the event queue, device
 * reset, unit attach and detach, the bootstrap loader, and the diagnostic
 * output. The parts a processor reaches only through a typed command are
 * stubs, each returning SCPE_NOFNC and saying which entry point was reached, so
 * a core that grows a new dependency on scp says so at run time instead of
 * failing quietly.
 */

#include "simh_shim.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scp.h"
#include "sim_fio.h"
#include "simh_shim_internal.h"

/* ------------------------------------------------------------------------ */
/* The host                                                                  */
/* ------------------------------------------------------------------------ */

static simh_shim_host_t shim_host;
static t_bool shim_host_bound = FALSE;

void simh_shim_bind (const simh_shim_host_t *host)
{
shim_host = *host;
shim_host_bound = TRUE;
if (shim_host.message_file == NULL)
    shim_host.message_file = stdout;
}

static FILE *shim_messages (void)
{
return shim_host_bound ? shim_host.message_file : stdout;
}

int simh_shim_console_get (void)
{
return shim_host_bound ? shim_host.console_get (shim_host.context) : -1;
}

void simh_shim_console_put (int c)
{
if (shim_host_bound)
    shim_host.console_put (shim_host.context, c);
}

double simh_shim_elapsed_usec (void)
{
return shim_host_bound ? shim_host.elapsed_usec (shim_host.context) : 0.0;
}

/* ------------------------------------------------------------------------ */
/* State scp owns                                                            */
/* ------------------------------------------------------------------------ */

int32 sim_interval = 0;                                 /* to the next event */
UNIT *sim_clock_queue = QUEUE_LIST_END;
int32 sim_switches = 0;                                 /* command switches */
int32 sim_switch_number = 0;
int32 sim_int_char = 005;                               /* ^E, as scp defaults */
FILE *sim_deb = NULL;                                   /* debug output */
FILE *stdnul = NULL;                                    /* the bit bucket */
t_value *sim_eval = NULL;                               /* examine buffer */
t_bool sim_idle_enab = FALSE;                           /* no host sleeping */
uint32 sim_brk_dflt = 0;
uint32 sim_brk_summ = 0;
uint32 sim_brk_types = 0;
const char *sim_savename = NULL;
CTAB *sim_vm_cmd = NULL;
const char **sim_clock_precalibrate_commands = NULL;
int32 sim_vm_initial_ips = 500000;
t_bool (*sim_vm_is_subroutine_call) (t_addr **ret_addrs) = NULL;

/* Simulated time, counted in the same units sim_interval counts down. Both
   start at zero so the first update charges nothing: the queue is empty until a
   device schedules something, and an empty queue would otherwise account a full
   NOQUEUE_WAIT before the first instruction ran. */
static double sim_time = 0.0;
static int32 noqueue_time = 0;

/* scp sizes sim_eval from the widest examine any device declares. The shim
   sizes it once, large enough for the longest VAX instruction, which is what
   the only reader - the disassembly of the instruction at PC - needs. */
#define SHIM_EVAL_SIZE  64

/* The element size of a buffered unit's file buffer, keyed by the device's
   declared data width, as scp's SZ_D() computes it. */
static const size_t shim_size_map[] = {
    sizeof (int8),
    sizeof (int8), sizeof (int16), sizeof (int32), sizeof (int32),
    sizeof (t_int64), sizeof (t_int64), sizeof (t_int64), sizeof (t_int64)
    };
#define SHIM_SZ_D(dp) (shim_size_map[((dp)->dwidth + CHAR_BIT - 1) / CHAR_BIT])

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------ */

int Fprintf (FILE *f, const char *fmt, ...)
{
int result;
va_list args;

va_start (args, fmt);
result = vfprintf (f ? f : shim_messages (), fmt, args);
va_end (args);
return result;
}

void sim_printf (const char *fmt, ...)
{
va_list args;

va_start (args, fmt);
vfprintf (shim_messages (), fmt, args);
va_end (args);
fflush (shim_messages ());
}

t_stat sim_messagef (t_stat stat, const char *fmt, ...)
{
va_list args;

va_start (args, fmt);
vfprintf (shim_messages (), fmt, args);
va_end (args);
fflush (shim_messages ());
return stat;
}

void _sim_debug_device (uint32 dbits, DEVICE *dptr, const char *fmt, ...)
{
va_list args;

if (sim_deb == NULL)
    return;
(void) dbits;
fprintf (sim_deb, "%s ", sim_dname (dptr));
va_start (args, fmt);
vfprintf (sim_deb, fmt, args);
va_end (args);
}

void _sim_debug_unit (uint32 dbits, UNIT *uptr, const char *fmt, ...)
{
va_list args;

if (sim_deb == NULL)
    return;
(void) dbits;
fprintf (sim_deb, "%s ", sim_uname (uptr));
va_start (args, fmt);
vfprintf (sim_deb, fmt, args);
va_end (args);
}

/* The bit-field tracing of scp, reduced to the value and the field names that
   are set. A trace reader keeps the register name and the numbers; the column
   layout scp produces is not reproduced. */
static void shim_debug_bits (FILE *f, BITFIELD *bitdefs, uint32 before, uint32 after)
{
int i;

fprintf (f, "%08x", after);
if (bitdefs == NULL)
    return;
fprintf (f, " {");
for (i = 0; bitdefs[i].name != NULL; i++) {
    uint32 mask = ((bitdefs[i].width >= 32) ? 0xFFFFFFFFu : ((1u << bitdefs[i].width) - 1));
    uint32 val = (after >> bitdefs[i].offset) & mask;

    if (val != 0)
        fprintf (f, " %s=%x", bitdefs[i].name, val);
    }
fprintf (f, " }");
if (before != after)
    fprintf (f, " was %08x", before);
}

void sim_debug_bits_hdr (uint32 dbits, DEVICE *dptr, const char *header,
                         BITFIELD *bitdefs, uint32 before, uint32 after, int terminate)
{
if ((sim_deb == NULL) || (dptr == NULL) || !(dptr->dctrl & dbits))
    return;
fprintf (sim_deb, "%s %s: ", sim_dname (dptr), header ? header : "");
shim_debug_bits (sim_deb, bitdefs, before, after);
if (terminate)
    fprintf (sim_deb, "\n");
}

void sim_debug_bits (uint32 dbits, DEVICE *dptr, BITFIELD *bitdefs,
                     uint32 before, uint32 after, int terminate)
{
sim_debug_bits_hdr (dbits, dptr, "", bitdefs, before, after, terminate);
}

/* ------------------------------------------------------------------------ */
/* Status codes                                                              */
/* ------------------------------------------------------------------------ */

/* In the order of the SCPE_* codes of sim_defs.h, starting at SCPE_BASE. */
static const char *shim_scp_messages[] = {
    "Non-existent memory",              "Unit not attached",
    "I/O error",                        "Checksum error",
    "Format error",                     "Unit not attachable",
    "File open error",                  "Memory exhausted",
    "Invalid argument",                 "Step expired",
    "Unknown command",                  "Read only argument",
    "Command not completed",            "Simulation stopped",
    "Goodbye",                          "Console input I/O error",
    "Console output I/O error",         "End of file",
    "Relocation error",                 "No settable parameters",
    "Unit already attached",            "Hardware timer error",
    "SIGINT handler setup error",       "Console terminal setup error",
    "Subscript out of range",           "Command not implemented",
    "Unit disabled",                    "Read only operation not allowed",
    "Invalid switch",                   "Missing value",
    "Too few arguments",                "Too many arguments",
    "Non-existent device",              "Non-existent unit",
    "Non-existent register",            "Non-existent parameter",
    "Nested DO command limit exceeded", "Internal error",
    "Invalid magtape record length",    "Console Telnet connection lost",
    "Console Telnet connection timed out", "Console Telnet output stall",
    "Assertion failed",                 "Invalid remote console command",
    "Expect matched",                   "Ambiguous register name",
    "Remote console command",           "Invalid expression",
    "SIGTERM received",                 "File system size larger than disk size",
    "Run time limit exhausted",         "Incompatible disk container"
    };

const char *sim_error_text (t_stat stat)
{
static char shim_status_buf[64];

stat = SCPE_BARE_STATUS (stat);
if (stat == SCPE_OK)
    return "No error";
if ((stat >= SCPE_BASE) &&
    ((size_t) (stat - SCPE_BASE) < sizeof (shim_scp_messages) / sizeof (shim_scp_messages[0])))
    return shim_scp_messages[stat - SCPE_BASE];
snprintf (shim_status_buf, sizeof shim_status_buf, "Simulator status %d", (int) stat);
return shim_status_buf;
}

const char *simh_shim_status_text (t_stat status)
{
return sim_error_text (status);
}

/* ------------------------------------------------------------------------ */
/* Parsing, as the SET and SHOW routines of the core expect it               */
/* ------------------------------------------------------------------------ */

static CONST char *shim_get_glyph (const char *iptr, char *optr, char mchar, t_bool uc)
{
size_t i = 0;

while (isspace ((unsigned char) *iptr))
    iptr++;
while ((*iptr != 0) && (!isspace ((unsigned char) *iptr)) &&
       ((mchar == 0) || (*iptr != mchar))) {
    if (i < CBUFSIZE - 1)
        optr[i++] = (char) (uc ? toupper ((unsigned char) *iptr) : *iptr);
    iptr++;
    }
optr[i] = 0;
if ((mchar != 0) && (*iptr == mchar))
    iptr++;
while (isspace ((unsigned char) *iptr))
    iptr++;
return (CONST char *) iptr;
}

CONST char *get_glyph (const char *iptr, char *optr, char mchar)
{
return shim_get_glyph (iptr, optr, mchar, TRUE);
}

CONST char *get_glyph_nc (const char *iptr, char *optr, char mchar)
{
return shim_get_glyph (iptr, optr, mchar, FALSE);
}

t_value strtotv (CONST char *inptr, CONST char **endptr, uint32 radix)
{
t_value val = 0;
int digit;

*endptr = inptr;
if ((radix < 2) || (radix > 36))
    return 0;
while (isspace ((unsigned char) *inptr))
    inptr++;
for (; *inptr != 0; inptr++) {
    if (isdigit ((unsigned char) *inptr))
        digit = *inptr - '0';
    else if (isalpha ((unsigned char) *inptr))
        digit = toupper ((unsigned char) *inptr) - 'A' + 10;
    else
        break;
    if ((uint32) digit >= radix)
        break;
    val = (val * radix) + digit;
    *endptr = inptr + 1;
    }
return val;
}

t_value get_uint (const char *cptr, uint32 radix, t_value max, t_stat *status)
{
t_value val;
CONST char *tptr;

*status = SCPE_OK;
val = strtotv ((CONST char *) cptr, &tptr, radix);
if ((cptr == tptr) || (val > max))
    *status = SCPE_ARG;
else {
    while (isspace ((unsigned char) *tptr))
        tptr++;
    if (*tptr != 0)
        *status = SCPE_ARG;
    }
return val;
}

CONST char *get_sim_sw (CONST char *cptr)
{
/* The embedded build has no command line, so a core asking for the switches of
   the command that reached it always sees none set. */
sim_switches = 0;
sim_switch_number = 0;
return cptr;
}

t_bool get_yn (const char *ques, t_bool deflt)
{
/* Nothing in the embedding answers a question. The default stands, and the
   question is reported so a run that hit one can be recognised. */
sim_printf ("%s %s\n", ques, deflt ? "Y" : "N");
return deflt;
}

t_stat sim_decode_quoted_string (const char *iptr, uint8 *optr, uint32 *osize)
{
uint32 n = 0;

if ((*iptr != '"') && (*iptr != '\''))
    return SCPE_ARG;
for (iptr++; (*iptr != 0) && (*iptr != '"') && (*iptr != '\''); iptr++)
    optr[n++] = (uint8) *iptr;
*osize = n;
return SCPE_OK;
}

/* ------------------------------------------------------------------------ */
/* Finding devices, units and registers                                      */
/* ------------------------------------------------------------------------ */

const char *sim_dname (DEVICE *dptr)
{
return dptr ? (dptr->lname ? dptr->lname : dptr->name) : "";
}

const char *sim_uname (UNIT *uptr)
{
static char shim_uname_buf[CBUFSIZE];
DEVICE *dptr;
uint32 i;

if (uptr == NULL)
    return "";
if (uptr->uname)
    return uptr->uname;
if ((dptr = find_dev_from_unit (uptr)) == NULL)
    return "";
if (dptr->numunits == 1)
    return sim_dname (dptr);
for (i = 0; i < dptr->numunits; i++)
    if (uptr == dptr->units + i)
        break;
snprintf (shim_uname_buf, sizeof shim_uname_buf, "%s%u", sim_dname (dptr), (unsigned) i);
return shim_uname_buf;
}

DEVICE *find_dev (CONST char *cptr)
{
int i;

for (i = 0; sim_devices[i] != NULL; i++) {
    if ((strcmp (cptr, sim_devices[i]->name) == 0) ||
        (sim_devices[i]->lname && (strcmp (cptr, sim_devices[i]->lname) == 0)))
        return sim_devices[i];
    }
return NULL;
}

DEVICE *find_dev_from_unit (UNIT *uptr)
{
int i;
uint32 j;

if (uptr == NULL)
    return NULL;
if (uptr->dptr)
    return uptr->dptr;
for (i = 0; sim_devices[i] != NULL; i++)
    for (j = 0; j < sim_devices[i]->numunits; j++)
        if (uptr == (sim_devices[i]->units + j))
            return sim_devices[i];
return NULL;
}

DEVICE *find_unit (const char *cptr, UNIT **uptr)
{
char gbuf[CBUFSIZE];
DEVICE *dptr;
size_t namelen;
uint32 unit = 0;
int i;

*uptr = NULL;
shim_get_glyph (cptr, gbuf, 0, TRUE);
if ((dptr = find_dev (gbuf)) != NULL) {                 /* whole name matches? */
    *uptr = dptr->units;
    return dptr;
    }
for (i = 0; sim_devices[i] != NULL; i++) {              /* else name plus number */
    dptr = sim_devices[i];
    namelen = strlen (dptr->name);
    if (strncmp (gbuf, dptr->name, namelen) != 0)
        continue;
    if (gbuf[namelen] == 0)
        continue;
    unit = (uint32) strtoul (gbuf + namelen, NULL, 10);
    if (unit >= dptr->numunits)
        return NULL;
    *uptr = dptr->units + unit;
    return dptr;
    }
return NULL;
}

REG *find_reg (CONST char *cptr, CONST char **optr, DEVICE *dptr)
{
char gbuf[CBUFSIZE];
CONST char *tptr;
REG *rptr;

if ((dptr == NULL) || (dptr->registers == NULL))
    return NULL;
tptr = shim_get_glyph (cptr, gbuf, 0, TRUE);
for (rptr = dptr->registers; rptr->name != NULL; rptr++)
    if (strcmp (gbuf, rptr->name) == 0) {
        if (optr != NULL)
            *optr = tptr;
        return rptr;
        }
return NULL;
}

/* ------------------------------------------------------------------------ */
/* Reset                                                                     */
/* ------------------------------------------------------------------------ */

t_stat reset_all (uint32 start)
{
DEVICE *dptr;
uint32 i;
t_stat reason;

for (i = 0; i < start; i++)
    if (sim_devices[i] == NULL)
        return SCPE_IERR;
for (i = start; (dptr = sim_devices[i]) != NULL; i++) {
    if (dptr->reset != NULL) {
        reason = dptr->reset (dptr);
        if (reason != SCPE_OK)
            return reason;
        }
    }
return SCPE_OK;
}

t_stat reset_all_p (uint32 start)
{
t_stat reason;
int32 saved_switches = sim_switches;

sim_switches = SWMASK ('P');
reason = reset_all (start);
sim_switches = saved_switches;
return reason;
}

/* ------------------------------------------------------------------------ */
/* Attach and detach                                                         */
/* ------------------------------------------------------------------------ */

static t_stat shim_attach_err (UNIT *uptr, t_stat stat)
{
free (uptr->filename);
uptr->filename = NULL;
return stat;
}

t_stat attach_unit (UNIT *uptr, CONST char *cptr)
{
DEVICE *dptr;

if (!(uptr->flags & UNIT_ATTABLE))
    return SCPE_NOATT;
if ((dptr = find_dev_from_unit (uptr)) == NULL)
    return SCPE_NOATT;
uptr->filename = (char *) calloc (CBUFSIZE, sizeof (char));
if (uptr->filename == NULL)
    return SCPE_MEM;
strncpy (uptr->filename, cptr, CBUFSIZE - 1);

if (uptr->flags & UNIT_RO) {                            /* write locked? */
    if ((uptr->flags & UNIT_ROABLE) == 0)
        return shim_attach_err (uptr, SCPE_NORO);
    uptr->fileref = sim_fopen (cptr, "rb");
    }
else {
    uptr->fileref = sim_fopen (cptr, "rb+");            /* read and write */
    if ((uptr->fileref == NULL) && (uptr->flags & UNIT_ROABLE)) {
        uptr->fileref = sim_fopen (cptr, "rb");         /* settle for reading */
        if (uptr->fileref != NULL) {
            uptr->flags |= UNIT_RO;
            sim_printf ("%s: unit is read only\n", sim_uname (uptr));
            }
        }
    }
if (uptr->fileref == NULL)
    return sim_messagef (shim_attach_err (uptr, SCPE_OPENERR), "%s: cannot open '%s': %s\n",
                         sim_uname (uptr), cptr, strerror (errno));

if (uptr->flags & UNIT_BUFABLE) {                       /* read the whole file? */
    uint32 cap = ((uint32) uptr->capac) / dptr->aincr;

    if (uptr->flags & UNIT_MUSTBUF) {
        uptr->filebuf = calloc (cap, SHIM_SZ_D (dptr));
        if (uptr->filebuf == NULL)
            return shim_attach_err (uptr, SCPE_MEM);
        }
    uptr->hwmark = (uint32) sim_fread (uptr->filebuf, SHIM_SZ_D (dptr), cap, uptr->fileref);
    uptr->flags |= UNIT_BUF;
    }

uptr->flags |= UNIT_ATT;
uptr->pos = 0;
return SCPE_OK;
}

t_stat detach_unit (UNIT *uptr)
{
DEVICE *dptr;

if (uptr == NULL)
    return SCPE_IERR;
if (!(uptr->flags & UNIT_ATTABLE))
    return SCPE_NOATT;
if (!(uptr->flags & UNIT_ATT))
    return SCPE_OK;
if ((dptr = find_dev_from_unit (uptr)) == NULL)
    return SCPE_OK;

if ((uptr->flags & UNIT_BUF) && (uptr->filebuf != NULL)) {
    if (uptr->hwmark && ((uptr->flags & UNIT_RO) == 0)) {   /* write it back */
        sim_printf ("%s: writing buffer to file\n", sim_uname (uptr));
        rewind (uptr->fileref);
        sim_fwrite (uptr->filebuf, SHIM_SZ_D (dptr), uptr->hwmark, uptr->fileref);
        }
    if (uptr->flags & UNIT_MUSTBUF) {
        free (uptr->filebuf);
        uptr->filebuf = NULL;
        }
    uptr->flags &= ~UNIT_BUF;
    }

fclose (uptr->fileref);
uptr->fileref = NULL;
free (uptr->filename);
uptr->filename = NULL;
uptr->flags &= ~UNIT_ATT;
return SCPE_OK;
}

/* ------------------------------------------------------------------------ */
/* The event queue                                                           */
/*                                                                           */
/* simh's queue, with the entries holding time relative to the entry before   */
/* them and sim_interval counting down to the head. Two features of scp's     */
/* version are left out: asynchronous I/O, which the core is built without,   */
/* and the backdating scp does when an event fires late, which only affects   */
/* the reported time.                                                        */
/* ------------------------------------------------------------------------ */

static void shim_update_time (void)
{
if (sim_clock_queue == QUEUE_LIST_END)
    sim_time += (double) (noqueue_time - sim_interval);
else
    sim_time += (double) (sim_clock_queue->time - sim_interval);
if (sim_clock_queue == QUEUE_LIST_END)
    noqueue_time = sim_interval;
else
    sim_clock_queue->time = sim_interval;
}

double sim_gtime (void)
{
return sim_time;
}

/* Batching. sim_instr() runs until something aborts it, so a caller that wants
   a bounded run gets its bound here: sim_interval is held down to what is left
   of the batch wherever the queue sets it, and the instruction loop calls
   sim_process_event() when it expires. */
static double shim_batch_end = 0.0;
static t_bool shim_batch_limited = FALSE;

static void shim_cap_interval (void)
{
double remaining;

if (!shim_batch_limited)
    return;
remaining = shim_batch_end - sim_time;
if (remaining < 1.0)
    remaining = 1.0;
if ((double) sim_interval > remaining)
    sim_interval = (int32) remaining;
}

t_stat sim_activate (UNIT *uptr, int32 event_time)
{
UNIT *cptr, *prvptr;
int32 accum;

if (event_time < 0)
    return SCPE_IERR;
if (sim_is_active (uptr))                               /* already scheduled */
    return SCPE_OK;
shim_update_time ();

prvptr = NULL;
accum = 0;
for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
    if (event_time < (accum + cptr->time))
        break;
    accum += cptr->time;
    prvptr = cptr;
    }
if (prvptr == NULL) {                                   /* new head */
    uptr->next = sim_clock_queue;
    sim_clock_queue = uptr;
    }
else {
    uptr->next = prvptr->next;
    prvptr->next = uptr;
    }
uptr->time = event_time - accum;
if (uptr->next != QUEUE_LIST_END)
    uptr->next->time -= uptr->time;
sim_interval = sim_clock_queue->time;
shim_cap_interval ();
return SCPE_OK;
}

t_stat sim_activate_abs (UNIT *uptr, int32 event_time)
{
sim_cancel (uptr);
return sim_activate (uptr, event_time);
}

/* An interval given in microseconds of wall time. The core uses this for the
   line clock and for device latencies. It is converted with the instruction
   rate the CPU model publishes, which is what scp does when the host has no
   calibrated clock to hang the request on. */
static int32 shim_usecs_to_interval (double usecs)
{
double ticks = (usecs * (double) sim_vm_initial_ips) / 1000000.0;

if (ticks < 1.0)
    return 1;
if (ticks > (double) 0x7FFFFFFF)
    return 0x7FFFFFFF;
return (int32) ticks;
}

t_stat sim_activate_after_d (UNIT *uptr, double usecs)
{
return sim_activate (uptr, shim_usecs_to_interval (usecs));
}

t_stat sim_activate_after (UNIT *uptr, uint32 usecs)
{
return sim_activate_after_d (uptr, (double) usecs);
}

t_stat sim_activate_after_abs (UNIT *uptr, uint32 usecs)
{
sim_cancel (uptr);
return sim_activate_after (uptr, usecs);
}

t_stat sim_activate_after_abs_d (UNIT *uptr, double usecs)
{
sim_cancel (uptr);
return sim_activate_after_d (uptr, usecs);
}

t_stat sim_cancel (UNIT *uptr)
{
UNIT *cptr, *nptr;

if (sim_clock_queue == QUEUE_LIST_END)
    return SCPE_OK;
if (!sim_is_active (uptr))
    return SCPE_OK;
shim_update_time ();
nptr = QUEUE_LIST_END;
if (sim_clock_queue == uptr)
    nptr = sim_clock_queue = uptr->next;
else {
    for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
        if (cptr->next == uptr) {
            nptr = cptr->next = uptr->next;
            break;
            }
        }
    }
if (nptr != QUEUE_LIST_END)
    nptr->time += uptr->time;
uptr->next = NULL;
uptr->time = 0;
if (sim_clock_queue != QUEUE_LIST_END)
    sim_interval = sim_clock_queue->time;
else
    sim_interval = noqueue_time = NOQUEUE_WAIT;
return SCPE_OK;
}

t_bool sim_is_active (UNIT *uptr)
{
return (uptr->next != NULL);
}

int32 sim_activate_time (UNIT *uptr)
{
UNIT *cptr;
int32 accum = 0;

for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
    if (cptr == sim_clock_queue)
        accum += sim_interval;
    else
        accum += cptr->time;
    if (cptr == uptr)
        return accum + 1;
    }
return 0;
}

double sim_activate_time_usecs (UNIT *uptr)
{
int32 ticks = sim_activate_time (uptr);

if (ticks == 0)
    return 0.0;
return ((double) (ticks - 1) * 1000000.0) / (double) sim_vm_initial_ips;
}

t_stat sim_process_event (void)
{
UNIT *uptr;
t_stat reason;

shim_update_time ();
if (shim_batch_limited && (sim_time >= shim_batch_end))
    return SCPE_STOP;                                   /* batch spent */
if (sim_interval > 0)
    return SCPE_OK;
if (sim_clock_queue == QUEUE_LIST_END) {                /* nothing scheduled */
    sim_interval = noqueue_time = NOQUEUE_WAIT;
    return SCPE_OK;
    }
if (sim_interval < 0)                                   /* fired late: catch up */
    sim_interval = 0;
do {
    uptr = sim_clock_queue;
    sim_clock_queue = uptr->next;
    uptr->next = NULL;
    uptr->time = 0;
    if (sim_clock_queue != QUEUE_LIST_END)
        sim_interval += sim_clock_queue->time;
    else
        sim_interval = noqueue_time = NOQUEUE_WAIT;
    reason = (uptr->action != NULL) ? uptr->action (uptr) : SCPE_OK;
    if (reason != SCPE_OK)
        return reason;
    } while ((sim_interval <= 0) && (sim_clock_queue != QUEUE_LIST_END));
if (sim_interval <= 0)
    sim_interval = noqueue_time = NOQUEUE_WAIT;
shim_cap_interval ();
return SCPE_OK;
}

/* ------------------------------------------------------------------------ */
/* Breakpoints                                                               */
/*                                                                           */
/* The embedded build has none. sim_brk_summ stays zero, so the core's        */
/* breakpoint tests fold away, and the SET BREAK entry points are stubs.      */
/* ------------------------------------------------------------------------ */

uint32 sim_brk_test (t_addr loc, uint32 btyp)
{
(void) loc;
(void) btyp;
return 0;
}

/* ------------------------------------------------------------------------ */
/* Loading a bootstrap                                                       */
/*                                                                           */
/* The 780 console loads VMB from an image the CPU model carries as an array. */
/* sim_set_memory_load_file() hands that array over, load_cmd() calls the     */
/* simulator's own sim_load(), and Fgetc() reads from the array instead of a  */
/* file. A named file is opened and read the same way.                       */
/* ------------------------------------------------------------------------ */

static const unsigned char *shim_load_data = NULL;
static size_t shim_load_size = 0;
static size_t shim_load_pos = 0;

t_stat sim_set_memory_load_file (const unsigned char *data, size_t size)
{
shim_load_data = data;
shim_load_size = size;
shim_load_pos = 0;
return SCPE_OK;
}

int Fgetc (FILE *f)
{
if (shim_load_data != NULL) {
    if (shim_load_pos >= shim_load_size)
        return EOF;
    return shim_load_data[shim_load_pos++];
    }
return fgetc (f);
}

t_stat load_cmd (int32 flag, CONST char *cptr)
{
char gbuf[CBUFSIZE];
FILE *loadfile = NULL;
t_stat reason;

if (*cptr == 0)
    return SCPE_2FARG;
cptr = get_glyph_nc (cptr, gbuf, 0);
if (shim_load_data == NULL) {
    loadfile = sim_fopen (gbuf, flag ? "wb" : "rb");
    if (loadfile == NULL)
        return SCPE_OPENERR;
    }
reason = sim_load (loadfile, cptr, gbuf, flag);
if (loadfile != NULL)
    fclose (loadfile);
return reason;
}

/* ------------------------------------------------------------------------ */
/* Entry points that only a typed command reaches                            */
/* ------------------------------------------------------------------------ */

static t_stat shim_no_command (const char *what)
{
return sim_messagef (SCPE_NOFNC, "%s is not available in the embedded build\n", what);
}

t_stat run_cmd (int32 flag, CONST char *cptr)
{
(void) flag;
(void) cptr;
return shim_no_command ("RUN");
}

void run_cmd_message (const char *cmdline, t_stat r)
{
(void) cmdline;
if (r != SCPE_OK)
    sim_printf ("%s\n", sim_error_text (r));
}

t_stat set_writelock (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
(void) cptr;
(void) desc;
if (val)
    uptr->flags |= UNIT_RO;
else
    uptr->flags &= ~UNIT_RO;
return SCPE_OK;
}

t_stat show_writelock (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
(void) val;
(void) desc;
fprintf (st, "%s", (uptr->flags & UNIT_RO) ? "write locked" : "write enabled");
return SCPE_OK;
}

void fprint_reg_help (FILE *st, DEVICE *dptr)
{
fprintf (st, "%s: no register help in the embedded build\n", sim_dname (dptr));
}

void fprint_set_help (FILE *st, DEVICE *dptr)
{
fprintf (st, "%s: no SET help in the embedded build\n", sim_dname (dptr));
}

void fprint_show_help (FILE *st, DEVICE *dptr)
{
fprintf (st, "%s: no SHOW help in the embedded build\n", sim_dname (dptr));
}

t_stat fprint_val (FILE *stream, t_value val, uint32 radix, uint32 width, uint32 format)
{
(void) width;
(void) format;
switch (radix) {
    case 8:
        fprintf (stream, "%llo", (unsigned long long) val);
        break;
    case 10:
        fprintf (stream, "%llu", (unsigned long long) val);
        break;
    default:
        fprintf (stream, "%llx", (unsigned long long) val);
        break;
    }
return SCPE_OK;
}

/* Read one datum at addr into sim_eval[0], which is what the core's
   disassembly of the instruction at PC wants. */
t_stat get_aval (t_addr addr, DEVICE *dptr, UNIT *uptr)
{
t_stat reason;

if ((dptr == NULL) || (uptr == NULL) || (dptr->examine == NULL))
    return SCPE_NOFNC;
if (sim_eval == NULL) {
    sim_eval = (t_value *) calloc (SHIM_EVAL_SIZE, sizeof (*sim_eval));
    if (sim_eval == NULL)
        return SCPE_MEM;
    }
reason = dptr->examine (&sim_eval[0], addr, uptr, SIM_SW_STOP);
return reason;
}

/* ------------------------------------------------------------------------ */
/* Entry points of simh layers the shim build does not carry                 */
/* ------------------------------------------------------------------------ */

/* The UNIBUS map of pdp11_io_lib.c offers this as the ATTACH-time writer of a
   DEC standard bad block table, out of simh's disk layer. No controller in the
   shim build attaches a disk - on the board the disks are emulated devices on
   the real bus - so reaching it means a device arrived without its support. */

t_stat sim_disk_pdp11_bad_block (UNIT *uptr, int32 sec, int32 wds)
{
(void) sec;
(void) wds;
return sim_messagef (SCPE_NOFNC, "%s: writing a bad block table needs simh's disk layer\n",
                     sim_uname (uptr));
}

/* ------------------------------------------------------------------------ */
/* The embedding API                                                         */
/* ------------------------------------------------------------------------ */

t_stat simh_shim_reset (void)
{
if (stdnul == NULL)
    stdnul = fopen ("/dev/null", "w");
if (sim_eval == NULL)
    sim_eval = (t_value *) calloc (SHIM_EVAL_SIZE, sizeof (*sim_eval));
return reset_all (0);
}

t_stat simh_shim_run (int32 max_instructions)
{
t_stat reason;

shim_batch_limited = (max_instructions > 0);
shim_batch_end = sim_time + (double) max_instructions;
shim_cap_interval ();
reason = sim_instr ();
shim_batch_limited = FALSE;
return reason;
}

t_stat simh_shim_attach (const char *unit_name, const char *filename)
{
DEVICE *dptr;
UNIT *uptr;

if ((dptr = find_unit (unit_name, &uptr)) == NULL)
    return SCPE_NXDEV;
if (uptr == NULL)
    return SCPE_NXUN;
if (dptr->attach != NULL)
    return dptr->attach (uptr, filename);
return attach_unit (uptr, filename);
}

t_stat simh_shim_boot (const char *unit_name)
{
DEVICE *dptr;
UNIT *uptr;
uint32 unit;

if ((dptr = find_unit (unit_name, &uptr)) == NULL)
    return SCPE_NXDEV;
if ((uptr == NULL) || (dptr->boot == NULL))
    return SCPE_NOFNC;
unit = (uint32) (uptr - dptr->units);
return dptr->boot ((int32) unit, dptr);
}
