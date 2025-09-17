#define RECORD_MODE 1

#if RECORD_MODE
#define USE_STDIO 0
#define OVERCLOCK 270000
#else
#define USE_STDIO 1
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/binary_info.h"
#include "hardware/vreg.h"
#include <hardware/sync.h>
#include <hardware/flash.h>

const uint8_t LED_PIN = PICO_DEFAULT_LED_PIN;

/* These pin values are the GPxx ones in green background on the pinout diagram */

/* Lowest 7 bits in the result from gpio_get_all() */
const uint8_t A0_GP         = 0;
const uint8_t A1_GP         = 1;
const uint8_t A2_GP         = 2;
const uint8_t A3_GP         = 3;
const uint8_t A4_GP         = 4;
const uint8_t A5_GP         = 5;
const uint8_t A6_GP         = 6;
/* A7 is currently unused but it's on the hardware design in case we ever need it */

const uint8_t RAS_GP        = 19;  /* GP19, pin 25 on Pico, 5th bottom one, on the right side */
const uint8_t CAS_GP        = 18;  /* GP18, pin 24 on Pico, 4th bottom one, on the right side */
const uint8_t WR_GP         = 17;  /* GP17, pin 22 on Pico, 2nd bottom one, on the right side */

/* Keep the address bus in the lowest GPIOs so no rotation is required to read it */
const uint32_t ADDR_GP_MASK = (1<<A0_GP |
                               1<<A1_GP |
                               1<<A2_GP |
                               1<<A3_GP |
                               1<<A4_GP |
                               1<<A5_GP |
                               1<<A6_GP);

const uint32_t RAS_GP_MASK   = (1<<RAS_GP);
const uint32_t CAS_GP_MASK   = (1<<CAS_GP);
const uint32_t WR_GP_MASK    = (1<<WR_GP);
const uint32_t STROBE_MASK   = (RAS_GP_MASK | CAS_GP_MASK);

const uint8_t TEST_OUTPUT_GP  = 28;
const uint8_t SWITCH_INPUT_GP = 15;

typedef struct
{
  uint8_t  ras_addr;
  uint8_t  cas_addr;
  uint8_t  wr;
} TRACE_ENTRY;
const uint32_t NUM_TRACE_ENTRIES = 50000;
const uint32_t STORE_SIZE        = NUM_TRACE_ENTRIES*sizeof(TRACE_ENTRY);

void dump_trace(void)
{
#if USE_STDIO
  stdio_init_all();

  while(1)
  {
    TRACE_ENTRY *trace_table = (TRACE_ENTRY*)(XIP_BASE+0x10000);
    uint32_t trace_index;

    printf("Trace table start\n");
    printf("=================\n");
    for(trace_index=0; trace_index<NUM_TRACE_ENTRIES; trace_index++)
    {
      if( (trace_table+trace_index)->wr == 0xFF )
	break;

      printf("%06d: RAS addr: 0x%02X, CAS addr: 0x%02X, Addr: 0x%04X, WR: %s\n",
	     trace_index,
	     (trace_table+trace_index)->ras_addr,
	     (trace_table+trace_index)->cas_addr,
	     (trace_table+trace_index)->ras_addr*128 + (trace_table+trace_index)->cas_addr,
	     (trace_table+trace_index)->wr ? "RD" : "WR");
    }
    printf("Trace table end\n");
    printf("===============\n");
    stdio_flush();
    sleep_ms(10*1000);
  }
#endif

  while(1)
  {
    gpio_put(LED_PIN, 1); busy_wait_us_32(1000000);
    gpio_put(LED_PIN, 0); busy_wait_us_32(1000000);
  }
}

int main()
{
  bi_decl(bi_program_description("ZX Spectrum Address Bus Tester binary."));

  /* Mustn't destroy a trace by overwriting it, default to showing whatever's there */
  gpio_init( SWITCH_INPUT_GP ); gpio_set_dir(SWITCH_INPUT_GP, GPIO_IN); gpio_pull_down( SWITCH_INPUT_GP );
  if( gpio_get(SWITCH_INPUT_GP) == 0 )
  {
    dump_trace();
  }

#ifdef OVERCLOCK
  set_sys_clock_khz( OVERCLOCK, 1 );
#endif

#if USE_STDIO
  stdio_init_all();
#endif

  /* All interrupts off if we're not using the USB */
#if !USE_STDIO
  irq_set_mask_enabled( 0xFFFFFFFF, 0 );
#endif

  /* All our GPIOs are inputs, except... */
  gpio_set_dir_in_masked( 0x1FFFFFFF );

  /* ...my result blipper pin, which are outputs from the pico */
  gpio_init(TEST_OUTPUT_GP);
  gpio_set_dir(TEST_OUTPUT_GP, GPIO_OUT);
  gpio_put(TEST_OUTPUT_GP, 0);

  /* Pull the buses to zeroes */
  gpio_init( A0_GP ); gpio_set_dir(A0_GP, GPIO_IN); gpio_pull_down( A0_GP );
  gpio_init( A1_GP ); gpio_set_dir(A1_GP, GPIO_IN); gpio_pull_down( A1_GP );
  gpio_init( A2_GP ); gpio_set_dir(A2_GP, GPIO_IN); gpio_pull_down( A2_GP );
  gpio_init( A3_GP ); gpio_set_dir(A3_GP, GPIO_IN); gpio_pull_down( A3_GP );
  gpio_init( A4_GP ); gpio_set_dir(A4_GP, GPIO_IN); gpio_pull_down( A4_GP );
  gpio_init( A5_GP ); gpio_set_dir(A5_GP, GPIO_IN); gpio_pull_down( A5_GP );
  gpio_init( A6_GP ); gpio_set_dir(A6_GP, GPIO_IN); gpio_pull_down( A6_GP );

  /* The control signals are active low, so pull them high */
  gpio_init( RAS_GP ); gpio_set_dir(RAS_GP, GPIO_IN); gpio_pull_up( RAS_GP );
  gpio_init( CAS_GP ); gpio_set_dir(CAS_GP, GPIO_IN); gpio_pull_up( CAS_GP );
  gpio_init( WR_GP );  gpio_set_dir(WR_GP,  GPIO_IN); gpio_pull_up( WR_GP );

  register TRACE_ENTRY *trace_table = malloc(STORE_SIZE);
  register uint32_t     trace_index = 0;
  for( trace_index=0; trace_index<NUM_TRACE_ENTRIES; trace_index++ )
    (trace_table+trace_index)->wr = 0xFF;

  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  gpio_put(LED_PIN, 1);

  register uint32_t previous_gpios = STROBE_MASK;
  register uint16_t ras_addr = 0xFFFF;
  register uint32_t gpios_state;
  trace_index = 0;
  while(1)
  {
    /* gpios_state is state of all 29 GPIOs in one value */

    while( (previous_gpios & (~ ((gpios_state = gpio_get_all()) ) & STROBE_MASK )) == 0 )
    {
#if USE_STDIO
      printf("Main loop, GPIOs %08X, gpios_state & STOBE_MASK %08X, saved RAS addr is 0x%04X\n",
	     gpios_state, gpios_state & STROBE_MASK, ras_addr);
      sleep_ms(500);
#endif
      previous_gpios = gpios_state;
    }
#if USE_STDIO
    printf("Escaped main loop, GPIOs %08X\n", gpios_state);
#endif
      gpio_put( TEST_OUTPUT_GP, 1 );
      __asm volatile ("nop");
      __asm volatile ("nop");
      __asm volatile ("nop");
      __asm volatile ("nop");
      __asm volatile ("nop");
      gpio_put( TEST_OUTPUT_GP, 0 );

    if( ((gpios_state & CAS_GP_MASK) == 0) )
    {
      /* CAS low edge found. */

      /* gpios_state is from the point CAS went low */
      if( gpios_state & WR_GP_MASK )
      {
	/* A read */
	(trace_table+trace_index)->wr = 1;  /* WRITE is active low, so keep with that, 1 means read */

	(trace_table+trace_index)->ras_addr = ras_addr;
	(trace_table+trace_index)->cas_addr = (uint8_t)(gpios_state & ADDR_GP_MASK);

#if USE_STDIO
	printf("CAS read address GPIOs 0x%08X, saved RAS address of 0x%02X, addr is 0x%04X\n",
	       gpios_state,
	       ras_addr,
	       (ras_addr*128) + (trace_table+trace_index)->cas_addr);
#endif

	/* Wait for CAS to go high indicating ZX has picked up the data */
	while( (gpio_get_all() & CAS_GP_MASK) == 0 );
	gpios_state = gpio_get_all();
      }
      else
      {
	/* We know this is a write. From the Z80, we assume */
	(trace_table+trace_index)->wr = 0;  /* WRITE is active low, so keep with that, 0 means write */

	/* Complete address, row+column, used to access our store's data */
	(trace_table+trace_index)->ras_addr = ras_addr;
	(trace_table+trace_index)->cas_addr = (uint8_t)(gpios_state & ADDR_GP_MASK);	
      }

      if( ++trace_index == NUM_TRACE_ENTRIES )
      {
	
#if USE_STDIO
	printf("Memory buffer is 0x%08X bytes\n",STORE_SIZE);
	printf("Erasing 0x%08X bytes in flash\n",(STORE_SIZE+FLASH_SECTOR_SIZE) & 0-FLASH_SECTOR_SIZE);
	printf("Programming 0x%08X bytes in flash\n",(STORE_SIZE+256) & 0-256);
#endif

	uint32_t interrupt_mask = save_and_disable_interrupts();
	flash_range_erase(0x10000, (STORE_SIZE+FLASH_SECTOR_SIZE) & 0-FLASH_SECTOR_SIZE);
	flash_range_program(0x10000, (uint8_t*)trace_table, STORE_SIZE);
	restore_interrupts( interrupt_mask );


#if USE_STDIO
	printf("End trace table start\n");
	printf("=====================\n");
	for(trace_index=0; trace_index<NUM_TRACE_ENTRIES; trace_index++)
	{
	  if( (trace_table+trace_index)->wr == 0xFF )
	    break;

	  printf("%06d: RAS addr: 0x%04X, CAS addr: 0x%02X, Addr: 0x%04X, WR: %d\n",
		 trace_index,
		 (trace_table+trace_index)->ras_addr,
		 (trace_table+trace_index)->cas_addr,
		 (trace_table+trace_index)->ras_addr*128 + (trace_table+trace_index)->cas_addr,
		 (trace_table+trace_index)->wr);
	}
	printf("Trace table end\n");
	printf("===============\n");
#endif

	while(1)
	{
	  gpio_put(LED_PIN, 1); busy_wait_us_32(1000000);
	  gpio_put(LED_PIN, 0); busy_wait_us_32(1000000);
	}
      }
      
    }
    else if( (gpios_state & RAS_GP_MASK) == 0 )
    {
      /* RAS was high and it's gone low - there's a new row value on the address bus */

      /* Pick up the address bus value. */
      (trace_table+trace_index)->ras_addr = (uint8_t)(gpios_state & ADDR_GP_MASK);
      ras_addr = (trace_table+trace_index)->ras_addr;


#if USE_STDIO
      printf("RAS address GPIOs 0x%08X, saved address of 0x%02X\n", gpios_state, ras_addr);
#endif
    }
    
    previous_gpios = gpios_state & STROBE_MASK;

  } /* Infinite loop */

}



#if 0
/* Blip the result pin, shows on scope */
gpio_put( TEST_OUTPUT_GP, 1 ); busy_wait_us_32(5);
gpio_put( TEST_OUTPUT_GP, 0 ); busy_wait_us_32(5);

gpio_put( TEST_OUTPUT_GP, 1 ); busy_wait_us_32(1000);
gpio_put( TEST_OUTPUT_GP, 0 ); busy_wait_us_32(1000);
__asm volatile ("nop");
#endif

