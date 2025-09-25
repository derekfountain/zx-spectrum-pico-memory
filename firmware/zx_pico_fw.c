/*
 * ZX-Pico RAM Emulation, a Raspberry Pi Pico based Spectrum DRAM device
 * Copyright (C) 2025 Derek Fountain, Andrew Menadue
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
 * export PICO_SDK_PATH=/home/derek/BEETLE/Derek/dev/Pico/pico-sdk-v2.x
 *
 * cmake -DPICO_BOARD=pico2 ..
 * cmake -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico2 ..
 * make -j10
 *
 * sudo openocd -f interface/picoprobe.cfg -f target/rp2040.cfg -c "program ./pico1.elf verify reset exit"
 * sudo openocd -f interface/picoprobe.cfg -f target/rp2040.cfg
 * sudo openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" -c "program zx_pico_fw.elf verify reset exit"
 * sudo openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000"
 *
 * gdb-multiarch ./pico1.elf
 *  target remote localhost:3333
 *  load
 *  monitor reset init
 *  continue
 */

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
//#define OVERCLOCK 270000
//#define OVERCLOCK 312000
#define OVERCLOCK 360000

/* I think a NOP on the RP2350 runs in half a clock cycle? */
#define _10_NOPS_  __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop"); \
                   __asm volatile ("nop")


#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/binary_info.h"
#include "hardware/clocks.h"
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
#if OVERCLOCK > 312000
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

  /* Init data bus GPIOs. D0 comes from IC6 on the ZX PCB */
  gpio_init( D0_GP );
  gpio_init( D1_GP );
  gpio_init( D2_GP );
  gpio_init( D3_GP );
  gpio_init( D4_GP );
  gpio_init( D5_GP );
  gpio_init( D6_GP );
  gpio_init( D7_GP );

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
  uint16_t ras_abus = 0;

  /*
   * Malloc a store for the ZX memory. We only need bytes because we only need to store
   * the 8 bits of the data bus, but it's quicker to store the entire GPIO space for each
   * databus read which is 29 bits in a uint32. The unused bits take up room, but that's
   * less inefficient than trying to mask out the ones we need.
   */
  uint32_t *store_ptr = malloc(STORE_SIZE*sizeof(uint32_t));

  uint32_t previous_gpios = STROBE_MASK;

  uint32_t gpios_state;
  uint16_t addr_requested = 0;
  while(1)
  {
    /* gpios_state is state of all 29 GPIOs in one value */

    /* This loop escapes in about 35ns after RAS or CAS goes low (270MHz) */
    while( (previous_gpios & ( ~((gpios_state = gpio_get_all())) & STROBE_MASK )) == 0 )
      previous_gpios = gpios_state;
//gpio_put( TEST_OUTPUT_GP, 1 );
//__asm volatile ("nop");
//gpio_put( TEST_OUTPUT_GP, 0 );
  
    /* This condition is 2 instructions */
    if( ((gpios_state & CAS_GP_MASK) == 0) )
    {

      /* CAS low edge found. */

      /* 40ns (360MHz), 50ns (270MHz) after CAS */

      /*
       * Column address is on the address bus. Add that to the row address we've
       * already got (and already done the multiply on) and we now have the whole
       * address of the relevant data in our store.
       */

      /* gpios_state is from the point CAS went low */
      if( gpios_state & WR_GP_MASK )
      {
	/* 50ns (360MHz), 90ns (270MHz) after CAS */

	/*
	 * A read. Get the data from store and get it on the bus before
	 * CAS goes back up
	 */
	  
	/* Prime the level shifter to send to the ZX. gpio_put(DIR_GP, 0); required, clr_mask is faster */
	gpio_clr_mask(DIR_GP_MASK);

	/* 55ns (360MHz) after CAS */

	/*
	 * Switch the data bus GPIOs to point towards the ZX - we do this last to prevent
	 * pico outputs driving the shifter while that's still in output mode
	 */
	gpio_set_dir_out_masked( DBUS_GP_MASK );

	/* 60ns (360MHz), 110ns (270MHz) after CAS */

	/* Put the stored value on the output data bus - this is the slow bit */
	gpio_put_masked( DBUS_GP_MASK, *(store_ptr+(addr_requested + (uint8_t)(gpios_state & ADDR_GP_MASK))) );

        /* Data is available, 120ns (360MHz), 130ns (270MHz) after CAS. Question mark on the 360MHZ value here */
       
	/* Wait for RAS or CAS to go high indicating ZX has picked up the data */
	while( (gpio_get_all() & STROBE_MASK) == 0 );

	/* Switch the data bus GPIOs back to pointing from ZX toward the pico */
	gpio_set_dir_in_masked( DBUS_GP_MASK );

	/* Put the level shifters back to reading from the ZX */
	gpio_put(DIR_GP, 1);

	/* 245ns (360MHz) after CAS fell, 40ns after CAS rose again */

	/*
	 * CAS has gone up showing ZX has collected our data. At this point
	 * in page mode CAS is just about to go low again. Not much time, we
	 * need to get back to the top of the loop and pick up the next
	 * column address which is going on the bus just about now.
	 * CAS stays high for about 75ns.
	 */
	
	/* Re-read the GPIOs state, CAS is high now */
	gpios_state = gpio_get_all();
      }
      else
      {
	/* 75ns (270MHz) after CAS */

	/*
	 * A write. We already have the data from the bus, so store it.
	 * We had the row address, now we have the column address as well.
	 *
	 * The ULA doesn't do writes, only the Z80. The Z80 runs at a much less
	 * demanding speed than the ULA, so timings here don't matter too much
	 */

	/* Store the entire value from the GPIOs, masking is done on the read cycle */
        addr_requested += (uint8_t)(gpios_state & ADDR_GP_MASK);
	*(store_ptr+addr_requested) = gpios_state;

	/*
	 * 65ns (360MHz), 100ns (270MHz) after CAS. The write is complete
	 * and we loop back to get the next RAS. There might be a little
	 * time to spare here.
	 */
      }

    }
    else if( (gpios_state & RAS_GP_MASK) == 0 )
    {
      /* RAS was high and it's gone low - there's a new row value on the address bus */
      
      /* 55ns (270MHz) after RAS */

      /*
       * Pick up the address bus value.
       */
      addr_requested = (uint16_t)(128 * (uint8_t)(gpios_state & ADDR_GP_MASK));
    
      /*
       * 60ns (270MHz) after RAS.
       *
       * We've got the row part of the address, now loop back to the top
       * and wait for CAS to go low.
       *
       * The ULA book says (pg122, para 3) that RAS to CAS delay is 78ns
       * for ULA video memory reads. According to my 'scope it's always
       * between 92ns and 100ns, including the blipping of the test signal
       * GPIO line, which adds 6 instructions (up, nop, down, twice). So
       * that's 17ns at 360MHz, or 22ns at 270MHz. 100ns less 22ns is 78ns,
       * so that's just about right when the test blips are removed.
       */
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


gpio_put( TEST_OUTPUT_GP, 1 );
__asm volatile ("nop");
gpio_put( TEST_OUTPUT_GP, 0 );

#endif

