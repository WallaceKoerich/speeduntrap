#!/bin/sh
#
# Restore the starter.cfg backup taken by install_boot.sh.
# Does NOT touch /fs/sda0/repository/istep/...
# NBT /bin has no tee, head, awk, expr.
#

export PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH

BAK=/fs/sda0/starter.cfg.speeduntrap.bak
LOG=/fs/sda0/speeduntrap_boot.log
CFG=

logmsg() {
    echo "$1"
    echo "$1" >> "$LOG" 2>/dev/null
}

ensure_rw() {
    touch /fs/sda0/.speeduntrap_rw_test 2>/dev/null
    if [ -f /fs/sda0/.speeduntrap_rw_test ]; then
        rm -f /fs/sda0/.speeduntrap_rw_test
        return 0
    fi
    mount -o remount rw /fs/sda0/ 2>/dev/null
    mount -uw /fs/sda0 2>/dev/null
    touch /fs/sda0/.speeduntrap_rw_test 2>/dev/null
    if [ -f /fs/sda0/.speeduntrap_rw_test ]; then
        rm -f /fs/sda0/.speeduntrap_rw_test
        logmsg "$0: remounted /fs/sda0 read-write"
        return 0
    fi
    logmsg "$0: /fs/sda0 is read-only"
    return 1
}

pick_cfg() {
    var=`ls -l /opt/sys/etc/starter.cfg 2>/dev/null | sed -n 's/.*starter.cfg\.\(0x[0-9a-fA-F]*\).*/\1/p'`
    if [ -n "$var" ] && [ -f /fs/sda0/opt/sys/etc/starter.cfg.$var ]; then
        CFG=/fs/sda0/opt/sys/etc/starter.cfg.$var
        return 0
    fi
    if [ -f /fs/sda0/opt/sys/etc/starter.cfg.0x10ee ]; then
        CFG=/fs/sda0/opt/sys/etc/starter.cfg.0x10ee
        return 0
    fi
    if [ -f /fs/sda0/opt/sys/etc/starter.cfg.0x1c42 ]; then
        CFG=/fs/sda0/opt/sys/etc/starter.cfg.0x1c42
        return 0
    fi
    CFG=/opt/sys/etc/starter.cfg
}

ensure_rw || exit 1
pick_cfg
logmsg "$0: using $CFG"

if [ ! -f "$BAK" ]; then
    logmsg "$0: missing $BAK — nothing to restore"
    exit 1
fi
if [ ! -f "$CFG" ]; then
    logmsg "$0: missing $CFG"
    exit 1
fi

cp "$BAK" "$CFG" || exit 1
rm -f /opt/sys/bin/start_speeduntrap.sh /fs/sda0/opt/sys/bin/start_speeduntrap.sh
slay speeduntrap 2>/dev/null

logmsg "$0: restored $CFG from $BAK"
if grep start_speeduntrap.sh "$CFG" >/dev/null 2>&1; then
    logmsg "$0: WARNING: start_speeduntrap.sh still in cfg — backup was already patched"
    exit 1
fi
logmsg "$0: done. reboot: shutdown -S reboot"
exit 0
