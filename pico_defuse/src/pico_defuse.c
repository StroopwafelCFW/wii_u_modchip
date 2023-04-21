#include <stdio.h>
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

uint32_t __in_flash("ipl_data") ipl[]  = {0x0};

uint debug_gpio_monitor_parallel_sm;
uint exi_inject_sm;
uint exi_inject_offset;
uint debug_gpio_monitor_parallel_offset;
uint debug_gpio_monitor_serial_offset;

#define MONITOR_SERIAL_DATA (0)
#define MONITOR_SERIAL_TEXT (1)

const uint8_t MONITOR_SERIAL_DATA_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xBE, 0xEF, 0xCA, 0xFE};
const uint8_t MONITOR_SERIAL_TEXT_MAGIC[12] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xF0, 0x0F, 0xCA, 0xFE};

char text_buffer[2048];
int text_buffer_pos = 0;

void main()
{
    stdio_init_all();

    // LED on for now to indicate life or something
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, true);

    gpio_init(PIN_NRST);
    gpio_set_dir(PIN_NRST, GPIO_OUT);
    gpio_put(PIN_NRST, true);

    // TODO: figure out the bare minimum I can pull off (maybe downclock later?)
    // 250MHz seems to capture debug pin transistions, which was the ~status quo
    // on my FPGA at 27MHz. Let's do 8x27MHz to be safe.
    set_sys_clock_khz(216000, true);

    sleep_ms(3000);
    printf("Hello!\n");

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

    PIO pio = pio0;

    // Logic
    debug_gpio_monitor_parallel_sm = pio_claim_unused_sm(pio, true);
    exi_inject_sm = pio_claim_unused_sm(pio, true);

    exi_inject_offset = pio_add_program(pio, &exi_inject_program);
    debug_gpio_monitor_parallel_offset = pio_add_program(pio, &debug_gpio_monitor_parallel_program);
    debug_gpio_monitor_serial_offset = pio_add_program(pio, &debug_gpio_monitor_serial_program);

    debug_gpio_monitor_parallel_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_parallel_offset, PIN_DEBUGLED_BASE, PIN_DEBUGLED_COUNT, 1.0);
    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);

    // Set up DMA for reading results
    int chan = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, debug_gpio_monitor_parallel_sm, false));
    
    // winning range: 0xB40 ~ 0x703 -- long tail? 0xbb9 ~ 0x461
    int winner = 0;
    for (int reset_attempt = (0x10000-0x900); reset_attempt < 0x10000; reset_attempt++)
    {
        winner = 0;
        printf("Starting... %u\n", reset_attempt);
        pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);
        exi_inject_program_init(pio, exi_inject_sm, exi_inject_offset, PIN_CLK, PIN_DATA_BASE, 1.0);

        // Read OTP fully at least once and then hold
        gpio_put(PIN_NRST, false);
        for(int i = 0; i < 0x200; i++)
        {
            __asm volatile ("\n");
        }
        gpio_put(PIN_NRST, true);
        for(int i = 0; i < 0x1000; i++)
        {
            __asm volatile ("\n");
        }
        gpio_put(PIN_NRST, false);
        for(int i = 0; i < 0x200; i++)
        {
            __asm volatile ("\n");
        }

        uint32_t cookie = save_and_disable_interrupts();

        // The actual timing-sensitive part...
        gpio_put(PIN_NRST, true);
        for(int i = 0; i < 0x10000 - reset_attempt; i++)
        {
            __asm volatile ("\n");
        }
        gpio_put(PIN_NRST, false);
        for(int i = 0; i < 0x200; i++)
        {
            __asm volatile ("\n");
        }
        restore_interrupts(cookie);

        // Clear out the FIFO
        for (int i = 0; i < 0x20; i++)
        {
            pio_sm_get(pio, debug_gpio_monitor_parallel_sm);
        }

        memset(read_debug_gpios, 0, sizeof(read_debug_gpios));
        dma_channel_configure(
            chan,
            &c,
            read_debug_gpios,
            &pio->rxf[debug_gpio_monitor_parallel_sm],
            sizeof(read_debug_gpios),
            true // start immediately
        );

        pio_sm_set_enabled(pio, exi_inject_sm, true);
        gpio_put(PIN_NRST, true);
        sleep_ms(100);
        dma_channel_abort(chan);
        pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, false);
        pio_sm_set_enabled(pio, exi_inject_sm, false);

        // Winner?
        printf("Results:\n");
        for (int i = 0; i < 0x100; i++)
        {
            //read_debug_gpios[i] = pio_sm_get_blocking(pio, debug_gpio_monitor_parallel_sm);
            //printf("%02x\n", read_debug_gpios[i]);
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
                //read_debug_gpios[i] = pio_sm_get_blocking(pio, debug_gpio_monitor_parallel_sm);
                printf("%02x\n", read_debug_gpios[i]);
                if (i && read_debug_gpios[i] == read_debug_gpios[i-1]) {
                    break;
                }
            }
            break;
        }

        //sleep_ms(1000);
    }

    // Switch to serial monitor
    debug_gpio_monitor_serial_program_init(pio, debug_gpio_monitor_parallel_sm, debug_gpio_monitor_serial_offset, PIN_DEBUGLED_BASE, PIN_DEBUGLED_COUNT, 1.0);
    memset(text_buffer, 0, sizeof(text_buffer));
    text_buffer_pos = 0;

    // simple monitor
    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);
    uint8_t last_16_bytes[16] = {0};
    uint8_t serial_data_cnt = 0;
    uint8_t current_mode = MONITOR_SERIAL_DATA;
    while (1)
    {
        uint8_t read_val = pio_sm_get_blocking(pio, debug_gpio_monitor_parallel_sm);
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

    while (true) {
        //printf("Hello, world!\n");
        tight_loop_contents();
    }
}
