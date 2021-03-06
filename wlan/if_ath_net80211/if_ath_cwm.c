/*
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
 
/*
 * CWM (Channel Width Management) 
 *
 *
 * Synchronization strategy:
 *	A single spinlock (ac_lock) is used to protect access to shared data structures:
 *		- CWM event queue
 *		- CWM state 
 *
 *	An event queue is used to serialize events from 
 *		- DPC context (CST interrupt, channel width notification)
 *		- CWM timer context (extension channel sensing)
 *		- OID/ioctl context  
 *
 *	All 3 contexts (DPC, timer, OID) can run concurrently on SMP machines.
 *	An ownership flag is used to make sure only one thread context
 *      can process the event queue.  No lock is held while processing events.
 */

#include "if_athvar.h"
#include "if_ath_cwm.h"			/* Channel Width Management */

/*
 *----------------------------------------------------------------------
 * local definitions/macros
 *----------------------------------------------------------------------
 */

#define ATH_CWM_MAC_DISABLE_REQUEUE         /* disable requeuing for MAC 40=>20 transition */
#define ATH_CWM_PHY_DISABLE_TRANSITIONS     /* disable PHY transitions */

#define ATH_CWM_TIMER_INTERVAL		1	/* timer interval (seconds) */
#define ATH_CWM_TIMER_EXTCHSENSING	10	/* ext channel sensing enable/disable (seconds) */

#define	ATH_CWM_LOCK_INIT(_ac)    spin_lock_init(&(_ac)->ac_lock)
#define	ATH_CWM_LOCK_DESTROY(_ac)
#define	ATH_CWM_LOCK(_ac)         spin_lock(&(_ac)->ac_lock)
#define	ATH_CWM_UNLOCK(_ac)       spin_unlock(&(_ac)->ac_lock)

/* CWM States */
enum ath_cwm_state {
	ATH_CWM_STATE_EXT_CLEAR,
	ATH_CWM_STATE_EXT_BUSY,
	ATH_CWM_STATE_EXT_UNAVAILABLE,
	ATH_CWM_STATE_MAX
};

/* CWM Events */
enum ath_cwm_event {
	ATH_CWM_EVENT_TXTIMEOUT,	/* tx timeout interrupt */
	ATH_CWM_EVENT_EXTCHCLEAR,	/* ext channel sensing - clear */
	ATH_CWM_EVENT_EXTCHBUSY,	/* ext channel sensing - busy */
	ATH_CWM_EVENT_EXTCHSTOP,	/* ext channel sensing - stop */
	ATH_CWM_EVENT_EXTCHRESUME,	/* ext channel sensing - resume */
	ATH_CWM_EVENT_DESTCW20,		/* dest channel width changed to 20  */
	ATH_CWM_EVENT_DESTCW40,		/* dest channel width changed to 40  */
	ATH_CWM_EVENT_MAX,
};

/* State transition */
struct ath_cwm_statetransition {
	enum ath_cwm_state	ct_newstate;			/* Output: new state */
	void (*ct_action)(struct ath_softc_net80211 *, void *arg);  	/* Output: action */
};

/* event queue entry */
struct ath_cwm_eventq_entry {
    TAILQ_ENTRY(ath_cwm_eventq_entry) ce_entry;

    enum ath_cwm_event 		ce_event;
    void *			ce_arg;
};

/* CWM resources/state */
struct ath_cwm {
	struct ath_timer        ac_timer;		/* CWM timer - monitor the extension channel */
	enum ath_cwm_state	ac_timer_prevstate;	/* CWM timer - prev state of last timer call */
	u_int8_t		ac_timer_statetime; 	/* CWM timer - time (sec) elapsed in current state */
	u_int8_t		ac_vextch; 	    	/* DBG virtual ext channel sensing enabled? */
	u_int8_t		ac_vextchbusy;	    	/* DBG virtual ext channel state */
	u_int32_t		ac_extchbusyper;	/* Last measurement of ext ch busy (percent) */
	spinlock_t          	ac_lock;		/* CWM spinlock */

	/* event queue (used to serialize events from DPC/timer contexts) */
	TAILQ_HEAD(, ath_cwm_eventq_entry) ac_eventq;	/* CWM event queue */
	int32_t			ac_eventq_owner;	/* CWM event queue ownership flag */

	/* State Machine */
	u_int8_t		ac_running;		/* CWM running */
	enum ath_cwm_state	ac_state;		/* CWM state */
	struct ath_cwm_hwstate	ac_hwstate;		/* CWM hardware state */

	/* Debug Information */
};

/*
 *----------------------------------------------------------------------
 * local function declarations
 *----------------------------------------------------------------------
 */
static void cwm_attach(struct ath_softc_net80211 *scn);
static void cwm_inithwstate(struct ieee80211_cwm *icw, struct ath_cwm *acw);
static void cwm_init(struct ath_softc_net80211 *scn);
static void cwm_start(struct ath_softc_net80211 *scn);
static void cwm_stop(struct ath_softc_net80211 *scn);
static void cwm_queueevent(struct ath_softc_net80211 *scn, enum ath_cwm_event event, void *arg);
static void cwm_statetransition(struct ath_softc_net80211 *scn, enum ath_cwm_event event, void *arg);
static int  cwm_timer(void *context);
static int  cwm_extchbusy(struct ath_softc_net80211 *scn);
static int  ath_cwm_ht40allowed(struct ath_softc_net80211 *scn);

static void cwm_action_invalid(struct ath_softc_net80211 *scn, void *arg);
static void cwm_action_mac40to20(struct ath_softc_net80211 *scn, void *arg);
static void cwm_action_mac20to40(struct ath_softc_net80211 *scn, void *arg);
static void cwm_action_phy40to20(struct ath_softc_net80211 *scn, void *arg);
static void cwm_action_phy20to40(struct ath_softc_net80211 *scn, void *arg);
static void cwm_action_requeue(struct ath_softc_net80211 *scn, void *arg);

static void cwm_sendactionmgmt(struct ath_softc_net80211 *scn);
#ifndef ATH_CWM_MAC_DISABLE_REQUEUE
static int  cwm_queryfcc(struct ath_softc_net80211 *scn);
static void cwm_stoptxdma(struct ath_softc_net80211 *scn);
static void cwm_resumetxdma(struct ath_softc_net80211 *scn);
#endif
static void cwm_rate_updatenode(struct ieee80211_node *ni);
static void cwm_rate_updateallnodes(struct ath_softc_net80211 *scn);
static void cwm_debuginfo(struct ath_softc_net80211 *scn);

/*
 *----------------------------------------------------------------------
 * static variables
 *----------------------------------------------------------------------
 */
static const char *ath_cwm_statename[ATH_CWM_STATE_MAX] = {
	"EXT CLEAR",		/* ATH_CWM_STATE_EXT_CLEAR */
	"EXT BUSY",		/* ATH_CWM_STATE_EXT_BUSY */
	"EXT UNAVAIL",		/* ATH_CWM_STATE_EXT_UNAVAILABLE */
};
   
static const char *ath_cwm_eventname[ATH_CWM_EVENT_MAX] = {
	"TXTIMEOUT", 		/* ATH_CWM_EVENT_TXTIMEOUT */
	"EXTCHCLEAR", 		/* ATH_CWM_EVENT_EXTCHCLEAR */
	"EXTCHBUSY", 		/* ATH_CWM_EVENT_EXTCHBUSY */
	"EXTCHSTOP", 		/* ATH_CWM_EVENT_EXTCHSTOP */
	"EXTCHRESUME", 		/* ATH_CWM_EVENT_EXTCHRESUME */
	"DESTCW20", 		/* ATH_CWM_EVENT_DESTCW20 */
	"DESTCW40", 		/* ATH_CWM_EVENT_DESTCW40 */
};

/*
 * State Transition Table 
 *
 * Input  (current state, event)
 * Output (new state, action)
 */
static const struct ath_cwm_statetransition ath_cwm_stt[ATH_CWM_STATE_MAX][ATH_CWM_EVENT_MAX] = {

	/* ATH_CWM_STATE_EXT_CLEAR */
	{
	/* ATH_CWM_EVENT_TXTIMEOUT	==> */ {ATH_CWM_STATE_EXT_BUSY,	 	cwm_action_mac40to20},
	/* ATH_CWM_EVENT_EXTCHCLEAR 	==> */ {ATH_CWM_STATE_EXT_CLEAR, 	NULL},
	/* ATH_CWM_EVENT_EXTCHBUSY 	==> */ {ATH_CWM_STATE_EXT_BUSY,  	cwm_action_mac40to20},
	/* ATH_CWM_EVENT_EXTCHSTOP 	==> */ {ATH_CWM_STATE_EXT_CLEAR, 	cwm_action_invalid},
	/* ATH_CWM_EVENT_EXTCHRESUME 	==> */ {ATH_CWM_STATE_EXT_CLEAR, 	cwm_action_invalid},
	/* ATH_CWM_EVENT_DESTCW20 	==> */ {ATH_CWM_STATE_EXT_CLEAR, 	cwm_action_requeue},
	/* ATH_CWM_EVENT_DESTCW40 	==> */ {ATH_CWM_STATE_EXT_CLEAR, 	NULL},
	},

	/* ATH_CWM_STATE_EXT_BUSY */
	{
	/* ATH_CWM_EVENT_TXTIMEOUT	==> */ {ATH_CWM_STATE_EXT_BUSY, 	NULL},
	/* ATH_CWM_EVENT_EXTCHCLEAR 	==> */ {ATH_CWM_STATE_EXT_CLEAR, 	cwm_action_mac20to40},
	/* ATH_CWM_EVENT_EXTCHBUSY 	==> */ {ATH_CWM_STATE_EXT_BUSY, 	NULL},
#ifdef ATH_CWM_PHY_DISABLE_TRANSITIONS
	/* ATH_CWM_EVENT_EXTCHSTOP  	==> */ {ATH_CWM_STATE_EXT_BUSY, 	NULL},
#else
	/* ATH_CWM_EVENT_EXTCHSTOP  	==> */ {ATH_CWM_STATE_EXT_UNAVAILABLE, 	cwm_action_phy40to20},
#endif    
	/* ATH_CWM_EVENT_EXTCHRESUME 	==> */ {ATH_CWM_STATE_EXT_BUSY, 	cwm_action_invalid},
	/* ATH_CWM_EVENT_DESTCW20 	==> */ {ATH_CWM_STATE_EXT_BUSY, 	NULL},
	/* ATH_CWM_EVENT_DESTCW40 	==> */ {ATH_CWM_STATE_EXT_BUSY, 	NULL},
	},

	/* ATH_CWM_STATE_EXT_UNAVAILABLE */
	{
	/* ATH_CWM_EVENT_TXTIMEOUT	==> */ {ATH_CWM_STATE_EXT_UNAVAILABLE, 	NULL},
	/* ATH_CWM_EVENT_EXTCHCLEAR 	==> */ {ATH_CWM_STATE_EXT_UNAVAILABLE, 	cwm_action_invalid},
	/* ATH_CWM_EVENT_EXTCHBUSY 	==> */ {ATH_CWM_STATE_EXT_UNAVAILABLE, 	cwm_action_invalid},
	/* ATH_CWM_EVENT_EXTCHSTOP 	==> */ {ATH_CWM_STATE_EXT_UNAVAILABLE, 	cwm_action_invalid},
	/* ATH_CWM_EVENT_EXTCHRESUME 	==> */ {ATH_CWM_STATE_EXT_BUSY, 	cwm_action_phy20to40},
	/* ATH_CWM_EVENT_DESTCW20 	==> */ {ATH_CWM_STATE_EXT_UNAVAILABLE, 	NULL},
	/* ATH_CWM_EVENT_DESTCW40 	==> */ {ATH_CWM_STATE_EXT_BUSY, 	cwm_action_phy20to40},
	}
};

/*
 *----------------------------------------------------------------------
 * public functions
 *----------------------------------------------------------------------
 */
/*
 * Device Attach
 */
int 
ath_cwm_attach(struct ath_softc_net80211 *scn, struct ath_reg_parm *ath_conf_parm)
{
	struct ieee80211com     *ic     = &scn->sc_ic;
	struct ieee80211_cwm 	*icw    = &ic->ic_cwm;
	struct ath_cwm          *acw;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);


	acw = (struct ath_cwm *)OS_MALLOC(scn->sc_osdev, 
                                      sizeof(struct ath_cwm),
                                      GFP_KERNEL);
	if (acw == NULL) {
		printk("%s: no memory for cwm attach\n", __func__);
		return ENOMEM;
	}

	memset(acw, 0, sizeof(struct ath_cwm));
	scn->sc_cwm = acw;

	/* ieee80211 Layer - Default Configuration */
	icw->cw_mode 		= IEEE80211_CWM_MODE20;
	icw->cw_extoffset 	= 0;
	icw->cw_extprotmode	= IEEE80211_CWM_EXTPROTNONE;
	icw->cw_extprotspacing 	= IEEE80211_CWM_EXTPROTSPACING25;
	icw->cw_enable      	= ath_conf_parm->cwmEnable;
	icw->cw_extbusythreshold = ATH_CWM_EXTCH_BUSY_THRESHOLD;

	/* Allocate resources */
	ath_initialize_timer(scn->sc_osdev, &acw->ac_timer, ATH_CWM_TIMER_INTERVAL * 1000,
			     cwm_timer, scn);
	ATH_CWM_LOCK_INIT(acw);
	TAILQ_INIT(&acw->ac_eventq);

	/* Initialize state machine */
	cwm_attach(scn);
				       
	return 0;
}

/*
 * Device Detach
 */
void
ath_cwm_detach(struct ath_softc_net80211 *scn)
{
    struct ath_cwm 		*acw	= scn->sc_cwm;
    struct ath_cwm_eventq_entry *eventqentry;

    DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);
	
    if (acw == NULL) {
        printk("%s: error - acw NULL. Possible attach failure\n", __func__);
        return;
    }

    /* Cleanup resources */
    ath_cancel_timer(&acw->ac_timer, CANCEL_NO_SLEEP);
    ATH_CWM_LOCK_DESTROY(acw);

    while (!TAILQ_EMPTY(&acw->ac_eventq)) {
        eventqentry = TAILQ_FIRST(&acw->ac_eventq);
        TAILQ_REMOVE(&acw->ac_eventq, eventqentry, ce_entry);
        OS_FREE(eventqentry);
        eventqentry = NULL;
	}

    /* free CWM memory */
    OS_FREE(acw);
    scn->sc_cwm = NULL;
}

/*
 * Device Init
 */
void
ath_cwm_init(struct ath_softc_net80211 *scn)
{
	struct ath_cwm 		*acw	= scn->sc_cwm;

    DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);
	
    if (acw == NULL) {
        printk("%s: error - acw NULL. Possible attach failure\n", __func__);
        return;
    }

	cwm_init(scn);
}

/*
 * Device Stop
 */
void
ath_cwm_stop(struct ath_softc_net80211 *scn)
{
	struct ath_cwm 		*acw	= scn->sc_cwm;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);
	
    if (acw == NULL) {
		printk("%s: error - acw NULL. Possible attach failure\n", __func__);
		return;
	}

	cwm_stop(scn);
}
/*
 * New State (Virtual AP)
 */
void
ath_cwm_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate)
{
	struct ieee80211com         *ic  = vap->iv_ic;
	struct ath_softc_net80211   *scn = ATH_SOFTC_NET80211(ic);
	struct ath_cwm              *acw = scn->sc_cwm;
	struct ieee80211_cwm        *icw = &ic->ic_cwm;

    if (acw == NULL) {
        printk("%s: error - acw NULL. Possible attach failure\n", __func__);
        return;
    }

	DPRINTF(scn, ATH_DEBUG_CWM, "%s: vap: %s -> %s\n", __func__,
		ieee80211_state_name[vap->iv_state],
		ieee80211_state_name[nstate]);

	switch (nstate) {
	case IEEE80211_S_INIT:		/* default state */
        switch (ieee80211vap_get_opmode(vap)) {
	    case IEEE80211_M_HOSTAP:
		case IEEE80211_M_STA:
#if tbd
			/* VAP support */
			if (ic->ic_nrunning == 0) {
				cwm_stop(scn);
				cwm_init(scn);
			}
#else
			cwm_stop(scn);
			cwm_init(scn);
#endif

			break;
		default:
			break;
		}
		break;
	case IEEE80211_S_SCAN:    	/* scanning */
		switch (ieee80211vap_get_opmode(vap)) {
		case IEEE80211_M_HOSTAP:
		case IEEE80211_M_STA:
			/* 20 MHz mode when scanning */
			acw->ac_hwstate.ht_macmode = HAL_HT_MACMODE_20;
			/* stop CWM state machine */
			cwm_stop(scn);
			break;
		default:
			break;
		}
		break;
	case IEEE80211_S_JOIN:		/* try to join */
		/* channel information is now valid */
		if ((icw->cw_mode == IEEE80211_CWM_MODE2040) ||
		    (icw->cw_mode == IEEE80211_CWM_MODE40)) {
			if (ath_cwm_ht40allowed(scn)) {
				/* set channel width to 40 MHz. 
				 * extension offset is assumed to be set correctly
				 * by config/auto channel select
				 */
				icw->cw_width = IEEE80211_CWM_WIDTH40;
			} else {
				DPRINTF(scn, ATH_DEBUG_CWM, "%s: BSS/Regulatory does not allow 40MHz.\n", __func__);
				icw->cw_width = IEEE80211_CWM_WIDTH20;
				icw->cw_extoffset = 0;
			}
			/* init hardware state */
			cwm_inithwstate(icw, acw);
		}
		break;
	case IEEE80211_S_AUTH:    	/* try to authenticate */
		break;
	case IEEE80211_S_ASSOC:    	/* try to assoc */
		break;
	case IEEE80211_S_RUN:      	/* associated */
		switch (ieee80211vap_get_opmode(vap)) {
		case IEEE80211_M_HOSTAP:
		case IEEE80211_M_STA:
			cwm_start(scn);
			break;
		default:
			break;
		}
		break;
	default:
		DPRINTF(scn, ATH_DEBUG_CWM, "%s: unrecognized state %d\n", __func__, nstate);
		break;
	}
}

/*
 * Scan start notification
 *
 * - leaving home chanel
 * - called before channel change 
 */
void
ath_cwm_scan_start(struct ieee80211com *ic)
{
    struct ath_softc_net80211 *scn = ATH_SOFTC_NET80211(ic);
	struct ieee80211vap *vap;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next) {
		ath_cwm_newstate(vap, IEEE80211_S_SCAN);
	 }
}

/*
 * Scan end notification
 *
 * - returning to home chanel
 * - called before channel change 
 */
void
ath_cwm_scan_end(struct ieee80211com *ic)
{
    struct ath_softc_net80211 *scn = ATH_SOFTC_NET80211(ic);
	struct ieee80211vap *vap;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next) {
		ath_cwm_newstate(vap, IEEE80211_S_JOIN);
	 }
}

/*
 * Radio disable notification
 *
 */
void
ath_cwm_radio_disable(struct ath_softc_net80211 *scn)
{
	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	/* stop CWM state machine and timer */
	cwm_stop(scn);
}

/*
 * Radio enable notification
 *
 */
void
ath_cwm_radio_enable(struct ath_softc_net80211 *scn)
{
	struct ieee80211com *ic = &scn->sc_ic;
	struct ieee80211vap *vap;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	/* set initial state */
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next) {
		ath_cwm_newstate(vap, IEEE80211_S_JOIN);
	 }

	/* restart CWM state machine and timer (if needed) */
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next) {
		ath_cwm_newstate(vap, IEEE80211_S_RUN);
	 }
}


#if tbd // XXX - TODO
/*
 * Atheros layer ioctl.
 *
 * Used to debug CWM state machine by triggering events
 *
 */
int
ath_cwm_ioctl(struct ath_softc_net80211 *scn, int cmd, caddr_t data)
{
	struct ieee80211com 	*ic  = &scn->sc_ic;
	struct ath_cwm 	   	*acw = scn->sc_cwm;
	enum ath_cwm_event	event;
	u_int32_t		value;
	struct ath_cwmdbg 	*dc;
	struct ath_cwminfo 	*ci;
	HAL_HT_CWM 		ht;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);
#if tbd	//XXX - TODO - locking
	ATH_CWM_LOCK(acw);
#endif
    if (acw == NULL) {
		printk("%s: error - acw NULL. Possible attach failure\n", __func__);
		return -EINVAL;
	}

	switch (cmd) {
	case SIOCGATHCWMDBG:
		if (!acw->ac_running) {
			DPRINTF(sc, ATH_DEBUG_CWM, "%s: cwm fsm not running\n", __func__);
			return -EINVAL;
		}
		dc = (struct ath_cwmdbg *) data;
		switch (dc->dc_cmd) {
		case ATH_DBGCWM_CMD_EVENT:
			event = dc->dc_arg;
			if (event < ATH_CWM_EVENT_MAX) {
				DPRINTF(scn, ATH_DEBUG_CWM, "%s: event %s\n", __func__, ath_cwm_eventname[event]);
				cwm_queueevent(scn, event, NULL);
				return 0;
			} else {
				DPRINTF(scn, ATH_DEBUG_CWM, "%s: invalid event %d\n", __func__, event);
				return -EINVAL;
			}
			break;
		case ATH_DBGCWM_CMD_CTL:
			/* Owl 2.0 only */
			value = ath_hal_get11nRxClear(ah);
			if (dc->dc_arg) {
				value |= HAL_RX_CLEAR_CTL_LOW;
			} else {
				value &= ~HAL_RX_CLEAR_CTL_LOW;
			}
			ath_hal_set11nRxClear(ah, value);
			return 0;
			break;
		case ATH_DBGCWM_CMD_EXT:
			/* Owl 2.0 only */
			value = ath_hal_get11nRxClear(ah);
			if (dc->dc_arg) {
				value |= HAL_RX_CLEAR_EXT_LOW;
			} else {
				value &= ~HAL_RX_CLEAR_EXT_LOW;
			}
			ath_hal_set11nRxClear(ah, value);
			return 0;
			break;
		case ATH_DBGCWM_CMD_VEXT:
			acw->ac_vextch = 1;
			acw->ac_vextchbusy = dc->dc_arg;
			return 0;
			break;
		default:
			DPRINTF(scn, ATH_DEBUG_CWM, "%s: invalid SIOCGATHCWMDBG command %d\n", __func__, dc->dc_cmd);
			return -EINVAL;
			break;
		}
		break;
	case SIOCGATHCWMINFO:
		ci = (struct ath_cwminfo *) data;
		ci->ci_chwidth = ic->ic_cwm.cw_width;
		memset(&ht, 0, sizeof(ht));
		ath_cwm_gethwstate(sc, &ht);
		ci->ci_macmode = ht.ht_macmode;
		ci->ci_phymode = ht.ht_phymode;
		ci->ci_extbusyper = acw->ac_extchbusyper;
		return 0;
		break;
	default:
		DPRINTF(scn, ATH_DEBUG_CWM, "%s: invalid command %d\n", __func__, cmd);
		return -EINVAL;
		break;
	}
}
#endif //TBD

void ath_cwm_switch_mode_static20(struct ieee80211com *ic)
{
	struct ath_softc_net80211   *scn    = ATH_SOFTC_NET80211(ic);
	struct ieee80211_cwm 	*icw = &ic->ic_cwm;
        void *arg=NULL;

        /* Disable cwm timer from running to prevent CWM from being active */
        cwm_stop(scn);

        /* Cache channel flags so that we can revert to them if needed */
        scn->cached_ic_flags = 0;

        if (ic->ic_curchan->ic_flags & IEEE80211_CHAN_HT40PLUS)
        	scn->cached_ic_flags |= IEEE80211_CHAN_HT40PLUS;

        if (ic->ic_curchan->ic_flags & IEEE80211_CHAN_HT40MINUS)
        	scn->cached_ic_flags |= IEEE80211_CHAN_HT40MINUS;

        icw->cw_width = IEEE80211_CWM_WIDTH20;
	icw->cw_mode = IEEE80211_CWM_MODE20;
        cwm_action_phy40to20(scn,arg);
        cwm_action_mac40to20(scn,arg);
}

void ath_cwm_switch_mode_dynamic2040(struct ieee80211com *ic)
{
	struct ath_softc_net80211   *scn    = ATH_SOFTC_NET80211(ic);
        void *arg=NULL;
	struct ieee80211_cwm 	*icw = &ic->ic_cwm;
	struct ath_cwm 	   	*acw = scn->sc_cwm;

        icw->cw_width = IEEE80211_CWM_WIDTH40;
        icw->cw_mode = IEEE80211_CWM_MODE2040;
        cwm_action_phy20to40(scn,arg);
        cwm_action_mac20to40(scn,arg);
        /* Re-arm cwm timer */
	if (!acw->ac_running) {
            cwm_start(scn);
        }
}

void
ath_chwidth_change(struct ieee80211_node *ni)
{
    ath_cwm_newchwidth(ni);
}

/*
 * Node has changed channel width 
 *
 */
void
ath_cwm_newchwidth(struct ieee80211_node *ni)
{
	struct ieee80211com         *ic     = ni->ni_ic;
	struct ath_softc_net80211   *scn    = ATH_SOFTC_NET80211(ic);
	struct ath_cwm              *acw    = scn->sc_cwm;
	enum ath_cwm_event          event;
	
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: chwidth = %d\n", __func__, ni->ni_chwidth);

    if (acw == NULL) {
		printk("%s: error - acw NULL. Possible attach failure\n", __func__);
		return;
	}

	if (!acw->ac_running) {
	      return;
	}

	event = (ni->ni_chwidth == IEEE80211_CWM_WIDTH40) ?
        ATH_CWM_EVENT_DESTCW40 : ATH_CWM_EVENT_DESTCW20;
	cwm_queueevent(scn, event, ni);

	/* update node's rate table */
	cwm_rate_updatenode(ni); 
}

 /*
  * get ext channel busy (percentage) 
  */
 u_int32_t
 ath_cwm_getextbusy(struct ath_softc_net80211 *scn)
 {
	struct ath_cwm 	*acw = scn->sc_cwm;
 
	if (acw == NULL) {
		printk("%s: error - acw NULL. Possible attach failure\n", __func__);
		return 0;
	}
 
	return acw->ac_extchbusyper;
 }

/*
 * Tx timeout interrupt 
 *
 */
void
ath_cwm_txtimeout(struct ath_softc_net80211 *scn)
{
	struct ath_cwm 		*acw	= scn->sc_cwm;
	
	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

    if (acw == NULL) {
		printk("%s: error - acw NULL. Possible attach failure\n", __func__);
		return;
	}

	if (!acw->ac_running) {
	      return;
	}

	cwm_queueevent(scn, ATH_CWM_EVENT_TXTIMEOUT, NULL);
}

/*
 * get hw state 
 *
 */
void
ath_cwm_gethwstate(struct ath_softc_net80211 *scn, struct ath_cwm_hwstate *cwm_hwstate)
{
	struct ath_cwm 		*acw	= scn->sc_cwm;

    if (acw == NULL) {
		printk("%s: error - acw NULL. Possible attach failure\n", __func__);
		return;
	}

	ATH_CWM_LOCK(acw);

	*cwm_hwstate = acw->ac_hwstate;

	ATH_CWM_UNLOCK(acw);
}

/*
 * get local channel width 
 */
enum ieee80211_cwm_width
ath_cwm_getlocalchwidth(struct ieee80211com *ic)
{
	struct ieee80211_cwm 	*icw;
    
	icw	= &ic->ic_cwm;
	return icw->cw_width;
}

HAL_HT_MACMODE
ath_net80211_cwm_macmode(ieee80211_handle_t ieee)
{
    struct ieee80211com         *ic  = NET80211_HANDLE(ieee);
    struct ath_softc_net80211   *scn = ATH_SOFTC_NET80211(ic);
	struct ath_cwm              *acw = scn->sc_cwm;
    
    /* Check if valid CWM HW state exists */
    if (acw) {
        return (acw->ac_hwstate.ht_macmode);
    } else {
        return HAL_HT_MACMODE_20;
    }   
}


/*
 *----------------------------------------------------------------------
 * local functions
 *----------------------------------------------------------------------
 */

/*
 * Acquire event queue ownership
 */
static INLINE u_int8_t
ATH_CWM_ACQUIRE_EVENT_QUEUE_OWNERSHIP(struct ath_cwm *ac) 
{
	return (cmpxchg(&ac->ac_eventq_owner, 0, 1) == 0);
}

/*
 * Release event queue ownership
 */
static INLINE int32_t
ATH_CWM_RELEASE_EVENT_QUEUE_OWNERSHIP(struct ath_cwm *ac) 
{
	return (cmpxchg(&ac->ac_eventq_owner, 1, 0));
}

/*
 * CWM Attach
 *
 * - initialize state machine according to default settings
 * - called by ath_attach() 
 *
 */
static void
cwm_attach(struct ath_softc_net80211 *scn)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
	struct ieee80211_cwm 	*icw	= &ic->ic_cwm;
	struct ath_cwm 		*acw	= scn->sc_cwm;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	/* ieee80211 layer initialization */
	icw->cw_width = IEEE80211_CWM_WIDTH20;

	/* Atheros layer initialization */
	acw->ac_running 	= 0;
	acw->ac_timer_statetime = 0;
	acw->ac_timer_prevstate = ATH_CWM_STATE_EXT_CLEAR;
	acw->ac_vextch 		= 0;
	acw->ac_vextchbusy 	= 0;
	acw->ac_extchbusyper 	= 0;

	/* default state */
	acw->ac_state  		= ATH_CWM_STATE_EXT_CLEAR;
	cwm_inithwstate(icw, acw);
}

/*
 * CWM init hardware state based on current configuration
 *
 */
static void 
cwm_inithwstate(struct ieee80211_cwm *icw, struct ath_cwm *acw)
{
	ATH_CWM_LOCK(acw);

	/* mac and phy mode */
	switch (icw->cw_width) {
	case IEEE80211_CWM_WIDTH40:
		acw->ac_hwstate.ht_macmode = HAL_HT_MACMODE_2040;
		break;

	case IEEE80211_CWM_WIDTH20:
	default:
		acw->ac_hwstate.ht_macmode = HAL_HT_MACMODE_20;
		break;
	}

	/* extension channel protection spacing */				   
	switch (icw->cw_extprotspacing) {
	case IEEE80211_CWM_EXTPROTSPACING20:
		acw->ac_hwstate.ht_extprotspacing = HAL_HT_EXTPROTSPACING_20;
		break;
	case IEEE80211_CWM_EXTPROTSPACING25:
	default:
		acw->ac_hwstate.ht_extprotspacing = HAL_HT_EXTPROTSPACING_25;
		break;
	}

	ATH_CWM_UNLOCK(acw);
}

/*
 * CWM Init
 *
 * - initialize state machine based on current configuration
 * - called by ath_init()
 *
 */
static void
cwm_init(struct ath_softc_net80211 *scn)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
	struct ieee80211_cwm 	*icw	= &ic->ic_cwm;
	struct ath_cwm 		*acw	= scn->sc_cwm;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	ATH_CWM_LOCK(acw);

 	/* Validate configuration */
	switch (icw->cw_mode) {
	case IEEE80211_CWM_MODE20:
		if (icw->cw_extoffset != 0) {
			DPRINTF(scn, ATH_DEBUG_CWM, "%s: Invalid configuration. Forcing extoffset to 0\n", __func__);
			icw->cw_extoffset = 0;
		}
		icw->cw_width = IEEE80211_CWM_WIDTH20;
                icw->cw_enable = 0;
		/* device not capable of 40 MHz */
		ic->ic_htcap &= ~IEEE80211_HTCAP_C_CHWIDTH40;
		break;
	case IEEE80211_CWM_MODE40:
		if (icw->cw_extoffset == 0) {
			DPRINTF(scn, ATH_DEBUG_CWM, "%s: Invalid configuration. Forcing extoffset to 1\n", __func__);
			icw->cw_extoffset = 1;
		}
		icw->cw_width = IEEE80211_CWM_WIDTH40;
		icw->cw_enable = 0;
		break;
	case IEEE80211_CWM_MODE2040:
		if (icw->cw_extoffset == 0) {
			DPRINTF(scn, ATH_DEBUG_CWM, "%s: Invalid configuration. Forcing extoffset to 1\n", __func__);
			icw->cw_extoffset = 1;
		}
		icw->cw_width = IEEE80211_CWM_WIDTH40;
		break;
	default:
		DPRINTF(scn, ATH_DEBUG_CWM, "%s: Invalid cwm mode. Forcing to 20MHz only\n", __func__);
		icw->cw_mode = IEEE80211_CWM_MODE20;
		icw->cw_extoffset = 0;
		icw->cw_width = IEEE80211_CWM_WIDTH20;
		icw->cw_enable = 0;
		break;
	}

	/* Display configuration */
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: cw_mode %d\n", __func__, icw->cw_mode);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: cw_extoffset %d\n", __func__, icw->cw_extoffset);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: cw_extprotmode %d\n", __func__, icw->cw_extprotmode);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: cw_extprotspacing %d\n", __func__, icw->cw_extprotspacing);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: cw_enable %d\n", __func__, icw->cw_enable);

	ATH_CWM_UNLOCK(acw);

}

/*
 * CWM Start
 */
static void
cwm_start(struct ath_softc_net80211 *scn)
{
	struct ieee80211com 	*ic	= &scn->sc_ic;
	struct ieee80211_cwm 	*icw	= &ic->ic_cwm;
	struct ath_cwm 		*acw	= scn->sc_cwm;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	if (acw->ac_running) {
		return;
	}

	if (!icw->cw_enable) {
		DPRINTF(scn, ATH_DEBUG_CWM, "%s: CWM state machine disabled via configuration\n", __func__);
		return;
	}

	if(!ath_cwm_ht40allowed(scn)) {
		DPRINTF(scn, ATH_DEBUG_CWM, "%s: CWM state machine disabled because of BSS/regulatory restrictions\n", __func__);
		return;
	}

	ATH_CWM_LOCK(acw);

    /* Start CWM */
	acw->ac_running = 1;

	/* Initial state */
	if (icw->cw_width == IEEE80211_CWM_WIDTH40) {
		acw->ac_state = ATH_CWM_STATE_EXT_CLEAR;
	} else {
		acw->ac_state = ATH_CWM_STATE_EXT_BUSY;
	}

	cwm_debuginfo(scn);

	ATH_CWM_UNLOCK(acw);

	ath_start_timer(&acw->ac_timer);
}


/*
 * CWM Stop
 */
static void
cwm_stop(struct ath_softc_net80211 *scn)
{
	struct ath_cwm 		*acw	= scn->sc_cwm;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	if (acw->ac_running) {
		acw->ac_running = 0;
		ath_cancel_timer(&acw->ac_timer, CANCEL_NO_SLEEP);
	}
}

/*
 * CWM queue event
 *
 * - queue and process events. This will serialize all CWM events without
 *   holding a spinlock.
 * - function may called from DPC/timer/passive contexts
 *
 */
static void
cwm_queueevent(struct ath_softc_net80211 *scn, enum ath_cwm_event event, void *arg)
{
	struct ath_cwm 			*acw	= scn->sc_cwm;
	struct ath_cwm_eventq_entry 	*eventqentry;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s: %s\n", __func__, ath_cwm_eventname[event]);

	/* Allocate event queue entry */
	eventqentry = (struct ath_cwm_eventq_entry *)
			OS_MALLOC(scn->sc_osdev, sizeof(struct ath_cwm_eventq_entry), GFP_ATOMIC);
	if (eventqentry == NULL) {
		return;
	}
	eventqentry->ce_event = event;
	eventqentry->ce_arg   = arg;

	/* Queue event */
	ATH_CWM_LOCK(acw);
	TAILQ_INSERT_TAIL(&acw->ac_eventq, eventqentry, ce_entry);
	ATH_CWM_UNLOCK(acw);

	/* Try to claim event queue ownership */
	if (ATH_CWM_ACQUIRE_EVENT_QUEUE_OWNERSHIP(acw)) {

		/* 
		 * We own the event queue => process all events. 
		 * This enforces serialization of events 
		 */
		while (!TAILQ_EMPTY(&acw->ac_eventq)) {

			/* Get event queue entry */
			ATH_CWM_LOCK(acw);
			eventqentry = TAILQ_FIRST(&acw->ac_eventq);
			TAILQ_REMOVE(&acw->ac_eventq, eventqentry, ce_entry);
			ATH_CWM_UNLOCK(acw);

			/* Process event queue entry (lock is _NOT_ held) */
			cwm_statetransition(scn, eventqentry->ce_event , eventqentry->ce_arg);

			/* Free event queue entry */
			OS_FREE(eventqentry);
			eventqentry = NULL;
		}

		(void)ATH_CWM_RELEASE_EVENT_QUEUE_OWNERSHIP(acw);
	}
}

/*
 * CWM State Transition
 *
 * Assumptions: serialized by caller
 * 
 */
static void
cwm_statetransition(struct ath_softc_net80211 *scn, enum ath_cwm_event event, void *arg)
{
	struct ath_cwm 				*acw = scn->sc_cwm;
	struct ieee80211com 			*ic = &scn->sc_ic;
	const struct ath_cwm_statetransition 	*st;
	static const struct ath_cwm_statetransition apst = {ATH_CWM_STATE_EXT_UNAVAILABLE,
						            NULL}; /* special case for AP */

	KASSERT(acw->ac_running, ("%s: event received but cwm fsm not running\n", __func__));
	if(!acw->ac_running) {
		return;
	}

        KASSERT(event < ATH_CWM_EVENT_MAX, ("%s: invalid event %d\n", __func__, event));

	/* Input:  current state, event 
	 * Output: new state, action    
	 */
	st =  &ath_cwm_stt[acw->ac_state][event];

	/* A little hacky, but avoids having a separate state transition table for the AP */
	if ((ic->ic_opmode == IEEE80211_M_HOSTAP) && (acw->ac_state == ATH_CWM_STATE_EXT_UNAVAILABLE) 
	    && (event == ATH_CWM_EVENT_DESTCW40)) {
		st = &apst;
	}

	DPRINTF(scn, ATH_DEBUG_CWM, "%s: Event %s. State %s => %s\n", __func__,
	       ath_cwm_eventname[event], ath_cwm_statename[acw->ac_state], ath_cwm_statename[st->ct_newstate]);

	/* update state */
	acw->ac_state = st->ct_newstate;

	/* associated action (if any) */
	if (st->ct_action != NULL) {
		st->ct_action(scn, arg);
	} else {
		DPRINTF(scn, ATH_DEBUG_CWM, "%s: no associated action\n", __func__);
	}
}



/*
 * CWM Timer
 * 
 * - monitor the extension channel 
 * - generate CWM events based on extension channel activity
 *
 * Return 0 to reschedule timer
 */
static int
cwm_timer(void *context)
{
	struct ath_softc_net80211   *scn	= (struct ath_softc_net80211 *)context;
	struct ath_cwm 		*acw	= scn->sc_cwm;
	int			extchbusy = 0, persistentstate = 0;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

	if(!acw->ac_running) {
		return 1;
	}

	/* monitor extension channel */
	if (acw->ac_state != ATH_CWM_STATE_EXT_UNAVAILABLE) {
		extchbusy = cwm_extchbusy(scn);
	}

	/* check for same state for a period of time */
	if (acw->ac_state == acw->ac_timer_prevstate) {
		acw->ac_timer_statetime++;
		if (acw->ac_timer_statetime == ATH_CWM_TIMER_EXTCHSENSING) {
			persistentstate = 1;
		}
	} else {
		/* reset counter */
		acw->ac_timer_statetime = 0;
		acw->ac_timer_prevstate = acw->ac_state;
	}

        switch (acw->ac_state) {
	case ATH_CWM_STATE_EXT_CLEAR:
		if (extchbusy) {
			cwm_queueevent(scn, ATH_CWM_EVENT_EXTCHBUSY, NULL);
		}
		break;
	case ATH_CWM_STATE_EXT_BUSY:
		if (extchbusy) {
			if (persistentstate) {
				cwm_queueevent(scn, ATH_CWM_EVENT_EXTCHSTOP, NULL);
			}
		} else {
			cwm_queueevent(scn, ATH_CWM_EVENT_EXTCHCLEAR, NULL);
		}
		break;
	case ATH_CWM_STATE_EXT_UNAVAILABLE:
		if (persistentstate) {
			cwm_queueevent(scn, ATH_CWM_EVENT_EXTCHRESUME, NULL);
		}
		break;
	default:
		DPRINTF(scn, ATH_DEBUG_CWM, "%s: invalid state, %d\n", __func__, acw->ac_state);
		break;

	}

	return 0;
}

/*
 * CWM Extension Channel Busy Check
 * 
 * - return 	1 if extension channel busy,
 *		0 otherwise
 */
static int
cwm_extchbusy(struct ath_softc_net80211 *scn)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
	struct ieee80211_cwm 	*icw	= &ic->ic_cwm;
	struct ath_cwm 		*acw	= scn->sc_cwm;
	int 			busy 	= 0;

	/* debugging - virtual extension channel sensing */
	if (acw->ac_vextch) {
		return acw->ac_vextchbusy;
	}

	/* Extension Channel busy (0-100%) */
	acw->ac_extchbusyper = scn->sc_ops->get_extbusyper(scn->sc_dev);
	if (acw->ac_extchbusyper > icw->cw_extbusythreshold) {
		busy = 1;
	}

	return busy;
}

/*
 * Actions
 *
 * Note: All actions are serialized by caller
 *
 */

/*
 * Action: Invalid state transition
 */
static void
cwm_action_invalid(struct ath_softc_net80211 *scn, void *arg)
{
	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);
}

void
ath_cwm_switch_to20(struct ieee80211com *ic)
{
    struct ath_softc_net80211 *scn = ATH_SOFTC_NET80211(ic);
    DPRINTF(scn, ATH_DEBUG_CWM, "%s: Switching MAC from 40 to 20\n", __func__);
    cwm_stop(scn); /* stop the cwm timer */
    cwm_action_mac40to20(scn, (void *)NULL);
}

void ath_cwm_switch_to40(struct ieee80211com *ic)
{
    struct ath_softc_net80211 *scn = ATH_SOFTC_NET80211(ic);
    DPRINTF(scn, ATH_DEBUG_CWM, "%s: Switching MAC from 20 to 20/40\n", __func__);
    cwm_action_mac20to40(scn, (void *)NULL);
    cwm_start(scn); /* start the cwm timer */
}

/*
 * Action: switch MAC from 40 to 20
 */
static void
cwm_action_mac40to20(struct ath_softc_net80211 *scn, void *arg)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
	struct ieee80211_cwm 	*icw	= &ic->ic_cwm;
	struct ath_cwm 		*acw	= scn->sc_cwm;
#ifndef ATH_CWM_MAC_DISABLE_REQUEUE
	int			ac;
	struct ath_txq      	*txq;
#endif

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

#if tbd //XXX - TODO - stats
	scn->sc_stats.ast_cwm_mac++;
#endif

#ifdef ATH_CWM_MAC_DISABLE_REQUEUE
	/* set channel width */
	icw->cw_width = IEEE80211_CWM_WIDTH20;

	/* set MAC to 20 MHz */
	acw->ac_hwstate.ht_macmode = HAL_HT_MACMODE_20;
    scn->sc_ops->set_macmode(scn->sc_dev, HAL_HT_MACMODE_20);

	/* notify rate control of new mode (select new rate table) */
	cwm_rate_updateallnodes(scn);
#else
	/* stop MAC */
	cwm_stoptxdma(scn);

	/*
	 * first complete all packets in h/w queue
	 * mark incomplete packets as sw filtered
	 */
	for (ac = WME_AC_BE; ac <= WME_AC_VO; ac++) {
		txq = sc->sc_ac2q[ac];
		owl_tx_processq(sc, txq, OWL_TXQ_FILTERED);
	}

	/* set channel width */
	icw->cw_width = IEEE80211_CWM_WIDTH20;

	/* set MAC to 20 MHz */
	acw->ac_hwstate.ht_macmode = HAL_HT_MACMODE_20;
	ath_hal_set11nmac2040(sc->sc_ah, HAL_HT_MACMODE_20);

	/* notify rate control of new mode (select new rate table) */
	cwm_rate_updateallnodes(scn);

	/* 
	 * Re-queue/re-aggregate packets as 20 MHz 
	 */
	for (ac = WME_AC_BE; ac <= WME_AC_VO; ac++) {
		txq = sc->sc_ac2q[ac];
		owl_tx_requeue(sc, txq);
	}

	/* Resume MAC */
	cwm_resumetxdma(sc);
#endif //ATH_CWM_MAC_DISABLE_REQUEUE

	/* all virtual APs - send ch width action management frame */
	cwm_sendactionmgmt(scn);
   
}

/*
 * Action: switch MAC from 20 to 40
 */
static void
cwm_action_mac20to40(struct ath_softc_net80211 *scn, void *arg)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
	struct ieee80211_cwm 	*icw	= &ic->ic_cwm;
	struct ath_cwm 		*acw	= scn->sc_cwm;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

#if tbd //XXX - TODO - stats
	scn->sc_stats.ast_cwm_mac++;
#endif

	/* No need to requeue existing frames on hardware queue
	 * This avoids stopping the hardware queue and re-queuing 
	 */

	/* set channel width */
	icw->cw_width = IEEE80211_CWM_WIDTH40;

	/* set MAC to 40 MHz */
	acw->ac_hwstate.ht_macmode = HAL_HT_MACMODE_2040;
        scn->sc_ops->set_macmode(scn->sc_dev, HAL_HT_MACMODE_2040);

	/* notify rate control of new mode (select new rate table) */
	cwm_rate_updateallnodes(scn);

	/* all virtual APs - send ch width action management frame */
	cwm_sendactionmgmt(scn);
}

/*
 * Action: switch PHY from 40 to 20 
 */
static void
cwm_action_phy40to20(struct ath_softc_net80211 *scn, void *arg)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
        u_int32_t newflags=0;
        struct ieee80211_channel *newchan=NULL;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);

#if tbd //XXX - TODO - stats
	scn->sc_stats.ast_cwm_phy++;
#endif

	/* PHY 20 MHz */

        /* Fix for bug 32813 */
        newflags = ic->ic_curchan->ic_flags;

        newflags &= ~(IEEE80211_CHAN_HT40PLUS | IEEE80211_CHAN_HT40MINUS);
        newflags |= IEEE80211_CHAN_HT20;

        newchan = ieee80211_find_channel(ic, ic->ic_curchan->ic_freq, 
                  newflags);

        if (newchan)
            ath_net80211_change_channel(ic, newchan);

	/* all virtual APs - re-send ch width action management frame */
	cwm_sendactionmgmt(scn);

}

/*
 * Action: switch PHY from 20 to 40 
 */
static void
cwm_action_phy20to40(struct ath_softc_net80211 *scn, void *arg)
{
#if tbd
	struct ieee80211_cwm 	*icw = &ic->ic_cwm;
        struct ath_cwm 		*acw	= scn->sc_cwm;
#endif
	struct ieee80211com 	*ic 	= &scn->sc_ic;

        u_int32_t newflags=0;
        struct ieee80211_channel *newchan=NULL;

#if tbd //XXX - TODO - stats
	scn->sc_stats.ast_cwm_phy++;
#endif

	/* PHY 20/40 MHz */

        /* Fix for bug 32813 */
        newflags = ic->ic_curchan->ic_flags;
        newflags &= ~IEEE80211_CHAN_HT20;
        newflags |= scn->cached_ic_flags;

	/* all virtual APs - re-send ch width action management frame */
	cwm_sendactionmgmt(scn);

        newchan = ieee80211_find_channel(ic, ic->ic_curchan->ic_freq, 
                  newflags);

        if (newchan)
            ath_net80211_change_channel(ic, newchan);
}
       
/*
 * Action: filter destination and requeue
 */
static void
cwm_action_requeue(struct ath_softc_net80211 *scn, void *arg)
{
	struct ieee80211_node 	*ni = (struct ieee80211_node *) arg;
#ifndef ATH_CWM_MAC_DISABLE_REQUEUE
	int 		      	ac;
	struct ath_txq      	*txq;
	int			requeue[WME_AC_VO + 1];
#endif

	DPRINTF(scn, ATH_DEBUG_CWM, "%s\n", __func__);
	KASSERT(ni != NULL, ("%s: error node is null\n", __func__));

#if tbd //XXX - TODO - stats
	scn->sc_stats.ast_cwm_requeue++;
#endif

	/* destination node channel width changed
	 * requeue tx frames with updated node's channel width setting
	 */

#ifdef ATH_CWM_MAC_DISABLE_REQUEUE
	/* update node's rate table */
	cwm_rate_updatenode(ni);
#else
	/* requeueing only needed if destination node has frames
	 * on _any_ hardware queue 
	 */
	if (ATH_NODE_HWQ_ACTIVE(ni)) {
		/* stop hw tx */
		cwm_stoptxdma(sc);

		/*
		 * first complete all packets in h/w queue
		 * mark incomplete packets as sw filtered
		 */
		for (ac = WME_AC_BE; ac <= WME_AC_VO; ac++) {
			/* check if destination node has frames on _this_ hardware queue */
			if (ATH_NODE_AC_ACTIVE(ni, ac)) {
				txq = sc->sc_ac2q[ac];
				owl_tx_processq(sc, txq, OWL_TXQ_FILTERED);
				requeue[ac] = 1;
			} else {
				requeue[ac] = 0;
			}
		}

		/* update node's rate table */
		cwm_rate_updatenode(ni);

		/* requeue frames */
		for (ac = WME_AC_BE; ac <= WME_AC_VO; ac++) {
			/* check if destination node has frames on _this_ hardware queue */
			if (requeue[ac]) {
				txq = sc->sc_ac2q[ac];
				owl_tx_requeue(sc, txq);
			}
		}
		/* restart hw tx */
		cwm_resumetxdma(sc);
	} else {
		/* update node's rate table */
		cwm_rate_updatenode(ni);
	}
#endif  //ATH_CWM_MAC_DISABLE_REQUEUE
}


/*
 * Helper functions
 */

/*
 * HT40 allowed?
 * - check device
 * - check regulatory (channel flags)
 */
static int
ath_cwm_ht40allowed(struct ath_softc_net80211 *scn) 
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
    struct ieee80211_channel *chan  = (ic->ic_bsschan != IEEE80211_CHAN_ANYC)?
                                      ic->ic_bsschan : ic->ic_curchan;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s: IC channel: chfreq %d, chflags 0x%X\n", __func__, 
		chan->ic_freq, chan->ic_flags);

	if ((ic->ic_htcap & IEEE80211_HTCAP_C_CHWIDTH40) &&
	    IEEE80211_IS_CHAN_11N_HT40(chan)) {
		return 1;
	} else {
		return 0;
	}   
}

/*
 * Send ch width action management frame 
 */
static void
cwm_sendactionmgmt(struct ath_softc_net80211 *scn)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
	struct ieee80211vap 	*vap;
	struct ieee80211_node   *ni = NULL;
	struct ieee80211_action_mgt_args actionargs;

       /* all virtual APs - send ch width action management frame */
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next) {
		if (ieee80211vap_get_opmode(vap) == IEEE80211_M_HOSTAP) {
			/* create temporary node for broadcast */
			ni = ieee80211_dup_bss(vap, IEEE80211_GET_BCAST_ADDR(ic));
		} else {
			ni = vap->iv_bss;
		}

		/* send channel width action frame */
		if (ni != NULL) {
			actionargs.category	= IEEE80211_ACTION_CAT_HT;
			actionargs.action	= IEEE80211_ACTION_HT_TXCHWIDTH;
			actionargs.arg1		= 0;
			actionargs.arg2		= 0;
			actionargs.arg3		= 0;
			ieee80211_send_action(ni, &actionargs);

		        if (ieee80211vap_get_opmode(vap) == IEEE80211_M_HOSTAP) {
				/* temporary node - decrement reference count so that the node will be 
				 * automatically freed upon completion */
				ieee80211_free_node(ni);
			}
		}
	 }
}

#ifndef ATH_CWM_MAC_DISABLE_REQUEUE
/*
 * Fast Channel Change Allowed?
 */
 static int cwm_queryfcc(struct ath_softc_net80211 *scn)
 {
	struct ieee80211com 	*ic 	= &scn->sc_ic;

	if (ic->ic_flags_ext & IEEE80211_FAST_CC) {
		return 1;
	} else {
		return 0;
	}
 }

/*
 * Stop tx DMA (fast).  Does not reset or channel change
 */
static void
cwm_stoptxdma(struct ath_softc *scn)
{
	struct ath_hal 		*ah 	= scn->sc_ah;
        
	/* disable interrupts */
	ath_hal_intrset(ah, 0);     	

        if (cwm_queryfcc(sc)) {
		/* fast tx abort */
		if (!ath_hal_aborttxdma(sc->sc_ah)) {
			printk("%s: unable to abort tx dma\n", __func__);
		}
	} else {
		int i;
		for (i = 0; i < HAL_NUM_TX_QUEUES; i++)
		    if (ATH_TXQ_SETUP(sc, i))
			ath_hal_stoptxdma(ah, sc->sc_txq[i].axq_qnum);
	}
}

/*
 * Resume tx DMA 
 */
static void
cwm_resumetxdma(struct ath_softc *sc)
{
	struct ath_hal 		*ah 	= sc->sc_ah;

        /* Re-enable interrupts */
        ath_hal_intrset(ah, sc->sc_imask);
}
#endif //#ifndef ATH_CWM_MAC_DISABLE_REQUEUE

/*
 * Update node's rate table 
 */
static void
cwm_rate_updatenode(struct ieee80211_node *ni) 
{
	struct ieee80211com         *ic = ni->ni_ic;
	struct ath_softc_net80211   *scn = ATH_SOFTC_NET80211(ic);
    struct ieee80211_key        *k = NULL;
    struct ieee80211vap         *vap = ni->ni_vap;
	u_int32_t                   capflag = 0;
        
	if ((ni->ni_chwidth == IEEE80211_CWM_WIDTH40) && 
		(ic->ic_cwm.cw_width == IEEE80211_CWM_WIDTH40))
	{
		capflag |=  ATH_RC_CW40_FLAG;
	}
	if (ni->ni_htcap & IEEE80211_HTCAP_C_SHORTGI40) {
		capflag |=  ATH_RC_SGI_FLAG;
	}
	if (ni->ni_flags & IEEE80211_NODE_HT) {
        u_int32_t tx_chainmask;
        capflag |= ATH_RC_HT_FLAG;
        scn->sc_ops->ath_get_config_param(scn->sc_dev, ATH_PARAM_TXCHAINMASK, &tx_chainmask);
        if (tx_chainmask != 1 && (ni->ni_ath_flags & IEEE80211_NODE_HT_DS) &&
            !(vap->iv_flags_ext & IEEE80211_FEXT_SMPS) &&
            (scn->sc_ops->has_capability)(scn->sc_dev, ATH_CAP_DS))
            capflag |= ATH_RC_DS_FLAG;
	}

    if (ni->ni_ucastkey.wk_cipher != &ieee80211_cipher_none) {
        k = &ni->ni_ucastkey;
    } else if (vap->iv_def_txkey != IEEE80211_KEYIX_NONE) {
        k = &vap->iv_nw_keys[vap->iv_def_txkey];
    }
                                   
    if (k && (k->wk_cipher->ic_cipher == IEEE80211_CIPHER_WEP || 
        k->wk_cipher->ic_cipher == IEEE80211_CIPHER_TKIP)) {
        capflag |= ATH_RC_WEP_TKIP_FLAG;
        ni->ni_flags |= IEEE80211_NODE_NOAMPDU;
    }

	/* Rx STBC is a 2-bit mask. Needs to convert from ieee definition to ath definition. */ 
	capflag |= (((ni->ni_htcap & IEEE80211_HTCAP_C_RXSTBC) >> IEEE80211_HTCAP_C_RXSTBC_S)
				<< ATH_RC_RX_STBC_FLAG_S);

    scn->sc_ops->ath_rate_newassoc(scn->sc_dev, ATH_NODE_NET80211(ni)->an_sta, 1, capflag,
                                   &ni->ni_rates, &ni->ni_htrates);
}

/*
 * Update all associated nodes and VAPs
 *
 * Called when local channel width changed.  e.g. if AP mode,
 * update all associated STAs when the AP's channel width changes.
 */
static void
cwm_rate_updateallnodes(struct ath_softc_net80211 *scn)
{
	struct ieee80211com 	*ic 	= &scn->sc_ic;
	struct ieee80211vap 	*vap;
	struct ath_vap_net80211 *avn;

       /* all virtual APs - update destination nodes */
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next) {
		avn = ATH_VAP_NET80211(vap);
		scn->sc_ops->ath_rate_newstate(scn->sc_dev,
						avn->av_if_id, 1);
	 }
}

/*
 * Debug - Display CWM information 
 */

static void
cwm_debuginfo(struct ath_softc_net80211 *scn)
{
	struct ath_cwm 		*acw	= scn->sc_cwm;

	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_running %d\n",		__func__, acw->ac_running);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_state %s\n", 		__func__, ath_cwm_statename[acw->ac_state]);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_hwstate.ht_macmode %d\n",	__func__, acw->ac_hwstate.ht_macmode);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_hwstate.ht_extprotspacing %d\n",__func__, acw->ac_hwstate.ht_extprotspacing);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_vextch %d\n", 		__func__, acw->ac_vextch);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_vextchbusy %d\n", 		__func__, acw->ac_vextchbusy);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_timer_prevstate %s\n",	__func__, ath_cwm_statename[acw->ac_timer_prevstate]);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_timer_statetime %d\n",	__func__, acw->ac_timer_statetime);

#if tbd  //XXX - TODO - use channel flags
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_hwstate.ht_phymode %d\n",	__func__, acw->ac_hwstate.ht_phymode);
	DPRINTF(scn, ATH_DEBUG_CWM, "%s: ac_hwstate.ht_extoff %d\n",	__func__, acw->ac_hwstate.ht_extoff);
#endif
}

