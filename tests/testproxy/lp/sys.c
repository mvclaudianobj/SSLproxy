/*-
 * SSLsplit - transparent SSL/TLS interception
 * https://www.roe.ch/SSLsplit
 *
 * Copyright (c) 2009-2019, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "sys.h"

#include "log.h"
#include "defaults.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/un.h>
#include <sys/time.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <fts.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifndef _SC_NPROCESSORS_ONLN
#include <sys/sysctl.h>
#endif /* !_SC_NPROCESSORS_ONLN */

#if HAVE_DARWIN_LIBPROC
#include <libproc.h>
#endif

#include <event2/util.h>

/*
 * Permanently drop from root privileges to an unprivileged user account.
 * Sets the real, effective and stored user and group ID and the list of
 * ancillary groups.  This is only safe if the effective user ID is 0.
 * If username is unset and the effective uid != uid, drop privs to uid.
 * This is to support setuid bit configurations.
 * If groupname is set, it will be used instead of the user's default primary
 * group.
 * If jaildir is set, also chroot to jaildir after reading system files
 * but before dropping privileges.
 * Returns 0 on success, -1 on failure.
 */
int
sys_privdrop(const char *username, const char *groupname, const char *jaildir)
{
	struct passwd *pw = NULL;
	struct group *gr = NULL;
	int ret = -1;

	if (groupname) {
		errno = 0;
		if (!(gr = getgrnam(groupname))) {
			log_err_level_printf(LOG_CRIT, "Failed to getgrnam group '%s': %s\n",
			               groupname, strerror(errno));
			goto error;
		}
	}

	if (username) {
		errno = 0;
		if (!(pw = getpwnam(username))) {
			log_err_level_printf(LOG_CRIT, "Failed to getpwnam user '%s': %s\n",
			               username, strerror(errno));
			goto error;
		}

		if (gr != NULL) {
			pw->pw_gid = gr->gr_gid;
		}

		if (initgroups(username, pw->pw_gid) == -1) {
			log_err_level_printf(LOG_CRIT, "Failed to initgroups user '%s': %s\n",
			               username, strerror(errno));
			goto error;
		}
	}

	if (jaildir) {
		if (chroot(jaildir) == -1) {
			log_err_level_printf(LOG_CRIT, "Failed to chroot to '%s': %s\n",
			               jaildir, strerror(errno));
			goto error;
		}
		if (chdir("/") == -1) {
			log_err_level_printf(LOG_CRIT, "Failed to chdir to '/': %s\n",
			               strerror(errno));
			goto error;
		}
	}

	if (username) {
		if (setgid(pw->pw_gid) == -1) {
			log_err_level_printf(LOG_CRIT, "Failed to setgid to %i: %s\n",
			               pw->pw_gid, strerror(errno));
			goto error;
		}
		if (setuid(pw->pw_uid) == -1) {
			log_err_level_printf(LOG_CRIT, "Failed to setuid to %i: %s\n",
			               pw->pw_uid, strerror(errno));
			goto error;
		}
	} else if (getuid() != geteuid()) {
		if (setuid(getuid()) == -1) {
			log_err_level_printf(LOG_CRIT, "Failed to setuid(getuid()): %s\n",
			               strerror(errno));
			goto error;
		}
	}

	ret = 0;
error:
	if (pw) {
		endpwent();
	}
	if (gr) {
		endgrent();
	}
	return ret;
}

/*
 * If the user exists and on successful lookup, return 0 and if uid != NULL,
 * write the uid of *username* to the value pointed to by uid.
 * Return -1 on failure or if the user does not exist.
 */
int
sys_uid(const char *username, uid_t *uid)
{
	struct passwd *pw;
	int rv;

	errno = 0;
	if (!(pw = getpwnam(username))) {
		if (errno != 0 && errno != ENOENT) {
			log_err_level_printf(LOG_CRIT, "Failed to load user '%s': %s (%i)\n",
			               username, strerror(errno), errno);
		}
		rv = -1;
	} else {
		if (uid)
			*uid = pw->pw_uid;
		rv = 0;
	}
	endpwent();
	return rv;
}

/*
 * Returns 1 if username can be loaded from user database, 0 otherwise.
 */
int
sys_isuser(const char *username)
{
	return sys_uid(username, NULL) == 0;
}

/*
 * If the group exists and on successful lookup, return 0 and if gid != NULL,
 * write the gid of *groupname* to the value pointed to by gid.
 * Return -1 on failure or if the group does not exist.
 */
int
sys_gid(const char *groupname, gid_t *gid)
{
	struct group *gr;
	int rv;

	errno = 0;
	if (!(gr = getgrnam(groupname))) {
		if (errno != 0 && errno != ENOENT) {
			log_err_level_printf(LOG_CRIT, "Failed to load group '%s': %s (%i)\n",
			               groupname, strerror(errno), errno);
		}
		rv = -1;
	} else {
		if (gid)
			*gid = gr->gr_gid;
		rv = 0;
	}
	endgrent();
	return rv;
}

/*
 * Returns 1 if groupname can be loaded from group database, 0 otherwise.
 */
int
sys_isgroup(const char *groupname)
{
	return sys_gid(groupname, NULL) == 0;
}

/*
 * Returns 1 if username is equivalent to the current effective UID.
 * Returns 0 otherwise.
 */
int
sys_isgeteuid(const char *username)
{
	uid_t uid;

	if (sys_uid(username, &uid) == -1)
		return 0;
	if (uid == geteuid())
		return 1;
	return 0;
}

/*
 * Open and lock process ID file fn.
 * Returns open file descriptor on success or -1 on errors.
 */
int
sys_pidf_open(const char *fn)
{
	int fd;

	if ((fd = open(fn, O_RDWR|O_CREAT, DFLT_PIDFMODE)) == -1) {
		log_err_level_printf(LOG_CRIT, "Failed to open '%s': %s\n", fn,
		               strerror(errno));
		return -1;
	}
	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		log_err_level_printf(LOG_CRIT, "Failed to lock '%s': %s\n", fn,
		               strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

/*
 * Write process ID to open process ID file descriptor fd.
 * Returns 0 on success, -1 on errors.
 */
int
sys_pidf_write(int fd)
{
	char pidbuf[4*sizeof(pid_t)];
	int rv;
	ssize_t n;

	rv = snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
	if (rv == -1 || rv >= (int)sizeof(pidbuf))
		return -1;

	n = write(fd, pidbuf, strlen(pidbuf));
	if (n < (ssize_t)strlen(pidbuf))
		return -1;

	rv = fsync(fd);
	if (rv == -1)
		return -1;

	rv = fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	if (rv == -1)
		return -1;

	return 0;
}

/*
 * Close and remove open process ID file before quitting.
 */
void
sys_pidf_close(int fd, const char *fn)
{
	unlink(fn);
	close(fd);
}

/*
 * Determine address family of addr
 */
int
sys_get_af(const char *addr)
{
	if (strstr(addr, ":"))
		return AF_INET6;
	else if (!strpbrk(addr, "abcdefghijklmnopqrstu"
							"vwxyzABCDEFGHIJKLMNOP"
							"QRSTUVWXYZ-"))
		return AF_INET;
	else
		return AF_UNSPEC;
}

/*
 * Parse an ascii host/IP and port tuple into a sockaddr_storage.
 * On success, returns address family and fills in addr, addrlen.
 * Returns -1 on error.
 */
int
sys_sockaddr_parse(struct sockaddr_storage *addr, socklen_t *addrlen,
                   char *naddr, char *nport, int af, int flags)
{
	struct evutil_addrinfo hints;
	struct evutil_addrinfo *ai;
	int rv;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = af;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = EVUTIL_AI_ADDRCONFIG | flags;
	rv = evutil_getaddrinfo(naddr, nport, &hints, &ai);
	if (rv != 0) {
		log_err_level_printf(LOG_CRIT, "Cannot resolve address '%s' port '%s': %s\n",
		               naddr, nport, gai_strerror(rv));
		return -1;
	}
	memcpy(addr, ai->ai_addr, ai->ai_addrlen);
	*addrlen = ai->ai_addrlen;
	af = ai->ai_family;
	freeaddrinfo(ai);
	return af;
}

/*
 * Converts an IPv4/IPv6 sockaddr into printable string representations of the
 * host and the service (port) part.  Writes allocated buffers to *host and
 * *serv which must both be freed by the caller.  Neither *host nor *port are
 * freed by this function before newly allocating.
 * Returns 0 on success, -1 otherwise.  When -1 is returned, pointers in *host
 * and *serv are invalid and must not be used nor freed by the caller.
 */
int
sys_sockaddr_str(struct sockaddr *addr, socklen_t addrlen,
                 char **host, char **serv)
{
	char tmphost[INET6_ADDRSTRLEN];
	char tmpserv[6]; /* max decimal digits of short plus terminator */
	int rv;

	rv = getnameinfo(addr, addrlen,
	                 tmphost, sizeof(tmphost),
	                 tmpserv, sizeof(tmpserv),
	                 NI_NUMERICHOST | NI_NUMERICSERV);
	if (rv != 0) {
		log_err_level_printf(LOG_CRIT, "Cannot get nameinfo for socket address: %s\n",
		               gai_strerror(rv));
		return -1;
	}
	*serv = strdup(tmpserv);
	if (!*serv) {
		log_err_level_printf(LOG_CRIT, "Cannot allocate memory\n");
		return -1;
	}
	*host = strdup(tmphost);
	if (!*host) {
		log_err_level_printf(LOG_CRIT, "Cannot allocate memory\n");
		free(*serv);
		*serv = NULL;
		return -1;
	}
	return 0;
}

/*
 * Sanitizes a valid IPv4 or IPv6 address for use in a filename, i.e. removes
 * characters that are invalid on NTFS and replaces them with more innocent
 * characters.  The function assumes that the input is a valid IPv4 or IPv6
 * address; it is not a generic filename sanitizer.
 *
 * Returns a copy of string s that must be freed by the caller.
 *
 * Invalid NTFS characters are < > : " / \ | ? * according to
 * https://msdn.microsoft.com/en-gb/library/windows/desktop/aa365247.aspx
 */
char *
sys_ip46str_sanitize(const char *s)
{
	char *copy, *p;

	copy = strdup(s);
	if (!copy)
		return NULL;
	p = copy;
	while (*p) {
		switch (*p) {
		case ':':
		case '%':
			*p = '_';
			break;
		}
		p++;
	}

	return copy;
}

/*
 * Returns 1 if path points to an existing directory node in the filesystem.
 * Returns 0 if path is NULL, does not exist, or points to a file of some kind.
 */
int
sys_isdir(const char *path)
{
	struct stat s;

	if (stat(path, &s) == -1) {
		if (errno != ENOENT) {
			log_err_level_printf(LOG_CRIT, "Error stating file: %s (%i)\n",
			               strerror(errno), errno);
		}
		return 0;
	}
	if (s.st_mode & S_IFDIR)
		return 1;
	return 0;
}

/*
 * Create directory including parent directories with mode_t.
 * Mode of existing parent directories is not changed.
 * Returns 0 on success, -1 and sets errno on error.
 */
int
sys_mkpath(const char *path, mode_t mode)
{
	char parent[strlen(path)+1];
	char *p;

	memcpy(parent, path, sizeof(parent));

	p = parent;
	do {
		/* skip leading '/' characters */
		while (*p == '/') p++;
		p = strchr(p, '/');
		if (p) {
			/* overwrite '/' to terminate the string at the next
			 * parent directory */
			*p = '\0';
		}

		struct stat sbuf;
		if (stat(parent, &sbuf) == -1) {
			if (errno == ENOENT) {
				if (mkdir(parent, mode) != 0)
					return -1;
			} else {
				return -1;
			}
		} else if (!S_ISDIR(sbuf.st_mode)) {
			errno = ENOTDIR;
			return -1;
		}

		if (p) {
			/* replace the overwritten slash */
			*p = '/';
			p++;
		}
	} while (p);

	return 0;
}

/*
 * Return realpath(dirname(path)) + / + basename(path) in a newly allocated
 * string.  Returns NULL on failure and sets errno to ENOENT if the directory
 * part does not exist.
 */
char *
sys_realdir(const char *path)
{
	char *sep, *udir, *rdir, *p;
	int rerrno, rv;

	if (path[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}

	udir = strdup(path);
	if (!udir)
		return NULL;

	sep = strrchr(udir, '/');
	if (!sep) {
		free(udir);
		rv = asprintf(&udir, "./%s", path);
		if (rv == -1)
			return NULL;
		sep = udir + 1;
	} else if (sep == udir) {
		return udir;
	}
	*sep = '\0';
	rdir = realpath(udir, NULL);
	if (!rdir) {
		rerrno = errno;
		free(udir);
		errno = rerrno;
		return NULL;
	}
	rv = asprintf(&p, "%s/%s", rdir, sep + 1);
	rerrno = errno;
	free(rdir);
	free(udir);
	errno = rerrno;
	if (rv == -1)
		return NULL;
	return p;
}

/*
 * Portably get the number of CPU cores online in the system.
 */
uint32_t
sys_get_cpu_cores(void)
{
#ifdef _SC_NPROCESSORS_ONLN
	return sysconf(_SC_NPROCESSORS_ONLN);
#else /* !_SC_NPROCESSORS_ONLN */
	int mib[2];
	uint32_t n;
	size_t len = sizeof(n);

	mib[0] = CTL_HW;
	mib[1] = HW_AVAILCPU;
	sysctl(mib, sizeof(mib)/sizeof(int), &n, &len, NULL, 0);

	if (n < 1) {
		mib[1] = HW_NCPU;
		sysctl(mib, sizeof(mib)/sizeof(int), &n, &len, NULL, 0);
		if (n < 1) {
			n = 1;
		}
	}
	return n;
#endif /* !_SC_NPROCESSORS_ONLN */
}

/*
 * Send a message and optional file descriptor on a connected AF_UNIX
 * SOCKET_DGRAM socket s.  Returns the return value of sendmsg().
 * If fd is -1, no file descriptor is passed.
 */
ssize_t
sys_sendmsgfd(int sock, void *buf, size_t bufsz, int fd)
{
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	ssize_t n;

	iov.iov_base = buf;
	iov.iov_len = bufsz;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_flags = 0;

	if (fd != -1) {
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);
		memset(cmsgbuf, 0, sizeof(cmsgbuf));

		cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg)
			return -1;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;

		*((int *) CMSG_DATA(cmsg)) = fd;
	} else {
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
	}
	do {
#ifdef MSG_NOSIGNAL
		n = sendmsg(sock, &msg, MSG_NOSIGNAL);
#else /* !MSG_NOSIGNAL */
		n = sendmsg(sock, &msg, 0);
#endif /* !MSG_NOSIGNAL */
	} while (n == -1 && errno == EINTR);
	return n;
}

/*
 * Receive a message and optional file descriptor on a connected AF_UNIX
 * SOCKET_DGRAM socket s.  Returns the return value of recvmsg()/recv()
 * and sets errno to EINVAL if the received message is malformed.
 * If pfd is NULL, no file descriptor is received; if a file descriptor was
 * part of the received message and pfd is NULL, then the kernel will close it.
 */
ssize_t
sys_recvmsgfd(int sock, void *buf, size_t bufsz, int *pfd)
{
	ssize_t n;

	if (pfd) {
		struct iovec iov;
		struct msghdr msg;
		struct cmsghdr *cmsg;
		unsigned char cmsgbuf[CMSG_SPACE(sizeof(int))];

		iov.iov_base = buf;
		iov.iov_len = bufsz;

		msg.msg_name = NULL;
		msg.msg_namelen = 0;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);
		do {
			n = recvmsg(sock, &msg, 0);
		} while (n == -1 && errno == EINTR);
		if (n <= 0)
			return n;
		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
			if (cmsg->cmsg_level != SOL_SOCKET) {
				errno = EINVAL;
				return -1;
			}
			if (cmsg->cmsg_type != SCM_RIGHTS) {
				errno = EINVAL;
				return -1;
			}
			*pfd = *((int *) CMSG_DATA(cmsg));
		} else {
			*pfd = -1;
		}
	} else {
		do {
			n = recv(sock, buf, bufsz, 0);
		} while (n == -1 && errno == EINTR);
	}
	return n;
}

/* vim: set noet ft=c: */
