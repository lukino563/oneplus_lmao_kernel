/*
 * drivers/param_read_write/param_read_write.c
 * Fake param_read_write Avengers theme
 * Made by pappschlumpf (Erik Müller)
 */

#include <linux/set_os.h>

#include <linux/init.h>
#include <linux/moduleparam.h>

static bool oos_detected = false;

bool is_oos(void) {
	return oos_detected;
}

void set_os(bool os)
{
	oos_detected = os;
}
