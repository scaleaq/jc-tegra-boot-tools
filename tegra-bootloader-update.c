/*
 * tegra-bootloader-update.c
 *
 * Tool for updating/initializing Tegra boot partitions
 * using a BUP package.
 *
 * Copyright (c) 2019-2021, Matthew Madison
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <tegra-eeprom/cvm.h>
#include <zlib.h>
#include "bup.h"
#include "gpt.h"
#include "bct.h"
#include "smd.h"
#include "ver.h"
#include "util.h"
#include "config.h"

static struct option options[] = {
	{ "initialize",		no_argument,		0, 'i' },
	{ "slot-suffix",	required_argument,	0, 's' },
	{ "dry-run",		no_argument,		0, 'n' },
	{ "needs-repartition",	no_argument,		0, 'N' },
	{ "help",		no_argument,		0, 'h' },
	{ "version",		no_argument,		0, 0   },
	{ 0,			0,			0, 0   }
};
static const char *shortopts = ":ins:h";

static char *optarghelp[] = {
	"--initialize         ",
	"--slot-suffix        ",
	"--dry-run            ",
	"--needs-repartition  ",
	"--help               ",
	"--version            ",
};

static char *opthelp[] = {
	"initialize the entire set of boot partitions",
	"update only the redundant boot partitions with the specified suffix (with no SMD update)",
	"do not perform any writes, just show what would be written",
	"check if boot device needs repartitioning (T186/T194 only)",
	"display this help text",
	"display version information"
};

struct update_entry_s {
	char partname[64];
	gpt_entry_t *part;
	char devname[PATH_MAX];
	off_t bup_offset;
	size_t length;
};

struct update_list_s {
	unsigned int count;
	const char **partnames;
};

#define MAX_ENTRIES 64
static struct update_entry_s redundant_entries[MAX_ENTRIES];
static struct update_entry_s nonredundant_entries[MAX_ENTRIES];
static unsigned int redundant_entry_count;
static unsigned int nonredundant_entry_count;
static size_t contentbuf_size;
static size_t slotbuf_size;
static uint8_t *contentbuf, *slotbuf, *zerobuf;
static int bct_updated;
static tegra_soctype_t soctype = TEGRA_SOCTYPE_INVALID;
static bool spiboot_platform;
static unsigned long bootdev_size;

/*
 * For tegra210 platforms, these are the names of partitions
 * to be updated, **in order**. Note that only the eMMC-based
 * tegra210 platforms have redundant copies of most of the boot
 * partitions, and that the naming of the redundant NVC partition
 * is different between eMMC and SPIflash platforms.
 */
static const char *t210_emmc_partnames[] = {
	"VER_b", "BCT", "NVC-1",
	"PT-1", "TBC-1", "RP1-1", "EBT-1", "WB0-1", "BPF-1", "DTB-1", "TOS-1", "EKS-1", "LNX-1",
	"BCT",
	"BCT",
	"PT", "TBC", "RP1", "EBT", "WB0", "BPF", "DTB", "TOS", "EKS", "LNX",
	"NVC", "VER",
};
static const char *t210_spi_sd_partnames[] = {
	"VER_b", "BCT", "NVC_R",
	"BCT",
	"BCT",
	"PT", "TBC", "RP1", "EBT", "WB0", "BPF", "DTB", "TOS", "EKS", "LNX",
	"NVC", "VER",
};
static const struct update_list_s update_list_t210_emmc = {
	.count = sizeof(t210_emmc_partnames)/sizeof(t210_emmc_partnames[0]),
	.partnames = t210_emmc_partnames,
};
static const struct update_list_s update_list_t210_spi_sd = {
	.count = sizeof(t210_spi_sd_partnames)/sizeof(t210_spi_sd_partnames[0]),
	.partnames = t210_spi_sd_partnames,
};


/*
 * print_usage
 *
 * Does what it says, extracting option strings and
 * help from the arrays defined above.
 *
 * Returns: nothing
 */
static void
print_usage (void)
{
	int i;
	printf("\nUsage:\n");
	printf("\ttegra-bootloader-update <option> <bup-package-path>\n\n");
	printf("Options:\n");
	for (i = 0; i < sizeof(options)/sizeof(options[0]) && options[i].name != 0; i++) {
		printf(" %s\t%c%c\t%s\n",
		       optarghelp[i],
		       (options[i].val == 0 ? ' ' : '-'),
		       (options[i].val == 0 ? ' ' : options[i].val),
		       opthelp[i]);
	}
	printf("\nArguments:\n");
	printf(" <bup-package-path>\tpathname of bootloader update package\n");

} /* print_usage */

/*
 * read_completely_at
 *
 * Utility function for seeking to a specific offset
 * and reading a fixed number of bytes into a buffer,
 * handling short reads.
 *
 * fd: file descriptor
 * buf: pointer to read buffer
 * bufsiz: number of bytes to read
 * offset: offset from start of file/device
 *
 * Returns: number of bytes read, or
 *          -1 on error (errno set)
 *
 */
static ssize_t
read_completely_at (int fd, void *buf, size_t bufsiz, off_t offset)
{
	ssize_t n, total;
	size_t remain;

	if (lseek(fd, offset, SEEK_SET) == (off_t) -1)
		return -1;
	for (remain = bufsiz, total = 0; remain > 0; total += n, remain -= n) {
		n = read(fd, (uint8_t *) buf + total, remain);
		if (n <= 0)
			return -1;
	}
	return total;

} /* read_completely_at */

/*
 * write_completely_at
 *
 * Utility function for seeking to a specific offset
 * and writing a fixed number of bytes to a file or device,
 * handling short writes.
 *
 * fd: file descriptor
 * buf: pointer to data to be written
 * bufsiz: number of bytes to write
 * offset: offset from start of file/device
 *
 * Returns: number of bytes written, or
 *          -1 on error (errno set)
 *
 */
static ssize_t
write_completely_at (int fd, void *buf, size_t bufsiz, off_t offset, size_t erase_size)
{
	ssize_t n, total;
	size_t remain;

	if (lseek(fd, offset, SEEK_SET) == (off_t) -1)
		return -1;
	if (erase_size != 0) {
		for (remain = erase_size, total = 0; remain > 0; total += n, remain -= n) {
			n = write(fd, (uint8_t *) zerobuf + total, remain);
			if (n <= 0)
				return -1;
		}
		fsync(fd);
		if (lseek(fd, offset, SEEK_SET) == (off_t) -1)
			return -1;
	}
	for (remain = bufsiz, total = 0; remain > 0; total += n, remain -= n) {
		n = write(fd, (uint8_t *) buf + total, remain);
		if (n <= 0)
			return -1;
	}
	return total;

} /* write_completely_at */

/*
 * redundant_part_format
 *
 * Returns a printf-style format string for formatting
 * the name of a redundant partition, handling the differences
 * in naming conventions between different platform variants.
 *
 * partname: base name of partition
 *
 * Returns: character string pointer
 *
 */
static const char *redundant_part_format(const char *partname)
{
	if (soctype != TEGRA_SOCTYPE_210)
		return "%s_b";
	if (strcmp(partname, "NVC") == 0)
		return (spiboot_platform ? "%s_R" : "%s-1");
	if (strcmp(partname, "VER") == 0)
		return "%s_b";
	return "%s-1";

} /* redundant_part_format */

/*
 * update_bct
 *
 * Special handling for writing the BCT. Content to be
 * written expected to be present in contentbuf.
 *
 * For tegra186/tegra194 platforms only.
 *
 * A block is 16KiB or 32KiB and holds multiple slots;
 * each slot is an even number of "pages" in size, where
 * the page size is 512 bytes for eMMC devices and 2KiB for
 * SPI flash.
 * The Tegra bootrom can handle up to 63 blocks, but
 * in practice, only block 0 slots 0 & 1, and block 1 slot 0
 * are used.
 *
 * Write sequence is block 0/slot 1, then block 1/slot 0,
 * then block 0/slot 0.
 *
 * bootfd: file descriptor for boot device
 * curbct: pointer to buffer holding current BCT partition, previously read
 *         (NULL if initializing)
 * newbct: pointer to new BCT to write (maybe)
 * ent:    pointer to entry from the update payload
 *
 * returns: 0 on success, -1 on error (errno not set)
 *
 */
static int
update_bct (int bootfd, void *curbct, void *newbct, struct update_entry_s *ent)
{
	unsigned int block_size = (spiboot_platform ? 32768 : 16384);
	unsigned int page_size = (spiboot_platform ? 2048 : 512);
	size_t bctslotsize;
	int i;

	if (soctype == TEGRA_SOCTYPE_210) {
		printf("[INTERNAL ERROR]\n");
		fprintf(stderr, "Internal error: incorrect BCT update function for t210\n");
		return -1;
	}
	if (curbct != NULL) {
		if ((soctype == TEGRA_SOCTYPE_186 && !bct_update_valid_t18x(curbct, newbct)) ||
		    (soctype == TEGRA_SOCTYPE_194 && !bct_update_valid_t19x(curbct, newbct))) {
			printf("[FAIL]\n");
			fprintf(stderr, "Error: validation check failed for BCT update\n");
			return -1;
		}
	}

	bctslotsize = page_size * ((ent->length + (page_size-1)) / page_size);

	for (i = 0; i < 3; i++) {
		off_t offset = 0;
		switch (i) {
			case 0:
				offset = (off_t) bctslotsize;
				break;
			case 1:
				offset = block_size;
				break;
			case 2:
				offset = 0;
				break;
		}
		if (curbct != NULL && memcmp(newbct, (uint8_t *)curbct + offset, ent->length) == 0)
			printf("[offset=%lu,no update needed]...", (unsigned long) offset);
		else {
			printf("[offset=%lu]...", (unsigned long) offset);
			if (write_completely_at(bootfd, newbct, ent->length, ent->part->first_lba * 512 + offset, bctslotsize) < 0) {
				printf("[FAIL]\n");
				perror("BCT");
				return -1;
			}
		}

	}

	fsync(bootfd);
	bct_updated = 1;
	printf("[OK]\n");
	return 0;

} /* update_bct */

/*
 * update_bct_t210
 *
 * Handles BCT updates to t210 platforms.
 *
 * On t210, there are up to 64 copies of the BCT.  Ordering is:
 *  Last entry, (other updates), middle entries, (other updates), first entry
 *
 * SPI flash platforms put two copies at block 0; MMC platforms put one.
 * All other entries start at beginning of a block.
 *
 * The 'which' argument is:
 *   < 0 -> update last BCT entry
 *   = 0 -> update first BCT entry
 *  other -> update middle entries (number varies by platform)
 *
 * Caller must initialize 'which' to -1 before the first call, since
 * the last BCT is always updated first.  This function will update
 * it with each call.
 *
 * Note that we use 0-based counts for the redundant partitions
 * (BCT, BCT-1, BCT-2, ... BCT-63), as opposed to 1-based counts
 * (BCT, BCT-2, BCT-3, ... BCT-64) used in the update script provided
 * in the L4T BSP.
 *
 * bootfd: file descriptor for boot device
 * curbct: pointer to buffer holding current BCT partition, previously read
 *         (NULL if initializing)
 * newbct: pointer to new BCT to write (maybe)
 * ent:    pointer to entry from the update payload
 * which:  pointer to BCT update context, see above
 *
 * returns: 0 on success, -1 on error (errno not set)
 */
static int
update_bct_t210 (int bootfd, void *curbct, void *newbct, struct update_entry_s *ent, int *which)
{
	unsigned int block_size = (spiboot_platform ? 32768 : 16384);
	unsigned int page_size = (spiboot_platform ? 2048 : 512);
	unsigned int bctcopies = (spiboot_platform ? 2 : 1);
	unsigned int bctpartsize;
	int bctcount, bctstart, bctend, bctidx;
	char bctname[32];
	static const char indent[] = "                    "; // length of 'Processing BCT... leader
	const char *prefix;

	if (soctype != TEGRA_SOCTYPE_210) {
		printf("[INTERNAL ERROR]\n");
		fprintf(stderr, "Internal error: incorrect BCT function for non-t210\n");
		return -1;
	}
	if (which == NULL) {
		printf("[INTERNAL ERROR]\n");
		fprintf(stderr, "Internal error: no BCT selection context for t210 update\n");
		return -1;
	}
	if (curbct != NULL && !bct_update_valid_t21x(curbct, newbct, &block_size, &page_size)) {
		printf("[FAIL]\n");
		fprintf(stderr, "Error: validation check failed for BCT update\n");
		return -1;
	}
	if (ent->length % page_size != 0) {
		printf("[FAIL]\n");
		fprintf(stderr, "Error: BCT update payload not an even multiple of boot device page size\n");
		return -1;
	}
	if (ent->length * bctcopies > block_size) {
		printf("[FAIL]\n");
		fprintf(stderr, "Error: %u BCT payload%s too large for boot device block size\n",
			bctcopies, (bctcopies == 1 ? "" : "s"));
		return -1;
	}
	bctpartsize = (ent->part->last_lba - ent->part->first_lba + 1) * 512;
	bctcount =  bctpartsize / block_size;
	if (bctcount > 64)
		bctcount = 64;

	if (*which < 0) {
		bctstart = bctend = bctcount - 1;
		/* Write middle entries next */
		*which = 1;
	} else if (*which == 0) {
		bctstart = bctend = 0;
		/* End of the line, just reset back to last */
		*which = -1;
	} else {
		bctstart = bctcount - 2;
		bctend = 1;
		/* Write first BCT next */
		*which = 0;
	}
	prefix = "";
	for (bctidx = bctstart; bctidx >= bctend; bctidx -= 1, prefix = indent) {
		off_t offset = bctidx * block_size;
		if (bctidx == 0)
			strcpy(bctname, "BCT");
		else
			sprintf(bctname, "BCT-%u", bctidx);

		if (curbct != NULL && memcmp(newbct, (uint8_t *)curbct + offset, ent->length) == 0) {
			printf("%s%s: [no update needed]\n", prefix, bctname);
			continue;
		}

		printf("%s%s: ", prefix, bctname);
		fflush(stdout);
		if (write_completely_at(bootfd, contentbuf, ent->length, ent->part->first_lba * 512 + offset, ent->length) < 0) {

			printf("[FAIL]\n");
			perror("BCT");
			return -1;
		}
		if (bctidx == 0 && bctcopies == 2) {
			offset += ent->length;
			if (write_completely_at(bootfd, contentbuf, ent->length, ent->part->first_lba * 512 + offset, ent->length) < 0) {
				printf("[FAIL]\n");
				perror("BCT");
				return -1;
			}
		}
		printf("[OK]\n");
	}
	fsync(bootfd);
	bct_updated = 1;
	return 0;

} /* update_bct_t210 */

/*
 * maybe_update_bootpart
 *
 * Update a boot partition if its current contents
 * differ from the BUP content (which is in contentbuf).
 *
 * On systems that boot from eMMC, boot partitions may be
 * located either in /dev/mmcblk0boot0 (called the "boot device")
 * or /dev/mmcblk0boot1 (called the "GPT device").
 *
 * bootfd: file descriptor for boot device
 * gptfd:  file descriptor for second boot (aka "GPT") device
 * ent:    pointer to entry from update payload
 * is_bct: 1 if this is a BCT update, 0 otherwise
 * initialize: non-zero if initializing, 0 otherwise
 * bctctx: 'which' context for BCT updates (for t210 platforms)
 *
 * Returns: 0 on success, -1 on error (errno not set)
 *
 */
static int
maybe_update_bootpart (int bootfd, int gptfd, struct update_entry_s *ent,
		       int is_bct, int initialize, int *bctctx)
{
	int fd;
	size_t partsize = (ent->part->last_lba - ent->part->first_lba + 1) * 512;
	off_t offset;

	if (ent->length > partsize) {
		printf("[FAIL]\n");
		fprintf(stderr, "Error: BUP contents too large for boot partition\n");
		return -1;
	}
	fd = bootfd;
	offset = ent->part->first_lba * 512;
	if (offset >= bootdev_size) {
		if (gptfd < 0) {
			printf("[FAIL]\n");
			fprintf(stderr, "Partition %s starts past end of boot device\n", ent->partname);
			return -1;
		}
		fd = gptfd;
		offset -= bootdev_size;
	}
	if (read_completely_at(fd, slotbuf, partsize, offset) < 0) {
		printf("[FAIL]\n");
		perror(ent->partname);
		return -1;
	}
	if (is_bct)
		return (soctype == TEGRA_SOCTYPE_210
			? update_bct_t210(bootfd, (initialize ? NULL : slotbuf), contentbuf, ent, bctctx)
			: update_bct(bootfd, (initialize ? NULL : slotbuf), contentbuf, ent));

	if (memcmp(contentbuf, slotbuf, ent->length) == 0) {
		printf("[no update needed]\n");
		return 0;
	}

	if (write_completely_at(fd, contentbuf, ent->length, offset, partsize) < 0) {
		printf("[FAIL]\n");
		perror(ent->partname);
		return -1;
	}

	fsync(fd);
	printf("[OK]\n");
	return 0;

} /* maybe_update_bootpart */

/*
 * process_entry
 *
 * Processes an entry from the update payload.
 *
 * bupctx: context pointer for the open BUP payload
 * bootfd: file descriptor for the boot device
 * gptfd:  file descriptor for the "GPT" device
 * ent:    pointer to update payload entry to process
 * dryrun: true for a dry run (no writes)
 * initialize: non-zero if initializing (skip BCT comparison check)
 * bctctx: pointer to 'which' contenxt for t210 BCT updates
 *
 * Returns: 0 on success, -1 on error (errno not set)
 *
 */
static int
process_entry (bup_context_t *bupctx, int bootfd, int gptfd, struct update_entry_s *ent,
	       bool dryrun, int initialize, int *bctctx)
{
	ssize_t total, n;
	unsigned int erase_size;
	int fd;

	printf("  Processing %s... ", ent->partname);
	fflush(stdout);
	if (bup_setpos(bupctx, ent->bup_offset) == (off_t) -1) {
		printf("[FAIL]\n");
		fprintf(stderr, "could not set position for %s\n", ent->partname);
		return -1;
	}
	for (total = 0; total < ent->length; total += n) {
		n = bup_read(bupctx, contentbuf + total, contentbuf_size - total);
		if (n <= 0) {
			printf("[FAIL]\n");
			fprintf(stderr, "error reading content for %s\n", ent->partname);
			return -1;
		}
	}

	if (dryrun) {
		printf("[OK] (dry run)\n");
		return 0;
	}
	if (ent->part != NULL)
		return maybe_update_bootpart(bootfd, gptfd, ent, strcmp(ent->partname, "BCT") == 0, initialize, bctctx);

	fd = open(ent->devname, O_RDWR);
	if (fd < 0) {
		printf("[FAIL]\n");
		perror(ent->devname);
		return -1;
	}

	erase_size = lseek(fd, 0, SEEK_END);
	if (erase_size < 0) {
		printf("[FAIL]\n");
		perror(ent->devname);
		close(fd);
		return -1;
	}

	if (lseek(fd, 0, SEEK_SET) < 0) {
		printf("[FAIL]\n");
		perror(ent->devname);
		close(fd);
		return -1;
	}

	if (write_completely_at(fd, contentbuf, ent->length, 0, erase_size) < 0) {
		printf("[FAIL]\n");
		perror(ent->devname);
		close(fd);
		return -1;
	}

	fsync(fd);
	close(fd);
	printf("[OK]\n");
	return 0;

} /* process_entry */

/*
 * order_entries
 *
 * Sorts an entries list to ensure that we process
 * mb2/mb2_b before BCT before mb1/mb1_b.  For
 * tegra186/tegra194 platforms only.
 *
 * orig: array of payload entries to process
 * ordered: array of pointers to be filled by this function
 * count: length of the arrays
 *
 * Returns: nothing
 */
static void
order_entries (struct update_entry_s *orig, struct update_entry_s **ordered, unsigned int count)
{
	int mb1, mb1_b, bct, bct1, bct2, mb2, mb2_b;
	unsigned int i, j;

	mb1 = mb1_b = bct = bct1 = bct2 = mb2 = mb2_b = -1;
	j = 0;
	for (i = 0; i < count; i++) {
		if (strcmp(orig[i].partname, "mb1") == 0)
			mb1 = i;
		else if (strcmp(orig[i].partname, "mb1_b") == 0)
			mb1_b = i;
		else if (strcmp(orig[i].partname, "mb2") == 0)
			mb2 = i;
		else if (strcmp(orig[i].partname, "mb2_b") == 0)
			mb2_b = i;
		else if (strcmp(orig[i].partname, "BCT") == 0){
			if (bct == -1)
				bct = i;
			else if (bct1 == -1)
				bct1 = i;
			else if (bct2 == -1)
				bct2 = i;
		}
		else
			ordered[j++] = &orig[i];
	}

	if (mb2 >= 0)
		ordered[j++] = &orig[mb2];
	if (mb2_b >= 0)
		ordered[j++] = &orig[mb2_b];
	if (bct >= 0)
		ordered[j++] = &orig[bct];
	if (bct1 >= 0)
		ordered[j++] = &orig[bct1];
	if (bct2 >= 0)
		ordered[j++] = &orig[bct2];
	if (mb1 >= 0)
		ordered[j++] = &orig[mb1];
	if (mb1_b >= 0)
		ordered[j++] = &orig[mb1_b];

	if (j != count)
		fprintf(stderr, "Warning: ordered entry list mismatch\n");

} /* order_entries */

/*
 * find_entry_by_name
 *
 * Returns a pointer to the update entry for a named partition.
 *
 * list: array of update entries
 * count: size of the array
 * name: name to locate in the array
 *
 * Returns: NULL on error, valid pointer otherwise
 */
static struct update_entry_s *
find_entry_by_name (struct update_entry_s *list, unsigned int count, const char *name)
{
	unsigned int i;
	for (i = 0; i < count; i += 1)
		if (strcmp(list[i].partname, name) == 0)
			return &list[i];
	return NULL;

} /* find_entry_by_name */

/*
 * order_entries_t210
 *
 * Builds an array of pointers to update entries for
 * performing partition updates in the correct order
 * on tegra210 systems.
 *
 * Note that on tegra210s (unlike tegra186/tegra194), the
 * ordered list will be longer than the original list, since
 * BCT updates are handled in multiple parts (last, middle, first),
 * with each update pointing back to the same original entry.
 *
 * Entries that do not appear in the fixed-order list are
 * appended to the end.
 *
 * orig: array of payload entries to process
 * ordered: array of pointers to be filled by this function
 * count: length of the arrays
 *
 * Returns: number of entries in the ordered list
 */
static unsigned int
order_entries_t210 (struct update_entry_s *orig, struct update_entry_s **ordered, unsigned int count)
{
	const struct update_list_s *update_list = (spiboot_platform
						   ? &update_list_t210_spi_sd
						   : &update_list_t210_emmc);
	struct update_entry_s *ent;
	unsigned int i, retcount;
	bool used[128];

	if (count > sizeof(used)/sizeof(used[0])) {
		fprintf(stderr, "Internal error: update entry list too long\n");
		return 0;
	}
	memset(used, 0, sizeof(used));
	for (i = 0, retcount = 0; i < update_list->count; i++) {
		ent = find_entry_by_name(orig, count, update_list->partnames[i]);
		if (ent == NULL) {
			/* EKS partitions are optional */
			if (memcmp(update_list->partnames[i], "EKS", 3) == 0)
				continue;
			else {
				fprintf(stderr, "Error: payload or partition not found for %s\n",
					update_list->partnames[i]);
				return 0;
			}
		}
		ordered[retcount++] = ent;
		used[ent-orig] = true;
	}
	for (i = 0; i < count; i++) {
		if (!used[i])
			ordered[retcount++] = &orig[i];
	}
	return retcount;

} /* order_entries_t210 */

/*
 * nvc_partitions_match
 *
 * Checks (via CRC computation) that the NVC partition
 * and its backup (NVC_R or NVC-1, depending) are identical.
 *
 * bootfd: fd to boot device
 * gptfd:  fd to gpt device
 * nvc:    array of two update entries - primary and backup NVC
 *
 * Returns:
 *    true:  NVCs are present with identical contents
 *    false: either an NVC is missing or the contents do not match
 */
static bool
nvc_parts_match (int bootfd, int gptfd, struct update_entry_s *nvc[2])
{
	int i, fd;
	off_t offset;
	size_t partsize;
	uint32_t crc[2];

	if (nvc[0] == NULL || nvc[0]->part == NULL || nvc[1] == NULL || nvc[1]->part == NULL)
		return false;

	for (i = 0; i < 2; i++) {
		fd = bootfd;
		offset = nvc[i]->part->first_lba * 512;
		partsize = (nvc[i]->part->last_lba - nvc[i]->part->first_lba + 1) * 512;
		if (offset >= bootdev_size) {
			fd = gptfd;
			offset -= bootdev_size;
		}
		if (read_completely_at(fd, slotbuf, partsize, offset) < 0)
			return false;
		crc[i] = crc32(0, slotbuf, partsize);
	}

	return crc[0] == crc[1];

} /* nvc_parts_match */

/*
 * invalid_version_or_downgrade
 *
 * Performs checks on the current version info partitions vs.
 * the information in the payload, and returns true if the
 * update should not be performed because (a) the version partitions
 * are corrupted, or (b) the payload was built from an older BSP
 * version.
 *
 * Logic should be equivalent to that in the L4T updater script.
 *
 * bupctx: BUP context pointer
 * bootfd: fd for the boot device
 * gptfd:  fd for the GPT device
 * entry_list: list of BUP entries to be processed
 * entry_count: number of entries in list
 * force_initialize: don't check for downgrade or bad VER partitions
 *
 * Returns:
 *    true:  cannot apply the update
 *    false: OK to apply the update
 */
static bool
invalid_version_or_downgrade (bup_context_t *bupctx, int bootfd, int gptfd,
			      struct update_entry_s *entry_list, size_t entry_count,
			      bool force_initialize)
{
	struct update_entry_s *ver[2], *nvc[2];
	struct ver_info_s verinfo[2], bup_verinfo;
	off_t offset;
	char ver_b_name[64], nvc_b_name[64];
	ssize_t total, n;
	unsigned int i;
	int fd;

	ver[0] = ver[1] = nvc[0] = nvc[1]  = NULL;
	sprintf(ver_b_name, redundant_part_format("VER"), "VER");
	sprintf(nvc_b_name, redundant_part_format("NVC"), "NVC");
	for (i = 0; i < entry_count; i += 1)
		if (strcmp(entry_list[i].partname, "VER") == 0)
			ver[0] = &entry_list[i];
		else if (strcmp(entry_list[i].partname, "NVC") == 0)
			nvc[0] = &entry_list[i];
		else if (strcmp(entry_list[i].partname, ver_b_name) == 0)
			ver[1] = &entry_list[i];
		else if (strcmp(entry_list[i].partname, nvc_b_name) == 0)
			nvc[1] = &entry_list[i];

	/*
	 * Update payloads that do not update the boot chain do not contain
	 * a VER entry, and that's OK
	 */
	if (ver[0] == NULL)
		return false;

	/*
	 * Read the version info from the payload
	 */
	if (bup_setpos(bupctx, ver[0]->bup_offset) == (off_t) -1) {
		fprintf(stderr, "Error: could not find version info in BUP payload\n");
		return true;
	}
	for (total = 0; total < ver[0]->length; total += n) {
		n = bup_read(bupctx, contentbuf + total, contentbuf_size - total);
		if (n <= 0) {
			fprintf(stderr, "Error reading version info from BUP payload");
			return true;
		}
	}
	if (ver_extract_info(contentbuf, ver[0]->length, &bup_verinfo) != 0) {
		fprintf(stderr, "Error validating version info in BUP payload: %s\n",
			strerror(errno));
		return true;
	}

	for (i = 0; i < 2; i += 1) {
		size_t partsize;
		memset(&verinfo[i], 0, sizeof(verinfo[i]));
		if (ver[i] == NULL)
			continue;
		if (ver[i]->part == NULL) {
			fprintf(stderr, "Error locating %s partition\n", ver[i]->partname);
			return true;
		}
		fd = bootfd;
		offset = ver[i]->part->first_lba * 512;
		if (offset >= bootdev_size) {
			fd = gptfd;
			offset -= bootdev_size;
		}
		partsize = (ver[i]->part->last_lba - ver[i]->part->first_lba + 1) * 512;
		if (read_completely_at(fd, slotbuf, partsize, offset) < 0) {
			fprintf(stderr, "Error reading %s partition: %s\n", ver[i]->partname, strerror(errno));
			return true;
		}
		/*
		 * We don't check the return code here, since we can recover if
		 * just one is valid, in some cases
		 */
		ver_extract_info(slotbuf, partsize, &verinfo[i]);
	}
	/*
	 * If both version partitions match and have a non-zero version (thus are valid),
	 * check for a rollback - downgrading can brick the device, so don't allow it.
	 */
	if (verinfo[0].bsp_version == verinfo[1].bsp_version && verinfo[0].bsp_version != 0) {
		if (verinfo[0].bsp_version > bup_verinfo.bsp_version) {
			fprintf(stderr, "Error: current bootloader version is %u.%u.%u; cannot roll back to %u.%u.%u\n",
				bsp_version_major(verinfo[0].bsp_version),
				bsp_version_minor(verinfo[0].bsp_version),
				bsp_version_maint(verinfo[0].bsp_version),
				bsp_version_major(bup_verinfo.bsp_version),
				bsp_version_minor(bup_verinfo.bsp_version),
				bsp_version_maint(bup_verinfo.bsp_version));
			return true;
		}
		/*
		 * Validate that the last update was completely applied by comparing the
		 * NVC partition against its redundant copy.  If there's a mismatch, something
		 * went wrong and we cannot apply the update.
		 */
		if (verinfo[0].crc == verinfo[1].crc && !nvc_parts_match(bootfd, gptfd, nvc)) {
			fprintf(stderr, "Error: NVC partition mismatch - reflash required\n");
			return true;
		}

		/*
		 * This is OK - no further checks required
		 */
		return false;
	}

	/*
	 * If the VER_b partition is invalid, but the primary VER is valid, that's OK - just check for a rollback.
	 * Otherwise, if VER_b is valid, the update can be applied if the BUP version exactly matches VER_b's version.
	 * Otherwise, if none of the checks have worked so far, something is wrong.
	 */
	if (verinfo[1].bsp_version == 0 && verinfo[0].bsp_version != 0 &&
	    verinfo[0].bsp_version > bup_verinfo.bsp_version) {
		if (force_initialize) {
			fprintf(stderr, "Warning: downgrading bootloader from %u.%u.%u to %u.%u.%u\n",
				bsp_version_major(verinfo[0].bsp_version),
				bsp_version_minor(verinfo[0].bsp_version),
				bsp_version_maint(verinfo[0].bsp_version),
				bsp_version_major(bup_verinfo.bsp_version),
				bsp_version_minor(bup_verinfo.bsp_version),
				bsp_version_maint(bup_verinfo.bsp_version));
			return false;
		}
		fprintf(stderr, "Error: current bootloader version is %u.%u.%u; cannot downgrade to %u.%u.%u\n",
			bsp_version_major(verinfo[0].bsp_version),
			bsp_version_minor(verinfo[0].bsp_version),
			bsp_version_maint(verinfo[0].bsp_version),
			bsp_version_major(bup_verinfo.bsp_version),
			bsp_version_minor(bup_verinfo.bsp_version),
			bsp_version_maint(bup_verinfo.bsp_version));
		return true;
	} else if (verinfo[1].bsp_version != 0 && verinfo[1].bsp_version != bup_verinfo.bsp_version) {
		fprintf(stderr, "Error: previous update was incomplete; please update with version %u.%u.%u\n",
			bsp_version_major(verinfo[1].bsp_version),
			bsp_version_minor(verinfo[1].bsp_version),
			bsp_version_maint(verinfo[0].bsp_version));
		return true;
	} else if (force_initialize) {
		fprintf(stderr, "Warning: bootloader version partitions were corrupted\n");
		return false;
	} else {
		fprintf(stderr, "Error: bootloader version partitions are corrupted; cannot apply update\n");
		return true;
	}

	return false;

} /* invalid_version_or_downgrade */

/*
 * find_largest_partition
 *
 * Locates the largest partition to be updated, for
 * allocating the buffers used for holding and
 * erasing partition contents.
 *
 * sizep: pointer to size_t to hold result
 *
 * Returns: 0 on success, -1 on error
 */
static int
find_largest_partition (size_t *sizep)
{
	int i;
	size_t largest = 0;
	size_t partlen;

	for (i = 0; i < redundant_entry_count; i++) {
		struct update_entry_s *ent = &redundant_entries[i];
		if (ent->part != NULL)
			partlen = (ent->part->last_lba - ent->part->first_lba + 1) * 512;
		else {
			off_t offset;
			int fd = open(ent->devname, O_RDONLY);
			if (fd < 0)
				goto error_depart;
			offset = lseek(fd, 0, SEEK_END);
			close(fd);
			if (offset == (off_t) -1)
				goto error_depart;
			partlen = (size_t) offset;
		}
		if (partlen > largest)
			largest = partlen;
	}
	for (i = 0; i < nonredundant_entry_count; i++) {
		struct update_entry_s *ent = &nonredundant_entries[i];
		if (ent->part != NULL)
			partlen = (ent->part->last_lba - ent->part->first_lba + 1) * 512;
		else {
			off_t offset;
			int fd = open(ent->devname, O_RDONLY);
			if (fd < 0)
				goto error_depart;
			offset = lseek(fd, 0, SEEK_END);
			close(fd);
			if (offset == (off_t) -1)
				goto error_depart;
			partlen = (size_t) offset;
		}
		if (partlen > largest)
			largest = partlen;
	}
	*sizep = 512 * ((largest + 511) / 512);
	return 0;
error_depart:
	return -1;

} /* find_largest_partition */


/*
 * main program
 */
int
main (int argc, char * const argv[])
{
	int c, which, fd = -1, gptfd = -1, err;
	bool reset_bootdev = false, reset_gptdev = false;
	gpt_context_t *gptctx = NULL;
	bup_context_t *bupctx = NULL;
	smd_context_t *smdctx = NULL;
	struct update_entry_s updent;
	void *bupiter;
	const char *partname;
	const char *suffix = NULL;
	const char *bootdev;
	off_t offset;
	size_t length;
	size_t largest_length;
	unsigned int version;
	char pathname[PATH_MAX];
	int initialize = 0;
	bool dryrun = false;
	int ret = 1;
	int missing_count;
	int curslot = -1;
	bool slot_specified = false;
	const char *missing[32];
	struct update_entry_s *ordered_entries[MAX_ENTRIES], mb1_other;
	unsigned int i;
	off_t bootdev_end_offset;
	bool check_only = false;

	while ((c = getopt_long_only(argc, argv, shortopts, options, &which)) != -1) {
		switch (c) {
			case 'h':
				print_usage();
				return 0;
			case 'i':
				if (suffix != NULL) {
					fprintf(stderr, "Error: cannot use --initialize with --slot-suffix\n");
					print_usage();
					return 1;
				}
				initialize = 1;
				break;
			case 's':
				if (initialize) {
					fprintf(stderr, "Error: cannot specify --slot-suffix with --initialize\n");
					print_usage();
					return 1;
				}
				if (strcmp(optarg, "_a") == 0)
					suffix = "";
				else
					suffix = optarg;
				if (suffix[0] != '\0' && strcmp(suffix, "_b") != 0) {
					fprintf(stderr, "Error: slot suffix must be either _a or _b\n");
					print_usage();
					return 1;
				}
				slot_specified = true;
				break;
			case 'n':
				dryrun = true;
				break;
			case 'N':
				check_only = dryrun = true;
				break;
			case 0:
				if (strcmp(options[which].name, "version") == 0) {
					printf("%s\n", VERSION);
					return 0;
				}
				/* fallthrough */
			default:
				fprintf(stderr, "Error: unrecognized option\n");
				print_usage();
				return 1;
		}
	}

	if (optind >= argc && !check_only) {
		fprintf(stderr, "Error: missing required argument\n");
		print_usage();
		return 1;
	}

	if (soctype == TEGRA_SOCTYPE_INVALID) {
		soctype = cvm_soctype();
		if (soctype == TEGRA_SOCTYPE_INVALID) {
			fprintf(stderr, "Error: could not determine SoC type\n");
			return 1;
		}
	}

	if (soctype == TEGRA_SOCTYPE_186 ||
	    soctype == TEGRA_SOCTYPE_194) {
		if (!slot_specified && !initialize) {
			curslot = smd_get_current_slot();
			if (curslot < 0) {
				perror("retrieving current boot slot");
				return 1;
			}
			if (curslot == 0)
				suffix = "_b";
			else
				suffix = "";
		}
	} else if (soctype == TEGRA_SOCTYPE_210) {
		if (slot_specified) {
			fprintf(stderr, "Error: unsupported operation for t210 platform\n");
			return 1;
		}
		/*
		 * On t210, the operation is always 'initialize'.
		 * If the user explicitly used the -i option, treat that
		 * as a forced initialization, even if the version checks fail.
		 */
		initialize += 1;
	} else {
		fprintf(stderr, "Error: unrecognized SoC type\n");
		return 1;
	}

	bupctx = bup_init(argv[optind]);
	if (bupctx == NULL) {
		perror(argv[optind]);
		return 1;
	}

	bootdev = bup_boot_device(bupctx);
	if (strlen(bootdev) < 8) {
		fprintf(stderr, "Error: unrecognized boot device: %s\n", bootdev);
		bup_finish(bupctx);
		return 1;
	}

	if (memcmp(bootdev, "/dev/mtd", 8) == 0)
		spiboot_platform = true;
	else if (memcmp(bootdev, "/dev/mmc", 8) != 0) {
		fprintf(stderr, "Error: unrecognized boot device: %s\n", bootdev);
		bup_finish(bupctx);
		return 1;
	}

	if (spiboot_platform)
		gptfd = -1;
	else {
		if (dryrun)
			gptfd = open(bup_gpt_device(bupctx), O_RDONLY);
		else {
			reset_gptdev = set_bootdev_writeable_status(bup_gpt_device(bupctx), true);
			gptfd = open(bup_gpt_device(bupctx), O_RDWR);
		}
		if (gptfd < 0) {
			perror(bup_gpt_device(bupctx));
			goto reset_and_depart;
		}
	}

	if (dryrun)
		fd = open(bootdev, O_RDONLY);
	else {
		reset_bootdev = set_bootdev_writeable_status(bootdev, true);
		fd = open(bootdev, O_RDWR);
	}
	if (fd < 0) {
		perror(bootdev);
		goto reset_and_depart;
	}

	gptctx = gpt_init(bup_gpt_device(bupctx), 512, (initialize && !dryrun) ? GPT_INIT_FOR_WRITING : 0);
	if (gptctx == NULL) {
		perror("boot sector GPT");
		goto reset_and_depart;
	}

	if (check_only) {
		if (soctype == TEGRA_SOCTYPE_210) {
			/*
			 * t210 platforms have no GPT in the boot device, --initialize will force rewrite anyway,
			 * so just return > 0 to indicate a full erasure is not required.
			 */
			ret = 1;
			goto reset_and_depart;
		}
		err = gpt_load(gptctx, GPT_BACKUP_ONLY|GPT_NVIDIA_SPECIAL);
		if (err != 0) {
			ret = 0;
			goto reset_and_depart;
		}
		err = gpt_layout_config_match(gptctx);
		if (err < 0) {
			fprintf(stderr, "could not compare existing boot partition layout with configuration");
			ret = 2;
		} else
			ret = (err == 0) ? 1 : 0;
		goto reset_and_depart;
	}

	if (initialize)
		err = gpt_load_from_config(gptctx);
	else
		err = gpt_load(gptctx, GPT_BACKUP_ONLY|GPT_NVIDIA_SPECIAL);

	if (err != 0) {
		fprintf(stderr, "Error: cannot load boot sector partition table\n");
		goto reset_and_depart;
	}

	if (initialize && !dryrun && soctype != TEGRA_SOCTYPE_210) {
		err = gpt_save(gptctx, GPT_BACKUP_ONLY|GPT_NVIDIA_SPECIAL);
		if (err != 0) {
			fprintf(stderr, "Error: could not initialize boot sector partition table\n");
			goto reset_and_depart;
		}
	}

	bootdev_end_offset = lseek(fd, 0, SEEK_END);
	if (bootdev_end_offset == (off_t) -1) {
		perror(bootdev);
		goto reset_and_depart;
	}
	bootdev_size = (unsigned long) bootdev_end_offset;
	lseek(fd, 0, SEEK_SET);

	if (soctype == TEGRA_SOCTYPE_210)
		smdctx = NULL;
	else if (initialize) {
		smdctx = smd_new(REDUNDANCY_FULL);
		if (smdctx == NULL) {
			perror("initializing slot metadata");
			goto reset_and_depart;
		}
	} else {
		smdctx = smd_init(gptctx, fd);
		if (smdctx == NULL) {
			perror("loading slot metadata");
			goto reset_and_depart;
		}
	}

	if (smdctx) {
		if (!slot_specified && smd_redundancy_level(smdctx) != REDUNDANCY_FULL) {
			if (dryrun)
				printf("[skip] enable redundancy in slot metadata\n");
			else if (smd_set_redundancy_level(smdctx, REDUNDANCY_FULL) < 0) {
				perror("enabling redundancy in slot metadata");
				goto reset_and_depart;
			}
		}
	}

	missing_count = bup_find_missing_entries(bupctx, missing, sizeof(missing)/sizeof(missing[0]));
	if (missing_count < 0) {
		fprintf(stderr, "Error checking BUP payload for missing entries\n");
		goto reset_and_depart;
	} else if (missing_count > 0) {
		int m;
		fprintf(stderr, "Error: missing entries for partition%s: %s",
			(missing_count == 1 ? "" : "s"), missing[0]);
		for (m = 1; m < missing_count; m++)
			fprintf(stderr, ", %s", missing[m]);
		fprintf(stderr, "\n       for TNSPEC %s\n", bup_tnspec(bupctx));
		goto reset_and_depart;
	}

	/*
	 * Verify that all of the partitions we need to update are acutally present,
	 * and at the same time build the set of update tasks.
	 *
	 * For initialization, we separate the redundant entries from the non-redundant
	 * ones and write the non-redundant entries last. While the BCT appears to
	 * be non-redundant (only one BCT partition), it is internally redundant and
	 * requires special handling (which is different for tegra186/194 vs tegra210).
	 *
	 * For updates on tegra186/194 platforms, we never write the non-redundant entries.
	 * For tegra210 platforms, `initialize` is always set, since those platforms are
	 * not A/B redundant.
	 */
	bupiter = 0;
	redundant_entry_count = nonredundant_entry_count = 0;
	largest_length = 0;
	memset(&mb1_other, 0, sizeof(mb1_other));
	printf("Native TNSPEC:   %s\n", bup_tnspec(bupctx));
	if (bup_compat_spec(bupctx) != NULL)
		printf("Compatible with: %s\n", bup_compat_spec(bupctx));
	while (bup_enumerate_entries(bupctx, &bupiter, &partname, &offset, &length, &version)) {
		gpt_entry_t *part, *part_b;
		char partname_b[64], pathname_b[PATH_MAX];

		sprintf(partname_b, redundant_part_format(partname), partname);
		memset(&updent, 0, sizeof(updent));
		strcpy(updent.partname, partname);
		updent.bup_offset = offset;
		updent.length = length;
		if (length > largest_length)
			largest_length = length;

		part = gpt_find_by_name(gptctx, partname);
		if (part != NULL) {
			/*
			 * Partition is located in the boot device
			 */
			part_b = gpt_find_by_name(gptctx, partname_b);
			if (initialize) {
				if (part_b != NULL || strcmp(partname, "BCT") == 0) {
					if (redundant_entry_count >= sizeof(redundant_entries)/sizeof(redundant_entries[0])) {
						fprintf(stderr, "too many partitions to initialize\n");
						goto reset_and_depart;
					}
					redundant_entries[redundant_entry_count] = updent;
					redundant_entries[redundant_entry_count].part = part;
					redundant_entry_count += 1;
					if (part_b != NULL) {
						redundant_entries[redundant_entry_count] = updent;
						strcpy(redundant_entries[redundant_entry_count].partname, partname_b);
						redundant_entries[redundant_entry_count].part = part_b;
						redundant_entry_count += 1;
					}
				} else {
					if (nonredundant_entry_count >= sizeof(nonredundant_entries)/sizeof(nonredundant_entries[0])) {
						fprintf(stderr, "too many (non-redundant) partitions to initialize\n");
						goto reset_and_depart;
					}
					nonredundant_entries[nonredundant_entry_count] = updent;
					nonredundant_entries[nonredundant_entry_count].part = part;
					nonredundant_entry_count += 1;
				}
			} else if (part_b != NULL || strcmp(partname, "BCT") == 0) {
				if (redundant_entry_count >= sizeof(redundant_entries)/sizeof(redundant_entries[0])) {
					fprintf(stderr, "too many partitions to update\n");
					goto reset_and_depart;
				}
				redundant_entries[redundant_entry_count] = updent;
				strcpy(redundant_entries[redundant_entry_count].partname,
				       (part_b == NULL || *suffix == '\0' ? partname : partname_b));
				redundant_entries[redundant_entry_count].part = (part_b == NULL || *suffix == '\0' ? part : part_b);
				/*
				 * Save the info for the other mb1 entry, in case the BCT
				 * was updated and we need to update both mb1's
				 */
				if (strcmp(partname, "mb1") == 0) {
					mb1_other = updent;
					strcpy(mb1_other.partname, (*suffix == '\0' ? partname_b : partname));
					mb1_other.part = (*suffix == '\0' ? part_b : part);
				}
				redundant_entry_count += 1;
			}
		} else {
			/*
			 * Normal partition, not in the boot device
			 */
			int redundant;
			sprintf(pathname, "/dev/disk/by-partlabel/%s", partname);
			if (access(pathname, F_OK|W_OK) != 0) {
				if (partition_should_be_present(partname)) {
					fprintf(stderr, "Error: cannot locate partition: %s\n", partname);
					goto reset_and_depart;
				} else
					continue;
			}
			strcpy(pathname_b, "/dev/disk/by-partlabel/");
			sprintf(pathname_b + strlen(pathname_b), redundant_part_format(partname), partname);
			redundant = access(pathname_b, F_OK|W_OK) == 0;
			if (initialize) {
				if (redundant) {
					redundant_entries[redundant_entry_count] = updent;
					strcpy(redundant_entries[redundant_entry_count].devname, pathname);
					redundant_entry_count += 1;
					redundant_entries[redundant_entry_count] = updent;
					strcpy(redundant_entries[redundant_entry_count].partname, partname_b);
					strcpy(redundant_entries[redundant_entry_count].devname, pathname_b);
					redundant_entry_count += 1;
				} else {
					nonredundant_entries[nonredundant_entry_count] = updent;
					strcpy(nonredundant_entries[nonredundant_entry_count].devname, pathname);
					nonredundant_entry_count += 1;
				}
			} else if (redundant) {
				redundant_entries[redundant_entry_count] = updent;
				strcpy(redundant_entries[redundant_entry_count].partname, (*suffix == '\0' ? partname : partname_b));
				strcpy(redundant_entries[redundant_entry_count].devname, (*suffix == '\0' ? pathname : pathname_b));
				redundant_entry_count += 1;
			}
		}
	}

	/*
	 * For tegra210, just lump all entries into the 'redundant' list.
	 */
	if (soctype == TEGRA_SOCTYPE_210) {
		unsigned int ent;
		if (redundant_entry_count + nonredundant_entry_count > MAX_ENTRIES) {
			fprintf(stderr, "Error: too many partitions to initialize\n");
			goto reset_and_depart;
		}
		for (ent = 0; ent < nonredundant_entry_count; ent += 1)
			redundant_entries[redundant_entry_count++] = nonredundant_entries[ent];
		nonredundant_entry_count = 0;
	}

	contentbuf = malloc(largest_length);
	if (find_largest_partition(&slotbuf_size) < 0) {
		fprintf(stderr, "Error obtaining partition sizes\n");
		goto reset_and_depart;
	}
	slotbuf = malloc(slotbuf_size);
	zerobuf = calloc(1, slotbuf_size);
	if (contentbuf == NULL || slotbuf == NULL || zerobuf == NULL) {
		perror("allocating content buffers");
		goto reset_and_depart;
	}
	contentbuf_size = largest_length;

	if (soctype == TEGRA_SOCTYPE_210) {
		int bctctx = -1;
		if (invalid_version_or_downgrade(bupctx, fd, gptfd, redundant_entries, redundant_entry_count, (initialize > 1)))
			goto reset_and_depart;
		redundant_entry_count = order_entries_t210(redundant_entries, ordered_entries, redundant_entry_count);
		if (redundant_entry_count == 0)
			goto reset_and_depart;
		for (i = 0; i < redundant_entry_count; i++)
			if (process_entry(bupctx, fd, gptfd, ordered_entries[i], dryrun, initialize, &bctctx) != 0)
				goto reset_and_depart;
	} else {
		order_entries(redundant_entries, ordered_entries, redundant_entry_count);

		for (i = 0; i < redundant_entry_count; i++)
			if (process_entry(bupctx, fd, gptfd, ordered_entries[i], dryrun, initialize, NULL) != 0)
				goto reset_and_depart;

		if (initialize) {
			for (i = 0; i < nonredundant_entry_count; i++)
				if (process_entry(bupctx, fd, gptfd, &nonredundant_entries[i], dryrun, initialize, NULL) != 0)
					goto reset_and_depart;
		} else if (bct_updated) {
			/*
			 * If the BCT was updated, we must update both mb1 and mb1_b
			 */
			if (mb1_other.partname[0] == '\0') {
				fprintf(stderr, "Error: could not update alternate mb1 partition\n");
				goto reset_and_depart;
			}
			if (process_entry(bupctx, fd, gptfd, &mb1_other, dryrun, initialize, NULL) != 0)
				goto reset_and_depart;
		}
		if (!slot_specified) {
			if (dryrun)
				printf("[skip] mark slot %d as active\n", (initialize ? 0 : 1 - curslot));
			else  {
				unsigned int newslot = (initialize ? 0 : 1 - curslot);
				if (smd_slot_mark_active(smdctx, newslot) < 0) {
					perror("marking new boot slot active");
					goto reset_and_depart;
				}
				printf("Slot %u marked as active for next boot\n", newslot);
				if (smd_update(smdctx, gptctx, fd, initialize) < 0)
					perror("updating slot metadata");
			}
		}
	}

	/*
	 * Success if we get through all of the above
	 */
	ret = 0;

  reset_and_depart:
	if (smdctx)
		smd_finish(smdctx);
	if (fd >= 0) {
		if (!dryrun)
			fsync(fd);
		close(fd);
	}
	if (gptfd >= 0) {
		fsync(gptfd);
		close(gptfd);
	}
	if (reset_bootdev)
		set_bootdev_writeable_status(bootdev, false);
	if (reset_gptdev)
		set_bootdev_writeable_status(bup_gpt_device(bupctx), false);
	if (slotbuf)
		free(slotbuf);
	if (contentbuf)
		free(contentbuf);
	if (zerobuf)
		free(zerobuf);

	if (gptctx)
		gpt_finish(gptctx);
	if (bupctx)
		bup_finish(bupctx);

	return ret;

} /* main */
