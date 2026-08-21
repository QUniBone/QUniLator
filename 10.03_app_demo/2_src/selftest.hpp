/* selftest.hpp: run one hardware self-test without the menu

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   The web service starts "<name>-cli --selftest <test>" as a child and reads
   its verdict from the exit code; these are that contract.
*/
#ifndef _SELFTEST_HPP_
#define _SELFTEST_HPP_

// The test ran and every checked access was good. A run the operator stopped
// with zero errors is a pass: these are endurance tests, stopping is how they
// end.
#define SELFTEST_EXIT_PASS	0
// The test ran and found errors.
#define SELFTEST_EXIT_FAIL	1
// The test could not run: unknown test name, no address width, no memory to
// test, no panel fitted.
#define SELFTEST_EXIT_ERROR	2

// A line naming a likely cause, when the failure has a shape the test can
// recognise - the missing loopback jumpers, above all. The service lifts such
// a line out of the stream and shows it beside the verdict, so it is read
// rather than scrolled past; what follows the prefix to the end of the line is
// operator-facing text and nothing else.
#define SELFTEST_HINT_PREFIX	"HINT: "

#endif
