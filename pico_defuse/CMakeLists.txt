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

elseif(PICO_ON_DEVICE)
    message(WARNING "not building because TinyUSB submodule is not initialized in the SDK")
endif()