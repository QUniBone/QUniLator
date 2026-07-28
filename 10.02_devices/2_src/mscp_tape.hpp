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
// TK50 formatted capacity (the default of the drive's settable capacity param).
#define TK50_CAPACITY       (94ull * (1ull << 20))

class mscp_tape_c : public storagedrive_c
{
public:
    mscp_tape_c(storagecontroller_c *controller, uint32_t driveNumber);
    ~mscp_tape_c(void) override;

    // A tape drives the frontend TapeWidget and carries removable media.
    const char *category(void) const override { return "tape"; }
    bool removable(void) const override { return true; }

    bool on_param_changed(parameter_c *param) override;

    // Front-panel WRITE PROTECT lamp, lit while the cartridge is write-locked
    // (a read-only image or a software lock). Refreshed each lamp poll so the
    // dashboard tracks it; the ACCESS lamp (storagedrive_c) covers tape motion.
    parameter_bool_c write_protect_lamp = parameter_bool_c(this, "writeprotectlamp", "wpl",
                                          /*readonly*/ true, "State of WRITE PROTECT lamp");
    void refresh_activity(void) override;

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

    // Serious-exception latch: set when a write reaches end of tape. The host
    // must clear it (a command with the clear-serious-exception modifier)
    // before it can continue. Reported to the host as EF_SXC.
    void SetSeriousException(void) { _seriousException = true; }
    void ClearSeriousException(void) { _seriousException = false; }
    bool HasSeriousException(void) { return _seriousException; }

    // The written position has reached the drive capacity (end of tape).
    bool AtEndOfTape(void) {
        return _tape.is_open() && capacity.value != 0 &&
               _tape.position() >= capacity.value;
    }

    // Consume the one-shot end-of-tape report. Returns true exactly once per
    // crossing of the drive capacity -- on the first command (write, read or
    // access) that leaves the head past it -- and false thereafter until the
    // position returns below capacity (a rewind). The host expects the
    // "success, end of tape" completion (ST_SUC | SB_SUC_EOT) a single time: it
    // latches the event, and repeating it on the following commands re-arms the
    // stop logic and drives the DRS supervisor into a spin.
    bool TakeEndOfTapeReport(void) {
        if (AtEndOfTape()) {
            if (_leotReported)
                return false;
            _leotReported = true;
            return true;
        }
        _leotReported = false;
        return false;
    }

    // End-message flag bits reflecting the drive's exception state:
    // EF_SERIOUS_EXCEPTION (0x10) while latched, EF_END_OF_TAPE (0x08) at EOT.
    uint8_t EndFlags(void) {
        uint8_t f = 0;
        if (_seriousException) f |= 0x10;
        if (AtEndOfTape())     f |= 0x08;
        return f;
    }

    // The record layer the tape server drives.
    simh_tape_c &tape(void) { return _tape; }

public:
    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;

private:
    simh_tape_c _tape;
    bool _online;
    bool _softwareWriteProtect;
    bool _seriousException = false;
    bool _leotReported = false;
    uint32_t _unitDeviceNumber;
};
