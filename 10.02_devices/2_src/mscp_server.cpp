/*
    mscp_server.cpp: Implementation of a simple MSCP server.

    Copyright Vulcan Inc. 2019 via Living Computers: Museum + Labs, Seattle, WA.
    Contributed under the BSD 2-clause license.

    This provides an implementation of the Minimal MSCP subset outlined
    in AA-L619A-TK (Chapter 6).  It takes a few liberties and errs on
    the side of implementation simplicity.

    In particular:
         All commands are executed sequentially, as they appear in the
         command ring.  This includes any commands in the "Immediate"
         category.  Technically this is incorrect:  Immediate commands
         should execute as soon as possible, before any other commands.
         In practice I have yet to find code that cares.

         This simplifies the implementation significantly, and apart
         from maintaining fealty to the MSCP spec for Immediate commands,
         there's no good reason to make it more complex:  real MSCP
         controllers (like the original UDA50) would resequence commands
         to allow optimal throughput across multiple units, etc.  On the
         Unibone, the underlying storage and the execution speed of the
         processor is orders of magnitude faster, so even a brute-force
         braindead implementation like this can saturate the Unibus.

    TODO:
    - Some commands aren't checked as thoroughly for errors as they could be.
    - Not all Invalid Command responses include the subcode data (which should,
      per section 5.5 of the MSCP spec, be the byte offset of the offending data
      in the invalid message.)  This is only really useful for diagnostic purposes
      and so the lack of it should not normally cause issues.
    - Same for the "flag" field, this is entirely unpopulated.
*/
#include <assert.h>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <pthread.h>
#include <stdio.h>
#include <memory>
#include <queue>

#include "logger.hpp"
#include "timeout.hpp"
#include "utils.hpp"

#include "mscp_drive.hpp"
#include "mscp_tape.hpp"
#include "mscp_server.hpp"
#include "mscp_port.hpp"

//
// polling_worker():
//  Runs the main MSCP polling thread.
//
void* polling_worker(
    void *context)
{
    mscp_server_base* server = reinterpret_cast<mscp_server_base*>(context);
    server->Poll();
    return nullptr;
}

mscp_server_base::mscp_server_base(
    mscp_port_c *port) :
        device_c(),
        _hostTimeout(0),
        _controllerFlags(0),
        _abort_polling(false),
        _pollState(PollingState::Wait),
        polling_cond(PTHREAD_COND_INITIALIZER),
        polling_mutex(PTHREAD_MUTEX_INITIALIZER),
        _credits(INIT_CREDITS)
{
    set_workers_count(0) ; // no std worker()
    name.value = "mscp_server" ;
    type_name.value = "mscp_server_c" ;
    log_label = "MSSVR" ;
    // Alias the port pointer.  We do not own the port, we merely reference it.
    _port = port;

    enabled.set(true) ;
    enabled.readonly = true ; // always active
}


mscp_server_base::~mscp_server_base()
{
}


bool mscp_server_base::on_param_changed(parameter_c *param)
{
    // no own parameter or "enable" logic
    if (param == &enabled)
    {
        // accept, but do not react on enable/disable, always active
        return true ;
    }
    return device_c::on_param_changed(param) ; // more actions (for enable)
}



//
// StartPollingThread():
//  Initializes the MSCP polling thread and starts it running.
//
void
mscp_server_base::StartPollingThread(void)
{
    _abort_polling = false;
    _pollState = PollingState::Wait;

    //
    // Initialize the polling thread and start it.
    // It will wait to be woken to do actual work.
    //
    pthread_attr_t attribs;
    pthread_attr_init(&attribs);

    int status = pthread_create(
        &polling_pthread,
        &attribs,
        &polling_worker,
        reinterpret_cast<void*>(this));

    if (status != 0)
    {
        FATAL("Failed to start mscp server thread.  Status 0x%x", status);
    }

    DEBUG_FAST("Polling thread created.");
}

//
// AbortPollingThread():
//  Stops the MSCP polling thread.
//
void
mscp_server_base::AbortPollingThread(void)
{
    pthread_mutex_lock(&polling_mutex);
    _abort_polling = true;
    _pollState = PollingState::Wait;
    pthread_cond_signal(&polling_cond);
    pthread_mutex_unlock(&polling_mutex);

    pthread_cancel(polling_pthread);

    uint32_t status = pthread_join(polling_pthread, NULL);

    if (status != 0)
    {
        FATAL("Failed to join polling thread, status 0x%x", status);
    }

    DEBUG_FAST("Polling thread aborted.");
}

//
// LingerForNextCommand():
//  Watch the command ring for a little while after a pass that did work, and
//  say whether the host handed another command over.
//
//  The host is only obliged to ring the doorbell when it hands a command to a
//  controller that has stopped polling, and it takes the controller to be
//  polling still while it is answering. It writes the next command into the
//  ring as soon as it has read the response, which is after the controller has
//  come round again - so a controller that stops at the first empty read of the
//  ring leaves that command sitting there, and neither side moves.
//
//  The window is the time the host needs to see the response and answer it,
//  measured against the world while the host runs at whatever speed the
//  emulation manages - which varies by a factor of several when the processor
//  is being traced. So it is generous: a window too short deadlocks, while one
//  too long only costs a descriptor read every few milliseconds in an idle
//  controller.
//
#define POLL_LINGER_INTERVAL_US 5000
#define POLL_LINGER_TOTAL_US    3000000

bool
mscp_server_base::LingerForNextCommand(void)
{
    timeout_c timer;

    for (unsigned waited = 0; waited < POLL_LINGER_TOTAL_US;
         waited += POLL_LINGER_INTERVAL_US)
    {
        if (_abort_polling || _pollState != PollingState::Run)
        {
            return false;               // a reset or a doorbell decides instead
        }
        if (_port->CommandPending())
        {
            return true;
        }
        timer.wait_us(POLL_LINGER_INTERVAL_US);
    }
    return false;
}

//
// Poll():
//  The MSCP polling thread.
//  This thread waits to be awoken, then pulls messages from the MSCP command
//  ring and executes them.  When no work is left to be done, it goes back to
//  sleep.
//  This is awoken by a write to the UDA IP register.
//
void
mscp_server_base::Poll(void)
{
    worker_init_realtime_priority(rt_device);

    while(!_abort_polling)
    {
        //
        // Wait to be awoken, then pull commands from the command ring
        //
        pthread_mutex_lock(&polling_mutex);
        while (_pollState == PollingState::Wait)
        {
            pthread_cond_wait(
                &polling_cond,
                &polling_mutex);
        }

        // Shouldn't happen but if it does we just return to the top.
        if (_pollState == PollingState::InitRun)
        {
           _pollState = PollingState::Run;
        }

        pthread_mutex_unlock(&polling_mutex);

        if (_abort_polling)
        {
            break;
        }

        //
        // Read all commands from the ring into a queue; then execute them.
        //
        std::queue<std::shared_ptr<Message>> messages;

        int msgCount = 0;
        while (!_abort_polling && _pollState != PollingState::InitRestart)
        {
            bool error = false;
            std::shared_ptr<Message> message(_port->GetNextCommand(&error));
            if (error)
            {
                DEBUG_FAST("Error while reading messages, returning to idle state.");
                // The lords of STL decreed that queue should have no "clear" method
                // so we do this garbage instead:
                messages = std::queue<std::shared_ptr<Message>>();
                break;
            }
            if (nullptr == message)
            {
                DEBUG_FAST("End of command ring; %d messages to be executed.", msgCount);
                break;
            }

            msgCount++;
            messages.push(message);
        }

        //
        // Pull commands from the queue until it is empty or we're told to quit.
        //
        while(!messages.empty() && !_abort_polling && _pollState != PollingState::InitRestart)
        {
            std::shared_ptr<Message> message(messages.front());
            messages.pop();

            //
            // Handle the message.  We dispatch on opcodes to the
            // appropriate methods.  These methods modify the message
            // object in place; this message object is then posted back
            // to the response ring.
            //
            ControlMessageHeader* header =
                reinterpret_cast<ControlMessageHeader*>(message->Message);

            DEBUG_FAST("Message size 0x%x opcode 0x%x rsvd 0x%x mod 0x%x unit %d, ursvd 0x%x, ref 0x%x",
                message->MessageLength,
                header->Word3.Command.Opcode,
                header->Word3.Command.Reserved,
                header->Word3.Command.Modifiers,
                header->UnitNumber,
                header->Reserved,
                header->ReferenceNumber);

            bool protocolError = false;
            uint32_t cmdStatus = 0;
            uint16_t modifiers = header->Word3.Command.Modifiers;

            switch (header->Word3.Command.Opcode)
            {
                case Opcodes::ABORT:
                    cmdStatus = Abort();
                    break;

                case Opcodes::GET_COMMAND_STATUS:
                    cmdStatus = GetCommandStatus(message);
                    break;

                case Opcodes::SET_CONTROLLER_CHARACTERISTICS:
                    cmdStatus = SetControllerCharacteristics(message);
                    break;

                default:
                {
                    bool handled = false;
                    cmdStatus = dispatch_command(
                        header->Word3.Command.Opcode,
                        message,
                        header->UnitNumber,
                        modifiers,
                        &handled);

                    if (!handled)
                    {
                        DEBUG_FAST("Unimplemented MSCP command 0x%x", header->Word3.Command.Opcode);
                        protocolError = true;
                    }
                    break;
                }
            }

            if (protocolError)
            {
                uint16_t subCode = offsetof(ControlMessageHeader, Word3) + HEADER_OFFSET;
                cmdStatus = STATUS(Status::INVALID_COMMAND, subCode, 0);
            }

            DEBUG_FAST("cmd 0x%x st 0x%x fl 0x%x", cmdStatus, GET_STATUS(cmdStatus), GET_FLAGS(cmdStatus));

            //
            // Set the endcode and status bits
            //
            header->Word3.End.Status = GET_STATUS(cmdStatus);
            header->Word3.End.Flags = GET_FLAGS(cmdStatus);

            // Set the End code properly -- for a protocol error,
            // this is just the End code, for all others it's the End code
            // or'd with the original opcode.
            if (protocolError)
            {
                 // Just the END code, no opcode
                 header->Word3.End.Endcode = Endcodes::END;
            }
            else
            {
                 header->Word3.End.Endcode |= Endcodes::END;
            }

            if (message->Word1.Info.MessageType == MessageTypes::Sequential &&
                header->Word3.End.Endcode & Endcodes::END)
            {
                //
                // We steal the credits hack from simh:
                // The controller gives all of its credits to the host,
                // thereafter it supplies one credit for every response
                // packet sent.
                //
                uint8_t grantedCredits = std::min(_credits, static_cast<uint8_t>(MAX_CREDITS));
                _credits -= grantedCredits;
                message->Word1.Info.Credits = grantedCredits + 1;
                DEBUG_FAST("granted credits %d", grantedCredits + 1);
            }
            else
            {
                message->Word1.Info.Credits = 0;
            }

            //
            // Post the response to the port's response ring.
            // If everything is working properly, there should always be room.
            //
            if(!_port->PostResponse(message.get()))
            {
                FATAL("Unexpected: no room in response ring.");
            }

            //
            // Go around and pick up the next one.
            //
        }

        //
        // A host that has just been given a response is about to hand over the
        // next command, and it does not ring the doorbell to do it: as far as
        // it is concerned the controller is still polling. So a pass that did
        // work looks again for a while, and the poll ends only when nothing
        // more arrives. Done before the lock is taken, so a doorbell arriving
        // meanwhile is not held up behind it.
        //
        bool moreWork = (msgCount > 0) && LingerForNextCommand();

        //
        // Go back to sleep.  If a UDA reset is pending, we need to signal
        // the Reset() call so it knows we've completed our poll and are
        // returning to sleep (i.e. the polling thread is now reset.)
        //
        pthread_mutex_lock(&polling_mutex);
        if (_pollState == PollingState::InitRestart)
        {
            DEBUG_FAST("MSCP Polling thread reset.");
            // Signal the Reset call that we're done so it can return
            // and release the Host.
            _pollState = PollingState::Wait;
            pthread_cond_signal(&polling_cond);
        }
        else if (_pollState == PollingState::InitRun)
        {
            _pollState = PollingState::Run;
        }
        else if (moreWork)
        {
            DEBUG_FAST("Command arrived after the pass; polling again.");
            _pollState = PollingState::Run;
        }
        else
        {
            _pollState = PollingState::Wait;
        }
        pthread_mutex_unlock(&polling_mutex);

    }
    DEBUG_FAST("MSCP Polling thread exiting.");
}

//
// The following are the transport-level (medium-independent) MSCP commands.
//

uint32_t
mscp_server_base::Abort()
{
    INFO("MSCP ABORT");

    //
    // Since we do not reorder messages and in fact pick up and execute
    // them one at a time, sequentially as they appear in the ring buffer,
    // by the time we've gotten this command, the command it's referring
    // to is long gone.
    // This is semi-legal behavior and it's legal for us to ignore ABORT in this
    // case.
    //
    // We just return SUCCESS here.
    return STATUS(Status::SUCCESS, 0, 0);
}

uint32_t
mscp_server_base::GetCommandStatus(
    std::shared_ptr<Message> message)
{
    INFO("MSCP GET COMMAND STATUS");

    #pragma pack(push,1)
    struct GetCommandStatusResponseParameters
    {
        uint32_t OutstandingReferenceNumber;
        uint32_t CommandStatus;
    };
    #pragma pack(pop)

    message->MessageLength = sizeof(GetCommandStatusResponseParameters)
        + HEADER_SIZE;

    GetCommandStatusResponseParameters* params =
        reinterpret_cast<GetCommandStatusResponseParameters*>(
            GetParameterPointer(message));

    //
    // This will always return zero; as with the ABORT command, at this
    // point the command being referenced has already been executed.
    //
    params->CommandStatus = 0;

    return STATUS(Status::SUCCESS, 0, 0);
}

uint32_t
mscp_server_base::SetControllerCharacteristics(
    std::shared_ptr<Message> message)
{
    #pragma pack(push,1)
    struct SetControllerCharacteristicsParameters
    {
        uint16_t MSCPVersion;
        uint16_t ControllerFlags;
        uint16_t HostTimeout;
        uint16_t Reserved;
        union
        {
            uint64_t TimeAndDate;
            struct
            {
                uint32_t UniqueDeviceNumber;
                uint16_t Unused;
                uint16_t ClassModel;
            } ControllerId;
        } w;
    };
    #pragma pack(pop)

    SetControllerCharacteristicsParameters* params =
        reinterpret_cast<SetControllerCharacteristicsParameters*>(
            GetParameterPointer(message));

    DEBUG_FAST("MSCP SET CONTROLLER CHARACTERISTICS");

    // Adjust message length for response
    message->MessageLength = sizeof(SetControllerCharacteristicsParameters) +
        HEADER_SIZE;
    //
    // Check the version, if non-zero we must return an Invalid Command
    // end message.
    //
    if (params->MSCPVersion != 0)
    {
        return STATUS(Status::INVALID_COMMAND, 0, 0); // TODO: set sub-status
    }
    else
    {
        _hostTimeout = params->HostTimeout;
        _controllerFlags = params->ControllerFlags;

        // At this time we ignore the time and date entirely.

        // Prepare the response message
        params->Reserved = 0;
        params->ControllerFlags = _controllerFlags & 0xfe;  // Mask off 576 byte sectors bit.
                                                            // it's read-only and we're a 512
                                                            // byte sector shop here.
        params->HostTimeout = 0xff;   // Controller timeout: return the max value.
        params->w.ControllerId.UniqueDeviceNumber = _port->controller_identifier();
        params->w.ControllerId.ClassModel = _port->controller_class_model();
        params->w.ControllerId.Unused = 0;

        return STATUS(Status::SUCCESS, 0, 0);
    }

}

//
// GetParameterPointer():
//  Returns a pointer to the Parameter text in the given Message.
//
uint8_t*
mscp_server_base::GetParameterPointer(
    std::shared_ptr<Message> message)
{
    // We silence a strict aliasing warning here; this is safe (if perhaps not recommended
    // the general case.)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    return reinterpret_cast<ControlMessageHeader*>(message->Message)->Parameters;
#pragma GCC diagnostic pop
}

//
// GetDrive():
//  Returns the storagedrive_c object for the specified unit number,
//  or nullptr if no such object exists.
//
storagedrive_c*
mscp_server_base::GetDrive(
    uint32_t unitNumber)
{
    storagedrive_c* drive = nullptr;
    if (unitNumber < _port->GetDriveCount())
    {
        drive = _port->GetDrive(unitNumber);
    }

    return drive;
}

//
// Reset():
//  Resets the MSCP server:
//   - Waits for the polling thread to finish its current work
//   - Releases all drives into the Available state
//
void
mscp_server_base::Reset(void)
{
    DEBUG_FAST("Aborting polling due to reset.");

    pthread_mutex_lock(&polling_mutex);
    if (_pollState != PollingState::Wait)
    {
        _pollState = PollingState::InitRestart;

        while (_pollState != PollingState::Wait)
        {
            pthread_cond_wait(
                &polling_cond,
                &polling_mutex);
        }
    }
    pthread_mutex_unlock(&polling_mutex);

    _credits = INIT_CREDITS;

    // Release all drives
    reset_drives();
}

//
// InitPolling():
//  Wakes the polling thread.
//
void
mscp_server_base::InitPolling(void)
{
    //
    // Wake the polling thread if not already awoken.
    //
    pthread_mutex_lock(&polling_mutex);
        DEBUG_FAST("Waking polling thread.");
        _pollState = PollingState::InitRun;
       	pthread_cond_signal(&polling_cond);
    pthread_mutex_unlock(&polling_mutex);
}


//
// mscp_disk_server: the MSCP disk command set.
//

mscp_disk_server::mscp_disk_server(
    mscp_port_c *port) :
        mscp_server_base(port)
{
    StartPollingThread();
}

mscp_disk_server::~mscp_disk_server()
{
    AbortPollingThread();
}

//
// GetDiskDrive():
//  Returns the mscp_drive_c object for the specified unit number,
//  or nullptr if no such object exists.
//
mscp_drive_c*
mscp_disk_server::GetDiskDrive(
    uint32_t unitNumber)
{
    return dynamic_cast<mscp_drive_c*>(GetDrive(unitNumber));
}

//
// reset_drives():
//  Releases all attached drives into the Available state.
//
void
mscp_disk_server::reset_drives(void)
{
    for (uint32_t i=0;i<_port->GetDriveCount();i++)
    {
        GetDiskDrive(i)->SetOffline();
    }
}

//
// dispatch_command():
//  Executes a disk command, mapping the opcode to the appropriate handler.
//
uint32_t
mscp_disk_server::dispatch_command(
    uint8_t opcode,
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers,
    bool *handled)
{
    *handled = true;

    switch (opcode)
    {
        case Opcodes::ACCESS:
            return Access(message, unitNumber);

        case Opcodes::AVAILABLE:
            return Available(unitNumber, modifiers);

        case Opcodes::COMPARE_HOST_DATA:
            return CompareHostData(message, unitNumber);

        case Opcodes::DETERMINE_ACCESS_PATHS:
            return DetermineAccessPaths(unitNumber);

        case Opcodes::ERASE:
            return Erase(message, unitNumber, modifiers);

        case Opcodes::GET_UNIT_STATUS:
            return GetUnitStatus(message, unitNumber, modifiers);

        case Opcodes::ONLINE:
            return Online(message, unitNumber, modifiers);

        case Opcodes::READ:
            return Read(message, unitNumber, modifiers);

        case Opcodes::REPLACE:
            return Replace(message, unitNumber);

        case Opcodes::SET_UNIT_CHARACTERISTICS:
            return SetUnitCharacteristics(message, unitNumber, modifiers);

        case Opcodes::WRITE:
            return Write(message, unitNumber, modifiers);

        default:
            *handled = false;
            return 0;
    }
}

//
// The following are all implementations of the MSCP disk commands we support.
//

uint32_t
mscp_disk_server::Available(
    uint16_t unitNumber,
    uint16_t modifiers)
{
    UNUSED(modifiers);

    // Message has no message-specific data.
    // Just set the specified drive as Available if appropriate.
    // We do nothing with the spin-down modifier.
    DEBUG_FAST("MSCP AVAILABLE");

    mscp_drive_c* drive = GetDiskDrive(unitNumber);

    if (nullptr == drive ||
        !drive->IsAvailable())
    {
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);
    }

    drive->SetOffline();

    return STATUS(Status::SUCCESS, 0x40, 0);  // still connected
}

uint32_t
mscp_disk_server::Access(
    std::shared_ptr<Message> message,
    uint16_t unitNumber)
{
    INFO("MSCP ACCESS");

    return DoDiskTransfer(
        Opcodes::ACCESS,
        message,
        unitNumber,
        0);
}

uint32_t
mscp_disk_server::CompareHostData(
    std::shared_ptr<Message> message,
    uint16_t unitNumber)
{
    INFO("MSCP COMPARE HOST DATA");
    return DoDiskTransfer(
        Opcodes::COMPARE_HOST_DATA,
        message,
        unitNumber,
        0);
}

uint32_t
mscp_disk_server::DetermineAccessPaths(
    uint16_t unitNumber)
{
    DEBUG_FAST("MSCP DETERMINE ACCESS PATHS drive %d", unitNumber);

    // "This command must be treated as a no-op that always succeeds
    //  if the unit is incapable of being connected to more than one
    //  controller." That's us!

    mscp_drive_c* drive = GetDiskDrive(unitNumber);
    if (nullptr == drive ||
        !drive->IsAvailable())
    {
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);
    }
    else
    {
        return STATUS(Status::SUCCESS, 0, 0);
    }
}

uint32_t
mscp_disk_server::Erase(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    return DoDiskTransfer(
        Opcodes::ERASE,
        message,
        unitNumber,
        modifiers);
}

uint32_t
mscp_disk_server::GetUnitStatus(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct GetUnitStatusResponseParameters
    {
        uint16_t MultiUnitCode;
        uint16_t UnitFlags;
        uint32_t Reserved0;
        uint32_t UnitIdDeviceNumber;
        uint16_t UnitIdUnused;
        uint16_t UnitIdClassModel;
        uint32_t MediaTypeIdentifier;
        uint16_t ShadowUnit;
        uint16_t Reserved1;
        uint16_t TrackSize;
        uint16_t GroupSize;
        uint16_t CylinderSize;
        uint16_t Reserved2;
        uint16_t RCTSize;
        uint8_t RBNs;
        uint8_t Copies;
    };
    #pragma pack(pop)

    DEBUG_FAST("MSCP GET UNIT STATUS drive %d", unitNumber);

    // Adjust message length for response
    message->MessageLength = sizeof(GetUnitStatusResponseParameters) +
        HEADER_SIZE;

    ControlMessageHeader* header =
        reinterpret_cast<ControlMessageHeader*>(message->Message);

    if (modifiers & 0x1)
    {
        // Next Unit modifier: return the next known unit >= unitNumber.
        // Unless unitNumber is greater than the number of drives we support
        // we just return the unit specified by unitNumber.
        if (unitNumber >= _port->GetDriveCount())
        {
            // In this case we act as if drive 0 was queried.
            unitNumber = 0;
            header->UnitNumber = 0;
        }
    }

    mscp_drive_c* drive = GetDiskDrive(unitNumber);

    GetUnitStatusResponseParameters* params =
        reinterpret_cast<GetUnitStatusResponseParameters*>(
            GetParameterPointer(message));

    if (nullptr == drive || !drive->IsAvailable())
    {
        // No such drive or drive image not loaded.
        params->UnitIdDeviceNumber = 0;
        params->UnitIdClassModel = 0;
        params->UnitIdUnused = 0;
        params->ShadowUnit = 0;
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);
    }

    params->Reserved0 = 0;
    params->Reserved1 = 0;
    params->Reserved2 = 0;
    params->UnitFlags = 0;  // TODO: 0 for now, which is sane.
    params->MultiUnitCode = 0; // Controller dependent, we don't support multi-unit drives.
    params->UnitIdDeviceNumber = drive->GetDeviceNumber();
    params->UnitIdClassModel = drive->GetClassModel();
    params->UnitIdUnused = 0;
    params->MediaTypeIdentifier = drive->GetMediaID();
    params->ShadowUnit = unitNumber;   // Always equal to unit number

    // From the MSCP spec: "As stated above, the host area of  a  disk  is  structured  as  a
    //  vector of logical blocks.  From a performance viewpoint, however,
    //  it  is  more  appropriate  to  view  the  host  area  as  a  four
    //  dimensional hyper-cube."
    // This has nothing whatsoever to do with what's going on here but it makes me snicker
    // every time I read it so I'm including it.
    // Let's relay some information about our data-tesseract:
    // For older VMS, this has to match actual drive parameters.
    //
    params->TrackSize = drive->GetSectsPerTrack();
    params->GroupSize = drive->GetTracksPerGroup();
    params->CylinderSize = drive->GetGroupsPerCylinder();

    params->RCTSize = drive->GetRCTSize();
    params->RBNs = drive->GetRBNs();
    params->Copies = drive->GetRCTCopies();

    if (drive->IsOnline())
    {
        return STATUS(Status::SUCCESS, 0, 0);
    }
    else
    {
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);
    }
}

uint32_t
mscp_disk_server::Online(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct OnlineParameters
    {
        uint16_t UnitFlags alignas(2);
        uint16_t Reserved0 alignas(2);
        uint32_t Reserved1;
        uint32_t Reserved2;
        uint32_t Reserved3;
        uint32_t DeviceParameters;
        uint32_t Reserved4;
    };
    #pragma pack(pop)

    //
    // TODO: Right now, ignoring all incoming parameters.
    // With the exception of write-protection none of them really
    // apply.
    // We still need to flag errors if someone tries to set
    // host-settable flags we can't support.
    //

    // "The ONLINE command performs a SET UNIT CHARACTERISTICS
    // operation after bringing a unit 'Unit-Online'"
    return SetUnitCharacteristicsInternal(message, unitNumber, modifiers, true /*bring online*/);
}

uint32_t
mscp_disk_server::Replace(
    std::shared_ptr<Message> message,
    uint16_t unitNumber)
{
    INFO("MSCP REPLACE");
    //
    // We treat this as a success for valid units as we do no block replacement at all.
    // Best just to smile and nod.  We could be more vigilant and check LBNs, etc...
    //
    message->MessageLength = HEADER_SIZE;

    mscp_drive_c* drive = GetDiskDrive(unitNumber);

    if (nullptr == drive ||
        !drive->IsAvailable())
    {
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);
    }
    else
    {
        return STATUS(Status::SUCCESS, 0, 0);
    }
}

uint32_t
mscp_disk_server::SetUnitCharacteristics(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct SetUnitCharacteristicsParameters
    {
        uint16_t UnitFlags;
        uint16_t Reserved0;
        uint32_t Reserved1;
        uint64_t Reserved2;
        uint32_t DeviceDependent;
        uint16_t Reserved3;
        uint16_t Reserved4;
    };
    #pragma pack(pop)

    // TODO: handle Set Write Protect modifier

    DEBUG_FAST("MSCP SET UNIT CHARACTERISTICS drive %d", unitNumber);

    return SetUnitCharacteristicsInternal(message, unitNumber, modifiers, false);
}


uint32_t
mscp_disk_server::Read(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    return DoDiskTransfer(
        Opcodes::READ,
        message,
        unitNumber,
        modifiers);
}

uint32_t
mscp_disk_server::Write(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    return DoDiskTransfer(
        Opcodes::WRITE,
        message,
        unitNumber,
        modifiers);
}

//
// SetUnitCharacteristicsInternal():
//  Logic common to both ONLINE and SET UNIT CHARACTERISTICS commands.
//
uint32_t
mscp_disk_server::SetUnitCharacteristicsInternal(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers,
    bool bringOnline)
{
    UNUSED(modifiers);
    // TODO: handle Set Write Protect modifier

    #pragma pack(push,1)
    struct SetUnitCharacteristicsResponseParameters
    {
        uint16_t UnitFlags;
        uint16_t MultiUnitCode;
        uint32_t Reserved0;
        uint32_t UnitIdDeviceNumber;
        uint16_t UnitIdUnused;
        uint16_t UnitIdClassModel;
        uint32_t MediaTypeIdentifier;
        uint32_t Reserved1;
        uint32_t UnitSize;
        uint32_t VolumeSerialNumber;
    };
    #pragma pack(pop)

    // Adjust message length for response
    message->MessageLength = sizeof(SetUnitCharacteristicsResponseParameters) +
        HEADER_SIZE;

    mscp_drive_c* drive = GetDiskDrive(unitNumber);
    // Check unit
    if (nullptr == drive ||
        !drive->IsAvailable())
    {
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);
    }

    SetUnitCharacteristicsResponseParameters* params =
        reinterpret_cast<SetUnitCharacteristicsResponseParameters*>(
            GetParameterPointer(message));

    params->UnitFlags = 0;  // TODO: 0 for now, which is sane.
    params->MultiUnitCode = 0; // Controller dependent, we don't support multi-unit drives.
    params->UnitIdDeviceNumber = drive->GetDeviceNumber();
    params->UnitIdClassModel = drive->GetClassModel();
    params->UnitIdUnused = 0;
    params->MediaTypeIdentifier = drive->GetMediaID();
    params->UnitSize = drive->GetBlockCount();
    params->VolumeSerialNumber = 0;
    params->Reserved0 = 0;
    params->Reserved1 = 0;

    if (bringOnline)
    {
        bool alreadyOnline = drive->IsOnline();
        drive->SetOnline();
        return STATUS(Status::SUCCESS,
            (alreadyOnline ? SuccessSubcodes::ALREADY_ONLINE : SuccessSubcodes::NORMAL), 0);
    }
    else
    {
        return STATUS(Status::SUCCESS, 0, 0);
    }
}

//
// DoDiskTransfer():
//  Common transfer logic for READ, WRITE, ERASE, COMPARE HOST DATA and ACCCESS commands.
//
uint32_t
mscp_disk_server::DoDiskTransfer(
    uint16_t operation,
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct ReadWriteEraseParameters
    {
        uint32_t ByteCount;
        uint32_t BufferPhysicalAddress;  // upper 8 bits are channel address for VAXen
        uint32_t Unused0;
        uint32_t Unused1;
        uint32_t LBN;
    };
    #pragma pack(pop)

    ReadWriteEraseParameters* params =
        reinterpret_cast<ReadWriteEraseParameters*>(GetParameterPointer(message));

    DEBUG_FAST("MSCP RWE 0x%x unit %d mod 0x%x chan o%o pa o%o count %d lbn %d",
        operation,
        unitNumber,
        modifiers,
        params->BufferPhysicalAddress >> 24,
        params->BufferPhysicalAddress & 0x00ffffff,
        params->ByteCount,
        params->LBN);

    // Adjust message length for response
    message->MessageLength = sizeof(ReadWriteEraseParameters) +
        HEADER_SIZE;

    mscp_drive_c* drive = GetDiskDrive(unitNumber);

    // Check unit
    if (nullptr == drive ||
        !drive->IsAvailable())
    {
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);
    }

    if (!drive->IsOnline())
    {
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);
    }

    // Are we accessing the RCT area?
    bool rctAccess = params->LBN >= drive->GetBlockCount();
    uint32_t rctBlockNumber = params->LBN - drive->GetBlockCount();

    // Check that the LBN is valid
    if (params->LBN >= drive->GetBlockCount() + drive->GetRCTBlockCount())
    {
        uint16_t subCode = offsetof(ReadWriteEraseParameters, LBN) + HEADER_OFFSET;
        return STATUS(Status::INVALID_COMMAND, subCode, 0);
    }

    // Check byte count:
    if (params->ByteCount > ((drive->GetBlockCount() + drive->GetRCTBlockCount()) - params->LBN) * drive->GetBlockSize())
    {
        uint16_t subCode = offsetof(ReadWriteEraseParameters, ByteCount) + HEADER_OFFSET;
        return STATUS(Status::INVALID_COMMAND, subCode, 0);
    }

    // If this is an RCT access, byte count must equal the block size.
    if (rctAccess && params->ByteCount != drive->GetBlockSize())
    {
        uint16_t subCode = offsetof(ReadWriteEraseParameters, ByteCount) + HEADER_OFFSET;
        return STATUS(Status::INVALID_COMMAND, subCode, 0);
    }

    //
    // OK: do the transfer from the PDP-11 to a buffer
    //
    switch (operation)
    {
        case Opcodes::ACCESS:
            // We don't need to actually do any sort of transfer; ACCESS merely checks
            // That the data can be read -- we checked the LBN, etc. above and we
            // will never encounter a read error, so there's nothing left to do.
        break;

        case Opcodes::COMPARE_HOST_DATA:
        {
            // Read the data in from disk, read the data in from memory, and compare.
            std::unique_ptr<uint8_t> diskBuffer;

            if (rctAccess)
            {
                diskBuffer.reset(drive->ReadRCTBlock(rctBlockNumber));
            }
            else
            {
                diskBuffer.reset(drive->Read(params->LBN, params->ByteCount));
            }

            DMABufferPtr<uint8_t> memBuffer(_port->DMARead(
                params->BufferPhysicalAddress & 0x00ffffff,
                params->ByteCount,
                params->ByteCount));

            if (!memBuffer)
            {
                return STATUS(Status::HOST_BUFFER_ACCESS_ERROR, HostBufferAccessSubcodes::NXM, 0);
            }

            if (memcmp(diskBuffer.get(), memBuffer.get(), params->ByteCount))
            {
                // Host and disk data differ: report a compare error.
                return STATUS(Status::COMPARE_ERROR, 0, 0);
            }

            // Data matches: fall out to the common success path below.
        }
        break;

        case Opcodes::ERASE:
        {
            std::unique_ptr<uint8_t> memBuffer(new uint8_t[params->ByteCount]);
            memset(reinterpret_cast<void*>(memBuffer.get()), 0, params->ByteCount);

            if (rctAccess)
            {
                drive->WriteRCTBlock(rctBlockNumber,
                    memBuffer.get());
            }
            else
            {
                drive->Write(params->LBN,
                    params->ByteCount,
                    memBuffer.get());
            }
        }
        break;

        case Opcodes::READ:
        {
            std::unique_ptr<uint8_t> diskBuffer;

            if (rctAccess)
            {
                diskBuffer.reset(drive->ReadRCTBlock(rctBlockNumber));
            }
            else
            {
                diskBuffer.reset(drive->Read(params->LBN, params->ByteCount));
            }

            if (!_port->DMAWrite(
                params->BufferPhysicalAddress & 0x00ffffff,
                params->ByteCount,
                diskBuffer.get()))
            {
                return STATUS(Status::HOST_BUFFER_ACCESS_ERROR, HostBufferAccessSubcodes::NXM, 0);
            }

        }
        break;

        case Opcodes::WRITE:
        {
            DMABufferPtr<uint8_t> memBuffer(_port->DMARead(
                params->BufferPhysicalAddress & 0x00ffffff,
                params->ByteCount,
                params->ByteCount));

            if (!memBuffer)
            {
                return STATUS(Status::HOST_BUFFER_ACCESS_ERROR, HostBufferAccessSubcodes::NXM, 0);
            }

            if (rctAccess)
            {
                drive->WriteRCTBlock(rctBlockNumber,
                    memBuffer.get());
            }
            else
            {
                drive->Write(params->LBN,
                    params->ByteCount,
                    memBuffer.get());
            }
        }
        break;

        default:
            // Should never happen.
            assert(false);
            break;
    }

    // Set parameters for response.
    // We leave ByteCount as is (for now anyway)
    // And set First Bad Block to 0.  (This is unnecessary since we're
    // not reporting a bad block, but we're doing it for completeness.)
    params->LBN = 0;

    return STATUS(Status::SUCCESS, 0, 0);
}

//
// mscp_tape_server: the TMSCP tape command set (TQK50/TK50).
//
// Command modifiers used by the tape commands (from pdp11_mscp.h).
#define MD_TAPE_CSE     0x2000      // b: clear serious exception
#define MD_TAPE_REV     0x0008      // t rd, pos: reverse
#define MD_TAPE_SWP     0x0004      // suc: enable set write protect
#define MD_TAPE_OBC     0x0004      // pos: object count = tape marks
#define MD_TAPE_RWD     0x0002      // pos: rewind
#define MD_TAPE_NXU     0x0001      // gus: next unit

// Unit flags reported for a write-protected tape.
#define UF_TAPE_WPS     0x1000      // software write protect
#define UF_TAPE_WPH     0x2000      // hardware write protect

mscp_tape_server::mscp_tape_server(
    mscp_port_c *port) :
        mscp_server_base(port)
{
    StartPollingThread();
}

mscp_tape_server::~mscp_tape_server()
{
    AbortPollingThread();
}

//
// GetTapeDrive():
//  Returns the mscp_tape_c object for the specified unit number, or nullptr.
//
mscp_tape_c*
mscp_tape_server::GetTapeDrive(
    uint32_t unitNumber)
{
    return dynamic_cast<mscp_tape_c*>(GetDrive(unitNumber));
}

//
// reset_drives():
//  Takes every attached tape drive offline.
//
void
mscp_tape_server::reset_drives(void)
{
    for (uint32_t i = 0; i < _port->GetDriveCount(); i++)
    {
        mscp_tape_c *drive = GetTapeDrive(i);
        if (drive != nullptr)
            drive->SetOffline();
    }
}

//
// dispatch_command():
//  Maps a TMSCP tape opcode to its handler.
//
uint32_t
mscp_tape_server::dispatch_command(
    uint8_t opcode,
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers,
    bool *handled)
{
    *handled = true;

    // A command with the clear-serious-exception modifier clears a latched
    // exception so the host can continue. The clear happens first, so a command
    // carrying the modifier both clears the latch and executes.
    if (modifiers & MD_TAPE_CSE)
    {
        mscp_tape_c* d = GetTapeDrive(unitNumber);
        if (d != nullptr)
            d->ClearSeriousException();
    }

    // While a serious exception is latched (raised when a read/access crosses a
    // tape mark), a motion command is refused with ST_SXC until the host
    // acknowledges it with the clear-serious-exception modifier -- matching the
    // reference controller's tq_mot_valid. This is how the data reliability
    // test's read-back recognizes a tape mark it spaced over rather than
    // treating it as a data fault. Non-motion commands (status, online) are
    // unaffected so the host can still query the unit.
    switch (opcode)
    {
        case Opcodes::READ:
        case Opcodes::ACCESS:
        case Opcodes::WRITE:
        case TapeOpcodes::WRITE_TAPE_MARK:
        case TapeOpcodes::REPOSITION:
        case Opcodes::ERASE:
        case TapeOpcodes::ERASE_GAP:
        {
            mscp_tape_c* d = GetTapeDrive(unitNumber);
            if (d != nullptr && d->HasSeriousException())
                return STATUS(TapeStatus::SERIOUS_EXCEPTION, 0, d->EndFlags());
            // A motion or transfer command runs the tape: light the ACCESS lamp
            // so the dashboard shows activity. The lamp poll ages it back out.
            if (d != nullptr)
                d->set_activity_led(true);
            break;
        }
        default:
            break;
    }

    switch (opcode)
    {
        case Opcodes::GET_UNIT_STATUS:
            return GetUnitStatus(message, unitNumber, modifiers);

        case Opcodes::AVAILABLE:
            return Available(unitNumber, message);

        case Opcodes::ONLINE:
            return Online(message, unitNumber, modifiers);

        case Opcodes::SET_UNIT_CHARACTERISTICS:
            return SetUnitCharacteristics(message, unitNumber, modifiers);

        case Opcodes::ERASE:
            return Erase(message, unitNumber);

        case TapeOpcodes::FLUSH:
            return Flush(message, unitNumber);

        case TapeOpcodes::ERASE_GAP:
            return EraseGap(message, unitNumber);

        case Opcodes::READ:
            return Read(message, unitNumber, modifiers);

        case Opcodes::ACCESS:
            return Access(message, unitNumber, modifiers);

        case Opcodes::WRITE:
            return Write(message, unitNumber, modifiers);

        case TapeOpcodes::WRITE_TAPE_MARK:
            return WriteTapeMark(message, unitNumber);

        case TapeOpcodes::REPOSITION:
            return Reposition(message, unitNumber, modifiers);

        default:
            *handled = false;
            return 0;
    }
}

//
// GetUnitStatus():
//  Returns the tape unit's status block (GUS_LNT_T = 44 bytes).
//
uint32_t
mscp_tape_server::GetUnitStatus(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct GetUnitStatusResponseParameters
    {
        uint16_t MultiUnitCode;         // w8
        uint16_t UnitFlags;             // w9
        uint32_t Reserved0;             // w10-11
        uint32_t UnitIdDeviceNumber;    // w12-13
        uint16_t UnitIdUnused;          // w14
        uint16_t UnitIdClassModel;      // w15
        uint32_t MediaTypeIdentifier;   // w16-17
        uint16_t Format;                // w18
        uint16_t Speed;                 // w19
        uint16_t Menu;                  // w20
        uint16_t Capacity;              // w21
        uint16_t FormatterVersion;      // w22
        uint16_t UnitVersion;           // w23
    };
    #pragma pack(pop)

    DEBUG_FAST("TMSCP GET UNIT STATUS drive %d", unitNumber);

    message->MessageLength = sizeof(GetUnitStatusResponseParameters) + HEADER_SIZE;

    ControlMessageHeader* header =
        reinterpret_cast<ControlMessageHeader*>(message->Message);

    if (modifiers & MD_TAPE_NXU)
    {
        // Next Unit: return the requested unit, wrapping to 0 past the last.
        if (unitNumber >= _port->GetDriveCount())
        {
            unitNumber = 0;
            header->UnitNumber = 0;
        }
    }

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    GetUnitStatusResponseParameters* params =
        reinterpret_cast<GetUnitStatusResponseParameters*>(
            GetParameterPointer(message));

    memset(params, 0, sizeof(GetUnitStatusResponseParameters));

    if (nullptr == drive || !drive->IsAvailable())
    {
        // No such drive or no tape mounted: report offline with the UnitId zeroed.
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::NO_VOLUME, 0);
    }

    uint16_t unitFlags = 0;
    if (drive->IsHardwareWriteProtected())
        unitFlags |= UF_TAPE_WPH;
    if (drive->IsWriteProtected())
        unitFlags |= UF_TAPE_WPS;

    params->MultiUnitCode = 0;
    params->UnitFlags = unitFlags;
    params->UnitIdDeviceNumber = drive->GetDeviceNumber();
    params->UnitIdUnused = 0;
    params->UnitIdClassModel = drive->GetClassModel();
    params->MediaTypeIdentifier = drive->GetMediaID();
    params->Format = TAPE_FORMAT;
    params->Speed = 0;
    params->Menu = TAPE_FORMAT;
    params->Capacity = 0;
    params->FormatterVersion = 0;
    params->UnitVersion = 0;

    if (drive->IsOnline())
        return STATUS(Status::SUCCESS, 0, 0);
    else
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);
}

//
// Available():
//  Takes the unit offline (AVL_LNT = 12, header only).
//
uint32_t
mscp_tape_server::Available(
    uint16_t unitNumber,
    std::shared_ptr<Message> message)
{
    DEBUG_FAST("TMSCP AVAILABLE drive %d", unitNumber);

    message->MessageLength = HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    drive->SetOffline();

    return STATUS(Status::SUCCESS, 0, 0);
}

//
// Online():
//  Brings the unit online (ONL_LNT = 44), performing a SET UNIT CHARACTERISTICS.
//
uint32_t
mscp_tape_server::Online(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    DEBUG_FAST("TMSCP ONLINE drive %d", unitNumber);
    return SetUnitCharacteristicsInternal(message, unitNumber, modifiers, true);
}

//
// SetUnitCharacteristics():
//  Sets unit flags without changing the online state (SUC_LNT = 44).
//
uint32_t
mscp_tape_server::SetUnitCharacteristics(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    DEBUG_FAST("TMSCP SET UNIT CHARACTERISTICS drive %d", unitNumber);
    return SetUnitCharacteristicsInternal(message, unitNumber, modifiers, false);
}

//
// SetUnitCharacteristicsInternal():
//  Shared status layout for ONLINE and SET UNIT CHARACTERISTICS.
//
uint32_t
mscp_tape_server::SetUnitCharacteristicsInternal(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers,
    bool bringOnline)
{
    #pragma pack(push,1)
    struct OnlineResponseParameters
    {
        uint16_t MultiUnitCode;         // w8
        uint16_t UnitFlags;             // w9
        uint32_t Reserved0;             // w10-11
        uint32_t UnitIdDeviceNumber;    // w12-13
        uint16_t UnitIdUnused;          // w14
        uint16_t UnitIdClassModel;      // w15
        uint32_t MediaTypeIdentifier;   // w16-17
        uint16_t Format;                // w18
        uint16_t Speed;                 // w19
        uint32_t MaxRecordSize;         // w20-21
        uint16_t NoiseRecord;           // w22
        uint16_t Reserved1;             // w23
    };
    #pragma pack(pop)

    message->MessageLength = sizeof(OnlineResponseParameters) + HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    // The host may enable the software write lock through SET UNIT CHARACTERISTICS.
    if (modifiers & MD_TAPE_SWP)
        drive->SetSoftwareWriteProtect(true);

    OnlineResponseParameters* params =
        reinterpret_cast<OnlineResponseParameters*>(
            GetParameterPointer(message));

    uint16_t unitFlags = 0;
    if (drive->IsHardwareWriteProtected())
        unitFlags |= UF_TAPE_WPH;
    if (drive->IsWriteProtected())
        unitFlags |= UF_TAPE_WPS;

    params->MultiUnitCode = 0;
    params->UnitFlags = unitFlags;
    params->Reserved0 = 0;
    params->UnitIdDeviceNumber = drive->GetDeviceNumber();
    params->UnitIdUnused = 0;
    params->UnitIdClassModel = drive->GetClassModel();
    params->MediaTypeIdentifier = drive->GetMediaID();
    params->Format = TAPE_FORMAT;
    params->Speed = 0;
    params->MaxRecordSize = MAX_RECORD_SIZE;
    params->NoiseRecord = 0;
    params->Reserved1 = 0;

    if (bringOnline)
    {
        bool alreadyOnline = drive->IsOnline();
        drive->SetOnline();
        return STATUS(Status::SUCCESS,
            (alreadyOnline ? SuccessSubcodes::ALREADY_ONLINE : SuccessSubcodes::NORMAL), 0);
    }
    else
    {
        return STATUS(Status::SUCCESS, 0, 0);
    }
}

//
// Erase():
//  Data-security erase: writes a tape mark at the current position, which
//  truncates the medium after it (ERS_LNT = 12, header only).
//
uint32_t
mscp_tape_server::Erase(
    std::shared_ptr<Message> message,
    uint16_t unitNumber)
{
    DEBUG_FAST("TMSCP ERASE drive %d", unitNumber);

    message->MessageLength = HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    if (!drive->IsOnline())
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);

    if (drive->IsWriteProtected())
        return STATUS(Status::WRITE_PROTECTED,
            drive->IsHardwareWriteProtected() ? HW_WRITE_LOCK : SW_WRITE_LOCK,
            EF_SERIOUS_EXCEPTION);

    if (drive->tape().write_tape_mark() != simh_tape_c::R_RECORD)
        return STATUS(Status::DRIVE_ERROR, 0, 0);

    return STATUS(Status::SUCCESS, 0, 0);
}

//
// Flush():
//  No-op that succeeds, reporting the current position (FLU_LNT = 32).
//
uint32_t
mscp_tape_server::Flush(
    std::shared_ptr<Message> message,
    uint16_t unitNumber)
{
    #pragma pack(push,1)
    struct PositionStatus
    {
        uint32_t Reserved[2];   // w8-11
        uint32_t Reserved2[2];  // w12-15
        uint32_t Position;      // w16-17
    };
    #pragma pack(pop)

    DEBUG_FAST("TMSCP FLUSH drive %d", unitNumber);

    message->MessageLength = sizeof(PositionStatus) + HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    PositionStatus* params =
        reinterpret_cast<PositionStatus*>(GetParameterPointer(message));
    memset(params, 0, sizeof(PositionStatus));
    params->Position = static_cast<uint32_t>(drive->tape().object_position());

    return STATUS(Status::SUCCESS, 0, 0);
}

//
// EraseGap():
//  No-op that succeeds (ERG_LNT = 12, header only).
//
uint32_t
mscp_tape_server::EraseGap(
    std::shared_ptr<Message> message,
    uint16_t unitNumber)
{
    DEBUG_FAST("TMSCP ERASE GAP drive %d", unitNumber);

    message->MessageLength = HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    return STATUS(Status::SUCCESS, 0, 0);
}

//
// Read():
//  Reads one record (RW_LNT_T = 36). MD_REV reads the previous record.
//
uint32_t
mscp_tape_server::Read(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct ReadWriteParameters
    {
        uint32_t ByteCount;                 // w8-9
        uint32_t BufferPhysicalAddress;     // w10-11
        uint32_t Map;                       // w12-13
        uint32_t Reserved;                  // w14-15
        uint32_t Position;                  // w16-17
        uint32_t RecordSize;                // w18-19
    };
    #pragma pack(pop)

    ReadWriteParameters* params =
        reinterpret_cast<ReadWriteParameters*>(GetParameterPointer(message));

    uint32_t byteCount = params->ByteCount;
    uint32_t bufferAddress = params->BufferPhysicalAddress & 0x00ffffff;

    DEBUG_FAST("TMSCP READ drive %d mod 0x%x pa o%o count %d",
        unitNumber, modifiers, bufferAddress, byteCount);

    message->MessageLength = sizeof(ReadWriteParameters) + HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    if (!drive->IsOnline())
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);

    // Buffer large enough for the requested count, rounded up to a whole word.
    uint32_t bufSize = ((byteCount + 1) & ~1u);
    if (bufSize == 0)
        bufSize = 2;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[bufSize]);
    memset(buf.get(), 0, bufSize);

    uint32_t recLen = 0;
    simh_tape_c::result_t result;

    if (modifiers & MD_TAPE_REV)
    {
        // Reverse read: back up over the preceding record, then read it forward.
        uint32_t spanned = 0;
        simh_tape_c::result_t sres = drive->tape().space_reverse(&spanned);
        if (sres == simh_tape_c::R_BEGIN_OF_TAPE)
        {
            params->Position = static_cast<uint32_t>(drive->tape().object_position());
            params->RecordSize = 0;
            params->ByteCount = 0;
            return STATUS(BOT, 0, EF_POSITION_LOST);
        }
        if (sres == simh_tape_c::R_TAPE_MARK)
        {
            params->Position = static_cast<uint32_t>(drive->tape().object_position());
            params->RecordSize = 0;
            params->ByteCount = 0;
            drive->SetSeriousException();
            return STATUS(TAPE_MARK, 0, EF_SERIOUS_EXCEPTION);
        }
        if (sres != simh_tape_c::R_RECORD)
            return STATUS(Status::DRIVE_ERROR, 0, 0);

        result = drive->tape().read_forward(buf.get(), byteCount, &recLen);
    }
    else
    {
        result = drive->tape().read_forward(buf.get(), byteCount, &recLen);
    }

    params->Position = static_cast<uint32_t>(drive->tape().object_position());

    // The first command that leaves the head past the drive's EOT marker reports
    // "success, end of tape encountered" (ST_SUC | SB_SUC_EOT, status word
    // 02000). Reading back the last burst, the host watches the read/access
    // status for this value: it latches an end-of-tape flag from it and stops
    // spacing before the trailing tape mark. The report is one-shot -- repeating
    // it on the following commands re-arms the host stop logic and spins the DRS
    // supervisor -- so a single crossing yields a single 02000. The subcode
    // carries it (a bare EF_END_OF_TAPE flag is a different field the host's
    // test does not read).
    uint32_t successStatus = drive->TakeEndOfTapeReport()
        ? STATUS(Status::SUCCESS, SB_SUC_EOT, EF_END_OF_TAPE)
        : STATUS(Status::SUCCESS, 0, 0);

    switch (result)
    {
        case simh_tape_c::R_RECORD:
        {
            uint32_t xfer = std::min(recLen, byteCount);
            uint32_t dmaLen = ((xfer + 1) & ~1u);
            if (dmaLen > 0 && !_port->DMAWrite(bufferAddress, dmaLen, buf.get()))
            {
                params->RecordSize = recLen;
                params->ByteCount = 0;
                return STATUS(Status::HOST_BUFFER_ACCESS_ERROR, HostBufferAccessSubcodes::NXM, 0);
            }
            params->RecordSize = recLen;
            params->ByteCount = xfer;
            if (recLen > byteCount)
                return STATUS(RECORD_TRUNCATED, 0, 0);
            return successStatus;
        }

        case simh_tape_c::R_TAPE_MARK:
            params->RecordSize = 0;
            params->ByteCount = 0;
            drive->SetSeriousException();
            return STATUS(TAPE_MARK, 0, EF_SERIOUS_EXCEPTION);

        case simh_tape_c::R_END_OF_MEDIUM:
            params->RecordSize = 0;
            params->ByteCount = 0;
            return STATUS(Status::DATA_ERROR, 0, EF_END_OF_TAPE);

        case simh_tape_c::R_BEGIN_OF_TAPE:
            params->RecordSize = 0;
            params->ByteCount = 0;
            return STATUS(BOT, 0, EF_POSITION_LOST);

        default:
            params->RecordSize = 0;
            params->ByteCount = 0;
            return STATUS(Status::DRIVE_ERROR, 0, 0);
    }
}

//
// Access():
//  Positions over one record like a read, but transfers no data to the host
//  (the reported transfer count is zero). The TKxx data reliability test uses
//  it to walk records without reading them back into memory. Reverse access
//  spaces backward over the preceding record.
//
uint32_t
mscp_tape_server::Access(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct ReadWriteParameters
    {
        uint32_t ByteCount;                 // w8-9
        uint32_t BufferPhysicalAddress;     // w10-11
        uint32_t Map;                       // w12-13
        uint32_t Reserved;                  // w14-15
        uint32_t Position;                  // w16-17
        uint32_t RecordSize;                // w18-19
    };
    #pragma pack(pop)

    ReadWriteParameters* params =
        reinterpret_cast<ReadWriteParameters*>(GetParameterPointer(message));

    DEBUG_FAST("TMSCP ACCESS drive %d mod 0x%x", unitNumber, modifiers);

    message->MessageLength = sizeof(ReadWriteParameters) + HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    if (!drive->IsOnline())
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);

    uint32_t recLen = 0;
    simh_tape_c::result_t result = (modifiers & MD_TAPE_REV)
        ? drive->tape().space_reverse(&recLen)
        : drive->tape().space_forward(&recLen);

    params->Position = static_cast<uint32_t>(drive->tape().object_position());
    params->ByteCount = 0;      // access transfers no data

    // The first command past the EOT marker carries ST_SUC | SB_SUC_EOT (status
    // word 02000), reported once per crossing (see Read): the reliability test
    // spaces records with ACCESS and watches for it to recognise the end zone
    // and stop before it spaces onto the trailing tape mark.
    uint32_t successStatus = drive->TakeEndOfTapeReport()
        ? STATUS(Status::SUCCESS, SB_SUC_EOT, EF_END_OF_TAPE)
        : STATUS(Status::SUCCESS, 0, 0);

    switch (result)
    {
        case simh_tape_c::R_RECORD:
            params->RecordSize = recLen;
            return successStatus;

        case simh_tape_c::R_TAPE_MARK:
            params->RecordSize = 0;
            drive->SetSeriousException();
            return STATUS(TAPE_MARK, 0, EF_SERIOUS_EXCEPTION);

        case simh_tape_c::R_END_OF_MEDIUM:
            params->RecordSize = 0;
            return STATUS(Status::DATA_ERROR, 0, EF_END_OF_TAPE);

        case simh_tape_c::R_BEGIN_OF_TAPE:
            params->RecordSize = 0;
            return STATUS(BOT, 0, EF_POSITION_LOST);

        default:
            params->RecordSize = 0;
            return STATUS(Status::DRIVE_ERROR, 0, 0);
    }
}

//
// Write():
//  Writes one record (RW_LNT_T = 36).
//
uint32_t
mscp_tape_server::Write(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    UNUSED(modifiers);

    #pragma pack(push,1)
    struct ReadWriteParameters
    {
        uint32_t ByteCount;                 // w8-9
        uint32_t BufferPhysicalAddress;     // w10-11
        uint32_t Map;                       // w12-13
        uint32_t Reserved;                  // w14-15
        uint32_t Position;                  // w16-17
        uint32_t RecordSize;                // w18-19
    };
    #pragma pack(pop)

    ReadWriteParameters* params =
        reinterpret_cast<ReadWriteParameters*>(GetParameterPointer(message));

    uint32_t byteCount = params->ByteCount;
    uint32_t bufferAddress = params->BufferPhysicalAddress & 0x00ffffff;

    DEBUG_FAST("TMSCP WRITE drive %d pa o%o count %d",
        unitNumber, bufferAddress, byteCount);

    message->MessageLength = sizeof(ReadWriteParameters) + HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    if (!drive->IsOnline())
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);

    if (drive->IsWriteProtected())
        return STATUS(Status::WRITE_PROTECTED,
            drive->IsHardwareWriteProtected() ? HW_WRITE_LOCK : SW_WRITE_LOCK,
            EF_SERIOUS_EXCEPTION);

    if (byteCount == 0)
        return STATUS(Status::DRIVE_ERROR, 0, 0);

    uint32_t dmaLen = ((byteCount + 1) & ~1u);
    DMABufferPtr<uint8_t> mem(_port->DMARead(bufferAddress, dmaLen, dmaLen));

    if (!mem)
        return STATUS(Status::HOST_BUFFER_ACCESS_ERROR, HostBufferAccessSubcodes::NXM, 0);

    if (drive->tape().write_record(mem.get(), byteCount) != simh_tape_c::R_RECORD)
        return STATUS(Status::DRIVE_ERROR, 0, 0);

    params->Position = static_cast<uint32_t>(drive->tape().object_position());
    params->RecordSize = byteCount;

    // The first record written at or past the drive capacity is stored and
    // succeeds, and its end packet reports "success, end of tape encountered"
    // (ST_SUC | SB_SUC_EOT, status word 02000). A host that streams records
    // until end of medium (the TKxx data reliability fill commands) watches for
    // this exact status to stop issuing writes and turn around to read back.
    // It is reported ONCE per crossing (shared with read-back, see Read): the
    // status sets the host's stop flag, and a repeat on a following command
    // re-arms that flag and spins the DRS supervisor. Writes already queued
    // behind the crossing complete as plain success so the in-flight burst
    // drains. Count-bounded writes complete before capacity and never report it;
    // the latch clears once the position is back below capacity (after a rewind).
    if (drive->TakeEndOfTapeReport())
        return STATUS(Status::SUCCESS, SB_SUC_EOT, EF_END_OF_TAPE);

    return STATUS(Status::SUCCESS, 0, 0);
}

//
// WriteTapeMark():
//  Writes a tape mark at the current position (WTM_LNT = 32).
//
uint32_t
mscp_tape_server::WriteTapeMark(
    std::shared_ptr<Message> message,
    uint16_t unitNumber)
{
    #pragma pack(push,1)
    struct PositionStatus
    {
        uint32_t Reserved[2];   // w8-11
        uint32_t Reserved2[2];  // w12-15
        uint32_t Position;      // w16-17
    };
    #pragma pack(pop)

    DEBUG_FAST("TMSCP WRITE TAPE MARK drive %d", unitNumber);

    message->MessageLength = sizeof(PositionStatus) + HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    if (!drive->IsOnline())
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);

    if (drive->IsWriteProtected())
        return STATUS(Status::WRITE_PROTECTED,
            drive->IsHardwareWriteProtected() ? HW_WRITE_LOCK : SW_WRITE_LOCK,
            EF_SERIOUS_EXCEPTION);

    PositionStatus* params =
        reinterpret_cast<PositionStatus*>(GetParameterPointer(message));
    memset(params, 0, sizeof(PositionStatus));

    if (drive->tape().write_tape_mark() != simh_tape_c::R_RECORD)
        return STATUS(Status::DRIVE_ERROR, 0, 0);

    params->Position = static_cast<uint32_t>(drive->tape().object_position());

    return STATUS(Status::SUCCESS, 0, 0);
}

//
// Reposition():
//  Rewinds, or spaces over a count of records or tape marks (POS_LNT = 32).
//
uint32_t
mscp_tape_server::Reposition(
    std::shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct RepositionParameters
    {
        uint32_t RecordCount;       // w8-9
        uint32_t TapeMarkCount;     // w10-11
        uint32_t Reserved0;         // w12-13
        uint32_t Reserved1;         // w14-15
        uint32_t Position;          // w16-17
    };
    #pragma pack(pop)

    RepositionParameters* params =
        reinterpret_cast<RepositionParameters*>(GetParameterPointer(message));

    uint32_t recordCount = params->RecordCount;
    uint32_t tapeMarkCount = params->TapeMarkCount;

    DEBUG_FAST("TMSCP REPOSITION drive %d mod 0x%x rec %d tm %d",
        unitNumber, modifiers, recordCount, tapeMarkCount);

    message->MessageLength = sizeof(RepositionParameters) + HEADER_SIZE;

    mscp_tape_c* drive = GetTapeDrive(unitNumber);

    if (nullptr == drive || !drive->IsAvailable())
        return STATUS(Status::UNIT_OFFLINE, UnitOfflineSubcodes::UNIT_UNKNOWN, 0);

    if (!drive->IsOnline())
        return STATUS(Status::UNIT_AVAILABLE, 0, 0);

    if (modifiers & MD_TAPE_RWD)
    {
        drive->tape().rewind();
        drive->TakeEndOfTapeReport();   // back at BOT: clear the EOT one-shot
        params->RecordCount = 0;
        params->TapeMarkCount = 0;
        params->Position = 0;
        return STATUS(Status::SUCCESS, 0, 0);
    }

    bool reverse = (modifiers & MD_TAPE_REV) != 0;

    // MD_TAPE_OBC selects object mode: the record-count field is a count of
    // objects -- records AND tape marks alike -- so a tape mark counts toward
    // the target and does not stop the motion. Without it, the command spaces
    // over that many records (a tape mark ends the motion) or, when no record
    // count is given, over that many tape marks.
    bool objectMode = (modifiers & MD_TAPE_OBC) != 0;
    bool spacingRecords = objectMode || (recordCount != 0);
    uint32_t target = spacingRecords ? recordCount : tapeMarkCount;

    uint32_t recordsSkipped = 0;
    uint32_t tapeMarksSkipped = 0;
    uint32_t done = 0;
    const uint32_t plainSuccess = STATUS(Status::SUCCESS, 0, 0);
    uint32_t status = plainSuccess;

    while (done < target)
    {
        uint32_t spanned = 0;
        simh_tape_c::result_t r = reverse
            ? drive->tape().space_reverse(&spanned)
            : drive->tape().space_forward(&spanned);

        if (r == simh_tape_c::R_RECORD)
        {
            recordsSkipped++;
            if (spacingRecords)
                done++;
        }
        else if (r == simh_tape_c::R_TAPE_MARK)
        {
            tapeMarksSkipped++;
            if (objectMode || !spacingRecords)
                done++;     // a tape mark is an object, and it is what tape-mark
                            // spacing counts
            else
            {
                // Spacing records (not objects) terminates at a tape-mark boundary.
                status = STATUS(TAPE_MARK, 0, 0);
                break;
            }
        }
        else if (r == simh_tape_c::R_BEGIN_OF_TAPE)
        {
            status = STATUS(BOT, 0, 0);
            break;
        }
        else if (r == simh_tape_c::R_END_OF_MEDIUM)
        {
            status = STATUS(Status::SUCCESS, 0, EF_END_OF_TAPE);
            break;
        }
        else
        {
            status = STATUS(Status::DRIVE_ERROR, 0, 0);
            break;
        }
    }

    params->RecordCount = recordsSkipped;
    params->TapeMarkCount = tapeMarksSkipped;
    params->Position = static_cast<uint32_t>(drive->tape().object_position());

    // A plain-success reposition that left the head past the EOT marker reports
    // ST_SUC | SB_SUC_EOT (status word 02000) once per crossing, the same
    // end-of-tape indication the read and access commands carry there (see Read).
    if (status == plainSuccess && drive->TakeEndOfTapeReport())
        status = STATUS(Status::SUCCESS, SB_SUC_EOT, EF_END_OF_TAPE);

    return status;
}
