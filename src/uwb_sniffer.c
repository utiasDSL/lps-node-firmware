/*
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * LPS node firmware.
 *
 * Copyright 2016, Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Foobar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
 */
/* uwb_sniffer.c: Uwb sniffer implementation */

/* ------------------ Modify the uwb_sniffer.c code for TDoA3 CIR log ------------------ */

#include "uwb.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "cfg.h"
#include "led.h"

#include "libdw1000.h"

#include "dwOps.h"
#include "mac.h"

// add header
#include "task.h"
#include "lpp.h"


//------------------------------- useful constents ----------------------------------- //
#define REMOTE_TX_MAX_COUNT 8
#define ID_COUNT 256
#define ANCHOR_STORAGE_COUNT 16
#define ANTENNA_OFFSET 154.6   // In meters
#define ANTENNA_DELAY  ((ANTENNA_OFFSET*499.2e6*128)/299792458.0) // In radio tick
#define MIN_TOF ANTENNA_DELAY
#define ID_COUNT 256
#define ID_WITHOUT_CONTEXT 0xff
#define ID_INVALID 0xff

#define MAX_CLOCK_DEVIATION_SPEC 10e-6
#define CLOCK_CORRECTION_SPEC_MIN (1.0d - MAX_CLOCK_DEVIATION_SPEC * 2)
#define CLOCK_CORRECTION_SPEC_MAX (1.0d + MAX_CLOCK_DEVIATION_SPEC * 2) 

#define CLOCK_CORRECTION_ACCEPTED_NOISE 0.03e-6
#define CLOCK_CORRECTION_FILTER 0.1d
#define CLOCK_CORRECTION_BUCKET_MAX 4

// for computing tof ranging distance
#define ANTENNA_OFFSET  154.6
#define LOCODECK_TS_FREQ  499.2e6 * 128
#define SPEED_OF_LIGHT  299792458.0
// LPP Packet types and format
#define LPP_HEADER_SHORT_PACKET 0xF0
#define LPP_SHORT_ANCHORPOS 0x01
// Time length of the preamble
#define PREAMBLE_LENGTH_S ( 128 * 1017.63e-9 )
#define PREAMBLE_LENGTH (uint64_t)( PREAMBLE_LENGTH_S * 499.2e6 * 128 )
// Guard length to account for clock drift and time of flight
#define TDMA_GUARD_LENGTH_S ( 1e-6 )
#define TDMA_GUARD_LENGTH (uint64_t)( TDMA_GUARD_LENGTH_S * 499.2e6 * 128 )
#define TDMA_EXTRA_LENGTH_S ( 300e-6 )
#define TDMA_EXTRA_LENGTH (uint64_t)( TDMA_EXTRA_LENGTH_S * 499.2e6 * 128 )
#define TDMA_HIGH_RES_RAND_S ( 1e-3 )
#define TDMA_HIGH_RES_RAND (uint64_t)( TDMA_HIGH_RES_RAND_S * 499.2e6 * 128 )
// -------------------------------- necessary defination ------------------------------ //
// tdoa3 packet formats
#define PACKET_TYPE_TDOA3 0x30
// tdoa4 protocol version
#define PACKET_TYPE_TDOA4 0x60
// define a packet type for Agent info
#define LPP_SHORT_AGENT_INFO 0x07

#define PAYLOAD_TYPE 0
#define TYPE 1
#define MODE 2

#define AGENT_ID 1    // select the agent to switch the mode

// Anchor context
typedef struct {
  uint8_t id;
  bool isUsed;
  uint8_t seqNr;
  uint32_t rxTimeStamp;
  uint32_t txTimeStamp;
  uint16_t distance;
  uint32_t distanceUpdateTime;
  bool isDataGoodForTransmission;

  double clockCorrection;
  int clockCorrectionBucket;
} anchorContext_t;

typedef struct {
  uint8_t type;
  uint8_t seq;
  uint32_t txTimeStamp;
  uint8_t remoteCount;
} __attribute__((packed)) rangePacketHeader3_t;

typedef struct {
  uint8_t id;
  uint8_t seq;
  uint32_t rxTimeStamp;
  uint16_t distance;
} __attribute__((packed)) remoteAnchorDataFull_t;

typedef struct {
  uint8_t id;
  uint8_t seq;
  uint32_t rxTimeStamp;
} __attribute__((packed)) remoteAnchorDataShort_t;

typedef struct {
  rangePacketHeader3_t header;
  uint8_t remoteAnchorData;
} __attribute__((packed)) rangePacket3_t;

// [Not used now]
// This context struct contains all the required global values of the algorithm
    // static struct ctx_s {
    //   int anchorId;
    //   // Information about latest transmitted packet
    //   uint8_t seqNr;
    //   uint32_t txTime; 

    //   // Next transmit time in system clock ticks
    //   uint32_t nextTxTick;
    //   int averageTxDelay; // ms

    //   // List of ids to transmit in remote data section
    //   uint8_t remoteTxId[REMOTE_TX_MAX_COUNT];
    //   uint8_t remoteTxIdCount;

    //   // The list of anchors to transmit and store is updated at regular intervals
    //   uint32_t nextAnchorListUpdate;

    //   // Remote anchor data
    //   uint8_t anchorCtxLookup[ID_COUNT];
    //   anchorContext_t anchorCtx[ANCHOR_STORAGE_COUNT];
    //   uint8_t anchorRxCount[ID_COUNT];
// } ctx;
// lpp packet
struct lppShortAnchorPos_s {
      float x;
      float y;
      float z;
      float q0;
      float q1;
      float q2;
      float q3;
    float imu0;
    float imu1;
    float imu2;
    float imu3;
    float imu4;
    float imu5;
} __attribute__((packed));
// [New] Define a struct containing the info of remote "anchor" --> agent
// global variable
static struct remoteAgentInfo_s{
    int remoteAgentID;           // source Agent 
    int destAgentID;             // destination Agent
    bool hasDistance;
    struct lppShortAnchorPos_s remoteData;
    double ranging;
}remoteAgentInfo;
// ------------------------------- Send TxData ------------------------------------ //
static const uint8_t base_address[] = {0,0,0,0,0,0,0xcf,0xbc};
static void adjustTxRxTime(dwTime_t *time)
{
  time->full = (time->full & ~((1 << 9) - 1)) + (1 << 9);
}
// Setup the radio to send a packet
static dwTime_t findTransmitTimeAsSoonAsPossible(dwDevice_t *dev)
{
  dwTime_t transmitTime = { .full = 0 };
  dwGetSystemTimestamp(dev, &transmitTime);
  // Add guard and preamble time
  transmitTime.full += TDMA_GUARD_LENGTH;
  transmitTime.full += PREAMBLE_LENGTH;
  // And some extra
  transmitTime.full += TDMA_EXTRA_LENGTH;
  // DW1000 can only schedule time with 9 LSB at 0, adjust for it
  adjustTxRxTime(&transmitTime);
  return transmitTime;
}
// send short lpp packet
static void setTxData_w(dwDevice_t *dev){
    static packet_t txPacket;
    // static bool firstEntry = true;
    static int lppLength = 0;
    
    // if(firstEntry){
    MAC80215_PACKET_INIT(txPacket, MAC802154_TYPE_DATA);
    // [change]: add '&' in front of txPacket 
    memcpy(&txPacket.sourceAddress, base_address, 8);
    // The ID of the Agent that send the signal. Set to 6 for sniffer
    txPacket.sourceAddress[0] = 6;    
    memcpy(&txPacket.destAddress, base_address, 8);
    txPacket.destAddress[0] = AGENT_ID;      // The ID of the Agent you want to switch  
    txPacket.payload[PAYLOAD_TYPE] = SHORT_LPP;   // payload type
        // firstEntry = false;
    // }

    txPacket.payload[TYPE] = LPP_SHORT_MODE;      // SHORT LPP type
    txPacket.payload[MODE] = LPP_SHORT_MODE_TDOA3; // switch to tdoa3 mode
    lppLength = 3; // SHORT LPP type", "LPP_SHORT_MODE" and "LPP_SHORT_MODE_TDOA3"
    dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH + lppLength);
}

void setupTx_w(dwDevice_t *dev)
{
    dwIdle(dev);
    dwTime_t txTime = findTransmitTimeAsSoonAsPossible(dev);
    //   ctx.txTime = txTime.low32;
    //   ctx.seqNr = (ctx.seqNr + 1) & 0x7f;
    setTxData_w(dev);

    dwNewTransmit(dev);
    dwSetDefaults(dev);
    dwSetTxRxTime(dev, txTime);

    dwStartTransmit(dev);
}
// switch back tdoa4
static void setTxData_d(dwDevice_t *dev){
    static packet_t txPacket;
    // static bool firstEntry = true;
    static int lppLength = 0;
    
    // if(firstEntry){
    MAC80215_PACKET_INIT(txPacket, MAC802154_TYPE_DATA);
    // [change]: add '&' in front of txPacket 
    memcpy(&txPacket.sourceAddress, base_address, 8);
    // The ID of the Agent that send the signal. Set to 6 for sniffer
    txPacket.sourceAddress[0] = 6;    
    memcpy(&txPacket.destAddress, base_address, 8);
    txPacket.destAddress[0] = AGENT_ID;      // The ID of the Agent you want to switch  
    txPacket.payload[PAYLOAD_TYPE] = SHORT_LPP;   // payload type
        // firstEntry = false;
    // }

    txPacket.payload[TYPE] = LPP_SHORT_MODE;      // SHORT LPP type
    lppLength = 3; // SHORT LPP type", "LPP_SHORT_MODE" and "LPP_SHORT_MODE_TDOA3"
    dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH + lppLength);
}

void setupTx_d(dwDevice_t *dev)
{
    dwIdle(dev);
    dwTime_t txTime = findTransmitTimeAsSoonAsPossible(dev);
    //   ctx.txTime = txTime.low32;
    //   ctx.seqNr = (ctx.seqNr + 1) & 0x7f;
    setTxData_d(dev);

    dwNewTransmit(dev);
    dwSetDefaults(dev);
    dwSetTxRxTime(dev, txTime);

    dwStartTransmit(dev);
}
// --------------------------------------------------------------------------------- //
static void setupRx(dwDevice_t *dev)
{
  dwNewReceive(dev);
  dwSetDefaults(dev);
  dwStartReceive(dev);
}

static void handleLppShortPacket(const uint8_t *data, const int length) {
    // printf("get in handleLppShortPacket \r\n");
  uint8_t type = data[0];
  if (type == LPP_SHORT_AGENT_INFO) {
    struct lppShortAnchorPos_s *pos = (struct lppShortAnchorPos_s*)&data[1];
    // printf("Position data is: (%f,%f,%f) \r\n", pos->x, pos->y, pos->z);
    // printf("Raw data: \r\n");
    // for (int i=0; i<length; i++) {
    //     printf("%02x ", data[i]);
    // }
    // results --> Raw data: 01 00 00 00 00 cd cc cc 3d cd cc 4c 3e 
    // a float -> 4 bytes. Raw data 13 bytes, the first one is type. 
    // The rest 12 bytes -> float x, y, z
    remoteAgentInfo.remoteData.x = pos->x;
    remoteAgentInfo.remoteData.y = pos->y;
    remoteAgentInfo.remoteData.z = pos->z;
    remoteAgentInfo.remoteData.q0 = pos->q0;
    remoteAgentInfo.remoteData.q1 = pos->q1;
    remoteAgentInfo.remoteData.q2 = pos->q2;
    remoteAgentInfo.remoteData.q3 = pos->q3;
    remoteAgentInfo.remoteData.imu0 = pos->imu0;
    remoteAgentInfo.remoteData.imu1 = pos->imu1;
    remoteAgentInfo.remoteData.imu2 = pos->imu2;
    remoteAgentInfo.remoteData.imu3 = pos->imu3;
    remoteAgentInfo.remoteData.imu4 = pos->imu4;
    remoteAgentInfo.remoteData.imu5 = pos->imu5;
    }
}

static void handleLppPacket(const int dataLength, int rangePacketLength, const packet_t* rxPacket) {
    // printf("get in handleLppPacket \r\n");
  const int32_t payloadLength = dataLength - MAC802154_HEADER_LENGTH;
  const int32_t startOfLppDataInPayload = rangePacketLength;
  const int32_t lppDataLength = payloadLength - startOfLppDataInPayload;
  const int32_t lppTypeInPayload = startOfLppDataInPayload + 1;
    //   printf("payloadLentgh is %d\r\n",(int)payloadLength);
    //   printf("startOfLppDataInPayload is %d\r\n",(int)startOfLppDataInPayload);
  if (lppDataLength > 0) {
    const uint8_t lppPacketHeader = rxPacket->payload[startOfLppDataInPayload];
    if (lppPacketHeader == LPP_HEADER_SHORT_PACKET) {
      const int32_t lppTypeAndPayloadLength = lppDataLength - 1;
      handleLppShortPacket(&rxPacket->payload[lppTypeInPayload], lppTypeAndPayloadLength);
    }
  }
}

static uint32_t tdoa3SnifferOnEvent(dwDevice_t *dev, uwbEvent_t event){
  static dwTime_t arrival;
  static packet_t rxPacket;
  //check event
  if (event == eventPacketReceived ) {
    float RX_POWER = 0.0f;    // received power
    float FP_POWER = 0.0f;    // first path power
    float SNR = 0.0f;         // signal-noise ratio
    int dataLength = dwGetDataLength(dev);
    dwGetRawReceiveTimestamp(dev, &arrival);
    dwGetData(dev, (uint8_t*)&rxPacket, dataLength);
    // get the FP_POWER and RX_POWER
    FP_POWER = dwGetFirstPathPower(dev);
    RX_POWER = dwGetReceivePower(dev);
    SNR = dwGetReceiveQuality(dev);

    printf("Received UWB signal \r\n");
    printf("The first path power is: %f dBm\r\n", FP_POWER);
    printf("The total received power is: %f dBm\r\n", RX_POWER);
    printf("The FP Amplitude / CIRE-noiseStd  is: %f \r\n", SNR);

    setupRx(dev);
    /*----------------- get access to ID and distance---------------------------*/
    // // For anchor code:
    // uint8_t remoteAnchorId = rxPacket.sourceAddress[0]; 
    remoteAgentInfo.remoteAgentID = *rxPacket.sourceAddress & 0xff;            // save the Agent IDs

    const rangePacket3_t* rangePacket = (rangePacket3_t *)rxPacket.payload;
    const void* anchorDataPtr = &rangePacket->remoteAnchorData;
    if(rxPacket.payload[0] == PACKET_TYPE_TDOA3){
        printf("----------------------Receive TDOA3 msg---------------------\r\n");
        }
     else{ printf("----------------------Receive unknown msg---------------------\r\n");}
  }
    //check event
    else{setupRx(dev);}

  return MAX_TIMEOUT;
}


static void SnifferInit(uwbConfig_t * newconfig, dwDevice_t *dev)
{
  // Set the LED for anchor mode
  ledBlink(ledMode, false);
}

uwbAlgorithm_t uwbSnifferAlgorithm = {
  .init = SnifferInit,
  .onEvent = tdoa3SnifferOnEvent,
};