/*
    tqk50.hpp: TMSCP tape controller (TQK50), on top of the generic MSCP port.

    Copyright (c) 2026, Hans Huebner
    hans@huebner.org
    MIT license, see any device source header for the full text.

    The TQK50 is the Q-bus TMSCP tape controller for the TK50 cartridge tape.
    It shares the whole MSCP transport (mscp_port_c) with the disk controller
    and supplies only the tape specifics: the register/vector defaults, the
    controller-identity values, the TMSCP tape server, and the tape drives.
*/

#pragma once

#include "mscp_port.hpp"
#include "mscp_server.hpp"
#include "mscp_tape.hpp"

// A free Q-bus interrupt slot (occupied: SLU1, LTC3, DZV11 4, DHV11 12,
// DEUNA 18, UDA50 20, KW11P 22).
#define TQK50_SLOT      24

// Number of tape units on the controller (SimH TQ_NUMDR).
#define TQK50_DRIVE_COUNT   4

class tqk50_c : public mscp_port_c
{
public:
    tqk50_c();
    virtual ~tqk50_c();

    bool on_param_changed(parameter_c *param) override;

    uint32_t controller_identifier(void) override;
    uint16_t controller_class_model(void) override;
    uint16_t controller_microcode_id(void) override;

protected:
    mscp_server_base *make_server(void) override;
    storagedrive_c *make_drive(unsigned unit) override;
};
