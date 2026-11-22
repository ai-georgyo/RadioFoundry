// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 half of gps_uart.c, included there once: the USART's registers (a status and a data register, no
// FIFO), its clock gate and its rate. Uses cfg of the including file.

// The status word's bits (USART_SR): a flagged character, a character received
#define USART_RX_ERRORS		(USART_SR_ORE | USART_SR_FE | USART_SR_NE)
#define USART_RX_READY		USART_SR_RXNE

// From the receive interrupt: the status word, with the data byte in *c
static inline uint32_t usart_take(USART_TypeDef *u, uint8_t *c)
{
	uint32_t sr = u->SR;
	*c = (uint8_t)u->DR;	// reading SR then DR clears RXNE and the error flags
	return sr;
}

static bool on_apb2(const USART_TypeDef *u)
{
	return u == USART1 || u == USART6;
}

static void clock_on(bool on)
{
	USART_TypeDef *u = cfg->uart;
	uint32_t bit = u == USART1 ? RCC_APB2ENR_USART1EN : u == USART6 ? RCC_APB2ENR_USART6EN :
		       u == USART2 ? RCC_APB1ENR_USART2EN : u == USART3 ? RCC_APB1ENR_USART3EN :
		       u == UART4 ? RCC_APB1ENR_UART4EN : RCC_APB1ENR_UART5EN;
	volatile uint32_t *en = on_apb2(u) ? &RCC->APB2ENR : &RCC->APB1ENR;
	if (on) {
		*en |= bit;
	} else {
		*en &= ~bit;
	}
	(void)*en;
}

// The USART off, set to the rate, then receiving with its interrupt on. Inlined into its one caller in the including
// file, where this code was before the families were split: the same code as then
static inline __attribute__((always_inline)) void uart_rate(uint32_t rate)
{
	USART_TypeDef *u = cfg->uart;
	uint32_t pclk = on_apb2(u) ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
	u->CR1 = 0;
	u->BRR = (pclk + rate / 2u) / rate;	// oversampling by 16
	u->CR2 = 0;
	u->CR3 = 0;
	(void)u->SR;
	(void)u->DR;
	u->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_RXNEIE;
}

static inline void usart_off(USART_TypeDef *u)
{
	u->CR1 = 0;
}
