cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico_defuse C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(pico_defuse src/pico_defuse.c)

pico_generate_pio_header(pico_defuse  
        ${CMAKE_CURRENT_LIST_DIR}/src/pico_defuse.pio
)

pico_set_program_name(pico_defuse "pico_defuse")
pico_set_program_version(pico_defuse "0.1")
pico_set_binary_type(pico_defuse copy_to_ram)

if (TARGET tinyusb_device)
    pico_enable_stdio_uart(pico_defuse 0)
    pico_enable_stdio_usb(pico_defuse 1)

    target_include_directories(pico_defuse PRIVATE src)

    target_link_libraries(pico_defuse PRIVATE pico_stdlib hardware_pio hardware_dma)

    pico_add_extra_outputs(pico_defuse)

    # Serial-only build for consoles that don't need de_Fuse (e.g. ISFShax):
    # no glitching, host can reset the console via baud rate 1201.
    add_executable(pico_serial src/pico_defuse.c)
    pico_generate_pio_header(pico_serial ${CMAKE_CURRENT_LIST_DIR}/src/pico_defuse.pio)
    pico_set_program_name(pico_serial "pico_serial")
    pico_set_program_version(pico_serial "0.1")
    pico_set_binary_type(pico_serial copy_to_ram)
    pico_enable_stdio_uart(pico_serial 0)
    pico_enable_stdio_usb(pico_serial 1)
    target_include_directories(pico_serial PRIVATE src)
    target_compile_definitions(pico_serial PRIVATE
        SERIAL_ONLY=1
        PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0)
    target_link_libraries(pico_serial PRIVATE pico_stdlib hardware_pio hardware_dma)
    pico_add_extra_outputs(pico_serial)

elseif(PICO_ON_DEVICE)
    message(WARNING "not building because TinyUSB submodule is not initialized in the SDK")
endif()