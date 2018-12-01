
/*
* multi-mode radio controller and synthesizer using an Arduino Uno
* and si5351 oscillator. Also used is an MCP23008 I/O expander
* for RF filter and hardware control of modules.
*
*
*  This code is GPL-3.0 There is no warranty, implied or otherwise
*  that this code will work. It may not. It might destroy society
*  as we know it. 
*
* k theis <theis.kurt@gmail.com> 11/2018
*
* Ver2 - has 16x2 LCD, a mode/menu button, a channel/vfo store/recall
* button, a rotary (mechanical) encoder with a button. The button steps
* thru tuning steps from 10hz thru 100 khz depending on long or short pushes.
*
* There is a MCP23008 I2C port expander that controls relays for band filters,
* cw/ssb tx/rx control and usb/lsb relay control for the receiver.
*
* The oscillator is a Si5351 used for Tx and Rx.
*
* Note: there is no provision for a frequency lockout. This will transmit
* across all frequencies. YOU are responsible to follow all laws in your
* region regarding what frequencies (if any) you can transmit on.
*
* Please Note: You can probably save some flash memory by tightening up many
* of these routines, but that would make it harder for a beginning programmer
* to understand. As long as there is available memory, I keep it verbose.
*/

/* Subroutines:
* 
* updateBand()   look at current frequency, send filter data to port expander
* updateFreq()   show the current frequency and channel data on the LCD display
* updateMode()   change register to one of cw-L, cw-U, lsb, ssb, update display
*                and update the port expander for hardware control
* Save()         Save frequency and mode to EEPROM based on channel #
* Recall()       Read frequency and mode from EEPROM, update registers and display
* UpdateDcVolt() Read analog pin, divide by constant for true voltage, display on LCD
* changeFreq()   Interrupt driven - read rotary encoder, change freq/channel/menu etc
* updateOsc()    Read from freq and mode. Send display freq to osc0, derived rx to osc1
* menu()         Menu sub-system. Control registers, run special functions
* setDefault()   Initialize the EEPROM with default frequencies
* wspr(freq)     Transmit WSPR on (freq) (called from menu, freq is pre-defined)
*                Call and Grid are predefined before setup() is called. Power
*                is defined in the wspr() routine.
* txKey()        key the transmitter (send 1 to GP5 of the MCP23008)
* txDekey()      dekey the transmitter (send 0 to GP5 of the MCP23008)
*/



#define MINFREQ  4900000    // lowest operating frequency (hz)
#define MAXFREQ 15100000    // highest operating frequency (hz)


#include <LiquidCrystal.h>
#include <stdlib.h>
#include <EEPROM.h>
#include <Wire.h>
#include <asserts.h>
#include <Adafruit_SI5351.h>
#include <errors.h>
//#include <string.h>    // for wspr
//#include <ctype.h>    // for wspr

Adafruit_SI5351 clockgen = Adafruit_SI5351();

// lcd1 is Frequency Display (LCD D0 thru D3 are unused and left floating)
LiquidCrystal lcd1(10,13,6,7,8,9);    // RS, e, D4, D5, D6, D7
// (original version used 2 lcd displays)
 
/* define input lines (output lines are on the port expander, I2C & LCD)*/ 
const int knobsw = 4;   // digital pin D4 (encoder switch) White wire on mine
const int knob = 2;     // digital pin D2 (encoder pulse) Brown wire on mine
const int knobDir = 5;  // digital pin D5 (encoder direction) Blue wire on mine

const int dcVoltage = A0;  // read dc voltage (thru 10K trimmer resistor) on A0
const int toneOut = A1;    // audio tone out while in transmit on A1
const int refIn = A3;      // dc reflected power input (used in transmit)
const int vc = 3;          // pushbutton, vfo/channel and sto/rcl on D3
const int keyIn = 11;      // key line in on D11
const int modesw = 12;     // Mode switch (menu long press)


const char *call = "D0UMY";  // WSPR Call Sign (change for your call)
const char *grid = "CM98";   // WSPR grid square (change for your grid)
//const int txpower = 15;    // WSPR power to antenna (set in wspr() routine)

float VOLT;          // read DC Voltage on pin A0
float freq;          // main frequency in Hz
float freqBak;       // use this when in channel mode to hold the vfo freq
float STEP = 10;     // step size when tuning (in Hz)
int FREQFLAG = 0;    // when HIGH update freq Display & Osc
int vfoChan = 0;     // 0=vfo, 1=chan, 2=menuMode
int chan = 0;        // channel number - 0-99
int menu_sel;        // used in menu sub-system
float MULTI = 28.0;  // multiplier (* XTAL) for PLL (used in transmit pll only)
float XTAL = 25.0;   // Crystal frequency of PLL clock (MHz)
unsigned int SIDETONE = 700;    // sidetone frequency for tone out and CW offset
int MODE = 0;
const char mode[4][6] = {"LSB ","USB ","CW-L","CW-U"};
byte radioReg;      // set radio filters, radio mode etc


/* set up hardware ports etc */ 
void setup() {
  
   /* init LCD displays */
   lcd1.begin(16,2);   // LCD #1 is frequency
   
   /* set up input devices */
   pinMode(knobsw, INPUT_PULLUP);
   pinMode(knob, INPUT_PULLUP);
   pinMode(knobDir, INPUT_PULLUP);
   pinMode(vc, INPUT_PULLUP);
   pinMode(keyIn, INPUT_PULLUP);
   pinMode(modesw, INPUT_PULLUP);
   analogReference(DEFAULT);    // use 5vdc for this 5v arduino
   
   //Serial.begin(9600);
   //Serial.print("Starting up\n\n");
   
   /* set up I2C */
   Wire.begin();
   
   /* initialize port-expander */
   Wire.beginTransmission(0x20);  // select port expander
   Wire.write(0x00);        // select IODIRA of port expander
   Wire.write(0x00);        // PORT A all output
   Wire.endTransmission();  // stop talking to port expander
   
   /* initialize Si5351 oscillator */
   if (clockgen.begin() != ERROR_NONE) {
     lcd1.home();
     lcd1.print("CLK ERR");    // if it doesn't start up, no reason to continue
     while (true) continue;    // so loop until power cycled
   }
      
   /* set up PLL B for rx freq range (12-120 MHz) */
   clockgen.setupPLLInt(SI5351_PLL_B, 40);  // PLL B is 1000 MHz (noisy at 900MHz)
   /* set up PLL A for TX freq range (3-30 MHz) */
   clockgen.setupPLLInt(SI5351_PLL_A, 28); // PLL A is 700 MHz
   /* turn on the outputs */
   clockgen.enableOutputs(true);
   
   /* set interrupts last */
   attachInterrupt(0, changeFreq, LOW);    // set interrupt on knob going LOW
   interrupts();    // allow interrupts (normally defaults on)
}
 
 
 
 
/***********************/ 
/***** SUBROUTINES *****/
/***********************/
 
 
 /**** show the current frequency ****/
void updateFreq() {  // this is called from inside an interrupt - delays, I2C disabled
   extern float freq;
   extern int vfoChan;

   if (vfoChan == 1) {    // show channel number
     lcd1.home();
     if (chan < 10) lcd1.print("CH 0");
     if (chan > 9) lcd1.print("CH ");
     lcd1.print(chan);
     lcd1.print(" ");
     /* now show the frequency */
     if ((freq/1000) < 10000) lcd1.print(" ");    // push display out, no leading 0
     lcd1.print(freq/1000);
   }
   
   if (vfoChan == 0) {    // show frequency of VFO
     lcd1.home();
     if (chan < 10) lcd1.print("VF 0");
     if (chan > 9) lcd1.print("VF ");
     lcd1.print(chan);
     lcd1.print(" ");
     /* now show the frequency */
     if ((freq/1000) < 10000) lcd1.print(" ");    // push display out, no leading 0
     lcd1.print(freq/1000);
   }
   
   return;
}




 
/**** Save vfo frequency to EEPROM ****/
void Save() {
  extern float freq;
  extern int chan;
  extern int MODE;
  int i;
  int address = chan * 5;  // store freq as (4 byte) long, mode as 1 byte
  union u_tag
  {
      byte b[4];
      float fval;
  } u;
  u.fval = freq;
  for (i=0; i<4; i++) {
  EEPROM.write(address+i, u.b[i]);  // save float value of freq
  }
  EEPROM.write(address+4, MODE);    // save mode value

  return;
}
  
  
  
/*** Read from EEPROM to freq ****/  
void Recall() {
  extern float freq;
  extern int chan;
  extern int MODE;
  int i;
  int address = chan * 5;  // recall freq as 4 byte values, mode as 1 byte
  union u_tag
  {
      byte b[4];
      float fval;
  } u;
  for (i=0; i<4; i++) {
    u.b[i] = EEPROM.read(address+i);
  }
  freq = u.fval;
  MODE = EEPROM.read(address+4);    // get mode value
  
  return;
} 
  
  
   
 
/**** Display D.C. Voltage ****/
void updateDcVolt() {
  // read analog pin, divide by constant for true voltage
  extern float VOLT;
  char buf[5];
  
  VOLT = analogRead(dcVoltage)/100.0;    // read DC voltage (use a float here)
  // dtostrf(float var,str len, digits after decimal point, var to hold val)
  dtostrf(VOLT,11,1,buf);  // arduino can't handle floats (WTF? it has a c compiler)
  // this stupid little routine takes >2K of memory!!
  lcd1.home();
  lcd1.print("DC");
  lcd1.print(buf);
  lcd1.setCursor(15,1);
  lcd1.write("V");  // show as voltage
  
  return;
}
 
 
  
 
/**** INTERRUPT 0 (encoder turned cw/ccw) ****/
/* read dir, change freq+/- by STEP size  ****/
void changeFreq() {
   extern float freq;
   extern int FREQFLAG;
   extern int menu_sel;    // determines menu selection
   long i;
   
   delay(1);      // deal with debounce using cheap encoders
   if (digitalRead(knob) == HIGH) {
     interrupts();
     return;
   }
   noInterrupts();  // stop further ints while in this routine
   
   if ((digitalRead(knobDir) == HIGH) && (digitalRead(knob) == LOW)) {
     
     if (vfoChan == 2) {    // in menu sub-system
       menu_sel++;
       if (menu_sel > 4) menu_sel = 0;
       for (i=0; i< 300000; i++);    // delay since delay() is off in ints   
     }
     
     if (vfoChan == 0) {  // increment vfo freq
       freq += STEP;
       if (freq > MAXFREQ) freq = MAXFREQ;  // freq limits
       FREQFLAG = 1;
       updateFreq(); //updateOsc();
     }
     
     if (vfoChan == 1) {  // increment channel number
       chan += 1;
       if (chan > 99) chan = 99;
       Recall();  // read freq from EEPROM save in freq
       updateFreq(); //updateOsc();
       FREQFLAG = 1;
     }
     
     while (digitalRead(knob)==LOW)
       continue;
       interrupts();  // resume ints
     return;
   }
   
   if ((digitalRead(knobDir) == LOW) && (digitalRead(knob) == LOW)) {
     
     if (vfoChan == 2) {    // in menu sub-system
       menu_sel--;
       if (menu_sel < 0) menu_sel = 4;
       for (i=0; i< 300000; i++);    // delay since delay() is off in ints
     }
     
     if (vfoChan == 0) {  // decrement vfo frequency
       freq -= STEP;
       if (freq < MINFREQ) freq = MINFREQ;  // freq limits
       FREQFLAG = 1;
       updateFreq();
     }
     
     if (vfoChan == 1) {  // decrement channel number
       chan -= 1;
       if (chan < 0) chan = 0;
       Recall();      // get the EEPROM channel freq
       updateFreq();
       FREQFLAG = 1;
     }
     
     while (digitalRead(knob)==LOW)
       continue;
       interrupts();  // resume ints
     return;
   }
   
   interrupts();
   return;    // nothing happend
}
   
   
   
/* update the si5351 oscillator for RX and TX */   
void updateOsc() {

   extern unsigned int SIDETONE;
   extern float freq, MULTI, XTAL;
   extern int MODE;  // 0=LSB, 1=USB, 2=CW-L, 3=CW-H
   float VB, VALUE, DIV, VA, VINT;
   float rxFreq;  // rx freq is 4x freq +/- ssb/cw offset
   
   VB = 1000000.0;
   
   // set up the receive frequency
   if (MODE == 0) rxFreq = freq + 1500.0;  // LSB
   if (MODE == 1) rxFreq = freq - 1500.0;  // USB
   if (MODE == 2) rxFreq = freq + (float)SIDETONE;    // CW-L
   if (MODE == 3) rxFreq = freq - (float)SIDETONE;   // CW-U
 
   rxFreq *= 4.0;      // using a tayloe detector, rx freq is 4X display freq
   VALUE = rxFreq/VB;
   DIV = XTAL * 40;    // MULTI for PLL B is 40 (below that osc output is jittery
   VALUE = DIV/VALUE;  // above about 118 MHz)
   VINT = (long)VALUE;
   VA = (long)((VALUE - VINT) * VB);
   clockgen.setupMultisynth(1, SI5351_PLL_B, VINT, VA, VB);  // output on osc 1
   
   // set up the transmit frequency
   
   VALUE = freq/VB;    // tx freq is display freq, no offsets
   DIV = XTAL * MULTI;
   VALUE = DIV/VALUE;
   VINT = (long)VALUE;
   VA = (long)((VALUE - VINT) * VB);
   clockgen.setupMultisynth(0, SI5351_PLL_A, VINT, VA, VB); // output on osc 0
   
   return;
}

void updateMode() {
  extern byte radioReg;
  extern int MODE;
/* 
    |   7  |   6   |   5  |  4  |  3  |   2   |   1  |   0  |
     cw/ssb usb/lsb    tx               11-15   8-11    4-8
      
4-8 is a high pass at 4, low pass at 8 mc
8-11 is a high pass at 8 mc, low pass at 11 mc
11-15 is a high pass at 11 mc, low pass at 15 mc

cw/ssb is 0 for cw, 1 for ssb
usb/lsb is 0 for lsb, 1 for usb
tx = 0 for RX, 1 for TX
*/ 

  if (MODE == 0) {    // LSB
    radioReg &= B00000111;    // clear current mode
    radioReg |= B10000000;    // set SSB/LSB
  }
  if (MODE == 1) {   // USB
    radioReg &= B00000111;    // clear current mode
    radioReg |= B11000000;    // set SSB/USB
  }
  if (MODE == 2) {   // CW-L
    radioReg &= B00000111;    // clear current mode
    radioReg |= B00000000;    // (not needed, just for consistancy)  
  }
  Wire.beginTransmission(0x20);  // set up communication with port expander
  Wire.write(0x09);              // select GPIO pins
  Wire.write(radioReg);           // set band pins
  Wire.endTransmission();        // done
  
  /* update LCD display */
  lcd1.setCursor(0,1);    // row 1 pos 0
  lcd1.print(mode[MODE]);
  
  return;
}




/* update the band register for the TX low pass filters and rx filter */
void updateBand() {
  extern float freq;
  extern byte radioReg;
/* 
    |   7  |   6   |   5  |  4  |  3  |   2   |   1  |   0  |
     cw/ssb usb/lsb    tx               11-15   8-11    4-8
      
4-8 is a high pass at 4, low pass at 8 mc
8-11 is a high pass at 8 mc, low pass at 11 mc
11-15 is a high pass at 11 mc, low pass at 15 mc

cw/ssb is 0 for cw, 1 for ssb
usb/lsb is 0 for lsb, 1 for usb
tx = 0 for RX, 1 for TX
*/ 
  if ((freq >= 4800000) && (freq < 8000000)) {  // 4.8mc thru 8 mc
    radioReg &= B11111000;  // turn off existing band filters
    radioReg |= B00000001;  // set band 1 line
  }
  if ((freq >= 8000000) && (freq < 11000000)) {  // 8 mc thru 11 mc
    radioReg &= B11111000;  // turn off existing band filters
    radioReg |= B00000010;  // set band 2 line
  }
  if ((freq >= 11000000) && (freq < 15200000)) {  // 11 mc thru 15.2 mc
    radioReg &= B11111000;  // turn off existing band filters
    radioReg |= B00000100;  // set band 3 line
  }
  
  Wire.beginTransmission(0x20);  // set up communication with port expander
  Wire.write(0x09);              // select GPIO pins
  Wire.write(radioReg);           // set band pins
  Wire.endTransmission();        // done
  
  return;
}




/* MENU mode - set certain values, read DC voltage, etc */
void menu() {
  extern int vfoChan;
  extern int menu_sel;
  
  long i;         // general purpose counter
  vfoChan = 2;    // in menu mode
  menu_sel = 0;   // select menu item
  
  lcd1.home();
  lcd1.print("Menu    ");
  delay(750);
  while (true) {
    
    /* show dc voltage */
    while (menu_sel == 0) { 
      updateDcVolt();
      for (i=0; i<200000; i++);  // primitive hysterisis 
      continue;
    }
    
    /* set default channels in EEPROM */
    if (menu_sel == 1) {
      while (menu_sel == 1) {
        lcd1.home();
        lcd1.print("set dflt");
        if (digitalRead(vc) == 0) {
          delay(200);
          if (digitalRead(vc) == 0) {
            lcd1.home();
            lcd1.print("working ");
            setDefault();
            lcd1.home();
            lcd1.print("done    ");
            while (digitalRead(vc) == 0) continue;
            delay(750);
          }
        }
      }
      delay(250);
      continue;
    }
    
    /*  */
    if (menu_sel == 2) {
      lcd1.home();
      lcd1.print("wspr 40 ");
      if  (digitalRead(vc) == 0)
        wspr(7040000.0);
      delay(200);
      continue;
    }
    
    if (menu_sel == 3) {
      lcd1.home();
      lcd1.print("wspr 30 ");
      if (digitalRead(vc) == 0)
         wspr(10140200.0);
      delay(200);
      continue;
  }
  
    if (menu_sel == 4) {
      lcd1.home();
      lcd1.print("wspr 20 ");
      if (digitalRead(vc) == 0)
         wspr(14097100.0);
      delay(200);
      continue;
     }
   
  }
   
}


void txKey() {  // key TX
  extern byte radioReg;
  radioReg |= B00100000;         // set key line 
  Wire.beginTransmission(0x20);  // set up communication with port expander
  Wire.write(0x09);              // select GPIO pins
  Wire.write(radioReg);          // update pins
  Wire.endTransmission();        // done
  return;
}

void txDekey() {   // unkey TX
  extern byte radioReg;
  radioReg &= B11011111;         // set key line 
  Wire.beginTransmission(0x20);  // set up communication with port expander
  Wire.write(0x09);              // select GPIO pins
  Wire.write(radioReg);          // update pins
  Wire.endTransmission();        // done
  return;
}


   
/**************************/   
/******* MAIN LOOP ********/
/**************************/
void loop() {

   /* delare global vars */ 
   extern int FREQFLAG; // 0 if no freq update, 1 if freq updated
   extern float STEP;  // tune step size in hz
   extern float VOLT;  // dc battery voltage read on pin A0
   int freqMSB;        // frequency MSB (use for band register)
   extern unsigned int SIDETONE;
   int refVoltage;     // reflected voltage displayed during transmit
   int SBCWVAL;    // used to detect ssb/cw switch change
   int USBLSBVAL;      // used to detect usb/lsb switch change
   int i,x;      // misc variable
   extern byte radioReg;
   long voltLoop = 0;    // timing loop to show DC voltage updates every 5 seconds
   extern int MODE;
   
   
/* if vfo/chan button (vc) is pressed during power-up, jump
 * to MENU mode to read DC voltage, set some things, and calibrate
 * the oscillator 
*/
   if (digitalRead(vc) == 0) {
     delay(100);    // debounce
     if (digitalRead(vc) == 0) {
       menu();
     }
   }
   
   // vfo/chan button - NOT pressed during startup; continue normal operation
   
   MODE = 0;    // initial define until read from the EEPROM   
   /* start in vfo mode (vfoChan = 0) */
   vfoChan = 0;
   /* read EEPROM from channel 0, set as vfo frequency */
   chan = 0;
   Recall();
   /* and set channel to 1 */
   chan = 1;
   /* show vfo freq */
   updateFreq();    // initial display of frequency
   updateOsc();     // set oscillator freq for TX/RX
   updateBand();    // set band register for low pass filters and rx filter
   freqMSB = (int)(freq/1000000);  // when this changes, update the band register
 
   
   
   /****************************/
   /* this is the command loop */
   /****************************/
   while (1) {
     
     
     /* test knob switch - change step size based on long/short push */
     if (digitalRead(knobsw) == LOW) {
        if (vfoChan == 1) {  // channel mode - show freq while held down
          vfoChan = 0; updateFreq();
          updateOsc();
          while (digitalRead(knobsw) == LOW) continue;
          vfoChan = 1; updateFreq(); updateOsc();
          delay(20);
          continue;
        }
        delay(250);
        if (digitalRead(knobsw) == HIGH) {  // short press - tune 10hz, 100 hz, 1khz 
          if (STEP >= 10000) {  // at 10khz go back to 10 hz
            STEP = 10;
          } else {
             STEP *= 10;
             if (STEP >= 10000)
               STEP = 10;
          }  // cycle between 10, 100, 1000 hz tune rates
          /* show where cursor is */
          if (STEP == 10)
           lcd1.setCursor(7,0);  // show cursor location
          if (STEP == 100)
           lcd1.setCursor(6,0);
          if (STEP == 1000)
           lcd1.setCursor(4,0); 
          lcd1.cursor();
          delay(550);
          lcd1.noCursor();    // turn off cursor
          delay(20);    // debounce after the fact 
          continue; 
        }
        //delay(150);  // add a little delay for a longer press
        if (digitalRead(knobsw) == LOW) {  // long press - tune 10K or 100K
          while (digitalRead(knobsw) == LOW){
            delay(20);    // stops falsing
            continue;
          }
          if (STEP == 10000) {
            STEP = 100000;
            lcd1.setCursor(2,0);  // show cursor location
            lcd1.cursor();
            delay(550);
            lcd1.noCursor();
            delay(20);  // stops falsing
            continue;
          }
          if ((STEP < 10000) || (STEP == 100000)) {
            STEP = 10000;
            lcd1.setCursor(3,0);   // show cursor location
            lcd1.cursor();
            delay(550);
            lcd1.noCursor();
            delay(20);  // stops falsing
            continue;
          }
          delay(20);    // debounce after press - stops falsing
        }
     }
 
   
   
     
       
     
     
     /* test for vfo/chan(short press), sto/rcl(long press) button press */
     if (digitalRead(vc) == LOW) {
       delay(250);
       if (digitalRead(vc) == HIGH) {    // short press
         vfoChan = abs(vfoChan - 1);
         if (vfoChan == 1) freqBak = freq;  // save vfo freq in chan mode
         if (vfoChan == 1) Recall();        // in chan mode read from EEPROM
         if (vfoChan == 0) freq = freqBak;  // restore vfo freq when back to vfo
         updateFreq();      // show vfo freq or channel #
         updateOsc();
         continue;
       }
       // wait until released
       if (digitalRead(vc) == LOW) {    // long press
          
          if (vfoChan == 0) {  // in vfo mode, save to EEPROM
            Save();    // save vfo to current channel number
            lcd1.home();
            if (chan < 10) lcd1.print("Saved 0");  // show channel number w/leading 0
            if (chan > 9) lcd1.print("Saved ");    // show 2 digit channel number
            lcd1.print(chan);
            while (digitalRead(vc) == LOW)
              continue;
            delay(700);
            updateFreq();  // show freq display
            updateOsc();
            continue;
          }
          
          if (vfoChan == 1) {  // in channel mode recall, switch to vfo mode
            Recall();
            vfoChan = 0;
            updateFreq();
            updateOsc();
            while (digitalRead(vc) == LOW)
              continue;
            delay(20);
            continue;
          }
       }
       
       // something wrong
       continue;
     }
     
     
     if (FREQFLAG) {    // FLAG changed due to interrupt on knob rotation
       //updateFreq() in knob routine (helps to slow down routine and stop glitches)
       updateOsc();
       FREQFLAG = 0;
     }
     
    /* if MHz digit changes, test and update the band register */ 
    if ((int)(freq/1000000) != freqMSB) {
      freqMSB = (int)(freq/1000000);    // get new MHz value
      updateBand();  // test for and change RF filters
    }
     
     
    /* test keyIn line */
    if (digitalRead(keyIn) == 0) {   // TX key pressed
      tone(toneOut, SIDETONE);       // turn on sidetone
      txKey();                       // key the transmitter
      while (digitalRead(keyIn)==0) { 
        refVoltage = analogRead(refIn);  // read reflected power
        refVoltage /= 128;               // bring in range 0-7
        lcd1.home();                     // print a bar graph of ref power
        for (i=0; i<refVoltage; i++) lcd1.write('|');
        for (x=i; x<8; x++) lcd1.write(' ');  // blank rest of display
        continue;                    // wait 'till released
      }
      noTone(toneOut);               // turn off sidetone
      txDekey();                     // dekey the transmitter
      updateFreq();                  // restore the display from swr reading
      continue;
    }
     
     
    /* test mode button */
    if (digitalRead(modesw) == 0) {
     delay(250);    // test for long/short press
     if (digitalRead(modesw) == 1) {    // short press - MODE function
        MODE += 1;
        if (MODE == 4) MODE = 0;      // cycle thru
        updateMode();   // update LCD/radio registers 
        updateOsc();    // switch rx offset based on mode
        continue;
     }
     // show 'menu' on LCD
     while (digitalRead(modesw) == 0) continue;
     // call to menu routine
     //menu();
     // update LCD on return
    }  // done
    
    
    
    /* update the DC voltage reading once every 5 seconds or so */
     voltLoop++;
     if (voltLoop == 500000) {  // 500,000 is roughly 5 seconds at 16 MHz clock
       updateDcVolt();
       voltLoop = 0;
     }

   
     
    continue;
   }
}

/***********************************************/
/************* End of Main Code ****************/
/***********************************************/

void setDefault() {  /* initialize the EEPROM with default frequencies */

  // NOTE: this is ONLY called to initialize the EEPROM
  // (and may not be called at all - user's choice)
  extern float freq;
  extern int chan;
  extern int MODE;
  int i;    // counter
  
  // EEPROM storage: frequency (4 bytes), mode (1 byte)
  // format: 0=lsb, 1=usb, 2=cw-lower sideband, 3=cw-upper sideband
  
  const PROGMEM float defaultFreq[100] = {
     7030000,  // start freq when turned on
     3525000, // 80M cw ch 1
     7035000,  // 40M cw ch 2
    10110000,  // 30M cw ch 3
    14035000,  // 20M cw ch 4
    18085000,  // 17M cw ch 5
    21035000,  // 15M cw ch 6
    24915000,  // 12M cw ch 7
    28035000,  // 10M cw ch 8
    
     3825000,  // 80M ssb ch  9
     7240000,  // 40M ssb ch 10
    14275000,  // 20M ssb ch 11
    18125000,  // 17M ssb ch 12
    21350000,  // 15M ssb ch 13
    24925000,  // 12M ssb ch 14
    28350000,  // 10M ssb ch 15
   
     5000000,  // WWV 5 mc  ch 16
    10000000,  // WWV 10 mc ch 17
    15000000,  // WWV 15 mc ch 18
    20000000,  // WWV 20 mc ch 19
    25000000,  // WWV 25 mc ch 20
     3330000,  // CHU 3 mc  ch 21
     7850000,  // CHU 7 mc  ch 22
    14670000,  // CHU 14 mc ch 23
    
     3485000,  // volmet US, Can ch 24
     6604000,  // volmet ""  ""  ch 25
    10051000,  // volmet "" ""   ch 26
    13270000,  // volmet "" ""   ch 27
     6754000,  // volmet Trenton ch 28
    15034000,  // volmet   ""    ch 29
     3413000,  // volmet Europe  ch 30
     5505000,  // volmet   ""    ch 31
     8957000,  // volmet   ""    ch 32
    13264000,  // volmet   ""    ch 33
     5450000,  // volmet Europe  ch 34
    11253000,  // volmet   ""    ch 35
     4742000,  // volmet Europe  ch 36
    11247000,  // volmet   ""    ch 37
     6679000,  // volmet Oceania ch 38
     8828000,  // volmet   ""    ch 39
    13282000,  // volmet   ""    ch 40
     6676000,  // volmet SE Asia ch 41
    11387000,  // volmet   ""    ch 42
     3458000,  // volmet   ""    ch 43
     5673000,  // volmet   ""    ch 44
     8849000,  // volmet   ""    ch 45
    13285000,  // volmet   ""    ch 46
 
     5696000,  // US Coast Guard ch 47
     8983000,  //       ""       ch 48
    11201000,  //       ""       ch 49
     5320000,  // USCG Tactical  ch 50
     8337000,  //       ""       ch 51
    10993600,  //       ""       ch 52
    11197400,  //       ""       ch 53
     4125000,  // USCG Distress  ch 54
     6215000,  //       ""       ch 55
    12290000,  //       ""       ch 56
    16420000,  //       ""       ch 57
 
     8992000,  // USAF HFGCS     ch 58
    11175000,  // USAF   ""      ch 59
     6739000,  // USAF Backup    ch 60
  
     5330500,  // US Ham 60M ch1 ch 61
     5346500,  // US Ham 60M ch2 ch 62
     5366500,  // US Ham 60M ch3 ch 63
     5371500,  // US Ham 60M ch4 ch 64
     5403500,  // US Ham 60M ch5 ch 65
  
     3570100,  // WSPR 80M       ch 66
     7040100,  // WSPR 40M       ch 67
    10140200,  // WSPR 30M       ch 68
    14097100,  // WSPR 20M       ch 69
    21096100,  // WSPR 15M       ch 70
    28126100,  // WSPR 10M       ch 71
     
     3000000,  // (place holder) ch 72
     4000000,  //      ""        ch 73
     5000000,  //      ""        ch 74
     6000000,  //      ""        ch 75
     7000000,  //      ""        ch 76
     8000000,  //      ""        ch 77
     9000000,  //      ""        ch 78
    10000000,  //      ""        ch 79
    11000000,  //      ""        ch 80
    12000000,  //      ""        ch 81
    13000000,  //      ""        ch 82
    14000000,  //      ""        ch 83
    15000000,  //      ""        ch 84
    16000000,  //      ""        ch 85
    17000000,  //      ""        ch 86
    18000000,  //      ""        ch 87
    19000000,  //      ""        ch 88
    20000000,  //      ""        ch 89
    21000000,  //      ""        ch 90
    22000000,  //      ""        ch 91
    23000000,  //      ""        ch 92
    24000000,  //      ""        ch 93
    25000000,  //      ""        ch 94
    26000000,  //      ""        ch 95
    27000000,  //      ""        ch 96
    28000000,  //      ""        ch 97
    29000000,  //      ""        ch 98
    30000000   // (place holder) ch 99    
  };
  
  // now load these channels in EEPROM
  MODE = 1;  // defaults to USB
  for (i=0; i<100; i++) { // includes ch 00
     chan = i;
     freq = defaultFreq[chan];
     Save();
  }
  // done
  return;
}

/**********************************/
/********* WSPR Transmitter *******/
/**********************************/

void wspr(float wfreq) { // (frequency sent from calling routine)

  /* much of the code generation was lifted directly from the excellant work 
     of Mark VandeWetterin at https://github.com/brainwagon/genwspr
     The c code as written does not work on the arduino (it wasn't meant to). 
     Some changes needed to be made (change ints to long, unsigned char 
     to byte, etc) and I added the bits needed to make it transmit. - kurt
  */

/********* START of encode routine *********/

const PROGMEM byte sync[] = {  // note byte inplace of char
    1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0,
    1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,
    0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1,
    0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1,
    1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1,
    0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1,
    1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0
} ;

const PROGMEM byte rdx[] = {   // note byte inplace of char
    0, 128, 64, 32, 160, 96, 16, 144, 80, 48, 112, 8, 136, 72, 40, 104, 24,
    152, 88, 56, 120, 4, 132, 68, 36, 100, 20, 148, 84, 52, 116, 12, 140,
    76, 44, 108, 28, 156, 92, 60, 124, 2, 130, 66, 34, 98, 18, 146, 82, 50,
    114, 10, 138, 74, 42, 106, 26, 154, 90, 58, 122, 6, 134, 70, 38, 102,
    22, 150, 86, 54, 118, 14, 142, 78, 46, 110, 30, 158, 94, 62, 126, 1,
    129, 65, 33, 161, 97, 17, 145, 81, 49, 113, 9, 137, 73, 41, 105, 25,
    153, 89, 57, 121, 5, 133, 69, 37, 101, 21, 149, 85, 53, 117, 13, 141,
    77, 45, 109, 29, 157, 93, 61, 125, 3, 131, 67, 35, 99, 19, 147, 83, 51,
    115, 11, 139, 75, 43, 107, 27, 155, 91, 59, 123, 7, 135, 71, 39, 103,
    23, 151, 87, 55, 119, 15, 143, 79, 47, 111, 31, 159, 95, 63, 127 
} ;



  char msg[162];

  int n;
  unsigned long time;

  float shift = 12000.0/8192.0;
  int txtime = 683;  // time in msec (actully 1/shift * 1000 (for msec))
  extern float freq;

  long c = encodecallsign(call) ;
  long g = encodegrid(grid) ;
  long p = encodepower(10) ;
    
  int i;
  int mp = 0 ;
  unsigned long acc = 0;

  for (i=0; i<162; i++) msg[i] = sync[i];
	
 
    /* by default, arduino does not do math well enough (think k&r c from '68) */
  for (i=27; i>=0; i--) {		/* encode the callsign, 28 bits */
    acc <<= 1 ;
    if (c & 1L<<i) acc |= 1;  // NOTE the 1L (damn 8 bit thinking...)
    msg[rdx[mp++]] += 2*parity(acc & 0xf2d05351L) ;
    msg[rdx[mp++]] += 2*parity(acc & 0xe4613c47L) ;
  }

  for (i=14; i>=0; i--) {		/* encode the grid, 15 bits */
    acc <<= 1 ;
    if (g & (1<<i)) acc |= 1 ;
    msg[rdx[mp++]] += 2*parity(acc & 0xf2d05351L) ;
    msg[rdx[mp++]] += 2*parity(acc & 0xe4613c47L) ;
  }

  for (i=6; i>=0; i--) {		/* encode the power, 7 bits */
    acc <<= 1 ;
    if (p & (1<<i)) acc |= 1 ;
    msg[rdx[mp++]] += 2*parity(acc & 0xf2d05351L) ;
    msg[rdx[mp++]] += 2*parity(acc & 0xe4613c47L) ;
  }

  for (i=30; i>=0; i--) {		/* pad with 31 zero bits */
    acc <<= 1 ;
    msg[rdx[mp++]] += 2*parity(acc & 0xf2d05351L) ;
    msg[rdx[mp++]] += 2*parity(acc & 0xe4613c47L) ;
  }
    
    /* we have the 162 byte code in msg[], send it */
    
  lcd1.home();
  lcd1.print("Sending");
  clockgen.enableOutputs(true);  // turn on osc
  for (i=0; i<162; i++) {
    n = msg[i]; 
    freq = wfreq + (n*shift);
    updateOsc();
    time = millis() + txtime;
    while (millis() < time) continue; // wait until 0.683 seconds pass
  }
  clockgen.enableOutputs(false);  // turn off osc
  return;
}


/********** WSPR Calculations Below ***********/

// lots of longs where int was specified to allow for arduino 8 bit math

int chval1(int ch) {
    if (isdigit(ch)) return ch - '0' ;
    if (isalpha(ch)) return 10 + toupper(ch) - 'A' ;
    if (ch == ' ') return 36 ;
}

int chval2(int ch) {
    if (isalpha(ch)) return toupper(ch) - 'A' ;
    if (ch == ' ') return 26 ;
}

long encodecallsign(const char *callsign) {
    /* find the first digit... */
    int i;
    long rc ;
    char call[6] ;

    for (i=0; i<6; i++) call[i] = ' ' ;

    if (isdigit(callsign[1])) {
	/* 1x callsigns... */
	for (i=0; i<strlen(callsign); i++)
	   call[1+i] = callsign[i] ;
    } else if (isdigit(callsign[2])) {
	/* 2x callsigns... */
	for (i=0; i<strlen(callsign); i++)
	   call[i] = callsign[i] ;
    } else {
	return 0 ;
    }

    rc  = chval1(call[0]) ; rc *= 36 ; 
    rc += chval1(call[1]) ; rc *= 10 ;
    rc += chval1(call[2]) ; rc *= 27 ;
    rc += chval2(call[3]) ; rc *= 27 ;
    rc += chval2(call[4]) ; rc *= 27 ;
    rc += chval2(call[5]) ;

    return rc ;
}

long encodegrid(const char *grid) {
    long rc ;

    rc = (179 - 10 * (grid[0]-'A') - (grid[2] - '0')) * 180
	 + (10 * (grid[1]-'A')) + (grid[3] - '0') ;

    return rc ;
}

int encodepower(const int p) {
    return p + 64 ;
}

int parity(unsigned long x) {  // returns 1 or 0, x is a BIG #
    int even = 0 ;
    while (x) {
	even = 1-even ;  // cleaver!
	x = x & (x - 1) ;
    }
    return even ;
}



/*******************/
/**** CW Beacon ****/
/*******************/

// this is just a stub for later

/*
void cwbeacon() {
  int i,n;
  int DELAY;
  int SPEED = 15;    // speed in WPM
  
  const PROGMEM char cwcode[42][8] = {
    "13","3111","3131","311","1","1131","331","1111","11","1333",  // a-j
    "313","1311","33","31","333","1331","3313","131","111","3",    // k-t
    "113","1113","133","3113","3133","3311",                       // u-z
    "33333","13333","11333","11133","11113","11111",               // 0-5
    "31111","33111","33311","33331",                               // 6-9
    "131313","113311","331133",   // period(.), question(?), comma(,)
    "31113","31131"               // double dash (-), dn(/)
  };
  
  const PROGMEM cwmsg[] = "DE MYCALL/5 CM98 BEACON"

}
*/
