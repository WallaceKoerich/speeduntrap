#!/bin/sh
#
# One-shot installer for speeduntrap auto-start on NBT iDrive 4.
# Run once from SSH, then:  shutdown -S reboot
#
#   chmod 775 /fs/sda0/install_boot.sh /fs/sda0/start_speeduntrap.sh /fs/sda0/speeduntrap
#   /fs/sda0/install_boot.sh
#
# Edits the FLASH variant file, not the /opt overlay symlink:
#   /opt/sys/etc/starter.cfg -> starter.cfg.0x10ee (or .0x1c42)
#   real file: /fs/sda0/opt/sys/etc/starter.cfg.0x10ee
# Name must be /opt/sys/bin/start_speeduntrap.sh (OEM never execs /fs/sda0).
# Adds a HostsProcess next to HmiMain3D.
# OnTerminate IGNORE_ALL / FinalAction IGNORE — a crash must NOT reboot the HU.
# Does NOT touch /fs/sda0/repository/istep/...
# NBT /bin has no tee, head, awk, expr, od, or wc.
#

export PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH

BAK=/fs/sda0/starter.cfg.speeduntrap.bak
BROKEN=/fs/sda0/starter.cfg.speeduntrap.broken
TMP=/fs/sda0/starter.cfg.speeduntrapnew
INS=/fs/sda0/starter_process_speeduntrap.xml
SH=/fs/sda0/start_speeduntrap.sh
BIN=/fs/sda0/speeduntrap
OPTSH=/opt/sys/bin/start_speeduntrap.sh
OPTSH_SDA=/fs/sda0/opt/sys/bin/start_speeduntrap.sh
LOG=/fs/sda0/speeduntrap_boot.log
LOGSHM=/dev/shmem/speeduntrap_boot.log
CFG=

logmsg() {
    echo "$1"
    echo "$1" >> "$LOG" 2>/dev/null
    echo "$1" >> "$LOGSHM" 2>/dev/null
}

inc() {
    n=$1
    out=""
    carry=1
    while [ -n "$n" ]
    do
        last=`echo "$n" | sed 's/.*\(.\)$/\1/'`
        n=`echo "$n" | sed 's/.$//'`
        if [ "$carry" -eq 1 ]; then
            case "$last" in
            0) last=1; carry=0 ;;
            1) last=2; carry=0 ;;
            2) last=3; carry=0 ;;
            3) last=4; carry=0 ;;
            4) last=5; carry=0 ;;
            5) last=6; carry=0 ;;
            6) last=7; carry=0 ;;
            7) last=8; carry=0 ;;
            8) last=9; carry=0 ;;
            9) last=0; carry=1 ;;
            *) return 1 ;;
            esac
        fi
        out="${last}${out}"
    done
    if [ "$carry" -eq 1 ]; then
        out="1${out}"
    fi
    echo "$out"
}

isnum() {
    echo "$1" | grep '^[0-9][0-9]*$' >/dev/null 2>&1
}

cfg_broken() {
    grep '<ProcessCount></ProcessCount>' "$1" >/dev/null 2>&1 && return 0
    grep '<Number></Number>' "$1" >/dev/null 2>&1 && return 0
    pc=`sed -n 's/.*<ProcessCount>\([0-9][0-9]*\)<\/ProcessCount>.*/\1/p' "$1"`
    set -- $pc
    [ -n "$1" ] || return 0
    return 1
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

proc_num_for() {
    needle=$1
    lastn=0
    found=""
    while IFS= read line
    do
        case "$line" in
        *"<Number>"*)
            n=`echo "$line" | sed -n 's/.*<Number>\([0-9][0-9]*\)<\/Number>.*/\1/p'`
            if isnum "$n"; then
                lastn=$n
            fi
            ;;
        *"$needle"*)
            found=$lastn
            ;;
        esac
    done < "$CFG"
    echo "$found"
}

add_hosts() {
    hmi=$1
    launch=$2
    if grep "<HostsProcess>$launch</HostsProcess>" "$CFG" >/dev/null 2>&1; then
        return 0
    fi
    logmsg "$0: HostsProcess $launch next to HmiMain3D ($hmi) in HMI_APPLICATION"
    sed "s/<HostsProcess>$hmi<\/HostsProcess>/<HostsProcess>$hmi<\/HostsProcess><HostsProcess>$launch<\/HostsProcess>/" "$CFG" > "$TMP" || return 1
    if grep "<HostsProcess>$launch</HostsProcess>" "$TMP" >/dev/null 2>&1; then
        :
    else
        logmsg "$0: HostsProcess insert failed"
        rm -f "$TMP"
        return 1
    fi
    cp "$TMP" "$CFG" || return 1
    rm -f "$TMP"
    return 0
}

install_opt_wrapper() {
    if [ -d /fs/sda0/opt/sys/bin ]; then
        cp "$SH" "$OPTSH_SDA" || return 1
        chmod 775 "$OPTSH_SDA" || return 1
        logmsg "$0: wrote $OPTSH_SDA"
    fi
    cp "$SH" "$OPTSH" 2>/dev/null
    chmod 775 "$OPTSH" 2>/dev/null
    if [ -x "$OPTSH" ]; then
        logmsg "$0: wrapper at $OPTSH"
        return 0
    fi
    if [ -x "$OPTSH_SDA" ]; then
        logmsg "$0: wrapper at $OPTSH_SDA (opt path not visible yet)"
        return 0
    fi
    logmsg "$0: could not install $OPTSH"
    return 1
}

pick_cfg
logmsg "$0: start `date`"
logmsg "$0: using $CFG"

ensure_rw || exit 1

if [ ! -f "$CFG" ]; then
    logmsg "$0: missing $CFG"
    exit 1
fi
if [ ! -x "$SH" ]; then
    logmsg "$0: missing or not executable: $SH"
    exit 1
fi
if [ ! -x "$BIN" ]; then
    logmsg "$0: missing or not executable: $BIN"
    exit 1
fi

HMI_NUM=`proc_num_for HmiMain3D`
if isnum "$HMI_NUM"; then
    :
else
    HMI_NUM=42
fi
logmsg "$0: HmiMain3D process Number $HMI_NUM"

if grep start_speeduntrap.sh "$CFG" >/dev/null 2>&1 || grep /fs/sda0/speeduntrap "$CFG" >/dev/null 2>&1; then
    if cfg_broken "$CFG"; then
        logmsg "$0: previous install left empty ProcessCount/Number — restoring $BAK"
        cp "$CFG" "$BROKEN" 2>/dev/null
        if [ ! -f "$BAK" ]; then
            logmsg "$0: missing $BAK — do not reboot. restore starter.cfg first"
            exit 1
        fi
        if cfg_broken "$BAK"; then
            logmsg "$0: backup is also broken — do not reboot"
            exit 1
        fi
        cp "$BAK" "$CFG" || exit 1
        logmsg "$0: restored $CFG from $BAK"
    else
        logmsg "$0: existing speeduntrap line:"
        grep start_speeduntrap.sh "$CFG"
        patched=0
        install_opt_wrapper || exit 1
        if grep /fs/sda0/speeduntrap "$CFG" >/dev/null 2>&1; then
            logmsg "$0: retargeting Name to $OPTSH"
            sed 's|/fs/sda0/speeduntrap|/opt/sys/bin/start_speeduntrap.sh|' "$CFG" > "$TMP" || exit 1
            cp "$TMP" "$CFG" || exit 1
            rm -f "$TMP"
            patched=1
        fi
        if grep /fs/sda0/start_speeduntrap.sh "$CFG" >/dev/null 2>&1; then
            logmsg "$0: retargeting Name to $OPTSH"
            sed 's|/fs/sda0/start_speeduntrap.sh|/opt/sys/bin/start_speeduntrap.sh|' "$CFG" > "$TMP" || exit 1
            cp "$TMP" "$CFG" || exit 1
            rm -f "$TMP"
            patched=1
        fi
        LAUNCH_NUM=`proc_num_for start_speeduntrap.sh`
        if isnum "$LAUNCH_NUM"; then
            if grep "<HostsProcess>$LAUNCH_NUM</HostsProcess>" "$CFG" >/dev/null 2>&1; then
                :
            else
                add_hosts "$HMI_NUM" "$LAUNCH_NUM" || exit 1
                patched=1
            fi
        fi
        grep start_speeduntrap.sh "$CFG"
        if [ "$patched" -eq 1 ]; then
            logmsg "$0: patched. reboot: shutdown -S reboot"
            exit 0
        fi
        logmsg "$0: already installed in $CFG (Name $OPTSH, HostsProcess $LAUNCH_NUM)"
        exit 0
    fi
fi

if [ ! -f "$BAK" ]; then
    if cfg_broken "$CFG"; then
        logmsg "$0: no backup and $CFG is broken — do not reboot"
        exit 1
    fi
    cp "$CFG" "$BAK" || exit 1
    logmsg "$0: backup $BAK"
fi

COUNT=`sed -n 's/.*<ProcessCount>\([0-9][0-9]*\)<\/ProcessCount>.*/\1/p' "$CFG"`
set -- $COUNT
COUNT=$1
if isnum "$COUNT"; then
    :
else
    logmsg "$0: could not read ProcessCount"
    exit 1
fi

MAXN=0
inpt=0
while IFS= read line
do
    case "$line" in
    *"<ProcessTable>"*)
        inpt=1
        continue
        ;;
    *"</ProcessTable>"*)
        inpt=0
        continue
        ;;
    esac
    if [ "$inpt" -eq 1 ]; then
        case "$line" in
        *"<Number>"*)
            n=`echo "$line" | sed -n 's/.*<Number>\([0-9][0-9]*\)<\/Number>.*/\1/p'`
            if isnum "$n"; then
                if [ "$n" -gt "$MAXN" ]; then
                    MAXN=$n
                fi
            fi
            ;;
        esac
    fi
done < "$CFG"

if [ "$COUNT" -gt 1 ]; then
    if [ "$MAXN" -lt 1 ]; then
        logmsg "$0: could not read process Number in ProcessTable"
        exit 1
    fi
fi

NEWCOUNT=`inc "$COUNT"` || exit 1
NEWNUM=`inc "$MAXN"` || exit 1
if isnum "$NEWCOUNT" && isnum "$NEWNUM"; then
    :
else
    logmsg "$0: increment failed  COUNT=$COUNT MAXN=$MAXN"
    exit 1
fi

logmsg "$0: ProcessCount $COUNT -> $NEWCOUNT  Number $NEWNUM"

install_opt_wrapper || exit 1

cat > "$INS" << EOF
    <Process>
      <Number>$NEWNUM</Number>
      <Name>/opt/sys/bin/start_speeduntrap.sh</Name>
      <Args/>
      <ResArgs/>
      <Prio>10</Prio>
      <Partition>softrt_SYS</Partition>
      <StartParam>BACKGROUND</StartParam>
      <OnTerminate>IGNORE_ALL</OnTerminate>
      <Shutdown>IGNORE</Shutdown>
      <ShutdownThread>-1</ShutdownThread>
      <KeepInterface>true</KeepInterface>
      <MaxProcessRestarts>0</MaxProcessRestarts>
      <FinalAction>IGNORE</FinalAction>
      <VariantCode>-1</VariantCode>
      <VariantMask>-1</VariantMask>
      <ProvidesInterface/>
      <RequiresInterface/>
      <RequiresEnvironment/>
    </Process>
EOF

rm -f "$TMP"
didins=0
didcnt=0
didhost=0
while IFS= read line
do
    case "$line" in
    *"<ProcessCount>"*)
        echo "$line" | sed "s/<ProcessCount>[0-9][0-9]*<\/ProcessCount>/<ProcessCount>$NEWCOUNT<\/ProcessCount>/" >> "$TMP"
        didcnt=1
        continue
        ;;
    *"<HostsProcess>$HMI_NUM</HostsProcess>"*)
        echo "$line" >> "$TMP"
        if [ "$didhost" -eq 0 ]; then
            echo "      <HostsProcess>$NEWNUM</HostsProcess>" >> "$TMP"
            didhost=1
        fi
        continue
        ;;
    *"</ProcessTable>"*)
        if [ "$didins" -eq 0 ]; then
            cat "$INS" >> "$TMP"
            didins=1
        fi
        echo "$line" >> "$TMP"
        continue
        ;;
    esac
    echo "$line" >> "$TMP"
done < "$CFG"

if [ "$didins" -eq 0 ]; then
    logmsg "$0: no ProcessTable close, leaving $CFG unchanged"
    rm -f "$TMP" "$INS"
    exit 1
fi
if [ "$didcnt" -eq 0 ]; then
    logmsg "$0: ProcessCount bump failed, leaving $CFG unchanged"
    rm -f "$TMP" "$INS"
    exit 1
fi
if grep /opt/sys/bin/start_speeduntrap.sh "$TMP" >/dev/null 2>&1; then
    :
else
    logmsg "$0: insert failed, leaving $CFG unchanged"
    rm -f "$TMP" "$INS"
    exit 1
fi
if grep "<ProcessCount>$NEWCOUNT</ProcessCount>" "$TMP" >/dev/null 2>&1; then
    :
else
    logmsg "$0: ProcessCount bump failed, leaving $CFG unchanged"
    rm -f "$TMP" "$INS"
    exit 1
fi
if grep "<Number>$NEWNUM</Number>" "$TMP" >/dev/null 2>&1; then
    :
else
    logmsg "$0: Number insert failed, leaving $CFG unchanged"
    rm -f "$TMP" "$INS"
    exit 1
fi
if grep "<HostsProcess>$NEWNUM</HostsProcess>" "$TMP" >/dev/null 2>&1; then
    :
else
    logmsg "$0: HMI_APPLICATION HostsProcess insert failed, leaving $CFG unchanged"
    rm -f "$TMP" "$INS"
    exit 1
fi
if cfg_broken "$TMP"; then
    logmsg "$0: temp cfg looks broken, leaving $CFG unchanged"
    rm -f "$TMP" "$INS"
    exit 1
fi

cp "$TMP" "$CFG" || exit 1
rm -f "$TMP" "$INS"
if cfg_broken "$CFG"; then
    logmsg "$0: write looked broken, restoring $BAK — do not reboot"
    cp "$BAK" "$CFG"
    exit 1
fi
logmsg "$0: wrote $CFG  grep:"
grep /opt/sys/bin/start_speeduntrap.sh "$CFG"
grep HostsProcess "$CFG" | grep "$NEWNUM"
grep ProcessCount "$CFG" | sed -n '1p'
logmsg "$0: done. reboot: shutdown -S reboot"
exit 0
