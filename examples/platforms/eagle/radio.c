/*
 *  Copyright (c) 2020, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements the OpenThread platform abstraction for radio communication.
 *
 */

#include <openthread/config.h>
#include <openthread/platform/alarm-milli.h>
#include <openthread/platform/diag.h>
#include <openthread/platform/radio.h>

#include "common/logging.hpp"
#include "utils/code_utils.h"
#include "utils/mac_frame.h"
#include "utils/soft_source_match_table.h"

#include "platform-eagle.h"

enum
{
    IEEE802154_MIN_LENGTH      = 5,
    IEEE802154_MAX_LENGTH      = 127,
    IEEE802154_ACK_LENGTH      = 5,
    IEEE802154_FRAME_TYPE_MASK = 0x7,
    IEEE802154_FRAME_TYPE_ACK  = 0x2,
    IEEE802154_FRAME_PENDING   = 1 << 4,
    IEEE802154_ACK_REQUEST     = 1 << 5,
    IEEE802154_DSN_OFFSET      = 2,
};

typedef enum
{
    PHY_CCA_IDLE    = 0x04,
    PHY_CCA_TRX_OFF = 0x03,
    PHY_CCA_BUSY    = 0x00,
} phy_ccaSts_t;


unsigned char tx_buffer[256] __attribute__((aligned(4)));


unsigned int   r_ptr     = 0;
unsigned int   w_ptr     = 0;

unsigned char current_channel;
#define	LOGICCHANNEL_TO_PHYSICAL(p)					(((p)-10)*5)

static otExtAddress   sExtAddress;
static otShortAddress sShortAddress;
static otPanId        sPanid;
static int8_t         sTxPower = 0;

static otRadioFrame sTransmitFrame;

otRadioFrame        sAckFrame;
static otError      sTransmitError;
static otError      sReceiveError;
static uint8_t      sTransmitPsdu[IEEE802154_MAX_LENGTH];
static uint8_t      sAckPsdu[8];
static otRadioState sState           = OT_RADIO_STATE_DISABLED;
static bool         sSrcMatchEnabled = false;
volatile int        tx_busy          = 0;

#define RX_FRAME_SLOT_NUM 6 
#define MEM_ALIGNMENT 4
#define MEM_ALIGN_SIZE(size) (((size) + MEM_ALIGNMENT - 1) & ~(MEM_ALIGNMENT-1))
unsigned char rx_buffer[160] __attribute__((aligned(4)));
unsigned char rx_frame_slots[RX_FRAME_SLOT_NUM][MEM_ALIGN_SIZE(sizeof(otRadioFrame)) + 160] __attribute__((aligned(4)));
typedef int semaphore;
semaphore empty = RX_FRAME_SLOT_NUM;
semaphore full = 0;
unsigned char rx_frame[MEM_ALIGN_SIZE(sizeof(otRadioFrame)) + 160] __attribute__((aligned(4)));

/**
 * Get the radio capabilities.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 *
 * @returns The radio capability bit vector (see `OT_RADIO_CAP_*` definitions).
 *
 */
otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);
    return OT_RADIO_CAPS_NONE;
}

/**
 * Get the radio receive sensitivity value.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 *
 * @returns The radio receive sensitivity value in dBm.
 *
 */
int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);
    return -99;
}

/**
 * Get the factory-assigned IEEE EUI-64 for this interface.
 *
 * @param[in]  aInstance   The OpenThread instance structure.
 * @param[out] aIeeeEui64  A pointer to the factory-assigned IEEE EUI-64.
 *
 */
void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    OT_UNUSED_VARIABLE(aInstance);

    uint8_t *    eui64;
    unsigned int i;

    eui64 = (uint8_t *)(SETTINGS_CONFIG_IEEE_EUI64_ADDRESS);

    for (i = 0; i < OT_EXT_ADDRESS_SIZE; i++)
    {
        aIeeeEui64[i] = eui64[i];
    }
}

/**
 * Set the PAN ID for address filtering.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 * @param[in] aPanId     The IEEE 802.15.4 PAN ID.
 *
 */
void otPlatRadioSetPanId(otInstance *aInstance, otPanId aPanId)
{
    OT_UNUSED_VARIABLE(aInstance);

    sPanid = aPanId;
    utilsSoftSrcMatchSetPanId(aPanId);
}

static void ReverseExtAddress(otExtAddress *aReversed, const otExtAddress *aOrigin)
{
    for (size_t i = 0; i < sizeof(*aReversed); i++)
    {
        aReversed->m8[i] = aOrigin->m8[sizeof(*aOrigin) - 1 - i];
    }
}

/**
 * Set the Extended Address for address filtering.
 *
 * @param[in] aInstance    The OpenThread instance structure.
 * @param[in] aExtAddress  A pointer to the IEEE 802.15.4 Extended Address stored in little-endian byte order.
 *
 *
 */
void otPlatRadioSetExtendedAddress(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    OT_UNUSED_VARIABLE(aInstance);
    ReverseExtAddress(&sExtAddress, aExtAddress);
}

/**
 * Set the Short Address for address filtering.
 *
 * @param[in] aInstance      The OpenThread instance structure.
 * @param[in] aShortAddress  The IEEE 802.15.4 Short Address.
 *
 */
void otPlatRadioSetShortAddress(otInstance *aInstance, otShortAddress aShortAddress)
{
    OT_UNUSED_VARIABLE(aInstance);
    sShortAddress = aShortAddress;
}

/**
 * Get the radio's transmit power in dBm.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 * @param[out] aPower    The transmit power in dBm.
 *
 * @retval OT_ERROR_NONE             Successfully retrieved the transmit power.
 * @retval OT_ERROR_INVALID_ARGS     @p aPower was NULL.
 * @retval OT_ERROR_NOT_IMPLEMENTED  Transmit power configuration via dBm is not implemented.
 *
 */
otError otPlatRadioGetTransmitPower(otInstance *aInstance, int8_t *aPower)
{
    OT_UNUSED_VARIABLE(aInstance);

    otError error = OT_ERROR_NONE;

    otEXPECT_ACTION(aPower != NULL, error = OT_ERROR_INVALID_ARGS);
    *aPower = sTxPower;

exit:
    return error;
}

/**
 * Set the radio's transmit power in dBm.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 * @param[in] aPower     The transmit power in dBm.
 *
 * @retval OT_ERROR_NONE             Successfully set the transmit power.
 * @retval OT_ERROR_NOT_IMPLEMENTED  Transmit power configuration via dBm is not implemented.
 *
 */
otError otPlatRadioSetTransmitPower(otInstance *aInstance, int8_t aPower)
{
    OT_UNUSED_VARIABLE(aInstance);

    rf_set_power_level (RF_POWER_P9p11dBm);

    sTxPower = aPower;

    return OT_ERROR_NONE;
}

/**
 * Get the radio's CCA ED threshold in dBm.
 *
 * @param[in] aInstance    The OpenThread instance structure.
 * @param[out] aThreshold  The CCA ED threshold in dBm.
 *
 * @retval OT_ERROR_NONE             Successfully retrieved the CCA ED threshold.
 * @retval OT_ERROR_INVALID_ARGS     @p aThreshold was NULL.
 * @retval OT_ERROR_NOT_IMPLEMENTED  CCA ED threshold configuration via dBm is not implemented.
 *
 */
otError otPlatRadioGetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t *aThreshold)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aThreshold);

    return OT_ERROR_NOT_IMPLEMENTED;
}

/**
 * Set the radio's CCA ED threshold in dBm.
 *
 * @param[in] aInstance   The OpenThread instance structure.
 * @param[in] aThreshold  The CCA ED threshold in dBm.
 *
 * @retval OT_ERROR_NONE             Successfully set the transmit power.
 * @retval OT_ERROR_INVALID_ARGS     Given threshold is out of range.
 * @retval OT_ERROR_NOT_IMPLEMENTED  CCA ED threshold configuration via dBm is not implemented.
 *
 */
otError otPlatRadioSetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t aThreshold)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aThreshold);

    return OT_ERROR_NOT_IMPLEMENTED;
}

/**
 * Get the status of promiscuous mode.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 *
 * @retval TRUE   Promiscuous mode is enabled.
 * @retval FALSE  Promiscuous mode is disabled.
 *
 */
bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);
    return false;
}

/**
 * Enable or disable promiscuous mode.
 *
 * @param[in]  aInstance The OpenThread instance structure.
 * @param[in]  aEnable   TRUE to enable or FALSE to disable promiscuous mode.
 *
 */
void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aEnable);
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return (sState != OT_RADIO_STATE_DISABLED) ? true : false;
}

otError otPlatRadioEnable(otInstance *aInstance)
{
    if (!otPlatRadioIsEnabled(aInstance))
    {
        otLogDebgPlat("State=OT_RADIO_STATE_SLEEP", NULL);
        sState = OT_RADIO_STATE_SLEEP;
    }

    return OT_ERROR_NONE;
}

otError otPlatRadioDisable(otInstance *aInstance)
{
    if (otPlatRadioIsEnabled(aInstance))
    {
        otLogDebgPlat("State=OT_RADIO_STATE_DISABLED", NULL);
        sState = OT_RADIO_STATE_DISABLED;
    }

    return OT_ERROR_NONE;
}

otError otPlatRadioSleep(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    otError error = OT_ERROR_INVALID_STATE;

    if (sState == OT_RADIO_STATE_SLEEP || sState == OT_RADIO_STATE_RECEIVE)
    {
        otLogDebgPlat("State=OT_RADIO_STATE_SLEEP", NULL);
        error  = OT_ERROR_NONE;
        sState = OT_RADIO_STATE_SLEEP;
        rf_set_txmode();
    }

    return error;
}


void rf_set_channel(uint8_t channel)
{
    current_channel = channel;
    rf_set_chn(LOGICCHANNEL_TO_PHYSICAL(channel));
}

otError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    OT_UNUSED_VARIABLE(aInstance);

    otError error = OT_ERROR_INVALID_STATE;

    if (sState != OT_RADIO_STATE_DISABLED)
    {
        otLogDebgPlat("State=OT_RADIO_STATE_RECEIVE", NULL);

        error  = OT_ERROR_NONE;
        sState = OT_RADIO_STATE_RECEIVE;
        rf_set_channel(aChannel);
        rf_set_rxmode();
    }

    return error;
}

/**
 * Get the radio transmit frame buffer.
 *
 * OpenThread forms the IEEE 802.15.4 frame in this buffer then calls `otPlatRadioTransmit()` to request transmission.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 *
 * @returns A pointer to the transmit frame buffer.
 *
 */
otRadioFrame *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return &sTransmitFrame;
}

/**
 * Get the most recent RSSI measurement.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 *
 * @returns The RSSI in dBm when it is valid.  127 when RSSI is invalid.
 *
 */
int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);
    signed char rssi_cur = -110;
    rssi_cur             = rf_get_rssi();
    return rssi_cur;
}

/**
 * Begin the energy scan sequence on the radio.
 *
 * This function is used when radio provides OT_RADIO_CAPS_ENERGY_SCAN capability.
 *
 * @param[in] aInstance      The OpenThread instance structure.
 * @param[in] aScanChannel   The channel to perform the energy scan on.
 * @param[in] aScanDuration  The duration, in milliseconds, for the channel to be scanned.
 *
 * @retval OT_ERROR_NONE             Successfully started scanning the channel.
 * @retval OT_ERROR_NOT_IMPLEMENTED  The radio doesn't support energy scanning.
 *
 */
otError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aScanChannel);
    OT_UNUSED_VARIABLE(aScanDuration);

    return OT_ERROR_NOT_IMPLEMENTED;
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    OT_UNUSED_VARIABLE(aInstance);

    sSrcMatchEnabled = aEnable;
}



static uint32_t m_in_critical_region = 0;

static inline void util_disable_rf_irq(void)
{
    rf_clr_irq_mask(FLD_RF_IRQ_RX);
    m_in_critical_region++;
}

static inline void util_enable_rf_irq(void)
{
    m_in_critical_region--;
    if (m_in_critical_region == 0)
    {
        rf_set_irq_mask(FLD_RF_IRQ_RX);
    }
}


static void setupTransmit(otRadioFrame *aFrame)
{
    int           i;
    unsigned char rf_data_len;
    unsigned int  rf_tx_dma_len;

    rf_data_len   = aFrame->mLength - 1;
    tx_buffer[4]  = aFrame->mLength;
    rf_tx_dma_len = rf_tx_packet_dma_len(rf_data_len);
    tx_buffer[3]  = (rf_tx_dma_len >> 24) & 0xff;
    tx_buffer[2]  = (rf_tx_dma_len >> 16) & 0xff;
    tx_buffer[1]  = (rf_tx_dma_len >> 8) & 0xff;
    tx_buffer[0]  = rf_tx_dma_len & 0xff;

    for (i = 0; i < aFrame->mLength - 2; i++)
    {
        tx_buffer[5 + i] = aFrame->mPsdu[i];
    }

    rf_set_channel(aFrame->mChannel);
}

phy_ccaSts_t rf_performCCA(void)
{
    unsigned int t1        = clock_time();
    signed char  rssi_peak = -110;
    signed char  rssi_cur  = -110;

    while (!clock_time_exceed(t1, 128))
    {
        rssi_cur = rf_get_rssi();
        if (rssi_cur > rssi_peak)
        {
            rssi_peak = rssi_cur;
        }
    }

    if (rssi_peak > -60)
    {
        // return PHY_CCA_BUSY;
        return PHY_CCA_IDLE;
    }
    else
    {
        return PHY_CCA_IDLE;
    }
}

otError otPlatRadioTransmit(otInstance *aInstance, otRadioFrame *aFrame)
{
    OT_UNUSED_VARIABLE(aInstance);

    otError error = OT_ERROR_INVALID_STATE;

    if (sState == OT_RADIO_STATE_RECEIVE)
    {
        phy_ccaSts_t cca_result;

        error          = OT_ERROR_NONE;
        sState         = OT_RADIO_STATE_TRANSMIT;
        sTransmitError = OT_ERROR_NONE;

        setupTransmit(aFrame);

        // wait for valid rssi
        cca_result = rf_performCCA();

        otEXPECT_ACTION((cca_result == PHY_CCA_IDLE), sTransmitError = OT_ERROR_CHANNEL_ACCESS_FAILURE);

        // begin transmit
        tx_busy = 1;
        rf_set_txmode();
        rf_tx_pkt(tx_buffer);
        otPlatRadioTxStarted(aInstance, aFrame);
    }

exit:
    return error;
}

bool isDataRequestAndHasFramePending(const otRadioFrame *aFrame)
{
    bool         rval = false;
    otMacAddress src;

    otEXPECT(otMacFrameIsDataRequest(aFrame));
    otEXPECT_ACTION(sSrcMatchEnabled, rval = true);
    otEXPECT(otMacFrameGetSrcAddr(aFrame, &src) == OT_ERROR_NONE);

    switch (src.mType)
    {
    case OT_MAC_ADDRESS_TYPE_SHORT:
        rval = utilsSoftSrcMatchShortFindEntry(src.mAddress.mShortAddress) >= 0;
        break;
    case OT_MAC_ADDRESS_TYPE_EXTENDED:
    {
        otExtAddress extAddr;

        ReverseExtAddress(&extAddr, &src.mAddress.mExtAddress);
        rval = utilsSoftSrcMatchExtFindEntry(&extAddr) >= 0;
        break;
    }
    default:
        break;
    }

exit:
    return rval;
}



uint8_t rf_rssi_to_lqi(int8_t aRss)
{
    int8_t  aNoiseFloor = -99;
    int8_t  linkMargin  = aRss - aNoiseFloor;
    uint8_t aLinkMargin;
    uint8_t threshold1, threshold2, threshold3;
    uint8_t linkQuality = 0;

    if (linkMargin < 0 || aRss == OT_RADIO_RSSI_INVALID)
    {
        linkMargin = 0;
    }

    aLinkMargin = (uint8_t)(linkMargin);

    threshold1 = 2;
    threshold2 = 10;
    threshold3 = 20;

    if (aLinkMargin > threshold3)
    {
        linkQuality = 3;
    }
    else if (aLinkMargin > threshold2)
    {
        linkQuality = 2;
    }
    else if (aLinkMargin > threshold1)
    {
        linkQuality = 1;
    }

    return linkQuality;
}

void eagleRadioInit(void)
{
    int i;
    otRadioFrame *p;

    sTransmitFrame.mLength = 0;
    sTransmitFrame.mPsdu   = sTransmitPsdu;

    for (i = 0; i < RX_FRAME_SLOT_NUM; i++)
    {
        p = (otRadioFrame *)rx_frame_slots[i];
        p->mLength = 0;
        p->mPsdu = &(rx_frame_slots[i][MEM_ALIGN_SIZE(sizeof(otRadioFrame))]);
    }
    p = (otRadioFrame *)rx_frame;
    p->mLength = 0;
    p->mPsdu = &(rx_frame[MEM_ALIGN_SIZE(sizeof(otRadioFrame))]);

    sAckFrame.mLength = 0;
    sAckFrame.mPsdu   = sAckPsdu;

    rf_mode_init();
    rf_set_zigbee_250K_mode();
    rf_set_power_level (RF_POWER_P9p11dBm);
    rf_set_tx_dma(2,256);
    rf_set_rx_dma(rx_buffer,3,256);
    plic_interrupt_enable(IRQ15_ZB_RT);
	rf_set_irq_mask(FLD_RF_IRQ_RX|FLD_RF_IRQ_TX);
}

void EagleRxTxIntHandler()
{
    if (rf_get_irq_status(FLD_RF_IRQ_RX))
    {
        dma_chn_dis(DMA1);
        rf_clr_irq_status(FLD_RF_IRQ_RX);

        if (rf_zigbee_packet_crc_ok(rx_buffer) /*&& rf_zigbee_packet_length_ok(rx_buffer)*/)
        {
            uint8_t     length;
            otRadioFrame *rx_frame_ptr;
            int i;
            
            //radioProcessFrame();
            otEXPECT(sState == OT_RADIO_STATE_RECEIVE || sState == OT_RADIO_STATE_TRANSMIT);
            length = rx_buffer[4];
            otEXPECT(IEEE802154_MIN_LENGTH <= length && length <= IEEE802154_MAX_LENGTH);
            
            if(empty > 0)
            {
                
            }
            else
            {
                goto exit;
            }
            
            rx_frame_ptr = (otRadioFrame *)rx_frame_slots[w_ptr];
            rx_frame_ptr->mLength  = length;
            rx_frame_ptr->mChannel = current_channel;
            for (i = 0; i < length - 2; i++)    
            {        
                rx_frame_ptr->mPsdu[i] = rx_buffer[5+i];    
            }
            if (length == IEEE802154_ACK_LENGTH)
            {
                empty --;
                w_ptr = (w_ptr + 1) % RX_FRAME_SLOT_NUM;
                full ++;
            }
            else
            {
                if (!otMacFrameDoesAddrMatch(rx_frame_ptr, sPanid, sShortAddress, &sExtAddress))
                {
                    // drop the frame
                    goto exit;
                }
                rx_frame_ptr->mInfo.mRxInfo.mRssi = ((signed char)(rx_buffer[rx_buffer[4] + 11])) - 110;
                rx_frame_ptr->mInfo.mRxInfo.mLqi  = rf_rssi_to_lqi(rx_frame_ptr->mInfo.mRxInfo.mRssi);
                rx_frame_ptr->mInfo.mRxInfo.mAckedWithFramePending = false;

                // generate acknowledgment
                if (otMacFrameIsAckRequested(rx_frame_ptr))
                {
                    sAckFrame.mLength  = IEEE802154_ACK_LENGTH;
                    sAckFrame.mPsdu[0] = IEEE802154_FRAME_TYPE_ACK;

                    if (isDataRequestAndHasFramePending(rx_frame_ptr))
                    {
                        sAckFrame.mPsdu[0] |= IEEE802154_FRAME_PENDING;
                        rx_frame_ptr->mInfo.mRxInfo.mAckedWithFramePending = true;
                    }

                    sAckFrame.mPsdu[1] = 0;
                    sAckFrame.mPsdu[2] = otMacFrameGetSequence(rx_frame_ptr);

                    sAckFrame.mChannel = rx_frame_ptr->mChannel;

                    // Transmit
                    setupTransmit(&sAckFrame);
                    rf_set_txmode();
                    rf_tx_pkt(tx_buffer);
                    //rf_start_stx(tx_buffer,0);
                }

                // push the frame
                empty --;
                w_ptr = (w_ptr + 1) % RX_FRAME_SLOT_NUM;
                full ++;
            }
        }

        exit:
        dma_chn_en(DMA1);
    }
    else if (rf_get_irq_status(FLD_RF_IRQ_TX))
    {
        rf_clr_irq_status(FLD_RF_IRQ_TX);

        if ((tx_busy == 1) && (sState == OT_RADIO_STATE_TRANSMIT))
        {
            tx_busy = 0;
        }
        // set to rx mode
        rf_set_rxmode();
    }

    //plic_interrupt_complete(IRQ15_ZB_RT);
}

void eagleRadioProcess(otInstance *aInstance)
{
    otRadioFrame *ptr;
    otRadioFrame *rx_frame_ptr;
    int i;

    ptr = (otRadioFrame *)rx_frame;
    util_disable_rf_irq();
    if(full > 0)
    {
        full --;
        rx_frame_ptr = (otRadioFrame *)rx_frame_slots[r_ptr];
        ptr->mChannel = rx_frame_ptr->mChannel;
        ptr->mLength = rx_frame_ptr->mLength;
        for (i = 0; i < ptr->mLength - 2; i++)    
        {        
            ptr->mPsdu[i] = rx_frame_ptr->mPsdu[i];    
        }

        ptr->mInfo.mRxInfo.mRssi = rx_frame_ptr->mInfo.mRxInfo.mRssi;
        ptr->mInfo.mRxInfo.mLqi = rx_frame_ptr->mInfo.mRxInfo.mLqi;
        ptr->mInfo.mRxInfo.mAckedWithFramePending = rx_frame_ptr->mInfo.mRxInfo.mAckedWithFramePending;
        r_ptr = (r_ptr + 1) % RX_FRAME_SLOT_NUM;
        empty ++;
        util_enable_rf_irq();
    }
    else
    {
        util_enable_rf_irq();
        ptr = NULL;
    }
    

    if ((sState == OT_RADIO_STATE_RECEIVE) || (sState == OT_RADIO_STATE_TRANSMIT))
    {
        if ((ptr != NULL) && (ptr->mLength > IEEE802154_ACK_LENGTH))
        {
            otPlatRadioReceiveDone(aInstance, ptr, sReceiveError);
        }
    }

    if (sState == OT_RADIO_STATE_TRANSMIT)
    {
        if (sTransmitError != OT_ERROR_NONE ||
            (((sTransmitFrame.mPsdu[0] & IEEE802154_ACK_REQUEST) == 0) && (tx_busy == 0)))
        {
            sState = OT_RADIO_STATE_RECEIVE;
            otPlatRadioTxDone(aInstance, &sTransmitFrame, NULL, sTransmitError);
        }
        else if ((ptr != NULL) && ptr->mLength == IEEE802154_ACK_LENGTH &&
                 (ptr->mPsdu[0] & IEEE802154_FRAME_TYPE_MASK) == IEEE802154_FRAME_TYPE_ACK &&
                 (ptr->mPsdu[IEEE802154_DSN_OFFSET] == sTransmitFrame.mPsdu[IEEE802154_DSN_OFFSET]))
        {
            sState = OT_RADIO_STATE_RECEIVE;
            otPlatRadioTxDone(aInstance, &sTransmitFrame, ptr, sTransmitError);
        }
    }
}

