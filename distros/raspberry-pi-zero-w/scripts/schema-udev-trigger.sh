#!/bin/sh
/usr/bin/udevadm trigger --action=add || true
/usr/bin/udevadm settle --timeout=15 || true
