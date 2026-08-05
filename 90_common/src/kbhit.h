/*
 * kbhit.h
 *
 *  Created on: 04.01.2012
 *      Author: joerg
 */

#ifndef KBHIT_H_
#define KBHIT_H_

//void reset_terminal_mode(void) ;
//void set_conio_terminal_mode(void) ;
int os_kbhit(void) ;

/*
 * Wait for a single key and return its code, without ECHO and without
 * waiting for ENTER. Unlike os_kbhit() this one blocks.
 * Return: the character code, or -1 if stdin is at EOF or is no terminal.
 * Use it only for "press a key" interaction: everything which is a command
 * must go through inputline_c, else command scripts can not drive it.
 */
int os_getkey(void) ;

#endif /* KBHIT_H_ */
