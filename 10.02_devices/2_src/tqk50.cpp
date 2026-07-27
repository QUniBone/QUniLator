/*
    tqk50.cpp: TMSCP tape controller (TQK50) implementation.

    Copyright (c) 2026, Hans Huebner
    hans@huebner.org
    MIT license, see tqk50.hpp for the full text.

    Supplies the tape-controller specifics on top of the generic MSCP port
    (mscp_port_c): the register/vector defaults, the controller-identity values
    reported by SET CONTROLLER CHARACTERISTICS and the fourth init step, the
    TMSCP tape server, and the tape drive objects.
*/

#include <string.h>

#include "logger.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "storagecontroller.hpp"
#include "mscp_tape.hpp"
#include "tqk50.hpp"

tqk50_c::tqk50_c() :
        mscp_port_c()
{
    name.value = "tqk50";
    type_name.value = "TQK50";
    type_name.readonly = true;
    base_addr.readonly = false;
    log_label = "tqk50";
    _22bitDMA = twenty_two_bit_DMA.value = (qunibus->addr_width == 22);

    // base addr, priority slot, intr-vector, intr level
    set_default_bus_params(0774500, TQK50_SLOT, 0260, 5);

    // The controller has two registers (IP @ base, SA @ base+2).
    register_count = 2;

    IP_reg = &(this->registers[0]); // @ base addr
    strcpy(IP_reg->name, "IP");
    IP_reg->active_on_dati = true;
    IP_reg->active_on_dato = true;
    IP_reg->reset_value = 0;
    IP_reg->writable_bits = 0xffff;

    SA_reg = &(this->registers[1]); // @ base addr + 2
    strcpy(SA_reg->name, "SA");
    SA_reg->active_on_dati = false;
    SA_reg->active_on_dato = true;
    SA_reg->reset_value = 0;
    SA_reg->writable_bits = 0xffff;

    _server.reset(make_server());

    // Initialize the tape drives.
    drivecount = TQK50_DRIVE_COUNT;
    for (uint32_t i = 0; i < drivecount; i++)
    {
        storagedrive_c *drive = make_drive(i);
        drive->unitno.value = i;
        drive->activity_led.value = i; // default: LED = unitno
        drive->name.value = name.value + std::to_string(i);
        drive->log_label = drive->name.value;
        drive->parent = this;
        storagedrives.push_back(drive);
    }
}

tqk50_c::~tqk50_c()
{
    for (uint32_t i = 0; i < drivecount; i++)
    {
        delete storagedrives[i];
    }

    storagedrives.clear();
}

bool tqk50_c::on_param_changed(parameter_c *param)
{
    return mscp_port_c::on_param_changed(param); // generic params + enable
}

//
// make_server():
//  Creates the TMSCP tape server for this controller.
//
mscp_server_base *tqk50_c::make_server(void)
{
    return new mscp_tape_server(this);
}

//
// make_drive():
//  Creates a TMSCP tape drive for the given unit.
//
storagedrive_c *tqk50_c::make_drive(unsigned unit)
{
    return new mscp_tape_c(this, unit);
}

//
// controller_identifier():
//  Returns the unique controller ID reported by SET CONTROLLER CHARACTERISTICS.
//
uint32_t tqk50_c::controller_identifier(void)
{
    return 0x54514B35;   // "TQK5", distinct from the disk controller's ID
}

//
// controller_class_model():
//  Class 1 (mass storage), model 9 (TQK50) reported by SET CONTROLLER
//  CHARACTERISTICS.
//
uint16_t tqk50_c::controller_class_model(void)
{
    return 0x0109;   // TQ_CLASS(1) << 8 | TQ5_CMOD(9)
}

//
// controller_microcode_id():
//  Control microcode version reported in the low byte of SA at the fourth
//  init step (the port ORs in the STEP4 bit).
//
uint16_t tqk50_c::controller_microcode_id(void)
{
    return 0x35;   // (uqpm 3 << 4) | (cver 5)
}

//
// step1_capability_flags():
//  The TQK50 advertises extended-diagnostics (SA_S1C_DI, 0x0100) and mapping
//  (SA_S1C_MP, 0x0040) support in its step-1 SA value; the TKxx diagnostics
//  require the controller to declare these to run their init/wrap tests.
//
uint16_t tqk50_c::step1_capability_flags(void)
{
    return 0x0140;   // SA_S1C_DI | SA_S1C_MP
}
