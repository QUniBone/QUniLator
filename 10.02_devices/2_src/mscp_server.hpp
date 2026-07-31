/*
    mscp_server.hpp: Implementation of a simple MSCP server.

    Copyright Vulcan Inc. 2019 via Living Computers: Museum + Labs, Seattle, WA.
    Contributed under the BSD 2-clause license.
*/

#pragma once

#include <stdint.h>
#include <memory>

class mscp_port_c;
class Message;
class storagedrive_c;
class mscp_drive_c;
class mscp_tape_c;

// Builds a uint32_t containing the status, flags, and endcode for a response message,
// used to simplify returning the appropriate status bits from command functions.
// This looks like:
// 31           15           0
// |subcode|code|flags|unused|
// The upper 16 bits correspond to the subcode/code field returned in the MSCP end message.
//
#define STATUS(code, subcode, flags) ((flags) << 8) | ((((code) & 0x1f) | (((subcode) & 0x7ff) << 5)) << 16)

#define GET_STATUS(status) (((status) >> 16) & 0xffff)
#define GET_FLAGS(status) (((status) >> 8) & 0xff)

#define MAX_CREDITS 14
#define INIT_CREDITS 1

#define HEADER_OFFSET 4

//
// ControlMessageHeader encapsulates the standard MSCP control
// message header: a 12-byte header followed by up to 36 bytes of
// parameters.
// Note: This struct (and many others like it in this code) assumes
// little-endian byte orderings.  Probably not a big deal unless this
// someday runs on something other than a Beaglebone.
//
#pragma pack(push,1)
struct ControlMessageHeader
{
    uint32_t ReferenceNumber;
    uint16_t UnitNumber;
    uint16_t Reserved;

    union
    {
        struct
        {
            uint8_t Opcode : 8;
            uint8_t Reserved : 8;
            uint16_t Modifiers : 16;
        } Command;

        struct
        {
            uint8_t Endcode : 8;
            uint8_t Flags : 8;
            uint16_t Status : 16;
        } End;
    } Word3;

    // M9312 DU boot loader writes invalid big messages sizes.
    // Temporary patch: enlarge buffer behind all reasonable limits.
    // uint8_t Parameters[512];
    uint8_t Parameters[10240];	// overkill
};
#pragma pack(pop)

// Size in bytes of the non-parameter portion of a ControlMessageHeader
#define HEADER_SIZE  12

//
// mscp_server_base implements the transport-independent MSCP server:
// the polling thread, the command-ring pump, header framing, credits,
// response posting, and the generic (transport-level) commands.
// Command semantics for a particular medium are supplied by a subclass
// through dispatch_command().
//
// This inherits from device_c solely so the logging macros work.
//
class mscp_server_base : public device_c
{
protected:
	// enums scoped to the MSCP server
    enum Opcodes
    {
        ABORT = 0x1,
        ACCESS = 0x10,
        AVAILABLE = 0x8,
        COMPARE_HOST_DATA = 0x20,
        DETERMINE_ACCESS_PATHS = 0x0b,
        ERASE = 0x12,
        GET_COMMAND_STATUS = 0x2,
        GET_UNIT_STATUS = 0x3,
        ONLINE = 0x9,
        READ = 0x21,
        REPLACE = 0x14,
        SET_CONTROLLER_CHARACTERISTICS = 0x4,
        SET_UNIT_CHARACTERISTICS = 0xa,
        WRITE = 0x22
    };

    enum Endcodes
    {
        END = 0x80,
        SERIOUS_EXCEPTION = 0x7,
    };

    enum Status
    {
        SUCCESS = 0x0,
        INVALID_COMMAND = 0x1,
        COMMAND_ABORTED = 0x2,
        UNIT_OFFLINE = 0x3,
        UNIT_AVAILABLE = 0x4,
        MEDIA_FORMAT_ERROR = 0x5,
        WRITE_PROTECTED = 0x6,
        COMPARE_ERROR = 0x7,
        DATA_ERROR = 0x8,
        HOST_BUFFER_ACCESS_ERROR = 0x9,
        CONTROLLER_ERROR = 0xa,
        DRIVE_ERROR = 0xb,
        DIAGNOSTIC_MESSAGE = 0x1f
    };

    enum SuccessSubcodes
    {
        NORMAL = 0x0,
        SPIN_DOWN_IGNORED = 0x20,
        STILL_CONNECTED = 0x40,
        DUPLICATE_UNIT_NUMBER = 0x80,
        ALREADY_ONLINE = 0x100,
        STILL_ONLINE = 0x200,
    };

    // Unit flags, as carried in the second parameter word of GET UNIT STATUS,
    // ONLINE and SET UNIT CHARACTERISTICS end messages. Hosts steer on these:
    // the MXV11-B2 bootstrap keys its two DU boot flavors off the
    // removable-media bit alone.
    enum UnitFlags
    {
        REMOVABLE_MEDIA = 0x80,
    };

    enum UnitOfflineSubcodes
    {
        UNIT_UNKNOWN = 0x0,
        NO_VOLUME = 0x1,
        UNIT_INOPERATIVE = 0x2,
        DUPLICATE_UNIT = 0x4,
        UNIT_DISABLED = 0x8,
        EXCLUSIVE = 0x10,
    };

    enum HostBufferAccessSubcodes
    {
        CAUSE_UNAVAILABLE = 0x0,
        ODD_TRANSFER_ADDRESS = 0x1,
        ODD_BYTE_COUNT = 0x2,
        NXM = 0x3,
        HOST_PARITY_ERROR = 0x4,
        INVALID_PTE = 0x5,
        INVALID_BUFFER_NAME = 0x6,
        BUFFER_LENGTH_VIOLATION = 0x7,
        ACL_VIOLATION = 0x8,
    };

    enum MessageTypes
    {
        Sequential = 0,
        Datagram = 1,
        CreditNotification = 2,
        Maintenance = 15,
    };

public:
    mscp_server_base(mscp_port_c *port);
    virtual ~mscp_server_base();
    bool on_param_changed(parameter_c *param) override ;

public:
    void Reset(void);
    void InitPolling(void);
    void Poll(void);

private:
    //
    // Watch the command ring for a little while after a pass that did work.
    // True when the host handed another command over in that time.
    //
    bool LingerForNextCommand(void);
    void WaitForPollStateChange(unsigned timeout_us);

public:

public:
    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override {
        UNUSED(aclo_edge) ;
        UNUSED(dclo_edge) ;
    }
    void on_init_changed(void) override {}

protected:
    //
    // dispatch_command() executes a non-transport (medium-specific) command.
    // Returns the command status (as built by STATUS()).  *handled is set
    // false for an opcode the subclass does not implement, so the caller can
    // report an Invalid Command.
    //
    virtual uint32_t dispatch_command(
        uint8_t opcode,
        std::shared_ptr<Message> message,
        uint16_t unitNumber,
        uint16_t modifiers,
        bool *handled) = 0;

    //
    // reset_drives() releases every attached drive on controller reset.
    //
    virtual void reset_drives(void) = 0;

protected:
    uint32_t Abort(void);
    uint32_t GetCommandStatus(std::shared_ptr<Message> message);
    uint32_t SetControllerCharacteristics(std::shared_ptr<Message> message);

    uint8_t* GetParameterPointer(std::shared_ptr<Message> message);
    storagedrive_c* GetDrive(uint32_t unitNumber);

    void StartPollingThread(void);
    void AbortPollingThread(void);

protected:
    uint32_t _hostTimeout;
    uint32_t _controllerFlags;

    mscp_port_c* _port;

    enum PollingState
    {
        Wait = 0,
        Run,
        InitRestart,
        InitRun
    };

    bool _abort_polling;
    PollingState _pollState;

    pthread_t polling_pthread;
    pthread_cond_t polling_cond;
    pthread_mutex_t polling_mutex;

    // Credits available
    uint8_t _credits;
};

//
// mscp_disk_server implements the MSCP disk command set on top of the
// transport-independent mscp_server_base.
//
class mscp_disk_server : public mscp_server_base
{
public:
    mscp_disk_server(mscp_port_c *port);
    virtual ~mscp_disk_server();

protected:
    uint32_t dispatch_command(
        uint8_t opcode,
        std::shared_ptr<Message> message,
        uint16_t unitNumber,
        uint16_t modifiers,
        bool *handled) override;

    void reset_drives(void) override;

private:
    uint32_t Access(std::shared_ptr<Message> message, uint16_t unitNumber);
    uint32_t Available(uint16_t unitNumber, uint16_t modifiers);
    uint32_t CompareHostData(std::shared_ptr<Message> message, uint16_t unitNumber);
    uint32_t DetermineAccessPaths(uint16_t unitNumber);
    uint32_t Erase(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t GetUnitStatus(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t Online(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t SetUnitCharacteristics(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t Read(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t Replace(std::shared_ptr<Message> message, uint16_t unitNumber);
    uint32_t Write(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);

    uint32_t SetUnitCharacteristicsInternal(
        std::shared_ptr<Message> message,
        uint16_t unitNumber,
        uint16_t modifiers,
        bool bringOnline);
    uint32_t DoDiskTransfer(uint16_t operation, std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);

    mscp_drive_c* GetDiskDrive(uint32_t unitNumber);
};

// The disk server is the concrete MSCP server in use today.  Retain the old
// name so external references (device identification in the web layer) resolve.
using mscp_server = mscp_disk_server;

//
// mscp_tape_server implements the TMSCP tape command set (TQK50/TK50) on top
// of the transport-independent mscp_server_base. It drives a simh_tape_c record
// layer per unit rather than a block device: commands map onto reading, writing
// and spacing over records and tape marks.
//
class mscp_tape_server : public mscp_server_base
{
public:
    mscp_tape_server(mscp_port_c *port);
    virtual ~mscp_tape_server();

protected:
    uint32_t dispatch_command(
        uint8_t opcode,
        std::shared_ptr<Message> message,
        uint16_t unitNumber,
        uint16_t modifiers,
        bool *handled) override;

    void reset_drives(void) override;

private:
    // TMSCP-specific opcodes not carried in the shared Opcodes enum.
    enum TapeOpcodes
    {
        FLUSH = 0x13,           // 19: flush - nop
        ERASE_GAP = 0x16,       // 22: erase gap - nop
        WRITE_TAPE_MARK = 0x24, // 36
        REPOSITION = 0x25,      // 37
    };

    // TMSCP status codes (from pdp11_mscp.h) beyond the shared Status enum.
    enum TapeStatus
    {
        BOT = 13,               // ST_BOT: BOT encountered
        TAPE_MARK = 14,         // ST_TMK: tape mark encountered
        RECORD_TRUNCATED = 16,  // ST_RDT: record truncated
        POSITION_LOST = 17,     // ST_POL: position lost
        SERIOUS_EXCEPTION = 18, // ST_SXC: a serious exception is latched; motion
                                // commands are refused until the host clears it
        LEOT_DETECTED = 19,     // ST_LED: logical end of tape (a soft end the
                                // host turns around on, not a hard error)
    };

    // End flags packed into the response Flags byte.
    enum TapeEndFlags
    {
        EF_SERIOUS_EXCEPTION = 0x10,    // EF_SXC
        EF_END_OF_TAPE = 0x08,          // EF_EOT
        EF_POSITION_LOST = 0x04,        // EF_PLS
    };

    // Success subcode: end of tape encountered on a successful write. STATUS()
    // shifts this <<5, giving the status word 0x400 (02000 octal) = ST_SUC |
    // SB_SUC_EOT -- the "success, EOT encountered" completion a real TK50
    // returns. The TKxx data-reliability fill command watches for exactly this
    // status word to stop streaming records and turn around to read-back.
    enum TapeSuccessSubcodes
    {
        SB_SUC_EOT = 0x20,
    };

    // Write-protect status subcodes (raw, shifted <<5 by STATUS()).
    enum WriteProtectSubcodes
    {
        SW_WRITE_LOCK = 128,    // SB_WPR_SW
        HW_WRITE_LOCK = 256,    // SB_WPR_HW
    };

    // TK50 tape format and menu (TF_CTP | TF_CTP_LO).
    static const uint16_t TAPE_FORMAT = 0x0201;
    // Largest record the controller reports it can transfer.
    static const uint32_t MAX_RECORD_SIZE = 0x0000FFFF;

    uint32_t GetUnitStatus(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t Available(uint16_t unitNumber, std::shared_ptr<Message> message);
    uint32_t Online(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t SetUnitCharacteristics(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t Erase(std::shared_ptr<Message> message, uint16_t unitNumber);
    uint32_t Flush(std::shared_ptr<Message> message, uint16_t unitNumber);
    uint32_t EraseGap(std::shared_ptr<Message> message, uint16_t unitNumber);
    uint32_t Read(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t Access(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t Write(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);
    uint32_t WriteTapeMark(std::shared_ptr<Message> message, uint16_t unitNumber);
    uint32_t Reposition(std::shared_ptr<Message> message, uint16_t unitNumber, uint16_t modifiers);

    // ONLINE and SET UNIT CHARACTERISTICS share a status layout.
    uint32_t SetUnitCharacteristicsInternal(
        std::shared_ptr<Message> message,
        uint16_t unitNumber,
        uint16_t modifiers,
        bool bringOnline);

    mscp_tape_c *GetTapeDrive(uint32_t unitNumber);
};
