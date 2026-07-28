/* pru1_main_unibusint.c: the UNIBUS firmware with an internal bus

   The same firmware as pru1_main_unibus.c, built with NO_PHYSICAL_BUS so the
   bus latches stay inside the PRU and read back what was written instead of
   driving a backplane. A board loaded with this image is a machine by itself:
   its emulated CPU reaches its emulated devices, which a board driving a
   physical bus cannot do, being unable to answer its own cycle.

   Both images are built and carried in the emulator, which loads one of them
   according to the board's bus-mode setting, so one binary serves either use.
   The whole variant compiles into its own object directory, because the switch
   reaches every object that touches a bus latch, not this file alone.
*/
#include "pru1_main_unibus.c"
