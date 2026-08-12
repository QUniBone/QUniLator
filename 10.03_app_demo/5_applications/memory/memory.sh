#!/root/10.03_app_demo/4_deploy/demo --verbose
# inputfile for demo to just emulate max memory
d			# device menu


pwr			# reboot PDP-11
.wait 3000		# wait for PDP-11 to reset
m i			# install max memory
