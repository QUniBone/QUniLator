/* vcb01.cpp: VCB01 (M7602) QVSS Qbus video subsystem

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
*/

#include <string.h>
#include <time.h>

#include "logger.hpp"
#include "qunibusadapter.hpp"
#include "qunibus.h"
#include "ddrmem.h"
#include "utils.hpp"

#include "vcb01.hpp"
#ifdef WEBUI
#include "webvcb01.hpp"
#endif

using namespace vcb01;

vcb01_c::vcb01_c() :
        qunibusdevice_c(),
        CSR_reg(nullptr), CURX_reg(nullptr), MPOS_reg(nullptr), MPOSY_reg(nullptr),
        CRTCP_reg(nullptr), CRTCD_reg(nullptr), ICDR_reg(nullptr), ICSR_reg(nullptr),
        csr(0), cursor_x(0), crtc_pointer(0),
        window_failed(false), next_vsync_ms(0), next_refresh_ms(0),
        pending_height(0)
{
    set_workers_count(1);       // the refresh loop

    name.value = "vcb01";
    type_name.value = "VCB01";
    log_label = "vcb01";

    pthread_mutex_init(&state_mutex, nullptr);

    memset(crtc, 0, sizeof(crtc));
    memset(&intc, 0, sizeof(intc));

    // 016 puts video memory at 16000000, below a machine's own memory rather
    // than at the top of Qbus memory space where a VAXstation II carries it.
    bank.value = 016;
    refresh_rate.value = 30;
    large_monitor.value = true;
    display.value = "";

    // The QVSS answers at 17777200 and interrupts on BR4. Its vectors come
    // from the interrupt controller the host programs, so intr_vector is only
    // what an unprogrammed controller would deliver.
    set_default_bus_params(0777200, 24, 0300, 4);

    register_count = 32;

    // Everything not named below reads as zero and swallows writes, which is
    // what the board's spare addresses do.
    for (unsigned i = 0; i < register_count; i++) {
        qunibusdevice_register_t *reg = &(this->registers[i]);
        sprintf(reg->name, "R%02o", i * 2);
        reg->active_on_dati = false;
        reg->active_on_dato = false;
        reg->reset_value = 0;
        reg->writable_bits = 0x0000;
    }

    CSR_reg = &(this->registers[0]);
    strcpy(CSR_reg->name, "CSR");
    CSR_reg->active_on_dato = true;
    CSR_reg->writable_bits = 0xffff;    // the device filters, see the CSR case

    CURX_reg = &(this->registers[1]);
    strcpy(CURX_reg->name, "CURX");
    CURX_reg->active_on_dato = true;
    CURX_reg->writable_bits = 0xffff;

    // QBone extension: the pointer's absolute screen position, which the real
    // board's relative mouse cannot give. MPOS carries X with bit 15 set when a
    // position is present, MPOSY carries Y. Read-only; the worker latches them.
    MPOS_reg = &(this->registers[2]);
    strcpy(MPOS_reg->name, "MPOS");
    MPOSY_reg = &(this->registers[3]);
    strcpy(MPOSY_reg->name, "MPOSY");

    CRTCP_reg = &(this->registers[4]);
    strcpy(CRTCP_reg->name, "CRTCP");
    CRTCP_reg->active_on_dato = true;
    CRTCP_reg->writable_bits = 0xffff;

    CRTCD_reg = &(this->registers[5]);
    strcpy(CRTCD_reg->name, "CRTCD");
    CRTCD_reg->active_on_dato = true;
    CRTCD_reg->writable_bits = 0xffff;

    ICDR_reg = &(this->registers[6]);
    strcpy(ICDR_reg->name, "ICDR");
    ICDR_reg->active_on_dati = true;    // the value depends on the preselect
    ICDR_reg->active_on_dato = true;
    ICDR_reg->writable_bits = 0xffff;

    ICSR_reg = &(this->registers[7]);
    strcpy(ICSR_reg->name, "ICSR");
    ICSR_reg->active_on_dato = true;
    ICSR_reg->writable_bits = 0xffff;

    // The DUART: registers 16..21 (channel A and the interrupt controller) and
    // 24..27 (channel B). Reads have side effects - a receive buffer pops, a
    // mode register advances - so they are active on DATI as well as DATO.
    static const char *duart_names[] = {
        "MR1A", "SRA", "CRA", "RBUFA", "IPCR", "ISR", "", "",
        "MR1B", "SRB", "CRB", "RBUFB" };
    for (unsigned i = 16; i <= 27; i++) {
        if (duart_reg(i) < 0)
            continue;
        qunibusdevice_register_t *reg = &(this->registers[i]);
        strcpy(reg->name, duart_names[i - 16]);
        // The status and interrupt-status registers are polled in tight loops,
        // so they answer from the mirrored value without waking the ARM. Only
        // a receive buffer (its read pops) and a mode register (its read
        // advances the pointer) need the ARM on DATI.
        reg->active_on_dati = (i == 19 || i == 27 || i == 16 || i == 24);
        reg->active_on_dato = true;
        reg->writable_bits = 0xffff;
    }
}

// duart_reg(): which 2681 register a QVSS register index addresses, or -1.
int vcb01_c::duart_reg(unsigned index) const
{
    if (index >= 16 && index <= 21)
        return (int) (index - 16);      // channel A + interrupt: 2681 reg 0..5
    if (index >= 24 && index <= 27)
        return (int) (index - 16);      // channel B: 2681 reg 8..11
    return -1;
}

// mirror_duart(): the DUART's read values live in this device's own state, so
// push them into the shared register file for the PRU to return on DATI.
void vcb01_c::mirror_duart(void)
{
    for (unsigned i = 16; i <= 27; i++) {
        int r = duart_reg(i);
        if (r >= 0)
            set_register_dati_value(&(this->registers[i]),
                    input.duart_peek((unsigned) r), __func__);
    }
}

vcb01_c::~vcb01_c()
{
    window.close();
    pthread_mutex_destroy(&state_mutex);
}

uint32_t vcb01_c::bank_base(void) const
{
    return (uint32_t) bank.value << 18;
}

// crtc_height(): the displayed height the 6845 is programmed for - vertical
// displayed character rows times the scan lines each row holds. Zero until the
// host programs it, which the caller reads as "keep the default".
unsigned vcb01_c::crtc_height(void) const
{
    return (unsigned) crtc[CRTC_VDSP] * (crtc[CRTC_MSCN] + 1u);
}

// reset_controller(): what INIT and power-up leave behind.
void vcb01_c::reset_controller(void)
{
    csr = 0;
    cursor_x = 0;
    crtc_pointer = 0;
    memset(crtc, 0, sizeof(crtc));

    memset(&intc, 0, sizeof(intc));
    intc.imr = 0xFF;            // every source masked until the host says otherwise

    input.reset();
    if (CSR_reg != nullptr) {
        mirror_duart();
        update_csr();
    }
    if (CURX_reg != nullptr)
        set_register_dati_value(CURX_reg, 0, __func__);

    renderer.invalidate_all();
}

// update_csr(): compose what a DATI on the CSR delivers. The bank switch, the
// monitor size and the pointer buttons are the board's to report, so they are
// pushed into the register rather than taken from what the host last wrote.
void vcb01_c::update_csr(void)
{
    uint16_t value = (uint16_t) (csr & CSR_RW_MASK);
    value |= (uint16_t) ((bank.value & 0xF) << CSR_V_MA);
    if (large_monitor.value)
        value |= CSR_MOD;
    // Each button reads as one when up.
    if (!input.button_left())   value |= CSR_MSA;
    if (!input.button_middle()) value |= CSR_MSB;
    if (!input.button_right())  value |= CSR_MSC;
    set_register_dati_value(CSR_reg, value, __func__);
}

// claim_video_memory(): have the PRU answer bus cycles to the bank out of DDR,
// so the bitmap runs at bus speed and the host never waits on the ARM. The bank
// takes the device slot, so a memory card in the memory slot keeps serving the
// rest of the machine; a bank that lands inside the card's range is refused.
bool vcb01_c::claim_video_memory(void)
{
    uint32_t base = bank_base();
    uint32_t last = base + BANK_BYTES - 2;

    if (!ddrmem->set_range(DDRMEM_RANGE_DEVICE, base, last)) {
        ERROR("cannot serve video memory at %s..%s", qunibus->addr2text(base),
                qunibus->addr2text(last));
        return false;
    }
    INFO("video memory at %s..%s, bank %o", qunibus->addr2text(base),
            qunibus->addr2text(last), bank.value);

    // A board comes up with whatever its memory held, and a driver clears it;
    // starting from zero keeps a stale picture off the screen.
    memset((void *) (ddrmem->base_virtual->memory.bytes + base), 0, BANK_BYTES);
    renderer.invalidate_all();
    return true;
}

void vcb01_c::release_video_memory(void)
{
    // start > end disables the range
    ddrmem->set_range(DDRMEM_RANGE_DEVICE, 0xffffffff, 0);
}

bool vcb01_c::on_param_changed(parameter_c *param)
{
    if (param == &priority_slot)
        intr_request.set_priority_slot(priority_slot.new_value);
    else if (param == &intr_level)
        intr_request.set_level(intr_level.new_value);
    else if (param == &bank) {
        if (bank.new_value > 016) {
            ERROR("bank %o would reach the I/O page; 0..016 are available",
                    bank.new_value);
            return false;
        }
        if (enabled.value)
            return false;       // a switch on the board, moved while it is out
    } else if (param == &refresh_rate) {
        if (refresh_rate.new_value < 1 || refresh_rate.new_value > 60) {
            ERROR("refresh rate %d is outside 1..60 Hz", refresh_rate.new_value);
            return false;
        }
    } else if (param == &display) {
        if (enabled.value)
            return false;       // the window is opened when the board goes in
    }
    return qunibusdevice_c::on_param_changed(param);
}

// on_before_install(): the screen has to be there before the board is, so a
// display that cannot be reached refuses the enable instead of leaving a
// controller on the bus with nowhere to draw.
bool vcb01_c::on_before_install(void)
{
    window_failed = false;
    // The X display is optional: with no display name the board draws only to
    // the web interface, so an empty name is not a failure. A name that is set
    // but cannot be reached still refuses the enable.
    if (!display.value.empty()) {
        if (!window.open(display.value, "QBone VCB01", XSIZE, renderer.height())) {
            ERROR("%s", window.error().c_str());
            return false;
        }
        INFO("display \"%s\" resolved to \"%s\"", display.value.c_str(),
                window.resolved_display().c_str());
    } else
        INFO("no X display configured; drawing to the web interface only");

    if (!claim_video_memory()) {
        window.close();
        return false;
    }

    // What the PRU restores on bus INIT without waking the ARM. The bank
    // switch and the monitor size are the board's own and survive a reset.
    CSR_reg->reset_value = (uint16_t) (((bank.value & 0xF) << CSR_V_MA)
            | (large_monitor.value ? CSR_MOD : 0) | CSR_MSA | CSR_MSB | CSR_MSC);
    return true;
}

void vcb01_c::on_after_install(void)
{
    pthread_mutex_lock(&state_mutex);
    reset_controller();
    pthread_mutex_unlock(&state_mutex);
    next_vsync_ms = next_refresh_ms = now_ms();
}

void vcb01_c::on_after_uninstall(void)
{
    release_video_memory();
    window.close();
}

// ------------------------------------------------------------ interrupts

void vcb01_c::raise_source(unsigned source)
{
    intc.irr |= (uint8_t) (1u << source);
    update_interrupt();
}

void vcb01_c::clear_source(unsigned source)
{
    intc.irr &= (uint8_t) ~(1u << source);
    update_interrupt();
}

// update_interrupt(): the lowest-numbered unmasked request wins, and its
// vector is what the controller would put on the bus.
void vcb01_c::update_interrupt(void)
{
    // The DUART's interrupt is the interrupt controller's source 0: it is
    // pending whenever an enabled DUART condition is - a keystroke waiting, a
    // pointer report waiting.
    if (input.interrupt_pending())
        intc.irr |= (uint8_t) (1u << IRQ_DUART);
    else
        intc.irr &= (uint8_t) ~(1u << IRQ_DUART);

    if (!(csr & CSR_IEN) || !(intc.mode & ICM_MM)) {
        qunibusadapter->cancel_INTR(intr_request);
        return;
    }

    uint8_t pending = (uint8_t) (intc.irr & ~intc.imr);
    if (pending == 0) {
        qunibusadapter->cancel_INTR(intr_request);
        return;
    }

    for (unsigned i = 0; i < IRQ_COUNT; i++)
        if (pending & (1u << i)) {
            intc.isr |= (uint8_t) (1u << i);
            if (intc.acr & (1u << i))
                intc.irr &= (uint8_t) ~(1u << i);
            intr_request.set_vector(intc.vector[i]);
            qunibusadapter->INTR(intr_request, nullptr, 0);
            return;
        }
}

uint8_t vcb01_c::read_intc_data(void)
{
    switch ((intc.mode & ICM_M_RP) >> ICM_V_RP) {
    case 0:
        return intc.isr;
    case 1:
        return intc.imr;
    case 2:
        return intc.irr;
    default:
        return intc.acr;
    }
}

void vcb01_c::write_intc_data(uint8_t value)
{
    if (intc.ptr == 8)
        intc.imr = value;
    else if (intc.ptr == 9)
        intc.acr = value;
    else
        intc.vector[intc.ptr & 7] = (uint8_t) (value & 0xFC);
    update_interrupt();
}

void vcb01_c::write_intc_command(uint8_t value)
{
    unsigned command = (value >> 4) & 0xF;
    bool one_bit = (value & 0x8) != 0;
    unsigned which = value & 0x7;

    switch (command) {
    case 0:                                     // reset
        memset(&intc, 0, sizeof(intc));
        intc.imr = 0xFF;
        break;
    case 2:                                     // clear IRR and IMR
        if (one_bit) {
            intc.irr &= (uint8_t) ~(1u << which);
            intc.imr &= (uint8_t) ~(1u << which);
        } else {
            intc.irr = 0;
            intc.imr = 0;
        }
        break;
    case 3:                                     // set IMR
        if (one_bit)
            intc.imr |= (uint8_t) (1u << which);
        else
            intc.imr = 0xFF;
        break;
    case 4:                                     // clear IRR
        if (one_bit)
            intc.irr &= (uint8_t) ~(1u << which);
        else
            intc.irr = 0;
        break;
    case 6:                                     // clear the highest ISR bit
        for (unsigned i = 0; i < IRQ_COUNT; i++)
            if (intc.isr & (1u << i)) {
                intc.isr &= (uint8_t) ~(1u << i);
                break;
            }
        break;
    case 7:                                     // clear ISR
        if (one_bit)
            intc.isr &= (uint8_t) ~(1u << which);
        else
            intc.isr = 0;
        break;
    case 8:
    case 9:                                     // load the mode bits
        intc.mode = (uint8_t) ((intc.mode & ~0x1F) | (value & 0x1F));
        break;
    case 10:                                    // preselect and master mask
        intc.mode = (uint8_t) ((intc.mode & ~ICM_M_RP) | ((value << 3) & ICM_M_RP));
        if ((value & 3) == 1 || (value & 3) == 2)
            intc.mode = (uint8_t) ((intc.mode & ~ICM_MM) | ((value << 7) & ICM_MM));
        break;
    case 11:                                    // the data port addresses IMR
        intc.ptr = 8;
        break;
    case 12:                                    // the data port addresses ACR
        intc.ptr = 9;
        break;
    case 14:                                    // the data port addresses a vector
        intc.ptr = (uint8_t) which;
        break;
    default:
        break;
    }
    update_interrupt();
}

// ------------------------------------------------------------ registers

void vcb01_c::on_after_register_access(qunibusdevice_register_t *device_reg,
        uint8_t unibus_control, DATO_ACCESS access)
{
    UNUSED(access);

    pthread_mutex_lock(&state_mutex);

    if (unibus_control == QUNIBUS_CYCLE_DATI) {
        // Only the interrupt controller's data port changes what it delivers
        // between reads, the preselect deciding which register answers.
        if (device_reg == ICDR_reg)
            set_register_dati_value(ICDR_reg, read_intc_data(), __func__);
        else if (duart_reg(device_reg->index) >= 0) {
            // Reading a receive buffer pops it; reload the file for next time.
            input.duart_consume((unsigned) duart_reg(device_reg->index));
            mirror_duart();
            update_interrupt();
        }
        pthread_mutex_unlock(&state_mutex);
        return;
    }

    uint16_t value = device_reg->active_dato_flipflops;
    DEBUG("DATO %s = %06o", device_reg->name, value);

    if (device_reg == CSR_reg) {
        uint16_t before = csr;
        csr = (uint16_t) ((csr & ~CSR_RW_MASK) | (value & CSR_RW_MASK));
        update_csr();
        if ((csr ^ before) & CSR_IEN)
            update_interrupt();
    } else if (device_reg == CURX_reg) {
        cursor_x = value;
        set_register_dati_value(CURX_reg, value, __func__);
    } else if (device_reg == CRTCP_reg) {
        crtc_pointer = (uint8_t) (value & CRTCP_REG);
        // Vertical blanking reads back here; the refresh worker keeps it.
        set_register_dati_value(CRTCP_reg,
                (uint16_t) (crtc_pointer | (CRTCP_reg->active_dati_flipflops & CRTCP_VB)),
                __func__);
    } else if (device_reg == CRTCD_reg) {
        if (crtc_pointer < CRTC_SIZE)
            crtc[crtc_pointer] = (uint8_t) (value & 0xFF);
        set_register_dati_value(CRTCD_reg,
                crtc_pointer < CRTC_SIZE ? crtc[crtc_pointer] : 0, __func__);
        // The vertical displayed count and the scan lines per row together set
        // the screen height. The worker resizes the window, off this thread.
        if (crtc_pointer == CRTC_VDSP || crtc_pointer == CRTC_MSCN) {
            unsigned h = crtc_height();
            if (h >= 64 && h <= vcb01::YMAX)
                pending_height = h;
        }
    } else if (device_reg == ICDR_reg) {
        write_intc_data((uint8_t) (value & 0xFF));
    } else if (device_reg == ICSR_reg) {
        write_intc_command((uint8_t) (value & 0xFF));
    } else if (duart_reg(device_reg->index) >= 0) {
        input.duart_write((unsigned) duart_reg(device_reg->index), value);
        mirror_duart();
        update_interrupt();
    }

    pthread_mutex_unlock(&state_mutex);
}

void vcb01_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge)
{
    UNUSED(aclo_edge);

    if (dclo_edge == SIGNAL_EDGE_RAISING) {
        pthread_mutex_lock(&state_mutex);
        reset_controller();
        pthread_mutex_unlock(&state_mutex);
    }
}

void vcb01_c::on_init_changed(void)
{
    if (init_asserted) {
        pthread_mutex_lock(&state_mutex);
        reset_controller();
        pthread_mutex_unlock(&state_mutex);
    }
}

// ------------------------------------------------------------ the screen

// screen_state(): what the registers contribute to the picture. The cursor's
// vertical position is the CRTC's to give, in character rows scaled by the
// scan lines each one holds.
state_t vcb01_c::screen_state(void)
{
    state_t st;
    st.video_enable = (csr & CSR_VID) != 0;
    st.cursor_or = (csr & CSR_FNC) != 0;
    st.cursor_x = cursor_x & 0x3FF;
    st.cursor_y = (unsigned) crtc[CRTC_CAH] * (crtc[CRTC_MSCN] + 1u) + crtc[CRTC_CSCS];
    st.cursor_visible = (crtc[CRTC_CSCS] & 0x20) == 0;
    return st;
}

// apply_pending_height(): a CRTC change asks for a different screen height; the
// renderer and, when one is open, the X window follow it. Run on the worker
// thread every pass, whether or not anything is watching, so the height is
// already right the instant a browser attaches - it does not wait for a client
// to be present when the CRTC is programmed.
void vcb01_c::apply_pending_height(void)
{
    pthread_mutex_lock(&state_mutex);
    unsigned want = pending_height;
    pending_height = 0;
    pthread_mutex_unlock(&state_mutex);

    if (want != 0 && want != renderer.height()) {
        renderer.set_height(want);
        if (window.is_open())
            window.resize(XSIZE, want);
        INFO("screen height %u lines", want);
    }
}

void vcb01_c::refresh_screen(void)
{
    const uint8_t *video = (const uint8_t *) ddrmem->base_virtual->memory.bytes
            + bank_base();

    pthread_mutex_lock(&state_mutex);
    state_t st = screen_state();
    pthread_mutex_unlock(&state_mutex);

    const std::vector<span_t> &spans = renderer.update(video, st);
    if (window.is_open()) {
        for (const span_t &s : spans)
            window.put_rows(renderer.pixels(), s.first, s.count);
        if (!spans.empty())
            window.flush();
    }
#ifdef WEBUI
    // The web clients get the same frame, whether or not an X window is open.
    webvcb01_publish(XSIZE, renderer.height(), renderer.pixels(), spans);
#endif
}

void vcb01_c::pump_window_events(void)
{
    bool input_changed = false;

    for (const x11display_c::event_t &ev : window.poll_events())
        switch (ev.kind) {
        case x11display_c::event_t::EXPOSED:
            renderer.invalidate_all();
            break;
        case x11display_c::event_t::CLOSED:
            // The board stays in the backplane when its monitor is switched
            // off, so the window closing only stops the picture.
            INFO("display window closed");
            window.close();
            break;
        case x11display_c::event_t::KEY_PRESS:
        case x11display_c::event_t::KEY_RELEASE:
            pthread_mutex_lock(&state_mutex);
            input.key_event(ev.keysym, ev.kind == x11display_c::event_t::KEY_PRESS);
            input_changed = true;
            pthread_mutex_unlock(&state_mutex);
            break;
        case x11display_c::event_t::BUTTON_PRESS:
        case x11display_c::event_t::BUTTON_RELEASE:
        case x11display_c::event_t::MOTION: {
            pthread_mutex_lock(&state_mutex);
            // X buttons: 1 left, 2 middle, 3 right. Track them across events.
            if (ev.button == 1) btn_l = (ev.kind == x11display_c::event_t::BUTTON_PRESS);
            if (ev.button == 2) btn_m = (ev.kind == x11display_c::event_t::BUTTON_PRESS);
            if (ev.button == 3) btn_r = (ev.kind == x11display_c::event_t::BUTTON_PRESS);
            input.pointer_event(ev.dx, ev.dy, btn_l, btn_m, btn_r);
            update_csr();       // buttons show in the CSR
            input_changed = true;
            pthread_mutex_unlock(&state_mutex);
            break;
        }
        default:
            break;
        }

    if (input_changed) {
        pthread_mutex_lock(&state_mutex);
        mirror_duart();
        update_interrupt();
        pthread_mutex_unlock(&state_mutex);
    }
}

// pump_web_input(): keyboard and pointer sent from a browser reach the DUART
// the same way the X window's events do. Buttons are held in the same fields,
// so an X monitor and a browser drive one shared pointer.
void vcb01_c::pump_web_input(void)
{
#ifdef WEBUI
    std::vector<webvcb01_input_t> evs;
    if (webvcb01_poll_input(evs) == 0)
        return;
    pthread_mutex_lock(&state_mutex);
    for (const webvcb01_input_t &e : evs)
        switch (e.kind) {
        case webvcb01_input_t::KEY:
            input.key_event(e.keysym, e.down);
            break;
        case webvcb01_input_t::MOTION:
            input.pointer_event(e.dx, e.dy, btn_l, btn_m, btn_r);
            break;
        case webvcb01_input_t::BUTTON:
            if (e.button == 1) btn_l = e.down;
            if (e.button == 2) btn_m = e.down;
            if (e.button == 3) btn_r = e.down;
            input.pointer_event(0, 0, btn_l, btn_m, btn_r);
            update_csr();       // buttons show in the CSR
            break;
        case webvcb01_input_t::ABSPOS:
            set_mouse_position(e.dx, e.dy, true);   // the QBone absolute-position extension
            break;
        }
    mirror_duart();
    update_interrupt();
    pthread_mutex_unlock(&state_mutex);
#endif
}

// set_mouse_position(): latch the absolute pointer position for MPOS/MPOSY.
// caller holds state_mutex.
void vcb01_c::set_mouse_position(int x, int y, bool valid)
{
    if (x < 0) x = 0; else if (x > (int) MPOS_XY) x = MPOS_XY;
    if (y < 0) y = 0; else if (y > (int) MPOS_XY) y = MPOS_XY;
    set_register_dati_value(MPOS_reg,
            (uint16_t) ((valid ? MPOS_VALID : 0) | (x & MPOS_XY)), __func__);
    set_register_dati_value(MPOSY_reg, (uint16_t) (y & MPOS_XY), __func__);
}

void vcb01_c::worker(unsigned instance)
{
    UNUSED(instance);

    while (!workers_terminate) {
        uint64_t now = now_ms();

        // Vertical sync at 60 Hz whatever the screen is refreshed at: a driver
        // counts on it for timing, not only for repainting.
        if (now >= next_vsync_ms) {
            next_vsync_ms = now + 1000 / 60;
            pthread_mutex_lock(&state_mutex);
            // Blanking is asserted between frames, which is when a driver
            // writes video memory without tearing the picture.
            set_register_dati_value(CRTCP_reg,
                    (uint16_t) (crtc_pointer | CRTCP_VB), __func__);
            raise_source(IRQ_VSYNC);
            pthread_mutex_unlock(&state_mutex);
        }

        if (now >= next_refresh_ms) {
            next_refresh_ms = now + 1000 / (refresh_rate.value ? refresh_rate.value : 30);
            if (window.is_open())
                pump_window_events();
            pump_web_input();
            apply_pending_height();     // track the CRTC even with nothing watching
            bool watching = window.is_open();
#ifdef WEBUI
            watching = watching || webvcb01_watching();
#endif
            if (watching)
                refresh_screen();
            pthread_mutex_lock(&state_mutex);
            set_register_dati_value(CRTCP_reg, crtc_pointer, __func__);
            pthread_mutex_unlock(&state_mutex);
        }

        uint64_t next = next_vsync_ms < next_refresh_ms ? next_vsync_ms : next_refresh_ms;
        now = now_ms();
        timeout_c::wait_ms(next > now ? (unsigned) (next - now) : 1);
    }
}
