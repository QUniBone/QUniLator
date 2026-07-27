/*
    mscp_tape.hpp: TMSCP tape drive (TK50), backed by a SIMH .tap image.

    Copyright (c) 2026, Hans Huebner
    hans@huebner.org
    MIT license, see any device source header for the full text.

    A tape unit attached to a TMSCP controller (tqk50_c). It wraps a
    simh_tape_c record layer over a .tap image file, and exposes the
    record/space/write primitives the tape server drives. Unlike an MSCP
    disk it has no geometry or RCT area: a tape is a linear stream of
    records and tape marks.
*/

#pragma once

#include <stdint.h>
#include <string>

#include "parameter.hpp"
#include "storagedrive.hpp"
#include "simh_tape.hpp"

// TK50 media identifier and unit class/model reported to MSCP.
#define TK50_MEDIA_ID       0x6D68B032
#define TK50_CLASS_MODEL    0x0303          // UID_TAPE(3) << 8 | TQ5_UMOD(3)
// TK50 formatted capacity, informational only.
#define TK50_CAPACITY       (94ull * (1ull << 20))
// Logical end-of-tape threshold: the writable region a host may fill before the
// controller reports end of tape and turns around (an unbounded .tap would
// otherwise be written forever). The TK50's ~94 MB media; a faithful,
// per-drive-configurable capacity belongs on a drive parameter.
#define TAPE_LEOT_CAPACITY  TK50_CAPACITY

class mscp_tape_c : public storagedrive_c
{
public:
    mscp_tape_c(storagecontroller_c *controller, uint32_t driveNumber);
    ~mscp_tape_c(void) override;

    // A tape drives the frontend TapeWidget and carries removable media.
    const char *category(void) const override { return "tape"; }
    bool removable(void) const override { return true; }

    bool on_param_changed(parameter_c *param) override;

    // MSCP unit identity.
    uint32_t GetMediaID(void) { return TK50_MEDIA_ID; }
    uint16_t GetClassModel(void) { return TK50_CLASS_MODEL; }
    uint32_t GetDeviceNumber(void) { return _unitDeviceNumber; }

    // A tape is available once an image is mounted; online after ONLINE.
    bool IsAvailable(void) { return _tape.is_open(); }
    bool IsOnline(void) { return _online; }
    void SetOnline(void) { _online = true; }
    void SetOffline(void);

    // Write protection: the mounted image may be read-only (hardware), and
    // SET UNIT CHARACTERISTICS can add a software write lock.
    bool IsHardwareWriteProtected(void) { return _tape.is_open() && _tape.is_readonly(); }
    bool IsWriteProtected(void) { return _softwareWriteProtect || IsHardwareWriteProtected(); }
    void SetSoftwareWriteProtect(bool on) { _softwareWriteProtect = on; }

    // The record layer the tape server drives.
    simh_tape_c &tape(void) { return _tape; }

public:
    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;

private:
    simh_tape_c _tape;
    bool _online;
    bool _softwareWriteProtect;
    uint32_t _unitDeviceNumber;
};
