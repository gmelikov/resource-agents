/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <errno.h>
#include <libgen.h>
#include <dirent.h>
#include <string.h>
#include <linux/kdev_t.h>

#define __user
#include <linux/gfs2_ondisk.h>

#include "gfs2_tool.h"

#define PAGE_SIZE (4096)

static char sysfs_buf[PAGE_SIZE];

char *__get_sysfs(char *fsname, char *filename)
{
	char path[PATH_MAX];
	int fd, rv;

	memset(path, 0, PATH_MAX);
	memset(sysfs_buf, 0, PAGE_SIZE);
	snprintf(path, PATH_MAX - 1, "%s/%s/%s", SYS_BASE, fsname, filename);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		die("can't open %s: %s\n", path, strerror(errno));
	rv = read(fd, sysfs_buf, PAGE_SIZE);
	if (rv < 0)
		die("can't read from %s: %s\n", path, strerror(errno));

	close(fd);
	return sysfs_buf;
}

char *
get_sysfs(char *fsname, char *filename)
{
	char *p = strchr(__get_sysfs(fsname, filename), '\n');
	if (p)
		*p = '\0';
	return sysfs_buf;
}

unsigned int
get_sysfs_uint(char *fsname, char *filename)
{
	unsigned int x;
	sscanf(__get_sysfs(fsname, filename), "%u", &x);
	return x;
}

void
set_sysfs(char *fsname, char *filename, char *val)
{
	char path[PATH_MAX];
	int fd, rv, len;

	len = strlen(val) + 1;
	if (len > PAGE_SIZE)
		die("value for %s is too larger for sysfs\n", path);

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX - 1, "%s/%s/%s", SYS_BASE, fsname, filename);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		die("can't open %s: %s\n", path, strerror(errno));

	rv = write(fd, val, len);
	if (rv != len){
		if (rv < 0)
			die("can't write to %s: %s", path, strerror(errno));
		else
			die("tried to write %d bytes to path, wrote %d\n",
			    len, rv);
	}
	close(fd);
}

/**
 * get_list - Get the list of GFS2 filesystems
 *
 * Returns: a NULL terminated string
 */

#define LIST_SIZE 1048576

char *
get_list(void)
{
	char path[PATH_MAX];
	char s_id[PATH_MAX];
	char *list, *p;
	int rv, fd, x = 0, total = 0;
	DIR *d;
	struct dirent *de;

	list = malloc(LIST_SIZE);
	if (!list)
		die("out of memory\n");

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s", SYS_BASE);

	d = opendir(path);
	if (!d)
		die("can't open %s: %s\n", SYS_BASE, strerror(errno));

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s/id", SYS_BASE, de->d_name);

		fd = open(path, O_RDONLY);
		if (fd < 0)
			die("can't open %s: %s\n", path, strerror(errno));

		memset(s_id, 0, PATH_MAX);

		rv = read(fd, s_id, sizeof(s_id));
		if (rv < 0)
			die("can't read %s: %s\n", path, strerror(errno));

		close(fd);

		p = strstr(s_id, "\n");
		if (p)
			*p = '\0';

		total += strlen(s_id) + strlen(de->d_name) + 2;
		if (total > LIST_SIZE)
			break;

		x += sprintf(list + x, "%s %s\n", s_id, de->d_name);

	}

	closedir(d);

	return list;
}

/**
 * str2lines - parse a string into lines
 * @list: the list
 *
 * Returns: An array of character pointers
 */

char **
str2lines(char *str)
{
	char *p;
	unsigned int n = 0;
	char **lines;
	unsigned int x = 0;

	for (p = str; *p; p++)
		if (*p == '\n')
			n++;

	lines = malloc((n + 1) * sizeof(char *));
	if (!lines)
		die("out of memory\n");

	for (lines[x] = p = str; *p; p++)
		if (*p == '\n') {
			*p = 0;
			lines[++x] = p + 1;
		}

	return lines;
}

/**
 * do_basename - Create dm-N style name for the device
 * @device:
 *
 * Returns: Pointer to dm name or basename
 */

static char *
do_basename(char *device)
{
	FILE *file;
	int found = FALSE;
	char line[PATH_MAX], major_name[PATH_MAX];
	unsigned int major_number;
	struct stat st;

	file = fopen("/proc/devices", "r");
	if (!file)
		goto punt;

	while (fgets(line, PATH_MAX, file)) {
		if (sscanf(line, "%u %s", &major_number, major_name) != 2)
			continue;
		if (strcmp(major_name, "device-mapper") != 0)
			continue;
		found = TRUE;
		break;
	}

	fclose(file);

	if (!found)
		goto punt;

	if (stat(device, &st))
		goto punt;
	if (major(st.st_rdev) == major_number) {
		static char realname[16];
		snprintf(realname, 16, "dm-%u", minor(st.st_rdev));
		return realname;
	}

 punt:
	return basename(device);
}

char *
mp2devname(char *mp)
{
	FILE *file;
	char line[PATH_MAX];
	static char device[PATH_MAX];
	char *name = NULL;
	char *realname;

	realname = realpath(mp, NULL);
	if (!realname)
		die("Unable to allocate memory for name resolution.\n");
	file = fopen("/proc/mounts", "r");
	if (!file)
		die("can't open /proc/mounts: %s\n", strerror(errno));

	while (fgets(line, PATH_MAX, file)) {
		char path[PATH_MAX], type[PATH_MAX];

		if (sscanf(line, "%s %s %s", device, path, type) != 3)
			continue;
		if (strcmp(path, realname))
			continue;
		if (strcmp(type, "gfs2"))
			die("%s is not a GFS2 filesystem\n", mp);

		name = do_basename(device);

		break;
	}

	free(realname);
	fclose(file);

	return name;
}

char *
find_debugfs_mount(void)
{
	FILE *file;
	char line[PATH_MAX];
	char device[PATH_MAX], type[PATH_MAX];
	char *path;

	file = fopen("/proc/mounts", "rt");
	if (!file)
		die("can't open /proc/mounts: %s\n", strerror(errno));

	path = malloc(PATH_MAX);
	if (!path)
		die("Can't allocate memory for debugfs.\n");
	while (fgets(line, PATH_MAX, file)) {

		if (sscanf(line, "%s %s %s", device, path, type) != 3)
			continue;
		if (!strcmp(type, "debugfs")) {
			fclose(file);
			return path;
		}
	}

	free(path);
	fclose(file);
	return NULL;
}

/* The fsname can be substituted for the mountpoint on the command line.
   This is necessary when we can't resolve a devname from /proc/mounts
   to a fsname. */

int is_fsname(char *name)
{
	int rv = 0;
	DIR *d;
	struct dirent *de;

	d = opendir(SYS_BASE);
	if (!d)
		die("can't open %s: %s\n", SYS_BASE, strerror(errno));

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		if (strcmp(de->d_name, name) == 0) {
			rv = 1;
			break;
		}
	}

	closedir(d);

	return rv;
}

/**
 * mp2fsname - Find the name for a filesystem given its mountpoint
 *
 * We do this by iterating through gfs2 dirs in /sys/fs/gfs2/ looking for
 * one where the "id" attribute matches the device id returned by stat for
 * the mount point.  The reason we go through all this is simple: the
 * kernel's sysfs is named after the VFS s_id, not the device name.
 * So it's perfectly legal to do something like this to simulate user
 * conditions without the proper hardware:
 *    # rm /dev/sdb1
 *    # mkdir /dev/cciss
 *    # mknod /dev/cciss/c0d0p1 b 8 17
 *    # mount -tgfs2 /dev/cciss/c0d0p1 /mnt/gfs2
 *    # gfs2_tool gettune /mnt/gfs2
 * In this example the tuning variables are in a directory named after the
 * VFS s_id, which in this case would be /sys/fs/gfs2/sdb1/
 *
 * Returns: the fsname
 */

char *
mp2fsname(char *mp)
{
	char device_id[PATH_MAX], *fsname = NULL;
	struct stat statbuf;
	DIR *d;
	struct dirent *de;

	if (stat(mp, &statbuf))
		return NULL;

	memset(device_id, 0, sizeof(device_id));
	sprintf(device_id, "%u:%u", (uint32_t)MAJOR(statbuf.st_dev),
		(uint32_t)MINOR(statbuf.st_dev));

	d = opendir(SYS_BASE);
	if (!d)
		die("can't open %s: %s\n", SYS_BASE, strerror(errno));

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		if (strcmp(get_sysfs(de->d_name, "id"), device_id) == 0) {
			fsname = strdup(de->d_name);
			break;
		}
	}

	closedir(d);

	return fsname;
}

/**
 * name2value - find the value of a name-value pair in a string
 * @str_in:
 * @name:
 *
 * Returns: the value string in a static buffer
 */

char *
name2value(char *str_in, char *name)
{
	char str[strlen(str_in) + 1];
	static char value[PATH_MAX];
	char **lines;
	unsigned int x;
	unsigned int len = strlen(name);

	strcpy(str, str_in);
	value[0] = 0;

	lines = str2lines(str);

	for (x = 0; *lines[x]; x++)
		if (memcmp(lines[x], name, len) == 0 &&
		    lines[x][len] == ' ') {
			strcpy(value, lines[x] + len + 1);
			break;
		}

	free(lines);

	return value;
}

/**
 * name2u32 - find the value of a name-value pair in a string
 * @str_in:
 * @name:
 *
 * Returns: the value uint32
 */

uint32_t
name2u32(char *str, char *name)
{
	char *value = name2value(str, name);
	uint32_t x = 0;

	sscanf(value, "%u", &x);

	return x;
}

/**
 * name2u64 - find the value of a name-value pair in a string
 * @str_in:
 * @name:
 *
 * Returns: the value uint64
 */

uint64_t
name2u64(char *str, char *name)
{
	char *value = name2value(str, name);
	uint64_t x = 0;

	sscanf(value, "%"SCNu64, &x);

	return x;
}
