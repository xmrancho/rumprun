/*-
 * Copyright (c) 2015 Antti Kantee.  All Rights Reserved.
 * Copyright (c) 2014 Martin Lucina.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * The public interface for rumprun unikernel configuration is documented in
 * doc/config.md. Refer there before making any changes to the JSON format
 * defined in this module.
 */

#include <sys/param.h>
#include <sys/disklabel.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <ufs/ufs/ufsmount.h>
#include <isofs/cd9660/cd9660_mount.h>
#include <fs/tmpfs/tmpfs_args.h>

#include <dev/vndvar.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rump/rump.h>
#include <rump/netconfig.h>

#include <rumprun-base/config.h>
#include <rumprun-base/json.h>
#include <rumprun-base/parseargs.h>
#include <rumprun-base/rumprun.h>

struct rumprun_execs rumprun_execs = TAILQ_HEAD_INITIALIZER(rumprun_execs);

static const char *
jtypestr(enum jtypes t)
{

	switch (t) {
	case jnull:	return "NULL";
	case jtrue:	return "BOOLEAN";
	case jfalse:	return "BOOLEAN";
	case jstring:	return "STRING";
	case jarray:	return "ARRAY";
	case jobject:	return "OBJECT";
	default:	return "UNKNOWN";
	}
}

static void
jexpect(enum jtypes t, jvalue *v, const char *loc)
{

	if (v->d != t)
		errx(1, "%s: expected %s, got %s", loc, jtypestr(t),
			jtypestr(v->d));
}

typedef struct {
	const char *name;
	void (*handler)(jvalue *, const char *);
} jhandler;

/*
 * Given an object with key/value pairs, v, and an array of handlers, h,
 * execute those handlers which match keys in v, *in priority order*. I.e.
 * earlier handlers in h are executed before later handlers.
 *
 * This works around the fact that a JSON object is unordered by definition,
 * but we need to do some operations in a deterministic order.
 *
 * TODO: This function is only efficient for small numbers of (handlers x keys).
 * While we still have the rumprun script as a client there's not a lot we can
 * do about it, since the script generates object with duplicate keys. To be
 * revisited if the rumprun script is removed.
 */
static void
handle_object(jvalue *v, jhandler h[], const char *loc)
{
	size_t j;

	jexpect(jobject, v, loc);

	/*
	 * Pass 1: Check for unknown keys in object.
	 */
	for (jvalue **i = v->u.v; *i; ++i) {
		for (j = 0; h[j].handler; j++) {
			if (strcmp((*i)->n, h[j].name) == 0) {
				break;
			}
		}
		if (!h[j].handler)
			warnx("%s: no match for key \"%s\", ignored", loc,
				(*i)->n);
	}

	/*
	 * Pass 2: Call handlers in the order they are defined. Given that JSON
	 * objects are unordered, this ensures * that configuration is done in
	 * a deterministic order.
	 */
	for (j = 0; h[j].handler; j++) {
		for (jvalue **i = v->u.v; *i; ++i) {
			if (strcmp((*i)->n, h[j].name) == 0) {
				h[j].handler(*i, loc);
			}
		}
	}
}

static int
handle_rc_dummy(void)
{
	int i;

	for (i = 0; rumprun_bins[i]; i++)
	{
		struct rumprun_exec *rre;

		rre = malloc(sizeof (*rre) + 2 * sizeof (char *));
		if (rre == NULL)
			errx(1, "malloc");
		rre->rre_flags = 0;
		rre->rre_main = rumprun_bins[i]->main;
		rre->rre_argc = 1;
		rre->rre_argv[0] = strdup(rumprun_bins[i]->binname);
		if (rre->rre_argv[0] == NULL)
			errx(1, "strdup");
		rre->rre_argv[rre->rre_argc] = NULL;

		TAILQ_INSERT_TAIL(&rumprun_execs, rre, rre_entries);
	}

	return i;
}

/*
 * Given a JSON object containing sysctl keys and values:
 *
 * {
 *   <key>: <value>,
 *   ...
 * }
 *
 * Returns a struct rr_sysctl[] (to *osc) and its size (to *onsc). If a prefix
 * is specified, prepends it to each key, separated from the key by '.'. The
 * caller is responsible for freeing *osc[] and its members when done.
 */
static void
sysctl_parse(jvalue *v, struct rr_sysctl **osc, size_t *onsc,
		const char *prefix, const char *loc)
{
	int nsc = 0;

	for (jvalue **i = v->u.v; *i; i++) {
		if ((*i)->d != jtrue &&
		    (*i)->d != jfalse &&
		    (*i)->d != jstring &&
		    (*i)->d != jnumber) {
			errx(1, "invalid type for key \"%s\" in \"%s\"",
				(*i)->n, __func__);
		}
		nsc += 1;
	}
	if (nsc == 0) {
		*osc = NULL;
		*onsc = 0;
		return;
	}

	struct rr_sysctl *sc = calloc(nsc, sizeof (struct rr_sysctl));
	if (sc == NULL)
		err(1, "calloc");
	*osc = sc;
	*onsc = nsc;
	for (jvalue **i = v->u.v; *i; i++, sc++) {
		if (prefix != NULL) {
			size_t nsz = strlen(prefix) + strlen((*i)->n) + 2;
			sc->key = malloc(nsz);
			if (sc->key == NULL)
				err(1, "malloc");
			snprintf(sc->key, nsz, "%s.%s", prefix, (*i)->n);
		}
		else {
			sc->key = strdup((*i)->n);
			if (sc->key == NULL)
				err(1, "strdup");
		}
		if ((*i)->d == jtrue) {
			sc->value = strdup("1");
		}
		else if ((*i)->d == jfalse) {
			sc->value = strdup("0");
		}
		else {
			sc->value = strdup((*i)->u.s);
		}
		if (sc->value == NULL)
			err(1, "strdup");
	}
	return;
}

static rre_mainfn *
getmain(const char *binname)
{
	/*
	 * Special undocumented case for backward compatibility with the
	 * existing rumprun launcher script which does not understand
	 * multibake.
	 */
	if (strcmp("*", binname) == 0)
		return rumprun_bins[0]->main;

	for (int i = 0; rumprun_bins[i]; i++)
	{
		if (strcmp(rumprun_bins[i]->binname, binname) == 0)
			return rumprun_bins[i]->main;
	}
	return NULL;
}

static void
handle_bin(jvalue *v, const char *loc)
{
	struct rumprun_exec *rre;
	bool have_argv;
	rre_mainfn *binmain;
	jvalue *v_bin, *v_argv, *v_runmode, *v_workdir, *v_sysctl, **v_arg;
	int rreflags;
	size_t nargv;

	jexpect(jobject, v, __func__);

	/* process and validate data */
	v_bin = v_argv = v_runmode = v_workdir = v_sysctl = NULL;
	for (jvalue **i = v->u.v; *i; ++i) {

		if (strcmp((*i)->n, "bin") == 0) {
			jexpect(jstring, *i, __func__);
			v_bin = *i;
		} else if (strcmp((*i)->n, "argv") == 0) {
			jexpect(jarray, *i, __func__);
			v_argv = *i;
		} else if (strcmp((*i)->n, "runmode") == 0) {
			jexpect(jstring, *i, __func__);
			v_runmode = *i;
		} else if (strcmp((*i)->n, "workdir") == 0) {
			jexpect(jstring, *i, __func__);
			v_workdir = *i;
		} else if (strcmp((*i)->n, "netbsd") == 0) {
			jexpect(jobject, *i, __func__);
			for (jvalue **j = (*i)->u.v; *j; ++j) {
				if (strcmp((*j)->n, "sysctl") == 0) {
					jexpect(jobject, *j, __func__);
					v_sysctl = *j;
				} else {
					errx(1, "unexpected key \"%s\" in \"%s\"",
						(*j)->n, __func__);
				}
			}
		} else {
			errx(1, "unexpected key \"%s\" in \"%s\"", (*i)->n,
				__func__);
		}
	}

	if (!v_bin)
		errx(1, "missing \"bin\" for rc entry");
	binmain = getmain(v_bin->u.s);
	if (!binmain)
		errx(1, "unknown \"bin\" \"%s\" in rc entry", v_bin->u.s);

	nargv = 0;
	if (v_argv) {
		for (jvalue **i = v_argv->u.v; *i; ++i) {
			jexpect(jstring, *i, __func__);
			nargv++;
		}
	}
	if (nargv != 0) {
		have_argv = true;
	}
	else {
		have_argv = false;
		nargv = 1;
	}

	if (v_runmode) {
		if (strcmp(v_runmode->u.s, "") == 0) {
			rreflags = 0;
		} else if (strcmp(v_runmode->u.s, "&") == 0) {
			rreflags = RUMPRUN_EXEC_BACKGROUND;
		} else if (strcmp(v_runmode->u.s, "|") == 0) {
			rreflags = RUMPRUN_EXEC_PIPE;
		} else {
			errx(1, "invalid runmode \"%s\" for bin \"%s\"",
				v_runmode->u.s, v_bin->u.s);
		}
	} else {
		rreflags = 0;
	}

	/* ok, we got everything.  save into rumprun_exec structure */
	rre = malloc(sizeof(*rre) + (1 + nargv) * sizeof(char *));
	if (rre == NULL)
		err(1, "%s: malloc(rumprun_exec)", __func__);
	if (have_argv) {
		for (v_arg = v_argv->u.v, nargv = 0; *v_arg; ++v_arg, ++nargv) {
			rre->rre_argv[nargv] = strdup((*v_arg)->u.s);
			if (rre->rre_argv[nargv] == NULL)
				err(1, "%s: strdup", __func__);
		}
	}
	else {
		rre->rre_argv[0] = strdup(v_bin->u.s);
		if (rre->rre_argv[0] == NULL)
			err(1, "%s: strdup", __func__);
	}
	rre->rre_argc = nargv;
	rre->rre_argv[rre->rre_argc] = NULL;
	rre->rre_flags = rreflags;
	rre->rre_main = binmain;
	if (v_workdir) {
		rre->rre_workdir = strdup(v_workdir->u.s);
		if (rre->rre_workdir == NULL)
			err(1, "%s: strdup", __func__);
	}
	else {
		rre->rre_workdir = NULL;
	}
	rre->rre_sc = NULL;
	rre->rre_nsc = 0;
	if (v_sysctl) {
		sysctl_parse(v_sysctl, &rre->rre_sc, &rre->rre_nsc,
			"proc.curproc", __func__);
	}

	TAILQ_INSERT_TAIL(&rumprun_execs, rre, rre_entries);
}

static void
handle_rc(jvalue *v, const char *loc)
{

	jexpect(jarray, v, __func__);
	for (jvalue **i = v->u.v; *i; ++i)
		handle_bin(*i, __func__);
}

static void
handle_env(jvalue *v, const char *loc)
{

	jexpect(jobject, v, __func__);
	for (jvalue **i = v->u.v; *i; ++i) {
		jexpect(jstring, *i, __func__);
		if (setenv((*i)->n, (*i)->u.s, 1) == -1)
			err(1, "setenv");
	}
}

static void
config_ipv4(const char *ifname, const char *method,
	const char *cidr)
{
	int rv;

	if (strcmp(method, "dhcp") == 0) {
		if ((rv = rump_pub_netconfig_dhcp_ipv4_oneshot(ifname)) != 0)
			errx(1, "%s: %s: configuring dhcp failed: %s",
			    __func__, ifname, strerror(rv));
	}
	else if (strcmp(method, "static") == 0) {
		char *addr, *mask;

		if (!cidr)
			errx(1, "%s: %s: missing \"addr\"", __func__,
				ifname);
		addr = strdup(cidr);
		if (!addr)
			err(1, "%s: strdup", __func__);
		mask = strchr(addr, '/');
		if (!addr || !mask) {
			errx(1, "%s: %s: invalid \"addr\" specified", __func__,
				ifname);
		}
		*mask = 0;
		mask += 1;

		if ((rv = rump_pub_netconfig_ipv4_ifaddr_cidr(ifname,
		    addr, atoi(mask))) != 0) {
			errx(1, "%s: %s: ifconfig \"%s/%s\" failed: %s",
			    __func__, ifname, addr, mask, strerror(rv));
		}
		free(addr);
	}
	else {
		errx(1, "%s: %s: "
		    "method \"static\" or \"dhcp\" expected, "
		    "got \"%s\"", __func__, ifname, method);
	}
}

static void
config_ipv6(const char *ifname, const char *method,
	const char *addrmask)
{
	int rv;

	if (strcmp(method, "auto") == 0) {
		if ((rv = rump_pub_netconfig_auto_ipv6(ifname)) != 0)
			errx(1, "%s: %s: ipv6 autoconfig failed: %s",
			    __func__, ifname, strerror(rv));
	}
	else if (strcmp(method, "static") == 0) {
		char *addr, *mask;

		if (!addrmask)
			errx(1, "%s: %s: missing \"addr\"", __func__,
				ifname);
		addr = strdup(addrmask);
		if (!addr)
			err(1, "%s: strdup", __func__);
		mask = strchr(addr, '/');
		if (!addr || !mask) {
			errx(1, "%s: %s: invalid \"addr\" specified", __func__,
				ifname);
		}
		*mask = 0;
		mask += 1;

		if ((rv = rump_pub_netconfig_ipv6_ifaddr(ifname,
		    addr, atoi(mask))) != 0) {
			errx(1, "%s: %s: ifconfig \"%s/%s\" failed: %s",
			    __func__, ifname, addr, mask, strerror(rv));
		}

		free(addr);
	}
	else {
		errx(1, "%s: %s: "
		    "method \"static\" or \"auto\" expected, "
		    "got \"%s\"", __func__, ifname, method);

	}
}

static void
handle_interface(jvalue *v, const char *loc)
{
	jvalue *addrs, *create;
	const char *ifname, *type, *method, *addr;
	int rv;

	jexpect(jobject, v, __func__);

	ifname = v->n;
	addrs = create = NULL;
	for (jvalue **i = v->u.v; *i; ++i) {
		if (strcmp((*i)->n, "create") == 0) {
			if ((*i)->d != jtrue && (*i)->d != jfalse) {
				errx(1, "%s: expected BOOLEAN for key"
					"\"create\" in \"%s\"", __func__,
					ifname);
			}
			create = *i;
		} else if (strcmp((*i)->n, "addrs") == 0) {
			jexpect(jarray, *i, __func__);
			addrs = *i;
		} else {
			warnx("%s: unexpected key \"%s\" in \"%s\", ignored",
				__func__, (*i)->n, ifname);
		}
	}

	if (create && create->d == jtrue) {
		if ((rv = rump_pub_netconfig_ifcreate(ifname)) != 0) {
			errx(1, "%s: ifcreate(%s) failed: %s", __func__, ifname,
				strerror(rv));
		}
	}

	if (!addrs) {
		warnx("%s: no addresses configured for interface \"%s\"",
			__func__, ifname);
		return;
	}

	for (jvalue **a = addrs->u.v; *a; ++a) {
		jexpect(jobject, *a, __func__);

		type = method = addr = NULL;
		for (jvalue **i = (*a)->u.v; *i; ++i) {
			if (strcmp((*i)->n, "type") == 0) {
				type = (*i)->u.s;
			} else if (strcmp((*i)->n, "method") == 0) {
				method = (*i)->u.s;
			} else if (strcmp((*i)->n, "addr") == 0) {
				addr = (*i)->u.s;
			} else {
				warnx("%s: unexpected key \"%s\""
					" in \"%s.addrs[]\"",
					__func__, (*i)->n, ifname);
			}
		}

		if (!type || !method) {
			errx(1, "%s: missing type/method in \"%s.addrs[]\"",
				__func__, ifname);
		}
		if (strcmp(type, "inet") == 0) {
			config_ipv4(ifname, method, addr);
		} else if (strcmp(type, "inet6") == 0) {
			config_ipv6(ifname, method, addr);
		} else {
			errx(1, "%s: address type \"%s\" not supported "
				"in \"%s.addrs[]\"", __func__, type, ifname);
		}
	}
}

static void
handle_interfaces(jvalue *v, const char *loc)
{

	jexpect(jobject, v, __func__);

	for (jvalue **i = v->u.v; *i; ++i) {
		handle_interface(*i, __func__);
	}
}

static void
handle_gateways(jvalue *v, const char *loc)
{
	const char *type, *addr;
	int rv;

	jexpect(jarray, v, __func__);

	for (jvalue **a = v->u.v; *a; ++a) {
		jexpect(jobject, *a, __func__);

		type = addr = NULL;
		for (jvalue **i = (*a)->u.v; *i; ++i) {
			if (strcmp((*i)->n, "type") == 0) {
				type = (*i)->u.s;
			} else if (strcmp((*i)->n, "addr") == 0) {
				addr = (*i)->u.s;
			} else {
				warnx("%s: unexpected key \"%s\""
					" in gateways[], ignored",
					__func__, (*i)->n);
			}
		}

		if (!type || !addr) {
			errx(1, "%s: missing type/addr in gateways[]",
				__func__);
		}
		if (strcmp(type, "inet") == 0) {
			if ((rv = rump_pub_netconfig_ipv4_gw(addr)) != 0) {
				errx(1, "%s: gw \"%s\" addition failed: %s",
					__func__, addr, strerror(rv));
			}
		} else if (strcmp(type, "inet6") == 0) {
			if ((rv = rump_pub_netconfig_ipv6_gw(addr)) != 0) {
				errx(1, "%s: gw \"%s\" addition failed: %s",
					__func__, addr, strerror(rv));
			}
		} else {
			errx(1, "%s: gateway type \"%s\" not supported "
				"in gateways[]", __func__, type);
		}
	}
}

static void
handle_dns(jvalue *v, const char *loc)
{
	jvalue *nameservers, *search;
	int fd, rv, len, maxlen, n;
	char *buf, *p;

	jexpect(jobject, v, __func__);

	nameservers = search = NULL;
	for (jvalue **i = v->u.v; *i; ++i) {
		if (strcmp((*i)->n, "nameservers") == 0) {
			jexpect(jarray, *i, __func__);
			nameservers = *i;
		} else if (strcmp((*i)->n, "search") == 0) {
			jexpect(jarray, *i, __func__);
			search = *i;
		} else {
			warnx("%s: unexpected key \"%s\", ignored", __func__,
				(*i)->n);
		}
	}

	if (!nameservers && !search)
		return;

	/*
	 * Create /etc if it does not exist, and truncate /etc/resolv.conf.
	 */
	rv = mkdir("/etc", 0755);
	if (rv == -1 && errno != EEXIST)
		err(1, "%s: mkdir(\"/etc\")", __func__);
	fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT, 0644);
	if (fd == -1)
		err(1, "%s: open(\"/etc/resolv.conf\")", __func__);

	/*
	 * Longest required string is "search " + 1024 characters + "\n\0".
	 * See limits in resolv.conf(5).
	 */
	maxlen = strlen("search ") + 1024 + 2;
	buf = malloc(maxlen);
	if (buf == NULL)
		err(1, "%s: malloc", __func__);

	/*
	 * Write out a "nameserver <address>" line for each nameserver.
	 */
	if (nameservers) {
		n = 0;
		for (jvalue **i = nameservers->u.v; *i; ++i) {
			jexpect(jstring, *i, __func__);
			n++;
			if (n > 3)
				errx(1, "%s: too many nameservers (max 3)",
					__func__);

			len = snprintf(buf, maxlen, "nameserver %s\n",
				(*i)->u.s);
			if (len >= maxlen)
				errx(1, "%s: nameserver \"%s\" too long",
					__func__, (*i)->u.s);
			rv = write(fd, buf, len);
			if (rv == -1)
				err(1, "%s: write", __func__);
		}
	}

	/*
	 * Write out search list, checking for limits.
	 */
	if (search) {
		n = 0;
		p = buf;
		for (jvalue **i = search->u.v; *i; ++i) {
			jexpect(jstring, *i, __func__);
			n++;
			if (n > 6)
				errx(1, "%s: too many search domains (max 6)",
					__func__);

			len = snprintf(p, maxlen, "%s %s%s",
				(n == 1) ? "search" : "",
				(*i)->u.s,
				(*(i+1) == NULL) ? "\n" : "");
			if (len >= maxlen)
				errx(1, "%s: search list too long", __func__);
			maxlen -= len;
			p += len;
		}
		if (n > 0) {
			rv = write(fd, buf, p - buf);
			if (rv == -1)
				err(1, "%s: write", __func__);
		}
	}

	close(fd);
	free(buf);
}

static jhandler handlers_net[] = {
	{ "interfaces", handle_interfaces },
	{ "gateways", handle_gateways },
	{ "dns", handle_dns },
	{ 0 }
};

static void
handle_net(jvalue *v, const char *loc)
{

	handle_object(v, handlers_net, __func__);
}

static void
makevnddev(int israw, int unit, int part, char *storage, size_t storagesize)
{

	snprintf(storage, storagesize, "/dev/%svnd%d%c",
	    israw ? "r" : "", unit, 'a' + part);
}

static devmajor_t
getvndmajor(int israw)
{
	struct stat sb;
	char path[32];

	makevnddev(israw, 0, RAW_PART, path, sizeof(path));
	if (stat(path, &sb) == -1)
		err(1, "failed to stat %s", path);
	return major(sb.st_rdev);
}

static void
configvnd(const char *dev, const char *path)
{
	int unit;
	struct vnd_ioctl vndio;
	char bbuf[32], rbuf[32];
	int fd;

	if(sscanf(dev, "vnd%d", &unit) != 1)
		errx(1, "%s: invalid vnd name \"%s\"", __func__, dev);

	makevnddev(0, unit, RAW_PART, bbuf, sizeof(bbuf));
	makevnddev(1, unit, RAW_PART, rbuf, sizeof(rbuf));

	memset(&vndio, 0, sizeof(vndio));
	vndio.vnd_file = __UNCONST(path);
	vndio.vnd_flags = VNDIOF_READONLY;

	fd = open(rbuf, O_RDWR);
	if (fd == -1) {
		/*
		 * node doesn't exist?  try creating it.  use majors from
		 * vnd0, which we (obviously) assume/hope exists
		 */
		if (errno == ENOENT) {
			const devmajor_t bmaj = getvndmajor(0);
			const devmajor_t rmaj = getvndmajor(1);

			if (mknod(bbuf, 0666 | S_IFBLK,
			    MAKEDISKDEV(bmaj, unit, RAW_PART)) == -1)
				err(1, "%s: mknod %s", __func__, bbuf);
			if (mknod(rbuf, 0666 | S_IFBLK,
			    MAKEDISKDEV(rmaj, unit, RAW_PART)) == -1)
				err(1, "%s: mknod %s", __func__, rbuf);

			fd = open(rbuf, O_RDWR);
		}
		if (fd == -1)
			err(1, "%s: open(%s)", __func__, rbuf);
	}

	if (ioctl(fd, VNDIOCSET, &vndio) == -1)
		err(1, "%s: VNDIOCSET on %s failed", __func__, rbuf);
	close(fd);
}

static void
configetfs(const char *dev, const char *hostpath, int hard)
{
	char key[32];
	int rv;

	snprintf(key, sizeof(key), "/dev/%s", dev);
	rv = rump_pub_etfs_register(key, hostpath, RUMP_ETFS_BLK);
	if (rv != 0 && hard) {
		errx(1, "etfs register for \"%s\" failed: %s", hostpath,
			strerror(rv));
	}
}

static void
handle_blk(jvalue *v, const char *loc)
{
	const char *dev, *type, *path;

	jexpect(jobject, v, __func__);
	dev = v->n;
	type = path = NULL;

	for (jvalue **i = v->u.v; *i; ++i) {
		if (strcmp((*i)->n, "type") == 0) {
			jexpect(jstring, *i, __func__);
			type = (*i)->u.s;
		} else if (strcmp((*i)->n, "path") == 0) {
			jexpect(jstring, *i, __func__);
			path = (*i)->u.s;
		} else {
			errx(1, "%s: unexpected key \"%s\" in \"%s\"",
				__func__, (*i)->n, dev);
		}
	}

	if (!type || !path) {
		errx(1, "%s: missing \"path\"/\"type\" in \"%s\"", __func__,
			dev);
	}

	if (strcmp(type, "etfs") == 0) {
		configetfs(dev, path, 1);
	} else if (strcmp(type, "vnd") == 0) {
		configvnd(dev, path);
	} else {
		errx(1, "%s: unsupported type \"%s\" in \"%s\"", __func__,
			type, dev);
	}
}

static void
handle_blks(jvalue *v, const char *loc)
{

	jexpect(jobject, v, __func__);

	for (jvalue **i = v->u.v; *i; ++i) {
		handle_blk(*i, __func__);
	}
}

static bool
mount_blk(const char *dev, const char *mp, jvalue *options)
{
	struct ufs_args mntargs_ufs = { .fspec = __UNCONST(dev) };
	struct iso_args mntargs_iso = { .fspec = dev };

	if (!dev)
		return false;

	if (mount(MOUNT_FFS, mp, 0, &mntargs_ufs, sizeof(mntargs_ufs)) == 0)
		return true;
	if (mount(MOUNT_EXT2FS, mp, 0, &mntargs_ufs, sizeof(mntargs_ufs)) == 0)
		return true;
	if (mount(MOUNT_CD9660,
	    mp, MNT_RDONLY, &mntargs_iso, sizeof(mntargs_iso)) == 0)
		return true;

	return false;
}

static bool
mount_kernfs(const char *dev, const char *mp, jvalue *options)
{

	if (mount(MOUNT_KERNFS, mp, 0, NULL, 0) == 0)
		return true;

	return false;
}

static bool
mount_tmpfs(const char *dev, const char *mp, jvalue *options)
{
	struct tmpfs_args ta = { 0 };
	const char *opt_size = NULL;
	int64_t size;

	if (options) {
		jexpect(jobject, options, __func__);

		for (jvalue **i = options->u.v; *i; ++i) {
			if (strcmp((*i)->n, "size") == 0) {
				jexpect(jstring, *i, __func__);
				opt_size = (*i)->u.s;
			}
			else {
				errx(1, "%s: unexpected key \"%s\" in \"%s\"",
					__func__, (*i)->n, "options");
			}
		}
	}
	if (!opt_size) {
		/*
		 * TODO: We should have a more sensible default size, e.g. 10%
		 * of core, but we don't have that information here.
		 */
		opt_size = "1M";
	}
	if (dehumanize_number(opt_size, &size) != 0) {
		errx(1, "%s: bad size for %s", __func__, mp);
	}

	ta.ta_version = TMPFS_ARGS_VERSION,
	ta.ta_size_max = size;
	ta.ta_root_mode = 01777;

	if (mount(MOUNT_TMPFS, mp, 0, &ta, sizeof (ta)) == 0)
		return true;

	return false;
}

struct {
	const char *mt_source;
	bool (*mt_mount)(const char *, const char *, jvalue *);
} mounters[] = {
	{ "blk",	mount_blk },
	{ "kernfs",	mount_kernfs },
	{ "tmpfs",      mount_tmpfs },
};

static void
mkdirhier(const char *path)
{
	char *pathbuf, *chunk;

	pathbuf = strdup(path);
	if (!path)
		err(1, "strdup");

	for (chunk = pathbuf;;) {
		bool end;

		/* find & terminate the next chunk */
		chunk += strspn(chunk, "/");
		chunk += strcspn(chunk, "/");
		end = (*chunk == '\0');
		*chunk = '\0';

		if (mkdir(pathbuf, 0755) == -1) {
			if (errno != EEXIST)
				err(1, "%s: mkdir(\"%s\") failed", __func__,
				    pathbuf);
		}

		/* restore path */
		if (!end)
			*chunk = '/';
		else
			break;
	}

	free (pathbuf);
}

static void
handle_mount(jvalue *v, const char *loc)
{
	jvalue *options;
	const char *source, *path, *mp;
	size_t mi;

	jexpect(jobject, v, __func__);

	mp = v->n;
	source = path = NULL;
	options = NULL;

	for (jvalue **i = v->u.v; *i; ++i) {

		if (strcmp((*i)->n, "source") == 0) {
			jexpect(jstring, *i, __func__);
			source = (*i)->u.s;
		} else if (strcmp((*i)->n, "path") == 0) {
			jexpect(jstring, *i, __func__);
			path = (*i)->u.s;
		} else if (strcmp((*i)->n, "options") == 0) {
			jexpect(jobject, *i, __func__);
			options = *i;
		} else {
			errx(1, "%s: unexpected key \"%s\" in \"%s\"", __func__,
				(*i)->n, mp);
		}
	}

	if (!source) {
		errx(1, "%s: missing \"source\" in \"%s\"", __func__, v->n);
	}

	mkdirhier(mp);

	for (mi = 0; mi < __arraycount(mounters); mi++) {
		if (strcmp(source, mounters[mi].mt_source) == 0) {
			if (!mounters[mi].mt_mount(path, mp, options))
				err(1, "%s: mount \"%s\" on \"%s\" "
				    "type \"%s\" failed",
				    __func__, path ? path : "(none)", mp,
				    source);
			break;
		}
	}
	if (mi == __arraycount(mounters)) {
		errx(1, "%s: unknown source \"%s\" in \"%s\"", __func__,
			source, mp);
	}
}

static void
handle_mounts(jvalue *v, const char *loc)
{

	jexpect(jobject, v, __func__);

	for (jvalue **i = v->u.v; *i; ++i) {
		handle_mount(*i, __func__);
	}
}

static void
handle_sysctl(jvalue *v, const char *loc)
{
	struct rr_sysctl *sc;
	size_t nsc;

	jexpect(jobject, v, __func__);
	sysctl_parse(v, &sc, &nsc, NULL, __func__);
	if (sc == NULL)
		return;

	for (size_t i = 0; i < nsc; i++) {
		int rc = rumprun_sysctlw(sc[i].key, sc[i].value);
		if (rc != 0) {
			errx(1, "error writing sysctl key \"%s\": %s",
					sc[i].key, strerror(rc));
		}
		free(sc[i].key);
		free(sc[i].value);
	}
	free(sc);
}

static jhandler handlers_netbsd[] = {
	{ "sysctl", handle_sysctl },
	{ 0 }
};

static void
handle_netbsd(jvalue *v, const char *loc)
{

	handle_object(v, handlers_netbsd, __func__);
}

static jhandler handlers_root[] = {
	{ "netbsd", handle_netbsd },
	{ "rc", handle_rc },
	{ "env", handle_env },
	{ "blk", handle_blks },
	{ "mount", handle_mounts },
	{ "net", handle_net },
	{ 0 }
};

/* don't believe we can have a >64k config */
#define CFGMAXSIZE (64*1024)
static char *
getcmdlinefromroot(const char *cfgname)
{
	const char *tryroot[] = {
		"/dev/ld0a",
		"/dev/sd0a",
	};
	struct stat sb;
	unsigned int i;
	int fd;
	char *p;

	if (mkdir("/rootfs", 0777) == -1)
		err(1, "mkdir /rootfs failed");

	/*
	 * XXX: should not be hardcoded to cd9660.  but it is for now.
	 * Maybe use mountroot() here somehow?
	 */
	for (i = 0; i < __arraycount(tryroot); i++) {
		if (mount_blk(tryroot[i], "/rootfs", NULL))
			break;
	}

	/* didn't find it that way.  one more try: etfs for sda1 (EC2) */
	if (i == __arraycount(tryroot)) {
		configetfs("rootfs", "blkfront:sda1", 0);

		if (!mount_blk("/dev/rootfs", "/rootfs", NULL))
			errx(1, "failed to mount /rootfs");
	}

	/*
	 * Ok, we've successfully mounted /rootfs.  Now get the config.
	 */

	while (*cfgname == '/')
		cfgname++;
	if (chdir("/rootfs") == -1)
		err(1, "chdir rootfs");

	if ((fd = open(cfgname, O_RDONLY)) == -1)
		err(1, "open %s", cfgname);
	if (stat(cfgname, &sb) == -1)
		err(1, "stat %s", cfgname);

	if (sb.st_size > CFGMAXSIZE)
		errx(1, "unbelievable cfg file size, increase CFGMAXSIZE");
	if ((p = malloc(sb.st_size+1)) == NULL)
		err(1, "cfgname storage");

	if (read(fd, p, sb.st_size) != sb.st_size)
		err(1, "read cfgfile");
	close(fd);

	p[sb.st_size] = '\0';
	return p;
}


#define ROOTCFG "_RUMPRUN_ROOTFSCFG="
static const size_t rootcfglen = sizeof(ROOTCFG)-1;
static char *
rumprun_config_path(char *cmdline)
{
	char *cfg = strstr(cmdline, ROOTCFG);

	if (cfg != NULL)
		cfg += rootcfglen;

	return cfg;
}
#undef ROOTCFG

void
rumprun_config(char *cmdline)
{
	char *cfg;
	struct rumprun_exec *rre;
	jvalue *root;
	bool have_config = true;

	/* is the config file on rootfs?  if so, mount & dig it out */
	cfg = rumprun_config_path(cmdline);
	if (cfg != NULL) {
		cmdline = getcmdlinefromroot(cfg);
		if (cmdline == NULL)
			errx(1, "could not get cfg from rootfs");
	}

	while (*cmdline != '{') {
		if (*cmdline == '\0') {
			warnx("could not find start of json.  no config?");
			have_config = false;
			break;
		}
		cmdline++;
	}

	if (have_config) {
		root = jparse(cmdline);
		if (!root) {
			errx(1, "jparse failed");
		}
		handle_object(root, handlers_root, __func__);
		jdel(root);
	}

	/*
	 * If no configuration or rc[] is passed, we populate rumprun_execs[]
	 * using the information in rumprun_bins[].
	 */
	rre = TAILQ_LAST(&rumprun_execs, rumprun_execs);
	if (rre == NULL) {
		int nbins = handle_rc_dummy();
		if (nbins == 0) {
			errx(1, "internal error: no rumprun_execs[]");
		}
		rre = TAILQ_LAST(&rumprun_execs, rumprun_execs);
	}

	/*
	 * Before we start running things, perform some sanity checks
	 */
	if (rre->rre_flags & RUMPRUN_EXEC_PIPE) {
		errx(1, "rumprun_config: last rc entry may not output to pipe");
	}
}
