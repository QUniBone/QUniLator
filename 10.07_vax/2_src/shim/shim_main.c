/* shim_main.c: a workstation host for the shimmed VAX core
 *
 * The first embedding of the seam simh_shim.h defines, and the one that says on
 * the build machine whether the seam holds. It gives the core a console on the
 * terminal, the wall clock for its timing, and a main loop that runs the
 * processor in batches - which is the shape the CPU device of the QUniLator
 * application will have when the core moves onto the board.
 *
 *	vax780-shim [-m megabytes] [-b instructions] [-l seconds] [image]
 *
 * With an image it is loaded at address 0 and the processor started there; with
 * none the core comes up reset, which is enough to say the seam links and the
 * event queue turns. -l bounds the run so an unattended check terminates.
 */

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

static void console_raw (void)
{
struct termios raw;

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
    "usage: %s [-m megabytes] [-b instructions] [-l seconds] [image]\n"
    "  -m  memory size, default 8\n"
    "  -b  instructions per batch, default 100000\n"
    "  -l  stop after this many seconds of host time, default 0 for no limit\n",
    argv0);
}

int main (int argc, char *argv[])
{
simh_shim_host_t host;
const char *image = NULL;
long megabytes = 8;
long batch = 100000;
long limit_s = 0;
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

cpu_unit.capac = (t_addr) megabytes << 20;

if ((r = simh_shim_reset ()) != SCPE_OK) {
    fprintf (stderr, "reset failed: %s\n", simh_shim_status_text (r));
    return 1;
    }
fprintf (stderr, "%s, %ld MB, %ld instructions per batch\n",
         sim_name, megabytes, batch);

if (image != NULL) {
    if ((r = load_cmd (0, image)) != SCPE_OK) {
        fprintf (stderr, "cannot load %s: %s\n", image, simh_shim_status_text (r));
        return 1;
        }
    PC = 0;
    fprintf (stderr, "loaded %s at 0, starting there\n", image);
    }

console_raw ();
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
