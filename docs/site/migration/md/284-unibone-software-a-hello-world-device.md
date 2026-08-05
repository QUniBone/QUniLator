<!-- migrated from https://retrocmp.com/projects/unibone/284-unibone-software-a-hello-world-device -->

To give an idea how it feels to implement an own UNIBUS device, here is a small "Hello World"-like device: "demo_io".

## demo_io function

UniBone has 4 red LEDs, 4 switches and one button.

"demo_io" simply presents the switch state in an UNIBUS I/O page register,  and controls the LEDs with a 2nd one.

![leds switches](images/leds-switches.jpg)

UNIBUS address map is as follows:

Switch register "SR": 160100\
Display register "DR": 160102

The switches & LEDs are connected to regular BeagleBone GPIO pins, and are controlled via the Debian pseudo-files in folder "/sys/class/gpio". The web is full of explanations about this interface, for example see [here](http://www.armhf.com/using-beaglebone-black-gpios/).

## How to program

The demo_io device is implemented as a C++ class. It inherits a lot from the base class "unibusdevice_c", so we only have two files to edit: "demo_io.hpp" and "demo_io.cpp".

Download them from the attachement, we will discuss them now.

## GPIO access

First, demo_io has some functions to access GPIO pins.

    fstream gpio_inputs[5]; // 4 switches, 1 button
    fstream gpio_outputs[4]; // 4 LEDs

    void gpio_open(fstream& value_stream, bool is_input, unsigned gpio_number);
    unsigned gpio_get_input(unsigned input_index);
    void gpio_set_output(unsigned output_index, unsigned value); 

After initialization, each GPIO pin is accessed by read or writing a pseudo file, with an path like "/sys/class/gpio33/value". So we have 4 open file streams for the LEDs and 5 file streams for switches. gpio_open() initializes this. gpio_get_input() and ... \_set_output() then work with the pin levels.

## Setup device properties

The device must be configured, then "registered" at the central "unibusadapter" object.

This is done in the device constructor "demo_io_c()":

    name.value = "DEMO_IO";
    type_name.value = "demo_io_c";
    log_label = "di";
    default_base_addr = 0760100; // overwritten in install()?
    default_intr_vector = 0;
    default_intr_level = 0;

As demo_io_c is derived from class unibusdevice_c, registration is done automatically.

## Setup parameters

demo_io_c has also one "parameter". A parameter is a property visible to the users, and controllable over separate panels. For the demo we have "switch_feedback", which allows to hard-write the LED state to the switch states, overriding any UNIBUS action. It is given in the class definition in demo_io.hpp:

    deviceparameter_bool_c switch_feedback = deviceparameter_bool_c(this, "switch_feedback", "sf",/*readonly*/
        false, "1 = hard wire Switches to LEDs, PDP-11 can not set LEDs");

Again its automatically registered behind the curtain.

## Setup UNIBUS registers

The only non-trivial part is to register some UNIBUS addresses as device registers. This tells the PRU what to do on DATI/DATO to these.

    register_count = 2 ;

    switch_reg = &(this->registers[0]); // @  base addr
    strcpy(switch_reg->name, "SR"); // "Switches and Display"
    switch_reg->active_on_dati = false; // no controller state change
    switch_reg->active_on_dato = false;
    switch_reg->reset_value = 0;
    switch_reg->writable_bits = 0x0000; // read only

    display_reg = &(this->registers[1]); // @  base addr + 2
    strcpy(display_reg->name, "DR"); // "Switches and Display"
    display_reg->active_on_dati = false; // no controller state change
    display_reg->active_on_dato = false;
    display_reg->reset_value = 0;
    display_reg->writable_bits = 0x000f; // not necessary

An device registers is an object of type "unibusdevice_register_t". 

Remember the way registers accesses are forwarded from PRU to Linux processes via interrupts, halting the current UNBUS cycle?

Register access can be a costly operation. So theres the concept of "active" and "passive" registers:

- a "passive" register is simply like a memory cell, not generating any interrupts on UNIBUS DATI or DATO cycles.
- "active" registers causes an PRU-to-ARM interupt and call of the device's "on_after_register_access()" callback.

When modeling a device, care must be taken to make all registers with frequent PDP-11 access "passive". A typical device has at least a "command" and a "status" register. PDP-11 software writes to "Command", then polls "Status" until some "completion" bit is set. So the "Command" register *must* be "active" on write (DATO), as it changes the working state of the device. The "Status" register *must not* be active on read (DATI), else the PDP-11's high-speed polling will flood the BeagleBone's ARM CPU with callback executions, the system will (almost) halt.

For demo_io, we just poll slowly the content of both UNIBUS registers, asynchronously to the PDP-11s actions. All accesses are "passive".

As time goes on, more and more basic register intelligence will be moved to the PRUs, to reduce the need for "active" registers. They already can write protect bits in a registers, and they could automatically clear certain bits in a register after read.

## We have work to do

The actual operation of a device is done in certain callbacks (none of which is used here), and in the "worker()" thread.

worker() must not be free running, it should wait for signals or timeouts, to periodically update the device state.

"demo_io" has no internal state ... it just connects UNIBUS register bits with BeagleBone GPIOs.

First switch GPIO levels are fetched, encoded into one 16 bit word and the UNIBUS "switch register" is updated.

         for (i = 0; i < 5; i++) {
             register_bitmask = (1 << i);
             if (gpio_get_input(i) != 0)
                register_value |= register_bitmask;
         }
         set_register_dati_value(switch_reg, register_value, __func__);

 Then bits from the display register control the LED GPIOs. If parameter "switch_feedback" is set, the previous switch state is used instead to drive the LEDs.

         if (! switch_feedback.value)
                register_value = get_register_dato_value(display_reg);
         for (i = 0; i < 4; i++) {
             register_bitmask = (1 << i);
             gpio_set_output(i, register_value & register_bitmask);
         }

 That's all to do for a simple device!

## Integration

To actually have a living "demo_io" device, you must integrate it in some Linux application or a system service.

Use it the following way:

    ...
    #include "demo_io.hpp"
    ...
    main() 
    {
        demo_io_c demo_io;
        demo_io.install();
        demo_io.worker_start();
        ... all the interesting stuff ....
        demo_io.worker_stop();
        demo_io.uninstall() ;
    }

 

To compile your application, just add "demo_io.hpp" and "demo_io.cpp" to the "10.02_devices/2_src" directory und update the "Makefile".

\* \* \*

You now can control the LEDs by manually DEPOSITing something into address 160102 over the front panel of your '11. Or read switches with EXAM 160100.

Or you access "demo_io" with a running PDP-11. The MACRO-11 program in the attachement updates LEDs from Switches. If the button is pressed, it returns to the console monitor in a PDP-11/34.

 

## Conclusion

As a typical "Hello world" program, "demo_io" is hiding more than it explains. However it shows how to make BeagleBone peripherals visible in the UNIBUS address space.

If you feel not challenged enough, check out the RL11 and RL02 devices.

An I²C thermometer is left as exercise to the reader!

 

[demo_io.hpp --](/attachments/article/284/demo_io.hpp)

[demo_io.cpp --](/attachments/article/284/demo_io.cpp)

[demoio.lst --](/attachments/article/284/demoio.lst)
