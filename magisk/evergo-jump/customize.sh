#!/sbin/sh
#
# Magisk module installer for evergo-jump.
#
# Installs the jump script, the prebuilt kernel module and the bundled FD, and
# the companion APK that puts an icon on the home screen.

SKIPUNZIP=0

ui_print "- Evergo UEFI Jump"
ui_print "- device: Redmi Note 11 5G (evergo, MT6833)"

# Refuse to install on anything else: the FD and the memory map are specific to
# this model, and running them elsewhere is how devices get bricked.
device=$(getprop ro.product.device)
if [ "$device" != "evergo" ]; then
  ui_print "! ro.product.device is '$device', expected 'evergo'"
  ui_print "! refusing to install on the wrong device"
  abort "! wrong device"
fi

ui_print "- unpacking"
unzip -o "$ZIPFILE" 'system/*' -d "$MODPATH" >&2
unzip -o "$ZIPFILE" 'evergo_stage1.ko' -d "$MODPATH" >&2
unzip -o "$ZIPFILE" 'fd/*' -d "$MODPATH" >&2
unzip -o "$ZIPFILE" 'EvergoJump.apk' -d "$MODPATH" >&2

set_perm_recursive "$MODPATH/system/bin" 0 0 0755 0755
set_perm "$MODPATH/evergo_stage1.ko" 0 0 0644
set_perm "$MODPATH/fd/evergo-hotjump.fd" 0 0 0644

# Install the companion APK as a user app so an icon appears in the launcher.
if [ -f "$MODPATH/EvergoJump.apk" ]; then
  ui_print "- installing the home-screen app"
  pm install -r "$MODPATH/EvergoJump.apk" >&2 || \
    ui_print "! could not install the app; run 'su -c evergo-jump' instead"
fi

ui_print "- done"
ui_print "- open 'Evergo Jump' from the app drawer to start the firmware"
ui_print "- a failed attempt is recovered by holding power for ~15 s"
