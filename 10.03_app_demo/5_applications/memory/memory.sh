#!/usr/bin/qbone-cli --verbose
# inputfile for the menu program to just emulate max memory
d			# device menu


pwr			# reboot PDP-11
.wait 3000		# wait for PDP-11 to reset
m i			# install max memory
