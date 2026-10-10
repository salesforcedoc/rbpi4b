/*
 * usb_devices.h -- what is on the USB bus, matched against the controller table.
 *
 * WHY THIS EXISTS. Two callers ask the same question and must never disagree.
 * controllers_cli's `detect` prints every recognised device so a diagnostic can compare
 * what is PLUGGED IN against what was SELECTED -- the drift the table itself exists to
 * make impossible to miss. And RB_MIXER_MODE=external asks whether an external digital
 * mixer is on the bus at all, because handing the mixing to a device's channel strips is
 * the only thing that mode is for: with a DDJ-FLX4 and no mixer, the decks would go out on
 * pairs nothing is listening to and the mix -- which is what the FLX4's own output carries
 * -- would have been dropped. So external routing APPLIES only while one of
 * controllers.h's mixer rows is here, and the config page does not offer the setting
 * while none of them is.
 *
 * It was one walk inside the CLI first; it lives here now so that the presence check and
 * the diagnostic cannot drift apart about what is plugged in.
 *
 * sysfs directly rather than `lsusb`, so this works on a unit where lsusb is not installed.
 * `root` is a parameter rather than a constant so a test can point the walk at a fixture
 * tree instead of the running kernel's bus -- the whole function is a directory walk and a
 * table lookup, and both halves are then testable without hardware.
 */
#ifndef RBPI4B_USB_DEVICES_H
#define RBPI4B_USB_DEVICES_H

#include "controllers.h"

/* The kernel's view of the bus: one directory per device, each with idVendor, idProduct
 * and product. The one string in this tree that names it. */
#define USB_DEVICES_ROOT "/sys/bus/usb/devices"

/* Called once per recognised device, in walk order. Return NONZERO to stop the walk --
 * that is how a caller that wants the first match rather than the whole list asks. */
typedef int (*usb_match_fn)(const struct controller *c, void *ctx);

/* Walk `root`, match each device against the table -- by USB id first, then by model name
 * for the rows that record one -- and call `fn` for each match. Returns the number of
 * matches, or -1 when `root` could not be read at all. The distinction is one a caller
 * acts on, which is why it is a return value and not a log line.
 *
 * A device that is present but matches no row is NOT reported: this answers "which of the
 * things this build knows about are here", not "what is here". */
int usb_table_walk_in(const char *root, usb_match_fn fn, void *ctx);

/* The external digital mixer on the bus, or NULL -- the check RB_MIXER_MODE=external
 * applies and the one the config page makes before offering the setting. NULL also when
 * the bus cannot be read, so a caller that must tell the two apart should walk the table
 * itself rather than read a NULL as an answer. */
const struct controller *mixer_present_in(const char *root);
const struct controller *mixer_present(void);

#endif /* RBPI4B_USB_DEVICES_H */
