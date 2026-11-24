#!/bin/sh
#
# Runtime wrapper. starter.cfg execs THIS file at every boot — not the
# installer, and not speeduntrap directly.
#
# Put it at:  /opt/sys/bin/start_speeduntrap.sh
#             (flash: /fs/sda0/opt/sys/bin/)
# One-time:   /fs/sda0/install_boot.sh
#
# starter.cfg Name must be under /opt or /boot — OEM never execs /fs/sda0.
# This wrapper then execs /fs/sda0/speeduntrap.
# Do NOT slay HmiMain3D. Do NOT use FinalAction SHUTDOWN_SYSTEM.
# NBT /bin has no expr — do not use it.
#

export PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH
export LD_LIBRARY_PATH=/fs/sda0/lib:$LD_LIBRARY_PATH

LOG=/fs/sda0/speeduntrap_boot.log
LOGSHM=/dev/shmem/speeduntrap_boot.log
BIN=/fs/sda0/speeduntrap
HMI=/dev/shmem/HmiStarted

echo "$0: start `date`" >> "$LOG" 2>/dev/null
echo "$0: start `date`" >> "$LOGSHM" 2>/dev/null

if [ -e "$HMI" ]; then
    echo "$0: $HMI already present" >> "$LOG" 2>/dev/null
    echo "$0: $HMI already present" >> "$LOGSHM" 2>/dev/null
else
    echo "$0: waiting for $HMI" >> "$LOG" 2>/dev/null
    echo "$0: waiting for $HMI" >> "$LOGSHM" 2>/dev/null
    waitfor "$HMI" 1800
fi

# Let HmiMain3D (and the ConnectedDrive launcher, if present) claim SAS first.
sleep 12

if [ ! -x "$BIN" ]; then
    echo "$0: missing or not executable: $BIN" >> "$LOG" 2>/dev/null
    echo "$0: missing or not executable: $BIN" >> "$LOGSHM" 2>/dev/null
    exit 1
fi

echo "$0: exec $BIN" >> "$LOG" 2>/dev/null
echo "$0: exec $BIN" >> "$LOGSHM" 2>/dev/null
exec "$BIN"
