/**
 * DmxSimple - A simple interface to DMX.
 *
 * Copyright (c) 2008-2009 Peter Knight, Tinker.it! All rights reserved.
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include "DmxSimple.h"

/** DmxSimple access the output pin directly for speed.
 * The output port, data direction register and bit are specified here.
 * This is board specific.
 */
#if defined(__AVR_ATmega1280__)
#define DMX_PORT PORTE
#define DMX_BIT 5
#define DMX_DDR DDRE
#else
#define DMX_PORT PORTD
#define DMX_BIT 3
#define DMX_DDR DDRD
#endif

/** Comparative math macros
 */
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))

/** dmxBuffer contains a software copy of all the DMX channels.
  */
volatile uint8_t dmxBuffer[DMX_SIZE];
static uint16_t dmxMax = 16; /* Default to sending the first 16 channels */
static uint8_t dmxStarted = 0;
static uint16_t dmxState = 0;

void dmxBegin();
void dmxEnd();
void dmxSendByte(volatile uint8_t);
void dmxWrite(int,uint8_t);
void dmxMaxChannel(int);


/** Initialise the DMX engine
 * @param maxChannel Highest channel to send
 */
void dmxBegin()
{
  dmxStarted = 1;

  // Set DMX pin to output
  DMX_DDR |= _BV(DMX_BIT);
  
  // Initialise DMX frame interrupt
  //
  // Presume Arduino has already set Timer2 to 64 prescaler,
  // Phase correct PWM mode
  // So the overflow triggers every 64*510 clock cycles
  // Which is 510 DMX bit periods at 16MHz,
  //          255 DMX bit periods at 8MHz,
  //          637 DMX bit periods at 20MHz

#if defined(__AVR_ATmega168__) || defined(__AVR_ATmega168P__) || defined(__AVR_ATmega328P__) || defined(__AVR_ATmega1280__)
  TIMSK2 |= _BV(TOIE2);
#elif defined(__AVR_ATmega8__)
  TIMSK |= _BV(TOIE2);
#else
  #warning "DmxSimple does not support this CPU"
#endif
}

/** Stop the DMX engine
 * Turns off the DMX interrupt routine
 */
void dmxEnd()
{
#if defined(__AVR_ATmega168__) || defined(__AVR_ATmega168P__) || defined(__AVR_ATmega328P__) || defined(__AVR_ATmega1280__)
  TIMSK2 &= ~_BV(TOIE2);
#elif defined(__AVR_ATmega8__)
  TIMSK &= ~_BV(TOIE2);
#else
  #warning "DmxSimple does not support this CPU"
#endif
  dmxStarted = 0;
  dmxMax = 0;
}

/** Transmit a complete DMX byte
 * Uses bit-banging to transmit byte without using a serial port.
 * Highly time critical code - tweak at your peril.
 */
void dmxSendByte(volatile uint8_t value)
{
  uint8_t bitCount, delCount;
  __asm__ volatile (
    "cbi %[outPort],%[outBit]\n"
    "nop\n nop\n nop\n nop\n"
    "ldi %[bitCount],11\n" // 11 bit intervals per transmitted byte
  "bitLoop%=:\n"
#if F_CPU == 8000000
    "nop\n"
    "ldi %[delCount],7\n" // 7 loops to hit exact 4us bit time on 8MHz clock
#elif F_CPU == 16000000
    "ldi %[delCount],18\n" // 18 loops to hit exact 4us bit time on 16MHz clock
#elif F_CPU == 20000000
    "nop\n"
    "ldi %[delCount],23\n" // 23 loops to hit exact 4us bit time on 20MHz clock
#else
    #warning "DmxSimple does not support this clock speed"
#endif
  "delLoop%=:\n"
    "dec %[delCount]\n"
    "brne delLoop%=\n"
    "sbrc %[value],0\n"
    "sbi %[outPort],%[outBit]\n"
    "sbrs %[value],0\n"
    "cbi %[outPort],%[outBit]\n"
    "lsr %[value]\n"
    "ori %[value],128\n" // After sending out 8 data bits, send out high state
    "dec %[bitCount]\n"
    "brne bitLoop%=\n"
    :[bitCount] "=&d" (bitCount), [delCount] "=&d" (delCount)
    :[outPort] "I" (_SFR_IO_ADDR(DMX_PORT)), [outBit] "I" (DMX_BIT), [value] "r" (value)
  );
}

/** DmxSimple interrupt routine
 * Transmit a chunk of DMX signal every timer overflow event.
 * 
 * The full DMX transmission takes too long, but some aspects of DMX timing
 * are flexible. This routine chunks the DMX signal, only sending as much as
 * it's time budget will alow.
 */
ISR(TIMER2_OVF_vect) {
  uint16_t bitsLeft = F_CPU / 31372; // DMX Bit periods per timer tick
  bitsLeft >>=2; // 25% CPU usage
  while (1) {
    if (dmxState == 0) {
      // Next thing to send is reset pulse and start code
      // which takes 35 bit periods
      uint8_t i;
      if (bitsLeft < 35) break;
      bitsLeft-=35;
      DMX_PORT &= ~_BV(DMX_BIT);
      for (i=0; i<11; i++) _delay_us(8);
      DMX_PORT |= _BV(DMX_BIT);
      _delay_us(8);
      dmxSendByte(0);
    } else {
      // Now send a channel which takes 11 bit periods
      if (bitsLeft < 11) break;
      bitsLeft-=11;
      dmxSendByte(dmxBuffer[dmxState-1]);
    }
    // Successfully completed that stage - move state machine forward
    dmxState++;
    if (dmxState > dmxMax) {
      dmxState = 0; // Send next frame
      break;
    }
  }
}

void dmxWrite(int channel, uint8_t value) {
  if (!dmxStarted) dmxBegin();
  if ((channel > 0) && (channel <= DMX_SIZE)) {
    if (value<0) value=0;
    if (value>255) value=255;
    dmxMax = max((unsigned)channel, dmxMax);
    dmxBuffer[channel-1] = value;
  }
}

void dmxMaxChannel(int channel) {
  if (channel <=0) {
    // End DMX transmission
    dmxEnd();
    dmxMax = 0;
  } else {
    dmxMax = min(channel, DMX_SIZE);
    if (!dmxStarted) dmxBegin();
  }
}


/* C++ wrapper */


/** Set DMX maximum channel
 * @param channel The highest DMX channel to use
 */
void DmxSimpleClass::maxChannel(int channel) {
  dmxMaxChannel(channel);
}

/** Write to a DMX channel
 * @param address DMX address in the range 1 - 512
 */
void DmxSimpleClass::write(int address, uint8_t value)
{
	dmxWrite(address, value);
}
DmxSimpleClass DmxSimple;