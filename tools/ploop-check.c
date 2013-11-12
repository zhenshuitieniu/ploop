/*
 *  Copyright (C) 2008-2012, Parallels, Inc. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/stat.h>
#include <getopt.h>
#include <linux/types.h>
#include <string.h>

#include "ploop.h"

static int ro;		/* read-only access to image file */
static int silent;	/* print messages only if errors detected */

static void usage(void)
{
	fprintf(stderr, "Usage: ploop check [-f|-F] [-c] [-r] [-s] [-d] DELTA\n"
			"       DELTA := path to image file\n"
			"	-f     - force check even if dirty flag is clear\n"
			"	-F     - -f and try to fix even fatal errors (dangerous)\n"
			"	-c     - check for duplicated blocks and holes\n"
			"	-r     - do not modify DELTA (read-only access)\n"
			"	-s     - be silent, report only errors\n"
			"	-d     - drop image \"in use\" flag\n"
		);
}

int main(int argc, char ** argv)
{
	int i;
	int flags = 0;

	while ((i = getopt(argc, argv, "fFcrsd")) != EOF) {
		switch (i) {
		case 'f':
			/* try to repair non-fatal conditions */
			flags |= CHECK_FORCE;
			break;
		case 'F':
			/* try to repair even fatal conditions */
			flags |= (CHECK_FORCE | CHECK_HARDFORCE);
			break;
		case 'c':
			/* build bitmap and check for duplicate blocks */
			flags |= CHECK_DETAILED;
			break;
		case 'd':
			flags |= CHECK_DROPINUSE;
			break;
		case 'r':
			ro = 1;
			break;
		case 's':
			silent = 1;
			break;
		default:
			usage();
			return SYSEXIT_PARAM;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1) {
		usage();
		return SYSEXIT_PARAM;
	}

	ploop_set_verbose_level(3);

	return ploop_check(argv[0], flags, ro, 0, !silent, NULL);
}
