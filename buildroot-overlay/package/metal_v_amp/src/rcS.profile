#!/bin/sh

# Diagnostic replacement for /etc/init.d/rcS. It preserves Buildroot's
# execution rules while reporting the elapsed boot time around each service.
boottrace()
{
	read -r uptime unused < /proc/uptime
	message="BOOTTRACE uptime=${uptime}s $*"
	printf '%s\n' "$message" > /dev/console
	printf '<6>%s\n' "$message" > /dev/kmsg
}

boottrace "rcS begin"

for i in /etc/init.d/S??*; do
	# Ignore dangling symlinks (if any).
	[ ! -f "$i" ] && continue

	boottrace "start $i"
	case "$i" in
		*.sh)
			# Source shell scripts using the same subshell convention as rcS.
			(
				trap - INT QUIT TSTP
				set start
				. "$i"
			)
			status=$?
			;;
		*)
			"$i" start
			status=$?
			;;
	esac
	boottrace "done status=$status $i"
done

boottrace "rcS complete"
