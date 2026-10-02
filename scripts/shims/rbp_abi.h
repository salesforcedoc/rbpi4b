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
/* The op BETWEEN a press and a release, and not a release itself. rbp's onKey
 * reads `keyinput->byte[11] & 0xf` and derives two bools from it (the call sites
 * at 0x335104 and the ~40 like it: b1 = (op == 0), b2 = (op <= 1) out of
 * `ands`/`sub`/`rsbs`/`adc`), and OP_PRESS/OP_RELEASE map to (1,1) and (0,0).
 * The pair (0,1) -- op 1, and only op 1 -- is the third edge, and it is the only
 * thing that advances UiKey_KeyPush's record state from 2 to 3.
 *
 * That op 1 is not a release is proved by rbp's own decoder, not assumed:
 * BrowseUiIf::TouchPreviewProc @0xd0020 branches `op == 0` -> start preview,
 * `op == 6` -> seek, else `op - 2 <= 1` (unsigned) -> stop preview. So the
 * release family is exactly {2,3} and op 1 is deliberately excluded from it.
 *
 * Exactly one handler needs it: UiKey_Filter (browse TRACK FILTER), whose
 * `[this+8] > 2` test at 0x118af8 is what gates ChangeBrowseMode(8). Every other
 * browse handler acts on any non-zero state, which is why press+release has
 * always been enough for them and why this value went unnoticed until now. */
#define OP_REPEAT     1
#define OP_RELEASE    2
#define OP_ROTATE     4
#define OP_VALUE      5        /* absolute value: payload is the float f arg */

#define CH_GLOBAL     1        /* browse/source/selector used ch 1 (deck1) live */

/* ---- RX3 keycodes ---- */
#define K_SELECTOR   0x420c
#define K_BROWSE     0x0202
#define K_SOURCE     0x0201    /* RX3 SOURCE menu key */
#define K_USB1       0x0209    /* RX3 USB1 direct browse key */
#define K_LINK       0x0207    /* RX3 LINK key */
#define K_REKORDBOX  0x0208    /* RX3 REKORDBOX key */
#define K_TAGLIST    0x0203
/* MENU is a real key with a *narrow* effect, measured on the unit 2026-09-29:
 * from the SOURCE screen it toggles the MY SETTINGS panel (INFO is replaced by
 * LOAD / BACKGROUND COLOR / WAVEFORM COLOR, and a second press brings INFO back
 * -- 263,596 px each way, and the open panel still there 3 s later, so it is a
 * toggle and not a timeout). From the BROWSE, TAG LIST and PERFORMANCE screens
 * it changes nothing at all: 0 px, full frame. That is why a MENU tap that
 * "did nothing" is not a broken keycode -- check the screen before doubting it. */
#define K_MENU       0x0206
#define K_INFO       0x020b

/* The two browse keys rbp has a code for but this repo had to leave unnamed:
 * PLAYLIST and SEARCH. They sat in map_kbd.c's table and in the top menu's as a
 * deliberate 0 sentinel -- "a plausible-looking trap, not an answer" -- because
 * the derivation is strong and strong is not measured. Measured on the unit
 * 2026-09-29, both at two independent sites, so the derivation was right:
 *
 *   0x0204, CH_GLOBAL  switches the browse view to the PLAYLIST view. The left
 *                      sidebar becomes BANK1..BANK4 + DELETE (an empty bank list
 *                      makes the header read "Please select a source").
 *   0x0205, CH_GLOBAL  opens the SEARCH view: the list area is replaced by a
 *                      search pane with an on-screen QWERTY keyboard.
 *
 * Both were sent from the blank browse screen and from the SOURCE screen and
 * landed on the same screen each time -- 0x0205 pixel-identically (0 px), 0x0204
 * differing by 12,805 px, all of it one 16-row band at the bottom (y 784..799),
 * which is the context hint row and not the view. The negative control is in the
 * same runs: with the two buttons on their shipped 0 sentinel, a tap changed
 * nothing at all (0 px), twice.
 *
 * The codes come from rbp's own name pool, which is contiguous over the whole
 * 0x02xx browse block -- Source, BROWSE, TAGLIST, PlayList, Search, menu -- and
 * reproduces every value in this block. It is what made 0x0204/0x0205 credible
 * enough to try; it is the measurement above, not the pool, that makes them
 * shippable. */
#define K_PLAYLIST   0x0204
#define K_SEARCH     0x0205

/* Two more of the same family, and both are read off rbp's LIVE dispatch table
 * rather than derived: the 230-record browse-key table at 0x327488c was read out
 * of the running process on 2026-09-30 (16-byte records, 229 of 230 slots
 * filled), and record 26 holds UiKey_Filter @0x118964 while record 120 holds
 * UiKey_Shortcut @0x11c290. rbp's own name pool calls the codes "TrackFilter" and
 * "Shortcut". 0x420e is the neighbouring literal in the same `movw` enum pair
 * (rbp names it "TagTrack" -> record 16, UiKey_AddTag); it is named here only so
 * the next reader does not have to re-derive it -- nothing sends it.
 *
 *   0x0210, CH_GLOBAL  opens the SHORTCUT view, browse mode 10. Measured as a
 *                      plain press+release on the loaded browse screen: mode
 *                      3 -> 10, 177,335 px, and again from rbp's resting screen
 *                      (471,268 px). A plain binding, like every row above.
 *   0x420f, CH_GLOBAL  opens the TRACK FILTER panel, browse mode 8 -- and this
 *                      one needs OP_REPEAT. See OP_REPEAT above for why: a press
 *                      alone leaves its record in state 2, UiKey_Filter's gate is
 *                      `state > 2`, and the panel opens only from state 3. A
 *                      press+release measures 0 px on every screen tried,
 *                      including a loaded USB1 list with every one of
 *                      IsEnableFilterKeyPush's terms verified open in the same
 *                      run. It is bound with add_note_repeat() below, never with
 *                      add_note(), and that is the whole of the difference. */
#define K_SHORTCUT   0x0210
#define K_TRACKFILTER 0x420f

/* 0x8002, CH_GLOBAL  the safe eject -- rbp's own name for it is "UsbStop", and it
 *                    is the one key in this file that needs OP_REPEAT *and* a
 *                    plain press: the press and the repeat each do a different
 *                    half of the job. Bound with the repeat edge, like
 *                    K_TRACKFILTER above.
 *
 * Derivation (unchanged since it was read off the binary, three witnesses): it is
 * the last of the 173 names in ui::KeyInput::keyCodeAsText() @0x37cde4 -- the
 * `movw r2, #0x8002` at 0x37d07c feeds the slot at 0x37dc18, whose neighbours are
 * "Power", "TouchTestBottomRight/Left" and "CallNext/Prev" -- and
 * ui::panel_protocol::EupRxDataCheck::checkOtherData() @0x37fb04 builds a KeyInput
 * with this code at two sites (0x3809cc, 0x380a2c), ch 1, press/release. So the
 * RX3's own panel sends it.
 *
 * WHAT IT DOES, and why the edge sequence is the whole of the binding.
 * ui::UsbStorageManager::onKey @0x3259f4 takes it straight to `onUsbStopKey`
 * @0x325788 (keycode 0x8002, channel == the manager's own [this+0x84], both
 * verified live), and that function branches on the op nibble into three
 * different halves of one flow:
 *
 *   op 0 (press),  needs [this+0x88] == 2   sets [this+0x8c] = 5, mutes both
 *                                           players and calls
 *                                           PlayerSkeleton::notifyMediaDisconnect
 *                                           on each plus DbProxy's
 *   op 1 (repeat), needs [0x88] in {1,2}    sets bit 7 of [this+0x3d0] and calls
 *                                           UsbStorageManager::request_usb_stop
 *                                           @0x324578 -- THIS is the eject request
 *   op 2 (release)                          unwinds: clears bit 7, unmutes both
 *                                           players, resets [0x8c]
 *
 * Measured on the unit 2026-10-01, which is what pins it: a press+release tap is a
 * silent no-op (this is the "0x8002 does nothing" that stood for a release), but
 * holding the key makes [usb1+0x8c] read 5 for exactly the length of the hold and
 * the release drop it back to 0 -- so the press really does run and it is the
 * missing REPEAT that leaves the eject unrequested. A press alone is therefore not
 * enough and a repeat alone is not either: send press, then repeat, then release.
 * The release has to be sent too, because the press path arms a UiTimer on the
 * key's own record (IKeyManager::onKey @0x37b6cc) and abandoning the key would
 * leave that record live.
 *
 * NOTE the contrast with K_TRACKFILTER, which needs the repeat *instead of* a
 * usable press. Here the press is load-bearing. */
#define K_USBSTOP    0x8002

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

/* QUANTIZE on/off, per deck. Derived rather than guessed, because the RX3's own
 * panel has no button we could read a number off: ui::PlayerInnards::
 * onPhysicalKey() @0x306b78 is rbp's keycode -> handler dispatch -- a binary
 * search over the key code at IKeyInput+8 whose leaves tail-branch into the
 * onKey_* handlers, and a vtable slot of PlayerInnards -- and 0x410b is the one
 * code whose leaf reaches onKey_Quantize() @0x3028bc (the `bls 306e9c` branch at
 * 0x306d5c, under the tree above it). The same tree reproduces every keycode
 * this file already names, which is what the reading rests on.
 *
 * The handler never looks at the keycode: it takes the deck from its own
 * channel, flips djengine::DjEngineIF::setDeckQuantizing(ch, !isDeckQuantizing)
 * and then calls into IPlayerSetting -- and that last call is what repaints the
 * widget. So sending this key *is* the whole gesture, display included; calling
 * the engine directly would leave the screen behind. */
#define K_QUANTIZE   0x410b
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
#define K_EFFECTQUANT 0x0493    /* BEAT FX quantize (the small QUANTIZE inside the
                                 * Beat FX panel), NOT the deck's on/off -- that
                                 * one is K_QUANTIZE above. Referenced nowhere:
                                 * rbp's Beat FX quantize is a touch class
                                 * (Shortcut_EffectQuantize_On/_Off). */
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

/* The map above, inverted: internal type -> switch position, -1 for a type no
 * position reaches (0 is not one of them, so the first entry is never a hit).
 *
 * It lives here, beside the forward map it inverts, because both controller
 * maps need it: each of them turns a rotary or a button into steps of this
 * switch, and when the control is first moved the map has to work out which
 * position rbp is already in -- which means reading the type rbp holds and
 * coming back through this table. A second copy in one of the maps is a copy
 * that can drift from the comment above without anything failing.
 *
 * map_jp21.c uses it to seed the RX3's FX-select rotary, which reads rbp's type
 * by calling ADDR_GET_BFX_TYPE itself -- that gesture is not in the JP21
 * fixture, so nothing there would catch a misread. map_flx4.c uses it on the
 * FLX4's FX SELECT button, and reads the type through rbp_bridge.h's
 * rbp_beatfx_type() instead, because that gesture IS in the FLX4 fixture and a
 * host test has to be able to answer for rbp. */
static const signed char bfx_type_to_pos[15] = {
     -1, 5, 9, 10, 6, 1, 0, 3, 12, 7, 8, 13, 11, 2, 4
};

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
/* NOT the pad mode -- see UI_GET_PADMODE below, which is the real one.  This byte
 * still discriminates PlayerInnards during the vtable scan (and a direct write to
 * it still sticks), which is why it was mistaken for the mode for so long: polled
 * 1.1 M times across all four pad-mode buttons while rbp's pad grid verifiably
 * switched, it never left 0 (2026-09-30).  Nothing should read it as a mode. */
#define PLAYERINNARDS_MODE_OFF 0x74

/* PlayerInnards+0x38: the word that decides which handler a HOT CUE pad key
 * reaches -- the *selector*, and the whole reason SHIFT + pad does nothing
 * today.  Player::onHotCueEvent @0x2f5720 loads it once at entry (`ldr r7,
 * [r0, #0x38]` off the innards) and takes a different arm per value: 1 and 2
 * call other things, and **only 3** tail-branches to
 * ui::Player::onHotCueDeleteEvent @0x2fb328.  It is written in exactly four
 * places in the whole binary, all inside PlayerInnards::pad_HotCue @0x307918
 * (0x307a30 =1, 0x307b18 =2, 0x307c70 =2, 0x307df4 =3), and pad_HotCue's only
 * caller is onKey_Pad @0x3063b4, whose only entry is a tail-branch at 0x306dbc
 * inside onPhysicalKey -- which clears the word at its own entry.
 *
 * That is the physical key path, and IKeyManager::sendKey does not take it.  So
 * every key the shim sends arrives here with the word 0, which is the
 * onHotCueCallModeEvent arm -- exactly why a shim-sent K_PAD1+p *triggers* a cue
 * and could never delete one.
 *
 * SETTING THIS WORD FROM THE SHIM DOES NOT WORK, and that is measured rather
 * than inferred (2026-10-01).  hotcue_delete() used to write 3 here around the
 * pad key it sends; the cue survived.  The proof is a sentinel: write the
 * impossible value 7 to the word on the deck's OWN innards, send one plain
 * K_PAD1 key, read it back -- it is **0**.  Something on the key path clears
 * the word before the handler reads it, so no value written from outside
 * survives the dispatch.  (onHotCueEvent's delete arm carries a second gate in
 * any case: `cmp r2, #1` on its IInnards3RES argument, which only rbp's own
 * caller supplies.)
 *
 * So the gesture is a direct call, not a key -- see ADDR_HOTCUE_DELETE.  This
 * offset is kept because it is what the delete arm reads, and what any
 * pad_HotCue-shaped fix would have to write. */
#define PLAYERINNARDS_PADCMD_OFF 0x38

/* ui::Player::onHotCueDeleteEvent(int pad) @0x2fb328 -- delete ONE hot cue,
 * addressed by the pad number itself (1..8).  The dispatch that reaches it is
 * virtual; this function is a plain concrete symbol, so the shim can call it
 * directly, and that is the only route left once the key path is excluded
 * above.
 *
 * What makes calling it from outside safe is how thoroughly it guards itself:
 * pad range first, then a per-pad flag in Player+0x479..0x47b (the bit SET
 * means that pad holds no cue -> return), then Player+0x13c TrackInfo (NULL ->
 * return), then TrackInfo::seacheHotCue(pad) returning NULL -> return, then a
 * per-pad state byte != 2 -> return.  Only past all of that does it reach
 * PlayerInnards::onEv_SetHotCueRecDisable and the DbProxy delete.  An empty pad
 * is therefore a no-op, not a stray write. */
#define ADDR_HOTCUE_DELETE 0x0002fb328UL

/* PlayerInnards+0x30: the deck's djengine::DjEngineIF*.  A plain pointer, and
 * the one `pad_HotCue` hands the engine -- see ADDR_ENGINE_CLEAR_HOTCUE. */
#define PLAYERINNARDS_ENGINE_OFF 0x30

/* THE OTHER HALF OF THE DELETE, and the reason a deleted cue kept drawing.
 *
 * ui::Player::onHotCueDeleteEvent -- the whole of what the shim calls -- does the
 * UI and DB half and stops: TrackInfo's cue list, the pad's own flag and state
 * byte, `onEv_SetHotCueRecDisable`, `DbProxy::deleteCue`.  It never touches the
 * engine, so the engine still holds the cue and the pad grid keeps drawing it.
 * The engine half lives only in ui::PlayerInnards::pad_HotCue @0x307918, and a
 * `bl 0x48a48` at 0x307e68 is the ONLY call to clearHotCue anywhere in the
 * binary.  pad_HotCue is the *physical* pad handler, reached from onPhysicalKey
 * via onKey_Pad -- the path IKeyManager::sendKey never takes, which is why no
 * keycode this shim can send reaches it.  Hence a second direct call.
 *
 * The two are a matched pair and both are made, in this order.  rbp's own
 * sequence at 0x307e18 is: channel = (PlayerInnards+0x26 == 2) ? 1 : 0  (the
 * 0-based deck index), engine = [PlayerInnards+0x30], then
 * isRegisteredHotCue(ch, pad) and clearHotCue(ch, pad).
 *
 * EnCueType is the PAD NUMBER 1..8, not an index -- measured from the binary,
 * not assumed: playengine::Player::backHotCueGate @0x66bf8 opens `sub r2, r1,
 * #1; cmp r2, #7; bhi <return 0>`, and isHotCueGatePlaying @0x5edc8 returns
 * [player+0xefc], which backHotCueGate compares against that same 1..8 value.
 *
 * Both entry points ignore their `this` (each reloads r0 from its own global
 * singleton at 0x011497d0, lazily constructing the PlayEngine if it is NULL), so
 * the engine pointer passed in is a faithful mirror of rbp's call rather than a
 * requirement -- but it is passed anyway, because "the receiver is dead" is the
 * kind of claim this file has been wrong about before.
 *
 * The ghost this answers is measured (2026-10-01): after a delete that by every
 * other witness succeeded -- the FLX4 pad LED goes off, the cue leaves
 * TrackInfo and the export DB -- deck 1 pad 5's StatWatcher record still held IN
 * 0x31fa6 with colour 0, and the grid kept drawing it as a default-cyan pillar,
 * because the grid draws a pillar for any pad whose IN is not 0xffffffff.  The
 * same stale IN was in the Player's own mirror at Player+0x268+52*(pad-1), and
 * clearing it there was undone by rbp -- i.e. that mirror is re-synced, so the
 * engine is the authority.  THAT is the diagnosis; the cure this constant
 * enables is measured separately, in the same memory. */
#define ADDR_ENGINE_IS_REGHOTCUE 0x00048b00UL   /* djengine::DjEngineIF::isRegisteredHotCue(EnPlayerChannel, EnCueType) */
#define ADDR_ENGINE_CLEAR_HOTCUE 0x00048a48UL   /* djengine::DjEngineIF::clearHotCue(EnPlayerChannel, EnCueType) */

#define ALOOP_POSITIONS        8

/* rbp's REAL per-deck pad mode, straight from rbp's own reader.
 *
 * UiGetPadMode(ENUM_DECK) @ 0xfd3cc is an exported C function -- deck 0 is deck 1
 * -- and it walks:
 *   holder      = *(UI_PADMODE_HOLDER_GLOBAL);
 *   statwatcher = *(holder + PADMODE_STATWATCHER_OFF);
 *   perdeck     = *(statwatcher + (deck ? PADMODE_DECK2_OFF : PADMODE_DECK1_OFF));
 *   return *(unsigned char *)(perdeck + PADMODE_BYTE_OFF);
 * returning 0 HOT CUE / 1 AUTO BEAT LOOP / 2 SLIP BEAT LOOP / 3 BEAT JUMP --
 * the same four modes, in the same order, as the onKey_* dispatch table.
 *
 * Measured 2026-09-30 against the live process: injecting ch0 notes 27/30/32/34
 * through the shim's own sequencer port moved deck 1's value 3 -> 0, 1, 3, 2
 * exactly, with deck 2's held at 0 throughout.  The address is a plain CALL, so
 * no scan and no +0x74 guesswork is needed to know what mode rbp is in -- which
 * is the missing half of the pad-mode LEDs (docs/13 S9.9). */
#define UI_GET_PADMODE           0x000fd3cc
#define UI_PADMODE_HOLDER_GLOBAL 0x02685f2cUL
#define PADMODE_STATWATCHER_OFF  0x38
#define PADMODE_DECK1_OFF        0x0c
#define PADMODE_DECK2_OFF        0x10
#define PADMODE_BYTE_OFF         0x281

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
 *  table is the complete, authoritative LED state.
 *
 *  One more word of the same entry is load-bearing, four bytes past State: see
 *  LED_ENTRY_OFF_UNASSIGNED. */
#define LEDMGR_HOLDER_GLOBAL 0x026867c0UL
#define LEDMGR_OFF_LEDSTAT   104
#define LEDSTAT_OFF          0x30
#define LED_ENTRY_SIZE       0x2c
#define LED_DUMP_MAX         256

/* "rbp has assigned this pad nothing": the u32 at entry +20, which is 1 for an
 * empty performance pad and 0 for one that has been given a colour. rbp's own
 * name for the field is not known; what it does is MEASURED.
 *
 * 2026-10-01, deck 1 in HOT CUE, read live out of /proc/pid/mem: every pad is
 * State=1, so State does NOT separate an assigned pad from an empty one -- the
 * four pads with no cue are rgb ffffff and carry 1 here, and the pads carrying
 * cues (1aff00 green, ff0045 pink, ffe800 yellow) carry 0. The operator set two
 * more cues while the dump ran and those two entries flipped 1 -> 0 as they
 * landed, which is what makes this a reading rather than a correlation.
 *
 * And it is about the PAD, not about the mode: in AUTO BEAT LOOP all eight
 * carry 0, rbp having coloured every one of them ff8c00. So a surface can use
 * this to dark a pad that holds nothing without a per-mode table -- which is
 * what the FLX4 needs, because State alone lights all eight white pads in HOT
 * CUE and the operator's panel showed exactly that. */
#define LED_ENTRY_OFF_UNASSIGNED 20


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
