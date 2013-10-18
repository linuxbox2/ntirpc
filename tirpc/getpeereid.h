/*-------------------------------------------------------------------------
 *
 * getpeereid.c
 *		get peer userid for UNIX-domain socket connection
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/getpeereid.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef GETPEERID_H
#define GETPEERID_H

int getpeereid(int s, uid_t *euid, gid_t *egid);

#endif /* GETPEERID_H */
