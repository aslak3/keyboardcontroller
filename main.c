/* Keyboard controller: A600 matrix to UART.
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
 * (c) 2016-2018 Lawrence Manning, lawrence@aslak.net. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/eeprom.h>

/* Size of event buffer; filled by timer interrupt, emptied by main program. */
#define BUFFER_SIZE 16

/* Time a key must be stable (stopped bouncing) to generate an event. */
#define STEADY_THRESH 5

#define USART_BAUDRATE 9600
#define BAUD_PRESCALE (((F_CPU / (USART_BAUDRATE * 16UL))) - 1)

/* Macro for obtaining a scancode from row, bank and column values. */
#define GETSCAN(row, bank, col) ((row << 4) | (bank << 3) | col)

/* Commands. */
#define COM_TYPE_MASK 0b11000000
#define COM_TYPE_REGULAR 0b00000000
#define COM_TYPE_DELAY 0b01000000
#define COM_TYPE_RATE 0b10000000
#define COM_VALUE_MASK 0b00111111

#define COM_RED_LED_OFF 0
#define COM_RED_LED_ON 1
#define COM_GREEN_LED_OFF 2
#define COM_GREEN_LED_ON 3
#define COM_BLUE_LED_OFF 4
#define COM_BLUE_LED_ON 5
#define COM_INIT 6

/* Special keys scancodes. */
#define KEY_CAPS_LOCK 0x30

/* Default typematic speeds. */
#define DEFAULT_TYPEMATIC_DELAY (63 << 2)
#define DEFAULT_TYPEMATIC_RATE (25 << 2)

/* Serial related. */
void writechar(char c);
void writestring(char *string);
char readchar(void);

/* Other local subs. */
void initkeybuffer(void);

/* GLOBALS */

/* Event buffer stuff. */
unsigned char readpointer = 0;
unsigned char writepointer = 0;
unsigned char keybuffer[BUFFER_SIZE];

/* Bitmap of scancodes. */
unsigned char keystate[128 / 8];

/* Debouncing counters, one per scancode (key) */
unsigned char steadycounts[128];

/* Typematic speed values. */
unsigned char typematicdelay = 0;
unsigned char typematicrate = 0;

int main(void)
{
	/* Configure the serial port UART */
	UBRRL = BAUD_PRESCALE;
	UBRRH = (BAUD_PRESCALE >> 8);
	UCSRC = (1 << URSEL) | (3 << UCSZ0);
	UCSRB = (1 << RXEN) | (1 << TXEN);   /* Turn on the transmission and reception circuitry. */

	/* DDRA is setup for each scan. */
	DDRA = 0b00000000; /* Inputs from keyboard: Column Low */
	DDRB = 0b10000000; /* Inputs from keyboard: Column High, bit 7 is
	                    * caps lock _LED output. */
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
	int capslockon = 0;

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
			readpointer = (readpointer + 1) & (BUFFER_SIZE - 1);

			if (
				!(lastevent & 0b10000000) &&
				((lastevent & 0x70) != 0x50) &&
				(lastevent != KEY_CAPS_LOCK)
			) {
				keydowntimer = typematicdelay;
			}
			else
				keydowntimer = 0;

			/* Caps lock handling. Caps lock up or down? */
			if ((lastevent & 0b01111111) == KEY_CAPS_LOCK)
			{
				/* Down? */
				if (!(lastevent & 0b10000000))
				{
					if (!capslockon)
					{
						/* If it was off before, make it on and
						 * send key down. */
						writechar(KEY_CAPS_LOCK);
						PORTB |= 0x80;
						capslockon = 1;
					}
					else
					{
						/* If it was on before, make it off
						 * and send key up. */
						writechar(KEY_CAPS_LOCK | 0x80);
						PORTB &= ~0x80;
						capslockon = 0;
					}
				}
			}
			else
			{
				/* Otherwise (normal key and not caps lock going
				 * up), send the key scancode. */
				writechar(lastevent);
			}
		}

		if (keydowntimer > 0)
		{
			/* Timer running, decrement timer. */
			keydowntimer--;
			if (keydowntimer == 0)
			{
				/* Until timer is zero, when we send the last
				 * scancode and reset to the (shorter) repeat
				 * timer. */
				writechar(lastevent);
				keydowntimer = 100;
			}
		}

		/* See if there is a command byte available. */
		if (UCSRA & (1 << RXC))
		{
			/* Grab it. */
			unsigned char incommand = UDR;

			/* Split the command. */
			unsigned char commandtype = incommand & COM_TYPE_MASK;
			unsigned char commandvalue = incommand & COM_VALUE_MASK;

			switch (commandtype)
			{
				case COM_TYPE_REGULAR:
					switch (commandvalue)
					{
						case COM_RED_LED_OFF:
							PORTE &= ~0x04;
							break;
						case COM_RED_LED_ON:
							PORTE |= 0x04;
							break;
						case COM_GREEN_LED_OFF:
							PORTE &= ~0x02;
							break;
						case COM_GREEN_LED_ON:
							PORTE |= 0x02;
							break;
						case COM_BLUE_LED_OFF:
							PORTE &= ~0x01;
							break;
						case COM_BLUE_LED_ON:
							PORTE |= 0x01;
							break;
						case COM_INIT:
							initkeybuffer();
							capslockon = 0;
							break;
						default:
							break;
					}
					break;
				/* Other commands have the value in the low
				 * six bits. */
				case COM_TYPE_DELAY:
					typematicdelay = commandvalue << 2;
					break;
				case COM_TYPE_RATE:
					typematicrate = commandvalue << 2;
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

	memset(steadycounts, 0, 128);

	typematicdelay = DEFAULT_TYPEMATIC_DELAY;
	typematicrate = DEFAULT_TYPEMATIC_RATE;

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
						/* Start the debouncing counter */
						steadycounts[scancode] = 1;	
						keystate[scancode >> 3] |= instrobe;
					}
				}
				else
				{
					/* Key up */
					if ((keystate[scancode >> 3] & instrobe))
					{
						/* Start the debouncing counter */
						steadycounts[scancode] = 1;
						keystate[scancode >> 3] &= ~instrobe;
					}
				}

				if (steadycounts[scancode] > STEADY_THRESH)
				{
					/* Key is "stuck" up, or down? Generate an event. */
					if (!(keystate[scancode >> 3] & instrobe))
					{
						keybuffer[writepointer] = scancode | 0b10000000;
					}
					if ((keystate[scancode >> 3] & instrobe))
					{
						keybuffer[writepointer] = scancode;
					}

					/* Advance the writepointer, and stop the debounce
					 * counter. */
					writepointer = (writepointer + 1) & (BUFFER_SIZE - 1);
					steadycounts[scancode] = 0;					
				}
				else if (steadycounts[scancode] > 0)
				{
					/* Counter is running, so count! */
					steadycounts[scancode]++;
				}

				instrobe <<= 1;
			}
		}
		outstrobe <<= 1;
	}
}
