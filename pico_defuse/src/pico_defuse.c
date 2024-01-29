#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/sync.h" 
#include "pico_defuse.pio.h"

#ifndef WAVESHARE_TINY
const uint PIN_DEBUGLED_BASE = 2;   // 8 consecutive pins
const uint PIN_SERIALOUT_BASE = 18; // 4 consecutive pins tied together

const uint PIN_LED = 25;                // Status LED
const uint PIN_DATA_BASE = 10;          // Base pin used for output, 4 consecutive pins are used 
const uint PIN_NRST = 15;               // Wii U reset
const uint PIN_CLK = 14;                // EXI bus clock line
#else
const uint PIN_DEBUGLED_BASE = 8;
const uint PIN_SERIALOUT_BASE = 26;

const uint PIN_DATA_BASE = 1;
const uint PIN_NRST = 6;
const uint PIN_CLK = 0;
#endif

uint8_t read_debug_gpios[0x100] = {0};

int nrst_fallback = 0;
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
#define WIIU_STATE_NORMAL_BOOT    (5)

const char* WIIU_STATE_NAMES[6] = {
    "WIIU_STATE_POWERED_OFF",
    "WIIU_STATE_NEEDS_DEFUSE",
    "WIIU_STATE_DEFUSED",
    "WIIU_STATE_MONITORING",
    "WIIU_CHECK_IF_POWERED_OFF",
    "WIIU_STATE_NORMAL_BOOT"
};

const uint32_t serial_LUT[256] = {
    0x00000000,0x0000000f,0x000000f0,0x000000ff,
    0x00000f00,0x00000f0f,0x00000ff0,0x00000fff,
    0x0000f000,0x0000f00f,0x0000f0f0,0x0000f0ff,
    0x0000ff00,0x0000ff0f,0x0000fff0,0x0000ffff,
    0x000f0000,0x000f000f,0x000f00f0,0x000f00ff,
    0x000f0f00,0x000f0f0f,0x000f0ff0,0x000f0fff,
    0x000ff000,0x000ff00f,0x000ff0f0,0x000ff0ff,
    0x000fff00,0x000fff0f,0x000ffff0,0x000fffff,
    0x00f00000,0x00f0000f,0x00f000f0,0x00f000ff,
    0x00f00f00,0x00f00f0f,0x00f00ff0,0x00f00fff,
    0x00f0f000,0x00f0f00f,0x00f0f0f0,0x00f0f0ff,
    0x00f0ff00,0x00f0ff0f,0x00f0fff0,0x00f0ffff,
    0x00ff0000,0x00ff000f,0x00ff00f0,0x00ff00ff,
    0x00ff0f00,0x00ff0f0f,0x00ff0ff0,0x00ff0fff,
    0x00fff000,0x00fff00f,0x00fff0f0,0x00fff0ff,
    0x00ffff00,0x00ffff0f,0x00fffff0,0x00ffffff,
    0x0f000000,0x0f00000f,0x0f0000f0,0x0f0000ff,
    0x0f000f00,0x0f000f0f,0x0f000ff0,0x0f000fff,
    0x0f00f000,0x0f00f00f,0x0f00f0f0,0x0f00f0ff,
    0x0f00ff00,0x0f00ff0f,0x0f00fff0,0x0f00ffff,
    0x0f0f0000,0x0f0f000f,0x0f0f00f0,0x0f0f00ff,
    0x0f0f0f00,0x0f0f0f0f,0x0f0f0ff0,0x0f0f0fff,
    0x0f0ff000,0x0f0ff00f,0x0f0ff0f0,0x0f0ff0ff,
    0x0f0fff00,0x0f0fff0f,0x0f0ffff0,0x0f0fffff,
    0x0ff00000,0x0ff0000f,0x0ff000f0,0x0ff000ff,
    0x0ff00f00,0x0ff00f0f,0x0ff00ff0,0x0ff00fff,
    0x0ff0f000,0x0ff0f00f,0x0ff0f0f0,0x0ff0f0ff,
    0x0ff0ff00,0x0ff0ff0f,0x0ff0fff0,0x0ff0ffff,
    0x0fff0000,0x0fff000f,0x0fff00f0,0x0fff00ff,
    0x0fff0f00,0x0fff0f0f,0x0fff0ff0,0x0fff0fff,
    0x0ffff000,0x0ffff00f,0x0ffff0f0,0x0ffff0ff,
    0x0fffff00,0x0fffff0f,0x0ffffff0,0x0fffffff,
    0xf0000000,0xf000000f,0xf00000f0,0xf00000ff,
    0xf0000f00,0xf0000f0f,0xf0000ff0,0xf0000fff,
    0xf000f000,0xf000f00f,0xf000f0f0,0xf000f0ff,
    0xf000ff00,0xf000ff0f,0xf000fff0,0xf000ffff,
    0xf00f0000,0xf00f000f,0xf00f00f0,0xf00f00ff,
    0xf00f0f00,0xf00f0f0f,0xf00f0ff0,0xf00f0fff,
    0xf00ff000,0xf00ff00f,0xf00ff0f0,0xf00ff0ff,
    0xf00fff00,0xf00fff0f,0xf00ffff0,0xf00fffff,
    0xf0f00000,0xf0f0000f,0xf0f000f0,0xf0f000ff,
    0xf0f00f00,0xf0f00f0f,0xf0f00ff0,0xf0f00fff,
    0xf0f0f000,0xf0f0f00f,0xf0f0f0f0,0xf0f0f0ff,
    0xf0f0ff00,0xf0f0ff0f,0xf0f0fff0,0xf0f0ffff,
    0xf0ff0000,0xf0ff000f,0xf0ff00f0,0xf0ff00ff,
    0xf0ff0f00,0xf0ff0f0f,0xf0ff0ff0,0xf0ff0fff,
    0xf0fff000,0xf0fff00f,0xf0fff0f0,0xf0fff0ff,
    0xf0ffff00,0xf0ffff0f,0xf0fffff0,0xf0ffffff,
    0xff000000,0xff00000f,0xff0000f0,0xff0000ff,
    0xff000f00,0xff000f0f,0xff000ff0,0xff000fff,
    0xff00f000,0xff00f00f,0xff00f0f0,0xff00f0ff,
    0xff00ff00,0xff00ff0f,0xff00fff0,0xff00ffff,
    0xff0f0000,0xff0f000f,0xff0f00f0,0xff0f00ff,
    0xff0f0f00,0xff0f0f0f,0xff0f0ff0,0xff0f0fff,
    0xff0ff000,0xff0ff00f,0xff0ff0f0,0xff0ff0ff,
    0xff0fff00,0xff0fff0f,0xff0ffff0,0xff0fffff,
    0xfff00000,0xfff0000f,0xfff000f0,0xfff000ff,
    0xfff00f00,0xfff00f0f,0xfff00ff0,0xfff00fff,
    0xfff0f000,0xfff0f00f,0xfff0f0f0,0xfff0f0ff,
    0xfff0ff00,0xfff0ff0f,0xfff0fff0,0xfff0ffff,
    0xffff0000,0xffff000f,0xffff00f0,0xffff00ff,
    0xffff0f00,0xffff0f0f,0xffff0ff0,0xffff0fff,
    0xfffff000,0xfffff00f,0xfffff0f0,0xfffff0ff,
    0xffffff00,0xffffff0f,0xfffffff0,0xffffffff
};

const uint8_t MONITOR_SERIAL_DATA_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xBE, 0xEF, 0xCA, 0xFE};
const uint8_t MONITOR_SERIAL_TEXT_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xF0, 0x0F, 0xCA, 0xFE};
const uint8_t MONITOR_PARALLEL_DATA_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xBE, 0xEF, 0xBE, 0xEF};
const uint8_t MONITOR_RESET_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x4E, 0x52, 0x53, 0x54};
const uint8_t MONITOR_RESET_PRSHHAX_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x50, 0x52, 0x53, 0x48};

char text_buffer[2048];
int text_buffer_pos = 0;
uint8_t last_16_bytes[16] = {0};
uint8_t serial_data_cnt = 0;
uint8_t current_mode = MONITOR_SERIAL_DATA;
uint8_t num_serial_loops = 0;

uint8_t wiiu_state = WIIU_STATE_POWERED_OFF;
uint8_t next_wiiu_state = WIIU_STATE_POWERED_OFF;
bool nrst_sense_state = false;

// Cycle-accurate JTAG enable
//#define DEFUSE_JTAG
#define DEFUSE_BYTE_UNIT (0x4)

#ifdef DEFUSE_JTAG
// Start from 0 and increase this to make it boot slightly faster,
// instead of just overshooting it for the guarantee
#define JTAG_VARIANCE (1)

#define RESET_RANGE_MIN (RESET_RANGE_MAX-(DEFUSE_BYTE_UNIT/2))
#define RESET_RANGE_MAX (0xEA8-JTAG_VARIANCE)
#else
#define RESET_RANGE_MIN (RESET_RANGE_MAX-(DEFUSE_BYTE_UNIT*0x40))
#define RESET_RANGE_MAX (0xE8C-(DEFUSE_BYTE_UNIT*0x7D))
#endif

// Empty OTP w/ JTAG disabled
//#define RESET_RANGE_MIN (RESET_RANGE_MAX-(DEFUSE_BYTE_UNIT*0x40))
//#define RESET_RANGE_MAX (0xE8C-(DEFUSE_BYTE_UNIT*0))

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
    gpio_set_function(PIN_NRST, GPIO_FUNC_SIO);

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
#ifndef WAVESHARE_TINY // The power LED on the Waveshare extension board is always on and the one on the main board is more complicated to setup
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, true);
#endif

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
    debug_gpio_monitor_parallel_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_parallel_offset, PIN_DEBUGLED_BASE, PIN_SERIALOUT_BASE, 1.0);
    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);

    // Set up DMA for reading results, but don't start it yet.
    debug_gpio_monitor_dmachan = dma_claim_unused_channel(true);
    debug_gpio_monitor_dmacfg = dma_channel_get_default_config(debug_gpio_monitor_dmachan);
    channel_config_set_transfer_data_size(&debug_gpio_monitor_dmacfg, DMA_SIZE_8);
    channel_config_set_read_increment(&debug_gpio_monitor_dmacfg, false);
    channel_config_set_write_increment(&debug_gpio_monitor_dmacfg, true);
    channel_config_set_dreq(&debug_gpio_monitor_dmacfg, pio_get_dreq(pio, debug_gpio_monitor_parallel_sm, false));
}

void do_normal_reset()
{
    // Disable NRST sensing and take control of the line.
    nrst_sense_set(false);

    // Read OTP fully at least once and then hold.
    // We do this twice just in case.
    // This boot does not need EXI injection, because
    // we try to make sure it ends before it hits boot1.
    gpio_put(PIN_NRST, false);
    for(int i = 0; i < 0x100; i++)
    {
        __asm volatile ("\n");
    }
    gpio_put(PIN_NRST, true);
    for(int i = 0; i < 0x4000; i++) // boot0 is running for 0x4000 ticks
    {
        __asm volatile ("\n");
    }
    gpio_put(PIN_NRST, false);
    for(int i = 0; i < 0x100; i++)
    {
        __asm volatile ("\n");
    }
    gpio_put(PIN_NRST, true);
    for(int i = 0; i < 0x4000; i++) // boot0 is running for 0x4000 ticks
    {
        __asm volatile ("\n");
    }
    gpio_put(PIN_NRST, false);
    for(int i = 0; i < 0x100; i++)
    {
        __asm volatile ("\n");
    }

    // Hopefully any de_Fuse quirks are flushed out, do the real reset
    gpio_put(PIN_NRST, false);
    for(int i = 0; i < 0x100; i++)
    {
        __asm volatile ("\n");
    }
    gpio_put(PIN_NRST, true);
    for(int i = 0; i < 0x4000; i++)
    {
        __asm volatile ("\n");
    }
    next_wiiu_state = WIIU_STATE_NORMAL_BOOT;
}

void de_fuse()
{
    // Disable NRST sensing and take control of the line.
    nrst_sense_set(false);

    // Make sure parallel debug monitor is on
    debug_gpio_monitor_parallel_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_parallel_offset, PIN_DEBUGLED_BASE, PIN_SERIALOUT_BASE, 1.0);
    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);

    // winning range (at 8x27MHz): 0xB40 ~ 0x703 -- long tail? 0xbb9 ~ 0x461 (optimal: 0x900, 0x48 bytes OTP loaded)
    // winning range (at 4x27MHz): 0x5a0 ~ 0x381 -- (optimal: 0x480, 0x48 bytes OTP loaded)
    int winner = 0;
    uint8_t error_code = 0;
    int only_zeros = 1;
    for (int reset_attempt = RESET_RANGE_MAX; reset_attempt >= RESET_RANGE_MIN; reset_attempt--)
    {
        for (int which = 0; which < 2; which++)
        {
            only_zeros = 1;
            winner = 0;
            error_code = 0;

            // Make sure the last attempt does not give a false success
            if (reset_attempt == RESET_RANGE_MIN && which) {
                break;
            }

            printf("Starting... %u:%u\n", reset_attempt, which);
            pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);
            exi_inject_program_init(pio_exi, exi_inject_sm, exi_inject_offset, PIN_CLK, PIN_DATA_BASE, 1.0);

            // Read OTP fully at least once and then hold.
            // We do this twice just in case.
            // This boot does not need EXI injection, because
            // we try to make sure it ends before it hits boot1.
            gpio_put(PIN_NRST, false);
            for(int i = 0; i < 0x100; i++)
            {
                __asm volatile ("\n");
            }
            gpio_put(PIN_NRST, true);
            for(int i = 0; i < 0x4000; i++)
            {
                __asm volatile ("\n");
            }
            gpio_put(PIN_NRST, false);
            for(int i = 0; i < 0x100; i++)
            {
                __asm volatile ("\n");
            }
            gpio_put(PIN_NRST, true);
            for(int i = 0; i < 0x4000; i++)
            {
                __asm volatile ("\n");
            }
            gpio_put(PIN_NRST, false);
            for(int i = 0; i < 0x100; i++)
            {
                __asm volatile ("\n");
            }

    #if 1
            // The actual timing-sensitive part...
            // This won't even reach boot1, it just has to get the fuse
            // readout counter to a value >0x380 so that the next
            // boot is de_Fused.
            uint32_t cookie = save_and_disable_interrupts();
            gpio_put(PIN_NRST, true);

            for(int i = 0; i < reset_attempt; i++)
            {
                __asm volatile ("\n");
            }

            // OK, we're done
            gpio_put(PIN_NRST, false);
            restore_interrupts(cookie);
    #endif

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
            pio_sm_set_enabled(pio_exi, exi_inject_sm, which ? false : true);
            gpio_put(PIN_NRST, true);

            // Give it enough time to reach boot1, then
            // disable all the debug monitor and EXI injector so we
            // can check the results
            if (reset_attempt <= RESET_RANGE_MIN+4) {
                sleep_ms(500);
            }
            sleep_ms(200);
            dma_channel_abort(debug_gpio_monitor_dmachan);
            pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, false);
            pio_sm_set_enabled(pio_exi, exi_inject_sm, false);

            // Winner?
            printf("Results:\n");
            for (int i = 0; i < 0x100; i++)
            {
                if (read_debug_gpios[i]) {
                    only_zeros = 0;
                }
                if (i && read_debug_gpios[i] == read_debug_gpios[i-1]) {
                    break;
                }
                if (read_debug_gpios[i] == 0x88) { // || read_debug_gpios[i] == 0x25
                    winner = 1;
                }
                if (read_debug_gpios[i] == 0xE1) {
                    winner = 0;
                }

                if (read_debug_gpios[i] >= 0xC0) {
                    error_code = read_debug_gpios[i];
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
                goto glitch_success;
            }
            else {
                for (int i = 0; i < 0x100; i++)
                {
                    printf("%02x\n", read_debug_gpios[i]);
                    if (i && read_debug_gpios[i] == read_debug_gpios[i-1]) {
                        break;
                    }
                }
                printf("Error code: %02x\n", error_code);
            }
        }
        //sleep_ms(1000);
    }

glitch_success:

    // JTAG has a much smaller window
#ifndef DEFUSE_JTAG
    // TODO: do this after trying a NAND boot1?
    // If no SD card is inserted, or an invalid one is inserted, boot0 stalls.
    if (!winner && error_code == 0x00 && !only_zeros) {
        printf("SD card not valid or not inserted, doing a normal boot.\n");
        do_normal_reset();
    }

    if (!winner && only_zeros) {
        printf("[Pico] Wii U doesn't seem to be powered on?\n");
        next_wiiu_state = WIIU_CHECK_IF_POWERED_OFF;
    }
#else
    ;
#endif
}

void post_de_fuse()
{
    // Switch to serial monitor and monitor NRST
    debug_gpio_monitor_serial_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_serial_offset, PIN_DEBUGLED_BASE, PIN_SERIALOUT_BASE, 1.0);
    nrst_sense_set(true);
    memset(text_buffer, 0, sizeof(text_buffer));
    text_buffer_pos = 0;

    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);
    memset(last_16_bytes, 0, sizeof(last_16_bytes));
    serial_data_cnt = 0;
    current_mode = MONITOR_SERIAL_TEXT;

    next_wiiu_state = WIIU_STATE_MONITORING;
}

void wiiu_serial_monitor()
{
    if (!pio_sm_is_tx_fifo_full(pio, debug_gpio_monitor_parallel_sm))
    {
        int char_in = getchar_timeout_us(0);
        if (char_in != PICO_ERROR_TIMEOUT && (char_in >= 0 && char_in <= 0xFF)) {
            //printf("[pico] char! %x\n", char_in);
            pio_sm_put(pio, debug_gpio_monitor_parallel_sm, 0xFFFFFFFF); // serial data is valid
            pio_sm_put(pio, debug_gpio_monitor_parallel_sm, serial_LUT[char_in]);
        }
    }
    
    if (pio_sm_is_rx_fifo_empty(pio, debug_gpio_monitor_parallel_sm)) {
        return;
    }

    uint8_t read_val = pio_sm_get(pio, debug_gpio_monitor_parallel_sm);
    for (int i = 0; i < 15; i++)
    {
        last_16_bytes[i] = last_16_bytes[i+1];
    }
    last_16_bytes[15] = read_val;

    if (!memcmp(last_16_bytes+4, MONITOR_RESET_MAGIC, 12))
    {
        next_wiiu_state = WIIU_STATE_NEEDS_DEFUSE;
        printf("[Pico] Console requested reset...\n");
        sleep_ms(500); // give it a delay
    }
    else if (!memcmp(last_16_bytes+4, MONITOR_RESET_PRSHHAX_MAGIC, 12))
    {
        printf("[Pico] Console requested prshhax prshhax reset...\n");
        do_normal_reset();
        sleep_ms(3000); // give it a delay
        next_wiiu_state = WIIU_STATE_NEEDS_DEFUSE;
    }
    else if (!memcmp(last_16_bytes+4, MONITOR_PARALLEL_DATA_MAGIC, 12)) {
        debug_gpio_monitor_parallel_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_parallel_offset, PIN_DEBUGLED_BASE, PIN_SERIALOUT_BASE, 1.0);
        current_mode = MONITOR_SERIAL_DATA;
        serial_data_cnt = 0;
        printf("[Pico] Switching to parallel data mode...\n");
    }
    else if (!memcmp(last_16_bytes+4, MONITOR_SERIAL_DATA_MAGIC, 12)) {
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

    num_serial_loops++;

    if (current_mode == MONITOR_SERIAL_DATA)
    {
        if (serial_data_cnt++ % 16 == 0) {
            printf("\n");
        }
        printf("%02x ", read_val);
    }
    else if (current_mode == MONITOR_SERIAL_TEXT)
    {
        if (text_buffer_pos >= sizeof(text_buffer) - 4 || num_serial_loops > 16) {
            printf("%s", text_buffer);
            memset(text_buffer, 0, text_buffer_pos);
            text_buffer_pos = 0;
            num_serial_loops = 0;
        }
        if (!read_val)
        {
            // reading input
        }
        else if (read_val == 0xAA || read_val == 0x1B || read_val == '\t' || read_val == '\n' || read_val == '\r' || (read_val >= ' ' && read_val <= '~')) {
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
            text_buffer_pos += 2;
        }
    }
    else {
        printf("Unknown mode?\n");
    }
}

void fallback_power_check()
{
    // Safety fallback
    if (nrst_sense_state) {
        if (!gpio_get(PIN_NRST)) {
            nrst_fallback++;
        }
        else {
            nrst_fallback = 0;
        }

        if (nrst_fallback > 1000) {
            printf("[pico] Fallback: Wii U was unplugged while I wasn't watching!\n");
            next_wiiu_state = WIIU_CHECK_IF_POWERED_OFF;
            nrst_fallback = 0;
        }
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
#if 0
    if (gpio_get(PIN_NRST)) {
        printf("[pico] Uhhh the console is on? Monitoring...\n");
        wiiu_state = WIIU_STATE_DEFUSED;
        next_wiiu_state = WIIU_STATE_DEFUSED;
    }
#endif
    if (gpio_get(PIN_NRST)) {
        printf("[pico] Uhhh the console is on? Defusing now...?\n");
        wiiu_state = WIIU_STATE_NEEDS_DEFUSE;
        next_wiiu_state = WIIU_STATE_NEEDS_DEFUSE;
    }

    // State machines yayyyy
    while (1)
    {
        switch (wiiu_state)
        {
        case WIIU_STATE_NORMAL_BOOT:
            nrst_sense_set(true); // wait for IRQs
            fallback_power_check();
            break;

        case WIIU_STATE_POWERED_OFF:
            nrst_sense_set(true); // wait for IRQs
            break;

        // Kinda redundant since the IRQ should catch the rising edge, 
        // but juuuust to be sure.
        case WIIU_CHECK_IF_POWERED_OFF:
            nrst_sense_set(true);
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
            //next_wiiu_state = WIIU_STATE_DEFUSED;
            break;

        case WIIU_STATE_DEFUSED:
            post_de_fuse();
            break;

        case WIIU_STATE_MONITORING:
            wiiu_serial_monitor();
            fallback_power_check();
            break;
        }

        

        if (wiiu_state != next_wiiu_state)
            printf("[pico] Changed state: %s -> %s\n", WIIU_STATE_NAMES[wiiu_state], WIIU_STATE_NAMES[next_wiiu_state]);
        wiiu_state = next_wiiu_state;
    }
}
