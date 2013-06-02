/* event.c -*-mode: C; c-file-style:"cc-mode";-*- */
/*----------------------------------------------------------------------------
  File:   event.c
  Description: Rudp event handling: registering file descriptors and timeouts
               and eventloop using the select() system call.
  Author: Olof Hagsand and Peter Sj�din
  CVS Version: $Id: event.c,v 1.3 2007/05/03 10:46:06 psj Exp $
 
  This is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This softare is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  *--------------------------------------------------------------------------*/
/*
 * Register functions to be called either when input on a file descriptor,
 * or as a a result of a timeout.
 * The callback function has the following signature:
 *   int fn(int fd, void* arg)
 * fd is the file descriptor where the input was received (for timeouts, this
 *    contains no information).
 * arg is an argument given when the callback was registered.
 * If the return value of the callback is < 0, it is treated as an unrecoverable
 * error, and the program is terminated.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* generated by config & autoconf */
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <assert.h>

#include "event.h"

/*
 * Internal types to handle eventloop
 */
struct event_data{
    struct event_data *e_next;          /* next in list */
    int (*e_fn)(int, void*);            /* callback function */
    enum {EVENT_FD, EVENT_TIME} e_type; /* type of event */
    int e_fd;                           /* File descriptor */
    struct timeval e_time;              /* Timeout */
    void *e_arg;                        /* function argument */
    char e_string[32];                  /* string for identification/debugging */
};

/*
 * Internal variables
 */
static struct event_data *ee = NULL;
static struct event_data *ee_timers = NULL;

/*
 * Sort into internal event list
 * Given an absolute timestamp, register function to call.
 */
int
event_timeout(struct timeval t,  
		   int (*fn)(int, void*), 
		   void *arg, 
		   char *str)
{
    struct event_data *e, *e1, **e_prev;

    e = (struct event_data *)malloc(sizeof(struct event_data));
    if (e == NULL){
	perror("event_timeout: malloc");
	return -1;
    }
    memset(e, 0, sizeof(struct event_data));
    strcpy(e->e_string, str);
    e->e_fn = fn;
    e->e_arg = arg;
    e->e_type = EVENT_TIME;
    e->e_time = t;

    /* Sort into right place */
    e_prev = &ee_timers;
    for (e1 = ee_timers; e1; e1 =e1->e_next){
	if (timercmp(&e->e_time, &e1->e_time, <))
	    break;
	e_prev = &e1->e_next;
    }
    e->e_next = e1;
    *e_prev = e;
    return 0;
}

/*
 * Deregister a rudp event.
 */
static int
event_delete(struct event_data **firstp, int (*fn)(int, void*), 
		  void *arg)
{
    struct event_data *e, **e_prev;

    e_prev = firstp;
    for (e = *firstp; e; e = e->e_next){
	if (fn == e->e_fn && arg == e->e_arg) {
	    *e_prev = e->e_next;
	    free(e);
	    return 0;
	}
	e_prev = &e->e_next;
    }
    /* Not found */
    return -1;
}

/*
 * Deregister a rudp event.
 */
int
event_timeout_delete(int (*fn)(int, void*), 
		  void *arg)
{
    return event_delete(&ee_timers, fn, arg);
}

/*
 * Deregister a file descriptor event.
 */
int
event_fd_delete(int (*fn)(int, void*), 
		  void *arg)
{
    return event_delete(&ee, fn, arg);
}

/*
 * Register a callback function when something occurs on a file descriptor.
 * When an input event occurs on file desriptor <fd>, 
 * the function <fn> shall be called  with argument <arg>.
 * <str> is a debug string for logging.
 */
int
event_fd(int fd, int (*fn)(int, void*), void *arg, char *str)
{
    struct event_data *e;
    e = (struct event_data *)malloc(sizeof(struct event_data));
    if (e==NULL){
	perror("event_fd: malloc");
	return -1;
    }
    memset(e, 0, sizeof(struct event_data));
    strcpy(e->e_string, str);
    e->e_fd = fd;
    e->e_fn = fn;
    e->e_arg = arg;
    e->e_type = EVENT_FD;
    e->e_next = ee;
    ee = e;
    return 0;
}


/*
 * Rudp event loop.
 * Dispatch file descriptor events (and timeouts) by invoking callbacks.
 */
int
eventloop()
{
    struct event_data *e, *e1;
    fd_set fdset;
    int n;
    struct timeval t, t0;

    while (ee || ee_timers){
	FD_ZERO(&fdset);
	for (e=ee; e; e=e->e_next)
	    if (e->e_type == EVENT_FD)
		FD_SET(e->e_fd, &fdset);

	if (ee_timers){
	    gettimeofday(&t0, NULL);
	    timersub(&ee_timers->e_time, &t0, &t); 
	    if (t.tv_sec < 0)
		n = 0;
	    else
		n = select(FD_SETSIZE, &fdset, NULL, NULL, &t); 
	}
	else
	    n = select(FD_SETSIZE, &fdset, NULL, NULL, NULL); 

	if (n == -1)
	    if (errno != EINTR)
		perror("eventloop: select");
	if (n == 0) {  /* Timeout */
	    e = ee_timers;
	    ee_timers = ee_timers->e_next;
#ifdef DEBUG
	    fprintf(stderr, "eventloop: timeout : %s[arg: %x]\n", 
		    e->e_string, (int)e->e_arg);
#endif /* DEBUG */
	    if ((*e->e_fn)(0, e->e_arg) < 0) {
		return -1;

	    }
	    switch(e->e_type) {
	    case EVENT_TIME:
		free(e);
		break;
	    default:
		fprintf(stderr, "eventloop: illegal e_type:%d\n", e->e_type);
	    }
	    continue;
	}
	e = ee;
	while (e) {
		e1 = e->e_next;
	    if (e->e_type == EVENT_FD && FD_ISSET(e->e_fd, &fdset)){
#ifdef DEBUG
		fprintf(stderr, "eventloop: socket rcv: %s[fd: %d arg: %x]\n", 
			e->e_string, e->e_fd, (int)e->e_arg);
#endif /* DEBUG */
		if ((*e->e_fn)(e->e_fd, e->e_arg) < 0) {
		    return  -1;
		}
	    }
	    e = e1;
	}
    }
#ifdef DEBUG
    fprintf(stderr, "eventloop: returning 0\n");
#endif /* DEBUG */
    return 0;
}
