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

#include "sim_defs.h"
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

int simh_shim_bus_read (unsigned addr, unsigned *data)
{
if (!shim_host_bound || (shim_host.bus_read == NULL))
    return 0;
return shim_host.bus_read (shim_host.context, addr, data);
}

int simh_shim_bus_write (unsigned addr, unsigned data, int byte)
{
if (!shim_host_bound || (shim_host.bus_write == NULL))
    return 0;
return shim_host.bus_write (shim_host.context, addr, data, byte);
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
int32 sim_quiet = 0;                                    /* suppress messages */
int32 sim_show_message = 1;                             /* show command results */
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

/* A block of transferred data, which scp prints as a hex and character dump.
   Here it is the length and where it went; a reader after the bytes themselves
   wants the device's own trace. */
void sim_data_trace (DEVICE *dptr, UNIT *uptr, const uint8 *data, const char *position,
                     size_t len, const char *txt, uint32 reason)
{
(void) data;
if ((sim_deb == NULL) || (dptr == NULL) || !(dptr->dctrl & reason))
    return;
fprintf (sim_deb, "%s %s %s %u bytes at %s\n", sim_dname (dptr), sim_uname (uptr),
         txt ? txt : "", (unsigned) len, position ? position : "");
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

const char *simh_shim_status_text (simh_shim_status_t status)
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

/* Leading switches, as scp reads them. The core builds its own command strings
   and the switches in them carry meaning: the boot code is placed with "-O
   <file> <offset>", and losing that origin leaves a bootstrap at address zero
   and a processor started somewhere else. */
static int32 shim_switch_mask (const char *letters)
{
int32 mask = 0;

if (*letters == 0)
    return -1;
for (; *letters != 0; letters++) {
    if (isdigit ((unsigned char) *letters)) {           /* a radix, not a switch */
        sim_switch_number = (int32) strtol (letters, NULL, 10);
        break;
        }
    if (!isalpha ((unsigned char) *letters))
        return -1;
    mask |= SWMASK (toupper ((unsigned char) *letters));
    }
return mask;
}

CONST char *get_sim_sw (CONST char *cptr)
{
char gbuf[CBUFSIZE];
int32 mask;

while ((cptr != NULL) && (*cptr == '-')) {
    cptr = get_glyph (cptr, gbuf, 0);
    if ((mask = shim_switch_mask (gbuf + 1)) < 0)
        return NULL;
    sim_switches |= mask;
    }
return cptr;
}

t_bool get_yn (const char *ques, t_bool deflt)
{
/* Nothing in the embedding answers a question. The default stands, and the
   question is reported so a run that hit one can be recognised. */
sim_printf ("%s %s\n", ques, deflt ? "Y" : "N");
return deflt;
}

/* The tail of a file name when it ends in the given extension, and NULL when it
   does not. simh's disk layer asks this of a container's name. */
CONST char *match_ext (CONST char *fnam, const char *ext)
{
const char *dot = strrchr (fnam, '.');
size_t i;

if ((dot == NULL) || (ext == NULL))
    return NULL;
for (i = 0; ext[i] != 0; i++)
    if (toupper ((unsigned char) dot[1 + i]) != toupper ((unsigned char) ext[i]))
        return NULL;
return (dot[1 + i] == 0) ? (CONST char *) (dot + 1) : NULL;
}

/* A number with thousands separators, for a message. */
const char *sim_fmt_numeric (double number)
{
static char shim_numeric_buf[64];

snprintf (shim_numeric_buf, sizeof shim_numeric_buf, "%.0f", number);
return shim_numeric_buf;
}

/* A unit's capacity in the units its device counts in, as scp prints it. */
const char *sprint_capac (DEVICE *dptr, UNIT *uptr)
{
static char shim_capac_buf[64];
double bytes = (double) uptr->capac;
const char *width = ((dptr->dwidth / dptr->aincr) > 8) ? "W" : "B";

if (dptr->flags & DEV_SECTORS)
    bytes *= 512.0;
if (bytes >= 1024.0 * 1024.0)
    snprintf (shim_capac_buf, sizeof shim_capac_buf, "%.0fM%s", bytes / (1024.0 * 1024.0), width);
else if (bytes >= 1024.0)
    snprintf (shim_capac_buf, sizeof shim_capac_buf, "%.0fK%s", bytes / 1024.0, width);
else
    snprintf (shim_capac_buf, sizeof shim_capac_buf, "%.0f%s", bytes, width);
return shim_capac_buf;
}

/* A name a device gives one of its units, which the shim keeps as the unit's
   own so sim_uname() reports it. */
const char *sim_set_uname (UNIT *uptr, const char *uname)
{
free (uptr->uname);
uptr->uname = uname ? strdup (uname) : NULL;
return uptr->uname;
}

/* Randomness, for the device models that vary a latency or seed a pack id.
   scp.h redirects every caller's rand() and srand() here, so the redirection
   has to come off before the host's own can be reached. */
#undef rand
#undef srand

int sim_rand (void)
{
return rand ();
}

void sim_srand (unsigned int seed)
{
srand (seed);
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
        if (uptr == (sim_devices[i]->units + j)) {
            uptr->dptr = sim_devices[i];        /* remember, as scp does */
            return sim_devices[i];
            }
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
/* Setting a parameter                                                       */
/*                                                                           */
/* SET is the one command of scp's that an embedded core genuinely needs. A   */
/* device's modifier table is where its drive types, its controller types and */
/* its write locks live, and both the embedding and simh's own disk layer     */
/* reach them only this way: sim_disk_attach() names the drive type a         */
/* container was made for and sets it, which is how attaching an image picks  */
/* the right geometry.                                                       */
/* ------------------------------------------------------------------------ */

/* One "NAME" or "NAME=VALUE" against a device's modifier table. */
static t_stat shim_set_one (DEVICE *dptr, UNIT *uptr, t_bool unit_named, char *glyph)
{
MTAB *mptr;
char *value = strchr (glyph, '=');

if (value != NULL)
    *value++ = 0;
if (dptr->modifiers == NULL)
    return SCPE_NOPARAM;
/* A modifier's match string carries its syntax after the name, as in
   "FORMAT={AUTO|SIMH|VHD|RAW}", so what is compared is the name the caller
   gave against the head of the modifier's string, which is what scp's
   MATCH_CMD does. */
for (mptr = dptr->modifiers; (mptr->mask != 0) || (mptr->pstring != NULL); mptr++) {
    if ((mptr->mstring == NULL) || (MATCH_CMD (glyph, mptr->mstring) != 0))
        continue;
    if (mptr->mask & MTAB_XTD) {
        if (MODMASK (mptr, MTAB_VUN) && !unit_named && (dptr->numunits > 1))
            return sim_messagef (SCPE_ARG, "%s: %s needs a unit\n", sim_dname (dptr), glyph);
        if (mptr->valid != NULL)
            return mptr->valid (uptr, (int32) mptr->match, (CONST char *) value, mptr->desc);
        return SCPE_ARG;
        }
    /* A plain modifier names a value for a field of the unit's flags, and may
       carry a routine besides: the memory sizes of a CPU are written this way,
       and the routine is what resizes the memory array behind them. */
    if (mptr->valid != NULL) {
        t_stat r = mptr->valid (uptr, (int32) mptr->match, (CONST char *) value, mptr->desc);

        if (r != SCPE_OK)
            return r;
        }
    uptr->flags = (uptr->flags & ~mptr->mask) | (mptr->match & mptr->mask);
    return SCPE_OK;
    }
return SCPE_NXPAR;
}

t_stat set_cmd (int32 flag, CONST char *cptr)
{
char gbuf[CBUFSIZE];
DEVICE *dptr;
UNIT *uptr;
t_bool unit_named;
t_stat r;

(void) flag;
cptr = get_glyph (cptr, gbuf, 0);

/* The settings scp keeps for itself, which simh's disk layer turns off around
   an attach it does not want narrated. */
if (strcmp (gbuf, "NOMESSAGE") == 0) { sim_show_message = 0; return SCPE_OK; }
if (strcmp (gbuf, "MESSAGE") == 0)   { sim_show_message = 1; return SCPE_OK; }
if (strcmp (gbuf, "QUIET") == 0)     { sim_quiet = 1; return SCPE_OK; }
if (strcmp (gbuf, "NOQUIET") == 0)   { sim_quiet = 0; return SCPE_OK; }

if ((dptr = find_unit (gbuf, &uptr)) == NULL)
    return sim_messagef (SCPE_NXDEV, "no such device: %s\n", gbuf);
if (uptr == NULL)
    return SCPE_NXUN;
unit_named = (find_dev (gbuf) == NULL);                 /* a name with a number */

/* ENABLED and DISABLED are scp's own and belong to no modifier table. A
   disabled device keeps its place in the device list and its address in its
   descriptor - which is what a boot command reads to find out where it would
   have answered - but takes no part in building the I/O page, so the address
   is free for whatever else claims it. */
if (MATCH_CMD (cptr, "DISABLED") == 0) {
    dptr->flags |= DEV_DIS;
    return SCPE_OK;
    }
if (MATCH_CMD (cptr, "ENABLED") == 0) {
    dptr->flags &= ~DEV_DIS;
    return SCPE_OK;
    }

while (*cptr != 0) {
    cptr = get_glyph (cptr, gbuf, ',');
    if ((r = shim_set_one (dptr, uptr, unit_named, gbuf)) != SCPE_OK)
        return sim_messagef (r, "%s: cannot set %s\n", sim_dname (dptr), gbuf);
    }
return SCPE_OK;
}

/* SHOW writes to a terminal scp owns, and the embedding has its own way of
   reporting what a device is doing. */
t_stat show_cmd (int32 flag, CONST char *cptr)
{
(void) flag;
return sim_messagef (SCPE_NOFNC, "SHOW %s is not available in the embedded build\n", cptr);
}

/* ------------------------------------------------------------------------ */
/* Reset                                                                     */
/* ------------------------------------------------------------------------ */

/* Reset every device, and then take the I/O page back.

   reset_all() rebuilds the dispatch tables from the devices simh carries, which
   drops whatever the bus had claimed, so every path that resets has to come
   through here. A boot resets too - that is where this was first missed, and a
   bootstrap then found nothing where its controller should have been. */
unsigned shim_iopage_claimed = 0;       /* words of the I/O page left to the bus */

t_stat shim_reset_devices (void)
{
t_stat r;

if ((r = reset_all (0)) != SCPE_OK)
    return r;
if (shim_host_bound && (shim_host.bus_read != NULL))
    shim_iopage_claimed = simh_shim_bus_install (shim_host.bus_owns_iopage);
else {
    simh_shim_bus_remove ();
    shim_iopage_claimed = 0;
    }
return SCPE_OK;
}

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

/* The same clock as a wrapping 32-bit count, which a device uses to stamp the
   start of a transfer and schedule its completion against. */
uint32 sim_grtime (void)
{
return (uint32) sim_time;
}

/* Batching. sim_instr() runs until something aborts it, so a caller that wants
   a bounded run gets its bound as an event: a unit of the shim's own, queued
   for the end of the batch, whose service routine stops the processor.

   The bound has to be an event and not a cap on sim_interval, because the
   queue's accounting rests on sim_interval being the time remaining to the
   entry at its head. Shortening it behind the queue's back makes the next
   update charge the difference as though those instructions had run, and the
   simulated clock then runs away from the work actually done. */
static t_stat shim_batch_svc (UNIT *uptr)
{
(void) uptr;
return SCPE_STOP;
}

/* Left empty here and given its service routine at reset: simh's UDATA() macro
   names only the first few members of a UNIT, which this project's warning
   level counts as an incomplete initialiser. */
static UNIT shim_batch_unit;

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
return SCPE_OK;
}

t_stat sim_activate_abs (UNIT *uptr, int32 event_time)
{
sim_cancel (uptr);
return sim_activate (uptr, event_time);
}

/* An interval given in microseconds of wall time. The core uses this for the
   line clock and for device latencies, and it is converted with the rate the
   core is measured to be running at, so a device's delay lasts as long
   relative to the work around it as the device's own manual says. */
static int32 shim_usecs_to_interval (double usecs)
{
double ticks = (usecs * simh_shim_ips ()) / 1000000.0;

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

/* Schedule for an absolute point on sim_grtime()'s clock. A point already past
   fires at the next opportunity. */
t_stat sim_activate_notbefore (UNIT *uptr, int32 rtime)
{
uint32 when = (uint32) rtime;
uint32 now;

sim_cancel (uptr);
now = sim_grtime ();
if ((when - now) >= 0x80000000u)                        /* already past */
    return sim_activate (uptr, 0);
return sim_activate (uptr, (int32) (when - now));
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
return ((double) (ticks - 1) * 1000000.0) / simh_shim_ips ();
}

t_stat sim_process_event (void)
{
UNIT *uptr;
t_stat reason;

/* A device's service routine may reconfigure it, and a reconfiguration
   rebuilds the I/O page dispatch, so the bus's claim on it is checked here -
   the one place every device's work passes through. */
simh_shim_bus_reassert ();

shim_update_time ();
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

sim_switches = 0;                                       /* a fresh command */
sim_switch_number = 0;
if ((cptr = get_sim_sw (cptr)) == NULL)
    return SCPE_INVSW;
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

/* Booting. scp's RUN command does three jobs, and only one of them belongs to
   an embedded core: RUN and CONTINUE start the instruction loop, which here is
   the embedding's own, but BOOT also empties the event queue, resets the
   machine and calls the device's boot routine, which is what places a
   bootstrap in memory and sets the processor at it. That part is real.

   A processor's boot reaches this through its own BOOT command, which parses
   the device the operator named and sets the registers its bootstrap expects
   before asking for the CPU to be booted. */
t_stat run_cmd (int32 flag, CONST char *cptr)
{
char gbuf[CBUFSIZE];
DEVICE *dptr;
UNIT *uptr;
t_stat r;

if (flag != RU_BOOT)
    return shim_no_command ("RUN");
if (*cptr == 0)
    return SCPE_2FARG;
get_glyph (cptr, gbuf, 0);
if ((dptr = find_unit (gbuf, &uptr)) == NULL)
    return sim_messagef (SCPE_NXDEV, "no such device: %s\n", gbuf);
if (uptr == NULL)
    return SCPE_NXUN;
if (dptr->boot == NULL)
    return sim_messagef (SCPE_NOFNC, "%s cannot boot\n", sim_dname (dptr));
if (uptr->flags & UNIT_DIS)
    return sim_messagef (SCPE_UDIS, "%s is disabled\n", sim_uname (uptr));
if ((uptr->flags & UNIT_ATTABLE) && !(uptr->flags & UNIT_ATT))
    return sim_messagef (SCPE_UNATT, "%s has nothing attached\n", sim_uname (uptr));

while (sim_clock_queue != QUEUE_LIST_END)               /* empty the queue */
    sim_cancel (sim_clock_queue);
sim_time = 0.0;
noqueue_time = sim_interval = 0;
if ((r = shim_reset_devices ()) != SCPE_OK)
    return r;
return dptr->boot ((int32) (uptr - dptr->units), dptr);
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

/* The Ethernet CRC of sim_ether.c, which the disk layer borrows for the
   checksum in a VHD footer and for a pack id. Reflected CRC-32 over the
   Autodin II polynomial, inverted at both ends, as its callers expect. */

uint32 eth_crc32 (uint32 crc, const void *vbuf, size_t len)
{
static uint32 table[256];
static int table_built = 0;
const unsigned char *buf = (const unsigned char *) vbuf;

if (!table_built) {
    uint32 i, j, entry;

    for (i = 0; i < 256; i++) {
        entry = i;
        for (j = 0; j < 8; j++)
            entry = (entry & 1) ? ((entry >> 1) ^ 0xEDB88320u) : (entry >> 1);
        table[i] = entry;
        }
    table_built = 1;
    }
crc ^= 0xFFFFFFFFu;
while (len-- != 0)
    crc = (crc >> 8) ^ table[(crc ^ (*buf++)) & 0xFF];
return crc ^ 0xFFFFFFFFu;
}

/* simh unpacks a disk container out of a tar archive through scp's TAR command,
   which shells out to the host's tar. The shim carries no command interpreter
   and no shell, so a container arrives unpacked or not at all. */

t_stat tar_cmd (int32 flag, CONST char *cptr)
{
(void) flag;
(void) cptr;
return sim_messagef (SCPE_NOFNC, "TAR is not available in the embedded build\n");
}

/* ------------------------------------------------------------------------ */
/* The embedding API                                                         */
/* ------------------------------------------------------------------------ */

simh_shim_status_t simh_shim_reset (void)
{
DEVICE *dptr;
uint32 i, j;

/* simh's file layer settles its endianness and its large-file support here,
   and what it works out is read far from the file layer: the disk layer asks
   sim_toffset_64 whether raw containers are possible at all, and answers no to
   every format probe while it is unset. scp does this first of all. */
sim_finit ();

if (stdnul == NULL)
    stdnul = fopen ("/dev/null", "w");
if (sim_eval == NULL)
    sim_eval = (t_value *) calloc (SHIM_EVAL_SIZE, sizeof (*sim_eval));

shim_batch_unit.action = shim_batch_svc;

/* A unit's back pointer to its device, which scp fills in when it walks
   sim_devices[] at startup. Device code reads it directly - simh's disk layer
   names a container's device through it, and sim_debug_unit() finds a device's
   trace flags through it - so it has to be there before anything runs. */
for (i = 0; (dptr = sim_devices[i]) != NULL; i++)
    for (j = 0; j < dptr->numunits; j++)
        dptr->units[j].dptr = dptr;

return shim_reset_devices ();
}

int simh_shim_batch_ended (simh_shim_status_t status)
{
return (status == SCPE_STOP);
}

simh_shim_status_t simh_shim_run (int max_instructions)
{
t_stat reason;

sim_cancel (&shim_batch_unit);
if (max_instructions > 0)
    sim_activate (&shim_batch_unit, max_instructions);
reason = sim_instr ();
sim_cancel (&shim_batch_unit);
return reason;
}

simh_shim_status_t simh_shim_set (const char *setting)
{
return set_cmd (0, setting);
}

int simh_shim_device_info (const char *name, unsigned *base_addr, int *enabled)
{
char gbuf[CBUFSIZE];
DEVICE *dptr;
UNIT *uptr;

shim_get_glyph (name, gbuf, 0, TRUE);
if ((dptr = find_dev (gbuf)) == NULL) {
    if ((dptr = find_unit (gbuf, &uptr)) == NULL)
        return 0;
    }
/* The device information block's first member is the address the device
   answers at, which is what a boot command reads out of it. Its declaration
   lives in the PDP-11 headers, which will not compile against a VAX's value
   types, so the one member wanted here is read where it sits. */
*base_addr = (dptr->ctxt != NULL) ? *(const uint32 *) dptr->ctxt : 0;
*enabled = (dptr->flags & DEV_DIS) ? 0 : 1;
return 1;
}

simh_shim_status_t simh_shim_attach (const char *unit_name, const char *filename)
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

simh_shim_status_t simh_shim_boot (const char *unit_name)
{
CTAB *cmd;

/* A processor that defines its own BOOT wants it: on the VAX it is what reads
   the unit and the R5 flags the operator gave, sets the registers VMB expects,
   and only then asks for the CPU to be booted. */
for (cmd = sim_vm_cmd; (cmd != NULL) && (cmd->name != NULL); cmd++)
    if (strcmp (cmd->name, "BOOT") == 0)
        return cmd->action (cmd->arg, unit_name);

return run_cmd (RU_BOOT, unit_name);
}
