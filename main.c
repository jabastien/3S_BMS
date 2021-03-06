/*
 * main.c
 *
 *  Created on: 18. 12. 2018
 *      Author: Zbysekz
 *      Processor: ATmega48P
 *      Fuses: L:0xF7 H:0xDF E:0xFF
 */

#include <avr/io.h>
#include <util/delay.h>
#include "main.h"
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include "twi.h"
#include <avr/sleep.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

#include "routines.h"

//#define DEBUG	// show debug messages on UART?

/***************************************************/

#define STATE_NORMAL 0
#define STATE_CHARGING 1
#define STATE_LOW 2
#define STATE_OFF 3
#define STATE_WAIT_OFF 4

#define pCHARGE PORTD
#define CHARGE PD2

#define pOUT PORTD
#define OUT PD3

#define pCHARGE_SIG PIND
#define CHARGE_SIG PD4

#define pOPTO1 PORTB
#define OPTO1 PB3

#define pOPTO2 PORTB
#define OPTO2 PB4

#define pOPTO3 PORTB
#define OPTO3 PB5

#define pLED_G PORTD
#define LED_G PD6

#define pLED_R PORTD
#define LED_R PD5


#define CELL_A_CALIB 1314 //value calculated as follows:  100 000 * realVoltage / "raw value from ReadADC()"
#define CELL_B_CALIB 1314
#define CELL_C_CALIB 1314


//0,01V
uint16_t parLow=310; // when go to LOW
uint16_t parOk=340; // when go back to NORMAL
uint16_t parMax=420; // when to some of the cell reaches this, stop immediately charging
uint16_t parBurnStart=415; // when start burning for specific cell
uint16_t parBurnStop=410; // when stop burning for specific cell
uint16_t parMinBurnTime=10; // 0,1s
uint16_t parFullDelay=600; // when at least two of cells are full, wait at least this time to start charging again

uint8_t currentState=STATE_NORMAL,nextState=STATE_NORMAL;

uint16_t cellA,cellB,cellC;

volatile uint8_t goOff=0;

volatile uint16_t stepTimer=0,tmrBlink1,tmrBlink2,tmrGeneral,tmrBurn1,tmrBurn2,tmrBurn3,tmrFull;

#ifdef DEBUG
static FILE uart_str = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);
#endif
/***************************************************/
int main(void)
{

	//set WDT prescaler to 8sec//////////
	wdt_reset();
	wdt_enable(WDTO_8S);

	//set outputs
	DDRB = (1<<OPTO1) | (1<<OPTO2) | (1<<OPTO3) ;
	//set outputs
	DDRD = (1<<CHARGE) | (1<<OUT) | (1<<LED_G) | (1<<LED_R);



	// Timer/Counter 0 initialization
	// Clock source: System Clock
	// Clock value: 244 Hz
	TCCR0B=(1<<CS00) | (1<<CS01);//div 8
	TCNT0=0x00;
	// Timer(s)/Counter(s) Interrupt(s) initialization
	TIMSK0=(1<<TOIE0);//overflow enable

	// Timer/Counter 2 initialization
	// Clock source: System Clock
	// Clock value: 244 Hz
	/*TCCR2B=(1<<CS22)|(1<<CS21)|(1<<CS20);//div 1024
	TCNT2=0x00;
	// Timer(s)/Counter(s) Interrupt(s) initialization
	TIMSK2=(1<<TOIE2);//overflow enable
*/

	TWI_Init();
	InitCRC();//calculate table for fast CRC calculation

	// Use the Power Save sleep mode - wake up
	set_sleep_mode(SLEEP_MODE_PWR_SAVE);

	//init interrupt
	sei();

	#ifdef DEBUG
	USARTInit();
	stdout = &uart_str;
	#endif
	// ADC initialization
	ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS0); //division factor:32

	_delay_s(1);

	while(1){
		wdt_reset();

		UpdateTxData();

		switch(currentState){
			case STATE_NORMAL:
			
				CellBalancing();
				
				//if one of the cell is below threshold go to LOW
				if(cellA<parLow || cellB<parLow || cellC<parLow){
					nextState=STATE_LOW;
					tmrBlink1=50;tmrBlink2=0;//3secs blink fast red
					break;
				}
				
				//if charger is connected, go to charging
				if(!getBit(pCHARGE_SIG,CHARGE_SIG)){
					nextState=STATE_CHARGING;
					break;
				}
				
				//output is on
				setBit(&pOUT,OUT);
			break;
			
			case STATE_CHARGING:
			
				CellBalancing();
				
				
				//run output only if cells have ok voltage
				if(stepTimer>50){//be in this step at least 5 sec to control OUT
					if(cellA>=parOk && cellB>=parOk && cellC>=parOk)
						setBit(&pOUT,OUT);
					else 
						clearBit(&pOUT,OUT);
				}
				
				//if we are burning energy for two or more cells, or one of cell's voltage is above max -> stop charging
				if(( (getBit(pOPTO1,OPTO1) + getBit(pOPTO2,OPTO2) + getBit(pOPTO3,OPTO3)) >= 2) ||
						(cellA>=parMax || cellB>=parMax || cellC>=parMax)){
					clearBit(&pCHARGE,CHARGE);
					tmrFull=parFullDelay;
				}else{
					if(tmrFull==0)//start charging again after some delay
						setBit(&pCHARGE,CHARGE);
				}
				
				//if charger is not connected anymore, go to normal operation
				if(getBit(pCHARGE_SIG,CHARGE_SIG)){
					nextState=STATE_NORMAL;
					break;
				}
			break;
			
			case STATE_LOW:

				ResetOptos();//turn off all optos

				//output is cut off
				clearBit(&pOUT,OUT);
				
				if (tmrBlink1==0)//only when you finish red fast blinking
				{//go sleep forever
					clearBit(&pLED_G,LED_G);
					clearBit(&pLED_R,LED_R);

					/* Clear the reset flag. */
					MCUSR &= ~(1<<WDRF);
					MCUSR = 0;
					wdt_disable();
					sleep_enable();
					cli();//disable interrupts
					sleep_cpu();//sleep forever
					sleep_disable();//it shouldn't never get here
					sei();
				}
				

			break;
			
			case STATE_WAIT_OFF://delayed entry to STATE_OFF
				if(stepTimer>200){//wait 20sec
					nextState=STATE_OFF;
				}
				break;
			case STATE_OFF:
				ResetOptos();//turn off all optos
				//output is cut off
				clearBit(&pOUT,OUT);
				clearBit(&pLED_G,LED_G);
				clearBit(&pLED_R,LED_R);

				/* Clear the reset flag. */
				MCUSR &= ~(1<<WDRF);
				MCUSR = 0;
				wdt_disable();
				sleep_enable();
				cli();//disable interrupts
				sleep_cpu();//sleep forever
				sleep_disable();//it shouldn't never get here
				sei();
				break;

		}
		
		//going OFF
		if(goOff && (currentState!= STATE_WAIT_OFF)&&(currentState!= STATE_OFF)){
			nextState=STATE_WAIT_OFF;
			tmrBlink1=500;
		}

		if(nextState!=currentState){
			//if(DEBUG)printf("Transition from state %d to %d \n",currentState,nextState);
			currentState = nextState;
			stepTimer = 0;
		}
		
		
		////////////////////////LED STATE/////////////////////////////////////////////
		if(currentState == STATE_CHARGING){//red color for CHARGING
			clearBit(&pLED_G,LED_G);
			setBit(&pLED_R,LED_R);
		}else if ((currentState == STATE_LOW)||(currentState == STATE_WAIT_OFF)){//blink fast red for a while for LOW
			clearBit(&pLED_G,LED_G);
			if(tmrBlink1>0){
				if(tmrBlink2==0){
					if(getBit(pLED_R,LED_R))//negate
						clearBit(&pLED_R,LED_R);
					else
						setBit(&pLED_R,LED_R);
					tmrBlink2=2;
				}
			}else clearBit(&pLED_R,LED_R);
		}else if (currentState == STATE_NORMAL){//green for NORMAL operation
			setBit(&pLED_G,LED_G);
			clearBit(&pLED_R,LED_R);
		}
		//////////////////////////////////////////////////////////////////////////////

		_delay_ms(100);
	}

return 0;
}

void ReadCells(){
	uint16_t a,b,c;
	/////////////////CELL A//////////////////////
	a = (uint16_t)((((uint32_t)ReadADC(0))*CELL_A_CALIB)/1000);
	
	b = (uint16_t)((((uint32_t)ReadADC(1))*CELL_B_CALIB)/1000);
	
	cellA=a>b?a-b:0;//return difference between first and second if it is positive


	/////////////////CELL B///////////////////////

	b = (uint16_t)((((uint32_t)ReadADC(1))*CELL_B_CALIB)/1000);
	
	c = (uint16_t)((((uint32_t)ReadADC(2))*CELL_C_CALIB)/1000);
	
	cellB=b>c?b-c:0;//return difference between second and third if it is positive


	//////////////////CELL C//////////////////////

	cellC = (uint16_t)((((uint32_t)ReadADC(2))*CELL_C_CALIB)/1000); //return just third, it is equal to cell C voltage


	#ifdef DEBUG
	printf("%d %d %d \n",cellA,cellB,cellC);
	#endif
}

void CellBalancing(){
	//CELL BALANCING
	//if one of the cell is above threshold, set opto
	//if it is lower then threshold reset opto

	ReadCells();

	if(cellA > parBurnStart){
		setBit(&pOPTO1,OPTO1);
		tmrBurn1=parMinBurnTime;
	}
	if(cellA < parBurnStop && tmrBurn1==0)
		clearBit(&pOPTO1,OPTO1);
	
	if(cellB > parBurnStart){
		setBit(&pOPTO2,OPTO2);
		tmrBurn2=parMinBurnTime;
	}
	if(cellB < parBurnStop  && tmrBurn2==0)
		clearBit(&pOPTO2,OPTO2);
	
	if(cellC > parBurnStart){
		setBit(&pOPTO3,OPTO3);
		tmrBurn3=parMinBurnTime;
	}
	if(cellC < parBurnStop  && tmrBurn3==0)
		clearBit(&pOPTO3,OPTO3);
}

void ResetOptos(){
	clearBit(&pOPTO1,OPTO1);
	clearBit(&pOPTO2,OPTO2);
	clearBit(&pOPTO3,OPTO3);
}

// Timer 0 overflow interrupt service routine - called each 244Hz
ISR(TIMER0_OVF_vect)
{

	if(tmrGeneral>=48){//each 0,1s
		tmrGeneral = 0;
		
		if(tmrBlink1>0)tmrBlink1--;
		if(tmrBlink2>0)tmrBlink2--;
		if(tmrBurn1>0)tmrBurn1--;
		if(tmrBurn2>0)tmrBurn2--;
		if(tmrBurn3>0)tmrBurn3--;
		if(tmrFull>0)tmrFull--;
		
		stepTimer++;
		
	}else tmrGeneral++;

}

// Timer 2 overflow interrupt service routine - called each 1,9Hz
ISR(TIMER2_OVF_vect)
{

}

ISR(INT0_vect){

}
ISR(INT1_vect){

}
void _delay_s(int sec){
	for(int c=0;c<sec*10;c++)
		_delay_ms(100);
}

void setBit(volatile uint8_t *port, int bit){
	*port|=(1<<bit);
}
void clearBit(volatile uint8_t *port, int bit){
	*port&=0xFF-(1<<bit);
}
uint8_t getBit(volatile uint8_t port, int bit){
	if((port&(1<<bit))==0)return 0;else return 1;
}
