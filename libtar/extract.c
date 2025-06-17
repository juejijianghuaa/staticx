/*
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  extract.c - libtar code to extract a file from a tar archive
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <libgen.h>
#include "libtar.h"
#include "compat.h"

#ifndef MIN
# define MIN(x, y)      (((x) < (y)) ? (x) : (y))
#endif

static int mkdirs_for(const char *filename)
{
	char *fndup;
	int rc;

	fndup = strdup(filename);
	if (!fndup) {
		errno = ENOMEM;
		return -1;
	}

	rc = mkdirhier(dirname(fndup));

	free(fndup);
	return rc;
}


/* switchboard */
int
tar_extract_file(TAR *t, const char *realname)
{
	int i;
	char *lnp;
	int pathname_len;
	int realname_len;

	printf("Debug: entering tar_extract_file for %s\n", realname);

	if (t->options & TAR_NOOVERWRITE)
	{
		struct stat s;
		printf("Debug: checking TAR_NOOVERWRITE\n");

		if (lstat(realname, &s) == 0 || errno != ENOENT)
		{
			errno = EEXIST;
			return -1;
		}
	}

	printf("Debug: checking file type\n");
	if (TH_ISDIR(t))
	{
		printf("Debug: is directory\n");
		i = tar_extract_dir(t, realname);
		if (i == 1)
			i = 0;
	}
	else if (TH_ISLNK(t))
	{
		printf("Debug: is hardlink\n");
		i = tar_extract_hardlink(t, realname);
	}
	else if (TH_ISSYM(t))
	{
		printf("Debug: is symlink\n");
		i = tar_extract_symlink(t, realname);
	}
	else if (TH_ISCHR(t))
	{
		printf("Debug: is chardev\n");
		i = tar_extract_chardev(t, realname);
	}
	else if (TH_ISBLK(t))
	{
		printf("Debug: is blockdev\n");
		i = tar_extract_blockdev(t, realname);
	}
	else if (TH_ISFIFO(t))
	{
		printf("Debug: is fifo\n");
		i = tar_extract_fifo(t, realname);
	}
	else /* if (TH_ISREG(t)) */
	{
		printf("Debug: is regular file\n");
		i = tar_extract_regfile(t, realname);
	}

	if (i != 0)
		return i;

	/**
	 * staticx: removed tar_set_file_perms() here as we set the only
	 * perms we care about in tar_extract_regfile().
	 */

	pathname_len = strlen(th_get_pathname(t)) + 1;
	realname_len = strlen(realname) + 1;
	lnp = calloc(1, pathname_len + realname_len);
	if (lnp == NULL)
		return -1;
	strcpy(&lnp[0], th_get_pathname(t));
	strcpy(&lnp[pathname_len], realname);

	return 0;
}

static size_t align_up(size_t val, size_t align)
{
	size_t r = val % align;

	if (r) {
		val += align - r;
	}

	return val;
}

/* extract regular file */
int
tar_extract_regfile(TAR *t, const char *realname)
{
	mode_t mode;
	size_t size;
	int fdout = -1;
	const char *filename;
	size_t to_read;
	char *buf = NULL;
	ssize_t n;
	int retval = -1;

	// 最小化系统调用和错误检查
	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	size = th_get_size(t);

	// 先直接试图创建文件
	fdout = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fdout == -1 && errno == ENOENT) {
		// 只有在文件创建失败时才尝试创建目录
		if (mkdirs_for(filename) == -1 || 
		    (fdout = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666)) == -1)
			goto out;
	}

	// 分配内存并读取数据
	to_read = align_up(size, T_BLOCKSIZE);
	buf = malloc(to_read);
	if (!buf) {
		errno = ENOMEM;
		goto out;
	}

	// 读取数据
	n = t->type->readfunc(t->context, buf, to_read);
	if (n != to_read) {
		errno = EINVAL;
		goto out;
	}

	// 写入数据
	n = write(fdout, buf, size);
	if (n != size) {
		errno = EINVAL;
		goto out;
	}

	// 设置权限
	if (fchmod(fdout, mode & 07777) == -1) {
		// 如果 fchmod 失败，尝试 chmod
		close(fdout);
		fdout = -1;
		if (chmod(filename, mode & 07777) == -1)
			goto out;
	}

	retval = 0;

out:
	free(buf);
	if (fdout != -1)
		close(fdout);
	return retval;
}


/* hardlink */
int
tar_extract_hardlink(TAR * t, const char *realname)
{
	const char *filename;
	const char *linktgt = NULL;

	if (!TH_ISLNK(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	if (mkdirs_for(filename) == -1)
		return -1;
	linktgt = th_get_linkname(t);

#ifdef DEBUG
	printf("  ==> extracting: %s (link to %s)\n", filename, linktgt);
#endif
	if (link(linktgt, filename) == -1)
	{
#ifdef DEBUG
		perror("link()");
#endif
		return -1;
	}

	return 0;
}

/* symlink */
int
tar_extract_symlink(TAR *t, const char *realname)
{
	const char *filename;

	if (!TH_ISSYM(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	if (mkdirs_for(filename) == -1)
		return -1;

	if (unlink(filename) == -1 && errno != ENOENT)
		return -1;

	// 复制 linkname 指向的文件内容到 filename，并打印日志
	const char *src = th_get_linkname(t);
	printf("[symlink->copy] copy %s to %s\n", src, filename);
	FILE *src_fp = fopen(src, "rb");
	if (!src_fp) {
		perror("fopen(src)");
		return -1;
	}
	FILE *dst_fp = fopen(filename, "wb");
	if (!dst_fp) {
		perror("fopen(dst)");
		fclose(src_fp);
		return -1;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), src_fp)) > 0) {
		if (fwrite(buf, 1, n, dst_fp) != n) {
			perror("fwrite");
			fclose(src_fp);
			fclose(dst_fp);
			return -1;
		}
	}
	fclose(src_fp);
	fclose(dst_fp);
	return 0;
}


/* character device */
int
tar_extract_chardev(TAR *t, const char *realname)
{
	mode_t mode;
	unsigned long devmaj, devmin;
	const char *filename;

	if (!TH_ISCHR(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	devmaj = th_get_devmajor(t);
	devmin = th_get_devminor(t);

	if (mkdirs_for(filename) == -1)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (character device %ld,%ld)\n",
	       filename, devmaj, devmin);
#endif
	if (mknod(filename, mode | S_IFCHR,
		  makedev(devmaj, devmin)) == -1)
	{
#ifdef DEBUG
		perror("mknod()");
#endif
		return -1;
	}

	return 0;
}


/* block device */
int
tar_extract_blockdev(TAR *t, const char *realname)
{
	mode_t mode;
	unsigned long devmaj, devmin;
	const char *filename;

	if (!TH_ISBLK(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	devmaj = th_get_devmajor(t);
	devmin = th_get_devminor(t);

	if (mkdirs_for(filename) == -1)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (block device %ld,%ld)\n",
	       filename, devmaj, devmin);
#endif
	if (mknod(filename, mode | S_IFBLK,
		  makedev(devmaj, devmin)) == -1)
	{
#ifdef DEBUG
		perror("mknod()");
#endif
		return -1;
	}

	return 0;
}


/* directory */
int
tar_extract_dir(TAR *t, const char *realname)
{
	mode_t mode;
	const char *filename;

	if (!TH_ISDIR(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);

	if (mkdirs_for(filename) == -1)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (mode %04o, directory)\n", filename,
	       mode);
#endif
	if (mkdir(filename, mode) == -1)
	{
		if (errno == EEXIST)
		{
			if (chmod(filename, mode) == -1)
			{
#ifdef DEBUG
				perror("chmod()");
#endif
				return -1;
			}
			else
			{
#ifdef DEBUG
				puts("  *** using existing directory");
#endif
				return 1;
			}
		}
		else
		{
#ifdef DEBUG
			perror("mkdir()");
#endif
			return -1;
		}
	}

	return 0;
}


/* FIFO */
int
tar_extract_fifo(TAR *t, const char *realname)
{
	mode_t mode;
	const char *filename;

	if (!TH_ISFIFO(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);

	if (mkdirs_for(filename) == -1)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (fifo)\n", filename);
#endif
	if (mkfifo(filename, mode) == -1)
	{
#ifdef DEBUG
		perror("mkfifo()");
#endif
		return -1;
	}

	return 0;
}


int
tar_extract_all(TAR *t, const char *prefix)
{
	const char *filename;
	char buf[MAXPATHLEN];
	char src_path[MAXPATHLEN];
	int i;
	struct symlink_entry {
		char target[MAXPATHLEN];
		char linkname[MAXPATHLEN];
		mode_t mode;
		struct symlink_entry *next;
	} *symlinks = NULL, *last = NULL;

#ifdef DEBUG
	printf("==> tar_extract_all(TAR *t, \"%s\")\n",
	       (prefix ? prefix : "(null)"));
#endif

	// 第一遍：只解包普通文件和目录，收集软链接
	while ((i = th_read(t)) == 0) {
		filename = th_get_pathname(t);
		if (prefix != NULL)
			snprintf(buf, sizeof(buf), "%s/%s", prefix, filename);
		else
			strlcpy(buf, filename, sizeof(buf));

		if (TH_ISSYM(t)) {
			// 收集软链接信息
			struct symlink_entry *entry = malloc(sizeof(struct symlink_entry));
			if (!entry) {
				errno = ENOMEM;
				return -1;
			}
			const char *target = th_get_linkname(t);
			if (prefix != NULL)
				snprintf(src_path, sizeof(src_path), "%s/%s", prefix, target);
			else
				strlcpy(src_path, target, sizeof(src_path));
			strncpy(entry->target, src_path, MAXPATHLEN-1);
			strncpy(entry->linkname, buf, MAXPATHLEN-1);
			entry->target[MAXPATHLEN-1] = '\0';
			entry->linkname[MAXPATHLEN-1] = '\0';
			entry->mode = 0700;  // 强制设置为可执行权限
			entry->next = NULL;
			if (last) last->next = entry;
			else symlinks = entry;
			last = entry;
		} else if (TH_ISREG(t) || TH_ISDIR(t)) {
			if (tar_extract_file(t, buf) != 0) {
				// 清理链表
				struct symlink_entry *cur = symlinks;
				while (cur) {
					struct symlink_entry *next = cur->next;
					free(cur);
					cur = next;
				}
				return -1;
			}
		}
	}

	// 第二遍：处理软链接（用复制模式）
	struct symlink_entry *cur = symlinks;
	while (cur) {
		printf("[symlink->copy] copy %s to %s (mode: 0%o)\n", 
               cur->target, cur->linkname, cur->mode);
		
		FILE *src_fp = fopen(cur->target, "rb");
		if (!src_fp) {
			perror("fopen(src)");
			printf("Failed to open source file: %s\n", cur->target);
			struct symlink_entry *tmp = cur;
			cur = cur->next;
			free(tmp);
			continue;
		}
		FILE *dst_fp = fopen(cur->linkname, "wb");
		if (!dst_fp) {
			perror("fopen(dst)");
			printf("Failed to open destination file: %s\n", cur->linkname);
			fclose(src_fp);
			struct symlink_entry *tmp = cur;
			cur = cur->next;
			free(tmp);
			continue;
		}
		
		char buf2[4096];
		size_t n;
		while ((n = fread(buf2, 1, sizeof(buf2), src_fp)) > 0) {
			if (fwrite(buf2, 1, n, dst_fp) != n) {
				perror("fwrite");
				printf("Failed to write to destination file: %s\n", cur->linkname);
				fclose(src_fp);
				fclose(dst_fp);
				struct symlink_entry *tmp = cur;
				cur = cur->next;
				free(tmp);
				continue;
			}
		}
		fclose(src_fp);
		fclose(dst_fp);
		
		// 设置文件权限并检查结果
		if (chmod(cur->linkname, cur->mode) == -1) {
			perror("chmod");
			printf("Failed to set permissions (0%o) for: %s\n", 
                   cur->mode, cur->linkname);
		} else {
			printf("[chmod] Set permissions 0%o for %s\n", 
                   cur->mode, cur->linkname);
		}
		
		// 验证权限
		struct stat st;
		if (stat(cur->linkname, &st) == 0) {
			printf("[verify] File %s has mode: 0%o\n", 
                   cur->linkname, st.st_mode & 0777);
		} else {
			perror("stat");
		}
		
		struct symlink_entry *tmp = cur;
		cur = cur->next;
		free(tmp);
	}

	return (i == 1 ? 0 : -1);
}
