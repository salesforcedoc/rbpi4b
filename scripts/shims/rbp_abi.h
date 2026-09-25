/*
 * rbp_abi.h -- every number that belongs to rbp, in one file.
 *
 * The addresses, offsets and enum values below come from disassembling the
 * XDJ-RX3 player binary (see tools/ and docs/). They are collected here rather
 * than beside their users for one reason: when rbp changes -- a different
 * firmware, a re-patched binary that shifts something -- this is the file that
 * gets re-derived and re-checked. No code, no state: only constants.
 *
 * Note what is *not* here: the SC Live 4's own note numbers and CCs, which
 * belong to the controller map, not to rbp, and are still in rbp_led.c.
 */
#ifndef RBLIVE4_RBP_ABI_H
#define RBLIVE4_RBP_ABI_H

#define UI_OBJ_MGR_GLOBAL 0x2685f2cUL
#define KEY_MANAGER_OFF   100
#define SENDKEY_VTABLE_WORD 2

#define OP_PRESS      0
#define OP_RELEASE    2
#define OP_ROTATE     4
#define OP_VALUE      5        /* absolute value: payload is the float f arg */

#define CH_GLOBAL     1        /* browse/source/selector used ch 1 (deck1) live */

/* ---- RX3 keycodes (see MAPPING.md) ---- */
#define K_SELECTOR   0x420c
#define K_BROWSE     0x0202
#define K_SOURCE     0x0201    /* RX3 SOURCE menu key */
#define K_USB1       0x0209    /* RX3 USB1 direct browse key */
#define K_LINK       0x0207    /* RX3 LINK key */
#define K_REKORDBOX  0x0208    /* RX3 REKORDBOX key */
#define K_TAGLIST    0x0203
#define K_MENU       0x0206
#define K_INFO       0x020b
#define K_BACK       0x420d    /* RX3 BACK key */
#define K_LOAD       0x4311
#define K_PLAY       0x4101
#define K_CUE        0x4102
#define K_SYNC       0x4112
#define K_VINYL      0x4104
#define K_JOG_TOUCH  0x4306    /* jog plate touch  (push/release) */
#define K_JOG_ROT    0x4305    /* jog wheel rotate: f=speed(rev/s) l=pos */
#define K_TEMPO_RANGE 0x4107   /* tempo range select (+-6/10/16/WIDE) */
#define K_MT         0x4108    /* master tempo (key lock) toggle */
#define K_TEMPO_SLIDER 0x4109  /* tempo slider (pitch fader) */
#define K_ALOOP      0x4114
#define K_HOTCUE     0x4113
#define K_SLIPLOOP   0x4115
#define K_BEATJUMP   0x4116
#define K_TRFWD      0x4214    /* XDJ TRACK SEARCH > (track skip next) */
#define K_TRREV      0x4215    /* XDJ TRACK SEARCH < (track skip prev) */
#define K_SRFWD      0x411f    /* XDJ SEARCH > (beat jump next) */
#define K_SRREV      0x4120    /* XDJ SEARCH < (beat jump prev) */
#define K_PAD1       0x4117
#define K_LOOPIN     0x410c
#define K_LOOPOUT    0x410d
#define K_RELOOP     0x410e
#define K_REV        0x410f
#define K_SLIP       0x4110
#define K_MASTER     0x4111
#define K_MASTERCUE  0x4407
#define K_TRIM       0x5019
#define K_EQH        0x501a
#define K_EQM        0x501b
#define K_EQL        0x501c
#define K_FADER      0x501e
#define K_XFADER     0x6017
#define K_HPMIX      0x4405
#define K_HPLEVEL    0x4406
#define K_MASTERLVL  0x4403    /* mixer MASTER LEVEL knob (CC20) */
#define K_COLOR      0x509d    /* Sound Color FX knob (per channel) */
#define K_FILTER     0x50a6    /* Sound Color FX: Filter button */
#define K_DUBECHO    0x50a2    /* Sound Color FX: Dub Echo button */
#define K_SWEEP      0x50a3    /* Sound Color FX: Sweep button */
#define K_NOISE      0x50a4    /* Sound Color FX: Noise button */
#define K_SPACE      0x50a5    /* Sound Color FX: Space button */
#define K_CRUSH      0x50a1    /* Sound Color FX: Crush button */
#define K_BFXTYPE    0x448b    /* Beat FX Type Select (0=Delay, 1=Echo, etc.) */
#define K_BFXCH      0x448c    /* Beat FX Channel Assign (5 = MASTER) */
#define BFX_CH_MASTER 5
#define K_BFX        0x448d    /* Beat FX Enable (ON / OFF toggle) */
#define K_TIME       0x448e    /* Beat FX Time */
#define K_DEPTH      0x448f    /* Beat FX Level/Depth (Wet/Dry intensity) */
#define K_BEATPREV   0x4490    /* Beat FX Beat Fraction < (halve) */
#define K_BEATNEXT   0x4491    /* Beat FX Beat Fraction > (double) */
#define K_TAP        0x4492    /* Beat FX Tap */
#define K_EFFECTQUANT 0x0493
#define K_MIC        0x0814


/* ---- direct headphone-cue (PFL) control ------------------------------
 * rbp's XDJ-RX3 panel has no per-channel PFL/CUE keycode (only deck CUE
 * 0x4102 and MASTER CUE 0x4407), so the SC Live 4's PFL buttons drive rbp's
 * mixer engine directly.  MixerEngine keeps a vector of channel pointers at
 * +12/+16; each channel stores its djengine::EnMixerInput at +20.
 *   MixerEngine singleton          @0x011493c0
 *   setMixerChHeadphoneCue(input,b)@0x000575a0
 *   getMixerChHeadphoneCue(input)  @0x000575e8
 */
#define ME_SINGLETON   0x011493c0UL
#define ME_SET_CUE     0x000575a0UL
#define ME_GET_CUE     0x000575e8UL

/* Headphone stereo type: 0 = stereo, 1 = mono split (L = cue, R = master).
 * rbp's HeadPhone mixes this internally, so the split stays time-aligned. */
#define ME_SET_STEREO  0x0005768cUL


/* master cue (RX3 MASTER CUE button; SC Live 4 has none - strips 3/4 use it) */
#define ME_SET_MASTER_CUE 0x0005766cUL
#define ME_GET_MASTER_CUE 0x0005767cUL


/* ---- DJ FX: channel assign, effect select, time/parameter -----------------
 * SC Live 4 panel (JP21_Controller_Assignments.qml):
 *   DJFxAssign: note 40, velocity = position [0,1,2,3,127] =
 *               ['Channel3','Channel1','Channel2','Channel4','Main']
 *   DJFxSelect: turnCC 35 (endless, 1 = +1, 127 = -1)
 *   DJFxTime:   turnCC 36 (endless, 1 = +1, 127 = -1)
 *
 * rbp side:
 *   EnBeatEffectSelectChannel (djengine::c_str):
 *     0=PLAYER_0 1=PLAYER_1 2=MIC_0 3=ASSIGN_A 4=ASSIGN_B 5=MASTER 6=AUX
 *   onEv_BeatEffectType(SW_BFX_TYPE) is a 14-position switch:
 *     pos -> internal type: 0->6 1->5 2->13 3->7 4->14 5->1 6->4
 *                           7->9 8->10 9->2 10->3 11->12 12->8 13->11
 *   Beat FX time is an index in [getBeatEffectMinTime(), getBeatEffectMaxTime()]
 *   per effect type, so we can step it without tracking our own value.
 */
#define BFX_TYPE_POSITIONS 14
#define ADDR_GET_BFX_TYPE  0x4d514


/* ---- Beat-loop knob -------------------------------------------------------
 * The RX3 has no beat-loop knob: it triggers loops from its pads in the AUTO
 * pad mode.  rbp does have the machinery though:
 *
 *   ui::PlayerInnards::execAutoBeatLoop(short padIndex, bool)   @0x300c64
 *
 * which maps a pad index 0..7 through a per-mode table to a beat fraction and
 * calls playengine::Loop::startAutoBeatLoop(numer, denom, ...):
 *
 *   AUTO (pad mode 1): [5, 7, 6, 13, 14, 15, 16, 17]
 *   SLIP (pad mode 2): [0, 1, 2, 4, 7, 8, 3, 5]
 *   codes -> (numer,denom): 0:16/1 1:8/1 2:4/1 3:3/1 4:2/1 5:4/3 6:2/3 7:1/1
 *                           8:1/2 10:1/8 11:1/16 12:1/32 13:1/3 14:1/5
 *                           15:1/6 16:1/7 17:1/9
 *
 * The Opus Quad has a dedicated delta handler (BeatLoopHandler::
 * selectBeatLoopLengthChange) which rbp does not, so we keep our own cursor
 * and call execAutoBeatLoop() directly.  The per-deck PlayerInnards is reached
 * the same way IUiObjManager::getPlayer() does it:
 *   uiobj = *(0x026867c0); arr = *(uiobj+64); count = *(uiobj+72);
 *   player = arr[ch-1]     (UiObject::Channel is 1-based)
 */
#define ADDR_EXEC_AUTOBEATLOOP 0x300c64
#define UIOBJ_HOLDER_GLOBAL    0x026867c0UL   /* *(here) = IUiObjManager */
#define UIOBJ_PLAYERS_OFF      64
#define UIOBJ_NPLAYERS_OFF     72
#define PLAYERINNARDS_CHAN_OFF 0x26
#define PLAYERINNARDS_MODE_OFF 0x74
#define ALOOP_POSITIONS        8

/* playengine::PlayEngine singleton + isLooping(EnPlayerChannel) (see the LED
 * section below, which defines these later in the file) */
#define ALOOP_PLAYENGINE_GLOBAL 0x011497d0UL
#define ALOOP_PE_ISLOOPING      0x5eadc

#define PLAYENGINE_GLOBAL  0x011497d0UL   /* djengine PlayEngine singleton */
#define PE_ISPLAYING       0x5d880
#define PE_ISLOADED        0x5d3b0
#define PE_ISSYNCON        0x5ff28
#define PE_ISMASTERTEMPO   0x5dfa8
#define PE_ISVINYLMODE     0x5e260
#define PE_ISSLIPMODEON    0x5fc60
#define PE_ISLOOPING       0x5eadc
#define PE_ISCANRELOOP     0x5eab0
#define PE_ISAUTOBEATLOOP  0x5ef74
#define PE_ISLOOPINADJ     0x5f000
#define PE_ISLOOPOUTADJ    0x5f02c

/* rbp LedStat ids we have identified (channels 1/2 = deck 1/2):
 *   49 = deck PLAY  (state 2 while paused  -> panel must blink)
 *    4 = SYNC       3 = VINYL      6 = KEY LOCK      11 = SLIP */
#define LEDSTAT_PLAY   49
#define LEDSTAT_SYNC   4
#define LEDSTAT_VINYL  3
#define LEDSTAT_MT     6
#define LEDSTAT_SLIP   11

/* ---- rbp's own LED table (uif::LedStat) -------------------------------
 * IUiObjManager::getLedManager() (0x31deb0) is:
 *    r3 = *(0x026867c0);  r3 = *(r3 + 104);  return r3;
 * and LedManager::refStatesNoUpdate() (0x33e3ec) is `add r0,r0,#0x30`, so
 * the LedStat lives at LedManager+0x30.  Its layout (from
 * LedStat::setLedState / Led::setState):
 *    +4  u16   number of Led entries
 *    +8  Led*  array base
 *    +14 u16   stride (u16 slots per LedDef::ID)
 *  and each Led entry (0x2c bytes) begins { +0 u32 id, +4 u32 channel,
 *  +16 u32 State }.  Every LED write in rbp funnels through
 *  LedStat::setLedState (45 sites) / setLedState_Color (31 sites), so this
 *  table is the complete, authoritative LED state. */
#define LEDMGR_HOLDER_GLOBAL 0x026867c0UL
#define LEDMGR_OFF_LEDSTAT   104
#define LEDSTAT_OFF          0x30
#define LED_ENTRY_SIZE       0x2c
#define LED_DUMP_MAX         256


/* ---- mixer input routing (djengine::MixerRouteMngr) -----------------------
 * On a real RX3 the physical DECK/LINE switches assign each mixer channel's
 * input. A control surface has no such switches, so a map's startup() writes
 * these two words to pin Input 0 -> Player 0 and Input 1 -> Player 1; the
 * engine's own default leaves channel 2 on player 0, which is not what a
 * two-deck surface means by "channel 2".
 *
 * map_jp21.c still carries these two writes as literals (it predates this
 * block and is deliberately untouched); these names are the same two words, so
 * a newer map does not have to spell an rbp address itself. */
#define ADDR_MIXER_ROUTE_PLAYER0 0x01149f50UL   /* *word = MIXER_ROUTE_INPUT0 */
#define MIXER_ROUTE_INPUT0       0x01149f08UL
#define ADDR_MIXER_ROUTE_PLAYER1 0x01149f54UL   /* *word = MIXER_ROUTE_INPUT1 */
#define MIXER_ROUTE_INPUT1       0x01149f10UL


/* ---- meter hook: ui::Mixer::MonoLvMeter::getLedValue(unsigned char) -----
 * rbp computes every meter's LED bitmask in this one function, and
 * ui::Mixer::checkLedStat calls it three times:
 *   master @0x2d093c   ch1 @0x2d0a40   ch2 @0x2d0a88
 * We patch the function prologue to jump to our hook, so we get rbp's own
 * meter reading (no guessing at level units), while still running the
 * original via a trampoline.  The caller's return address tells us which
 * meter it is. */
#define ADDR_GETLEDVALUE   0x2d07a8UL
#define RET_MASTER         0x2d0940UL
#define RET_CH1            0x2d0a44UL
#define RET_CH2            0x2d0a8cUL
#define PROLOGUE_GETLED    0xe92d4070u   /* push {r4,r5,r6,lr} */


/* ui::Mixer's cached meter levels: uiobj = *(0x026867c0); mixer = *(uiobj+80);
 * lvl = *(long**)(mixer+0xb8)  (see IUiObjManager::getMixer @0x31e054 and
 * ui::Mixer::checkLedStat @0x2d0874). */
#define UI_OBJ_MGR_HOLDER 0x026867c0UL
#define MIXER_OFF         80
#define MIXER_LVLOFF      0xb8

#define RBP_METER_SEGMENTS 11

#endif /* RBLIVE4_RBP_ABI_H */
