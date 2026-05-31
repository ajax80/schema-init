#!/bin/sh
udevadm trigger --subsystem-match=input
udevadm settle --timeout=5
