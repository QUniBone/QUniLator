/* shim_main.c: a workstation host for the shimmed VAX core
 *
 * The first embedding of the seam simh_shim.h defines, and the one that says on
 * the build machine whether the seam holds. It gives the core a console on the
 * terminal, the wall clock for its timing, and a main loop that runs the
 * processor in batches - which is the shape the CPU device of the QUniLator
 * application will have when the core moves onto the board.
 *
 *	vax780-shim [-m megabytes] [-b instructions] [-l seconds]
 *	            [-s "DEV PARAM"] [-a UNIT=file] [-B UNIT] [-D] [image]
 *
 * With an image it is loaded at address 0 and the processor started there; with
 * a unit to boot from it runs that unit's bootstrap; with neither the core
 * comes up reset, which is enough to say the seam links and the event queue
 * turns. -l bounds the run so an unattended check terminates.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include "simh_shim.h"
#include "scp.h"
#include "vax_defs.h"

/* ------------------------------------------------------------------------ */
/* The console, on the terminal                                              */
/* ------------------------------------------------------------------------ */

static struct termios saved_termios;
static int termios_saved = 0;

/* The console the embedding gives the core must never block its instruction
   loop: sim_poll_kbd() is called from inside the emulation and has to answer
   "nothing waiting" at once. A terminal is put in raw mode besides, so a
   keystroke reaches the guest rather than the line editor. */
static void console_open (void)
{
struct termios raw;
int flags = fcntl (STDIN_FILENO, F_GETFL, 0);

if (flags != -1)
    (void) fcntl (STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
if (!isatty (STDIN_FILENO))
    return;
if (tcgetattr (STDIN_FILENO, &saved_termios) != 0)
    return;
termios_saved = 1;
raw = saved_termios;
raw.c_lflag &= ~(ICANON | ECHO);
raw.c_cc[VMIN] = 0;
raw.c_cc[VTIME] = 0;
tcsetattr (STDIN_FILENO, TCSANOW, &raw);
}

static void console_restore (void)
{
if (termios_saved)
    tcsetattr (STDIN_FILENO, TCSANOW, &saved_termios);
}

static int host_console_get (void *context)
{
unsigned char c;

(void) context;
if (read (STDIN_FILENO, &c, 1) != 1)
    return -1;
return c;
}

static void host_console_put (void *context, int c)
{
(void) context;
putchar (c & 0177);
fflush (stdout);
}

static double host_elapsed_usec (void *context)
{
struct timeval now;

(void) context;
gettimeofday (&now, NULL);
return (double) now.tv_sec * 1000000.0 + (double) now.tv_usec;
}

/* ------------------------------------------------------------------------ */

static void usage (const char *argv0)
{
fprintf (stderr,
    "usage: %s [-m megabytes] [-b instructions] [-l seconds]\n"
    "          [-s \"DEV PARAM\"] [-a UNIT=file] [-B UNIT] [image]\n"
    "  -m  memory size, default 8\n"
    "  -b  instructions per batch, default 100000\n"
    "  -l  stop after this many seconds of host time, default 0 for no limit\n"
    "  -s  set a device parameter, repeatable, e.g. -s \"RQ0 RD54\"\n"
    "  -a  attach a file to a unit, repeatable, e.g. -a RQ0=system.dsk\n"
    "  -B  boot from a unit instead of loading an image\n"
    "  -D  trace every device to stderr\n"
    "  image  loaded at address 0 and started there\n",
    argv0);
}

/* Every device's trace, to stderr. The shim's sim_debug() prints whatever a
   device's dctrl admits, so admitting everything is what a first look wants. */
static void trace_all_devices (void)
{
extern DEVICE *sim_devices[];
int i;

sim_deb = stderr;
for (i = 0; sim_devices[i] != NULL; i++)
    sim_devices[i]->dctrl = 0xFFFFFFFF;
}

#define MAX_SETTINGS 16

int main (int argc, char *argv[])
{
simh_shim_host_t host;
const char *image = NULL;
const char *boot_unit = NULL;
const char *settings[MAX_SETTINGS];
const char *attachments[MAX_SETTINGS];
int nsettings = 0, nattachments = 0;
long megabytes = 8;
long batch = 100000;
long limit_s = 0;
int trace = 0;
char memory[32];
double started;
t_stat r;
int i;

for (i = 1; i < argc; i++) {
    if ((strcmp (argv[i], "-m") == 0) && (i + 1 < argc))
        megabytes = strtol (argv[++i], NULL, 10);
    else if ((strcmp (argv[i], "-b") == 0) && (i + 1 < argc))
        batch = strtol (argv[++i], NULL, 10);
    else if ((strcmp (argv[i], "-l") == 0) && (i + 1 < argc))
        limit_s = strtol (argv[++i], NULL, 10);
    else if ((strcmp (argv[i], "-B") == 0) && (i + 1 < argc))
        boot_unit = argv[++i];
    else if (strcmp (argv[i], "-D") == 0)
        trace = 1;
    else if ((strcmp (argv[i], "-s") == 0) && (i + 1 < argc) && (nsettings < MAX_SETTINGS))
        settings[nsettings++] = argv[++i];
    else if ((strcmp (argv[i], "-a") == 0) && (i + 1 < argc) && (nattachments < MAX_SETTINGS))
        attachments[nattachments++] = argv[++i];
    else if (argv[i][0] == '-') {
        usage (argv[0]);
        return 2;
        }
    else
        image = argv[i];
    }

memset (&host, 0, sizeof host);
host.console_get = host_console_get;
host.console_put = host_console_put;
host.elapsed_usec = host_elapsed_usec;
host.message_file = stderr;
simh_shim_bind (&host);

if ((r = simh_shim_reset ()) != SCPE_OK) {
    fprintf (stderr, "reset failed: %s\n", simh_shim_status_text (r));
    return 1;
    }
fprintf (stderr, "%s, %ld MB, %ld instructions per batch\n",
         sim_name, megabytes, batch);
if (trace)
    trace_all_devices ();

/* Memory goes through the CPU's own modifier, which sizes the array and tells
   the memory controllers what they are answering for; writing the unit's
   capacity alone leaves them describing a machine that is not there. */
snprintf (memory, sizeof memory, "CPU %ldM", megabytes);
if ((r = simh_shim_set (memory)) != SCPE_OK) {
    fprintf (stderr, "cannot set %s: %s\n", memory, simh_shim_status_text (r));
    return 1;
    }

for (i = 0; i < nsettings; i++) {
    if ((r = simh_shim_set (settings[i])) != SCPE_OK) {
        fprintf (stderr, "cannot set %s: %s\n", settings[i], simh_shim_status_text (r));
        return 1;
        }
    }

for (i = 0; i < nattachments; i++) {
    char unit[64];
    const char *file = strchr (attachments[i], '=');

    if ((file == NULL) || ((size_t) (file - attachments[i]) >= sizeof unit)) {
        fprintf (stderr, "-a wants UNIT=file, got %s\n", attachments[i]);
        return 2;
        }
    memcpy (unit, attachments[i], (size_t) (file - attachments[i]));
    unit[file - attachments[i]] = 0;
    if ((r = simh_shim_attach (unit, file + 1)) != SCPE_OK) {
        fprintf (stderr, "cannot attach %s to %s: %s\n", file + 1, unit,
                 simh_shim_status_text (r));
        return 1;
        }
    fprintf (stderr, "attached %s to %s\n", file + 1, unit);
    }

if (image != NULL) {
    if ((r = load_cmd (0, image)) != SCPE_OK) {
        fprintf (stderr, "cannot load %s: %s\n", image, simh_shim_status_text (r));
        return 1;
        }
    PC = 0;
    fprintf (stderr, "loaded %s at 0, starting there\n", image);
    }

if (boot_unit != NULL) {
    if ((r = simh_shim_boot (boot_unit)) != SCPE_OK) {
        fprintf (stderr, "cannot boot %s: %s\n", boot_unit, simh_shim_status_text (r));
        return 1;
        }
    fprintf (stderr, "booting from %s\n", boot_unit);
    }

console_open ();
started = host_elapsed_usec (NULL);
for (;;) {
    r = simh_shim_run ((int32) batch);
    if (r != SCPE_STOP)                                 /* SCPE_STOP ends a batch */
        break;
    if ((limit_s > 0) &&
        ((host_elapsed_usec (NULL) - started) > (double) limit_s * 1000000.0)) {
        fprintf (stderr, "\ntime limit reached after %.0f instructions\n", sim_gtime ());
        r = SCPE_OK;
        break;
        }
    }
console_restore ();

fprintf (stderr, "\nstopped at PC %08x after %.0f instructions: %s\n",
         (unsigned) PC, sim_gtime (), (r == STOP_HALT) ? "halted" : simh_shim_status_text (r));

/* A program that reached HALT ran to its end, as did one the time limit
   stopped; anything else is a failure of the run. */
return ((SCPE_BARE_STATUS (r) == SCPE_OK) || (r == STOP_HALT)) ? 0 : 1;
}
