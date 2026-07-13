cmake_minimum_required(VERSION 3.22)

# SECoreBin is a separate executable that produces SECoreBin.bin.
# This binary is then embedded into the main SBSFU firmware via .incbin.
# Build order: SECoreBin must be built BEFORE open-lifu-transmitter-bl.

project(SECoreBin C ASM)

# SECoreBin sources from the project
set(SE_CORE_PROJECT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/se_crypto_bootloader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/se_low_level.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/data_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Startup/se_key.s
)

# SE Core middleware sources (compiled into SECoreBin, NOT into SBSFU)
set(SE_CORE_MIDDLEWARE_SOURCES
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core/se_callgate.c
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core/se_crypto_common.c
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core/se_exception.c
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core/se_fwimg.c
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core/se_startup.c
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core/se_user_application.c
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core/se_utils.c
)

# HAL drivers needed by SECoreBin (minimal set)
set(SE_CORE_HAL_SOURCES
    ${CMAKE_SOURCE_DIR}/Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_crc.c
    ${CMAKE_SOURCE_DIR}/Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_crc_ex.c
    ${CMAKE_SOURCE_DIR}/Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_flash.c
    ${CMAKE_SOURCE_DIR}/Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_flash_ex.c
    ${CMAKE_SOURCE_DIR}/Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_flash_ramfunc.c
    ${CMAKE_SOURCE_DIR}/Core/Src/system_stm32l4xx.c
)

add_executable(SECoreBin
    ${SE_CORE_PROJECT_SOURCES}
    ${SE_CORE_MIDDLEWARE_SOURCES}
    ${SE_CORE_HAL_SOURCES}
)

# Include paths for SECoreBin
target_include_directories(SECoreBin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Inc
    ${CMAKE_SOURCE_DIR}/SBSFU/App/Inc
    ${CMAKE_SOURCE_DIR}/SBSFU/Target/Inc
    ${CMAKE_SOURCE_DIR}/Linker
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Core
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Secure_Engine/Key
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Cryptographic/Fw_Crypto/STM32L4/Inc
    ${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32L4xx/Include
    ${CMAKE_SOURCE_DIR}/Drivers/STM32L4xx_HAL_Driver/Inc
    ${CMAKE_SOURCE_DIR}/Drivers/STM32L4xx_HAL_Driver/Inc/Legacy
    ${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Include
)

# Compile definitions for SECoreBin
target_compile_definitions(SECoreBin PRIVATE
    USE_HAL_DRIVER
    STM32L443xx
)

# SECoreBin uses -Os optimization as per the reference project
target_compile_options(SECoreBin PRIVATE
    -Os
    -Wno-strict-aliasing
    -ffunction-sections
)

# Linker options for SECoreBin:
# - Use SECoreBin linker script (defines SE regions)
# - Add Linker/ dir so INCLUDE directives find mapping_sbsfu.ld / mapping_fwimg.ld
# - Link against the pre-compiled STM32 Cryptographic library
target_link_options(SECoreBin PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/STM32L443xx.ld"
    -Wl,--gc-sections
    -Wl,-Map=SECoreBin.map
    -Wl,--print-memory-usage
    -Wl,--no-warn-mismatch
)

# Relink when the linker scripts change (see the matching note in the
# top-level CMakeLists.txt: Ninja cannot see through GNU ld INCLUDE, and a
# stale SECoreBin carries stale SE_IF bounds in its call-gate caller check).
set_target_properties(SECoreBin PROPERTIES LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/STM32L443xx.ld;${CMAKE_SOURCE_DIR}/Linker/mapping_sbsfu.ld;${CMAKE_SOURCE_DIR}/Linker/mapping_fwimg.ld"
)

# Add Linker/ to library search path so GNU ld INCLUDE directives find mapping_*.ld
target_link_directories(SECoreBin PRIVATE
    "${CMAKE_SOURCE_DIR}/Linker"
    ${CMAKE_SOURCE_DIR}/Middlewares/ST/STM32_Cryptographic/Fw_Crypto/STM32L4/Lib
)

target_link_libraries(SECoreBin PRIVATE
    ":libSTM32CryptographicV3.0.0_CM4_GCC.a"
    m
)

# Generate SECoreBin.bin via an OUTPUT-based custom command so that Ninja has an
# explicit build rule for the .bin file.  A POST_BUILD side-effect is not visible
# to Ninja as a rule, which causes 'missing and no known rule' when another target
# uses OBJECT_DEPENDS on this file.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/SECoreBin.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:SECoreBin>
            ${CMAKE_CURRENT_BINARY_DIR}/SECoreBin.bin
    DEPENDS SECoreBin
    COMMENT "Generating SECoreBin.bin"
    VERBATIM
)

# Custom target that drives the OUTPUT rule above and can be listed as a
# dependency of the main firmware target.
add_custom_target(SECoreBin_bin ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/SECoreBin.bin
)
