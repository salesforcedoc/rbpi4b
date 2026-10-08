#!/bin/sh
# doctor.sh — check a deployed rblive4 unit and report on it. Changes NOTHING.
#
#   sh /opt/rblive4/doctor.sh
#   RB_DEPLOY_ROOT=/srv/rblive4 sh doctor.sh
#   sh install.sh doctor            # same thing, via the installer's entry point
#
# Exit status: 0 if nothing failed, 1 if something did. Warnings alone do not
# fail the run — they are things worth knowing, not things that break a set.
#
# --- why this exists --------------------------------------------------------
#
# The sibling public port (github.com/mutlisensor/Rx3-flx4) ships an
# `install.sh doctor`: the ordinary install path with a failure accumulator,
# exiting *before* the first `sudo -v`, printing one paste-ready fix. Its value
# is that it is the same code that would do the work, so the check cannot drift
# from what an install really requires.
#
# This port's install.sh interleaves its checks with the mutations they protect,
# and this tree deploys by `scp` far more often than by running install.sh at
# all. So doctor is a separate script — and to keep the drift the sibling
# avoids by construction, every path and every name below is read out of the
# SAME SOURCES OF TRUTH the other scripts use: rb.conf, lib.sh and
# start-rb.sh's own override list. It hardcodes no path of its own. If a check
# is added here, the comment says which file the check came from.
#
# --- what it does NOT do ----------------------------------------------------
#
# It opens nothing for writing, creates nothing, mounts nothing, loads no
# module, and starts or stops nothing. The one thing it executes is a chroot'd
# /bin/busybox, which prints one line and exits (line 9 of the chroot check).
# That restraint is the point: this is safe to run on a live unit in the middle
# of a set — including from a shell whose next command is a restart.
#
# It also does not look for `rbp` by name. It reads the running process's maps
# to say WHICH BUILD IS ACTUALLY LOADED, which is the single question this tree
# has most often got wrong: start-rb.sh's install_override() skips a shim whose
# deploy-root copy is absent, in silence, leaving an older one in the chroot.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)

FAILS=0
WARNS=0
FIX=""

say()  { echo "doctor: $*"; }
hdr()  { echo; echo "[$1] $2"; }
ok()   { echo "  ok    $*"; }
warn() { echo "  warn  $*"; WARNS=$((WARNS + 1)); }
bad()  { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
note() { echo "        $*"; }

# Collect a paste-ready line for the summary at the end, de-duplicated: two
# failing checks frequently have the same one-line fix, and printing it twice
# makes the summary look like two problems.
fix() {
	case "${FIX_SEEN:-}" in
		*"|$1|"*) return 0;;
	esac
	FIX="$FIX
  $1"
	FIX_SEEN="${FIX_SEEN:-}|$1|"
}

# --- the sources of truth ---------------------------------------------------

# RB_DEPLOY_ROOT goes first because rb.conf uses it to find rb.local.conf, and
# rb.local.conf is where this unit's measured overrides live.
RB_DEPLOY_ROOT="${RB_DEPLOY_ROOT:-/opt/rblive4}"
RB_CONF_FILE="${RB_CONF_FILE:-$HERE/rb.conf}"
[ -f "$RB_CONF_FILE" ] || RB_CONF_FILE="$RB_DEPLOY_ROOT/rb.conf"

if [ ! -f "$RB_CONF_FILE" ]; then
	echo "doctor: no rb.conf at $RB_CONF_FILE, and none at $RB_DEPLOY_ROOT/rb.conf." >&2
	echo "  Nothing below can be checked without it: every path this script" >&2
	echo "  tests comes out of rb.conf. Run install.sh, or set RB_DEPLOY_ROOT." >&2
	exit 1
fi

# lib.sh for RB_CONF_SCHEMA (the one place the schema number lives) and
# pids_matching() (which finds the player the same way the launcher does —
# through /proc/<pid>/cmdline, never `ps | awk`).
. "$HERE/lib.sh"
. "$RB_CONF_FILE"

# The two paths every other script derives the same way (lib.sh:58-59).
RB_CHROOT="${RB_CHROOT:-$RB_DEPLOY_ROOT/rbx3-run}"
MNT="${RB_MEDIA_MOUNT:-$RB_DEPLOY_ROOT/media/usb1}/sda1"  # usb-watch.sh:32
CH_MNT="$RB_CHROOT${RB_CHROOT_MEDIA:-/media/usb1/sda1}"   # usb-watch.sh:39

say "deploy root $RB_DEPLOY_ROOT"
say "chroot      $RB_CHROOT"
say "config      $RB_CONF_FILE"

if [ "$(id -u)" != "0" ]; then
	warn "not root: the chroot execution test is skipped (it needs chroot(2)),
  and some of the device checks read as absent when they are only unreadable."
	note "re-run as root for the full report: sudo sh $HERE/doctor.sh"
fi

# --- small helpers ----------------------------------------------------------

# First 8 hex digits of a file's md5, for display only. Equality is decided by
# cmp, which needs no md5 tool at all — so a target without md5sum still gets a
# correct verdict, just a less readable table.
_sum() {
	if [ -x "$(command -v md5sum 2>/dev/null)" ]; then
		md5sum "$1" 2>/dev/null | cut -c1-8
	else
		echo '--------'
	fi
}

# Is $1 a mountpoint? mountpoint(1) is util-linux; /proc/mounts is everywhere.
# Falling back to it keeps a minimal target from reporting every bind as absent,
# which is the failure mode a diagnostic can least afford. (Neither form is
# fooled by the chroot's own /etc/mtab, which fix-dev.sh points at /proc/mounts.)
is_mount() {
	mountpoint -q "$1" 2>/dev/null && return 0
	awk -v p="$1" '$2 == p { f = 1 } END { exit !f }' /proc/mounts 2>/dev/null
}

# A deployed-from file and the copy the player actually loads. $1 = label,
# $2 = the deploy-root copy (what start-rb.sh's install_override reads),
# $3 = the chroot copy (what LD_PRELOAD names).
#
# Three outcomes, and the middle one is the trap this whole section exists for:
# install_override() returns 0 in silence when $2 is absent, so the chroot keeps
# whatever older build was there and rbp runs it. Memory, 2026-10-06: "check the
# shim's vintage before diagnosing any on-unit symptom".
pair() {
	_p2=$2
	_p3=$3
	if [ ! -f "$_p3" ]; then
		bad "$1: $3 is MISSING — rbp runs without it"
		note "ld.so prints 'cannot preload' and continues, so this is silent on the glass."
		if [ -f "$_p2" ]; then
			fix "cp $_p2 $_p3"
		else
			# No paste-ready line, deliberately: the fix is to build the file,
			# which is not a command. Saying so here beats printing a cp whose
			# source does not exist.
			note "and there is nothing at the deploy root to copy from: $_p2"
		fi
		return 0
	fi
	if [ ! -f "$_p2" ]; then
		warn "$1: no deploy-root copy at $2"
		note "start-rb.sh only copies when that file exists, so the chroot's"
		note "copy ($(_sum "$_p3")) is what will run."
		return 0
	fi
	if cmp -s "$_p2" "$_p3"; then
		ok "$1: $(_sum "$_p3") (deployed copy and loaded copy are the same file)"
	else
		bad "$1: the loaded copy is a DIFFERENT build from the deployed one"
		note "deployed $(_sum "$_p2")  ->  loaded $(_sum "$_p3")"
		fix "cp $_p2 $_p3"
	fi
	return 0
}

# --- 1. the layout install.sh would have produced ---------------------------

hdr 1 "layout"
[ -d "$RB_DEPLOY_ROOT" ] || { bad "$RB_DEPLOY_ROOT does not exist — not installed here"; }
[ -d "$RB_CHROOT" ] || bad "$RB_CHROOT does not exist (the chroot)"

if [ -d "$RB_CHROOT" ]; then
	ok "chroot tree present: $RB_CHROOT"
fi

# The schema guard, the same comparison lib.sh:48 makes — but reported rather
# than fatal, because reporting is the whole job here.
if [ "${RB_CONF_VERSION:-0}" = "$RB_CONF_SCHEMA" ]; then
	ok "rb.conf schema v${RB_CONF_VERSION} matches lib.sh's v$RB_CONF_SCHEMA"
else
	bad "rb.conf is schema v${RB_CONF_VERSION:-?}; these scripts expect v$RB_CONF_SCHEMA"
	note "every script that sources lib.sh exits on this, so the unit looks"
	note "broken rather than misconfigured. Re-deploy a matching pair."
fi

for s in lib.sh fix-dev.sh start-rb.sh usb-watch.sh; do
	[ -f "$RB_DEPLOY_ROOT/$s" ] || {
		bad "$RB_DEPLOY_ROOT/$s is missing"
		fix "scp scripts/device/$s root@<unit>:$RB_DEPLOY_ROOT/"
	}
done

# rb.local.conf is where a value MEASURED ON THIS UNIT belongs: rb.conf is
# shipped and overwritten on every install, this file never is (install.sh:149-156).
if [ -f "$RB_DEPLOY_ROOT/rb.local.conf" ]; then
	ok "rb.local.conf present (machine-local overrides survive a reinstall)"
else
	warn "no rb.local.conf — this unit has no place for measured values"
	note "RB_POINT_KIND=rel is the one that has actually been needed here; set by"
	note "hand, it reverts on the next install without this file."
	fix "printf 'RB_POINT_KIND=rel\\n' > $RB_DEPLOY_ROOT/rb.local.conf"
fi

# --- 2. the shims: deployed, versus what the loader will actually take ------

hdr 2 "shims: the deployed file and the copy rbp loads"

# The list comes from RB_LD_PRELOAD itself (rb.conf:636), so a shim added to the
# preload list is checked here without touching this script. basename of each
# entry is also the deploy-root filename, which is exactly what start-rb.sh's
# install_override() list mirrors (start-rb.sh:115-120).
_seen_preload=0
for p in $(echo "${RB_LD_PRELOAD:-}" | tr ':' ' '); do
	_seen_preload=$((_seen_preload + 1))
	pair "$(basename "$p")" "$RB_DEPLOY_ROOT/$(basename "$p")" "$RB_CHROOT$p"
done
if [ "$_seen_preload" -eq 0 ]; then
	bad "RB_LD_PRELOAD is empty in $RB_CONF_FILE — rbp would run with no shims at all"
	fix "set RB_LD_PRELOAD in $RB_CONF_FILE"
fi

# The patched DirectFB system module, whose two names differ (start-rb.sh:122).
pair "libdirectfb_fbdev (rot16)" \
     "$RB_DEPLOY_ROOT/libdirectfb_fbdev-rot16.so" \
     "$RB_CHROOT/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"

# The player itself. Two different questions, and keeping them apart is the
# point: is a player installed, and is it OURS. build-chroot.sh bakes the patched
# player into the chroot, with scripts/patch-rbp-nopc.py as its last stage
# (build-chroot.sh:204), so the chroot copy is the one that runs.
#
# The three hashes are the three stages of tools/patch-rbp/PATCHES.md, and naming
# them is what makes a wrong player diagnosable: "differs from what I expected"
# cannot tell you whether you are looking at Pioneer's stock binary or at our own
# half-patched stage 1, and those two are not the same problem. It also closes a
# real trap. The deploy-root override is named `rbp-audio` -- which is STAGE 1 --
# and start-rb.sh:121 copies it over the installed player at EVERY launch when it
# exists. So a file of that name at the deploy root does not merely sit there; it
# wins, and it puts back the getPcController() NULL-deref that kills rbp about a
# second after start, with no display and no log.
#
# Updated 2026-10-08. Dropping the section-6 waveform gate (PATCHES.md §6) changed
# stage 1's output, and through it the final build, so both hashes moved. The
# pre-revert pair -- stage 1 `3706c68f`, final `3cecd92a` -- is no longer produced
# by anything in this tree; a unit carrying either is running history.
RB_RBP_STOCK=4f2efcfc0c9e3f539289f863acfddcc6	# Pioneer XDJ-RX3 v1.20, untouched
RB_RBP_STAGE1=3dda2d4e10187a75bfc16a7b4f16f192	# +66 words; no getPcController() fix
# Superseded 2026-10-07. This is the WORST of the four to meet in the field, because
# it looks perfect: rbp starts, paints, plays. Its getPcController() is a permanent
# `mov r0,#0`, which is byte-identical to the thunks Pioneer ships for its own
# unimplemented functions -- and it makes the Pro DJ Link gate structurally
# unreadable, so Link is dead on both routes with nothing on screen to say so.
RB_RBP_NOPC2=18a64bc4d0ffd1cbd35f3a6ea447fca8	# +2 words; SILENTLY DISABLES Pro DJ Link
RB_RBP_FINAL=97aa2223c4ca5906b66389420f29d03f	# +12 words; the build that ships

# The full digest, unlike _sum() above which truncates for display: these are
# compared, not just shown. No md5sum means NO verdict rather than a wrong one,
# which is why the caller below tests for "?" separately.
_md5() {
	if [ -x "$(command -v md5sum 2>/dev/null)" ]; then
		md5sum "$1" 2>/dev/null | cut -d' ' -f1
	else
		echo "?"
	fi
}

# Name a build from its hash, so that both the report and the fix can say which
# of the three it is instead of only that it is not the one we wanted.
_rbp_build() {
	case "$1" in
		"$RB_RBP_FINAL")  echo "final -- stock + all interoperability patches";;
		"$RB_RBP_NOPC2")  echo "SUPERSEDED -- runs fine, but Pro DJ Link is silently dead";;
		"$RB_RBP_STAGE1") echo "STAGE 1 ONLY -- missing the getPcController() fix";;
		"$RB_RBP_STOCK")  echo "Pioneer's stock binary, unpatched";;
		"?")              echo "unknown (no md5sum on this target)";;
		*)                echo "not one of the four known builds";;
	esac
}

RBP_INSTALLED="$RB_CHROOT/root/pdj/rbp"
if [ ! -f "$RBP_INSTALLED" ]; then
	bad "the player is missing: $RBP_INSTALLED"
	fix "# scripts/build-chroot.sh stages it; or tools/patch-rbp/rbp_patch.py <stock> -o rbp-audio && scripts/patch-rbp-nopc.py rbp-audio -o rbp-nopc"
else
	_rbp_sum=$(_md5 "$RBP_INSTALLED")
	if [ "$_rbp_sum" = "$RB_RBP_FINAL" ]; then
		ok "player presents as our final build: $_rbp_sum"
	else
		# Both of the other known builds are wrong on this hardware, and the
		# stock one is wrong in a way that reads as "the unit is dead".
		if [ "$_rbp_sum" = "?" ]; then
			warn "player present but unverifiable: $(wc -c < "$RBP_INSTALLED") bytes, no md5sum here"
			note "expected $RB_RBP_FINAL; compare by hand before trusting this unit."
		else
			bad "the installed player is NOT our build: $_rbp_sum"
		fi
		note "$RBP_INSTALLED -- $(_rbp_build "$_rbp_sum")"
		note "ours is $RB_RBP_FINAL (tools/patch-rbp/PATCHES.md pins all four)"
		fix "# rebuild into the chroot, or scp a known-good player over $RBP_INSTALLED"
	fi
fi

# The deploy-root override. Not a curiosity: start-rb.sh:121 copies it over the
# installed player at EVERY launch when it is present, so a stale or stock file
# here does not sit quietly -- it wins at the next restart. Absent is the normal
# case and is reported as nothing at all.
if [ -f "$RB_DEPLOY_ROOT/rbp-audio" ]; then
	_ovr_sum=$(_md5 "$RB_DEPLOY_ROOT/rbp-audio")
	if [ "$_ovr_sum" = "$RB_RBP_FINAL" ]; then
		ok "deploy-root override is the final build, and will re-install cleanly"
	elif [ "$_ovr_sum" = "$RB_RBP_STAGE1" ] || [ "$_ovr_sum" = "$RB_RBP_STOCK" ] \
	  || [ "$_ovr_sum" = "$RB_RBP_NOPC2" ]; then
		bad "$RB_DEPLOY_ROOT/rbp-audio is $(_rbp_build "$_ovr_sum")"
		note "start-rb.sh copies that over the installed player at every launch, so"
		note "the next restart would replace a working player with a broken one."
		fix "mv $RB_DEPLOY_ROOT/rbp-audio $RB_DEPLOY_ROOT/rbp-audio.rejected"
	else
		warn "$RB_DEPLOY_ROOT/rbp-audio is $(_rbp_build "$_ovr_sum")"
		note "start-rb.sh will copy it over the installed player at the next launch."
		fix "mv $RB_DEPLOY_ROOT/rbp-audio $RB_DEPLOY_ROOT/rbp-audio.rejected   # unless you mean to deploy it"
	fi
fi

# --- 3. the loader wiring ---------------------------------------------------

hdr 3 "loader wiring"
case "${RB_LD_PRELOAD:-}" in
	*/crashcatch.so*) ok "crashcatch.so is first in RB_LD_PRELOAD (its constructor must arm first)";;
	*) bad "crashcatch.so is NOT first in RB_LD_PRELOAD"
	   note "RB_LD_PRELOAD=$RB_LD_PRELOAD"
	   note "the exit witness has to install its handler before any other preload"
	   note "can fault or exit, so an earlier shim can hide a crash entirely."
	   fix "put /usr/lib/crashcatch.so first in RB_LD_PRELOAD ($RB_CONF_FILE)";;
esac

# netshim vs fbshim, and this one is a POSITION rather than a presence: both
# define ioctl (fb_shim.c:180), and a process resolves ioctl to the FIRST
# preloaded object that defines it. Get it wrong and there is no error anywhere --
# either the network rewrite silently never runs, or fbshim never sees an ioctl
# and the display breaks. So it is asked as a comparison of positions in the
# list, not as a substring test that both names would satisfy.
case "${RB_LD_PRELOAD:-}" in
	*netshim.so*)
		_net=${RB_LD_PRELOAD%%netshim.so*}
		_fb=${RB_LD_PRELOAD%%fbshim.so*}
		if [ "${#_net}" -lt "${#_fb}" ]; then
			ok "netshim.so precedes fbshim.so in RB_LD_PRELOAD (both define ioctl; first wins)"
		else
			bad "netshim.so comes AFTER fbshim.so in RB_LD_PRELOAD"
			note "both define ioctl, and the first one loaded is the one that runs."
			note "fbshim's would win, so every network query would pass through"
			note "unrewritten and rbp's Link stack would stay silent with no error."
			fix "put /usr/lib/netshim.so second in RB_LD_PRELOAD ($RB_CONF_FILE)"
		fi
		# The flag is deliberately OFF by default, so say which state the unit is
		# in rather than leaving "it does nothing" unexplained.
		if [ "${RB_NETALIAS:-0}" = "1" ]; then
			ok "RB_NETALIAS=1 — the alias is armed${RB_NETALIAS_IFACE:+ for $RB_NETALIAS_IFACE}"
		else
			note "RB_NETALIAS is not 1, so netshim passes every call through"
			note "unchanged. That is the shipped default; set RB_NETALIAS=1 in"
			note "$RB_DEPLOY_ROOT/rb.local.conf to arm it (docs/18-prodjlink.md)."
		fi
		# The bring-up trigger. Worth a line of its own because it is the only
		# part of netshim that runs rbp's own code, and because it is useless
		# without the substitution above — a reader who has armed one and not
		# the other should be told here rather than from an empty log.
		if [ "${RB_NETALIAS_CONNECT:-0}" = "1" ]; then
			ok "RB_NETALIAS_CONNECT=1 — the Link bring-up is armed, watching ${RB_NETALIAS_CONNECT_FILE:-/tmp/rb_link.req}"
			if [ "${RB_NETALIAS:-0}" != "1" ]; then
				bad "but RB_NETALIAS is not 1: the call will refuse with no-ip forever"
				fix "set RB_NETALIAS=1 as well ($RB_CONF_FILE) — the substitution is what gives rbp an address to connect with"
			fi
		fi;;
	*) note "netshim.so is not in RB_LD_PRELOAD (fine unless you want Pro DJ Link)";;
esac

# --- 4. the ABI test: can this kernel actually run the tree -----------------

hdr 4 "chroot execution (the real ABI test)"
if [ "$(id -u)" = "0" ]; then
	# stderr is captured into a variable rather than a temp file, so the whole
	# script still opens nothing for writing. The redirection order matters:
	# `2>&1` must come first, while stdout is still the command substitution.
	_err=$(chroot "$RB_CHROOT" /bin/busybox echo ok 2>&1 >/dev/null)
	if [ -z "$_err" ]; then
		ok "the chroot runs a 32-bit ARM EABI5 binary against its own glibc"
	else
		bad "the chroot could NOT execute a 32-bit binary"
		printf '%s\n' "$_err" | sed 's/^/        /'
		note "usually a 64-bit kernel with 32-bit emulation disabled (CONFIG_COMPAT)."
		note "Every shim and the player are ARM32 soft-float; there is no 64-bit rbp."
		fix "install Pi OS Lite 32-bit (armhf) — see docs/13-raspberrypi4.md"
	fi
else
	warn "skipped (not root)"
fi

# --- 5. device nodes --------------------------------------------------------

hdr 5 "device nodes"
if [ -e /dev/fb0 ]; then
	ok "/dev/fb0 present (no display without it)"
else
	bad "/dev/fb0 is missing — there is no display and rbp exits early"
	fix "enable dtoverlay=vc4-kms-v3d in /boot/firmware/config.txt and reboot"
fi

if [ -e /dev/snd/seq ]; then
	ok "/dev/snd/seq present (no sequencer = no controller input at all)"
else
	bad "/dev/snd/seq is missing — rbp starts and receives no control events"
	note "the classic silent 'controls do nothing'; snd-seq is not autoloaded"
	note "on every boot, and fix-dev.sh's modprobe is the only thing that fixes it."
	fix "modprobe snd-seq   # and check CONFIG_SND_SEQUENCER in the kernel"
fi

if [ -d /dev/input ] && [ -n "$(ls /dev/input 2>/dev/null)" ]; then
	ok "/dev/input: $(ls /dev/input 2>/dev/null | tr '\n' ' ')"
else
	warn "/dev/input is empty — no keyboard, mouse or panel"
	note "the keyboard map still drives playback; pointing needs a device."
fi

# The second of the two defences around rbp's i.MX6 register mappings
# (fix-dev.sh:123); audioshim's mmap interposition is the first.
if [ -e /dev/mem ]; then
	_dm=$(stat -c '%a' /dev/mem 2>/dev/null || echo '?')
	if [ "$_dm" = "0" ]; then
		ok "/dev/mem mode 000 (rbp cannot reach real peripherals)"
	else
		warn "/dev/mem mode is $_dm, not 000"
		note "fix-dev.sh sets this at every start; a value here means the launcher"
		note "has not run since the last boot."
		fix "chmod 000 /dev/mem"
	fi
fi

# --- 6. what fix-dev.sh is supposed to have put in place --------------------

hdr 6 "binds, stubs and FIFOs (fix-dev.sh's output)"
if [ -d "$RB_CHROOT" ]; then
	for d in dev proc sys tmp; do
		if is_mount "$RB_CHROOT/$d"; then
			ok "$RB_CHROOT/$d is a mount"
		else
			bad "$RB_CHROOT/$d is NOT mounted"
			note "rbp cannot open /tmp (the shim rendezvous), /dev or /proc without it."
			fix "sh $RB_DEPLOY_ROOT/fix-dev.sh"
		fi
	done

	# The FIFOs: rbp polls these at full speed, and a regular file makes the poll
	# return immediately and forever, pegging a core under RT priority 98
	# (fix-dev.sh:95-105). So the check is the file type, not the presence.
	for d in subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0; do
		if [ -p "$RB_CHROOT/dev/$d" ]; then
			:
		elif [ -e "$RB_CHROOT/dev/$d" ]; then
			bad "$RB_CHROOT/dev/$d exists but is not a FIFO"
			fix "sh $RB_DEPLOY_ROOT/fix-dev.sh"
		else
			warn "$RB_CHROOT/dev/$d is missing"
		fi
	done
	[ -p "$RB_CHROOT/dev/subucom_spi1.0" ] && ok "the SPI/hid FIFOs are in place"

	# paudiog0 must NOT exist: with it present JUCE takes the USB-gadget-audio
	# path and issues gadget ioctls this kernel does not have (fix-dev.sh:119).
	if [ -e "$RB_CHROOT/dev/paudiog0" ]; then
		bad "$RB_CHROOT/dev/paudiog0 exists — JUCE will take the gadget-audio path"
		fix "rm -f $RB_CHROOT/dev/paudiog0"
	else
		ok "no paudiog0 (the gadget-audio path is closed)"
	fi

	# glibc 2.13 reads /etc/mtab as a regular file; it has to point at the real
	# mount table or the chroot sees the tarball's stale copy (fix-dev.sh:141).
	if [ -L "$RB_CHROOT/etc/mtab" ]; then
		ok "$RB_CHROOT/etc/mtab -> $(readlink "$RB_CHROOT/etc/mtab")"
	else
		warn "$RB_CHROOT/etc/mtab is not a symlink to /proc/mounts"
		fix "sh $RB_DEPLOY_ROOT/fix-dev.sh"
	fi
fi

# Both slots have their own FIFO: rbp opens all four (the two mount ones and the
# two it must never be sent "connect" on) at startup, and a missing one means that
# slot can never be told anything.
for _f in 1 2; do
	if [ -p "/tmp/udev_usb$_f" ]; then
		ok "/tmp/udev_usb$_f FIFO present (this is how rbp hears about slot $_f)"
	else
		bad "/tmp/udev_usb$_f is missing or not a FIFO — rbp will never see slot $_f's media"
		fix "sh $RB_DEPLOY_ROOT/fix-dev.sh"
	fi
done

# --- 7. the service, and what is ACTUALLY running ---------------------------

hdr 7 "the service, and the build actually loaded"
if [ -f /etc/systemd/system/rblive4.service ]; then
	_um=$(stat -c '%a' /etc/systemd/system/rblive4.service 2>/dev/null || echo '?')
	if [ "$_um" = "644" ]; then
		ok "rblive4.service installed, mode 644"
	else
		warn "rblive4.service mode is $_um (644 is what install.sh sets)"
	fi
else
	bad "/etc/systemd/system/rblive4.service is not installed"
	fix "sh $RB_DEPLOY_ROOT/install.sh   # or: systemctl enable --now rblive4"
fi

if command -v systemctl >/dev/null 2>&1; then
	_en=$(systemctl is-enabled rblive4.service 2>/dev/null || echo unknown)
	if [ "${RB_AUTOSTART:-1}" = "1" ]; then
		case "$_en" in
			enabled) ok "enabled (RB_AUTOSTART=1) — the player starts at boot";;
			*)       bad "RB_AUTOSTART=1 in rb.conf but the unit is '$_en' — it will not start at boot"
			         fix "systemctl enable rblive4";;
		esac
	else
		case "$_en" in
			enabled) warn "RB_AUTOSTART=${RB_AUTOSTART} but the unit is enabled";;
			*)       ok "disabled (RB_AUTOSTART=${RB_AUTOSTART})";;
		esac
	fi
fi

# pids_matching, not `ps | awk`: the same lookup start-rb.sh uses (start-rb.sh:269),
# so doctor and the launcher cannot disagree about which process is rbp. The
# empty-pattern guard is not decoration: pids_matching() treats its argument as
# a glob, and an empty one matches every process on the machine.
if [ -z "${RB_PLAYER:-}" ]; then
	bad "RB_PLAYER is empty in $RB_CONF_FILE"
	note "the launcher uses it to find and stop the player; empty, it matches"
	note "every process on the unit."
	RBPID=""
else
	RBPID=$(pids_matching "$RB_PLAYER" 2>/dev/null | head -1)
fi
if [ -z "$RBPID" ]; then
	warn "rbp is not running (${RB_PLAYER:-?})"
	note "nothing below can report a loaded build; start it with"
	note "  systemctl start rblive4"
else
	_started=$(stat -c '%y' "/proc/$RBPID" 2>/dev/null | cut -d. -f1 || echo '?')
	ok "rbp is running: pid $RBPID, started $_started"

	# THE question this tree has most often got wrong. /proc/<pid>/maps names the
	# file by its path on THIS filesystem — the chroot is a chroot, not a mount
	# namespace — so the line matches the $RB_CHROOT path directly.
	if [ -r "/proc/$RBPID/maps" ]; then
		for p in $(echo "${RB_LD_PRELOAD:-}" | tr ':' ' '); do
			_host="$RB_CHROOT$p"
			if grep -qF "$_host" "/proc/$RBPID/maps" 2>/dev/null; then
				ok "$(basename "$p") is loaded ($(_sum "$_host"))"
			else
				bad "$(basename "$p") is NOT loaded into the running rbp"
			fi
		done
	fi
fi

# --- 8. the media -----------------------------------------------------------

hdr 8 "media"
# Both slots, derived the same way usb-watch.sh derives them (usb-watch.sh:59-69).
# rbp has two media devices and either may be empty, so an empty slot is not a
# fault on its own; a slot that is mounted but not bound is.
media_check() {
	mslot=$1
	if [ "$mslot" = 2 ]; then
		mmnt="${RB_MEDIA_MOUNT2:-$RB_DEPLOY_ROOT/media/usb2}/sda1"
		mchmnt="$RB_CHROOT${RB_CHROOT_MEDIA2:-/media/usb2/sda1}"
	else
		mmnt="$MNT"
		mchmnt="$CH_MNT"
	fi
	if is_mount "$mmnt"; then
		ok "slot $mslot stick mounted host-side: $mmnt"
	else
		note "slot $mslot: nothing mounted at $mmnt (normal if nothing is plugged in)"
	fi
	if is_mount "$mchmnt"; then
		ok "slot $mslot bound into the chroot: $mchmnt (rbp's view of the media)"
	elif is_mount "$mmnt"; then
		bad "slot $mslot: $mmnt is mounted but $mchmnt is not — rbp cannot see the stick"
		fix "sh $RB_DEPLOY_ROOT/usb-watch.sh   # it owns both mounts"
	else
		note "slot $mslot: not bound (nothing to bind)"
	fi
}
media_check 1
media_check 2

# --- 9. the controller table ------------------------------------------------

hdr 9 "controller"

# The table (scripts/shims/controllers.c) is the one place a control surface's
# map, its sequencer port-name hint, its ALSA card id and its USB id are written
# down together. Every value below is read out of it through controllers_cli,
# which links the PRODUCTION controllers.c -- so this script keeps no copy of any
# surface's name. A copy here would be in shell, where nothing checks it, and it
# would be the fourth place one device is named, which is the drift the table
# exists to remove.
#
# What is checked is AGREEMENT, not correctness: the table is the source of
# truth, so the only question left is whether what is configured on this unit
# still points at the surface the table says is selected. A disagreement is a
# warn and not a fail, because a bench deliberately wired to a different surface
# is legitimate -- it is the SILENT version of it that costs an afternoon.
CLI="$RB_DEPLOY_ROOT/controllers_cli"

if [ ! -x "$CLI" ]; then
	warn "no controller table CLI at $CLI"
	note "nothing rbp loads is missing -- this is a diagnostic. Without it the"
	note "one place the surface's names are written down cannot be read here."
	fix "make -C scripts/shims controllers_cli && scp scripts/shims/controllers_cli root@<unit>:$RB_DEPLOY_ROOT/"
else
	# `detect` matches the USB ids in sysfs against the table's rows. It is NOT
	# how the shim finds its surface -- the shim matches the ALSA sequencer PORT
	# NAME -- so what this answers is "what is plugged in", which is the other
	# half of the comparison below. Its exit status is a three-way contract and
	# collapsing it would report "no controller" on a machine whose /sys simply
	# could not be read: 0 matched, 1 read and nothing matched, 2 not read.
	att_id=""
	attached=$("$CLI" detect 2>/dev/null)
	case $? in
	0)
		att_id=$(printf '%s\n' "$attached" | head -1 | cut -d' ' -f1)
		ok "attached: $(printf '%s' "$attached" | tr '\n' ' ')"
		;;
	2)
		warn "$CLI detect could not read /sys/bus/usb/devices"
		note "nothing below can compare the selection against a device."
		;;
	*)
		note "no controller attached (no row's USB id is on this machine)"
		note "normal on a bench with nothing plugged in, and on a unit whose"
		note "surface is the keyboard: MIDI_MAP=kbd needs no device."
		;;
	esac

	# `ref` is the row the settings below are compared against: the row MIDI_MAP
	# names when it names one, and otherwise the attached controller's -- because
	# kbd and none are MAPS, not controllers, and under either the shim still
	# looks for the default surface by name.
	sel="${RB_MIDI_MAP:-}"
	ref="$sel"
	if [ -n "$sel" ] &&
	   ! "$CLI" names 2>/dev/null | awk '{print $1}' | grep -Fxq "$sel"; then
		ref=""
	fi
	[ -n "$ref" ] || ref="$att_id"

	if [ -z "$sel" ]; then
		ok "MIDI_MAP is empty: the table's default row selects the surface"
	elif [ "$ref" = "$sel" ]; then
		ok "MIDI_MAP=$sel is a row in this table"
	else
		note "MIDI_MAP=$sel names no controller row (kbd and none are maps, by design)"
	fi

	if [ -n "$att_id" ] && [ -n "$ref" ]; then
		if [ "$ref" = "$att_id" ]; then
			ok "the attached controller '$att_id' is the one the settings below are read against"
		else
			warn "MIDI_MAP selects '$ref', but '$att_id' is the controller attached"
			note "legitimate on a bench, and a fault on a unit that should play:"
			note "the shim waits for a surface that is not here, and the log says"
			note "which name it is waiting for."
		fi
	fi

	# The two values the table supplies, asked for once. An empty answer means
	# the table records none for that row, which is not a failure to report --
	# the JP21 row's hint and card id are recorded as UNKNOWN rather than
	# guessed, and that is the honest state of this port's knowledge.
	hint=""
	card=""
	if [ -n "$ref" ]; then
		hint=$("$CLI" match "$ref" 2>/dev/null) || hint=""
		card=$("$CLI" card-id "$ref" 2>/dev/null) || card=""
	fi

	# The port-name hint. The shim uses it as a case-insensitive SUBSTRING of the
	# sequencer's port name; empty means "the row's own hint", which is the
	# shipped default and the thing that keeps detection and the mapping in step.
	case "${RB_MIDI_IN_MATCH:-}" in
	"")
		if [ -n "$hint" ]; then
			ok "MIDI_IN_MATCH is empty -> the table's hint for '$ref' is '$hint'"
		elif [ -n "$ref" ]; then
			note "MIDI_IN_MATCH is empty and the table records no hint for '$ref'"
			note "nothing is matched by name, so the rawmidi route takes over."
		else
			note "MIDI_IN_MATCH is empty: the selected row's hint is used (nothing"
			note "is attached here to name it against)."
		fi
		;;
	*)
		if [ -z "$ref" ]; then
			note "MIDI_IN_MATCH=${RB_MIDI_IN_MATCH} (nothing attached to compare it against)"
		elif [ -z "$hint" ]; then
			note "the table records no port hint for '$ref', so MIDI_IN_MATCH="
			note "${RB_MIDI_IN_MATCH} is an override this unit has to answer for."
		elif [ "$RB_MIDI_IN_MATCH" = "$hint" ]; then
			ok "MIDI_IN_MATCH=${RB_MIDI_IN_MATCH} is the table's hint for '$ref'"
		else
			warn "MIDI_IN_MATCH=${RB_MIDI_IN_MATCH}, but the table's hint for '$ref' is '$hint'"
			note "the shim looks for the port name set here, so a surface that is"
			note "never found is this line rather than the device."
		fi
		;;
	esac

	# The audio card. rb.conf used to write this down on its own, with nothing
	# tying it to the surface selected above -- so a bench that switched surface
	# left audio pointed at the old card, silently. Empty now means the table's.
	dev_card=""
	if [ -n "${RB_AUDIO_DEV:-}" ]; then
		dev_card=$(printf '%s\n' "$RB_AUDIO_DEV" |
			sed -n 's/.*CARD=\([^,]*\).*/\1/p')
	fi
	case "${RB_AUDIO_DEV:-}" in
	"")
		if [ -n "$card" ]; then
			ok "AUDIO_DEV is empty -> the table's card for '$ref' is hw:CARD=$card,DEV=0"
		elif [ -n "$ref" ]; then
			note "AUDIO_DEV is empty and the table records no card id for '$ref'"
			note "the default row's card is used instead, so set AUDIO_DEV by hand"
			note "on a bench whose surface the table cannot name a card for."
		else
			note "AUDIO_DEV is empty: the selected row's card is used (nothing is"
			note "attached here to name it against)."
		fi
		;;
	*)
		if [ -z "$dev_card" ]; then
			note "AUDIO_DEV=$RB_AUDIO_DEV names its card by index, so the table's"
			note "card id cannot be compared with it."
		elif [ -z "$ref" ]; then
			note "AUDIO_DEV=$RB_AUDIO_DEV (nothing attached to compare it against)"
		elif [ -z "$card" ]; then
			note "the table records no card id for '$ref', so AUDIO_DEV=$RB_AUDIO_DEV"
			note "is an override this unit has to answer for."
		elif [ "$dev_card" = "$card" ]; then
			ok "AUDIO_DEV's card '$card' is the table's card for '$ref'"
		else
			warn "AUDIO_DEV names card '$dev_card'; the table's card for '$ref' is '$card'"
			note "expected on a bench wired to a card the table does not name; on a"
			note "unit that plays, one of the two is stale."
		fi
		;;
	esac

	# CTRL_KEEPALIVE is the one setting here that is not a de-duplication: it
	# gates the vendor SysEx the surface's row carries (see rb.conf). All that can
	# be checked from here is that rb.conf can REACH it -- start-rb.sh exports
	# every name in its SHIM_VARS list and nothing else, so a knob missing from
	# that list is read by nobody and says nothing at all (docs/08-controls.md).
	case "${RB_CTRL_KEEPALIVE:-1}" in
	0) note "CTRL_KEEPALIVE=0: no keepalive is sent to the surface" ;;
	*) ok "CTRL_KEEPALIVE=${RB_CTRL_KEEPALIVE:-1}: the selected surface's keepalive is sent" ;;
	esac
	if [ -f "$RB_DEPLOY_ROOT/start-rb.sh" ]; then
		# The list is the quoted block between the SHIM_VARS=" line and the first
		# line that is exactly a quote (start-rb.sh:183-199). -w so a name inside
		# a longer one cannot pass for it.
		if sed -n '/^SHIM_VARS="/,/^"/p' "$RB_DEPLOY_ROOT/start-rb.sh" 2>/dev/null |
		   grep -qw 'CTRL_KEEPALIVE'; then
			ok "CTRL_KEEPALIVE is in start-rb.sh's SHIM_VARS list"
		else
			warn "CTRL_KEEPALIVE is not in $RB_DEPLOY_ROOT/start-rb.sh's SHIM_VARS"
			note "start-rb.sh exports the names in that list and no others, so the"
			note "value in rb.conf would be read by nobody. Re-deploy the pair."
			fix "scp scripts/device/start-rb.sh root@<unit>:$RB_DEPLOY_ROOT/"
		fi
	fi
fi

# --- summary ----------------------------------------------------------------

echo
if [ "$FAILS" -eq 0 ] && [ "$WARNS" -eq 0 ]; then
	say "no problems found."
	exit 0
fi

say "$FAILS failure(s), $WARNS warning(s)."

if [ -n "$FIX" ]; then
	echo
	echo "To fix (paste):"
	# printf, not echo: this is the one place a line can contain a backslash
	# (the rb.local.conf line ends in \n), and dash's and bash-in-POSIX-mode's
	# echo both expand escapes, which would break it across two lines.
	printf '%s\n' "$FIX"
fi

echo
[ "$FAILS" -eq 0 ] || exit 1
exit 0
