//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#include "global/global.h"
#include "global/types.h"
#include "filesystem/stat.h"
#include "filesystem/fs.h"
#include "filesystem/file.h"
#include "log/log.h"
#include "posix/posix_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* note for this posix handlers' (syscall handler) return value and errno.
 * glibc checks syscall return value in INLINE_SYSCALL macro.
 * if the return value is negative, it sets the value as errno
 * (after turnning to positive one) and returns -1 to application.
 * Therefore, the return value must be correct -errno in posix semantic
 */

#define SET_MLFS_FD(fd) fd + g_fd_start
#define GET_MLFS_FD(fd) fd - g_fd_start

#if 0
// Is the directory dp empty except for "." and ".." ?
static int isdirempty(struct inode *dp)
{
	int off;
	struct dirent de;

	for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
		if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
			panic("isdirempty: readi");
		if(de.inum != 0)
			return 0;
	}
	return 1;
}
#endif
int mlfs_posix_chdir(const char *pathname)
{
    if (pathname == NULL) {
        return -ENOENT;
    }
    else if (*pathname == '/') { // pathname is absolute
        struct inode *inode = namei(pathname);
        if (inode == NULL) {
            return -ENOENT;
        }
        if (inode->itype == T_FILE) {
            return -ENOTDIR;
        }
        strncpy(pwd, pathname, MAX_PATH);
        return 0;
    }
    else { // pathname is relative
        size_t len = strlen(pwd);
        strncat(pwd, pathname, MAX_PATH - len);
        struct inode *inode = namei(pwd);
        if (inode == NULL) {
            pwd[len] = 0; // reset pwd to origin path
            return -ENOENT;
        }
        if (inode->itype == T_FILE) {
            pwd[len] = 0; // reset pwd to origin path
            return -ENOTDIR;
        }
        // pwd has already been concatenated above
        return 0;
    }

}

int mlfs_posix_open(const char *input_path, int flags, uint16_t mode)
{
	struct file *f;
	struct inode *inode;
	int fd;
    char fullpath[MAX_PATH + 1];
    const char *path;
    if (input_path == NULL) {
        return -ENOENT;
    }
    else if (*input_path != '/') { // path is relative
        strncpy(fullpath, pwd, MAX_PATH);
        strncat(fullpath, input_path, MAX_PATH - strlen(fullpath));
        path = fullpath;
    }
    else { // path is absolute
        path = input_path;
    }

	start_log_tx();

	if (flags & O_CREAT) {
		if (flags & O_DIRECTORY)
			panic("O_DIRECTORY cannot be set with O_CREAT\n");

		uint8_t exist;
		inode = mlfs_object_create(path, T_FILE, &exist);

		mlfs_debug("create file %s - inum %u\n", path, inode->inum);

		if (!inode) {
			abort_log_tx();
			return -ENOENT;
		}

		if ((flags & O_EXCL) && exist) {
			abort_log_tx();
			return -EEXIST;
		}
	} else {
		// opendir API
		if (flags & O_DIRECTORY) {
			// Fall through..
			// it is OK to return fd for directory. glibc allocates
			// DIR structure and fill it with fd and result from stats.
			// check: sysdeps/posix/opendir.c
		}

		if ((inode = namei(path)) == NULL) {
			abort_log_tx();
			return -ENOENT;
		}

		if (inode->itype == T_DIR) {
			if (!(flags |= (O_RDONLY|O_DIRECTORY))) {
				abort_log_tx();
				return -EACCES;
			}
		}
	}

	f = mlfs_file_alloc();

	if (f == NULL) {
		iput(inode);
		abort_log_tx();
		return -ENOMEM;
	}

	fd = f->fd;

	mlfs_debug("open file %s inum %u fd %d (%p)\n", path, inode->inum, fd, inode->_dinode);

	commit_log_tx();

	pthread_rwlock_wrlock(&f->rwlock);

	if (flags & O_DIRECTORY) {
		mlfs_debug("directory file inum %d\n", inode->inum);
		f->type = FD_DIR;
	} else {
		f->type = FD_INODE;
	}

	f->ip = inode;
	f->readable = !(flags & O_WRONLY);
	f->writable = (flags & O_WRONLY) || (flags & O_RDWR);

	f->off = 0;

	/* TODO: set inode permission based the mode
	if (mode & S_IRUSR)
		// Set read permission
	if (mode & S_IWUSR)
		// Set write permission
	*/

	pthread_rwlock_unlock(&f->rwlock);

	return SET_MLFS_FD(fd);
}

int mlfs_posix_access(const char *pathname, int mode)
{
	struct inode *inode;

	if (mode != F_OK)
		panic("does not support other than F_OK\n");

	inode = namei(pathname);

	if (!inode) {
		return -ENOENT;
	}

	iput(inode);

	return 0;
}

int mlfs_posix_creat(char *path, uint16_t mode)
{
	return mlfs_posix_open(path, O_CREAT|O_RDWR, mode);
}

ssize_t mlfs_posix_read(int fd, void *buf, size_t count)
{
	ssize_t ret = 0;
	struct file *f;

	f = &g_fd_table.open_files[fd];

	pthread_rwlock_rdlock(&f->rwlock);

	mlfs_assert(f);

	if (f->ref == 0) {
		panic("file descriptor is wrong\n");
		return -EBADF;
	}

	ret = mlfs_file_read(f, (uint8_t*)buf, count);

	pthread_rwlock_unlock(&f->rwlock);

	return ret;
}

ssize_t mlfs_posix_pread64(int fd, void *buf, size_t count, loff_t off)
{
	ssize_t ret = 0;
	struct file *f;

	f = &g_fd_table.open_files[fd];

	pthread_rwlock_rdlock(&f->rwlock);

	mlfs_assert(f);

	if (f->ref == 0) {
		panic("file descriptor is wrong\n");
		return -EBADF;
	}

	ret = mlfs_file_read_offset(f, (uint8_t*)buf, count, off);

	pthread_rwlock_unlock(&f->rwlock);

	return ret;
}

ssize_t mlfs_posix_write(int fd, const void *buf, size_t count)
{
	ssize_t ret;
	struct file *f;

	f = &g_fd_table.open_files[fd];

	pthread_rwlock_wrlock(&f->rwlock);

	mlfs_assert(f);

	if (f->ref == 0) {
		panic("file descriptor is wrong\n");
		return -EBADF;
	}

	ret = mlfs_file_write(f, (uint8_t*)buf, f->off, count);
	// change offset here since mlfs_file_write doesn't touch f->off
	if (ret > 0) {
		f->off += ret;
	}

	pthread_rwlock_unlock(&f->rwlock);

	return ret;
}

ssize_t mlfs_posix_pwrite64(int fd, const void *buf, size_t count, loff_t off)
{
	int ret;
	struct file *f;

	f = &g_fd_table.open_files[fd];

	pthread_rwlock_wrlock(&f->rwlock);

	mlfs_assert(f);

	if (f->ref == 0) {
		panic("file descriptor is wrong\n");
		return -EBADF;
	}

	ret = mlfs_file_write(f, (uint8_t*)buf, off, count);

	pthread_rwlock_unlock(&f->rwlock);

	return ret;
}

off_t mlfs_posix_lseek(int fd, int64_t offset, int origin)
{
	struct file *f;
	off_t ret = 0;

	f = &g_fd_table.open_files[fd];

	if (f->ref == 0) {
		return -EBADF;
	}

	mlfs_assert(f);

	//lock file
	pthread_rwlock_rdlock(&f->rwlock);

	switch(origin) {
		case SEEK_SET:
			f->off = offset;
			ret = f->off;
			break;
		case SEEK_CUR:
			f->off += offset;
			ret = f->off;
			break;
		case SEEK_END:
			//f->ip->size += offset;
			irdlock(f->ip);
			f->off = f->ip->size + offset;
			iunlock(f->ip);
			ret = f->off;
			break;
		default:
			ret = -EINVAL;
			break;
	}
	//unlock file
	pthread_rwlock_unlock(&f->rwlock);

	return ret;
}

int mlfs_posix_close(int fd)
{
	struct file *f;

	f = &g_fd_table.open_files[fd];

	if (!f) {
		return -EBADF;
	}

	mlfs_debug("close file inum %u fd %d\n", f->ip->inum, f->fd);

	return mlfs_file_close(f);
}

int mlfs_posix_mkdir(char *path, mode_t mode)
{
	int ret = 0;
	struct inode *inode;
	uint8_t exist;

	start_log_tx();

	// return inode with holding ilock.
	inode = mlfs_object_create(path, T_DIR, &exist);

	if (!inode) {
		abort_log_tx();
		return -ENOENT;
	}

exit_mkdir:
	commit_log_tx();
	if (exist) {
		return -EEXIST;
	}
	else {
		return ret;
	}
}

int mlfs_posix_rmdir(char *path)
{
    return mlfs_posix_unlink(path);
/*
	int ret = 0;
	struct inode *dir_inode;

	start_log_tx();

	dir_inode = namei(path);

	if (!dir_inode) {
		abort_log_tx();
		return -ENOENT;
	}

	if (dir_inode->size > 0) {
		abort_log_tx();
		return -EINVAL;
	}

	mlfs_debug("%s\n", path);
	dir_remove_entry(dir_inode, path, dir_inode->inum);
	iunlockput(dir_inode);

exit_rmdir:
	commit_log_tx();
	return ret;
*/
}

int mlfs_posix_stat(const char *filename, struct stat *stat_buf)
{
	struct inode *inode;

	inode = namei((char *)filename);

	if (!inode) {
		return -ENOENT;
	}

	stati(inode, stat_buf);

	return 0;
}

int mlfs_posix_fstat(int fd, struct stat *stat_buf)
{
	struct file *f;

	f = &g_fd_table.open_files[fd];

	if (f->ref == 0)
		return -EBADF;

	mlfs_assert(f->ip);

	stati(f->ip, stat_buf);

	return 0;
}

int mlfs_posix_fallocate(int fd, offset_t offset, offset_t len)
{
	struct file *f;
	int ret = 0;

	f = &g_fd_table.open_files[fd];

	if (f->ref == 0)
		return -EBADF;
	ret = mlfs_file_fallocate(f, offset, len);
	return 0;
}

int mlfs_posix_unlink(const char *filename)
{
	int ret = 0;
	char name[DIRSIZ];
	struct inode *inode;
	struct inode *dir_inode;

	/* TODO: handle struct file deletion
	 * e.g., unlink without calling close */

	dir_inode = nameiparent((char *)filename, name);
	if (!dir_inode)
		return -ENOENT;

	//inode = namei((char *)filename);
	inode = dir_lookup(dir_inode, name, NULL);

	if (!inode)
		return -ENOENT;

	start_log_tx();

	// remove file from directory
	ret = dir_remove_entry(dir_inode, name, inode->inum);
	if (ret < 0) {
		abort_log_tx();
		return ret;
	}

	mlfs_debug("unlink filename %s - inum %u\n", name, inode->inum);

	dlookup_del(inode->dev, filename);

	iput(dir_inode);
	iput(inode);

	ret = idealloc(inode);


	// write to the log for digest.
	add_to_loghdr(L_TYPE_UNLINK, inode, 0, sizeof(struct dinode), NULL, 0);

	commit_log_tx();


	return ret;
}

int mlfs_posix_truncate(const char *filename, off_t length)
{
	struct inode *inode;

	inode = namei((char *)filename);

	if (!inode) {
		return -ENOENT;
	}

	start_log_tx();
	
	itrunc(inode, length);

	commit_log_tx();

	iput(inode);

	return 0;
}

int mlfs_posix_ftruncate(int fd, off_t length)
{
	struct file *f;
	int ret = 0;

	f = &g_fd_table.open_files[fd];

	if (f->ref == 0) {
		return -EBADF;
	}

	start_log_tx();

	itrunc(f->ip, length);

	commit_log_tx();

	return 0;
}

int mlfs_posix_rename(char *oldpath, char *newpath)
{
	int ret = 0;
	struct inode *old_dir_inode, *new_dir_inode;
	char old_file_name[DIRSIZ], new_file_name[DIRSIZ];

	old_dir_inode = nameiparent((char *)oldpath, old_file_name);
	new_dir_inode = nameiparent((char *)newpath, new_file_name);

	mlfs_assert(old_dir_inode);
	mlfs_assert(new_dir_inode);

	if (old_dir_inode != new_dir_inode)
		panic("Only support rename in a same directory\n");

	start_log_tx();

	mlfs_assert(strlen(old_file_name) <= DIRSIZ);
	mlfs_assert(strlen(new_file_name) <= DIRSIZ);

	dlookup_del(old_dir_inode->dev, newpath);

	ret = dir_change_entry(old_dir_inode, old_file_name, new_file_name);
	if (ret < 0) {
		abort_log_tx();

		iput(old_dir_inode);
		iput(new_dir_inode);

		dlookup_del(old_dir_inode->dev, oldpath);
		return ret;
	}

	mlfs_debug("rename %s to %s\n", old_file_name, new_file_name);

	dlookup_del(old_dir_inode->dev, oldpath);

	iput(old_dir_inode);
	iput(new_dir_inode);

	commit_log_tx();

	return 0;
}

// FIXME: off is not used.
int mlfs_posix_getdents(int fd, struct linux_dirent *buf,
		size_t nbytes, offset_t off)
{
	struct file *f;
	int bytes;

	f = &g_fd_table.open_files[fd];

	if (f->ref == 0) {
		return -EBADF;
	}

	if (f->type != FD_DIR)
		return -EBADF;

	/* glibc compute bytes with struct linux_dirent
	 * but ip->size is is computed by struct dirent,
	 * which is much small size than struct linux_dirent
	if (nbytes < f->ip->size)
		return -EINVAL;
	*/

	if (f->off >= f->ip->size)
		return 0;

	if (f->ip->itype == T_DIR) {
		bytes = dir_get_linux_dirent(f->ip, buf, &(f->off), nbytes);
	}
	else {
		return -ENOTDIR;
	}

	return bytes;
}

int mlfs_posix_fcntl(int fd, int cmd, void *arg)
{
	struct file *f;
	int ret = 0;

	f = &g_fd_table.open_files[fd];

	if (f->ref == 0) {
		return -EBADF;
	}

	if (cmd != F_SETLK) {
		mlfs_debug("%s: cmd %d\n", __func__, cmd);
		//panic("Only support F_SETLK\n");
	}

	return 0;
}

#ifdef __cplusplus
}
#endif
