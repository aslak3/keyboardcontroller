/* Keyboard controller: A1200 matrix to UART.
 *
 * PORTA - Input Coloum Low
 * PORTB - Input Column High Caps LED on bit 7
 * PORTC - Input Column Metas
 * PORTD - Output Rows (bit 7 down to bit 3), Key request (bit 2), TX (bit 1),
 *         RX (bit 0)
 * PORTE - RGB LED
 *
 * Scancode format:
 * DRRRCCCC
 *
 * D = 0 -> down, d = 1 -> up
 * R = 0-4 -> regular, 5 = metas
 * C = bits 2,1,0 -> column, bit 3 -> 0 for low, 1 for high
 *
 * For the ATMEGA8515 and perhaps others.
 *
 * (c) 2016 Lawrence Manning. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/eeprom.h>

#define BUFFER_SIZE 64

#define USART_BAUDRATE 9600
#define BAUD_PRESCALE (((F_CPU / (USART_BAUDRATE * 16UL))) - 1)

#define GETSCAN(row, bank, col) ((row << 4) | (bank << 3) | col)

#define COM_INIT 0
#define COM_CAPSLOCKLED_ON 1
#define COM_CAPSLOCKLED_OFF 2
#define COM_REDLED_ON 3
#define COM_REDLED_OFF 4
#define COM_GREENLED_ON 5
#define COM_GREENLED_OFF 6
#define COM_BLUELED_ON 7
#define COM_BLUELED_OFF 8

/* Serial related */
void writechar(char c);
void writestring(char *string);
char readchar(void);

unsigned char keybuffer[BUFFER_SIZE];
unsigned char keystate[16]; /* bit map of scancodes */
unsigned char readpointer = 0;
unsigned char writepointer = 0;

void initkeybuffer(void);

int main(void)
{
	unsigned char incommand;

	/* Configure the serial port UART */
	UBRRL = BAUD_PRESCALE;
	UBRRH = (BAUD_PRESCALE >> 8);
	UCSRC = (1 << URSEL) | (3 << UCSZ0);
	UCSRB = (1 << RXEN) | (1 << TXEN);   /* Turn on the transmission and reception circuitry. */

	/* DDRA is setup for each scan. */
	DDRA = 0b00000000; /* Inputs from keyboard: Column Low */
	DDRB = 0b10000000; /* Inputs from keyboard: Column High, bit 7 is
	                    * caps lock LED output. */
	DDRC = 0b00000000; /* Inputs from keyboard: Column Metas */
	DDRD = 0b11111100; /* Outputs to keyboard: Rows, and INT */
	DDRE = 0b00000111; /* -----RGB */

	TCCR1B |= (1 << WGM12); // CTC
	TCCR1B |= ((1 << CS10) | (1 << CS11)); // Set up timer at Fcpu/64
	OCR1A   = 625; // 200Hz
	TIMSK  |= (1 << OCIE1A); // Enable CTC interrupt

	PORTA = 0b11111111; /* Pullups for Column Low */
	PORTB = 0b01111111; /* Pullups for Column High */
	PORTC = 0b11111111; /* Pullups for Column Metas */
	PORTD = 0x04; /* High INT. */
	
	initkeybuffer();

	sei();

	int keydowntimer = 0;
	unsigned char lastevent = 0;

	while (1)
	{
		/* See if there is a scancode available. */
		cli();
		unsigned char pointerdiff = writepointer - readpointer;
		sei();

		if (pointerdiff)
		{
			/* If so, put the first one out. */
			lastevent = keybuffer[readpointer];
			writechar(lastevent);

			readpointer = (readpointer + 1) & (BUFFER_SIZE - 1);

			if (!(lastevent & 0b10000000))
				keydowntimer = 200;
			else
				keydowntimer = 0;
		}

		if (keydowntimer > 0)
		{
			keydowntimer--;
			if (keydowntimer == 0)
			{
				writechar(lastevent);
				keydowntimer = 100;
			}
		}

		if (UCSRA & (1 << RXC))
		{
			incommand = UDR;

			switch (incommand)
			{
				case COM_INIT:
					initkeybuffer();
					break;
				case COM_CAPSLOCKLED_ON:
					PORTB |= 0x80;
					break;
				case COM_CAPSLOCKLED_OFF:
					PORTB &= ~0x80;
					break;
				case COM_REDLED_ON:
					PORTE |= 0x04;
					break;
				case COM_REDLED_OFF:
					PORTE &= ~0x04;
					break;
				case COM_GREENLED_ON:
					PORTE |= 0x02;
					break;
				case COM_GREENLED_OFF:
					PORTE &= ~0x02;
					break;
				case COM_BLUELED_ON:
					PORTE |= 0x01;
					break;
				case COM_BLUELED_OFF:
					PORTE &= ~0x01;
					break;
				
				default:
					break;
			}
		}

		_delay_ms(1);
	}

	return 0; /* Not reached. */
}

void writechar(char c)
{
	while(!(UCSRA & (1<<UDRE)));

	UDR = c;
}

void writestring(char *string)
{
	char *p = string;
	
	while (*p)
	{
		writechar(*p);
		p++;
	}
}

char readchar(void)
{
	char x;

	/* Will block until there is a char, no interrupts here. */
	while(!(UCSRA & (1 << RXC)));

	x = UDR;

	return x;
}

void initkeybuffer(void)
{
	memset(keystate, 0, 16);

	readpointer = 0;
	writepointer = 0;

	/* Turn the RGB and caps lock LEDs off. */
	PORTE = 0x00;
	PORTB &= ~0x80;
}

/* The thing that makes it all work: timer interrupt. */
ISR(TIMER1_COMPA_vect)
{
	unsigned char outstrobe = 0b00001000;

	for (int row = 0; row < 6; row++)
	{
		/* Set the A-G we are scanning on as output. */
		if (row < 5)
			DDRD = outstrobe | 0b0000100;
		else
			DDRD = 0b00000100;

		_delay_us(10);
		
		for (int bank = 0; bank < (row < 5 ? 2 : 1); bank++)
		{
			unsigned char in;
			unsigned char instrobe = 1;

			if (row < 5)
			{
				if (bank == 0)
				{
					in = PINA;
				}
				else
				{
					in = PINB;
				}
			}
			else
			{
				in = PINC;
			}

			for (int col = 0; col < (bank < 1 ? 8 : 7); col++)
			{
				unsigned char scancode = GETSCAN(row, bank, col);
				if (!(in & instrobe))
				{
					/* Key down */
					if (!(keystate[scancode >> 3] & instrobe))
					{
						keybuffer[writepointer] = scancode;
						keystate[scancode >> 3] |= instrobe;
						writepointer = (writepointer + 1) & (BUFFER_SIZE - 1);
					}
				}
				else
				{
					/* Key up */
					if ((keystate[scancode >> 3] & instrobe))
					{
						keybuffer[writepointer] = scancode | 0b10000000;
						keystate[scancode >> 3] &= ~instrobe;
						writepointer = (writepointer + 1) & (BUFFER_SIZE - 1);
					}
				}

				instrobe <<= 1;
			}
		}
		outstrobe <<= 1;
	}
}
