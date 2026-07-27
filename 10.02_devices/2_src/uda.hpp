/*
    uda.hpp: MSCP controller port (UDA50)

    Copyright Vulcan Inc. 2019 via Living Computers: Museum + Labs, Seattle, WA.
    Contributed under the BSD 2-clause license.
*/

#pragma once

#include <memory>
#include "utils.hpp"
#include "mscp_port.hpp"
#include "mscp_drive.hpp"
#include "mscp_server.hpp"

// The number of drives supported by the controller.
// This is arbitrarily fixed at 8 but could be set to any
// value up to 65535.
#define DRIVE_COUNT 8

// The control/microcode version info returned by SA in the fourth initialization step.
#define UDA50_ID 0x0063
#define RQDX3_ID 0x0133

/*
  This implements a Qbus/Unibus MSCP disk controller (UDA50 / RQDX3) on top
  of the generic MSCP port.

  While the name "UDA" is used here, this is not a strict emulation
  of a real UDA50 -- it is a general MSCP implementation and can be
  thought of as the equivalent of the third-party MSCP controllers
  from Emulex, CMD, etc. that were available.
*/
class uda_c : public mscp_port_c
{
public:
    uda_c();
    virtual ~uda_c();

	bool on_param_changed(parameter_c *param) override;

    uint32_t controller_identifier(void) override;
    uint16_t controller_class_model(void) override;
    uint16_t controller_microcode_id(void) override;

protected:
    mscp_server_base *make_server(void) override;
    storagedrive_c *make_drive(unsigned unit) override;

private:
    enum ControllerType {
        UDA50 = 0,
        RQDX3 = 1,
    } _controllerType;
};
