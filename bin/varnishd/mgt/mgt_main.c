/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The management process and CLI handling
 */

#include "config.h"

#include <sys/utsname.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "mgt/mgt.h"
#include "common/heritage.h"

#include "hash/hash_slinger.h"
#include "vav.h"
#include "vcli.h"
#include "vcli_common.h"
#include "vev.h"
#include "vfil.h"
#include "vin.h"
#include "vpf.h"
#include "vrnd.h"
#include "vsha256.h"
#include "vtim.h"

#include "compat/daemon.h"

struct heritage		heritage;
unsigned		d_flag = 0;
pid_t			mgt_pid;
struct vev_base		*mgt_evb;
int			exit_status = 0;
struct vsb		*vident;
struct VSC_C_mgt	static_VSC_C_mgt;
struct VSC_C_mgt	*VSC_C_mgt;

static struct vpf_fh *pfh = NULL;

/*--------------------------------------------------------------------*/

static void
mgt_sltm(const char *tag, const char *sdesc, const char *ldesc)
{
	int i;

	assert(sdesc != NULL && ldesc != NULL);
	assert(*sdesc != '\0' || *ldesc != '\0');
	printf("\n%s\n", tag);
	i = strlen(tag);
	printf("%*.*s\n\n", i, i, "------------------------------------");
	if (*ldesc != '\0')
		printf("%s\n", ldesc);
	else if (*sdesc != '\0')
		printf("%s\n", sdesc);
}

/*lint -e{506} constant value boolean */
static void
mgt_DumpRstVsl(void)
{

	printf(
	    "\n.. The following is autogenerated output from "
	    "varnishd -x dumprstvsl\n\n");

#define SLTM(tag, flags, sdesc, ldesc) mgt_sltm(#tag, sdesc, ldesc);
#include "tbl/vsl_tags.h"
#undef SLTM
}

/*--------------------------------------------------------------------*/

static void
build_vident(void)
{
	struct utsname uts;

	vident = VSB_new_auto();
	AN(vident);
	if (!uname(&uts)) {
		VSB_printf(vident, ",%s", uts.sysname);
		VSB_printf(vident, ",%s", uts.release);
		VSB_printf(vident, ",%s", uts.machine);
	}
}

/*--------------------------------------------------------------------*/

const void *
pick(const struct choice *cp, const char *which, const char *kind)
{

	for(; cp->name != NULL; cp++) {
		if (!strcmp(cp->name, which))
			return (cp->ptr);
	}
	ARGV_ERR("Unknown %s method \"%s\"\n", kind, which);
}

/*--------------------------------------------------------------------*/

static void
usage(void)
{
#define FMT "    %-28s # %s\n"

	fprintf(stderr, "usage: varnishd [options]\n");
	fprintf(stderr, FMT, "-a address:port", "HTTP listen address and port");
	fprintf(stderr, FMT, "-b address:port", "backend address and port");
	fprintf(stderr, FMT, "", "   -b <hostname_or_IP>");
	fprintf(stderr, FMT, "", "   -b '<hostname_or_IP>:<port_or_service>'");
	fprintf(stderr, FMT, "-C", "print VCL code compiled to C language");
	fprintf(stderr, FMT, "-d", "debug");
	fprintf(stderr, FMT, "-f file", "VCL script");
	fprintf(stderr, FMT, "-F", "Run in foreground");
	fprintf(stderr, FMT, "-h kind[,hashoptions]", "Hash specification");
	fprintf(stderr, FMT, "", "  -h critbit [default]");
	fprintf(stderr, FMT, "", "  -h simple_list");
	fprintf(stderr, FMT, "", "  -h classic");
	fprintf(stderr, FMT, "", "  -h classic,<buckets>");
	fprintf(stderr, FMT, "-i identity", "Identity of varnish instance");
	fprintf(stderr, FMT, "-j jail[,jailoptions]", "Jail specification");
#ifdef HAVE_SETPPRIV
	fprintf(stderr, FMT, "", "  -j solaris");
#endif
	fprintf(stderr, FMT, "", "  -j unix[,user=<user>][,ccgroup=<group>]");
	fprintf(stderr, FMT, "", "  -j none");
	fprintf(stderr, FMT, "-l shl,free,fill", "Size of shared memory file");
	fprintf(stderr, FMT, "", "  shl: space for SHL records [80m]");
	fprintf(stderr, FMT, "", "  free: space for other allocations [1m]");
	fprintf(stderr, FMT, "", "  fill: prefill new file [+]");
	fprintf(stderr, FMT, "-M address:port", "Reverse CLI destination.");
	fprintf(stderr, FMT, "-n dir", "varnishd working directory");
	fprintf(stderr, FMT, "-P file", "PID file");
	fprintf(stderr, FMT, "-p param=value", "set parameter");
	fprintf(stderr, FMT, "-r param[,param...]", "make parameter read-only");
	fprintf(stderr, FMT,
	    "-s [name=]kind[,options]", "Backend storage specification");
	fprintf(stderr, FMT, "", "  -s malloc[,<size>]");
#ifdef HAVE_LIBUMEM
	fprintf(stderr, FMT, "", "  -s umem");
#endif
	fprintf(stderr, FMT, "", "  -s file,<dir_or_file>");
	fprintf(stderr, FMT, "", "  -s file,<dir_or_file>,<size>");
	fprintf(stderr, FMT, "",
	    "  -s file,<dir_or_file>,<size>,<granularity>");
	fprintf(stderr, FMT, "", "  -s persist{experimental}");
	fprintf(stderr, FMT, "-S secret-file",
	    "Secret file for CLI authentication");
	fprintf(stderr, FMT, "-T address:port",
	    "Telnet listen address and port");
	fprintf(stderr, FMT, "-t", "Default TTL");
	fprintf(stderr, FMT, "-V", "version");
#undef FMT
	exit(1);
}

/*--------------------------------------------------------------------*/

static void
cli_check(const struct cli *cli)
{
	if (cli->result == CLIS_OK) {
		VSB_clear(cli->sb);
		return;
	}
	AZ(VSB_finish(cli->sb));
	fprintf(stderr, "Error:\n%s\n", VSB_data(cli->sb));
	exit(2);
}

/*--------------------------------------------------------------------
 * All praise POSIX!  Thanks to our glorious standards there are no
 * standard way to get a back-trace of the stack, and even if we hack
 * that together from spit and pieces of string, there is no way no
 * standard way to translate a pointer to a symbol, which returns anything
 * usable.  (See for instance FreeBSD PR-134391).
 *
 * Attempt to run nm(1) on our binary during startup, hoping it will
 * give us a usable list of symbols.
 */

struct symbols {
	uintptr_t		a;
	uintptr_t		l;
	char			*n;
	VTAILQ_ENTRY(symbols)	list;
};

static VTAILQ_HEAD(,symbols) symbols = VTAILQ_HEAD_INITIALIZER(symbols);

int
Symbol_Lookup(struct vsb *vsb, void *ptr)
{
	struct symbols *s, *s0;
	uintptr_t pp;

	pp = (uintptr_t)ptr;
	s0 = NULL;
	VTAILQ_FOREACH(s, &symbols, list) {
		if (s->a > pp || s->a + s->l < pp)
			continue;
		if (s0 == NULL || s->l < s0->l)
			s0 = s;
	}
	if (s0 == NULL)
		return (-1);
	VSB_printf(vsb, "%p: %s+0x%jx", ptr, s0->n, (uintmax_t)pp - s0->a);
	return (0);
}

static void
Symbol_hack(const char *a0)
{
	char buf[BUFSIZ];
	FILE *fi;
	struct symbols *s;
	uintmax_t aa, ll;
	char type[10];
	char name[100];
	int i;

	bprintf(buf, "nm -t x -n -P %s 2>/dev/null", a0);
	fi = popen(buf, "r");
	if (fi == NULL)
		return;
	while (fgets(buf, sizeof buf, fi)) {
		i = sscanf(buf, "%99s\t%9s\t%jx\t%jx\n", name, type, &aa, &ll);
		if (i != 4)
			continue;
		s = malloc(sizeof *s + strlen(name) + 1);
		AN(s);
		s->a = aa;
		s->l = ll;
		s->n = (void*)(s + 1);
		strcpy(s->n, name);
		VTAILQ_INSERT_TAIL(&symbols, s, list);
	}
	(void)pclose(fi);
}

/*--------------------------------------------------------------------
 * This function is called when the CLI on stdin is closed.
 */

static void
cli_stdin_close(void *priv)
{

	(void)priv;
	(void)close(0);
	(void)close(1);
	(void)close(2);
	AZ(open("/dev/null", O_RDONLY));
	assert(open("/dev/null", O_WRONLY) == 1);
	assert(open("/dev/null", O_WRONLY) == 2);

	if (d_flag) {
		mgt_stop_child();
		mgt_cli_close_all();
		if (pfh != NULL)
			(void)VPF_Remove(pfh);
		exit(0);
	}
}

/*--------------------------------------------------------------------*/

static void
mgt_secret_atexit(void)
{

	/* Only master process */
	if (getpid() != mgt_pid)
		return;
	VJ_master(JAIL_MASTER_FILE);
	AZ(unlink("_.secret"));
	VJ_master(JAIL_MASTER_LOW);
}

static const char *
make_secret(const char *dirname)
{
	char *fn;
	int fd;
	int i;
	unsigned char buf[256];

	assert(asprintf(&fn, "%s/_.secret", dirname) > 0);

	VJ_master(JAIL_MASTER_FILE);
	fd = open(fn, O_RDWR|O_CREAT|O_TRUNC, 0640);
	if (fd < 0) {
		fprintf(stderr, "Cannot create secret-file in %s (%s)\n",
		    dirname, strerror(errno));
		exit(1);
	}
	VRND_Seed();
	for (i = 0; i < sizeof buf; i++)
		buf[i] = random() & 0xff;
	assert(sizeof buf == write(fd, buf, sizeof buf));
	AZ(close(fd));
	VJ_master(JAIL_MASTER_LOW);
	AZ(atexit(mgt_secret_atexit));
	return (fn);
}

/*--------------------------------------------------------------------*/

static char stackmin[20];
static char stackdef[20];

static void
init_params(struct cli *cli)
{
	ssize_t def, low;

	MCF_CollectParams();

	MCF_TcpParams();

	if (sizeof(void *) < 8) {
		/*
		 * Adjust default parameters for 32 bit systems to conserve
		 * VM space.
		 */
		MCF_SetDefault("workspace_client", "24k");
		MCF_SetDefault("workspace_backend", "16k");
		MCF_SetDefault("http_resp_size", "8k");
		MCF_SetDefault("http_req_size", "12k");
		MCF_SetDefault("gzip_buffer", "4k");
	}

	low = sysconf(_SC_THREAD_STACK_MIN);
	bprintf(stackmin, "%jd", (intmax_t)low);
	MCF_SetMinimum("thread_pool_stack", stackmin);

	def = 48 * 1024;
	if (def < low)
		def = low;
	bprintf(stackdef, "%jd", (intmax_t)def);
	MCF_SetDefault("thread_pool_stack", stackdef);

	MCF_InitParams(cli);
}

/*--------------------------------------------------------------------*/

int
main(int argc, char * const *argv)
{
	int o;
	unsigned C_flag = 0;
	unsigned F_flag = 0;
	const char *b_arg = NULL;
	const char *f_arg = NULL;
	const char *i_arg = NULL;
	const char *h_arg = "critbit";
	const char *M_arg = NULL;
	const char *n_arg = NULL;
	const char *P_arg = NULL;
	const char *S_arg = NULL;
	const char *s_arg = "malloc,100m";
	int s_arg_given = 0;
	const char *T_arg = "localhost:0";
	char *p, *vcl = NULL;
	struct cli cli[1];
	char *dirname;
	char **av;
	unsigned clilim;
	int jailed = 0;

	/* Set up the mgt counters */
	memset(&static_VSC_C_mgt, 0, sizeof static_VSC_C_mgt);
	VSC_C_mgt = &static_VSC_C_mgt;

	/*
	 * Start out by closing all unwanted file descriptors we might
	 * have inherited from sloppy process control daemons.
	 */
	for (o = getdtablesize(); o > STDERR_FILENO; o--)
		(void)close(o);

	VRND_Seed();

	mgt_got_fd(STDERR_FILENO);

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	build_vident();

	Symbol_hack(argv[0]);

	/* for ASSERT_MGT() */
	mgt_pid = getpid();

	assert(VTIM_parse("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777);
	assert(VTIM_parse("Sunday, 06-Nov-94 08:49:37 GMT") == 784111777);
	assert(VTIM_parse("Sun Nov  6 08:49:37 1994") == 784111777);

	/* Check that our SHA256 works */
	SHA256_Test();

	/* Create a cli for convenience in otherwise CLI functions */
	INIT_OBJ(cli, CLI_MAGIC);
	cli[0].sb = VSB_new_auto();
	XXXAN(cli[0].sb);
	cli[0].result = CLIS_OK;
	clilim = 32768;
	cli[0].limit = &clilim;

	/* Various initializations */
	VTAILQ_INIT(&heritage.socks);
	mgt_evb = vev_new_base();
	AN(mgt_evb);

	init_params(cli);
	cli_check(cli);

	while ((o = getopt(argc, argv,
	    "a:b:Cdf:Fh:i:j:l:M:n:P:p:r:S:s:T:t:Vx:")) != -1) {
		/*
		 * -j must be the first argument if specified, because
		 * it (may) affect subsequent argument processing.
		 */
		if (!jailed) {
			jailed++;
			if (o == 'j') {
				VJ_Init(optarg);
				continue;
			}
			VJ_Init(NULL);
		} else {
			if (o == 'j')
				ARGV_ERR("\t-j must be the first argument\n");
		}

		switch (o) {
		case 'a':
			MAC_Arg(optarg);
			break;
		case 'b':
			b_arg = optarg;
			break;
		case 'C':
			C_flag = 1 - C_flag;
			break;
		case 'd':
			d_flag++;
			break;
		case 'f':
			f_arg = optarg;
			break;
		case 'F':
			F_flag = 1 - F_flag;
			break;
		case 'h':
			h_arg = optarg;
			break;
		case 'i':
			i_arg = optarg;
			break;
		case 'l':
			av = VAV_Parse(optarg, NULL, ARGV_COMMA);
			AN(av);
			if (av[0] != NULL)
				ARGV_ERR("\t-l ...: %s", av[0]);
			if (av[1] != NULL) {
				MCF_ParamSet(cli, "vsl_space", av[1]);
				cli_check(cli);
			}
			if (av[1] != NULL && av[2] != NULL) {
				MCF_ParamSet(cli, "vsm_space", av[2]);
				cli_check(cli);
			}
			VAV_Free(av);
			break;
		case 'M':
			M_arg = optarg;
			break;
		case 'n':
			n_arg = optarg;
			break;
		case 'P':
			P_arg = optarg;
			break;
		case 'p':
			p = strchr(optarg, '=');
			if (p == NULL)
				usage();
			AN(p);
			*p++ = '\0';
			MCF_ParamSet(cli, optarg, p);
			*--p = '=';
			cli_check(cli);
			break;
		case 'r':
			MCF_ParamProtect(cli, optarg);
			cli_check(cli);
			break;
		case 's':
			s_arg_given = 1;
			STV_Config(optarg);
			break;
		case 't':
			MCF_ParamSet(cli, "default_ttl", optarg);
			break;
		case 'S':
			S_arg = optarg;
			break;
		case 'T':
			T_arg = optarg;
			break;
		case 'V':
			/* XXX: we should print the ident here */
			VCS_Message("varnishd");
			exit(0);
		case 'x':
			if (!strcmp(optarg, "dumprstparam")) {
				MCF_DumpRstParam();
				exit(0);
			}
			if (!strcmp(optarg, "dumprstvsl")) {
				mgt_DumpRstVsl();
				exit(0);
			}
			usage();
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;
	if (argc != 0)
		ARGV_ERR("Too many arguments (%s...)\n", argv[0]);

	if (M_arg != NULL && *M_arg == '\0')
		M_arg = NULL;
	if (T_arg != NULL && *T_arg == '\0')
		T_arg = NULL;

	/* XXX: we can have multiple CLI actions above, is this enough ? */
	if (cli[0].result != CLIS_OK) {
		AZ(VSB_finish(cli[0].sb));
		ARGV_ERR("Failed parameter creation:\n%s\n",
		    VSB_data(cli[0].sb));
	}

	if (d_flag && F_flag)
		ARGV_ERR("Only one of -d or -F can be specified\n");

	if (b_arg != NULL && f_arg != NULL)
		ARGV_ERR("Only one of -b or -f can be specified\n");

	if (T_arg == NULL && d_flag == 0 && b_arg == NULL &&
	    f_arg == NULL && M_arg == NULL)
		ARGV_ERR("At least one of -d, -b, -f, -M or -T "
		    "must be specified\n");

	if (S_arg != NULL && *S_arg == '\0') {
		fprintf(stderr,
		    "Warning: Empty -S argument, no CLI authentication.\n");
	} else if (S_arg != NULL) {
		VJ_master(JAIL_MASTER_FILE);
		o = open(S_arg, O_RDONLY, 0);
		if (o < 0)
			ARGV_ERR("Cannot open -S file (%s): %s\n",
			    S_arg, strerror(errno));
		AZ(close(o));
		VJ_master(JAIL_MASTER_LOW);
	}

	if (f_arg != NULL) {
		vcl = VFIL_readfile(NULL, f_arg, NULL);
		if (vcl == NULL)
			ARGV_ERR("Cannot read -f file (%s): %s\n",
			    f_arg, strerror(errno));
	}

	if (VIN_N_Arg(n_arg, &heritage.name, &dirname, NULL) != 0)
		ARGV_ERR("Invalid instance (-n) name: %s\n", strerror(errno));

	if (i_arg != NULL) {
		if (strlen(i_arg) + 1 > sizeof heritage.identity)
			ARGV_ERR("Identity (-i) name too long.\n");
		strncpy(heritage.identity, i_arg, sizeof heritage.identity);
	}

	if (n_arg != NULL)
		openlog(n_arg, LOG_PID, LOG_LOCAL0);	/* XXX: i_arg ? */
	else
		openlog("varnishd", LOG_PID, LOG_LOCAL0);

	VJ_make_workdir(dirname);

	/* XXX: should this be relative to the -n arg ? */
	VJ_master(JAIL_MASTER_FILE);
	if (P_arg && (pfh = VPF_Open(P_arg, 0644, NULL)) == NULL)
		ARGV_ERR("Could not open pid/lock (-P) file (%s): %s\n",
		    P_arg, strerror(errno));
	VJ_master(JAIL_MASTER_LOW);

	mgt_vcc_init();
	mgt_vcl_init();

	if (b_arg != NULL || f_arg != NULL) {
		mgt_vcc_default(cli, b_arg, vcl, C_flag);
		if (C_flag) {
			cli_check(cli);
			AZ(VSB_finish(cli->sb));
			fprintf(stderr, "%s\n", VSB_data(cli->sb));
			exit(0);
		}
		cli_check(cli);
		free(vcl);
	} else if (C_flag)
		ARGV_ERR("-C only good with -b or -f\n");

	if (VTAILQ_EMPTY(&heritage.socks))
		ARGV_ERR("Need -a argument(s)\n");

	if (!d_flag) {
		if (b_arg == NULL && f_arg == NULL) {
			fprintf(stderr,
			    "Warning: Neither -b nor -f given,"
			    " won't start a worker child.\n"
			    "         Master process started,"
			    " use varnishadm to control it.\n");
		}
	}

	/* If no -s argument specified, process default -s argument */
	if (!s_arg_given)
		STV_Config(s_arg);

	/* Configure Transient storage, if user did not */
	STV_Config_Transient();

	HSH_config(h_arg);

	mgt_SHM_Init();

	AZ(VSB_finish(vident));

	if (S_arg == NULL)
		S_arg = make_secret(dirname);
	AN(S_arg);

	if (!d_flag && !F_flag)
		AZ(varnish_daemon(1, 0));

	/**************************************************************
	 * After this point diagnostics will only be seen with -d
	 */

	assert(pfh == NULL || !VPF_Write(pfh));

	if (d_flag)
		fprintf(stderr, "Platform: %s\n", VSB_data(vident) + 1);
	syslog(LOG_NOTICE, "Platform: %s\n", VSB_data(vident) + 1);

	mgt_pid = getpid();	/* daemon() changed this */

	if (d_flag)
		mgt_cli_setup(0, 1, 1, "debug", cli_stdin_close, NULL);

	if (*S_arg != '\0')
		mgt_cli_secret(S_arg);

	if (M_arg != NULL)
		mgt_cli_master(M_arg);
	if (T_arg != NULL)
		mgt_cli_telnet(T_arg);

	/* Instantiate VSM */
	mgt_SHM_Create();

	MGT_Run();

	if (pfh != NULL)
		(void)VPF_Remove(pfh);
	exit(exit_status);
}
