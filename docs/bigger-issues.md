# dashboard

- grid system with fixed grid size horizontally/vertically
- current configuration name and title shown, cog icon to go to configuration directly
- arrange front panel dip switches and leds horizontally
- console gets a "console" card
- all cards get same background/title
- cards can be dragged and dropped on the dashboard canvas, positions stored in config.  separate "edit" mode to rearrange and save/revert.  widgets can be hidden (eye icon, widget only shown in edit mode)
- verbal device status for disk drives needs to be shown on widget

# configuration

- no separate devices for each address.  
- address/irq as option dropdowns, conflicts checked which configuring.
- on/off options should use checkbox
- human friendly configuration title (editable)
- "stored" not needed, "current" as chip, "live" can go
- dip switch setting selects auto-loaded configuration - no default configuration, if dip switches set to value not corresponding to a configuration, load empty config (passive on bus)
- "Edits are staged here and reach nothing until you Save." text should go

# rl02

- verbal status needs to include "spinning up" and "spinning down"
- rl11 vs rlv12 - the original controller implementation was rl11, we made changes to conform it to rlv12.  we need to support both variants (rl11 for unibus, rlv12 for qbus) and properly model the differences

# tty backends (for all muxes/serial devices)

- telnet (in/outbound)
- bbb serial port
- websocket

# diagnostics

- log should be shown newest first
- reloading should load the log from disk
- endless scroll
- empty log should show "no log entries matched by filter"
