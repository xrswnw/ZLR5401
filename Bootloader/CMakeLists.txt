set(TARGET boot)

set(SDK_SRC
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/misc.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_gpio.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_rcc.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_flash.c
  ${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/c/string.c
  ${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/c/stdlib.c
)

# STM32 official USB FS device library
set(STM32_USB_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_core.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_regs.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_int.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_istr.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_mem.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_pwr.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_init.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_desc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/src/usb_prop.c
)

# Bootloader 私有源码 (自包含, 不依赖 Common)
set(SOURCES
  src/main.c
  src/Boot_Dispatch.c
  src/stm32f10x_it.c
  src/system_stm32f10x.c
  src/Boot_Config.c
  src/Boot_CustomProtocol.c
  src/Boot_Param.c
  src/Boot_Led.c
  src/Boot_Iwdg_HL.c
  src/Boot_FaultStorm.c
  src/System_PeriphClk.c
  src/Boot_Led_HL.c
  src/Boot_Param_HL.c
  src/Boot_SysTick_HL.c
  src/Boot_Usb.c
  src/Boot_Usb_HL.c
  Boot_startup_md.s
  ${SDK_SRC}
  ${STM32_USB_SRC}
)

set(SDK_INC
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/CMSIS/CM3/CoreSupport
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/CMSIS/CM3/DeviceSupport/ST/STM32F10x
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/inc
  ${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/c
  inc
  ${CMAKE_CURRENT_SOURCE_DIR}/STM32_USB/inc
)

add_executable(${TARGET}.elf ${SOURCES})
target_include_directories(${TARGET}.elf PRIVATE ${SDK_INC})

target_compile_definitions(${TARGET}.elf PRIVATE
  STM32F10X_MD
  USE_STDPERIPH_DRIVER
  HSE_VALUE=12000000
  HSE_STARTUP_TIMEOUT=0xFFFF
)

target_compile_options(${TARGET}.elf PRIVATE
  -g -Os -ffunction-sections -fdata-sections -fno-common
)

target_link_options(${TARGET}.elf PRIVATE
  -T${CMAKE_CURRENT_SOURCE_DIR}/ld/Boot_STM32F103C8.ld
  -L${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/lib
  -Wl,--gc-sections
  -Wl,--print-memory-usage
  -nostartfiles
  -lc
  -lgcc
)

add_custom_command(TARGET ${TARGET}.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}.elf>
          ${CMAKE_SOURCE_DIR}/Build/${TARGET}.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${TARGET}.elf>
          ${CMAKE_SOURCE_DIR}/Build/${TARGET}.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${TARGET}.elf>
)
