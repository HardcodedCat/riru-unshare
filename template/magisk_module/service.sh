MODDIR="${0%/*}"
MODNAME="${MODDIR##*/}"
MAGISKTMP="$(magisk --path)" || MAGISKTMP=/sbin

if [ ! -f "$MAGISKTMP/.magisk/mirror/sepolicy.rules/$MODNAME/sepolicy.rule" ]; then
    magiskpolicy --live --apply "$MODDIR/sepolicy.rule"
fi