/*
 * Copyright (c) 2002-2005 Sam Leffler, Errno Consulting
 * Copyright (c) 2009, Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#include "opt_ah.h"

#ifdef AH_SUPPORT_2133

#include "ah.h"
#include "ah_internal.h"

#include "ar5416/ar5416.h"
#include "ar5416/ar5416reg.h"
#include "ar5416/ar5416phy.h"

#define REG_WRITE_RF_ARRAY(iniarray, regData, regWr) do {               \
    int r;                              \
    for (r = 0; r < ((iniarray)->ia_rows); r++) {             \
        OS_REG_WRITE(ah, INI_RA((iniarray), r, 0), (regData)[r]);   \
		HDPRINTF(ah, HAL_DBG_CHANNEL, "RF 0x%x V 0x%x\n", INI_RA((iniarray), r, 0), (regData)[r]); \
        WAR_6773(regWr);                    \
    }                               \
} while (0)

extern  void ar5416ModifyRfBuffer(u_int32_t *rfBuf, u_int32_t reg32,
        u_int32_t numBits, u_int32_t firstBit, u_int32_t column);

#ifdef ATH_FORCE_BIAS
static void ar5416ForceBiasCurrent(struct ath_hal *ah, u_int16_t synth_freq);
#endif

static void
ar5416DecreaseChainPower(struct ath_hal *ah, HAL_CHANNEL *chan);

static void
ar2133WriteRegs(struct ath_hal *ah, u_int modesIndex, u_int freqIndex, int regWrites)
{
    struct ath_hal_5416 *ahp = AH5416(ah);

    REG_WRITE_ARRAY(&ahp->ah_iniBB_RfGain, freqIndex, regWrites);
}

/*
 * Take the MHz channel value and set the Channel value
 *
 * New for 2133: Workaround FOWL bug by forcing rf_pwd_icsyndiv
 * value as a function of synth frequency.
 *
 * ASSUMES: Writes enabled to analog bus
 */
static HAL_BOOL
ar2133SetChannel(struct ath_hal *ah,  HAL_CHANNEL_INTERNAL *chan)
{
    u_int32_t channelSel  = 0;
    u_int32_t bModeSynth  = 0;
    u_int32_t aModeRefSel = 0;
    u_int32_t reg32       = 0;
    u_int16_t freq;
    CHAN_CENTERS centers;

    OS_MARK(ah, AH_MARK_SETCHANNEL, chan->channel);

    ar5416GetChannelCenters(ah, chan, &centers);
    freq = centers.synth_center;

    if (freq < 4800) {
        u_int32_t txctl;

        if (((freq - 2192) % 5) == 0) {
            channelSel = ((freq - 672) * 2 - 3040)/10;
            bModeSynth = 0;
        } else if (((freq - 2224) % 5) == 0) {
            channelSel = ((freq - 704) * 2 - 3040) / 10;
            bModeSynth = 1;
        } else {
            HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: invalid channel %u MHz\n",
                __func__, freq);
            return AH_FALSE;
        }

        channelSel = (channelSel << 2) & 0xff;
        channelSel = ath_hal_reverseBits(channelSel, 8);

        txctl = OS_REG_READ(ah, AR_PHY_CCK_TX_CTRL);
        if (freq == 2484) {
            /* Enable channel spreading for channel 14 */
            OS_REG_WRITE(ah, AR_PHY_CCK_TX_CTRL,
                txctl | AR_PHY_CCK_TX_CTRL_JAPAN);
        } else {
            OS_REG_WRITE(ah, AR_PHY_CCK_TX_CTRL,
                txctl &~ AR_PHY_CCK_TX_CTRL_JAPAN);
        }

    } else if ((freq % 20) == 0 && freq >= 5120) {
        channelSel = ath_hal_reverseBits(
            ((freq - 4800) / 20 << 2), 8);
        if (AR_SREV_HOWL(ah) || AR_SREV_SOWL_10_OR_LATER(ah))
            aModeRefSel = ath_hal_reverseBits(3, 2);
        else
            aModeRefSel = ath_hal_reverseBits(1, 2);
    } else if ((freq % 10) == 0) {
        channelSel = ath_hal_reverseBits(
            ((freq - 4800) / 10 << 1), 8);
        if (AR_SREV_HOWL(ah) || AR_SREV_SOWL_10_OR_LATER(ah))
            aModeRefSel = ath_hal_reverseBits(2, 2);
        else
            aModeRefSel = ath_hal_reverseBits(1, 2);
    } else if ((freq % 5) == 0) {
        channelSel = ath_hal_reverseBits(
            (freq - 4800) / 5, 8);
        aModeRefSel = ath_hal_reverseBits(1, 2);
    } else {
        HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: invalid channel %u MHz\n",
            __func__, freq);
        return AH_FALSE;
    }

#ifdef ATH_FORCE_BIAS
    /* FOWL orientation sensitivity workaround */
    ar5416ForceBiasCurrent(ah, freq);

    /*
     * Antenna Control with forceBias.
     * This function must be called after ar5416ForceBiasCurrent() and
     * ar5416SetRfRegs() and ar5416EepromSetBoardValues().
     */
    ar5416DecreaseChainPower(ah, (HAL_CHANNEL*)chan);
#endif

    reg32 = (channelSel << 8) | (aModeRefSel << 2) | (bModeSynth << 1) |
            (1 << 5) | 0x1;

    OS_REG_WRITE(ah, AR_PHY(0x37), reg32);

    AH_PRIVATE(ah)->ah_curchan = chan;

#ifdef AH_SUPPORT_DFS
    if (chan->privFlags & CHANNEL_DFS) {
        struct ar5416RadarState *rs;
        u_int8_t index;

        rs = ar5416GetRadarChanState(ah, &index);
        if (rs != AH_NULL) {
            AH5416(ah)->ah_curchanRadIndex = (int16_t) index;
        } else {
            HDPRINTF(ah, HAL_DBG_DFS, "%s: Couldn't find radar state information\n",
                 __func__);
            return AH_FALSE;
        }
    } else
#endif
        AH5416(ah)->ah_curchanRadIndex = -1;

    return AH_TRUE;
}

/*
 * Take the MHz channel value and set the Channel value
 *
 * ASSUMES: Writes enabled to analog bus
 *
 * Actual Expression,
 *
 * For 2GHz channel, 
 * Channel Frequency = (3/4) * freq_ref * (chansel[8:0] + chanfrac[16:0]/2^17) 
 * (freq_ref = 40MHz)
 *
 * For 5GHz channel,
 * Channel Frequency = (3/2) * freq_ref * (chansel[8:0] + chanfrac[16:0]/2^10)
 * (freq_ref = 40MHz/(24>>amodeRefSel))
 *
 * For 5GHz channels which are 5MHz spaced,
 * Channel Frequency = (3/2) * freq_ref * (chansel[8:0] + chanfrac[16:0]/2^17)
 * (freq_ref = 40MHz)
 */
static HAL_BOOL
ar9280SetChannel(struct ath_hal *ah,  HAL_CHANNEL_INTERNAL *chan)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    u_int16_t bMode, fracMode, aModeRefSel = 0;
    u_int32_t freq, ndiv, channelSel = 0, channelFrac = 0, reg32 = 0;
    CHAN_CENTERS centers;
    u_int32_t refDivA = 24;

    OS_MARK(ah, AH_MARK_SETCHANNEL, chan->channel);

    ar5416GetChannelCenters(ah, chan, &centers);
    freq = centers.synth_center;
    
    reg32 = OS_REG_READ(ah, AR_PHY_SYNTH_CONTROL);
    reg32 &= 0xc0000000;

    if (freq < 4800) {     /* 2 GHz, fractional mode */
        u_int32_t txctl;

        bMode = 1;
        fracMode = 1;
        aModeRefSel = 0;       
        channelSel = (freq * 0x10000)/15;

        txctl = OS_REG_READ(ah, AR_PHY_CCK_TX_CTRL);
        if (freq == 2484) {
            /* Enable channel spreading for channel 14 */
            OS_REG_WRITE(ah, AR_PHY_CCK_TX_CTRL,
                txctl | AR_PHY_CCK_TX_CTRL_JAPAN);
        } else {
            OS_REG_WRITE(ah, AR_PHY_CCK_TX_CTRL,
                txctl &~ AR_PHY_CCK_TX_CTRL_JAPAN);
        }     
    } else {
        bMode = 0;
        fracMode = 0;

        switch (ar5416EepromGet(ahp, EEP_FRAC_N_5G)) {
        case 0:
            if ((freq % 20) == 0) {
                aModeRefSel = 3;
            } else if ((freq % 10) == 0) {
                aModeRefSel = 2;
            }
            if (aModeRefSel) break;
        case 1:
        default:
            aModeRefSel = 0;
            /* Enable 2G (fractional) mode for channels which are 5MHz spaced */
            fracMode = 1;
            refDivA = 1;
            channelSel = (freq * 0x8000)/15;

            /* RefDivA setting */
            OS_REG_RMW_FIELD(ah, AR_AN_SYNTH9, AR_AN_SYNTH9_REFDIVA, refDivA);
        }

        if (!fracMode) {
            ndiv = (freq * (refDivA >> aModeRefSel))/60;
            channelSel =  ndiv & 0x1ff;         
            channelFrac = (ndiv & 0xfffffe00) * 2;
            channelSel = (channelSel << 17) | channelFrac;
        }
    }

    reg32 = reg32 | 
           (bMode << 29) |
           (fracMode << 28) |
           (aModeRefSel << 26) |
           (channelSel);

    OS_REG_WRITE(ah, AR_PHY_SYNTH_CONTROL, reg32);

    AH_PRIVATE(ah)->ah_curchan = chan;

#ifdef AH_SUPPORT_DFS
    if (chan->privFlags & CHANNEL_DFS) {
        struct ar5416RadarState *rs;
        u_int8_t index;

        rs = ar5416GetRadarChanState(ah, &index);
        if (rs != AH_NULL) {
            AH5416(ah)->ah_curchanRadIndex = (int16_t) index;
        } else {
            HDPRINTF(ah, HAL_DBG_DFS, "%s: Couldn't find radar state information\n",
                 __func__);
            return AH_FALSE;
        }
    } else
#endif
        AH5416(ah)->ah_curchanRadIndex = -1;

    return AH_TRUE;
}

/*
 * Reads EEPROM header info from device structure and programs
 * all rf registers
 *
 * REQUIRES: Access to the analog rf device
 */
static HAL_BOOL
ar2133SetRfRegs(struct ath_hal *ah, HAL_CHANNEL_INTERNAL *chan,
                u_int16_t modesIndex)
{
#define RF_BANK_SETUP(_bank, _iniarray, _col) do {                  \
    int i;                                  \
    for (i = 0; i < (_iniarray)->ia_rows; i++)             \
        (_bank)[i] = INI_RA((_iniarray), i, _col);;\
} while (0)

    struct ath_hal_5416 *ahp = AH5416(ah);

    u_int32_t eepMinorRev;
    u_int32_t ob5GHz = 0, db5GHz = 0;
    u_int32_t ob2GHz = 0, db2GHz = 0;
    int regWrites = 0;

    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
        /* Software does not need to program bank data for chips after Merlin */
        return AH_TRUE;
    }

    HALASSERT(ahp->ah_analogBank0Data);
    HALASSERT(ahp->ah_analogBank1Data);
    HALASSERT(ahp->ah_analogBank2Data);
    HALASSERT(ahp->ah_analogBank3Data);
    HALASSERT(ahp->ah_analogBank6Data);
    HALASSERT(ahp->ah_analogBank6TPCData);
    HALASSERT(ahp->ah_analogBank7Data);

    /* Setup rf parameters */
    eepMinorRev = ar5416EepromGet(ahp, EEP_MINOR_REV);

    /* Setup Bank 0 Write */
    RF_BANK_SETUP(ahp->ah_analogBank0Data, &ahp->ah_iniBank0, 1);

    /* Setup Bank 1 Write */
    RF_BANK_SETUP(ahp->ah_analogBank1Data, &ahp->ah_iniBank1, 1);

    /* Setup Bank 2 Write */
    RF_BANK_SETUP(ahp->ah_analogBank2Data, &ahp->ah_iniBank2, 1);

    /* Setup Bank 3 Write */
    RF_BANK_SETUP(ahp->ah_analogBank3Data, &ahp->ah_iniBank3, modesIndex);

    /* Setup Bank 6 Write */
    {
        int i;
        for (i = 0; i < ahp->ah_iniBank6TPC.ia_rows; i++) {
            ahp->ah_analogBank6Data[i] = INI_RA(&ahp->ah_iniBank6TPC, i, modesIndex);
        }
    }

    /* Only the 5 or 2 GHz OB/DB need to be set for a mode */
    if (eepMinorRev >= 2) {
        if (IS_CHAN_2GHZ(chan)) {
            ob2GHz = ar5416EepromGet(ahp, EEP_OB_2);
            db2GHz = ar5416EepromGet(ahp, EEP_DB_2);
            ar5416ModifyRfBuffer(ahp->ah_analogBank6Data, ob2GHz, 3, 197, 0);
            ar5416ModifyRfBuffer(ahp->ah_analogBank6Data, db2GHz, 3, 194, 0);
        } else {
            ob5GHz = ar5416EepromGet(ahp, EEP_OB_5);
            db5GHz = ar5416EepromGet(ahp, EEP_DB_5);
            ar5416ModifyRfBuffer(ahp->ah_analogBank6Data, ob5GHz, 3, 203, 0);
            ar5416ModifyRfBuffer(ahp->ah_analogBank6Data, db5GHz, 3, 200, 0);
        }
    }

    /* Setup Bank 7 Setup */
    RF_BANK_SETUP(ahp->ah_analogBank7Data, &ahp->ah_iniBank7, 1);

    /* Write Analog registers */
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank0, ahp->ah_analogBank0Data, regWrites);
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank1, ahp->ah_analogBank1Data, regWrites);
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank2, ahp->ah_analogBank2Data, regWrites);
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank3, ahp->ah_analogBank3Data, regWrites);
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank6TPC, ahp->ah_analogBank6Data, regWrites);
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank7, ahp->ah_analogBank7Data, regWrites);

    return AH_TRUE;
#undef  RF_BANK_SETUP
}

/*
 * Free memory for analog bank scratch buffers
 */
static void
ar2133Detach(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);

    if (ahp->ah_analogBank0Data != AH_NULL) {
	ath_hal_free(ahp->ah_analogBank0Data);
	ahp->ah_analogBank0Data = AH_NULL;
    }
    if (ahp->ah_analogBank1Data != AH_NULL) {
	ath_hal_free(ahp->ah_analogBank1Data);
	ahp->ah_analogBank1Data = AH_NULL;
    }
    if (ahp->ah_analogBank2Data != AH_NULL) {
	ath_hal_free(ahp->ah_analogBank2Data);
	ahp->ah_analogBank2Data = AH_NULL;
    }
    if (ahp->ah_analogBank3Data != AH_NULL) {
	ath_hal_free(ahp->ah_analogBank3Data);
	ahp->ah_analogBank3Data = AH_NULL;
    }
    if (ahp->ah_analogBank6Data != AH_NULL) {
	ath_hal_free(ahp->ah_analogBank6Data);
	ahp->ah_analogBank6Data = AH_NULL;
    }
    if (ahp->ah_analogBank6TPCData != AH_NULL) {
	ath_hal_free(ahp->ah_analogBank6TPCData);
	ahp->ah_analogBank6TPCData = AH_NULL;
    }
    if (ahp->ah_analogBank7Data != AH_NULL) {
	ath_hal_free(ahp->ah_analogBank7Data);
	ahp->ah_analogBank7Data = AH_NULL;
    }
    if (ahp->ah_addacOwl21 != AH_NULL) {
	ath_hal_free(ahp->ah_addacOwl21);
	ahp->ah_addacOwl21 = AH_NULL;
    }
    if (ahp->ah_bank6Temp != AH_NULL) {
	ath_hal_free(ahp->ah_bank6Temp);
	ahp->ah_bank6Temp = AH_NULL;
    }
}

static HAL_BOOL
ar2133GetChipPowerLimits(struct ath_hal *ah, HAL_CHANNEL *chans, u_int32_t nchans)
{
    HAL_BOOL retVal = AH_TRUE;
    int i;

    for (i=0; i < nchans; i ++) {
        chans[i].maxTxPower = AR5416_MAX_RATE_POWER;
        chans[i].minTxPower = AR5416_MAX_RATE_POWER;
    }
    return (retVal);
}

/*
 * Allocate memory for analog bank scratch buffers
 * Scratch Buffer will be reinitialized every reset so no need to zero now
 */
HAL_BOOL
ar2133RfAttach(struct ath_hal *ah, HAL_STATUS *status)
{
    struct ath_hal_5416 *ahp = AH5416(ah);

    if (!AR_SREV_MERLIN_10_OR_LATER(ah)) {
        /* Allocate analog bank arrays for chips prior to Merlin */
        HALASSERT(ahp->ah_analogBank0Data == AH_NULL);
        ahp->ah_analogBank0Data = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank0.ia_rows);
        HALASSERT(ahp->ah_analogBank1Data == AH_NULL);
        ahp->ah_analogBank1Data = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank1.ia_rows);
        HALASSERT(ahp->ah_analogBank2Data == AH_NULL);
        ahp->ah_analogBank2Data = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank2.ia_rows);
        HALASSERT(ahp->ah_analogBank3Data == AH_NULL);
        ahp->ah_analogBank3Data = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank3.ia_rows);
        HALASSERT(ahp->ah_analogBank6Data == AH_NULL);
        ahp->ah_analogBank6Data = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank6.ia_rows);
        HALASSERT(ahp->ah_analogBank6TPCData == AH_NULL);
        ahp->ah_analogBank6TPCData = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank6TPC.ia_rows);
        HALASSERT(ahp->ah_analogBank7Data == AH_NULL);
        ahp->ah_analogBank7Data = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank7.ia_rows);

        if (ahp->ah_analogBank0Data == AH_NULL || ahp->ah_analogBank1Data == AH_NULL ||
	    ahp->ah_analogBank2Data == AH_NULL || ahp->ah_analogBank3Data == AH_NULL ||
	    ahp->ah_analogBank6Data == AH_NULL || ahp->ah_analogBank6TPCData == AH_NULL ||
	    ahp->ah_analogBank7Data == AH_NULL) {
            HDPRINTF(ah, HAL_DBG_MALLOC, "%s: cannot allocate RF banks\n", __func__);
            *status = HAL_ENOMEM;       /* XXX */
            return AH_FALSE;
        }

        HALASSERT(ahp->ah_addacOwl21 == AH_NULL);
        ahp->ah_addacOwl21 = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniAddac.ia_rows * ahp->ah_iniAddac.ia_columns);
        if (ahp->ah_addacOwl21 == AH_NULL) {
            HDPRINTF(ah, HAL_DBG_MALLOC, "%s: cannot allocate ah_addacOwl21\n", __func__);
            *status = HAL_ENOMEM;       /* XXX */
            return AH_FALSE;
        }

        HALASSERT(ahp->ah_bank6Temp == AH_NULL);
        ahp->ah_bank6Temp = ath_hal_malloc(ah->ah_osdev, sizeof(u_int32_t) * ahp->ah_iniBank6.ia_rows);
        if (ahp->ah_bank6Temp == AH_NULL) {
	    HDPRINTF(ah, HAL_DBG_MALLOC, "%s: cannot allocate ah_bank6Temp\n", __func__);
	    *status = HAL_ENOMEM;       /* XXX */
	    return AH_FALSE;
        }
    }

    ahp->ah_rfHal.rfDetach      = ar2133Detach;
    ahp->ah_rfHal.writeRegs     = ar2133WriteRegs;

    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
        ahp->ah_rfHal.setChannel    = ar9280SetChannel;
    } else {
        ahp->ah_rfHal.setChannel    = ar2133SetChannel; 
    }
    ahp->ah_rfHal.setRfRegs     = ar2133SetRfRegs;

    ahp->ah_rfHal.getChipPowerLim   = ar2133GetChipPowerLimits;
    ahp->ah_rfHal.decreaseChainPower   = ar5416DecreaseChainPower;
    return AH_TRUE;
}

#ifdef ATH_FORCE_BIAS
/*
 * Workaround FOWL orientation sensitivity bug by increasing rf_pwd_icsyndiv.
 * Call from ar2133SetChannel().
 *
 * Theoretical Rules:
 *   if 2 GHz band
 *      if forceBiasAuto
 *         if synth_freq < 2412
 *            bias = 0
 *         else if 2412 <= synth_freq <= 2422
 *            bias = 1
 *         else // synth_freq > 2422
 *            bias = 2
 *      else if forceBias > 0
 *         bias = forceBias & 7
 *      else
 *         no change, use value from ini file
 *   else
 *      no change, invalid band
 *
 *  1st Mod:
 *    2422 also uses value of 2
 *    <approved>
 *
 *  2nd Mod:
 *    Less than 2412 uses value of 0, 2412 and above uses value of 2
 */
static void
ar5416ForceBiasCurrent(struct ath_hal *ah, u_int16_t synth_freq)
{
    u_int32_t               tmpReg;
    int                     regWrites = 0;
    u_int32_t               newBias = 0;
    struct ath_hal_5416     *ahp = AH5416(ah);
    struct ath_hal_private  *ap  = AH_PRIVATE(ah);

    if (synth_freq >= 3000) {
        /* force not valid outside of 2.4 band, return with no change */
        return;
    }
    if (ap->ah_config.ath_hal_forceBiasAuto) {
        if (synth_freq < 2412) {
            newBias = 0;
        } else if (synth_freq < 2422) {
            newBias = 1;
        } else {
            newBias = 2;
        }
    } else if (ap->ah_config.ath_hal_forceBias) {
        newBias = ap->ah_config.ath_hal_forceBias & 7;
    } else {
        /* (!forceBiasAuto && !forceBias): take no action */
        return;
    }
    /* pre-reverse this field */
    tmpReg = ath_hal_reverseBits(newBias, 3);
    /* DEBUG */
    HDPRINTF(ah, HAL_DBG_FORCE_BIAS, 
             "Force rf_pwd_icsyndiv to %1d on %4d (FBA: %1d cfgFB: %1d)\n",
             newBias, synth_freq, ap->ah_config.ath_hal_forceBiasAuto,
             ap->ah_config.ath_hal_forceBias);

    /* swizzle rf_pwd_icsyndiv */
    ar5416ModifyRfBuffer(ahp->ah_analogBank6Data, tmpReg, 3, 181, 3);
#ifdef DEBUG_PRINT_BANK_6
    {
        int ii;
        HDPRINTF(ah, HAL_DBG_FORCE_BIAS, "DUMP BANK 6 Force Bias\n");
        for (ii = 0; ii < 33; ii++) {
            HDPRINTF(ah, HAL_DBG_FORCE_BIAS, " %8.8x\n", ahp->ah_analogBank6Data[ii]);
        }
    }
#endif
    /* write Bank 6 with new params */
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank6, ahp->ah_analogBank6Data, regWrites);
}
#endif /* ATH_FORCE_BIAS */

/**************************************************************
 * ar5416DecreaseChainPower
 *
 * For AR5416-only hardware sets a chain internal RF path to
 * the lowest output power.  Any further writes to bank6 after
 * this setting will override these changes.  Thus this function
 * must be the last function in the sequence to modify bank 6.
 *
 * This function must be called after ar5416ForceBiasCurrent() which is
 * called from ar5416SetChannel5133() which modifies bank 6 after each
 * channel change.  Must also be called after ar5416SetRfRegs() which is
 * called from ar5416ProcessIni() due to swizzling of bank 6.
 * Depends on pDev->pHalInfo->pAnalogBanks being initialized by
 * ar5416SetRfRegs5133().
 *
 * TODO: Additional additive reduction in power - 
 * change chain's switch table so chain's tx state is actually the rx
 * state value. May produce different results in 2GHz/5GHz as well as 
 * board to board but in general should be a reduction.
 * Activated by #ifdef ALTER_SWITCH.  Not tried yet.  If so, must be
 * called after ar5416EepromSetBoardValues() due to RMW of PHY_SWITCH_CHAIN_0
 * 
 */

#define ANTSWAP_AB 0x0001
#define REDUCE_CHAIN_0 0x00000050   /* config bank 5 to alter bank 6 chain 0 only */
#define REDUCE_CHAIN_1 0x00000051   /* config bank 5 to alter bank 6 chain 1 only */

static void
ar5416DecreaseChainPower(struct ath_hal *ah, HAL_CHANNEL *chan)
{
    int                    i, regWrites = 0;
    struct ath_hal_5416    *ahp = AH5416(ah);
    u_int32_t              bank6SelMask;
    u_int32_t              *bank6Temp = ahp->ah_bank6Temp;

    HALASSERT(ahp->ah_analogBank6Data);
    HALASSERT(bank6Temp);

    switch (ahp->ah_diversityControl) {
    case HAL_ANT_FIXED_A:
        /* This means transmit on the first antenna only */
        bank6SelMask = (ahp->ah_antennaSwitchSwap & ANTSWAP_AB) ? 
                       REDUCE_CHAIN_0 :    /* swapped, reduce chain 0 */
                       REDUCE_CHAIN_1;     /* normal, select chain 1/2 to reduce */
        break;
    case HAL_ANT_FIXED_B:
        /* This means transmit on the second antenna only */
        bank6SelMask = (ahp->ah_antennaSwitchSwap & ANTSWAP_AB) ? 
                       REDUCE_CHAIN_1 :    /* swapped, reduce chain 1/2 */
                       REDUCE_CHAIN_0;     /* normal, select chain 0 to reduce */
        break;
    case HAL_ANT_VARIABLE:
        /* This means transmit on all active antennas */
        return; /* do not change anything */
        break;
    default:
        return; /* do not change anything */
        break;
    }

    for (i = 0; i < ahp->ah_iniBank6.ia_rows; i++) {
        bank6Temp[i] = ahp->ah_analogBank6Data[i];
    }


    /* Write Bank 5 to switch Bank 6 write to selected chain only */
     OS_REG_WRITE(ah, AR_PHY_BASE + 0xD8, bank6SelMask);

    /*
     * Modify Bank6 selected chain to use lowest amplification
     * Modifies the parameters to a value of 1.
     * Depends on existing bank 6 values to be cached in pDev->pHalInfo->pAnalogBanks.
     */
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 189, 0); // rf_pwdmix2_int
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 190, 0); // rf_pwdpa2_int
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 191, 0); // rf_pwdmix5_int
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 192, 0); // rf_pwdvga5_int
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 193, 0); // rf_pwdpa5_int
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 222, 0); // rf_pwd4Gtx
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 245, 0); // rf_pwdfilt_int
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 246, 0); // rf_pwdbbmix2_int
    ar5416ModifyRfBuffer(bank6Temp, 1, 1, 247, 0); // rf_pwdbbmix5_int

    /* Write Bank 6 */
    REG_WRITE_RF_ARRAY(&ahp->ah_iniBank6, bank6Temp, regWrites);

    /* Reset Bank 5 to switch Bank 6 write to all chains */
    OS_REG_WRITE(ah, AR_PHY_BASE + 0xD8, 0x00000053);
#ifdef ALTER_SWITCH
    /*
     * Possible additive reduction in power:
     *   change chain 0 switch table so chain 0's tx state is actually the rx
     *   state value.  May produce different results in 2GHz/5GHz as well as
     *   board to board but in general should be a reduction.
     */
    OS_REG_WRITE(ah, PHY_SWITCH_CHAIN_0, (OS_REG_READ(ah, PHY_SWITCH_CHAIN_0) & ~0x38)
        | ((OS_REG_READ(ah, PHY_SWITCH_CHAIN_0) >> 3) & 0x38));
#endif
}
#endif /* AH_SUPPORT_2133 */
