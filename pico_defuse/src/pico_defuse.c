#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/sync.h" 
#include "pico_defuse.pio.h"

const uint PIN_DEBUGLED_BASE = 2;
const uint PIN_DEBUGLED_COUNT = 8;

const uint PIN_LED = 25;                // Status LED
const uint PIN_DATA_BASE = 10;          // Base pin used for output, 4 consecutive pins are used 
const uint PIN_NRST = 15;               // Wii U reset
const uint PIN_CLK = 14;                // EXI bus clock line

uint8_t read_debug_gpios[0x100] = {0};

PIO pio;
PIO pio_exi;
int debug_gpio_monitor_dmachan;
dma_channel_config debug_gpio_monitor_dmacfg;
uint debug_gpio_monitor_parallel_sm;
uint exi_inject_sm;
uint exi_inject_offset;
uint debug_gpio_monitor_parallel_offset;
uint debug_gpio_monitor_serial_offset;

#define MONITOR_SERIAL_DATA (0)
#define MONITOR_SERIAL_TEXT (1)

#define WIIU_STATE_POWERED_OFF    (0)
#define WIIU_STATE_NEEDS_DEFUSE   (1)
#define WIIU_STATE_DEFUSED        (2)
#define WIIU_STATE_MONITORING     (3)
#define WIIU_CHECK_IF_POWERED_OFF (4)

const char* WIIU_STATE_NAMES[5] = {
    "WIIU_STATE_POWERED_OFF",
    "WIIU_STATE_NEEDS_DEFUSE",
    "WIIU_STATE_DEFUSED",
    "WIIU_STATE_MONITORING",
    "WIIU_CHECK_IF_POWERED_OFF",
};

const uint8_t MONITOR_SERIAL_DATA_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xBE, 0xEF, 0xCA, 0xFE};
const uint8_t MONITOR_SERIAL_TEXT_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xF0, 0x0F, 0xCA, 0xFE};

char text_buffer[2048];
int text_buffer_pos = 0;
uint8_t last_16_bytes[16] = {0};
uint8_t serial_data_cnt = 0;
uint8_t current_mode = MONITOR_SERIAL_DATA;

uint8_t wiiu_state = WIIU_STATE_POWERED_OFF;
uint8_t next_wiiu_state = WIIU_STATE_POWERED_OFF;
bool nrst_sense_state = false;

void nrst_sense_callback(uint gpio, uint32_t events) 
{
    //printf("NRST %x\n", events);

    // NRST went from low to high, we need to inject de_Fuse
    if (events & 8)
    {
        switch (wiiu_state)
        {
        case WIIU_STATE_POWERED_OFF:
        case WIIU_CHECK_IF_POWERED_OFF:
            next_wiiu_state = WIIU_STATE_NEEDS_DEFUSE;
            break;
        }
    }
    else if (events & 4) {
        next_wiiu_state = WIIU_CHECK_IF_POWERED_OFF;
    }
    
}

void nrst_sense_set(bool is_on)
{
    if (is_on == nrst_sense_state) {
        return;
    }

    if (is_on) {
        gpio_put(PIN_NRST, true);
        gpio_set_dir(PIN_NRST, GPIO_IN);
        gpio_set_irq_enabled_with_callback(PIN_NRST, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &nrst_sense_callback);
    }
    else {
        gpio_set_irq_enabled_with_callback(PIN_NRST, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false, &nrst_sense_callback);
        gpio_put(PIN_NRST, true);
        gpio_set_dir(PIN_NRST, GPIO_OUT);
    }
    nrst_sense_state = is_on;
}

// Stuff that needs to happen extremely early
void fast_one_time_init()
{
    stdio_init_all();

    // LED on for now to indicate life or something
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, true);

    gpio_init(PIN_NRST);
    gpio_set_dir(PIN_NRST, GPIO_IN);
    gpio_put(PIN_NRST, true); // make sure we don't accidentally inject rst pulses when switching dirs lol

    // TODO: figure out the bare minimum I can pull off (maybe downclock later?)
    // 250MHz seems to capture debug pin transistions, which was the ~status quo
    // on my FPGA at 27MHz. Let's do 4x27MHz to be safe.
    set_sys_clock_khz(108000, true);

    wiiu_state = WIIU_STATE_POWERED_OFF;
    next_wiiu_state = WIIU_STATE_POWERED_OFF;
    nrst_sense_set(true);
}

void slow_one_time_init()
{
    // Prioritize DMA engine
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    // From PicoBoot: put some extra oomph into the EXI data pin,
    // because the Wii U has a stupidly high current sink on inputs
    gpio_set_slew_rate(PIN_DATA_BASE, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_DATA_BASE + 1, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_DATA_BASE + 2, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_DATA_BASE + 3, GPIO_SLEW_RATE_FAST);

    gpio_set_drive_strength(PIN_DATA_BASE, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIN_DATA_BASE + 1, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIN_DATA_BASE + 2, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIN_DATA_BASE + 3, GPIO_DRIVE_STRENGTH_4MA);

    pio = pio0;
    pio_exi = pio1;

    // We use two statemachines: one for the debug monitor, and one for the EXI injecting.
    debug_gpio_monitor_parallel_sm = pio_claim_unused_sm(pio, true);
    exi_inject_sm = pio_claim_unused_sm(pio_exi, true);

    // The exi inject statemachine is only used during de_Fusing, it just watches
    // for 64 EXI clks and shoves the line high so the SDboot bit gets read as 1.
    exi_inject_offset = pio_add_program(pio_exi, &exi_inject_program);

    // The debug monitor has two programs: The parallel monitor, used to check boot0
    // codes, and the serial monitor, which is used for the text console in minute/etc
    debug_gpio_monitor_parallel_offset = pio_add_program(pio, &debug_gpio_monitor_parallel_program);
    debug_gpio_monitor_serial_offset = pio_add_program(pio, &debug_gpio_monitor_serial_program);

    // Debug monitor can start early, it's basically always on.
    debug_gpio_monitor_parallel_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_parallel_offset, PIN_DEBUGLED_BASE, PIN_DEBUGLED_COUNT, 1.0);
    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);

    // Set up DMA for reading results, but don't start it yet.
    debug_gpio_monitor_dmachan = dma_claim_unused_channel(true);
    debug_gpio_monitor_dmacfg = dma_channel_get_default_config(debug_gpio_monitor_dmachan);
    channel_config_set_transfer_data_size(&debug_gpio_monitor_dmacfg, DMA_SIZE_8);
    channel_config_set_read_increment(&debug_gpio_monitor_dmacfg, false);
    channel_config_set_write_increment(&debug_gpio_monitor_dmacfg, true);
    channel_config_set_dreq(&debug_gpio_monitor_dmacfg, pio_get_dreq(pio, debug_gpio_monitor_parallel_sm, false));
}

void de_fuse()
{
    // Disable NRST sensing and take control of the line.
    nrst_sense_set(false);

    // Make sure parallel debug monitor is on
    debug_gpio_monitor_parallel_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_parallel_offset, PIN_DEBUGLED_BASE, PIN_DEBUGLED_COUNT, 1.0);
    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);

    // winning range (at 8x27MHz): 0xB40 ~ 0x703 -- long tail? 0xbb9 ~ 0x461 (optimal: 0x900, 0x48 bytes OTP loaded)
    // winning range (at 4x27MHz): 0x5a0 ~ 0x381 -- (optimal: 0x480, 0x48 bytes OTP loaded)
    int winner = 0;
    for (int reset_attempt = (0x10000-0x480); reset_attempt < 0x10000; reset_attempt++)
    {
        winner = 0;
        printf("Starting... %u\n", reset_attempt);
        pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);
        exi_inject_program_init(pio_exi, exi_inject_sm, exi_inject_offset, PIN_CLK, PIN_DATA_BASE, 1.0);

        // Read OTP fully at least once and then hold
        // This boot does not need EXI injection, because
        // we try to make sure it ends before it hits boot1.
        gpio_put(PIN_NRST, false);
        for(int i = 0; i < 0x100; i++)
        {
            __asm volatile ("\n");
        }
        gpio_put(PIN_NRST, true);
        for(int i = 0; i < 0x800; i++)
        {
            __asm volatile ("\n");
        }
        gpio_put(PIN_NRST, false);
        for(int i = 0; i < 0x100; i++)
        {
            __asm volatile ("\n");
        }


        // The actual timing-sensitive part...
        // This won't even reach boot1, it just has to get the fuse
        // readout counter to a value >0x380 so that the next
        // boot is de_Fused.
        uint32_t cookie = save_and_disable_interrupts();
        gpio_put(PIN_NRST, true);

        for(int i = 0; i < 0x10000 - reset_attempt; i++)
        {
            __asm volatile ("\n");
        }

        // OK, we're done
        gpio_put(PIN_NRST, false);
        restore_interrupts(cookie);


        // Clear out the FIFO so our results readout is clean.
        for (int i = 0; i < 0x20; i++)
        {
            pio_sm_get(pio, debug_gpio_monitor_parallel_sm);
        }

        // Start DMAing results while we're held in reset
        memset(read_debug_gpios, 0, sizeof(read_debug_gpios));
        dma_channel_configure(
            debug_gpio_monitor_dmachan,
            &debug_gpio_monitor_dmacfg,
            read_debug_gpios,
            &pio->rxf[debug_gpio_monitor_parallel_sm],
            sizeof(read_debug_gpios),
            true // start immediately
        );

        // Make extra sure we've held in reset long enough
        // for the not-eFuse things on the chip that actually 
        // reset correctly lol
        for(int i = 0; i < 0x100; i++)
        {
            __asm volatile ("\n");
        }

        // Start EXI injection and deassert reset
        pio_sm_set_enabled(pio_exi, exi_inject_sm, true);
        gpio_put(PIN_NRST, true);

        // Give it enough time to reach boot1, then
        // disable all the debug monitor and EXI injector so we
        // can check the results
        sleep_ms(100);
        dma_channel_abort(debug_gpio_monitor_dmachan);
        pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, false);
        pio_sm_set_enabled(pio_exi, exi_inject_sm, false);

        // Winner?
        printf("Results:\n");
        for (int i = 0; i < 0x100; i++)
        {
            if (i && read_debug_gpios[i] == read_debug_gpios[i-1]) {
                break;
            }
            if (read_debug_gpios[i] == 0x88 || read_debug_gpios[i] == 0x25) {
                winner = 1;
            }
            if (read_debug_gpios[i] == 0xE1) {
                winner = 0;
            }
        }

        if (winner) {
            printf("Winner! 0x%x\n", 0x10000 - reset_attempt);
            
            for (int i = 0; i < 0x100; i++)
            {
                printf("%02x\n", read_debug_gpios[i]);
                if (i && read_debug_gpios[i] == read_debug_gpios[i-1]) {
                    break;
                }
            }
            next_wiiu_state = WIIU_STATE_DEFUSED;
            break;
        }

        //sleep_ms(1000);
    }
}

void post_de_fuse()
{
    // Switch to serial monitor and monitor NRST
    debug_gpio_monitor_serial_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_serial_offset, PIN_DEBUGLED_BASE, PIN_DEBUGLED_COUNT, 1.0);
    nrst_sense_set(true);
    memset(text_buffer, 0, sizeof(text_buffer));
    text_buffer_pos = 0;

    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);
    memset(last_16_bytes, 0, sizeof(last_16_bytes));
    serial_data_cnt = 0;
    current_mode = MONITOR_SERIAL_DATA;

    next_wiiu_state = WIIU_STATE_MONITORING;
}

void wiiu_serial_monitor()
{
    if (pio_sm_is_rx_fifo_empty(pio, debug_gpio_monitor_parallel_sm)) {
        return;
    }

    uint8_t read_val = pio_sm_get(pio, debug_gpio_monitor_parallel_sm);
    for (int i = 0; i < 15; i++)
    {
        last_16_bytes[i] = last_16_bytes[i+1];
    }
    last_16_bytes[15] = read_val;

    if (!memcmp(last_16_bytes+4, MONITOR_SERIAL_DATA_MAGIC, 12)) {
        current_mode = MONITOR_SERIAL_DATA;
        serial_data_cnt = 0;
        printf("[Pico] Switching to data mode...\n");
    }
    else if (!memcmp(last_16_bytes+4, MONITOR_SERIAL_TEXT_MAGIC, 12)) {
        current_mode = MONITOR_SERIAL_TEXT;
        printf("[Pico] Switching to text mode...\n");
        printf("%s", text_buffer); // dump text buffer in case this is just a soft reboot
        memset(text_buffer, 0, sizeof(text_buffer));
        text_buffer_pos = 0;
    }

    if (current_mode == MONITOR_SERIAL_DATA)
    {
        if (serial_data_cnt++ % 16 == 0) {
            printf("\n");
        }
        printf("%02x ", read_val);
    }
    else if (current_mode == MONITOR_SERIAL_TEXT)
    {
        if (text_buffer_pos >= sizeof(text_buffer) - 4) {
            printf("%s", text_buffer);
            memset(text_buffer, 0, text_buffer_pos);
            text_buffer_pos = 0;
        }
        if (read_val == '\t' || read_val == '\n' || read_val == '\r' || (read_val >= ' ' && read_val <= '~')) {
            if ((read_val == '\r' && last_16_bytes[14] != '\n') || read_val == '\n') {
                printf("%s\n", text_buffer);
                memset(text_buffer, 0, text_buffer_pos);
                text_buffer_pos = 0;
            }
            else {
                text_buffer[text_buffer_pos++] = read_val;
            }
        }
        else {
            sprintf(text_buffer + text_buffer_pos, "%02x", read_val);
        }
    }
    else {
        printf("Unknown mode?\n");
    }
}

void main()
{
    fast_one_time_init();

    // Keep this delay so that if the MCU hangs for whatever reason, the user
    // can unplug the console and race to flash it w/o needing the program btn
    sleep_ms(3000);
    printf("hello there\n");

    slow_one_time_init();

    printf("Start state machine.\n");
    if (gpio_get(PIN_NRST)) {
        printf("[pico] Uhhh the console is on? Monitoring...\n");
        wiiu_state = WIIU_STATE_MONITORING;
        next_wiiu_state = WIIU_STATE_MONITORING;
    }

    // State machines yayyyy
    while (1)
    {
        switch (wiiu_state)
        {
        case WIIU_STATE_POWERED_OFF:
            nrst_sense_set(true); // wait for IRQs
            break;

        // Kinda redundant since the IRQ should catch the rising edge, 
        // but juuuust to be sure.
        case WIIU_CHECK_IF_POWERED_OFF:
            sleep_ms(10);
            if (gpio_get(PIN_NRST)) {
                next_wiiu_state = WIIU_STATE_NEEDS_DEFUSE;
            }
            else {
                next_wiiu_state = WIIU_STATE_POWERED_OFF;
            }
            break;

        case WIIU_STATE_NEEDS_DEFUSE:
            de_fuse();
            break;

        case WIIU_STATE_DEFUSED:
            post_de_fuse();
            break;

        case WIIU_STATE_MONITORING:
            wiiu_serial_monitor();
            break;
        }
        if (wiiu_state != next_wiiu_state)
            printf("[pico] Changed state: %s -> %s\n", WIIU_STATE_NAMES[wiiu_state], WIIU_STATE_NAMES[next_wiiu_state]);
        wiiu_state = next_wiiu_state;
    }
}
