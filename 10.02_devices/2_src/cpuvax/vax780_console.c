/* vax780_console.c: the command language of the VAX-11/780 console
 *
 * A 780 is started from its console, an LSI-11 with the RX01 floppy that
 * carries the console program, the bootstraps and a command file for every way
 * of starting the machine. BOOT runs one of those files, @ runs any of them, and
 * the commands inside halt and initialize the processor, load a program off the
 * floppy, deposit its parameters in the registers and start it. This file
 * speaks enough of that language for the command files DEC and the operating
 * systems shipped: VMS boots through the memory controller's ROM program and
 * VMB, ULTRIX through its own BOOT, and both are driven exactly as the floppy
 * says.
 *
 * Commands are those of the console, with its abbreviations:
 *
 *	BOOT [name]		run <name>BOO.CMD, or DEFBOO.CMD without a name
 *	@file			run a command file, .CMD by default
 *	LOAD file[/START:a]	read a file off the floppy into memory at a
 *	DEPOSIT[/q] addr data	a register, a processor register or memory
 *	EXAMINE[/q] addr	the same, printed on the console terminal
 *	START [addr]		initialize the processor and start it at addr
 *	WAIT DONE		let the program run until it signals completion
 *	HALT, UNJAM, INIT	the processor and the SBI
 *	SET DEFAULT ..., SET RELOCATION:n
 *	! comment
 *
 * Numbers are hexadecimal unless SET DEFAULT names another radix. @ in an
 * address or a datum is the value last examined or deposited, which is how a
 * command file hands the address the ROM program found on to VMB.
 *
 * Every command file is read and every command's name checked before the
 * first command runs, so a missing file or a misspelt command fails the START
 * with nothing done; a command whose arguments are wrong fails when it runs.
 * The commands then run on the processor's thread. START hands the processor
 * over; when WAIT DONE follows, the console takes the remaining
 * commands up again once the program has halted or signalled completion, which
 * the embedding reports through simh_shim_console_resume().
 *
 * As the real console does, every line of a command file is printed on the
 * console terminal as it is executed.
 */

#include "simh_shim.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_defs.h"
#include "scp.h"
#include "vax_defs.h"
#include "simh_shim_internal.h"

/* The floppy and its RT-11 file system, as simh's FLOAD reads them. */
extern UNIT fl_unit;
extern t_bool rtfile_parse (char *pntr, uint16 *file_name);
extern uint32 rtfile_lookup (uint16 *file_name, uint32 *start);
extern t_stat rtfile_read (uint32 block, uint32 count, uint16 *buffer);

/* The memory controllers' ROMs, which the console can start a program in. */
extern uint32 rom_lw[][ROMSIZE >> 2];

extern t_stat cpu_reset (DEVICE *dptr);

/* The console's limits. A command file is a block or two of text; nesting
   deeper than a few files means one calls itself. */
#define CON_LINE_MAX            132
#define CON_COMMANDS_MAX        512
#define CON_DEPTH_MAX           8
#define CON_FILE_BLOCKS_MAX     16

/* Where a line came from, for the operator's error message. */
#define CON_SOURCE_MAX          16

/* What the processor is left with by INIT and START: interrupt stack, IPL 31,
   kernel mode, which on a 780 is PSL 041F0000. */
#define CON_INIT_PSL            (PSL_IS | PSL_IPL1F)

typedef struct {
    char text[CON_LINE_MAX];                    /* the command, upper case, no comment */
    char echo[CON_LINE_MAX];                    /* the line as the terminal shows it */
    char source[CON_SOURCE_MAX];                /* the file, empty for the command line */
    unsigned line;
    } con_command_t;

static con_command_t con_commands[CON_COMMANDS_MAX];
static unsigned con_count = 0;
static unsigned con_next = 0;

static uint32 con_radix = 16;
static uint32 con_size = L_LONG;                /* default data length */
static uint32 con_rel = 0;                      /* relocation */
static uint32 con_last = 0;                     /* @: last examined or deposited */
static t_bool con_started = FALSE;              /* a START has run */

static char *con_msg = NULL;
static size_t con_msg_size = 0;

/* ------------------------------------------------------------------------ */
/* Messages and the terminal                                                 */
/* ------------------------------------------------------------------------ */

static void con_print (const char *text)
{
for (; *text != 0; text++) {
    if (*text == '\n')
        simh_shim_console_put ('\r');
    simh_shim_console_put (*text);
    }
}

static void con_where (const con_command_t *cmd, char *buf, size_t size)
{
if (cmd->source[0] != 0)
    snprintf (buf, size, "%s line %u", cmd->source, cmd->line);
else snprintf (buf, size, "bootdevice command %u", cmd->line);
}

/* Fail with a message naming the command and where it was written. */
static simh_shim_console_state_t con_fail (const con_command_t *cmd, const char *fmt, ...)
{
char where[CON_SOURCE_MAX + 32];
char what[160];
va_list args;

va_start (args, fmt);
vsnprintf (what, sizeof what, fmt, args);
va_end (args);
con_where (cmd, where, sizeof where);
if ((con_msg != NULL) && (con_msg_size > 0))
    snprintf (con_msg, con_msg_size, "%s: \"%s\": %s", where, cmd->echo, what);
con_print ("?");
con_print (what);
con_print ("\n");
return SIMH_SHIM_CONSOLE_ERROR;
}

/* ------------------------------------------------------------------------ */
/* Words                                                                     */
/* ------------------------------------------------------------------------ */

/* A keyword the console knows, and how short it may be written. */
typedef struct {
    const char *name;
    size_t min;
    } con_keyword_t;

static t_bool con_is (const char *word, const char *name, size_t min)
{
size_t n = strlen (word);

return (n >= min) && (n <= strlen (name)) && (strncmp (word, name, n) == 0);
}

/* A number in the current radix, or @ for the last value. */
static t_bool con_number (const char *text, uint32 radix, uint32 *value)
{
char *end;
unsigned long long v;

if (strcmp (text, "@") == 0) {
    *value = con_last;
    return TRUE;
    }
if (*text == 0)
    return FALSE;
v = strtoull (text, &end, (int) radix);
if ((*end != 0) || (v > 0xFFFFFFFFull))
    return FALSE;
*value = (uint32) v;
return TRUE;
}

/* A general register by name: R0 to R15, AP, FP, SP, PC. */
static int con_register_name (const char *text)
{
char *end;
long n;

if (strcmp (text, "AP") == 0)
    return nAP;
if (strcmp (text, "FP") == 0)
    return nFP;
if (strcmp (text, "SP") == 0)
    return nSP;
if (strcmp (text, "PC") == 0)
    return nPC;
if ((text[0] == 'R') && isdigit ((unsigned char) text[1])) {
    n = strtol (text + 1, &end, 10);
    if ((*end == 0) && (n >= 0) && (n <= 15))
        return (int) n;
    }
return -1;
}

/* A command split into its words: the verb, the qualifiers wherever they were
   written, and the arguments. */
#define CON_WORDS_MAX           8

typedef struct {
    char verb[CON_LINE_MAX];
    char qual[CON_WORDS_MAX][CON_LINE_MAX];     /* without the slash */
    unsigned nqual;
    char arg[CON_WORDS_MAX][CON_LINE_MAX];
    unsigned narg;
    } con_words_t;

static t_bool con_split (const char *text, con_words_t *w)
{
char word[CON_LINE_MAX];
const char *p = text;
t_bool first = TRUE;

memset (w, 0, sizeof *w);
while (*p != 0) {
    size_t n = 0;
    char *part;
    char *slash;

    while (isspace ((unsigned char) *p))
        p++;
    if (*p == 0)
        break;
    while ((*p != 0) && !isspace ((unsigned char) *p) && (n < sizeof word - 1))
        word[n++] = *p++;
    word[n] = 0;
    /* a word is an argument with qualifiers hanging off it, or qualifiers alone */
    slash = strchr (word, '/');
    if (slash != NULL)
        *slash = 0;
    if (word[0] != 0) {
        if (first)
            strcpy (w->verb, word);
        else {
            if (w->narg >= CON_WORDS_MAX)
                return FALSE;
            strcpy (w->arg[w->narg++], word);
            }
        }
    first = FALSE;
    for (part = slash; part != NULL; part = slash) {
        slash = strchr (part + 1, '/');
        if (slash != NULL)
            *slash = 0;
        if (w->nqual >= CON_WORDS_MAX)
            return FALSE;
        strcpy (w->qual[w->nqual++], part + 1);
        }
    }
return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Reading command files off the floppy                                      */
/* ------------------------------------------------------------------------ */

static simh_shim_console_state_t con_expand (const char *line, const char *source,
                                             unsigned lineno, unsigned depth);

/* Read an RT-11 file whole into text, ending it at the first NUL or ^Z. */
static t_bool con_read_file (const char *name, char *text, size_t size, const char **why)
{
uint16 rad50[3];
uint16 buf[CON_FILE_BLOCKS_MAX * 256];
uint32 start, blocks, i;
char rtname[CON_LINE_MAX];

if (((fl_unit.flags & UNIT_ATT) == 0) || (fl_unit.filebuf == NULL)) {
    *why = "no floppy in the console drive";
    return FALSE;
    }
strncpy (rtname, name, sizeof rtname - 1);
rtname[sizeof rtname - 1] = 0;
if (!rtfile_parse (rtname, rad50)) {
    *why = "not a file name";
    return FALSE;
    }
if ((blocks = rtfile_lookup (rad50, &start)) == 0) {
    *why = "no such file on the console floppy";
    return FALSE;
    }
if (blocks > CON_FILE_BLOCKS_MAX) {
    *why = "too long for a command file";
    return FALSE;
    }
if (!rtfile_read (start, blocks, buf)) {
    *why = "the file runs past the end of the floppy";
    return FALSE;
    }
for (i = 0; (i < blocks * 512) && (i < size - 1); i++) {
    char c = (char) ((buf[i >> 1] >> ((i & 1) * 8)) & 0177);

    if ((c == 0) || (c == 032))
        break;
    text[i] = c;
    }
text[i] = 0;
return TRUE;
}

/* Expand a command file, line by line, into the command list. */
static simh_shim_console_state_t con_expand_file (const con_command_t *at, const char *name,
                                                  unsigned depth)
{
static char text[CON_DEPTH_MAX][CON_FILE_BLOCKS_MAX * 512 + 1];
const char *why = NULL;
char *p, *eol;
unsigned lineno = 0;
simh_shim_console_state_t s;

if (depth >= CON_DEPTH_MAX)
    return con_fail (at, "command files nest deeper than %u", CON_DEPTH_MAX);
if (!con_read_file (name, text[depth], sizeof text[depth], &why))
    return con_fail (at, "%s: %s", name, why);
for (p = text[depth]; *p != 0; p = eol) {
    char line[CON_LINE_MAX];
    size_t n;

    eol = p + strcspn (p, "\r\n");
    n = (size_t) (eol - p);
    if (n >= sizeof line)
        n = sizeof line - 1;
    memcpy (line, p, n);
    line[n] = 0;
    if (*eol == '\r')
        eol++;
    if (*eol == '\n')
        eol++;
    lineno++;
    if ((s = con_expand (line, name, lineno, depth + 1)) != SIMH_SHIM_CONSOLE_HALTED)
        return s;
    }
return SIMH_SHIM_CONSOLE_HALTED;
}

/* The file BOOT runs for a device: DUA0 and DU0 both name DU0BOO.CMD, as the
   console shortens a controller letter away; anything else is used as given. */
static t_bool con_boot_file (const char *device, char *file, size_t size)
{
char name[8];

if (*device == 0)
    strcpy (name, "DEF");
else if ((strlen (device) == 4) && isalpha ((unsigned char) device[0]) &&
         isalpha ((unsigned char) device[1]) && isalpha ((unsigned char) device[2]) &&
         isdigit ((unsigned char) device[3])) {
    name[0] = device[0];
    name[1] = device[1];
    name[2] = device[3];
    name[3] = 0;
    }
else if (strlen (device) <= 3)
    strcpy (name, device);
else return FALSE;
snprintf (file, size, "%sBOO.CMD", name);
return TRUE;
}

/* The commands the console executes, and how short each may be written. */
static const con_keyword_t con_verbs[] = {
    { "BOOT", 1 }, { "DEPOSIT", 1 }, { "EXAMINE", 1 }, { "HALT", 1 },
    { "INITIALIZE", 1 }, { "LOAD", 1 }, { "SET", 2 }, { "START", 1 },
    { "UNJAM", 1 }, { "WAIT", 1 } };

static t_bool con_known (const char *verb)
{
size_t i;

for (i = 0; i < sizeof con_verbs / sizeof con_verbs[0]; i++)
    if (con_is (verb, con_verbs[i].name, con_verbs[i].min))
        return TRUE;
return FALSE;
}

/* One line: recorded for execution, or for BOOT and @, replaced by the lines of
   the file it names. */
static simh_shim_console_state_t con_expand (const char *line, const char *source,
                                             unsigned lineno, unsigned depth)
{
con_command_t *cmd;
con_words_t w;
char *bang;
size_t n;

if (con_count >= CON_COMMANDS_MAX) {
    con_command_t over;

    memset (&over, 0, sizeof over);
    strncpy (over.source, source, sizeof over.source - 1);
    over.line = lineno;
    return con_fail (&over, "more than %u commands", CON_COMMANDS_MAX);
    }
cmd = &con_commands[con_count];
memset (cmd, 0, sizeof *cmd);
strncpy (cmd->source, source, sizeof cmd->source - 1);
cmd->line = lineno;

/* the line as printed, without trailing blanks */
strncpy (cmd->echo, line, sizeof cmd->echo - 1);
for (n = strlen (cmd->echo); (n > 0) && isspace ((unsigned char) cmd->echo[n - 1]); n--)
    cmd->echo[n - 1] = 0;
for (n = 0; cmd->echo[n] == ' '; n++)
    ;
if (n > 0)
    memmove (cmd->echo, cmd->echo + n, strlen (cmd->echo + n) + 1);

/* the command, upper case and without its comment */
strncpy (cmd->text, cmd->echo, sizeof cmd->text - 1);
if ((bang = strchr (cmd->text, '!')) != NULL)
    *bang = 0;
for (n = 0; cmd->text[n] != 0; n++)
    cmd->text[n] = (char) toupper ((unsigned char) cmd->text[n]);
for (n = strlen (cmd->text); (n > 0) && isspace ((unsigned char) cmd->text[n - 1]); n--)
    cmd->text[n - 1] = 0;
for (n = 0; isspace ((unsigned char) cmd->text[n]); n++)
    ;
if (n > 0)
    memmove (cmd->text, cmd->text + n, strlen (cmd->text + n) + 1);

if ((cmd->echo[0] == 0) && (cmd->text[0] == 0))
    return SIMH_SHIM_CONSOLE_HALTED;            /* an empty line */
con_count++;

if (cmd->text[0] == '@') {
    char file[CON_LINE_MAX];
    const char *name = cmd->text + 1;

    while (isspace ((unsigned char) *name))
        name++;
    if (*name == 0)
        return con_fail (cmd, "@ names no file");
    snprintf (file, sizeof file, "%s%s", name, strchr (name, '.') ? "" : ".CMD");
    return con_expand_file (cmd, file, depth);
    }
if (!con_split (cmd->text, &w))
    return con_fail (cmd, "too many words");
if ((w.verb[0] != 0) && !con_known (w.verb))
    return con_fail (cmd, "unknown command %s", w.verb);
if (con_is (w.verb, "BOOT", 1)) {
    char file[CON_LINE_MAX];

    if (w.nqual != 0)
        return con_fail (cmd, "BOOT takes no qualifier here");
    if (w.narg > 1)
        return con_fail (cmd, "BOOT takes one device");
    if (!con_boot_file ((w.narg == 1) ? w.arg[0] : "", file, sizeof file))
        return con_fail (cmd, "%s names no boot command file", w.arg[0]);
    return con_expand_file (cmd, file, depth);
    }
return SIMH_SHIM_CONSOLE_HALTED;
}

/* ------------------------------------------------------------------------ */
/* Executing                                                                 */
/* ------------------------------------------------------------------------ */

/* Where EXAMINE and DEPOSIT reach. */
typedef enum { CON_GPR, CON_PSL, CON_IPR, CON_MEM } con_space_t;

typedef struct {
    con_space_t space;
    uint32 addr;
    uint32 size;                                /* bytes, memory only */
    uint32 count;                               /* locations, /N:n adds n */
    } con_target_t;

static simh_shim_console_state_t con_target (const con_command_t *cmd, const con_words_t *w,
                                             con_target_t *t)
{
t_bool gpr = FALSE, ipr = FALSE;
unsigned i;
int rn;

t->size = con_size;
t->count = 1;
for (i = 0; i < w->nqual; i++) {
    const char *q = w->qual[i];

    if (strcmp (q, "G") == 0)
        gpr = TRUE;
    else if (strcmp (q, "I") == 0)
        ipr = TRUE;
    else if (strcmp (q, "P") == 0)
        ;                                       /* physical, the only space here */
    else if (strcmp (q, "B") == 0)
        t->size = L_BYTE;
    else if (strcmp (q, "W") == 0)
        t->size = L_WORD;
    else if (strcmp (q, "L") == 0)
        t->size = L_LONG;
    else if ((q[0] == 'N') && ((q[1] == ':') || (q[1] == '='))) {
        uint32 n;

        if (!con_number (q + 2, con_radix, &n) || (n > 0xFFFF))
            return con_fail (cmd, "/N wants a count");
        t->count = n + 1;
        }
    else if (strcmp (q, "V") == 0)
        return con_fail (cmd, "virtual addresses are not reached by this console");
    else if (strcmp (q, "ID") == 0)
        return con_fail (cmd, "the ID bus is not reached by this console");
    else return con_fail (cmd, "unknown qualifier /%s", q);
    }
if (w->narg == 0)
    return con_fail (cmd, "no address");
if (strcmp (w->arg[0], "PSL") == 0) {
    t->space = CON_PSL;
    t->addr = 0;
    return SIMH_SHIM_CONSOLE_RUNNING;
    }
if ((rn = con_register_name (w->arg[0])) >= 0) {
    t->space = CON_GPR;
    t->addr = (uint32) rn;
    return SIMH_SHIM_CONSOLE_RUNNING;
    }
if (!con_number (w->arg[0], con_radix, &t->addr))
    return con_fail (cmd, "%s is not an address", w->arg[0]);
if (gpr) {
    if (t->addr > 15)
        return con_fail (cmd, "there is no general register %X", t->addr);
    t->space = CON_GPR;
    }
else if (ipr)
    t->space = CON_IPR;
else {
    t->space = CON_MEM;
    t->addr = t->addr + con_rel;
    }
return SIMH_SHIM_CONSOLE_RUNNING;
}

/* One location of memory, physical, of the target's size. */
static t_bool con_mem (uint32 addr, uint32 size, uint32 *value, t_bool write)
{
uint32 i;
uint32 v = 0;

for (i = 0; i < size; i++) {
    uint32 a = addr + i;
    uint32 sh = (a & 3) * 8;

    if (!ADDR_IS_MEM (a))
        return FALSE;
    if (write)
        M[a >> 2] = (M[a >> 2] & ~(0xFFu << sh)) | (((*value >> (i * 8)) & 0xFF) << sh);
    else v |= ((M[a >> 2] >> sh) & 0xFF) << (i * 8);
    }
if (!write)
    *value = v;
return TRUE;
}

/* The processor registers a console sets up a bootstrap with. The rest are
   the operating system's business and reached by its own MTPR. */
static t_bool con_ipr (uint32 n, uint32 *value, t_bool write)
{
int32 *reg;

switch (n) {
    case MT_SCBB:
        reg = &SCBB;
        break;
    case MT_PCBB:
        reg = &PCBB;
        break;
    default:
        return FALSE;
    }
if (write)
    *reg = (int32) (*value & BR_MASK);
else *value = (uint32) *reg;
return TRUE;
}

static simh_shim_console_state_t con_access (const con_command_t *cmd, const con_words_t *w,
                                             t_bool write)
{
con_target_t t;
simh_shim_console_state_t s;
uint32 data = 0, i;
char line[64];

if ((s = con_target (cmd, w, &t)) != SIMH_SHIM_CONSOLE_RUNNING)
    return s;
if (write) {
    if (w->narg != 2)
        return con_fail (cmd, "DEPOSIT wants an address and a datum");
    if (!con_number (w->arg[1], con_radix, &data))
        return con_fail (cmd, "%s is not a number", w->arg[1]);
    }
else if (w->narg != 1)
    return con_fail (cmd, "EXAMINE wants one address");

for (i = 0; i < t.count; i++) {
    switch (t.space) {
        case CON_GPR:
            if (t.addr > 15)
                return con_fail (cmd, "past the last general register");
            if (write)
                R[t.addr] = (int32) data;
            else data = (uint32) R[t.addr];
            snprintf (line, sizeof line, "G %08X %08X\n", t.addr, data);
            break;
        case CON_PSL:
            if (write)
                PSL = (int32) data;
            else data = (uint32) PSL;
            snprintf (line, sizeof line, "  PSL %08X\n", data);
            break;
        case CON_IPR:
            if (!con_ipr (t.addr, &data, write))
                return con_fail (cmd, "processor register %X is not reached by this console",
                                 t.addr);
            snprintf (line, sizeof line, "I %08X %08X\n", t.addr, data);
            break;
        case CON_MEM:
            if (!con_mem (t.addr, t.size, &data, write))
                return con_fail (cmd, "no memory at %08X", t.addr);
            snprintf (line, sizeof line, "P %08X %0*X\n", t.addr, (int) (t.size * 2), data);
            break;
        }
    if (!write)
        con_print (line);
    con_last = data;
    if (t.space == CON_MEM)
        t.addr += t.size;
    else t.addr++;
    }
return SIMH_SHIM_CONSOLE_RUNNING;
}

/* LOAD, through simh's own FLOAD, which reads the floppy's RT-11 file system. */
static simh_shim_console_state_t con_load (const con_command_t *cmd, const con_words_t *w)
{
uint32 origin = 0;
unsigned i;
char args[CON_LINE_MAX + 16];
CTAB *ctab;
t_stat r;

if (w->narg != 1)
    return con_fail (cmd, "LOAD wants one file");
for (i = 0; i < w->nqual; i++) {
    const char *q = w->qual[i];
    const char *colon = strpbrk (q, ":=");
    char name[CON_LINE_MAX];
    size_t n = colon ? (size_t) (colon - q) : strlen (q);

    if (n >= sizeof name)
        n = sizeof name - 1;
    memcpy (name, q, n);
    name[n] = 0;
    if (con_is (name, "START", 1) && (colon != NULL)) {
        if (!con_number (colon + 1, con_radix, &origin))
            return con_fail (cmd, "%s is not an address", colon + 1);
        }
    else if (strcmp (name, "P") == 0)
        ;
    else return con_fail (cmd, "unknown qualifier /%s", q);
    }
origin = origin + con_rel;
if ((fl_unit.flags & UNIT_ATT) == 0)
    return con_fail (cmd, "no floppy in the console drive");
for (ctab = sim_vm_cmd; (ctab != NULL) && (ctab->name != NULL); ctab++)
    if (strcmp (ctab->name, "FLOAD") == 0)
        break;
if ((ctab == NULL) || (ctab->name == NULL))
    return con_fail (cmd, "the processor has no floppy loader");
snprintf (args, sizeof args, "%s %X", w->arg[0], origin);
if ((r = ctab->action (ctab->arg, args)) != SCPE_OK) {
    if (r == SCPE_ARG)
        return con_fail (cmd, "%s: no such file on the console floppy", w->arg[0]);
    if (r == SCPE_NXM)
        return con_fail (cmd, "%s does not fit in memory at %08X", w->arg[0], origin);
    return con_fail (cmd, "%s: %s", w->arg[0], sim_error_text (r));
    }
return SIMH_SHIM_CONSOLE_RUNNING;
}

/* What INIT and START do to the processor: PSL 041F0000, memory management
   off, no software interrupt pending. The general registers keep what was
   deposited in them, since that is how a bootstrap gets its parameters. */
static t_stat con_init (void)
{
return cpu_reset (&cpu_dev);
}

/* The MS780 memory controller carries a ROM whose program a VMS command file
   starts at 20003000. It finds the first 64 KB of good memory, leaves SP
   pointing ^X200 into it and halts, and the command file loads VMB there. The
   emulation does not carry DEC's ROM image; its memory has no bad pages, so
   the program it puts in an empty ROM does the same in two instructions:
   MOVL #^X200,SP and HALT. */
static void con_rom_program (uint32 addr)
{
uint32 (*rom)[ROMSIZE >> 2] = NULL;

if (ADDR_IS_ROM0 (addr))
    rom = &rom_lw[0];
else if (ADDR_IS_ROM1 (addr))
    rom = &rom_lw[1];
if ((rom == NULL) || ((*rom)[0] != 0) || ((*rom)[1] != 0))
    return;
(*rom)[0] = 0x02008FD0;                         /* D0 8F 00 02 */
(*rom)[1] = 0x005E0000;                         /* 00 00 5E 00 */
}

static simh_shim_console_state_t con_start (const con_command_t *cmd, const con_words_t *w)
{
uint32 addr;
t_stat r;

if (w->nqual != 0)
    return con_fail (cmd, "START takes no qualifier");
if (w->narg > 1)
    return con_fail (cmd, "START wants one address");
if (w->narg == 1) {
    if (!con_number (w->arg[0], con_radix, &addr))
        return con_fail (cmd, "%s is not an address", w->arg[0]);
    addr = addr + con_rel;
    }
else addr = (uint32) PC;
if ((r = con_init ()) != SCPE_OK)
    return con_fail (cmd, "the processor did not initialize: %s", sim_error_text (r));
con_rom_program (addr);
PSL = CON_INIT_PSL;
PC = (int32) addr;
con_started = TRUE;
return SIMH_SHIM_CONSOLE_RUNNING;
}

static simh_shim_console_state_t con_set (const con_command_t *cmd, const con_words_t *w)
{
static const con_keyword_t radix[] = {
    { "HEXADECIMAL", 1 }, { "OCTAL", 1 }, { "DECIMAL", 1 } };
static const uint32 radix_value[] = { 16, 8, 10 };
static const con_keyword_t size[] = {
    { "BYTE", 1 }, { "WORD", 1 }, { "LONG", 1 } };
static const uint32 size_value[] = { L_BYTE, L_WORD, L_LONG };
char what[CON_LINE_MAX];
const char *value = NULL;
char *sep;
unsigned i, j;

if (w->narg == 0)
    return con_fail (cmd, "SET what?");
strcpy (what, w->arg[0]);
if ((sep = strpbrk (what, ":=")) != NULL) {
    *sep = 0;
    value = sep + 1;
    if (*value == 0)
        value = (w->narg > 1) ? w->arg[1] : NULL;
    }
else if (w->narg > 1)
    value = w->arg[1];

if (con_is (what, "RELOCATION", 3)) {
    if ((value == NULL) || !con_number (value, con_radix, &con_rel))
        return con_fail (cmd, "SET RELOCATION wants an address");
    return SIMH_SHIM_CONSOLE_RUNNING;
    }
if (con_is (what, "DEFAULT", 3)) {
    /* SET DEFAULT HEX, LONG or both, separated by commas */
    for (i = 1; i < w->narg; i++) {
        char list[CON_LINE_MAX];
        char *item;

        strcpy (list, w->arg[i]);
        for (item = strtok (list, ","); item != NULL; item = strtok (NULL, ",")) {
            t_bool known = FALSE;

            for (j = 0; j < 3; j++) {
                if (con_is (item, radix[j].name, radix[j].min)) {
                    con_radix = radix_value[j];
                    known = TRUE;
                    }
                }
            for (j = 0; !known && (j < 3); j++) {
                if (con_is (item, size[j].name, size[j].min)) {
                    con_size = size_value[j];
                    known = TRUE;
                    }
                }
            if (!known && (strcmp (item, "PHYSICAL") == 0))
                known = TRUE;                   /* the only space here */
            if (!known)
                return con_fail (cmd, "SET DEFAULT %s is not known", item);
            }
        }
    return SIMH_SHIM_CONSOLE_RUNNING;
    }
return con_fail (cmd, "SET %s is not known", what);
}

/* One command. RUNNING means it ran and the next may follow. */
static simh_shim_console_state_t con_execute (const con_command_t *cmd)
{
con_words_t w;

if (cmd->text[0] == 0)
    return SIMH_SHIM_CONSOLE_RUNNING;           /* a comment */
if (cmd->text[0] == '@')
    return SIMH_SHIM_CONSOLE_RUNNING;           /* expanded already */
if (!con_split (cmd->text, &w))
    return con_fail (cmd, "too many words");
if (con_is (w.verb, "BOOT", 1))
    return SIMH_SHIM_CONSOLE_RUNNING;           /* expanded already */
if (con_is (w.verb, "DEPOSIT", 1))
    return con_access (cmd, &w, TRUE);
if (con_is (w.verb, "EXAMINE", 1))
    return con_access (cmd, &w, FALSE);
if (con_is (w.verb, "LOAD", 1))
    return con_load (cmd, &w);
if (con_is (w.verb, "START", 1))
    return con_start (cmd, &w);
if (con_is (w.verb, "SET", 2))
    return con_set (cmd, &w);
if (con_is (w.verb, "HALT", 1) || con_is (w.verb, "UNJAM", 1)) {
    /* the processor is halted while the console runs it, and the SBI the
       emulation carries does not jam */
    if ((w.narg != 0) || (w.nqual != 0))
        return con_fail (cmd, "%s takes nothing more", w.verb);
    return SIMH_SHIM_CONSOLE_RUNNING;
    }
if (con_is (w.verb, "INITIALIZE", 1)) {
    t_stat r;

    if ((w.narg != 0) || (w.nqual != 0))
        return con_fail (cmd, "INITIALIZE takes nothing more");
    if ((r = con_init ()) != SCPE_OK)
        return con_fail (cmd, "the processor did not initialize: %s", sim_error_text (r));
    return SIMH_SHIM_CONSOLE_RUNNING;
    }
if (con_is (w.verb, "WAIT", 1)) {
    if ((w.narg != 1) || !con_is (w.arg[0], "DONE", 1))
        return con_fail (cmd, "WAIT knows only DONE");
    if (!con_started)
        return con_fail (cmd, "WAIT DONE with no program started");
    return SIMH_SHIM_CONSOLE_WAITING;
    }
return con_fail (cmd, "unknown command %s", w.verb);
}

/* A command as the terminal shows it: a line of a command file as it stands,
   a command of the command line after the console's prompt. */
static void con_echo (const con_command_t *cmd)
{
if (cmd->source[0] == 0)
    con_print (">>>");
con_print (cmd->echo);
con_print ("\n");
}

/* Run commands from where the list stands until it ends, a START hands the
   processor over, or a WAIT DONE waits for the program. */
static simh_shim_console_state_t con_run (void)
{
simh_shim_console_state_t s;

while (con_next < con_count) {
    const con_command_t *cmd = &con_commands[con_next++];

    con_echo (cmd);
    s = con_execute (cmd);
    if (s != SIMH_SHIM_CONSOLE_RUNNING)
        return s;
    if (con_started) {
        /* START ran: the program has the processor. A WAIT DONE right after it
           (comments aside) is the console waiting for that program, and the
           commands beyond it run when it is done. */
        while ((con_next < con_count) && (con_commands[con_next].text[0] == 0))
            con_echo (&con_commands[con_next++]);
        if (con_next < con_count) {
            con_words_t w;

            cmd = &con_commands[con_next];
            if (con_split (cmd->text, &w) && con_is (w.verb, "WAIT", 1)) {
                con_next++;
                con_echo (cmd);
                return con_execute (cmd);
                }
            return con_fail (cmd, "a command follows START with no WAIT DONE before it");
            }
        return SIMH_SHIM_CONSOLE_RUNNING;
        }
    }
return SIMH_SHIM_CONSOLE_HALTED;
}

/* ------------------------------------------------------------------------ */
/* The embedding API                                                         */
/* ------------------------------------------------------------------------ */

simh_shim_console_state_t simh_shim_console_begin (const char *commands,
                                                   char *message, size_t message_size)
{
const char *p = commands;
unsigned n = 0;
simh_shim_console_state_t s;

con_msg = message;
con_msg_size = message_size;
if ((message != NULL) && (message_size > 0))
    message[0] = 0;
con_count = con_next = 0;
con_radix = 16;
con_size = L_LONG;
con_rel = 0;
con_last = 0;
con_started = FALSE;

/* commands on the command line are separated by semicolons */
while (*p != 0) {
    char line[CON_LINE_MAX];
    size_t len = strcspn (p, ";");
    size_t copy = (len < sizeof line) ? len : sizeof line - 1;

    memcpy (line, p, copy);
    line[copy] = 0;
    n++;
    if ((s = con_expand (line, "", n, 0)) != SIMH_SHIM_CONSOLE_HALTED)
        return s;
    p += len;
    if (*p == ';')
        p++;
    }
return con_run ();
}

simh_shim_console_state_t simh_shim_console_resume (char *message, size_t message_size)
{
con_msg = message;
con_msg_size = message_size;
if ((message != NULL) && (message_size > 0))
    message[0] = 0;
con_started = FALSE;
return con_run ();
}

int simh_shim_console_done (simh_shim_status_t status)
{
return (status == STOP_HALT) || (status == STOP_SWDN);
}
