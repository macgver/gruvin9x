/*
 * Authors - Bertrand Songis <bsongis@gmail.com>, Bryan J.Rentoul (Gruvin) <gruvin@gmail.com> and Philip Moss
 *
 * Adapted from jeti.cpp code by Karl Szmutny <shadow@privy.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include "gruvin9x.h"
#include "frsky.h"
#include "ff.h"
#include <stdlib.h>

// Enumerate FrSky packet codes
#define LINKPKT         0xfe
#define USRPKT          0xfd
#define A11PKT          0xfc
#define A12PKT          0xfb
#define A21PKT          0xfa
#define A22PKT          0xf9
#define ALRM_REQUEST    0xf8
#define RSSI1PKT        0xf7
#define RSSI2PKT        0xf6

#define START_STOP      0x7e
#define BYTESTUFF       0x7d
#define STUFF_MASK      0x20

// Fr-Sky Single Packet receive buffer. 9 bytes (full packet), worst case 18 bytes with byte-stuffing (+1). Ditto for tx buffer
uint8_t frskyRxPacketBuf[FRSKY_RX_PACKET_SIZE];
uint8_t frskyTxPacketBuf[FRSKY_TX_PACKET_SIZE];
uint8_t frskyTxPacketCount = 0;
uint8_t FrskyRxPacketReady = 0;
uint8_t frskyStreaming = 0;
uint8_t frskyTxISRIndex = 0;

FrskyData frskyTelemetry[2];
FrskyData frskyRSSI[2];

struct FrskyAlarm {
  uint8_t level;    // The alarm's 'urgency' level. 0=disabled, 1=yellow, 2=orange, 3=red
  uint8_t greater;  // 1 = 'if greater than'. 0 = 'if less than'
  uint8_t value;    // The threshold above or below which the alarm will sound
};
struct FrskyAlarm frskyAlarms[4];

class CircularBuffer
{
public:
  uint8_t in;     // input / write index
  uint8_t out;    // output / read index
  uint8_t size;   // queue size
  char *queue;    // data queue (ring)

  CircularBuffer(uint8_t bufSize) // constructor
  {
    in = 0;
    out = 0;
    size = bufSize;
    queue = (char *)malloc(bufSize);
  }

  ~CircularBuffer() // destructor
  {
    free(queue);
  }

  inline void put(uint8_t byte)
  {
    if (((in + 1) % size) != out) // if queue not full
    {
      queue[in] = byte;
      in++;
      in %= size;
    }
  }

  inline uint8_t get()
  {
    uint8_t data;
    if (out != in) // if queue not empty
    {
      data = queue[out];
      out++;
      out %= size;
      return data;
    }
  }

  inline uint8_t isFull()
  {
    return (((in + 1) % size) == out);
  }

  inline uint8_t isEmpty()
  {
    return (out == in);
  }
};

// Fr-Sky User Data receive buffer. (Circular)
#define FRSKY_USER_DATA_SIZE 20
CircularBuffer frskyUserData(FRSKY_USER_DATA_SIZE);


// inherit and extend CircularBuffer to make parsing data more efficient
class FrskyRxParser : public CircularBuffer {

  uint8_t numPktBytes;
  uint8_t dataState;

public:
// packet parser machine states
#define frskyDataIdle    0
#define frskyDataStart   1
#define frskyDataInFrame 2
#define frskyDataXOR     3
  FrskyRxParser(uint8_t bufSize) : CircularBuffer(bufSize) 
  {
    numPktBytes = 0;
    dataState = frskyDataIdle;
  };

  /*
    Fr-Sky telemtry packet parser, called from perMain() for each incoming serial byte
    (State machine)
    When a full packet has been collected, it is processed immediately from here with 
    a call to processFrskyPacket()
  */
  void parseData()
  {
    while (out != in) // if queue not empty
    {
      uint8_t data = get(); // get() is inline to save stack etc

      if (FrskyRxPacketReady == 0) // can't get more data if the buffer hasn't been cleared
      {
        switch (dataState) 
        {
          case frskyDataStart:
            if (data == START_STOP) break; // Remain in userDataStart if possible 0x7e,0x7e doublet found.

            if (numPktBytes < FRSKY_RX_PACKET_SIZE)
              frskyRxPacketBuf[numPktBytes++] = data;
            dataState = frskyDataInFrame;
            break;

          case frskyDataInFrame:
            if (data == BYTESTUFF)
            { 
                dataState = frskyDataXOR; // XOR next byte
                break; 
            }
            if (data == START_STOP) // end of frame detected
            {
              // Process Fr-Sky Packet(uint8_t *frskyRxPacketBuf)

              switch (frskyRxPacketBuf[0]) // What type of frskyRxPacketBuf?
              {
                case A22PKT:
                case A21PKT:
                case A12PKT:
                case A11PKT:
                  {
                    struct FrskyAlarm *alarmptr ;
                    alarmptr = &frskyAlarms[(frskyRxPacketBuf[0]-A22PKT)] ;
                    alarmptr->value = frskyRxPacketBuf[1];
                    alarmptr->greater = frskyRxPacketBuf[2] & 0x01;
                    alarmptr->level = frskyRxPacketBuf[3] & 0x03;
                  }
                  break;

                case LINKPKT: // A1/A2/RSSI values
                  frskyTelemetry[0].set(frskyRxPacketBuf[1]);
                  frskyTelemetry[1].set(frskyRxPacketBuf[2]);
                  frskyRSSI[0].set(frskyRxPacketBuf[3]);
                  frskyRSSI[1].set(frskyRxPacketBuf[4] / 2);
                  break;

                case USRPKT: // User Data frskyRxPacketBuf
                  uint8_t numBytes = frskyRxPacketBuf[1] & 0x07; // sanitize in case of data corruption leading to buffer overflow
                  for(uint8_t i=0; (i < numBytes) && (!frskyUserData.isFull()); i++)
                      frskyUserData.put(frskyRxPacketBuf[3+i]);
                  break;
              }

              FrskyRxPacketReady = 0;
              frskyStreaming = FRSKY_TIMEOUT10ms; // reset counter only if valid frsky frskyRxPacketBufs are being detected

              dataState = frskyDataIdle;
              break;
            }
            frskyRxPacketBuf[numPktBytes++] = data;
            break;

          case frskyDataXOR:
            if (numPktBytes < FRSKY_RX_PACKET_SIZE)
              frskyRxPacketBuf[numPktBytes++] = data ^ STUFF_MASK;
            dataState = frskyDataInFrame;
            break;

          case frskyDataIdle:
            if (data == START_STOP)
            {
              numPktBytes = 0;
              dataState = frskyDataStart;
            }
            break;

        } // switch
      } // if (FrskyRxPacketReady == 0)
    } // while
  }; // parseData method
};

FrskyRxParser frskyRxParser(FRSKY_RX_BUFFER_SIZE);





/*
  Copies all available bytes (up to max bufsize) from frskyUserData circular 
  buffer into supplied *buffer. Returns number of bytes copied (or zero)
*/
uint8_t frskyGetUserData(char *buffer, uint8_t bufSize)
{
  uint8_t i = 0;
  while (!frskyUserData.isEmpty())
  {
    buffer[i] = frskyUserData.get();
    i++;
  }
  return i;
}

/*
  Empties any current USART0 receiver buffer content into the Fr-Sky packet
  parser state machine. (Called from main loop.)
*/
char telemRxByteBuf[FRSKY_RX_BUFFER_SIZE];
void frskyParseRxData()
{
  UCSR0B &= ~(1 << RXCIE0); // disable USART RX interrupt

  frskyRxParser.parseData();

  UCSR0B |= (1 << RXCIE0); // enable USART RX interrupt
}

/*
   Receive USART0 data bytes, loading them into circular buffer
   'frskyRxBuffer'. Swallows incoming bytes if buffer is full.
*/
#ifndef SIMU
ISR(USART0_RX_vect)
{
  UCSR0B &= ~(1 << RXCIE0); // disable (ONLY) USART RX interrupt

  // Must read data byte regardless, to clear interrupt flag
  char data = UDR0;  // USART0 received byte register

  sei(); // un-block other interrupts. VITAL to allow TIMER1_COMPA (PPM_out) int to fire from here

  // Ignore this byte if (frame | overrun | partiy) error
  if ((UCSR0A & ((1 << FE0) | (1 << DOR0) | (1 << UPE0))) == 0)
    frskyRxParser.put(data);

  UCSR0B |= (1 << RXCIE0); // enable (ONLY) USART RX interrupt
}

/*
   USART0 Transmit Data Register Emtpy ISR
   Used to transmit FrSky data packets
*/
ISR(USART0_UDRE_vect)
{
  if (frskyTxPacketCount > 0) {
    UDR0 = frskyTxPacketBuf[frskyTxISRIndex++];
    frskyTxPacketCount--;
  }
  else {
    UCSR0B &= ~(1 << UDRIE0); // disable UDRE0 interrupt
  }
}
#endif

/******************************************/

void frskyPushValue(uint8_t & i, uint8_t value);

void frskyTransmitBuffer()
{
  frskyTxISRIndex = 0;
  UCSR0B |= (1 << UDRIE0); // enable  UDRE0 interrupt
}


uint8_t FrskyAlarmSendState = 0 ;

void FRSKY10mspoll(void)
{

  if (frskyTxPacketCount)
    return; // we only have one buffer. If it's in use, then we can't send yet.

  // Now send a packet
  {
    FrskyAlarmSendState -= 1 ;
    uint8_t channel = 1 - (FrskyAlarmSendState / 2);
    uint8_t alarm = 1 - (FrskyAlarmSendState % 2);
    
    uint8_t i = 0;
    frskyTxPacketBuf[i++] = START_STOP; // Start of packet
    frskyTxPacketBuf[i++] = (A22PKT + FrskyAlarmSendState); // fc - fb - fa - f9
    frskyPushValue(i, g_model.frsky.channels[channel].alarms_value[alarm]);
    {
      uint8_t *ptr ;
      ptr = &frskyTxPacketBuf[i] ;
      *ptr++ = ALARM_GREATER(channel, alarm);
      *ptr++ = ALARM_LEVEL(channel, alarm);
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = START_STOP;        // End of packet
      i += 8 ;
    }
    frskyTxPacketCount = i;
    frskyTransmitBuffer(); 
  }
}

void FRSKY_setRSSIAlarms(void)
{
  if (frskyTxPacketCount) 
    return; // we only have one buffer. If it's in use, then we can't send yet.

  uint8_t i = 0;

  for (int alarm=0; alarm<2; alarm++) {
    frskyTxPacketBuf[i++] = START_STOP;        // Start of packet
    frskyTxPacketBuf[i++] = (RSSI1PKT-alarm);  // f7 - f6
    frskyPushValue(i, g_eeGeneral.frskyRssiAlarms[alarm].value+50-(10*i));
    {
      uint8_t *ptr ;
      ptr = &frskyTxPacketBuf[i] ;
      *ptr++ = 0x00 ;
      *ptr++ = g_eeGeneral.frskyRssiAlarms[alarm].level;
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = 0x00 ;
      *ptr++ = START_STOP;        // End of packet
      i += 8 ;
    }
  }

  frskyTxPacketCount = i;
  frskyTransmitBuffer(); 
}

bool FRSKY_alarmRaised(uint8_t idx)
{
  for (int i=0; i<2; i++) {
    if (ALARM_LEVEL(idx, i) != alarm_off) {
      if (ALARM_GREATER(idx, i)) {
        if (frskyTelemetry[idx].value > g_model.frsky.channels[idx].alarms_value[i])
          return true;
      }
      else {
        if (frskyTelemetry[idx].value < g_model.frsky.channels[idx].alarms_value[i])
          return true;
      }
    }
  }
  return false;
}

inline void FRSKY_EnableTXD(void)
{
  frskyTxPacketCount = 0;
  UCSR0B |= (1 << TXEN0) | (1 << UDRIE0); // enable TX and TX interrupt
}

inline void FRSKY_EnableRXD(void)
{
  UCSR0B |= (1 << RXEN0);  // enable RX
  UCSR0B |= (1 << RXCIE0); // enable Interrupt
}

#if 0
void FRSKY_DisableTXD(void)
{
  UCSR0B &= ~((1 << TXEN0) | (1 << UDRIE0)); // disable TX pin and interrupt
}

void FRSKY_DisableRXD(void)
{
  UCSR0B &= ~(1 << RXEN0);  // disable RX
  UCSR0B &= ~(1 << RXCIE0); // disable Interrupt
}
#endif

void FRSKY_Init(void)
{
  // clear frsky variables
  memset(frskyAlarms, 0, sizeof(frskyAlarms));
  resetTelemetry();

  DDRE &= ~(1 << DDE0);    // set RXD0 pin as input
  PORTE &= ~(1 << PORTE0); // disable pullup on RXD0 pin

#undef BAUD
#define BAUD 9600
#ifndef SIMU
#include <util/setbaud.h>

  UBRR0H = UBRRH_VALUE;
  UBRR0L = UBRRL_VALUE;
  UCSR0A &= ~(1 << U2X0); // disable double speed operation.

  // set 8N1
  UCSR0B = 0 | (0 << RXCIE0) | (0 << TXCIE0) | (0 << UDRIE0) | (0 << RXEN0) | (0 << TXEN0) | (0 << UCSZ02);
  UCSR0C = 0 | (1 << UCSZ01) | (1 << UCSZ00);

  
  while (UCSR0A & (1 << RXC0)) UDR0; // flush receive buffer

#endif

  // These should be running right from power up on a FrSky enabled '9X.
  FRSKY_EnableTXD(); // enable FrSky-Telemetry reception
  FRSKY_EnableRXD(); // enable FrSky-Telemetry reception
}

void frskyPushValue(uint8_t & i, uint8_t value)
{
  // byte stuff the only byte than might need it
  if (value == START_STOP) {
    frskyTxPacketBuf[i++] = BYTESTUFF;
    frskyTxPacketBuf[i++] = 0x5e;
  }
  else if (value == BYTESTUFF) {
    frskyTxPacketBuf[i++] = BYTESTUFF;
    frskyTxPacketBuf[i++] = 0x5d;
  }
  else {
    frskyTxPacketBuf[i++] = value;
  }
}

void FrskyData::set(uint8_t value)
{
   this->value = value;
   if (!max || max < value)
     max = value;
   if (!min || min > value)
     min = value;
 }

#if defined (PCBV3)
char g_logFilename[22]; //  "/G9XLOGS/12345678.000\0" max required length = 22
// These global so we can close any open file from anywhere
FATFS FATFS_Obj;
FIL fil_obj;
#endif
void resetTelemetry()
{
  memset(frskyTelemetry, 0, sizeof(frskyTelemetry));
  memset(frskyRSSI, 0, sizeof(frskyRSSI));

#if defined (PCBV3)

  // Determine and set log file filename
  
  FRESULT result;

  // close any file left open. E.G. Changing models with log switch still on.
  if (fil_obj.fs) f_close(&fil_obj); 

  strcpy(g_logFilename, "/G9XLOGS/M00_000.TXT");

  uint8_t num = g_eeGeneral.currModel + 1;
  itoa(num, (char *)(g_logFilename+10+(num<10)), 10);
  g_logFilename[12] = '_'; // restore the underscore from the \0 itoa wrote

  result = f_mount(0, &FATFS_Obj);
  if (result!=FR_OK)
  {
    strcpy(g_logFilename, "FILE SYSTEM ERROR");
  }
  else
  {
    // Skip over any existing log files ... _000, _001, etc
    while (1)
    {
      result = f_open(&fil_obj, g_logFilename, FA_OPEN_EXISTING | FA_READ);

      if (result == FR_OK)
      {
        f_close(&fil_obj);
        // bump log file counter (file extension)
        num = atoi((char *)(g_logFilename+13));
        if (++num < 1000) // stop at 999, which will simply casue an error when trying to create an existing file
        {
          // figure start column for itoa
          uint8_t index = 15;
          if (num > 9) index--;
          if (num > 99) index--;
          itoa(num, (char *)(g_logFilename+index), 10);
          g_logFilename[16] = '.'; // restore the period from the \0 itoa wrote
        }
      }
      else if (result == FR_NO_PATH)
      {
        if (f_mkdir("/G9XLOGS") != FR_OK)
        {
          result = FR_NO_PATH;
          break;
        }
        else
          continue;
      }
      else
        break;
    }

    switch (result)
    {
      case FR_NO_PATH:
        strcpy(g_logFilename, "Check /G9XLOGS folder");
        break;
      case FR_NOT_READY:
        strcpy(g_logFilename, "DATA CARD NOT PRESENT");
        break;

      default:
        break;
    }
  }

  // g_logFilename should now be set appropriately.

#endif

}

