
MAGISKHIDE_SUPPORT_VERSION=1

if [ "$(magisk magiskhide version | sed "s/MAGISKHIDE://")" -lt $MAGISKHIDE_SUPPORT_VERSION ]; then
    touch "${0%/*}/disable"
fi

# Enhanced mode not support sulist yet
if magisk magiskhide sulist; then
    touch "${0%/*}/disable"
fi