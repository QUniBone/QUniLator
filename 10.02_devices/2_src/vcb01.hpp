/* vcb01.hpp: VCB01 (M7602) QVSS Qbus video subsystem

   Copyright (c) 2026, Hans Huebner

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Emulation of the DEC VCB01 monochrome video subsystem, 1024 x 864 on a
   Q22 bus. Register semantics follow appendix C of AZ-GLFAB-MN (VAXstation II
   Technical Manual); the SIMH vax_vc module served as the reference for much
   of the behaviour.

   The controller occupies 32 words of the I/O page:

     base+00        control and status
     base+02        cursor X
     base+04        mouse position
     base+10        CRTC address pointer, with blanking status
     base+12        CRTC data, addressing 18 six-bit-eight-bit registers
     base+14        interrupt controller data
     base+16        interrupt controller command and status
     base+40..66    SCN2681 DUART, keyboard on A and pointer on B

   Its 256 KB of video memory is served straight out of the emulated machine's
   DDR by the PRU, so bus cycles to the bitmap cost the host nothing. The
   refresh worker reads that memory, renders what changed, and pushes it to an
   X server; see vcb01_render.hpp for the layout it reads.

   Where video memory answers is a switch on the board rather than something
   software places: CSR bits 11..14 report it and reject writes.
*/
#ifndef _VCB01_HPP_
#define _VCB01_HPP_

#include <stdint.h>
#include <pthread.h>
#include <string>

#include "parameter.hpp"
#include "qunibusdevice.hpp"
#include "vcb01_render.hpp"
#include "vcb01_input.hpp"
#include "x11display.hpp"

class vcb01_c: public qunibusdevice_c {
public:
    vcb01_c();
    virtual ~vcb01_c();

    const char *category(void) const override { return "video"; }

    bool on_param_changed(parameter_c *param) override;
    void worker(unsigned instance) override;
    void on_after_register_access(qunibusdevice_register_t *device_reg,
            uint8_t unibus_control, DATO_ACCESS access) override;
    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;

    // The register file is only addressable between install and uninstall, so
    // the screen is brought up and taken down around them.
    bool on_before_install(void) override;
    void on_after_install(void) override;
    void on_after_uninstall(void) override;

    // Where the X server is, as a display name. Resolved to a literal address
    // before Xlib sees it, so a host name works from a headless board.
    parameter_string_c display = parameter_string_c(this, "display", "disp", false,
            "X display carrying the screen, e.g. \"host:0\"");

    // The board's memory bank switch, CSR bits 11..14: video memory answers
    // at bank * 256 KB. 016 puts it at 16000000.
    parameter_unsigned_c bank = parameter_unsigned_c(this, "bank", "bk", false, "",
            "%o", "video memory bank, 256 KB each: address = bank * 1000000", 4, 8);

    parameter_unsigned_c refresh_rate = parameter_unsigned_c(this, "refreshrate", "rr", false,
            "Hz", "%d", "screen updates per second", 8, 10);

    // A monitor big enough to show all 864 lines, reported in CSR bit 0.
    parameter_bool_c large_monitor = parameter_bool_c(this, "largemonitor", "lm", false,
            "report a 19-inch monitor rather than a 15-inch one");

    intr_request_c intr_request{this};

private:
    // --- CSR (base+00) ---
    static const uint16_t CSR_MOD = 0x0001;     // monitor size [RO]
    static const uint16_t CSR_VID = 0x0004;     // video output enable [RW]
    static const uint16_t CSR_FNC = 0x0008;     // cursor function, 1 = OR [RW]
    static const uint16_t CSR_VRB = 0x0010;     // video readback enable [RW]
    static const uint16_t CSR_TST = 0x0020;     // test [RW]
    static const uint16_t CSR_IEN = 0x0040;     // interrupt enable [RW]
    static const uint16_t CSR_CUR = 0x0080;     // cursor active [RO]
    static const uint16_t CSR_MSA = 0x0100;     // pointer button A [RO]
    static const uint16_t CSR_MSB = 0x0200;     // pointer button B [RO]
    static const uint16_t CSR_MSC = 0x0400;     // pointer button C [RO]
    static const unsigned CSR_V_MA = 11;        // memory bank switch [RO]
    static const uint16_t CSR_M_MA = 0x7800;

    static const uint16_t CSR_RW_MASK = CSR_VID | CSR_FNC | CSR_VRB | CSR_TST | CSR_IEN;

    // --- CRTC address pointer (base+10) ---
    static const uint16_t CRTCP_REG = 0x001F;   // which CRTC register [RW]
    static const uint16_t CRTCP_VB = 0x0020;    // vertical blanking [RO]
    static const uint16_t CRTCP_LPF = 0x0040;   // light pen register full [RO]
    static const uint16_t CRTCP_US = 0x0080;    // update strobe [RO]

    // --- the 6845's own registers, reached through the pointer ---
    static const unsigned CRTC_VDSP = 6;        // vertical displayed, char rows
    static const unsigned CRTC_MSCN = 9;        // maximum scan line
    static const unsigned CRTC_CSCS = 10;       // cursor scan start
    static const unsigned CRTC_CAH = 14;        // cursor address high
    static const unsigned CRTC_SIZE = 18;

    // --- interrupt controller sources, each with its own vector ---
    static const unsigned IRQ_DUART = 0;
    static const unsigned IRQ_VSYNC = 1;
    static const unsigned IRQ_MOUSE = 2;
    static const unsigned IRQ_CSTRT = 3;
    static const unsigned IRQ_MBA = 4;
    static const unsigned IRQ_MBB = 5;
    static const unsigned IRQ_MBC = 6;
    static const unsigned IRQ_COUNT = 8;

    // The controller's mode register preselects which of its registers the
    // data port reads, in bits 5..6.
    static const uint8_t ICM_V_RP = 5;
    static const uint8_t ICM_M_RP = 0x60;
    static const uint8_t ICM_MM = 0x80;         // master mask

    // register file
    qunibusdevice_register_t *CSR_reg;
    qunibusdevice_register_t *CURX_reg;
    qunibusdevice_register_t *MPOS_reg;    // QBone extension: absolute pointer X + valid bit
    qunibusdevice_register_t *MPOSY_reg;   // QBone extension: absolute pointer Y
    qunibusdevice_register_t *CRTCP_reg;
    qunibusdevice_register_t *CRTCD_reg;
    qunibusdevice_register_t *ICDR_reg;
    qunibusdevice_register_t *ICSR_reg;

    // state behind the registers
    uint16_t csr;
    uint16_t cursor_x;
    uint8_t crtc[CRTC_SIZE];
    uint8_t crtc_pointer;

    struct {
        uint8_t vector[IRQ_COUNT];
        uint8_t irr;            // requested
        uint8_t imr;            // masked
        uint8_t isr;            // in service
        uint8_t acr;            // auto-clear
        uint8_t mode;
        // Where a write to the data port lands: 0..7 a vector, 8 the mask
        // register, 9 the auto-clear register. Reads are steered by the
        // preselect field of the mode register instead.
        uint8_t ptr;
    } intc;

    // the screen
    vcb01::renderer_c renderer;
    x11display_c window;
    bool window_failed;         // reported once, not once per frame

    // keyboard and pointer, behind the DUART
    vcb01::input_c input;
    bool btn_l = false, btn_m = false, btn_r = false;    // pointer buttons held
    // Registers 16..21 and 24..27 are the DUART's; this maps a register index
    // to a 2681 register, or returns -1.
    int duart_reg(unsigned index) const;
    void mirror_duart(void);    // push the DUART's read values into the file

    uint64_t next_vsync_ms;
    uint64_t next_refresh_ms;

    // Height the CRTC last asked for, applied by the worker so all Xlib calls
    // stay on one thread. 0 means "no change pending".
    unsigned pending_height;

    pthread_mutex_t state_mutex;

    void reset_controller(void);
    void update_csr(void);
    unsigned crtc_height(void) const;
    uint32_t bank_base(void) const;
    bool claim_video_memory(void);
    void release_video_memory(void);

    void raise_source(unsigned source);
    void clear_source(unsigned source);
    void update_interrupt(void);

    uint8_t read_intc_data(void);
    void write_intc_data(uint8_t value);
    void write_intc_command(uint8_t value);

    vcb01::state_t screen_state(void);
    void apply_pending_height(void);
    void refresh_screen(void);
    void pump_window_events(void);
    void pump_web_input(void);
    // QBone extension: latch the pointer's absolute screen position for a
    // program to read from MPOS/MPOSY. The real board has only a relative
    // pointer, so bit 15 of MPOS marks the position present.
    void set_mouse_position(int x, int y, bool valid);
    static const uint16_t MPOS_VALID = 0x8000;
    static const uint16_t MPOS_XY = 0x03FF;
};

#endif
