<!-- migrated from https://retrocmp.com/projects/unibone/281-unibone-as-memory-emulator-user-view -->

 At first, an UniBone can behave like UNIBUS memory.

![unibone ms11](images/unibone-ms11.jpg)

If you have a PDP-11 which is only partly populated with memory, UniBone can extend usable memory to the full UNIBUS 18bit address range of 248 kBytes.

That said, this article is already complete ... so lets fill it up with a real-world walk-through in detail here:

## The "magic memory extender"

We have a PDP-11/34 here which only has a single MS11-J memory card with 32 KB installed.

![pdp1134 ms11](images/pdp1134-ms11.jpg)

 

UniBone can scan the existing memory in a PDP-11 and emulate the missing memory range.

On UniBone Linux the application "Demo" is running. It shows a crude tree of text-mode menus. Here is a screenshot of the "memory" submenu:

![screenshot demo memory menu](images/screenshot-demo-memory-menu.jpg)

The user (I!) first typed "sz" to the "\>\>\>" prompt to scan available memory. (UniBone is doing bus master DMA here, see next page.) The 32kB MS11 card occupies address range 0-77776.

Then the magic "m" excutes the "auto-complete" function. UniBone is now emulating the missing memory from 100000 up to the IO page border 757776.

 

Booting XXDP before and after the memory expansion shows the success: the emulated memory is accepted. We have now 124kwords / 248 kBytes of memory.

|  |  |
|----|----|
| ![screenshot xxdp boot 16kw small](images/screenshot-xxdp-boot-16kw-small.jpg) | ![screenshot xxdp boot 124kw small](images/screenshot-xxdp-boot-124kw-small.jpg) |
