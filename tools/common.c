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

#include <linux/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "ploop1_image.h"
#include "libploop.h"

int parse_size(const char * opt, off_t * sz)
{
	__u64 val;
	char * endptr;

	val = strtoul(opt, &endptr, 0);

	if (opt == endptr)
		return -1;

	if (strlen(endptr) > 1)
		return -1;

	switch (*endptr) {
	case 'G': case 'g':
		if (val >= ~0ULL/(1024*1024*1024/512))
			return -1;
		val *= 1024*1024*1024/512;
		*sz = val;
		break;
	case 'M': case 'm':
		if (val >= ~0ULL/(1024*1024/512))
			return -1;
		val *= 1024*1024/512;
		*sz = val;
		break;
	case 'K': case 'k':
		if (val >= ~0ULL/(1024/512))
			return -1;
		val *= 1024/512;
		*sz = val;
		break;
	case 0:
		*sz = (off_t)val;
		break;
	default:
		return -1;
	}
	if (val >= (0xffffffffULL << PLOOP1_SECTOR_LOG))
		return -1;
	return 0;
}

int parse_format_opt(const char *opt)
{
	if (strcmp(opt, "raw") == 0)
		return PLOOP_RAW_MODE;
	else if ((strcmp(opt, "ploop1") == 0) ||
		 (strcmp(opt, "expanded") == 0))
		return PLOOP_EXPANDED_MODE;
	else if (strcmp(opt, "preallocated") == 0)
		return PLOOP_EXPANDED_PREALLOCATED_MODE;

	fprintf(stderr, "Bad -f argument: %s\n", opt);
	return -1;
}

int is_xml_fname(const char *fname)
{
	const char *p;

	p = strrchr(fname, '.');
	return (p != NULL && !strcmp(p, ".xml"));
}

int read_dd(struct ploop_disk_images_data **di, const char *file)
{
	*di = ploop_alloc_diskdescriptor();
	if (*di == NULL)
		return SYSEXIT_NOMEM;

	if (ploop_read_diskdescriptor(file, *di)) {
		ploop_free_diskdescriptor(*di);
		return SYSEXIT_DISKDESCR;
	}

	return 0;
}
