#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <linux/types.h>
#include <string.h>

#include "ploop.h"
#include "ploop_if.h"

#define EXT4_IOC_OPEN_BALLOON		_IO('f', 42)


char *mntn2str(int mntn_type)
{
	switch (mntn_type) {
	case PLOOP_MNTN_OFF:
		return "OFF";
	case PLOOP_MNTN_BALLOON:
		return "BALLOON";
	case PLOOP_MNTN_FBLOADED:
		return "FBLOADED";
	case PLOOP_MNTN_TRACK:
		return "TRACK";
	case PLOOP_MNTN_RELOC:
		return "RELOC";
	case PLOOP_MNTN_MERGE:
		return "MERGE";
	case PLOOP_MNTN_GROW:
		return "GROW";
	}

	return "UNKNOWN";
}

static int open_device(const char *device)
{
	int fd = open(device, O_RDONLY);
	if (fd < 0) {
		ploop_err(errno, "Can't open ploop device %s",
			device);
		return -1;
	}
	return fd;
}

static int ioctl_device(int fd, int req, void *arg)
{
	if (fd < 0)
		return -1;

	if (ioctl(fd, req, arg)) {
		char *msg = "UNKNOWN";

		switch (req) {
		case PLOOP_IOC_BALLOON:
			msg = "PLOOP_IOC_BALLOON";
			break;
		case PLOOP_IOC_FREEBLKS:
			msg = "PLOOP_IOC_FREEBLKS";
			break;
		case PLOOP_IOC_RELOCBLKS:
			msg = "PLOOP_IOC_RELOCBLKS";
			break;
		case PLOOP_IOC_FBGET:
			msg = "PLOOP_IOC_FBGET";
			break;
		}
		ploop_err(errno, "%s", msg);
		return(SYSEXIT_DEVIOC);
	}
	return 0;
}

static int fsync_balloon(int fd)
{
	if (fsync(fd)) {
		ploop_err(errno, "Can't fsync balloon");
		return(SYSEXIT_FSYNC);
	}
	return 0;
}

/*
 * Open, flock and stat balloon.
 *
 * Returns balloon fd.
 */
int get_balloon(const char *mount_point, struct stat *st, int *outfd)
{
	int fd, fd2;

	if (mount_point == NULL)
		return -1;

	fd = open(mount_point, O_RDONLY);
	if (fd < 0) {
		ploop_err(errno, "Can't open mount_point");
		return(SYSEXIT_OPEN);
	}

	fd2 = ioctl(fd, EXT4_IOC_OPEN_BALLOON, 0);
	close(fd);

	if (fd2 < 0) {
		ploop_err(errno, "Can't ioctl mount_point");
		return(SYSEXIT_DEVIOC);
	}

	if (outfd != NULL) {
		if (flock(fd2, LOCK_EX | LOCK_NB)) {
			close(fd2);
			if (errno == EWOULDBLOCK) {
				ploop_err(0, "Hidden balloon is in use "
					"by someone else!");
				return(SYSEXIT_EBUSY);
			}
			ploop_err(errno, "Can't flock balloon");
			return(SYSEXIT_FLOCK);
		}
		*outfd = fd2;
	}

	if (st != NULL && fstat(fd2, st)) {
		close(fd2);
		ploop_err(errno, "Can't stat balloon");
		return(SYSEXIT_FSTAT);
	}
	if (outfd == NULL)
		close(fd2);

	return 0;
}

static int open_top_delta(const char *device, struct delta *delta, int *lvl)
{
	char *image = NULL;
	char *fmt = NULL;

	if (ploop_get_attr(device, "top", lvl)) {
		ploop_err(0, "Can't find top delta");
		return(SYSEXIT_SYSFS);
	}

	if (find_delta_names(device, *lvl, *lvl, &image, &fmt))
		return(SYSEXIT_SYSFS);

	if (strcmp(fmt, "raw") == 0) {
		ploop_err(0, "Ballooning for raw format is not supported");
		return(SYSEXIT_PARAM);
	}

	if (open_delta(delta, image, O_RDONLY|O_DIRECT, OD_ALLOW_DIRTY)) {
		ploop_err(errno, "open_delta");
		return(SYSEXIT_OPEN);
	}
	return 0;
}

static __u32 *alloc_reverse_map(__u32 len)
{
	__u32 *reverse_map;

	reverse_map = malloc(len * sizeof(__u32));
	if (reverse_map == NULL) {
		ploop_err(errno, "Can't allocate reverse map");
		return NULL;
	}
	return reverse_map;
}

static int do_truncate(int fd, int mntn_type, off_t old_size, off_t new_size)
{
	int ret;

	switch (mntn_type) {
	case PLOOP_MNTN_OFF:
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
		break;
	case PLOOP_MNTN_BALLOON:
		ploop_err(0, "Error: mntn_type is PLOOP_MNTN_BALLOON "
			"after IOC_BALLOON");
		return(SYSEXIT_PROTOCOL);
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		ploop_err(0, "Can't truncate hidden balloon before previous "
		       "balloon operation (%s) is completed. Use \"ploop-balloon "
		       "complete\".", mntn2str(mntn_type));
		return(SYSEXIT_EBUSY);
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)", mntn_type);
		return(SYSEXIT_PROTOCOL);
	}

	if (new_size == old_size) {
		ploop_log(0, "Nothing to do: new_size == old_size");
	} else if (ftruncate(fd, new_size)) {
		ploop_err(errno, "Can't truncate hidden balloon");
		fsync_balloon(fd);
		return(SYSEXIT_FTRUNCATE);
	} else {
		ret = fsync_balloon(fd);
		if (ret)
			return ret;
		ploop_log(0, "Successfully truncated balloon from %llu to %llu bytes",
			(unsigned long long)old_size, (unsigned long long)new_size);
	}
	return 0;
}

static int do_inflate(int fd, int mntn_type, off_t old_size, off_t *new_size, int *drop_state)
{
	struct stat st;
	int err;

	*drop_state = 0;
	switch (mntn_type) {
	case PLOOP_MNTN_BALLOON:
		break;
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
		ploop_err(0, "Can't inflate hidden balloon while another "
			"maintenance operation is in progress (%s)",
			mntn2str(mntn_type));
		return(SYSEXIT_EBUSY);
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		ploop_err(0, "Can't inflate hidden balloon before previous "
			"balloon operation (%s) is completed. Use "
			"\"ploop-balloon complete\".", mntn2str(mntn_type));
		return(SYSEXIT_EBUSY);
	case PLOOP_MNTN_OFF:
		ploop_err(0, "Error: mntn_type is PLOOP_MNTN_OFF after "
			"IOC_BALLOON");
		return(SYSEXIT_PROTOCOL);
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)", mntn_type);
		return(SYSEXIT_PROTOCOL);
	}
	err = sys_fallocate(fd, 0, 0, *new_size);
	if (err)
		ploop_err(errno, "Can't fallocate balloon");

	if (fstat(fd, &st)) {
		ploop_err(errno, "Can't stat balloon (2)");
		if (ftruncate(fd, old_size))
			ploop_err(errno, "Can't revert old_size back");
		return(err ? SYSEXIT_FALLOCATE : SYSEXIT_FSTAT);
	}

	if (err) {
		if (st.st_size != old_size) {
			if (ftruncate(fd, old_size))
				ploop_err(errno, "Can't revert old_size back (2)");
			else
				*drop_state = 1;
		}
		return(SYSEXIT_FALLOCATE);
	}

	if (st.st_size < *new_size) {
		ploop_err(0, "Error: after fallocate(%d, 0, 0, %llu) fstat "
			"reported size == %llu", fd,
				(unsigned long long)*new_size, (unsigned long long)st.st_size);
		if (ftruncate(fd, old_size))
			ploop_err(errno, "Can't revert old_size back (3)");
		else
			*drop_state = 1;
		return(SYSEXIT_FALLOCATE);
	}
	*new_size = st.st_size;

	err = fsync_balloon(fd);
	if (err)
		return err;

	ploop_log(0, "Successfully inflate balloon from %llu to %llu bytes",
			(unsigned long long)old_size, (unsigned long long)*new_size);
	return 0;
}

int ploop_balloon_change_size(const char *device, int balloonfd, off_t new_size)
{
	int    fd = -1;
	int    ret;
	off_t  old_size;
	__u32  dev_start;  /* /sys/block/ploop0/ploop0p1/start */
	__u32  n_free_blocks;
	__u32  freezed_a_h;
	struct ploop_balloon_ctl    b_ctl;
	struct stat		    st;
	struct pfiemap		   *pfiemap = NULL;
	struct freemap		   *freemap = NULL;
	struct freemap		   *rangemap = NULL;
	struct relocmap		   *relocmap = NULL;
	struct ploop_freeblks_ctl  *freeblks = NULL;
	struct ploop_relocblks_ctl *relocblks = NULL;
	__u32 *reverse_map = NULL;
	__u32  reverse_map_len;
	int top_level;
	struct delta delta = { .fd = -1 };
	int entries_used;
	int drop_state = 0;

	if (fstat(balloonfd, &st)) {
		ploop_err(errno, "Can't get balloon file size");
		return SYSEXIT_FSTAT;
	}

	old_size = st.st_size;
	new_size = ((new_size << 9) + st.st_blksize - 1) & ~(st.st_blksize - 1);

	ploop_log(0, "change balloon size old_size=%ld new_size=%ld",
			old_size, new_size);

	pfiemap = fiemap_alloc(128);
	freemap = freemap_alloc(128);
	rangemap = freemap_alloc(128);
	relocmap = relocmap_alloc(128);
	if (!pfiemap || !freemap || !rangemap || !relocmap) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	fd = open_device(device);
	if (fd == -1) {
		ret = SYSEXIT_OPEN;
		goto err;
	}

	memset(&b_ctl, 0, sizeof(b_ctl));
	if (old_size < new_size)
		b_ctl.inflate = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	drop_state = 1;
	if (old_size >= new_size) {
		ret = do_truncate(balloonfd, b_ctl.mntn_type, old_size, new_size);
		goto err;
	}

	if (dev_num2dev_start(device, st.st_dev, &dev_start)) {
		ploop_err(0, "Can't find out offset from start of ploop "
			"device (%s) to start of partition",
			device);
		ret = SYSEXIT_SYSFS;
		goto err;
	}

	ret = open_top_delta(device, &delta, &top_level);
	if (ret)
		goto err;

	ret = do_inflate(balloonfd, b_ctl.mntn_type, old_size, &new_size, &drop_state);
	if (ret)
		goto err;

	reverse_map_len = delta.l2_size + delta.l2_size;
	reverse_map = alloc_reverse_map(reverse_map_len);
	if (reverse_map == NULL) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	ret = fiemap_get(balloonfd, dev_start << 9, old_size, new_size, &pfiemap);
	if (ret)
		goto err;
	fiemap_adjust(pfiemap);
	ret = fiemap_build_rmap(pfiemap, reverse_map, reverse_map_len, &delta);
	if (ret)
		goto err;

	ret = rmap2freemap(reverse_map, 0, reverse_map_len, &freemap, &entries_used);
	if (ret)
		goto err;
	if (entries_used == 0) {
		drop_state = 1;
		ploop_log(0, "no unused cluster blocks found");
		goto out;
	}

	ret = freemap2freeblks(freemap, top_level, &freeblks, &n_free_blocks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_FREEBLKS, freeblks);
	if (ret)
		goto err;
	freezed_a_h = freeblks->alloc_head;
	if (freezed_a_h > reverse_map_len) {
		ploop_err(0, "Image corrupted: a_h=%u > rlen=%u",
			freezed_a_h, reverse_map_len);
		ret = SYSEXIT_PLOOPFMT;
		goto err;
	}

	ret = range_build(freezed_a_h, n_free_blocks, reverse_map, reverse_map_len,
		    &delta, freemap, &rangemap, &relocmap);
	if (ret)
		goto err;

	ret = relocmap2relocblks(relocmap, top_level, freezed_a_h, n_free_blocks,
			   &relocblks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_RELOCBLKS, relocblks);
	if (ret)
		goto err;
	ploop_log(0, "TRUNCATED: %u cluster-blocks (%llu bytes)",
			relocblks->alloc_head,
			(unsigned long long)(relocblks->alloc_head * CLUSTER));
out:
	ret = 0;
err:
	if (drop_state) {
		memset(&b_ctl, 0, sizeof(b_ctl));
		ioctl(fd, PLOOP_IOC_BALLOON, &b_ctl);
	}
	close(fd);
	free(pfiemap);
	free(freemap);
	free(rangemap);
	free(relocmap);
	free(reverse_map);
	free(freeblks);
	free(relocblks);
	if (delta.fd != -1)
		close_delta(&delta);

	return ret;
}

int ploop_balloon_get_state(const char *device, __u32  *state)
{
	int fd, ret;
	struct ploop_balloon_ctl b_ctl;

	fd = open_device(device);
	if (fd == -1)
		return SYSEXIT_OPEN;

	bzero(&b_ctl, sizeof(b_ctl));
	b_ctl.keep_intact = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	*state = b_ctl.mntn_type;

err:
	close(fd);

	return ret;
}

int ploop_balloon_clear_state(const char *device)
{
	int fd, ret;
	struct ploop_balloon_ctl b_ctl;

	fd = open_device(device);
	if (fd == -1)
		return SYSEXIT_OPEN;

	bzero(&b_ctl, sizeof(b_ctl));
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	if (b_ctl.mntn_type != PLOOP_MNTN_OFF) {
		ploop_err(0, "Can't clear stale in-kernel \"BALLOON\" "
				"maintenance state because kernel is in \"%s\" "
				"state now", mntn2str(b_ctl.mntn_type));
		ret = SYSEXIT_EBUSY;
	}
err:
	close(fd);
	return ret;
}

int ploop_baloon_complete(const char *device)
{
	int    ret = -1, fd = -1;
	__u32  n_free_blocks = 0;
	__u32  freezed_a_h;
	struct ploop_balloon_ctl    b_ctl;
	struct freemap		   *freemap = NULL;
	struct freemap		   *rangemap = NULL;
	struct relocmap		   *relocmap = NULL;
	struct ploop_freeblks_ctl  *freeblks = NULL;
	struct ploop_relocblks_ctl *relocblks = NULL;;
	__u32 *reverse_map = NULL;
	__u32  reverse_map_len;
	int top_level;
	struct delta delta = {};

	freemap  = freemap_alloc(128);
	rangemap = freemap_alloc(128);
	relocmap = relocmap_alloc(128);
	if (freemap == NULL || rangemap == NULL || relocmap == NULL) {
		ret = SYSEXIT_NOMEM;
		goto err;
	}

	fd = open_device(device);
	if (fd == -1)
		goto err;

	memset(&b_ctl, 0, sizeof(b_ctl));
	b_ctl.keep_intact = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	switch (b_ctl.mntn_type) {
	case PLOOP_MNTN_BALLOON:
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
	case PLOOP_MNTN_OFF:
		ploop_log(0, "Nothing to complete: kernel is in \"%s\" state",
			mntn2str(b_ctl.mntn_type));
		return 0;
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		top_level   = b_ctl.level;
		freezed_a_h = b_ctl.alloc_head;
		break;
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)",
			b_ctl.mntn_type);
		ret = SYSEXIT_PROTOCOL;
		goto err;
	}

	if (b_ctl.mntn_type == PLOOP_MNTN_RELOC)
		goto reloc;

	ret = freeblks_alloc(&freeblks, 0);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_FBGET, freeblks);
	if (ret)
		goto err;

	if (freeblks->n_extents == 0)
		goto reloc;

	ret = freeblks_alloc(&freeblks, freeblks->n_extents);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_FBGET, freeblks);
	if (ret)
		goto err;

	ret = freeblks2freemap(freeblks, &freemap, &n_free_blocks);
	if (ret)
		goto err;

	ret = open_top_delta(device, &delta, &top_level);
	if (ret)
		goto err;
	reverse_map_len = delta.l2_size + delta.l2_size;
	reverse_map = alloc_reverse_map(reverse_map_len);
	if (reverse_map == NULL) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	ret = range_build(freezed_a_h, n_free_blocks, reverse_map, reverse_map_len,
		    &delta, freemap, &rangemap, &relocmap);
	if (ret)
		goto err;
reloc:
	ret = relocmap2relocblks(relocmap, top_level, freezed_a_h, n_free_blocks,
			   &relocblks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_RELOCBLKS, relocblks);
	if (ret)
		goto err;

	ploop_log(0, "TRUNCATED: %u cluster-blocks (%llu bytes)",
			relocblks->alloc_head,
			(unsigned long long)(relocblks->alloc_head * CLUSTER));
err:

	close(fd);
	free(freemap);
	free(rangemap);
	free(relocmap);
	free(reverse_map);
	free(freeblks);
	free(relocblks);

	return ret;
}

int ploop_baloon_check_and_repair(const char *device, char *mount_point, int repair)
{
	int   ret, fd = -1;
	int   balloonfd = -1;
	__u32 n_free_blocks;
	__u32 freezed_a_h;
	__u32 dev_start;  /* /sys/block/ploop0/ploop0p1/start */
	struct ploop_balloon_ctl    b_ctl;
	struct stat		    st;
	struct pfiemap		   *pfiemap  = NULL;
	struct freemap		   *freemap  = NULL;
	struct freemap		   *rangemap = NULL;
	struct relocmap		   *relocmap = NULL;
	struct ploop_freeblks_ctl  *freeblks = NULL;
	struct ploop_relocblks_ctl *relocblks= NULL;
	char *msg = repair ? "repair" : "check";
	__u32 *reverse_map = NULL;
	__u32  reverse_map_len;
	int top_level;
	int entries_used;
	struct delta delta = {};
	int drop_state = 0;

	ret = get_balloon(mount_point, &st, &balloonfd);
	if (ret)
		return ret;

	if (st.st_size == 0) {
		ploop_log(0, "Nothing to do: hidden balloon is empty");
		close(balloonfd);
		return 0;
	}

	pfiemap = fiemap_alloc(128);
	freemap = freemap_alloc(128);
	rangemap = freemap_alloc(128);
	relocmap = relocmap_alloc(128);
	if (!pfiemap || !freemap || !rangemap || !relocmap) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	fd = open_device(device);
	if (fd == -1) {
		ret = SYSEXIT_OPEN;
		goto err;
	}

	memset(&b_ctl, 0, sizeof(b_ctl));
	/* block other maintenance ops even if we only check balloon */
	b_ctl.inflate = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	switch (b_ctl.mntn_type) {
	case PLOOP_MNTN_BALLOON:
		drop_state = 1;
		ret = open_top_delta(device, &delta, &top_level);
		if (ret)
			goto err;
		reverse_map_len = delta.l2_size + delta.l2_size;
		reverse_map = alloc_reverse_map(reverse_map_len);
		if (reverse_map == NULL) {
			ret = SYSEXIT_MALLOC;
			goto err;
		}
		break;
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
		ploop_err(0, "Can't %s hidden balloon while another "
		       "maintenance operation is in progress (%s)",
			msg, mntn2str(b_ctl.mntn_type));
		ret = SYSEXIT_EBUSY;
		goto err;
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		ploop_err(0, "Can't %s hidden balloon before previous "
			"balloon operation (%s) is completed. Use "
			"\"ploop-balloon complete\".",
			msg, mntn2str(b_ctl.mntn_type));
		ret = SYSEXIT_EBUSY;
		goto err;
	case PLOOP_MNTN_OFF:
		ploop_err(0, "Error: mntn_type is PLOOP_MNTN_OFF after "
			"IOC_BALLOON");
		ret = SYSEXIT_PROTOCOL;
		goto err;
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)",
			b_ctl.mntn_type);
		ret = SYSEXIT_PROTOCOL;
		goto err;
	}

	if (dev_num2dev_start(device, st.st_dev, &dev_start)) {
		ploop_err(0, "Can't find out offset from start of ploop "
			"device (%s) to start of partition where fs (%s) "
			"resides", device, mount_point);
		ret = SYSEXIT_SYSFS;
		goto err;
	}

	ret = fiemap_get(balloonfd, dev_start << 9, 0, st.st_size, &pfiemap);
	if (ret)
		goto err;
	fiemap_adjust(pfiemap);

	ret = fiemap_build_rmap(pfiemap, reverse_map, reverse_map_len, &delta);
	if (ret)
		goto err;

	ret = rmap2freemap(reverse_map, 0, reverse_map_len, &freemap, &entries_used);
	if (ret)
		goto err;
	if (entries_used == 0) {
		ploop_log(0, "No free blocks found");
		goto err;
	}

	ret = freemap2freeblks(freemap, top_level, &freeblks, &n_free_blocks);
	if (ret)
		return ret;
	if (!repair) {
		ploop_log(0, "Found %u free blocks. Consider using "
		       "\"ploop-balloon repair\"", n_free_blocks);
		ret = 0;
		goto err;
	} else {
		ploop_log(0, "Found %u free blocks", n_free_blocks);
	}

	ret = ioctl_device(fd, PLOOP_IOC_FREEBLKS, freeblks);
	if (ret)
		return ret;
	drop_state = 0;
	freezed_a_h = freeblks->alloc_head;
	if (freezed_a_h > reverse_map_len) {
		ploop_err(0, "Image corrupted: a_h=%u > rlen=%u",
			freezed_a_h, reverse_map_len);
		ret = SYSEXIT_PLOOPFMT;
		goto err;
	}

	ret = range_build(freezed_a_h, n_free_blocks, reverse_map, reverse_map_len,
		    &delta, freemap, &rangemap, &relocmap);
	if (ret)
		goto err;

	ret = relocmap2relocblks(relocmap, top_level, freezed_a_h, n_free_blocks,
			   &relocblks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_RELOCBLKS, relocblks);
	if (ret)
		return ret;

	ploop_log(0, "TRUNCATED: %u cluster-blocks (%llu bytes)",
			relocblks->alloc_head,
			(unsigned long long)(relocblks->alloc_head * CLUSTER));

err:
	if (drop_state) {
		memset(&b_ctl, 0, sizeof(b_ctl));
		ioctl(fd, PLOOP_IOC_BALLOON, &b_ctl);
	}

	// FIXME: close_delta()
	close(balloonfd);
	close(fd);
	free(freemap);
	free(rangemap);
	free(relocmap);
	free(reverse_map);
	free(freeblks);
	free(relocblks);

	return ret;
}
