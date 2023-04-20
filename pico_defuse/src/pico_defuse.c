#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico_defuse.pio.h"

const uint PIN_DEBUGLED_BASE = 2;
const uint PIN_DEBUGLED_COUNT = 8;

const uint PIN_LED = 25;                // Status LED
const uint PIN_DATA_BASE = 10;          // Base pin used for output, 4 consecutive pins are used 
const uint PIN_NRST = 15;               // Wii U reset
const uint PIN_CLK = 14;                // EXI bus clock line

uint8_t read_debug_gpios[0x100] = {0};

uint32_t __in_flash("ipl_data") ipl[]  = {0x0};

void main()
{
    stdio_init_all();

    // LED on for now to indicate life or something
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, true);

    // TODO: figure out the bare minimum I can pull off (maybe downclock later?)
    // 250MHz seems to capture debug pin transistions, which was the ~status quo
    // on my FPGA at 27MHz
    set_sys_clock_khz(250000, true);

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
    uint sm = 0;
    uint dma_chan = 0;

    // Logic
    uint debug_gpio_monitor_parallel_sm = pio_claim_unused_sm(pio, true);

    debug_gpio_monitor_parallel_program_init(pio, debug_gpio_monitor_parallel_sm, PIN_DEBUGLED_BASE, PIN_DEBUGLED_COUNT, 1.0);

    //pio_sm_put(pio, debug_gpio_monitor_parallel_sm, (uint32_t) 224); // CS pulses
    //pio_sm_exec(pio, debug_gpio_monitor_parallel_sm, pio_encode_pull(true, true));
    //pio_sm_exec(pio, debug_gpio_monitor_parallel_sm, pio_encode_mov(pio_x, pio_osr));
    //pio_sm_exec(pio, debug_gpio_monitor_parallel_sm, pio_encode_out(pio_null, 32));

    // Set up DMA for reading results
    int chan = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, debug_gpio_monitor_parallel_sm, false));

    dma_channel_configure(
        chan,
        &c,
        read_debug_gpios,
        &pio->rxf[debug_gpio_monitor_parallel_sm],
        0x20, //  sizeof(read_debug_gpios)
        true // start immediately
    );

    sleep_ms(3000);

    pio_sm_set_enabled(pio, debug_gpio_monitor_parallel_sm, true);
    
    int round = 0;
    while (1)
    {
        printf("Waiting... %x\n", round++);
        dma_channel_wait_for_finish_blocking(chan);

        for (int i = 0; i < 0x20; i++)
        {
            //read_debug_gpios[i] = pio_sm_get_blocking(pio, debug_gpio_monitor_parallel_sm);
            printf("%02x\n", read_debug_gpios[i]);
        }
        memset(read_debug_gpios, 0, sizeof(read_debug_gpios));
        //sleep_ms(1000);
        //dma_channel_start(chan);
        dma_channel_configure(
            chan,
            &c,
            read_debug_gpios,
            &pio->rxf[debug_gpio_monitor_parallel_sm],
            0x20, //  sizeof(read_debug_gpios)
            true // start immediately
        );
    }

#if 0
    //
    // State Machine: Transfer Start
    //
    // Counts all consecutive transfers and sets IRQ 
    // when first 1 kilobyte transfer starts.
    //

    uint transfer_start_sm = pio_claim_unused_sm(pio, true);
    uint transfer_start_offset = pio_add_program(pio, &on_transfer_program);

    on_transfer_program_init(pio, transfer_start_sm, transfer_start_offset, PIN_CLK, PIN_CS, PIN_DATA_BASE);

    pio_sm_put(pio, transfer_start_sm, (uint32_t) 224); // CS pulses
    pio_sm_exec(pio, transfer_start_sm, pio_encode_pull(true, true));
    pio_sm_exec(pio, transfer_start_sm, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec(pio, transfer_start_sm, pio_encode_out(pio_null, 32));

    //
    // State Machine: Clocked Output
    // 
    // It waits for IRQ signal from first SM and samples clock signal
    // to output IPL data bits.
    //

    uint clocked_output_sm = pio_claim_unused_sm(pio, true);
    uint clocked_output_offset = pio_add_program(pio, &clocked_output_program);

    clocked_output_program_init(pio, clocked_output_sm, clocked_output_offset, PIN_DATA_BASE, PIN_CLK, PIN_CS);

    pio_sm_put(pio, clocked_output_sm, 8191); // 8192 bits, 1024 bytes, minus 1 because counting starts from 0 
    pio_sm_exec(pio, clocked_output_sm, pio_encode_pull(true, true));
    pio_sm_exec(pio, clocked_output_sm, pio_encode_mov(pio_y, pio_osr));
    pio_sm_exec(pio, clocked_output_sm, pio_encode_out(pio_null, 32));

    // Start PIO state machines
    pio_sm_set_enabled(pio, transfer_start_sm, true);
    pio_sm_set_enabled(pio, clocked_output_sm, true);
#endif

    gpio_init(PIN_NRST);
    gpio_set_dir(PIN_NRST, GPIO_OUT);
    gpio_put(PIN_NRST, true);

    /*
    gpio_put(PIN_NRST, true);
        gpio_put(PIN_LED, true);
        sleep_ms(1000);
        gpio_put(PIN_NRST, false);
        gpio_put(PIN_LED, false);
        sleep_ms(1000);
    */

    while (true) {
        printf("Hello, world!\n");
        tight_loop_contents();
    }
}
