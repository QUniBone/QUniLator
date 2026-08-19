/*
 * Author: Dave Plummer (davepl@davepl.com)
 * (c) 2026 Plummer's Software LLC
 * Contributed under the BSD License
 *
 * DEUNA Ethernet Controller Emulation for QUniBone
 * ================================================
 *
 * This module emulates the DEC DEUNA (UNIBUS Ethernet controller).
 * It provides a port-command interface (PCSR0-3) with descriptor
 * rings in host memory, and bridges Ethernet frames to a host
 * interface through a raw AF_PACKET socket.
 */
#ifndef _DEUNA_HPP_
#define _DEUNA_HPP_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "qunibusdevice.hpp"
#include "priorityrequest.hpp"
#include "ether_bridge.hpp"
#include "parameter.hpp"

/*
 * Default DEUNA I/O page parameters
 * Base address is a typical DEUNA CSR location (octal)
 */
#define DEUNA_DEFAULT_ADDR 0174510
#define DEUNA_DEFAULT_SLOT 18
#define DEUNA_DEFAULT_VECTOR 0120
#define DEUNA_DEFAULT_LEVEL 5

#define DEUNA_FILTER_MAX 12 
#define DEUNA_UDB_WORDS 200 

// Safe string copy with guaranteed NUL termination.
// Returns length of src (like strlcpy).
static inline size_t safe_strcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size > 0) {
        size_t copy_len = (len < size - 1) ? len : size - 1;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return len;
}

#define DEUNA_REG_PCSR0 0
#define DEUNA_REG_PCSR1 1
#define DEUNA_REG_PCSR2 2
#define DEUNA_REG_PCSR3 3

class deuna_c : public qunibusdevice_c {
public:
    deuna_c();
    ~deuna_c() override;

    /*
     * User-configurable parameters (set via menu system before install)
     */
    parameter_string_c interface = parameter_string_c(this, "interface", "if", false,
            "host Ethernet interface shared with the emulated controller");
    parameter_string_c mac = parameter_string_c(this, "mac", "mac", false,
            "station address, 6 hex bytes separated by \":\", empty = device default");

    // Index into the shared activity LED bank, as for the other controllers.
    parameter_unsigned_c activity_led = parameter_unsigned_c(this, "activityled", "al", false,
            "", "%d", "Number of LED to used for activity display.", 8, 10);

    // Lit by traffic in either direction, held on past the frame so a
    // periodic sampler sees it.
    parameter_bool_c activity_lamp = parameter_bool_c(this, "activitylamp", "actl", true,
            "State of activity lamp");

    parameter_unsigned_c rx_slots = parameter_unsigned_c(this, "rx_slots", "rx", false, "",
            "%d", "RX ring scan limit (0 = no limit)", 0, 10);
    parameter_unsigned_c tx_slots = parameter_unsigned_c(this, "tx_slots", "tx", false, "",
            "%d", "TX ring scan limit (0 = no limit)", 0, 10);
    parameter_bool_c trace = parameter_bool_c(this, "trace", "tr", false,
            "Trace CSR/ring events to log");

    /*
     * Read-only statistics (updated during operation, visible in menu)
     */
    parameter_unsigned64_c stat_rx_frames = parameter_unsigned64_c(this, "rx_frames", "rxf", true, "",
            "%llu", "Received frames count", 64, 10);
    parameter_unsigned64_c stat_tx_frames = parameter_unsigned64_c(this, "tx_frames", "txf", true, "",
            "%llu", "Transmitted frames count", 64, 10);
    parameter_unsigned64_c stat_rx_errors = parameter_unsigned64_c(this, "rx_errors", "rxe", true, "",
            "%llu", "Receive error count", 64, 10);
    parameter_unsigned64_c stat_tx_errors = parameter_unsigned64_c(this, "tx_errors", "txe", true, "",
            "%llu", "Transmit error count", 64, 10);

    // What the board has moved, for the performance panel; see delqa.hpp for
    // why frames and bytes are counted apart and why the count is taken where
    // the frame crosses between the guest and the host interface rather than at
    // the socket. These are the live rates, unrelated to the stat_* counters
    // above, which are the controller's own registers as the driver reads them.
    metric_c tx_frames{this, "tx_frames", metric_c::UNIT_COUNT,
                                  "Frames sent"};
    metric_c tx_bytes{this, "tx_bytes", metric_c::UNIT_BYTE,
                                 "Sent"};
    metric_c rx_frames{this, "rx_frames", metric_c::UNIT_COUNT,
                                  "Frames received"};
    metric_c rx_bytes{this, "rx_bytes", metric_c::UNIT_BYTE,
                                 "Received"};

    /*
     * QUniBone device framework callbacks
     */
    const char *category(void) const override { return "network"; }

    bool on_param_changed(parameter_c *param) override;
    bool on_before_install(void) override;
    void on_after_install(void) override;
    void on_after_uninstall(void) override;

    void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
    void on_init_changed(void) override;

    void on_after_register_access(qunibusdevice_register_t *device_reg, uint8_t qunibus_control,
            DATO_ACCESS access) override;

    void worker(unsigned instance) override;
    void worker_wake(void) override;
    void refresh_activity(void) override;

private:
    /*
     * Bus requests for interrupts and DMA
     */
    intr_request_c intr_request{this};
    dma_request_c dma_request{this};
    dma_request_c dma_desc_request{this};

    /*
     * Network bridge: a raw AF_PACKET socket sharing the host interface
     */
    ether_bridge_c bridge = ether_bridge_c("deuna");

    /*
     * DMA synchronization
     */
    std::recursive_mutex dma_mutex;

    /*
     * Device Registers
     */
    qunibusdevice_register_t *reg_pcsr0 = nullptr;
    qunibusdevice_register_t *reg_pcsr1 = nullptr;
    qunibusdevice_register_t *reg_pcsr2 = nullptr;
    qunibusdevice_register_t *reg_pcsr3 = nullptr;

    /*
     * Thread synchronization
     */
    std::recursive_mutex state_mutex;
    std::mutex queue_mutex;  // New: Serialize queue access from PCAP callbacks
    std::atomic<bool> reset_in_progress{false};  // New: Flag to abort worker operations during reset

    /*
     * Pending register writes from PDP-11 (preserve write order)
     */
    struct pending_reg_write {
        uint8_t reg_index = 0;
        uint16_t value = 0;
        uint8_t access = 0;
        uint16_t w1c_snapshot = 0;
    };
    std::mutex pending_reg_mutex;
    std::deque<pending_reg_write> pending_reg_queue;

    /*
     * Pending port command for worker thread (DMA required)
     */
    std::mutex pending_cmd_mutex;
    std::condition_variable pending_cmd_cv;
    uint16_t pending_cmd = 0;  // 0 = no command pending

    /*
     * Setup packet state (MAC filtering)
     */
    struct setup_state {
        bool valid = false;
        bool promiscuous = false;
        bool multicast = false;
        int mac_count = 0;
        uint8_t macs[DEUNA_FILTER_MAX][6] = {{0}};
    } setup;

    /*
     * Network statistics
     */
    struct stats_state {
        uint32_t secs = 0;
        uint32_t frecv = 0;
        uint32_t mfrecv = 0;
        uint16_t rxerf = 0;
        uint16_t frecve = 0;
        uint32_t rbytes = 0;
        uint32_t mrbytes = 0;
        uint16_t rlossi = 0;
        uint16_t rlossl = 0;
        uint32_t ftrans = 0;
        uint32_t mftrans = 0;
        uint32_t ftrans3 = 0;
        uint32_t ftrans2 = 0;
        uint32_t ftransd = 0;
        uint32_t tbytes = 0;
        uint32_t mtbytes = 0;
        uint16_t txerf = 0;
        uint16_t ftransa = 0;
        uint16_t txccf = 0;
        uint16_t porterr = 0;
        uint16_t bablcnt = 0;
        uint64_t last_update_ns = 0;
    } stats;

    /*
     * Packet buffer for RX/TX operations
     */
    struct packet_buffer {
        std::vector<uint8_t> msg;
        size_t len = 0;
        size_t used = 0;
        size_t crc_len = 0;
        int status = 0;
    } read_buffer, write_buffer;

    /*
     * Queue item for received packets waiting to be delivered
     */
    struct queue_item {
        bool loopback = false;
        packet_buffer packet;
    };

    std::deque<queue_item> read_queue;
    unsigned read_queue_loss = 0;

    /*
     * Port command and ring state
     */
    uint16_t pcsr0 = 0;
    uint16_t pcsr1 = 0;
    uint16_t pcsr2 = 0;
    uint16_t pcsr3 = 0;
    uint32_t mode = 0;
    uint16_t stat = 0;
    bool irq = false;

    uint32_t pcbb = 0;
    uint32_t tdrb = 0;
    uint32_t telen = 0;
    uint32_t trlen = 0;
    uint32_t txnext = 0;
    uint32_t rdrb = 0;
    uint32_t relen = 0;
    uint32_t rrlen = 0;
    uint32_t rxnext = 0;

    std::vector<uint16_t> wcs_mem;
    std::vector<uint16_t> link_mem;

    uint16_t pcb[4] = {0};
    uint16_t udb[DEUNA_UDB_WORDS] = {0};
    uint16_t rxhdr[4] = {0};
    uint16_t txhdr[4] = {0};

    uint8_t load_server[6] = {0};

    /*
     * MAC address state
     */
    bool mac_override = false;
    uint8_t mac_addr[6] = {0};

    /*
     * Controller reset/initialization
     */
    void reset_controller(void);
    void init_internal_memory(void);

    /*
     * Register value update functions
     */
    void update_pcsr_regs(void);
    void update_transceiver_bits(void);
    void update_intr(void);

    /*
     * Register write handling
     */
    void handle_register_write(uint8_t reg_index, uint16_t val, DATO_ACCESS access,
            uint16_t w1c_snapshot);
    void apply_pending_reg_writes(void);
    void process_pending_command(void);

    /*
     * DMA operations
     */
    bool dma_read_words(uint32_t addr, uint16_t *buffer, size_t wordcount);
    bool dma_write_words(uint32_t addr, const uint16_t *buffer, size_t wordcount);
    bool desc_read_words(uint32_t addr, uint16_t *buffer, size_t wordcount);
    bool desc_write_words(uint32_t addr, const uint16_t *buffer, size_t wordcount);
    bool dma_read_bytes(uint32_t addr, uint8_t *buffer, size_t len);
    bool dma_write_bytes(uint32_t addr, const uint8_t *buffer, size_t len);
    bool process_bootrom(uint32_t dst_addr);
    bool load_system_microcode(uint32_t udbb);
    bool transfer_internal_memory(uint32_t udbb, bool to_internal);

    uint32_t make_addr(uint16_t hi, uint16_t lo) const;

    /*
     * Port command processing
     */
    void port_command(uint16_t cmd); 
    bool execute_command(void); 

    /*
     * Receive/transmit ring processing
     */
    void enqueue_readq(const uint8_t *data, size_t len, bool loopback);
    bool process_receive(void); 
    bool process_transmit(unsigned max_descriptors = 0); 
    void dump_tx_ring(unsigned max_entries);

    /*
     * Packet filtering
     */
    bool accept_packet(const uint8_t *data, size_t len) const;
    // Point the bridge's kernel filter at the address the driver has set, so
    // the socket carries what this controller answers to and little else.
    void update_address_filter(void);

    /*
     * Timer services
     */
    void service_timers(void);

    /*
     * Worker thread entry points
     */
    void worker_rx(void);
    void worker_tx(void);

    /*
     * Utility
     */
    static bool parse_mac(const std::string &text, uint8_t out[6]);

    // Pulse the activity LED and lamp; called for traffic in either direction.
    void note_activity(void);
    uint64_t activity_lamp_until_ms = 0; // activity_lamp stays lit until then
};

#endif
