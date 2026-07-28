/* pru1_main_qbusint.c: the QBUS firmware with an internal bus

   The same firmware as pru1_main_qbus.c, built with NO_PHYSICAL_BUS so the bus
   latches stay inside the PRU and read back what was written instead of
   driving a backplane. See pru1_main_unibusint.c for the reasoning; the
   emulator carries both images and loads one according to the board's bus-mode
   setting.
*/
#include "pru1_main_qbus.c"
