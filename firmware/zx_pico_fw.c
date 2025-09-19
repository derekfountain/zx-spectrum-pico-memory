/*

Idealised psuedo code, without even the most basic optimisations:

change_shifter_direction( FROM_ZX )                // dbus is primed for reading

start with RAS and CAS both high

:SCAN
  current_ras := read_gpio(RAS)

  while(1)
    read GPIOs
    if CAS is low                       // CAS is always high entering this loop, so this is an edge
      break
    else if current_ras is high and RAS is low, RAS has gone low
      ras_addr := address bus on GPIOs
      current_ras := low
    else if current_ras is low and RAS is high, RAS has gone high
      current_ras := high

  cas_addr   := address bus on GPIOs
  addr := (ras_addr * 128) + cas_addr
  write_flag := WR GPIO

  if write_flag == DOING_A_READ
    change_shifter_direction( TOWARDS_ZX )
    value := store[addr]
    set_dbus_gpios( value )
    spin waiting for CAS to go high
    change_shifter_direction( FROM_ZX )
    jmp SCAN (with RAS staying low)
  else
    value := read_dbus_gpios()
    store[addr] := value
    spin waiting for RAS and CAS to go high
    jmp SCAN
  endif

  // Note: At the end of a cycle
  // For reads, the Spectrum always raises CAS before RAS.
  // For writes, the Spectrum always raises RAS before CAS.

*/


/* 1 instruction on the 133MHz microprocessor is 7.5ns */
/* 1 instruction on the 200MHz microprocessor is 5.0ns */
/* 1 instruction on the 270MHz microprocessor is 3.7ns */
/* 1 instruction on the 360MHz microprocessor is 2.8ns */

//#define OVERCLOCK 200000
//#define OVERCLOCK 250000
#define OVERCLOCK 270000
//#define OVERCLOCK 360000

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/binary_info.h"
#include "hardware/vreg.h"

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

const uint8_t D0_GP         = 8;
const uint8_t D1_GP         = 9;
const uint8_t D2_GP         = 10;
const uint8_t D3_GP         = 11;
const uint8_t D4_GP         = 12;
const uint8_t D5_GP         = 13;
const uint8_t D6_GP         = 14;
const uint8_t D7_GP         = 15;

const uint8_t RAS_GP        = 19;  /* GP19, pin 25 on Pico, 5th bottom one, on the right side */
const uint8_t CAS_GP        = 18;  /* GP18, pin 24 on Pico, 4th bottom one, on the right side */
const uint8_t WR_GP         = 17;  /* GP17, pin 22 on Pico, 2nd bottom one, on the right side */
const uint8_t DIR_GP        = 16;  /* GP16, pin 21 on Pico, bottom one, on the right side */

/* Keep the address bus in the lowest GPIOs so no rotation is required to read it */
const uint32_t ADDR_GP_MASK = (1<<A0_GP |
                               1<<A1_GP |
                               1<<A2_GP |
                               1<<A3_GP |
                               1<<A4_GP |
                               1<<A5_GP |
                               1<<A6_GP);

const uint32_t DBUS_GP_MASK = (1<<D0_GP |
                               1<<D1_GP |
                               1<<D2_GP |
                               1<<D3_GP |
                               1<<D4_GP |
                               1<<D5_GP |
                               1<<D6_GP |
                               1<<D7_GP);
const uint8_t  DBUS_ROTATE   = 8;

const uint32_t RAS_GP_MASK   = (1<<RAS_GP);
const uint32_t CAS_GP_MASK   = (1<<CAS_GP);
const uint32_t WR_GP_MASK    = (1<<WR_GP);
const uint32_t STROBE_MASK   = (RAS_GP_MASK | CAS_GP_MASK);
const uint32_t DIR_GP_MASK   = (1<<DIR_GP);

const uint8_t TEST_INPUT_GP  = 28;  /* Only use one of these */
const uint8_t TEST_OUTPUT_GP = 28;

#define STORE_SIZE 16384

int main()
{
  bi_decl(bi_program_description("ZX Spectrum Lower memory Pico board binary."));

#ifdef OVERCLOCK
#if OVERCLOCK > 270000
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(1000);
#endif
#endif

#ifdef OVERCLOCK
  set_sys_clock_khz( OVERCLOCK, 1 );
#endif

  /* All interrupts off */
  irq_set_mask_enabled( 0xFFFFFFFF, 0 );

  /* All our GPIOs are inputs, except... */
  gpio_set_dir_in_masked( 0x1FFFFFFF );

  /* ...the data direction and my result blipper pin, which are outputs from the pico */
  gpio_init(DIR_GP);
  gpio_set_dir(DIR_GP, GPIO_OUT);
  gpio_set_slew_rate(DIR_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(DIR_GP, GPIO_DRIVE_STRENGTH_12MA);
	
  gpio_init(TEST_OUTPUT_GP);
  gpio_set_dir(TEST_OUTPUT_GP, GPIO_OUT);
  gpio_put(TEST_OUTPUT_GP, 0);
  gpio_set_slew_rate(TEST_OUTPUT_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(TEST_OUTPUT_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  int signal;
  for( signal=0; signal<2; signal++ )
  {
    gpio_put(LED_PIN, 1);
    busy_wait_us_32(250000);
    gpio_put(LED_PIN, 0);
    busy_wait_us_32(250000);
  }
  gpio_put(LED_PIN, 1);

  /* Pull the buses to zeroes */
  gpio_init( A0_GP ); gpio_set_dir(A0_GP, GPIO_IN); gpio_pull_down( A0_GP );
  gpio_init( A1_GP ); gpio_set_dir(A1_GP, GPIO_IN); gpio_pull_down( A1_GP );
  gpio_init( A2_GP ); gpio_set_dir(A2_GP, GPIO_IN); gpio_pull_down( A2_GP );
  gpio_init( A3_GP ); gpio_set_dir(A3_GP, GPIO_IN); gpio_pull_down( A3_GP );
  gpio_init( A4_GP ); gpio_set_dir(A4_GP, GPIO_IN); gpio_pull_down( A4_GP );
  gpio_init( A5_GP ); gpio_set_dir(A5_GP, GPIO_IN); gpio_pull_down( A5_GP );
  gpio_init( A6_GP ); gpio_set_dir(A6_GP, GPIO_IN); gpio_pull_down( A6_GP );

  /*
   * The data bus toggles from inputs to outputs based on the Spectrum's
   * writes and reads. Strength and slew rates only apply to outputs, so
   * I'm not sure if these persist or even do anything. Worth trying.
   */
  /* D0 comes from IC6 on the ZX PCB */
  gpio_init( D0_GP );
  gpio_set_slew_rate(D0_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D0_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init( D1_GP );
  gpio_set_slew_rate(D1_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D1_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init( D2_GP );
  gpio_set_slew_rate(D2_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D2_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init( D3_GP );
  gpio_set_slew_rate(D3_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D3_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init( D4_GP );
  gpio_set_slew_rate(D4_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D4_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init( D5_GP );
  gpio_set_slew_rate(D5_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D5_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init( D6_GP );
  gpio_set_slew_rate(D6_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D6_GP, GPIO_DRIVE_STRENGTH_12MA);

  gpio_init( D7_GP );
  gpio_set_slew_rate(D7_GP, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(D7_GP, GPIO_DRIVE_STRENGTH_12MA);

  /* The control signals are active low, so pull them high */
  gpio_init( RAS_GP ); gpio_set_dir(RAS_GP, GPIO_IN); gpio_pull_up( RAS_GP );
  gpio_init( CAS_GP ); gpio_set_dir(CAS_GP, GPIO_IN); gpio_pull_up( CAS_GP );
  gpio_init( WR_GP );  gpio_set_dir(WR_GP,  GPIO_IN); gpio_pull_up( WR_GP );

  /*
   * We run with the direction set to ZX->pico unless specifically required otherwise.
   * It's important we don't try to drive the data bus unless the ZX is specifically
   * wanting us to do so (otherwise we'll corrupt the bus when the Z80 or ROM or
   * whatever is putting data on it). So we keep the level shifter in ZX->pico mode
   * unless we want to put our data on the bus.
   */
  gpio_put(DIR_GP, 1);



  /*
   * This is reset outside the main loop because it holds the row value during
   * successive page mode reads. We mustn't lose it.
   *
   * Address bus at RAS time
   */
  register uint16_t ras_abus = 0;

  /*
   * Malloc a store for the ZX memory. We only need bytes because we only need to store
   * the 8 bits of the data bus, but it's quicker to store the entire GPIO space for each
   * databus read which is 29 bits in a uint32. The unused bits take up room, but that's
   * less inefficient than trying to mask out the ones we need.
   */
  register uint32_t *store_ptr = malloc(STORE_SIZE*sizeof(uint32_t));

#define TEST_KNOWN_STORE 0
#if TEST_KNOWN_STORE
  {
    uint32_t i;

    /* Fill wth 0x03, which is no flash, no bright, black paper, white ink */
    for( i=0; i<STORE_SIZE; i++ )
      *(store_ptr+i) = (uint32_t)(0x39<<DBUS_ROTATE)&DBUS_GP_MASK;

    /*
     * Trying to set patterns in here expecting to see them on screen doesn't
     * really work. The address lines aren't lined up in the hardware as
     * A0, A1, A2, etc. Everything gets mixed up.
     */
    *(store_ptr+0) = (uint32_t)(0x07<<DBUS_ROTATE);
  }
#endif

  register uint32_t previous_gpios = STROBE_MASK;

  register uint32_t gpios_state;
  register uint16_t addr_requested = 0;
  while(1)
  {
    /* gpios_state is state of all 29 GPIOs in one value */

    while( (previous_gpios & (~ ((gpios_state = gpio_get_all()) ) & STROBE_MASK )) == 0 )
      previous_gpios = gpios_state;
  
    {
    
      /* This condition is 2 instructions */
      if( ((gpios_state & CAS_GP_MASK) == 0) )
      {
	/* 40ns (360MHz) after CAS */

	/* CAS low edge found. */

	/*
	 * Column address is on the address bus. Add that to the row address we've
	 * already got (and already done the multiply on) and we now have the whole
	 * address of the relevant data in our store.
	 */

	/* gpios_state is from the point CAS went low */
	if( gpios_state & WR_GP_MASK )
	{
	  /*
	   * We know this is a read. Get the data from store and get it on the bus before
	   * CAS goes back up
	   */
	  
	  /* 50ns (360MHz) after CAS */
	  
	  /* Prime the level shifters to send to the ZX. gpio_put(DIR_GP, 0); required, clr_mask is faster */
	  gpio_clr_mask(DIR_GP_MASK);

	  /* 55ns (360MHz) after CAS */

	  /*
	   * Switch the data bus GPIOs to point towards the ZX - we do this last to prevent
	   * pico outputs driving the shifter while that's still in output mode
	   */
	  gpio_set_dir_out_masked( DBUS_GP_MASK );

	  /* 60ns (360MHz) after CAS */

	  /* Put the stored value on the output data bus */
	  gpio_put_masked( DBUS_GP_MASK, *(store_ptr+(addr_requested + (uint8_t)(gpios_state & ADDR_GP_MASK))) );

	  /* Data is available - 4116 spec says we need to be here within 100ns of CAS going low. */

	  /* 120ns (360MHz) after CAS */
       
/* Target is to have the data on the bus at 216ns after CAS */
/* 141ns after CAS - data is already available. We have about 75ns spare here. */
//gpio_put( TEST_OUTPUT_GP, 1 ); busy_wait_us_32(5);
//gpio_put( TEST_OUTPUT_GP, 0 );

          /* Wait for CAS to go high indicating ZX has picked up the data */
          while( (gpio_get_all() & CAS_GP_MASK) == 0 );

/* 242ns after CAS - data about to be removed from the bus. */
/* That's correct, it was there at the 216ns after CAS point when dataLatch occurred */
//gpio_put( TEST_OUTPUT_GP, 1 ); busy_wait_us_32(5);
//gpio_put( TEST_OUTPUT_GP, 0 );

	  /* Switch the data bus GPIOs back to pointing from ZX toward the pico */
	  gpio_set_dir_in_masked( DBUS_GP_MASK );

	  /* Put the level shifters back to reading from the ZX */
	  gpio_put(DIR_GP, 1);

          /* 245ns (360MHz) after CAS fell, 40ns after CAS rose again */

/* 264ns after CAS - data is removed from the bus and we're going to wait for CAS again. */
//gpio_put( TEST_OUTPUT_GP, 1 ); busy_wait_us_32(5);
//gpio_put( TEST_OUTPUT_GP, 0 );

/* CAS will fall again 288ns after the original fall. That's about 24ns from now. */
__asm volatile ("nop"); // 1 NOP leave the (c) ok, so Z80 still working
__asm volatile ("nop"); // Another NOP stops the (c) working
          /*
	   * CAS has gone up showing ZX has collected our data. At this point
	   * in page mode CAS is just about to go low again. Not much time, we
	   * need to get back to the top of the loop and pick up the next
	   * column address which is going on the bus just about now.
	   * CAS stays high for about 75ns.
	   */
	}
	else
	{
	  /*
	   * We know this is a write. We already have the data from the bus, so store it.
	   */

	  /* 45ns (360MHz) after CAS */

	  /* We had the row address, now we have the column address */

	  /* Store the entire value from the GPIOs, masking is done on the read cycle */
          *(store_ptr+(addr_requested + (uint8_t)(gpios_state & ADDR_GP_MASK))) = gpios_state;

	  /*
	   * 65ns (360MHz) after CAS, 145ns (360MHz) after WR went low.
	   * WR stays low another 120ns.
	   */
	}

      }
      else if( (gpios_state & RAS_GP_MASK) == 0 )
      {
	/* 50ns (360MHz) afer RAS */

	/* RAS was high and it's gone low - there's a new row value on the address bus */

	/*
	 * Pick up the address bus value.
	 */
	addr_requested = (uint16_t)(128 * (uint8_t)(gpios_state & ADDR_GP_MASK));
    
	/* 65ns (360MHz) after RAS. We have another 35ns before CAS goes low. */
      }

    } /* endif a GPIO has gone low */


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

