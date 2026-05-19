#!/bin/sh
# reboot_system.sh
# Deferred system reboot triggered by HGW-Doctor.
# Syncs filesystems before rebooting to minimise data loss.

sync
sleep 2
reboot
