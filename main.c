/* Keyboard controller: C64 matrix to 8 bit bus with handshaking.
 *
 * PORTA - Outputs to keyboard A-G
 * PORTB - Inputs from keyboard 0-7
 * PORTC - 8 bit bus to VIA
 * PORTE - 0: CA1, 1: CA2 on VIA, 2: Buzzer
 *
 * For the ATMEGA8515 and perhaps others.
 *
 * (c) 2014 Lawrence Manning. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/eeprom.h>

#define BUFFER_SIZE 64

unsigned char keybuffer[BUFFER_SIZE];
//unsigned char oldin[8];
unsigned char keystate[8];
unsigned char readpointer = 0;
unsigned char writepointer = 0;

void fillbuffer(void);

int main(void)
{
	/* DDRA is setup for each scan. */
	DDRB = 0x00; /* Inputs from keyboard: 0-7. */
	DDRC = 0xff; /* VIA bus */
	DDRE = 0b00000101; /* low to high: CA1 (output), CA2 (input), buzzer (output) */

	TCCR1B |= (1 << WGM12); // CTC
	TCCR1B |= ((1 << CS10) | (1 << CS11)); // Set up timer at Fcpu/64
	OCR1A   = 625; // 200Hz
	TIMSK |= (1 << OCIE1A); // Enable CTC interrupt

	PORTA = 0; /* DDRA is used to scan. Output bit will always be 0. */

	PORTB = 0xff; /* Pullups. */
	
	PORTC = 0xff;
	
	memset(keybuffer, 0, BUFFER_SIZE);
	memset(keystate, 0, 8);
//	memset(oldin, 0, 8);

	for (int buzzcount = 0; buzzcount < 100; buzzcount++)
	{
		PORTE = 0b00000100;
		_delay_us(500);
		PORTE = 0b00000000;
		_delay_us(500);
	}

	PORTE = 0x01; /* Clear CA1, no data. */

	sei();

	while (1)
	{
		/* See if there is a scancode available. */
		cli();
		unsigned char pointerdiff = writepointer - readpointer;
		sei();

		if (pointerdiff)
		{
			/* If so, put the first one out. */
			PORTC = keybuffer[readpointer];
			readpointer = (readpointer + 1) & (BUFFER_SIZE - 1);
			
			/* Set CA1 low. */
			PORTE = 0x00;
			/* Wait for CA2 to go high. */
			while ((PINE & 0x02) == 0x02) _delay_us(5);
			/* Set CA1 high again. */
			PORTE = 0x01;
			
			_delay_us(5);
		}

		/* After the last scancode has gone, set a dummy code. */		
		PORTC = 0xff;
	}

	return 0; /* Not reached. */
}

/* The thing that makes it all work: timer interrupt. */
ISR(TIMER1_COMPA_vect)
{
	unsigned char outstrobe = 1;

	for (int c = 0; c < 8; c++)
	{
		unsigned char instrobe = 1;
		unsigned char in;

		/* Set the A-G we are scanning on as output. */
		DDRA = outstrobe;
		_delay_us(10);
		in = PINB;

//		if (in == oldin[c])
//		{
			for (int d = 0; d < 8; d++)
			{
				if (!(in & instrobe))
				{
					/* Key down */
					if (!(keystate[c] & instrobe))
					{
						keybuffer[writepointer] = (c << 3) | d;
						keystate[c] |= instrobe;
						writepointer = (writepointer + 1) & (BUFFER_SIZE - 1);
					}
				}
				else
				{
					/* Key up */
					if ((keystate[c] & instrobe))
					{
						keybuffer[writepointer] = (c << 3) | d | 0x80;
						keystate[c] &= ~instrobe;
						writepointer = (writepointer + 1) & (BUFFER_SIZE - 1);
					}
				}

				instrobe <<= 1;
			}
			outstrobe <<= 1;
//		}

//		oldin[c] = in;
	}
	
}
