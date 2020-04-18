/*! ----------------------------------------------------------------------------
*  @file    ss_init_main.c
*  @brief   Single-sided two-way ranging (SS TWR) initiator example code
*
*           This is a simple code example which acts as the initiator in a SS TWR distance measurement exchange.
            This application sends a "poll" frame (recording the TX time-stamp of the poll), 
            after which it waits for a "response" message from the "DS TWR responder" example code (companion to this application)
            to complete the exchange. The response message contains the remote responder's time-stamps of poll RX, and response TX.
            With this data and the local time-stamps, (of poll TX and response RX), this example application works out a value
*           for the time-of-flight over-the-air and, thus, the estimated distance between the two devices, which it writes to the LCD.
*
*
*           Notes at the end of this file, expand on the inline comments.
* 
* @attention
*
* Copyright 2015 (c) Decawave Ltd, Dublin, Ireland.
*
* All rights reserved.
*
* @author Decawave
*/
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "string.h"
#include "inttypes.h"
#include "deca_device_api.h"
#include "deca_regs.h"
#include "port_platform.h"
#include "ss_init_main.h"

#define APP_NAME "SS TWR INIT v1.3"

/* Inter-ranging delay period, in milliseconds. */
#define RNG_DELAY_MS 0

/* Frames used in the ranging process. See NOTE 1,2 below. */
static uint8 rx_resp_msg[] = {0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', 0xE1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
/* Length of the common part of the message (up to and including the function code, see NOTE 1 below). */
#define ALL_MSG_COMMON_LEN 10
/* Indexes to access some of the fields in the frames defined above. */
#define ALL_MSG_SN_IDX 2
#define TAG_ID_IDX_0 7
#define TAG_ID_IDX_1 8
#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14
#define RESP_MSG_TS_LEN 4
/* Frame sequence number, incremented after each transmission. */
static uint8 frame_seq_nb = 0;

/* Buffer to store received response message.
* Its size is adjusted to longest frame that this example code is supposed to handle. */
#define RX_BUF_LEN 20
static uint8 rx_buffer[RX_BUF_LEN];

/* Hold copy of status register state here for reference so that it can be examined at a debug breakpoint. */
static uint32 status_reg = 0;

/* UWB microsecond (uus) to device time unit (dtu, around 15.65 ps) conversion factor.
* 1 uus = 512 / 499.2 s and 1 s = 499.2 * 128 dtu. */
#define UUS_TO_DWT_TIME 65536

/* Speed of light in air, in metres per second. */
#define SPEED_OF_LIGHT 299702547

/* Hold time of flight (in nanoseconds) and distance (in meters) from master anchor here*/
static double dist = 1;
#define TOF (dist * 1e9 / SPEED_OF_LIGHT);

#define rollover (17.2 * 1e9) //nanoseconds

static long long R, tS, tM;
static uint64 masterFramesReceived = 0;
char masterID[] = "MS";
char frameID[2];

/* Declaration of static functions. */
static void resp_msg_get_ts(uint8 *ts_field, uint64 *ts);
static uint64_t get_rx_timestamp_u64(void);

/*Interrupt flag*/
static volatile int tx_int_flag = 0 ; // Transmit success interrupt flag
static volatile int rx_int_flag = 0 ; // Receive success interrupt flag
static volatile int to_int_flag = 0 ; // Timeout interrupt flag
static volatile int er_int_flag = 0 ; // Error interrupt flag 

/*Transactions Counters */
static volatile int tx_count = 0 ; // Successful transmit counter
static volatile int rx_count = 0 ; // Successful receive counter 


/*! ------------------------------------------------------------------------------------------------------------------
* @fn main()
*
* @brief Application entry point.
*
* @param  none
*
* @return none
*/
int ss_init_run(void)
{

  // start reception immediately
  dwt_rxenable(DWT_START_RX_IMMEDIATE);

  /* Wait for reception, timeout or error interrupt flag*/
  while (!(rx_int_flag || to_int_flag|| er_int_flag))
  {};

  if (rx_int_flag)
  {		
    uint32 frame_len;
    uint64_t resp_rx_ts, resp_tx_ts;
    long double resp_rx_ts_microsec, resp_rx_ts_sec;
    long double resp_tx_ts_microsec, resp_tx_ts_sec;
    long long resp_rx_ts_nanosec, resp_tx_ts_nanosec;
    float clockOffsetRatio ;

    /* A frame has been received, read it into the local buffer. */
    frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFLEN_MASK;
    if (frame_len <= RX_BUF_LEN)
    {
      dwt_readrxdata(rx_buffer, frame_len, 0);
    }
  
    frameID[0] = rx_buffer[TAG_ID_IDX_0];
    frameID[1] = rx_buffer[TAG_ID_IDX_1];

    if (strcmp(frameID,masterID) == 0)
    {
      masterFramesReceived++;

      /* Read timestamp of reception & convert to seconds */
      resp_rx_ts = get_rx_timestamp_u64();
      resp_msg_get_ts(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX], &resp_tx_ts);
      //
      resp_rx_ts_microsec = (long double) resp_rx_ts  / (499.2 * 128);
      resp_rx_ts_nanosec = resp_rx_ts_microsec * (1.0e3);
      // resp_rx_ts_sec = resp_rx_ts_microsec / (1.0e6);
      resp_tx_ts_microsec = (long double) resp_tx_ts  / (499.2 * 128);
      resp_tx_ts_nanosec = resp_tx_ts_microsec * (1.0e3);
      // resp_tx_ts_sec = resp_tx_ts_microsec / (1.0e6);

      long long tmN = resp_tx_ts_nanosec - TOF;
      if (tmN < 0) tmN += rollover;

      if (masterFramesReceived >= 2)
      {
        long long tempTM = tmN - tM;
        if (tempTM < 0) tempTM += rollover;

        long long tempTS = resp_rx_ts_nanosec - tS;
        if (tempTS < 0) tempTS += rollover;

        R = (tempTM) / (tempTS);
      }
      tS = resp_rx_ts_nanosec;
      tM = tmN;

      /* Read carrier integrator value and calculate clock offset ratio. See NOTE 6 below. */
      // clockOffsetRatio = dwt_readcarrierintegrator() * (FREQ_OFFSET_MULTIPLIER * HERTZ_TO_PPM_MULTIPLIER_CHAN_5 / 1.0e6) ;

      // printf("resp_rx_ts: %llx\r\n",resp_rx_ts);
      // printf("resp_tx_ts: %llx\r\n",resp_tx_ts);
      /*
      printf("resp_rx_ts_sec: %llf\r\n",resp_rx_ts_sec);
      */
      // printf("resp_rx_ts_nanosec: %lli\r\n",resp_rx_ts_nanosec);
      // printf("resp_tx_ts_nanosec: %lli\r\n",resp_tx_ts_nanosec);
    }

    else if (masterFramesReceived >= 2)
    {
      // get resp_rx_ts_nanosec & calculate syncT
      resp_rx_ts = get_rx_timestamp_u64();
      resp_rx_ts_microsec = (long double) resp_rx_ts  / (499.2 * 128);
      resp_rx_ts_nanosec = resp_rx_ts_microsec * (1.0e3);

      long long tempTS = resp_rx_ts_nanosec - tS;
      if (tempTS < 0) tempTS += rollover;
      long long syncT = (R * tempTS) + tM;

      printf("Reception #: %d\r\n",rx_count);
      printf("Pulse #: %d\r\n",rx_buffer[ALL_MSG_SN_IDX]);
      printf("sync_ts_nanosec: %lli\r\n",syncT);
      printf("masterFramesReceived: %lli\r\n",masterFramesReceived);
      printf("anchor id: MAGENTA\r\n");
      printf("tag id: '%c %c'\r\n",rx_buffer[TAG_ID_IDX_0],rx_buffer[TAG_ID_IDX_1]);
      printf("END frame\r\n"); 
    }

    /*Reseting receive interrupt flag*/
    rx_int_flag = 0; 
   }

  if (to_int_flag || er_int_flag)
  {
    /* Reset RX to properly reinitialise LDE operation. */
    dwt_rxreset();

    /*Reseting interrupt flag*/
    to_int_flag = 0 ;
    er_int_flag = 0 ;
  }

    /* Execute a delay between ranging exchanges. */
    //     deca_sleep(RNG_DELAY_MS);
    //	return(1);
}

/*! ------------------------------------------------------------------------------------------------------------------
* @fn get_rx_timestamp_u64()
*
* @brief Get the RX time-stamp in a 64-bit variable.
*        /!\ This function assumes that length of time-stamps is 40 bits, for both TX and RX!
*
* @param  none
*
* @return  64-bit value of the read time-stamp.
*/
static uint64_t get_rx_timestamp_u64(void)
{
  uint8 ts_tab[5];
  uint64_t ts = 0;
  int i;
  dwt_readrxtimestamp(ts_tab);
  for (i = 4; i >= 0; i--)
  {
    ts <<= 8;
    ts |= ts_tab[i];
  }
  return ts;
}

/*! ------------------------------------------------------------------------------------------------------------------
* @fn rx_ok_cb()
*
* @brief Callback to process RX good frame events
*
* @param  cb_data  callback data
*
* @return  none
*/
void rx_ok_cb(const dwt_cb_data_t *cb_data)
{
  rx_int_flag = 1 ;
  /* TESTING BREAKPOINT LOCATION #1 */
}

/*! ------------------------------------------------------------------------------------------------------------------
* @fn rx_to_cb()
*
* @brief Callback to process RX timeout events
*
* @param  cb_data  callback data
*
* @return  none
*/
void rx_to_cb(const dwt_cb_data_t *cb_data)
{
  to_int_flag = 1 ;
  /* TESTING BREAKPOINT LOCATION #2 */
  printf("TimeOut\r\n");
}

/*! ------------------------------------------------------------------------------------------------------------------
* @fn rx_err_cb()
*
* @brief Callback to process RX error events
*
* @param  cb_data  callback data
*
* @return  none
*/
void rx_err_cb(const dwt_cb_data_t *cb_data)
{
  er_int_flag = 1 ;
  /* TESTING BREAKPOINT LOCATION #3 */
  printf("Transmission Error : may receive package from different UWB device\r\n");
}

/*! ------------------------------------------------------------------------------------------------------------------
* @fn tx_conf_cb()
*
* @brief Callback to process TX confirmation events
*
* @param  cb_data  callback data
*
* @return  none
*/
void tx_conf_cb(const dwt_cb_data_t *cb_data)
{
  /* This callback has been defined so that a breakpoint can be put here to check it is correctly called but there is actually nothing specific to
  * do on transmission confirmation in this example. Typically, we could activate reception for the response here but this is automatically handled
  * by DW1000 using DWT_RESPONSE_EXPECTED parameter when calling dwt_starttx().
  * An actual application that would not need this callback could simply not define it and set the corresponding field to NULL when calling
  * dwt_setcallbacks(). The ISR will not call it which will allow to save some interrupt processing time. */

  tx_int_flag = 1 ;
  /* TESTING BREAKPOINT LOCATION #4 */
}


/*! ------------------------------------------------------------------------------------------------------------------
* @fn resp_msg_get_ts()
*
* @brief Read a given timestamp value from the response message. In the timestamp fields of the response message, the
*        least significant byte is at the lower address.
*
* @param  ts_field  pointer on the first byte of the timestamp field to get
*         ts  timestamp value
*
* @return none
*/
static void resp_msg_get_ts(uint8 *ts_field, uint64 *ts)
{
  int i;
  *ts = 0;
  for (i = 0; i < RESP_MSG_TS_LEN; i++)
  {
  // *ts += ts_field[i] << ((i+1) * 8); // fill in bytes 8-40
  *ts += ts_field[i] << (i * 8); // fill in bytes 8-40
  }
  // zero out upper 24 bytes
  *ts = *ts << 8;
  *ts = *ts & 0xFFFFFFFF00L;
}


/**@brief SS TWR Initiator task entry function.
*
* @param[in] pvParameter   Pointer that will be used as the parameter for the task.
*/
void ss_initiator_task_function (void * pvParameter)
{
  UNUSED_PARAMETER(pvParameter);

  dwt_setleds(DWT_LEDS_ENABLE);

  while (true)
  {
    ss_init_run();
    /* Delay a task for a given number of ticks */
    vTaskDelay(RNG_DELAY_MS);
    /* Tasks must be implemented to never return... */
  }
}
/*****************************************************************************************************************************************************
* NOTES:
*
* 1. The frames used here are Decawave specific ranging frames, complying with the IEEE 802.15.4 standard data frame encoding. The frames are the
*    following:
*     - a poll message sent by the initiator to trigger the ranging exchange.
*     - a response message sent by the responder to complete the exchange and provide all information needed by the initiator to compute the
*       time-of-flight (distance) estimate.
*    The first 10 bytes of those frame are common and are composed of the following fields:
*     - byte 0/1: frame control (0x8841 to indicate a data frame using 16-bit addressing).
*     - byte 2: sequence number, incremented for each new frame.
*     - byte 3/4: PAN ID (0xDECA).
*     - byte 5/6: destination address, see NOTE 2 below.
*     - byte 7/8: source address, see NOTE 2 below.
*     - byte 9: function code (specific values to indicate which message it is in the ranging process).
*    The remaining bytes are specific to each message as follows:
*    Poll message:
*     - no more data
*    Response message:
*     - byte 10 -> 13: poll message reception timestamp.
*     - byte 14 -> 17: response message transmission timestamp.
*    All messages end with a 2-byte checksum automatically set by DW1000.
* 2. Source and destination addresses are hard coded constants in this example to keep it simple but for a real product every device should have a
*    unique ID. Here, 16-bit addressing is used to keep the messages as short as possible but, in an actual application, this should be done only
*    after an exchange of specific messages used to define those short addresses for each device participating to the ranging exchange.
* 3. dwt_writetxdata() takes the full size of the message as a parameter but only copies (size - 2) bytes as the check-sum at the end of the frame is
*    automatically appended by the DW1000. This means that our variable could be two bytes shorter without losing any data (but the sizeof would not
*    work anymore then as we would still have to indicate the full length of the frame to dwt_writetxdata()).
* 4. [ELIMINATED, was justification for using readrxtimestamplo32 instead of readrxtimestamphi32]
* 5. The user is referred to DecaRanging ARM application (distributed with EVK1000 product) for additional practical example of usage, and to the
*     DW1000 API Guide for more details on the DW1000 driver functions.
* 6. The use of the carrier integrator value to correct the TOF calculation, was added Feb 2017 for v1.3 of this example.  This significantly
*     improves the result of the SS-TWR where the remote responder unit's clock is a number of PPM offset from the local inmitiator unit's clock.
*     As stated in NOTE 2 a fixed offset in range will be seen unless the antenna delsy is calibratred and set correctly.
*
****************************************************************************************************************************************************/
