#pragma once

/* HSE=25MHz. Both profiles keep HCLK=250MHz, PCLK=125MHz and TIM=250MHz. */
#ifndef CMCAR_CPU_MHZ
#define CMCAR_CPU_MHZ 250
#endif
#if CMCAR_CPU_MHZ != 250 && CMCAR_CPU_MHZ != 500
#error "Only validated 250/500MHz clock profiles are supported"
#endif
#ifndef CMCAR_CPU_CACHE
#define CMCAR_CPU_CACHE 1
#endif

/* Keep synchronized with the linker DMA_RAM region and MPU region 1. */
#define PLATFORM_DMA_BASE 0x24000000u
#define PLATFORM_DMA_BYTES 8192u
