#include "Boot_Param_HL.h"
#include "stm32f10x.h"

#define RAMCODE __attribute__((section(".ramcode"), used, noinline))

RAMCODE void FlashHl_Unlock(void) {
    FLASH->KEYR = 0x45670123;
    FLASH->KEYR = 0xCDEF89AB;
}

RAMCODE void FlashHl_Lock(void) {
    FLASH->CR |= FLASH_CR_LOCK;
}

RAMCODE int FlashHl_ErasePage(uint32_t addr) {
    while (FLASH->SR & FLASH_SR_BSY)
        ;
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = addr;
    FLASH->CR |= FLASH_CR_STRT;
    while (FLASH->SR & FLASH_SR_BSY)
        ;
    FLASH->CR &= ~FLASH_CR_PER;
    while (FLASH->SR & FLASH_SR_BSY)
        ;
    __asm volatile ("dsb; isb");
    return (FLASH->SR & FLASH_SR_EOP) ? 0 : -1;
}

RAMCODE int FlashHl_WriteWord(uint32_t addr, uint32_t data) {
    while (FLASH->SR & FLASH_SR_BSY)
        ;
    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint16_t *)addr = (uint16_t)data;
    while (FLASH->SR & FLASH_SR_BSY)
        ;
    *(volatile uint16_t *)(addr + 2) = (uint16_t)(data >> 16);
    while (FLASH->SR & FLASH_SR_BSY)
        ;
    FLASH->CR &= ~FLASH_CR_PG;
    while (FLASH->SR & FLASH_SR_BSY)
        ;
    __asm volatile ("dsb; isb");
    return (*(volatile uint32_t *)addr == data) ? 0 : -1;
}

RAMCODE void FlashHl_SaveBegin(void) {
    __asm volatile ("cpsid i");
    __asm volatile ("dsb; isb");
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk;
    *(volatile uint32_t *)0xE000ED28 = 0;  /* CFSR */
    *(volatile uint32_t *)0xE000ED2C = 0;
    *(volatile uint32_t *)0xE000ED34 = 0;
    __asm volatile ("dsb; isb");
    FLASH->ACR &= ~FLASH_ACR_PRFTBE;
    __asm volatile ("dsb; isb");
    FlashHl_Unlock();
}

RAMCODE void FlashHl_SaveFinish(void) {
    FlashHl_Lock();
    FLASH->ACR |= FLASH_ACR_PRFTBE;
    __asm volatile ("cpsie i");
}

RAMCODE void FlashHl_WriteBegin(void) {
    FlashHl_Unlock();
    FLASH->ACR &= ~FLASH_ACR_PRFTBE;
    __asm volatile ("dsb; isb");
}

RAMCODE void FlashHl_WriteEnd(void) {
    FLASH->ACR |= FLASH_ACR_PRFTBE;
    __asm volatile ("dsb; isb");
    FlashHl_Lock();
}
