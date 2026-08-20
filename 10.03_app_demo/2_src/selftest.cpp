/* selftest.cpp: run one hardware self-test without the menu

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   Each test here is one entry of the interactive test menus (menu_buslatches,
   menu_qunibus_signals, menu_panel, menu_gpio, menu_masterslave), with the
   same hardware setup and teardown around it - so what a test needs and what
   it costs the machine is decided in one place, the menu code it mirrors.

   What is different from the menus is who is watching. The caller is the web
   service with a pipe on stdout and a kill(SIGINT) for a stop button, so:
   - stdout is unbuffered: the progress characters the test routines print
     would otherwise sit in a 4 KB block buffer until long after the operator
     stopped reading,
   - the --seconds bound is an alarm that sets the same SIGINTreceived flag the
     routines already poll for ^C,
   - the verdict is the exit code (selftest.hpp), because printed text is for
     the operator and the exit code is for the service.
*/

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <thread>

#include "utils.hpp"
#include "application.hpp"
#include "pru.hpp"

#include "gpios.hpp"
#include "buslatches.hpp"
#include "qunibussignals.hpp"
#include "panel.hpp"
#include "ddrmem.h"
#include "qunibusadapter.hpp"

#if defined(WEBUI)
#include "boardclaim.hpp"
#endif

#include "selftest.hpp"

// Distinguishes "the --seconds bound ended this step" from "the operator sent
// SIGINT": both set SIGINTreceived, but only a stepped test that saw the alarm
// goes on to its next step.
static volatile sig_atomic_t alarm_fired;

static void selftest_alarm_handler(int sig)
{
    UNUSED(sig);
    alarm_fired = 1;
    SIGINTreceived = 1;
}

// Arm the bound for one unbounded routine; 0 = run until SIGINT.
static void selftest_alarm(unsigned seconds)
{
    alarm_fired = 0;
    alarm(seconds);
}

// The operator (not the alarm) ended the last routine: everything stepped
// stops here instead of running its remaining steps.
static bool selftest_stopped_by_operator(void)
{
    return SIGINTreceived && !alarm_fired;
}

#if defined(WEBUI)
// The claim connection is the service's word that this process may drive the
// hardware. The service closing it - a restart, a crash - voids that word, and
// a test still driving the bus while a fresh service rebuilds its machine puts
// two masters on one set of latches. So the test stops as if the operator had.
static void selftest_claim_watchdog(void)
{
    int fd = boardclaim_fd();
    if (fd < 0)
        return; // no service was there to yield; nothing to watch
    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 1000) <= 0)
            continue;
        char scratch[64];
        if (read(fd, scratch, sizeof scratch) <= 0) {
            printf("\nThe service withdrew the board; stopping.\n");
            SIGINTreceived = 1;
            return;
        }
    }
}
#endif

// ---- reading a failure ---------------------------------------------------

// The grant chain leaves the board on one pin and comes back on another, so a
// latch test run without the loopback jumpers fails on exactly those bits and
// on nothing else. It is the commonest way to run these tests wrong, and what
// it leaves behind is a screenful of signal paths for the operator to read and
// draw the conclusion from - so draw it for them.
#if defined(UNIBUS)
static const unsigned grant_latch = 0;      // BG4567, NPG - the whole latch
static const uint8_t grant_mask = 0x1f;
static const char *grant_signals = "BG4, BG5, BG6, BG7 and NPG";
static const char *grant_jumpers = "the 5 loopback jumpers on BG4/5/6/7 and NPG (IN to OUT)";
#else
static const unsigned grant_latch = 6;      // IAKI/IAKO on .5, DMGI/DMGO on .6
static const uint8_t grant_mask = 0x60;
static const char *grant_signals = "IAKI/IAKO and DMGI/DMGO";
static const char *grant_jumpers = "the 2 loopback jumpers on IAKI-IAKO and DMGI-DMGO";
#endif

// error_masks is BUSLATCHES_COUNT bytes of "these bits of this latch failed".
// Only the clean signature is named: grant bits wrong and nothing else wrong.
// Errors elsewhere as well mean something the jumpers do not explain, and a
// wrong cause is worse than none.
static void selftest_diagnose_latches(const uint8_t *error_masks)
{
    if ((error_masks[grant_latch] & grant_mask) == 0)
        return; // the grant lines were not the problem
    for (unsigned i = 0; i < BUSLATCHES_COUNT; i++) {
        uint8_t other = (i == grant_latch) ? (error_masks[i] & ~grant_mask) : error_masks[i];
        if (other != 0)
            return; // something failed that the jumpers would not explain
    }
    printf(SELFTEST_HINT_PREFIX
            "every error was on the grant lines (%s) and on nothing else - which is "
            "what this test does when %s are not fitted. Fit them and run it again.\n",
            grant_signals, grant_jumpers);
}

// ---- the tests, one function per test id ---------------------------------

// menu "tl", commands <id> u/o/z/t/r: every latch through every ARM-driven
// pattern, the run's --seconds spread over the 32 steps. Errors do not stop
// the sweep: the point of the single-latch test is naming every bad wire, not
// the first one.
static int selftest_latch_single(unsigned seconds)
{
    unsigned failed_steps = 0;
    unsigned step_seconds = seconds ? (seconds + 31) / 32 : 1;
    uint8_t error_masks[BUSLATCHES_COUNT] = { 0 };

    for (unsigned pattern = 2; pattern <= 5; pattern++) {
        for (unsigned i = 0; i < 8; i++) {
            selftest_alarm(step_seconds);
            if (!buslatches.test_simple_pattern(pattern, buslatches[i], &error_masks[i]))
                failed_steps++;
            if (selftest_stopped_by_operator()) {
                printf("Stopped; %u of the 32 latch/pattern steps were run.\n",
                        pattern * 8 + i - 15);
                if (failed_steps)
                    selftest_diagnose_latches(error_masks);
                return failed_steps ? SELFTEST_EXIT_FAIL : SELFTEST_EXIT_PASS;
            }
        }
    }
    alarm(0);
    if (failed_steps) {
        printf("\n%u of 32 latch/pattern steps failed.\n", failed_steps);
        selftest_diagnose_latches(error_masks);
    }
    return failed_steps ? SELFTEST_EXIT_FAIL : SELFTEST_EXIT_PASS;
}

// menu "tl", command "* r": all 8 latches at once, random values, driven by
// the PRU exerciser. Runs through errors and reports the count.
static int selftest_latch_multi(unsigned seconds)
{
    uint8_t error_masks[BUSLATCHES_COUNT] = { 0 };
    selftest_alarm(seconds);
    uint64_t errors = buslatches.test_simple_pattern_multi(5, /*stop_on_error*/false,
            error_masks);
    if (errors)
        selftest_diagnose_latches(error_masks);
    return errors ? SELFTEST_EXIT_FAIL : SELFTEST_EXIT_PASS;
}

// menu "tl", command "t": PRU high-speed timing stress. Errors are signalled
// on PRU1.12 for a logic analyzer; nothing comes back to the ARM, so a
// completed run is "ran", never "passed".
static int selftest_latch_timing(unsigned seconds)
{
    selftest_alarm(seconds);
    buslatches.test_timing(0x55, 0xaa, 0x00, 0xff);
    printf("Timing stress ran; errors, if any, were signalled on PRU1.12 only.\n");
    return SELFTEST_EXIT_PASS;
}

#if defined(UNIBUS)
// menu "tl", command "gst".
static int selftest_m9302_sack(unsigned seconds)
{
    selftest_alarm(seconds);
    if (buslatches_m9302_sack_test())
        return SELFTEST_EXIT_PASS;
    // the mirror image of the latch tests: here the grants must reach the far
    // end of the bus, so the jumpers that make those tests pass make this one fail
    printf(SELFTEST_HINT_PREFIX
            "the M9302 answers SACK only when the grants reach it: check that an M9302 "
            "terminates the bus, and that the BG*/NPG loopback jumpers are removed.\n");
    return SELFTEST_EXIT_FAIL;
}
#endif

// The setup and teardown shared by every latch test above, from
// menu_buslatches entry/exit.
static int selftest_with_latches(unsigned seconds, int (*test)(unsigned))
{
    if (!qunibus->addr_width)
        qunibus->set_addr_width(22);
    app->hardware_startup(pru_c::PRUCODE_TEST);
    buslatches.output_enable(true);
    printf("*** Bus drivers are active: run only on an empty " QUNIBUS_NAME "!\n");

    int result = test(seconds);

    buslatches.output_enable(false);
    app->hardware_shutdown();
    return result;
}

// menu "bs", command "tp": all signals on, then each oscillated for the probe
// board's LEDs. Visual only; self-bounded at ~500 ms per signal.
static int selftest_probe_leds(unsigned seconds)
{
    UNUSED(seconds);
    if (!qunibus->addr_width)
        qunibus->set_addr_width(22);
    app->hardware_startup(pru_c::PRUCODE_TEST);
    qunibus_signals.reset(0);
    buslatches.output_enable(true);
    printf("*** Bus drivers are active: run only on an empty " QUNIBUS_NAME "!\n");
    printf("Watch the " QUNIBUS_PROBE_NAME " LEDs: all on, then one by one at half intensity.\n");

    qunibus_signals.reset(1);
    bool aborted = false;
    try {
        aborted = test_probe(/*timeout_ms*/500);
    } catch (std::exception &e) {
        aborted = true; // oscillate_bit throws on ^C
    }
    qunibus_signals.reset(0);
    printf(aborted ? "Stopped.\n" : "All signals were shown.\n");

    buslatches.output_enable(false);
    app->hardware_shutdown();
    return SELFTEST_EXIT_PASS; // the LEDs are the result; only eyes judge them
}

// menu "tp", command "tmo": light the panel lamps one by one. Needs a panel.
static int selftest_panel_lamps(unsigned seconds)
{
    UNUSED(seconds);
    paneldriver->reset();
    if (!paneldriver->present()) {
        printf("No panel: no MC23017 answers on I2C.\n");
        return SELFTEST_EXIT_ERROR;
    }
    printf("Watch the panel: each lamp lights for half a second.\n");
    paneldriver->test_moving_ones();
    paneldriver->enabled.set(false);
    printf("All lamps were driven.\n");
    return SELFTEST_EXIT_PASS;
}

// menu "tp", command "tlb": panel buttons drive their lamps until stopped.
static int selftest_panel_loopback(unsigned seconds)
{
    paneldriver->reset();
    if (!paneldriver->present()) {
        printf("No panel: no MC23017 answers on I2C.\n");
        return SELFTEST_EXIT_ERROR;
    }
    printf("Panel loopback: every switch drives its lamp. Stop when satisfied.\n");
    selftest_alarm(seconds);
    paneldriver->test_manual_loopback();
    paneldriver->enabled.set(false);
    return SELFTEST_EXIT_PASS;
}

// menu "tg", command "lb": the board's own switches drive its LEDs.
static int selftest_gpio_loopback(unsigned seconds)
{
    printf("GPIO loopback: the 4 switches drive the 4 LEDs, the button drives bus_enable.\n");
    printf("Stop when satisfied.\n");
    selftest_alarm(seconds);
    gpios->test_loopback();
    return SELFTEST_EXIT_PASS;
}

// ---- memory tests: menu "tm" without a menu ------------------------------

static int selftest_mem(const char *testname, unsigned seconds)
{
#if defined(QBUS)
    if (!qunibus->addr_width) {
        printf("Address width of the QBUS CPU is not set; give --addresswidth 16, 18 or 22.\n");
        return SELFTEST_EXIT_ERROR;
    }
#endif
    // menu_masterslave entry, the no-CPU path: QUniBone is sole bus master,
    // emulated memory stays off - the test exercises what the machine carries.
    app->emulated_memory_start_addr = 0x7fffffff;
    app->emulated_memory_end_addr = 0;
    app->hardware_startup(pru_c::PRUCODE_EMULATION);
    buslatches.output_enable(true);
    qunibus->set_cpu_bus_activity(0); // QBUS: even a HALTed CPU does ODT traffic
    qunibus->set_arbitrator_active(false);
    qunibusadapter->enabled.set(true);
    ddrmem->set_range(DDRMEM_RANGE_MEMORY, app->emulated_memory_start_addr,
            app->emulated_memory_end_addr);

    int result = SELFTEST_EXIT_PASS;
    bool no_grant = false;
    uint32_t first_invalid_addr = qunibus->test_sizer(&no_grant);
    if (no_grant) {
        printf("No bus grant: nothing arbitrates. Is a CPU or grant chain in the backplane?\n");
        result = SELFTEST_EXIT_ERROR;
    } else if (!strcmp(testname, "mem-sizer")) {
        if (first_invalid_addr == 0)
            printf("Address [0] invalid: the machine carries no memory at 0.\n");
        else
            printf("Found valid addresses in range 0..%s.\n",
                    qunibus->addr2text(first_invalid_addr - 2));
    } else if (first_invalid_addr < 2) {
        printf("No memory found at address 0: nothing to test.\n");
        result = SELFTEST_EXIT_ERROR;
    } else {
        uint32_t end_addr = first_invalid_addr - 2;
        unsigned mode = strcmp(testname, "mem-random") ? 1 : 2;
        printf("Testing 0..%s %s ...\n", qunibus->addr2text(end_addr),
                mode == 1 ? "linear with \"address\" data pattern" : "randomly");
        selftest_alarm(seconds);
        if (!qunibus->test_mem(0, end_addr, mode))
            result = SELFTEST_EXIT_FAIL;
    }

    qunibusadapter->enabled.set(false);
    qunibus->set_cpu_bus_activity(1);
    buslatches.output_enable(false);
    app->hardware_shutdown();
    return result;
}

// ---- dispatch ------------------------------------------------------------

int application_c::run_selftest(const std::string &testname, unsigned seconds)
{
    // The reader is a pipe: every progress character goes out as it is printed.
    setvbuf(stdout, nullptr, _IONBF, 0);
    // The service ignores SIGPIPE process-wide and dispositions survive exec;
    // with the reader gone this process must die, not print into the void.
    signal(SIGPIPE, SIG_DFL);
    signal(SIGALRM, selftest_alarm_handler);

#if defined(WEBUI)
    std::thread(selftest_claim_watchdog).detach();
#endif

    const char *name = testname.c_str();
    if (!strcmp(name, "latch-single"))
        return selftest_with_latches(seconds, selftest_latch_single);
    if (!strcmp(name, "latch-multi"))
        return selftest_with_latches(seconds, selftest_latch_multi);
    if (!strcmp(name, "latch-timing"))
        return selftest_with_latches(seconds, selftest_latch_timing);
#if defined(UNIBUS)
    if (!strcmp(name, "m9302-sack"))
        return selftest_with_latches(seconds, selftest_m9302_sack);
#endif
    if (!strcmp(name, "probe-leds"))
        return selftest_probe_leds(seconds);
    if (!strcmp(name, "panel-lamps"))
        return selftest_panel_lamps(seconds);
    if (!strcmp(name, "panel-loopback"))
        return selftest_panel_loopback(seconds);
    if (!strcmp(name, "gpio-loopback"))
        return selftest_gpio_loopback(seconds);
    if (!strcmp(name, "mem-sizer") || !strcmp(name, "mem-address")
            || !strcmp(name, "mem-random"))
        return selftest_mem(name, seconds);

    printf("Unknown self-test \"%s\". Tests: latch-single latch-multi latch-timing "
#if defined(UNIBUS)
            "m9302-sack "
#endif
            "probe-leds panel-lamps panel-loopback gpio-loopback "
            "mem-sizer mem-address mem-random\n", name);
    return SELFTEST_EXIT_ERROR;
}
