/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * wiredtiger_version --
 *	Return library version information.
 */
const char *
wiredtiger_version(int *majorp, int *minorp, int *patchp)
{
	if (majorp != NULL)
		*majorp = WIREDTIGER_VERSION_MAJOR;
	if (minorp != NULL)
		*minorp = WIREDTIGER_VERSION_MINOR;
	if (patchp != NULL)
		*patchp = WIREDTIGER_VERSION_PATCH;
	return ((char *)WIREDTIGER_VERSION_STRING);
}
