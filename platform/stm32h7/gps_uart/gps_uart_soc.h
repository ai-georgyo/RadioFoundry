// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7 half of gps_uart.c, included there once: the USART's registers (separate status, data and flag clear
// registers; the FIFO stays off, so it is one character at a time as on the other family), its clock gate and its
// rate. Uses cfg of the including file. No DMA: ES0392 2.20.4 (a DMA stream locked by a USART) has nothing to lock.

// The status word's bits (USART_ISR): a flagged character, a character received
#define USART_RX_ERRORS		(USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)
#define USART_RX_READY		USART_ISR_RXNE_RXFNE

// From the receive interrupt: the status word, with the data byte in *c. Reading the data register takes the
// character; the error flags stay until they are written to the clear register, and an overrun left set would bring
// the interrupt back for ever. The three flags have the same bit in both registers
_Static_assert(USART_ICR_ORECF == USART_ISR_ORE && USART_ICR_FECF == USART_ISR_FE && USART_ICR_NECF == USART_ISR_NE,
	       "the error flags' clear bits are at the flags' positions");
static inline uint32_t usart_take(USART_TypeDef *u, uint8_t *c)
{
	uint32_t isr = u->ISR;
	*c = (uint8_t)u->RDR;
	u->ICR = isr & (USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF);
	return isr;
}

static bool on_apb2(const USART_TypeDef *u)
{
	return u == USART1 || u == USART6;
}

static void clock_on(bool on)
{
	USART_TypeDef *u = cfg->uart;
	uint32_t bit = u == USART1 ? RCC_APB2ENR_USART1EN : u == USART6 ? RCC_APB2ENR_USART6EN :
		       u == USART2 ? RCC_APB1LENR_USART2EN : u == USART3 ? RCC_APB1LENR_USART3EN :
		       u == UART4 ? RCC_APB1LENR_UART4EN : RCC_APB1LENR_UART5EN;
	volatile uint32_t *en = on_apb2(u) ? &RCC->APB2ENR : &RCC->APB1LENR;
	if (on) {
		*en |= bit;
	} else {
		*en &= ~bit;
	}
	(void)*en;
}

// The USART off, set to the rate, then receiving with its interrupt on. Switching it off also clears its status
// flags (RM0433, USART_CR1 UE), so nothing of the old rate is left to read. The rate is counted from the bus clock:
// the USARTs' kernel clock selections are left at their reset value, the bus clock (the platform's clock.c), and the
// block's own prescaler at its reset value, 1
static void uart_rate(uint32_t rate)
{
	USART_TypeDef *u = cfg->uart;
	uint32_t pclk = on_apb2(u) ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
	u->CR1 = 0;
	u->BRR = (pclk + rate / 2u) / rate;	// oversampling by 16
	u->CR2 = 0;
	u->CR3 = 0;
	u->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE;
}

static inline void usart_off(USART_TypeDef *u)
{
	u->CR1 = 0;
}
