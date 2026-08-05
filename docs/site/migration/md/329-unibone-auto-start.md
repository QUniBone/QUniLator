<!-- migrated from https://retrocmp.com/projects/unibone/329-unibone-auto-start -->

## Power-On Sequence

A PDP-11 or VAX with UniBone/QBone installed will not run, until QUniBone software closes the so-called "GRANT chain" for Interrupts and DMA transactions.

The Power-On sequence to get a PDP-11+QUniBone system up and running is:

1.  PDP-11 power is switched ON.
2.  PDP-11 shows interactive console monitor or tries to boot. This fails, because system not yet complete operational.
3.  Unibone/QBone onboard Linux boots and connects to network. This needs more than a minute ... (Linux gurus?)
4.  User logs in into QUniBone wie ssh as root/root. QUnibone software must be started and selected device emulation begins.\
    Now the PDP-11 system is complete, UNIBUS/QBUS DMA and Interrupt Grant signals are handled correctly.
5.  PDP-11 gets a 2nd Power-On signal by the starting emulation script, and can boot correctly this time.

Item 4 in this sequence can be automated by the "auto-start" feature, so no manual intervention is required too boot.

 

## Auto-Start - Running an Emulation after Power-On

In short:

- The special script "autostart.sh" runs always once after Linux reboot.
- Selected emulation scripts are automatically started after UniBone/QBone power-up.
- Up to 16 scripts can be selected via 4 onboard DIP switches.
- You can preset up to 16 run configurations by editing the script "autostart.sh", see below.
- Interactive ssh sessions may connect to the running application.

 

**The autostart sequence is:**

**1.** After BBB power-on, an automatic (but hidden) login session is started,\
   Shell scripts .profile and autostart.sh execute the script corresponding to the DIP switch settings, these encode a number 0 to 15.

When looking onto UniBone/QBone PCB front, switch arrangement is:

![unibone leds switches annotated](images/unibone-leds-switches-annotated.jpg)

 "Switch up"=0, "down" = 1. Example: switches = "down,down,up,down" =\> mirror =\> 1101 =\> 1+2+8 =\> run script \#11.\
\

**2.** the selected "demo" emulation confirms startup by echoing DIP switch settings to the LEDs.\
  So when LED == DIP switches, the emulation runs!

**3**. The emulation is now running in background.\
On further logins via ssh, you may connect to the terminal window of the running "demo" emulation (Emulation console).\
See "man screen" for what is happening.

 

**Editing autostart.sh**

Assign your run scripts to DIP switch values in autostart.sh, function get_config() :\
Your autostart.sh will not be erased on software update.

A sample file is given with all switch cases commented out:

 

`[...]`\
`####################################################################`\
`# get_config() {`\
`# Set up "config_info" and "config_cmd" for config "n"`\
`# Called applications ("demo") should echo "n" onto the LEDs on start.`\
`function get_config() {`\
`# param $1: selection number from switches`\
`# Adapt to your own needs!`\
`n=$1`\
`case $n in`\
`#1)`\
`#        config_info="Just memory"`\
`#        config_cmd="./memory.sh --leds $n"`\
`#        ;;`\
`#2)`\
`#        config_info="XXDP on RL1"`\
`#        config_cmd="./xxdp2.5_dl1.sh --leds $n"`\
`#        ;;`\
`#3)`\
`#        config_info="RT11 5.5 single on RL1"`\
`#        config_cmd="./rt11v5.5sj_dl1_34.sh --leds $n"`\
`#        ;;`\
`#4)`\
`#        config_info="11/34: RT11 5.5 FB on DU0:"`\
`#        config_cmd="./rt11v5.5fb_du0_34.sh --leds $n"`\
`#        ;;`\
`#5)`\
`#        config_info="11/34: RSX11M4.8 on DU0:"`\
`#        config_cmd="./rsx11m4.8_du0+rl_34.sh --leds $n"`\
`#        ;;`\
`#6)`\
`#        config_info="11/34: Unix V6 on RK05"`\
`#        config_cmd="./unixv6_dk0_34.sh --leds $n"`\
`#        ;;`\
`*)`\
`        config_info=`\
`        config_cmd=`\
`        ;;`\
`esac`\
`}`\
`[...]`\

 

 
