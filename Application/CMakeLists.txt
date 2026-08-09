set(TARGET app)

set(SDK_SRC
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/misc.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_gpio.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_rcc.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_flash.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_tim.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_spi.c
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_usart.c
  ${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/c/string.c
  ${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/c/stdlib.c
)

# STM32 official USB FS device library (通用代码 verbatim)
# 注: usb_endp.c 不编译 - 端点回调由 App_Usb_HL.c 提供 (端点方向: EP1 IN/EP2 OUT)
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

# Application 私有源码
set(SOURCES
  src/main.c
  src/stm32f10x_it.c
  src/system_stm32f10x.c
  src/App_Dispatch.c
  src/App_Led.c
  src/App_Sys_CfgClock.c
  src/App_Config.c
  src/App_CustomProtocol.c
  src/App_Param.c
  src/App_Iwdg_HL.c
  src/App_Led_HL.c
  src/App_Param_HL.c
  src/App_SysTick_HL.c
  src/App_Usb.c
  src/App_Usb_HL.c
  src/App_Usb_Transport.c
  src/System_PeriphClk.c
  src/App_Motor_HL.c
  src/App_Stepper.c
  src/App_UHF.c
  src/App_UHF_HL.c
  src/App_AM.c
  src/App_AM_HL.c
  src/App_Locker.c
  Drv8434S/drv8434s.c
  App_startup_md.s
  ${SDK_SRC}
  ${STM32_USB_SRC}
)

set(SDK_INC
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/CMSIS/CM3/CoreSupport
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/CMSIS/CM3/DeviceSupport/ST/STM32F10x
  ${CMAKE_SOURCE_DIR}/Sdk/STM32F10x_StdPeriph_Lib_V3.6.0/Libraries/STM32F10x_StdPeriph_Driver/inc
  ${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/c
  inc
  ${CMAKE_CURRENT_SOURCE_DIR}/Drv8434S
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
  -T${CMAKE_CURRENT_SOURCE_DIR}/ld/App_STM32F103C8.ld
  -L${CMAKE_SOURCE_DIR}/Sdk/arm-none-eabi/arm-none-eabi/lib
  -Wl,--gc-sections
  -Wl,--print-memory-usage
  -nostartfiles
  -lc
  -lgcc
)

add_custom_command(TARGET ${TARGET}.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}.elf>
          ${CMAKE_SOURCE_DIR}/Build/app.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${TARGET}.elf>
          ${CMAKE_SOURCE_DIR}/Build/app.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${TARGET}.elf>
  COMMENT "App output: app.elf / app.bin / app.hex (timestamped by gen_firmware.sh)"
)
