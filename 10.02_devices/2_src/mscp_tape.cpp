/*
    mscp_tape.cpp: TMSCP tape drive (TK50) implementation.

    Copyright (c) 2026, Hans Huebner
    hans@huebner.org
    MIT license, see mscp_tape.hpp for the full text.

    The drive mounts a SIMH .tap image through simh_tape_c: setting the "image"
    parameter opens the tape, clearing it detaches. Everything the controller
    needs (read/space/write of records and tape marks) is served by the wrapped
    record layer; this class only tracks the mount, the online state, and the
    software write lock.
*/

#include "logger.hpp"
#include "utils.hpp"
#include "mscp_tape.hpp"

mscp_tape_c::mscp_tape_c(storagecontroller_c *ctrl, uint32_t driveNumber) :
    storagedrive_c(ctrl),
    _online(false),
    _softwareWriteProtect(false)
{
    set_workers_count(0);   // needs no worker()
    log_label = "MSCPT";

    type_name.value = "TK50";
    type_name.readonly = true;

    // TK50 formatted capacity (informational).
    capacity.value = TK50_CAPACITY;

    // MSCP unit device number: one-based, matching the disk convention.
    _unitDeviceNumber = driveNumber + 1;

    SetOffline();
}

mscp_tape_c::~mscp_tape_c(void)
{
    _tape.close();
}

//
// on_param_changed():
//  The "image" parameter names the .tap file. Setting it (re)mounts the tape;
//  clearing it detaches. A read/write mount creates the file if absent, and
//  falls back to a read-only mount when it cannot be written.
//
bool mscp_tape_c::on_param_changed(parameter_c *param)
{
    if (param == &image_filepath) {
        _tape.close();
        SetOffline();
        _softwareWriteProtect = false;

        const std::string &path = image_filepath.new_value;
        if (!path.empty() && !_tape.open(path, false)) {
            ERROR("mscp_tape: cannot open tape image '%s'", path.c_str());
            return false;
        }
        return true;
    }

    return device_c::on_param_changed(param);
}

//
// SetOffline():
//  Takes the unit out of the online state and re-opens configuration so the
//  image can be swapped.
//
void mscp_tape_c::SetOffline(void)
{
    _online = false;
    image_params_readonly(false);
}

//
// on_power_changed() / on_init_changed():
//  A bus reset returns the unit to the offline state, as on real hardware.
//
void mscp_tape_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
    UNUSED(aclo_edge);
    UNUSED(dclo_edge);
    SetOffline();
}

void mscp_tape_c::on_init_changed(void)
{
    SetOffline();
}
