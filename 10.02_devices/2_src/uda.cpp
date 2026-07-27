/*
    uda.cpp: Implementation of a Qbus/Unibus MSCP disk controller (UDA50 / RQDX3).

    Copyright Vulcan Inc. 2019 via Living Computers: Museum + Labs, Seattle, WA.
    Contributed under the BSD 2-clause license.

    This supplies the disk-controller specifics on top of the generic MSCP
    port (mscp_port_c): the register/vector defaults, the controller-identity
    values, the MSCP disk server, and the disk drive objects.

    While the name "UDA" is used here, this is not a strict emulation
    of a real UDA50 -- it is a general MSCP implementation and can be
    thought of as the equivalent of the third-party MSCP controllers
    from Emulex, CMD, etc. that were available.
*/

#include <string.h>
#include <strings.h>

#include "logger.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "storagecontroller.hpp"
#include "mscp_drive.hpp"
#include "uda.hpp"

uda_c::uda_c() :
        mscp_port_c(),
        _controllerType(UDA50)
{
    name.value = "uda";
    type_name.value = "UDA50";
    type_name.readonly = false;
    base_addr.readonly = false;
    log_label = "uda";
    _22bitDMA = twenty_two_bit_DMA.value = (qunibus->addr_width == 22);

    // base addr, intr-vector, intr level
    set_default_bus_params(0772150, 20, 0154, 5) ;

    // The UDA50 controller has two registers.
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

    //
    // Initialize drives.  We support up to eight attached drives.
    //
    drivecount = DRIVE_COUNT;
    for (uint32_t i=0; i<drivecount; i++)
    {
        storagedrive_c *drive = make_drive(i);
        drive->unitno.value = i;
        drive->activity_led.value = i ; // default: LED = unitno
        drive->name.value = name.value + std::to_string(i);
        drive->log_label = drive->name.value;
        drive->parent = this;
        storagedrives.push_back(drive);
    }
}

uda_c::~uda_c()
{
    for(uint32_t i=0; i<drivecount; i++)
    {
        delete storagedrives[i];
    }

    storagedrives.clear();
}

bool uda_c::on_param_changed(parameter_c *param)
{
    if (param == &type_name)
    {
        if (strcasecmp("uda50", type_name.new_value.c_str()) == 0)
        {
            _controllerType = UDA50;
        }
        else if (strcasecmp("rqdx3", type_name.new_value.c_str()) == 0)
        {
            _controllerType = RQDX3;
        }
        else
        {
            return false;
        }
    }

    return mscp_port_c::on_param_changed(param) ; // generic params + enable
}

//
// make_server():
//  Creates the MSCP disk server for this controller.
//
mscp_server_base *uda_c::make_server(void)
{
    return new mscp_disk_server(this);
}

//
// make_drive():
//  Creates an MSCP disk drive for the given unit.
//
storagedrive_c *uda_c::make_drive(unsigned unit)
{
    return new mscp_drive_c(this, unit);
}

//
// controller_identifier():
//  Returns the ID used by SET CONTROLLER CHARACTERISTICS.
//
uint32_t
uda_c::controller_identifier(void)
{
    // TODO: make this not hardcoded
    // ID 0x12345678
    return 0x12345678;
}

//
// controller_class_model():
//  Returns the Class and Model information used by SET CONTROLLER CHARACTERISTICS.
//
uint16_t
uda_c::controller_class_model(void)
{
    return 0x0102;   // Class 1 (mass storage), model 2 (UDA50)
}

//
// controller_microcode_id():
//  Returns the control microcode version reported by SA in the fourth init step.
//
uint16_t
uda_c::controller_microcode_id(void)
{
    return _controllerType == UDA50 ? UDA50_ID : RQDX3_ID;
}
