#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <string.h>

#include "ploop.h"

enum {
	ZEROFIX = 0,
	IGNORE
};

enum {
	NONFATAL = 0,
	FATAL
};

enum {
	SOFT_FIX = 0,
	HARD_FIX
};

struct ploop_fsck_desc {
	int    fd;
	int    ro;
	int    hard_force;
	int    check;
	off_t  bd_size;
	off_t  size;
	__u32 *bmap;
	int   *clean;
	int   *fatality;
	__u32 *alloc_head;
};

static int READ(int fd, void * buf, unsigned int size, off_t pos, char *msg)
{
	ssize_t res;

	res = pread(fd, buf, size, pos);
	if (res == size)
		return 0;

	if (res < 0) {
		ploop_err(errno, "%s", msg);
		return SYSEXIT_READ;
	}

	ploop_log(0, "Short %s", msg);
	return SYSEXIT_READ;
}

static int WRITE(int fd, void * buf, unsigned int size, off_t pos, char *msg)
{
	ssize_t res;

	res = pwrite(fd, buf, size, pos);
	if (res == size)
		return 0;

	if (res < 0) {
		ploop_err(errno, "%s", msg);
		return SYSEXIT_WRITE;
	}
	ploop_log(0,  "Short %s", msg);
	return SYSEXIT_WRITE;
}

static int FSYNC(int fd)
{
	if (fsync(fd)) {
		ploop_err(errno, "fsync");
		return SYSEXIT_WRITE;
	}
	return 0;
}

static int CLOSE(int fd)
{
	if (close(fd)) {
		ploop_err(errno, "close");
		return SYSEXIT_WRITE;
	}
	return 0;
}

static int zero_index(int fd, __u32 clu)
{
	__u32 zero = 0;

	return WRITE(fd, &zero, sizeof(zero),
		     clu * sizeof(clu) + sizeof(struct ploop_pvd_header),
		     "write zero index");
}

static int zero_index_fix(struct ploop_fsck_desc *d, __u32 clu,
			   int hard_fix, int ignore, int fatal)
{
	char *msg;
	int   skip = d->ro;
	int   ret = 0;

	if (hard_fix && !d->hard_force)
		skip = 1;

	if (skip) {
		msg = fatal ? "FATAL" : "Skipped";
		*d->clean = 0;
		if (fatal)
			*d->fatality = 1;
	} else {
		msg = ignore ? "Ignored" : "Fixed";
		if (!ignore)
			ret = zero_index(d->fd, clu);
	}

	ploop_log(0,  "%s", msg);
	return ret;
}

int check_one_slot(struct ploop_fsck_desc *d, __u32 clu, __u32 isec)
{
	__u32 iblk = isec >> PLOOP1_DEF_CLUSTER_LOG;

	if ((clu << PLOOP1_DEF_CLUSTER_LOG) > d->bd_size) {
		ploop_log(0, "Data cluster (%u) beyond block device size... ",
				clu);
		return zero_index_fix(d, clu, SOFT_FIX, ZEROFIX, NONFATAL);
	}

	if (isec % (1 << PLOOP1_DEF_CLUSTER_LOG) != 0) {
		ploop_log(0, "L2 slot (%u) corrupted... ",
				clu);
		return zero_index_fix(d, clu, HARD_FIX, ZEROFIX, FATAL);
	}

	if ((off_t)iblk * CLUSTER + CLUSTER > d->size) {
		ploop_log(0, "Data cluster %u beyond EOF, vsec=%u... ",
			iblk, clu);
		return zero_index_fix(d, clu, HARD_FIX, ZEROFIX, FATAL);
	}

	if (d->check) {
		if (d->bmap[iblk / 32] & (1 << (iblk % 32))) {
			ploop_log(0, "Block %u is used more than once, vsec=%u... ",
				iblk, clu);
			zero_index_fix(d, clu, HARD_FIX, IGNORE, FATAL);
		}
		d->bmap[iblk / 32] |= (1 << (iblk % 32));
	}

	if (iblk > *d->alloc_head)
		*d->alloc_head = iblk;

	return 0;
}

int ploop_fsck(char *img, int force, int hard_force, int check, int ro, int verbose)
{
	struct ploop_fsck_desc d;
	int i, j;
	int fd;
	int ret = 0;
	int ret2;
	off_t bd_size;
	struct stat stb;
	unsigned char buf[CLUSTER];
	__u32 *l2_ptr = (__u32*)buf;

	struct ploop_pvd_header vh_buf;
	struct ploop_pvd_header *vh = &vh_buf;

	__u32 alloc_head;
	__u32 l1_slots;
	__u32 l2_slot = 0;
	__u32 m_Flags;

	__u32 *bmap = NULL;
	unsigned int bmap_size = 0;

	int fatality = 0;   /* fatal errors detected */
	int clean = 1;	    /* image is clean */

	fd = open(img, ro ? O_RDONLY : O_RDWR);
	if (fd < 0) {
		ploop_err(errno, "ploop_fsck: can't open %s",
				img);
		return SYSEXIT_OPEN;
	}

	if (fstat(fd, &stb)) {
		ploop_err(errno, "ploop_fsck: can't fstat %s",
				img);
		ret = SYSEXIT_OPEN;
		goto done;
	}

	ret = READ(fd, vh, sizeof(*vh), 0, "read PVD header");
	if (ret)
		goto done;

	ret = SYSEXIT_PLOOPFMT;
	if (memcmp(vh->m_Sig, SIGNATURE_STRUCTURED_DISK, sizeof(vh->m_Sig))) {
		ploop_err(0, "Wrong signature in image %s", img);
		goto done;
	}
	if (vh->m_Type != PRL_IMAGE_COMPRESSED) {
		ploop_err(0, "Wrong type in image %s", img);
		goto done;
	}
	if (vh->m_Sectors != 1 << PLOOP1_DEF_CLUSTER_LOG) {
		ploop_err(0, "Wrong cluster size in image %s", img);
		goto done;
	}

	l1_slots = vh->m_FirstBlockOffset >> PLOOP1_DEF_CLUSTER_LOG;
	if (vh->m_FirstBlockOffset % vh->m_Sectors != 0 || l1_slots == 0) {
		ploop_err(0, "Wrong first block offset in image %s", img);
		goto done;
	}

	ret = 0;
	bd_size = vh->m_SizeInSectors;
	alloc_head = l1_slots - 1;

	if (!vh->m_DiskInUse && !force) {
		if (verbose)
			ploop_log(0, "Image is clean, fsck is skipped");
		goto done;
	}

	if (check) {
		bmap_size = (stb.st_size + CLUSTER - 1)/(CLUSTER);
		bmap_size = (bmap_size + 31)/8;
		bmap = malloc(bmap_size);
		if (bmap == NULL) {
			ploop_err(ENOMEM, "ploop_fsck: malloc");
			if (verbose) {
				check = 0;
			} else {
				ret = SYSEXIT_MALLOC;
				goto done;
			}
		}
		if (check) {
			memset(bmap, 0, bmap_size);
			for (i = 0; i < l1_slots; i++)
				bmap[i / 32] |= 1 << (i % 32);
		}
	}

	/* in */
	d.fd	     = fd;
	d.ro	     = ro;
	d.hard_force = hard_force;
	d.check	     = check;
	d.bd_size    = bd_size;
	d.size	     = stb.st_size;
	/* out */
	d.bmap	     = bmap;
	d.clean	     = &clean;
	d.fatality   = &fatality;
	d.alloc_head = &alloc_head;

	for (i = 0; i < l1_slots; i++) {
		int skip = (i == 0) ? sizeof(*vh) / sizeof(__u32) : 0;

		ret = READ(fd, buf, sizeof(buf), i * CLUSTER,
			   "read index table");
		if (ret)
			goto done;

		if (!ro && vh->m_DiskInUse) {
			ret = WRITE(fd, buf, sizeof(buf), i * CLUSTER,
				    "re-write index table");
			if (ret)
				goto done;
		}

		for (j = skip; j < CLUSTER/4; j++, l2_slot++) {
			if (l2_ptr[j] == 0)
				continue;

			ret = check_one_slot(&d, l2_slot, l2_ptr[j]);
			if (ret)
				goto done;
		}
	}

	alloc_head++;

	if (check) {
		for (i = 0; i < bmap_size/4; i++) {
			int k;

			if (bmap[i] == 0xFFFFFFFF)
				continue;

			if (i * 32 >= alloc_head)
				break;

			for (k = 0; k < 32 && k < alloc_head - i * 32; k++) {
				if (!(bmap[i] & (1 << k))) {
					ploop_log(0, "Hole at block %u", i*32 + k);
				}
			}
		}
	}

	if (fatality) {
		ploop_err(0, "Fatal errors were found, image %s is not repaired", img);
		ret = SYSEXIT_PLOOPFMT;
		goto done;
	}

	if ((off_t)alloc_head * CLUSTER < stb.st_size) {
		if (!ro) {
			ploop_log(0, "Trimming tail");
			if (ftruncate(fd, (off_t)alloc_head * CLUSTER)) {
				ploop_err(errno, "ftruncate");
				ret = SYSEXIT_FTRUNCATE;
				goto done;
			}
		} else {
			ploop_err(0, "Want to trim tail");
			alloc_head = (stb.st_size + CLUSTER - 1)/(CLUSTER);
		}
	}

	if (alloc_head > l1_slots)
		m_Flags = vh->m_Flags & ~CIF_Empty;
	else
		m_Flags = vh->m_Flags | CIF_Empty;

	if (vh->m_DiskInUse != 0)
		ploop_err(0, "Dirty flag is set");
	if (vh->m_Flags != m_Flags)
		ploop_err(0, "CIF_Empty flag is incorrect");

	/* useless to repair header if content was not fixed */
	if (!clean) {
		ret = SYSEXIT_PLOOPFMT;
		goto done;
	}

	/* the content is clean, only header checks remained */
	if (vh->m_DiskInUse == 0 && vh->m_Flags == m_Flags)
		goto done;

	/* header needs fix but we can't */
	if (ro) {
		ploop_err(0, "Image is clean but unable to fix the header on ro image");
		ret = SYSEXIT_PLOOPFMT;
		goto done;
	}

	vh->m_DiskInUse = 0;
	vh->m_Flags = m_Flags;

	ret = WRITE(fd, vh, sizeof(*vh), 0, "write PVD header");
	if (!ret)
		ret = FSYNC(fd);
done:
	ret2 = CLOSE(fd);
	if (ret2 && !ret)
		ret = ret2;

	if (bmap)
		free(bmap);

	return ret;
}
