#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <semaphore.h>
#include <assert.h>

typedef long double quad;

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif

#ifndef FLOAT
#define FLOAT FAUSTFLOAT
#endif

static struct {
	unsigned sr = 44100, nr = 10, bs = 512, sk, xt;
	double   nr_s, sk_s;
	unsigned NO, no;
	int it;
} G;

static int cli_stop = -1;
#define CLI_STOP do { if (cli_stop < 0) cli_stop = i0; } while (0)

static void ui_add_opt(const char *, FAUSTFLOAT*, FAUSTFLOAT);

struct Meta {
	void declare(const char *k, const char *v);
};
struct UI {

	#define	F(n)	inline void n(...) {}
	F(openVerticalBox) F(openHorizontalBox) F(closeBox) F(declare)
	#undef	F

	#define F(n)	inline void n(const char *n, FAUSTFLOAT *e, FAUSTFLOAT v, ...) \
			{ ui_add_opt(n, e, v); }
	F(addNumEntry) F(addVerticalSlider) F(addHorizontalSlider)
	#undef	F

	#define F(n)	inline void n(const char *n, FAUSTFLOAT *e) \
			{ ui_add_opt(n, e, 0); }
	F(addButton) F(addCheckButton)
	#undef	F
};

<<includeIntrinsic>>
<<includeclass>>

static mydsp DSP;

#define NOUTS	16
#define BUFSZ	1024
static FAUSTFLOAT _outputs[NOUTS][BUFSZ];

#define eprint(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

static int _try_;
#define die(fmt, ...) do {					\
	fprintf(stderr, "ERR!! " fmt "\n", ##__VA_ARGS__);	\
	if (_try_) { throw 0; } exit(1);			\
} while (0)

#define TRY(expr)	\
	do { try { _try_ = 1; expr; _try_ = 0; } catch (...) {} } while (0)

// ----------------------------------------------------------------------------
#define FIFO_FD	1

static bool fifo_run(const char *fifo, const char *comm, const char *argv[])
{
	struct stat st;
	int r = 0, fd;

	if (stat(fifo, &st) == 0) {
		if (!S_ISFIFO(st.st_mode))
			die("'%s' is not a fifo.", fifo);
	} else {
		eprint("CLI: create '%s' ...\n", fifo);
		assert(mkfifo(fifo, 0666) == 0);
		goto start;
	}

	fd = open(fifo, O_WRONLY|O_NONBLOCK);
	if (fd < 0)
		assert(errno == ENXIO);
	else {
		int nb = 0;
		assert(ioctl(fd, FIONBIO, &nb) == 0);
		goto out;
	}

start:	eprint("CLI: starting '%s' ...\n", comm);
	r = 1;

	if (!fork()) {
		if (!fork()) {
			fd = open(fifo, O_RDONLY);
			// unused; keep a writer so read(fd) blocks
			open(fifo, O_WRONLY);
			dup2(fd, 0); close(fd);
			execvp(comm, (char**)argv);
			die("exec '%s' failed: %m", comm);
		}
		// unused; sync with the child's open(O_RDONLY)
		open(fifo, O_WRONLY);
		exit(0);
	}

	wait(NULL);
	fd = open(fifo, O_WRONLY);
	assert(fd >= 0);
out:
	if (fd != FIFO_FD) {
		assert(dup2(fd, FIFO_FD) == FIFO_FD);
		close(fd);
	}
	return r;
}

#define PROC_OPEN()					\
	int pfd[2]; char chan[64];			\
	assert(pipe(pfd) == 0);				\
	snprintf(chan, sizeof(chan), "/proc/%d/fd/%d",	\
		 getpid(),pfd[1])

#define PROC_CLOSE(tmpf)				\
	int r = read(pfd[0], chan, sizeof(chan)-1);	\
	if (r <= 0 || strncmp(chan, "ACK\n", r))	\
		die("bad ACK from pipe.");		\
	close(FIFO_FD); close(pfd[0]); close(pfd[1]);	\
	O_B::eof(); unlink(tmpf)

//-----------------------------------------------------------------------------
static struct O_N {
	virtual bool _ck(void)		{ return true; }
	virtual bool cli(char*)		{ return false; }
	virtual void ini(void)		{}
	virtual void out(unsigned)	{};
	virtual void eob(void)		{}
	virtual void eof(void)		{}
} __o_n;

static struct O_T : public O_N {
	void out(unsigned i)
	{
		for (unsigned o = 0; o < G.no; o++) {
			printf("%#-+18.12g%c", double(_outputs[o][i]),
				o == G.no-1 ? '\n' : '\t');
		}
	}
	void eof()
	{
		fflush(stdout);
	}
} __o_t;

static struct O_B : public O_N {
	FLOAT buf[NOUTS * BUFSZ];
	int ofd = 1, cnt = 0;

	void out(unsigned i)
	{
		for (unsigned o = 0; o < G.no; o++) {
			assert(cnt < NOUTS * BUFSZ);
			buf[cnt++] = _outputs[o][i];
		}
	}
	void eob(void)
	{
		write(ofd, buf, sizeof(buf[0]) * cnt);
		cnt = 0;
	}
	void eof()
	{
		close(ofd);
	}
} __o_b;

static struct O_GP : public O_B {
	char ylims[32] = {0};

	bool cli(char *arg)
	{
		assert(strlen(arg) < sizeof(ylims));
		strcpy(ylims, arg);
		return true;
	}

	void ini(void)
	{
		const char *argv[] = { "CLI-gnuplot", NULL };
		const char *icmd =
			"do for [c=0:9] { bind sprintf('%d', c) sprintf('toggle %d', c+1) }\n"
			"set grid\n";

		if (fifo_run("/tmp/gp.fifo", "gnuplot", argv))
			write(FIFO_FD, icmd, strlen(icmd));

		ofd = open("/tmp/gp.data", O_CREAT|O_TRUNC|O_WRONLY, 0666);
		assert(ofd >= 0);
	}

	void eof(void)
	{
		const char *dt	= sizeof(FLOAT) == 4 ? "%float"
				: sizeof(FLOAT) == 8 ? "%double"
				: NULL;
		if (!dt) die("unsupported gnuplot datasize");

		dprintf(FIFO_FD, "FN='/tmp/gp.data'; DT='%s'; NO=%d\n"
			"BF = ''; do for [O=1:NO] { BF = BF . DT }\n"
			"plot [][%s] for [O=1:NO] FN volatile binary format=BF "
			"u O w l t sprintf('%%d',O-1)\n",
			dt, G.no, ylims);

		PROC_OPEN();
		dprintf(FIFO_FD, "set print '%s'; print 'ACK'; set print\n", chan);
		PROC_CLOSE("/tmp/gp.data");
	}
} __o_gp;

static struct O_IR : public O_B {
	int norm;

	void ini(void)
	{
		const char *argv[] = { "CLI-plot_ir", NULL };
		fifo_run("/tmp/ir.fifo", "plot_ir", argv);

		ofd = open("/tmp/ir.data", O_CREAT|O_TRUNC|O_WRONLY, 0666);
		assert(ofd >= 0);
	}

	void eof(void)
	{
		PROC_OPEN();
		dprintf(FIFO_FD, "/tmp/ir.data %d %ld %d %d %s\n",
				norm, sizeof(FLOAT), G.no, G.sr, chan);
		PROC_CLOSE("/tmp/ir.data");
	}
} __o_ir;

struct O_SOX : public O_B {
	bool _ck(void) { return !!file; }

	const char *file;
	int pid;

	void ini(void)
	{
		int fds[2];
		assert(!pipe(fds));

		if ((pid = fork())) {
			ofd = fds[1];
			close(fds[0]);
		} else {
			close(fds[1]);
			assert(dup2(fds[0], 0) == 0);

			char o_b[16], o_r[64], o_c[64];
			sprintf(o_b, "-b%d", int(sizeof(FLOAT))*8);
			sprintf(o_r, "-r%d", G.sr);
			sprintf(o_c, "-c%d", G.no);

			const char *argv[] = {
				file ? "sox" : "play",
				"-q", "-traw", "-ef",
				o_b, o_r, o_c,
				"-",
				file ? "-t.wav" : NULL,
				file,
				NULL,
			};

			execvp(argv[0], (char**)argv);
			die("exec '%s' failed: %m", argv[0]);
		}
	}

	void eof(void)
	{
		O_B::eof();
		waitpid(pid, NULL, 0);
	}
} __o_sox;

static const char *__ON = "t";
static struct O_N *O = &__o_t;

// ----------------------------------------------------------------------------
static unsigned ARGC;
static struct {
	const char *n;
	FAUSTFLOAT *v;
} ARGV[32];

static __typeof__(ARGV+0) cli_get_opt(const char *n)
{
	for (unsigned i = 0; i < ARGC; ++i)
		if (!strcmp(n,  ARGV[i].n))
			return &ARGV[i];
	return NULL;
}

static void ui_add_opt(const char *__n, FAUSTFLOAT *e, FAUSTFLOAT v)
{
	char *n = strdup(__n);
	for (char *p = n; *p; p++)
		if (isspace(*p)) *p = '_';
	assert(ARGC < sizeof(ARGV)/sizeof(ARGV[0]));
	assert(!cli_get_opt(n));
	auto o = ARGV + ARGC++;
	*(o->v = e) = v;
	o->n = n;
}

static void cli_add_opt(char *n, char *p)
{
	*p++ = 0;
	char *e; FAUSTFLOAT v = strtod(p, &e);
	if (e == p || *e) die("bad number: '%s'", p);

	auto o = cli_get_opt(n);
	if (!o)	eprint("WARN! unused opt '%s'\n", n);
	else	*o->v = v;
}

// ----------------------------------------------------------------------------
#define	IF(a)	if (!strcmp(n, #a))

static void parse_o(char *n)
{
	static char __on[64];
	char *p = NULL;

	if (n && (p = strchr(n, '=')))
		*p++ = 0;

	if (!n)
		goto err;
	else IF (t)
		O = &__o_t;
	else IF (n)
		O = &__o_n;
	else IF (b)
		O = &__o_b;
	else IF (p)
		O = &__o_sox, __o_sox.file = NULL;
	else IF (f)
		O = &__o_sox, __o_sox.file = "-";
	else IF (gp)
		O = &__o_gp;
	else IF (ir)
		O = &__o_ir, __o_ir.norm = 0;
	else IF (fr)
		O = &__o_ir, __o_ir.norm = 1;
	else
		err: die("bad -o name: '%s'", n);

	__ON = strcpy(__on, n);
	if (!p) return;

	if (!O->cli(p))
		die("bad arg '%s' for -o %s", p, n);

	assert(strlen(n) + 1 + strlen(p) < sizeof(__on));
	if (*p) sprintf(__on + strlen(n), "=%s", p);
}

static struct { FAUSTFLOAT g,s; } GV[NOUTS];
static unsigned GN;

static void apply_g(unsigned i)
{
	for (unsigned __o = 0, o = 0; o < G.NO; o++) {
		if (!GV[o].g) continue;
		_outputs[__o++][i] = _outputs[o][i] * GV[o].g + GV[o].s;
	}
}
static void parse_g(const char *p)
{
	G.no = GN = 0;
	memset(GV, 0, sizeof(GV));

	if (!p) goto err;
	if (!strcmp(p, "-")) return;

	for (char *e;; p = e + 1) {
		if (GN >= G.NO)
			die("-g: too many args");
		if ((GV[GN].g = strtod(p, &e)))
			G.no++;
		if (e != p) {
			if (*e == '+' || *e == '-')
				GV[GN].s = strtod(p = e, &e);
			GN++;
			if (*e == ',') continue;
			if (*e == '\0') break;
		}
		err: die("-g: bad number: '%s'", p);
	}

	if (!G.no) die("-g: no outputs");
}

static void parse_G(const char *n, const char *v)
{
	int x; float f;
	char *e = NULL;
	bool s = false;

	if (n[0] != '-' || n[1] ==  0)  goto err;
	if (n[1] == 'N' || n[1] == 'S') s = true;

	if (v) {
		if (s) f = strtod(v, &e);
		else   x = strtoll(v, &e, 0);
	}
	if (e == v || *e)
		die("%s: bad number: '%s'", n, v);

	if (s) {
		if (n[2]) goto err;
		switch (n[1]) {
		case 'N': G.nr_s = f; G.nr = 0; break;
		case 'S': G.sk_s = f; G.sk = 0; break;
		}
		return;
	}
	for (const char *o = n;;)
		switch (*++o) {
		case   0: return;
		case 'n': G.nr = x; G.nr_s = 0; break;
		case 's': G.sk = x; G.sk_s = 0; break;
		case 'r': G.sr = x; break;
		case 'x': G.xt = x; break;
		case 'b': G.bs = x; break;
		default: goto err;
		};

err:	die("bad option '%s'", n);
}

static void parse_args(char* argv[])
{
	for (char *n; (n = *argv++);) {
		if (char *p = strchr(n, '='))
			cli_add_opt(n, p);
		else IF (-i)
			G.it = 1;
		else IF (-o)
			parse_o(*argv++);
		else IF (-g)
			parse_g(*argv++);
		else
			parse_G(n, *argv++);
	}

	assert(G.bs <= BUFSZ);
	if (!GN) G.no = G.NO;
}

static void dump_args(void)
{
	eprint("  ! -r %d", G.sr);
	if (G.sk_s || G.sk)
		G.sk_s ? eprint(" -S %g", G.sk_s) : eprint(" -s %d", G.sk);
	if (1)
		G.nr_s ? eprint(" -N %g", G.nr_s) : eprint(" -n %d", G.nr);
	for (unsigned gn = 0; gn < GN; ++gn) {
		gn == 0 ? eprint(" -g ") : eprint(",");
		eprint("%g", GV[gn].g);
		if (GV[gn].s) eprint("%+g", GV[gn].s);
	}
	eprint(" -o %s\n", __ON);
}

// ----------------------------------------------------------------------------
static struct { char *k, *v; } __map[128];

static char *__it_getline(void)
{
	static char *line = NULL;
	static size_t size = 0;

	eprint(": ");
	return getline(&line, &size, stdin) >= 0 ? line : NULL;
}

#include <readline/readline.h>
#include <readline/history.h>
static char *rl_next_match(const char *inp, int state)
{
	static unsigned idx, len;

	if (!state) { idx = 0; len = strlen(inp); }

	while (idx < ARGC) {
		auto arg = ARGV + idx++;
		if (!strncmp(arg->n, inp, len))
			return strdup(arg->n);
	}

	while (idx - ARGC < sizeof(__map)/sizeof(__map[0])) {
		auto kv = __map + idx++ - ARGC;
		if (!kv->k) break;
		if (!strncmp(kv->k, inp, len))
			return strdup(kv->k);
	}

	return NULL;
}
static char **rl_cmpl_func(const char *inp, int start, int end)
{
	rl_attempted_completion_over = 1;
	return rl_completion_matches(inp, rl_next_match);
}
void __rl_on_exit(int, void *)
{
	kill(getpid(), SIGINT); // why ???
	rl_deprep_terminal();
};
static char *__it_readline(void)
{
	static char *inp;

	if (inp) free(inp);
	else {
		assert((rl_instream  = fopen("/dev/tty", "r")));
		assert((rl_outstream = fopen("/dev/tty", "w")));
		rl_attempted_completion_function = rl_cmpl_func;
		on_exit(__rl_on_exit, NULL);
	}

	inp = readline(": ");
	if (inp && *inp) {
		for (int i = history_length; i; --i) {
			HIST_ENTRY *he = history_get(i);
			if (!strcmp(he->line, inp)) {
				remove_history(i-1);
				break;
			}
		}
		add_history(inp);
	}
	return inp;
}

static inline char *it_readline(void)
{
	return 1 ? __it_readline() : __it_getline();
}

// ----------------------------------------------------------------------------
static char *map(const char *k, const char *v)
{
	__typeof__(__map + 0) kv = NULL;

	for (auto &__kv : __map)
		if (!__kv.k || !strcmp(__kv.k, k)) {
			kv = &__kv;
			break;
		}

	if (!kv)
		return NULL;
	else if (v) {
		if (!kv->k)   kv->k = strdup(k);
		free(kv->v);  kv->v = strdup(v);
	}

	return kv->v;
}

static struct { sem_t sem[2]; char *cmd; } IT;

static unsigned parse_it_cmd(int rec, unsigned argc, const char *cmd)
{
	static char* argv[32];

	assert(rec < 8);
	#define PUSH(arg) \
		do { free(argv[argc]), argv[argc++] = arg; } while (0)

	for (const char *m, *p = cmd;;) {
		while (*p &&  isspace(*p)) ++p;
		const char *a = p;
		while (*p && !isspace(*p)) ++p;
		if (a == p) break;

		assert(argc + 1 < sizeof(argv)/sizeof(argv[0]));
		char *arg = strndup(a, p - a);
		if ((m = map(arg, NULL))) {
			argc = parse_it_cmd(rec+1, argc, m);
			free(arg);
		} else {
			PUSH(arg);
		}
	}

	if (!rec) {
		PUSH(NULL);
		parse_args(argv);
	}

	#undef PUSH
	return argc;
}

static void *it_loop(void *)
{
	for (;;) {
		char *inp;
		char *p, c;  int eat;
		char n[128]; float v;

		#define next(fmt, ...) do {				\
			eprint("ERR!! " fmt "\n", ##__VA_ARGS__);	\
			goto next;					\
		} while (0)
next:
		if(!(inp = it_readline())) _exit(0);
		if ((p = strchr(inp, '\n'))) *p = 0;
		if ((p = strchr(inp,  '#'))) *p = 0;
		if (!*inp) goto dump;

		if (*inp == '!') {
			cli_stop = 0;
			if (inp[1] == '!') continue;
			IT.cmd = inp + 1;
			sem_post(IT.sem+0);
			sem_wait(IT.sem+1);
			continue;
		}

		if (sscanf(inp, " %127[^=: ] %[:] %n", n,&c,&eat) == 2) {
			char *v = inp + eat;
			if (!*v) {
				static char dump[1024] = ""; // for ARGC == 0
				int sz = sizeof(dump), wr = 0;
				for (unsigned i = 0; i < ARGC; ++i) {
					int w = snprintf(dump+wr,sz,"%s=%.16g ",
						ARGV[i].n,  double(*ARGV[i].v));
					wr += w; sz -= w;
					if (sz <= 0)
						die("dump[] overflow.\n");
				}
				v = dump;
			}

			if (!map(n, v))
				eprint("ERR!! map is full.\n");
			continue;
		}

		if (sscanf(inp, " %127[^=: ] %c", n,&c) == 1) {
			if (!(inp = map(n, NULL)))
				next("undefined '%s'", n);
		}

		for (; *inp; inp += eat) {
			if (sscanf(inp, " %127[^= ] %*[=] %f %n", n,&v,&eat) != 2)
				next("can't parse '%s'", inp);
			auto o = cli_get_opt(n);
			if (!o)
				next("bad option '%s'", n);
			*o->v = v;
		}
		continue;

dump:		dump_args();
		for (const auto &kv : __map) {
			if (kv.k) eprint("  %s: %s\n", kv.k, kv.v);
			else	  break;
		}
		eprint("\n");
		for (unsigned i = 0; i < ARGC; ++i)
			eprint("  %-16s % -.8g\n", ARGV[i].n, double(*ARGV[i].v));
		eprint("\n");
	}
}

int main(int argc, char* argv[])
{
	#ifdef CLI_INIT
	CLI_INIT
	#endif

	if (DSP.getNumInputs() > 0)
		die("no inputs allowed");

	G.NO = DSP.getNumOutputs();
	assert(G.NO <= NOUTS);

	FAUSTFLOAT *outputs[NOUTS];
	for (int o = 0; o < NOUTS; o++)
		outputs[o] = _outputs[o];

	DSP.buildUserInterface(NULL);
	parse_args(++argv);

restart:
	if (G.it) {
		static int run; if (!run++) {
			pthread_t t;
			sem_init(IT.sem+0, 0,0);
			sem_init(IT.sem+1, 0,0);
			pthread_create(&t, NULL, it_loop, NULL);
		}
		do {
			if (sem_wait(IT.sem+0)) exit(1);
			TRY(parse_it_cmd(0, 0, IT.cmd));
			sem_post(IT.sem+1);
			cli_stop = -1;
		} while (_try_);
	}

	DSP.instanceClear();
	DSP.instanceConstants(G.sr);

	O->ini();
	bool _ck = O->_ck(); unsigned total = 0;
	unsigned G_nr = G.nr ?: G.nr_s * G.sr + .5;
	unsigned G_sk = G.sk ?: G.sk_s * G.sr + .5;
	if (G_nr <= G_nr + G_sk) G_nr += G_sk; // avoid overflow
	for (unsigned count, stopped = 0, nr = G_nr; nr; nr -= count) {
		count = G.bs;
		if (count > nr) count = nr;

		DSP.compute(count, 0, outputs);
		if (cli_stop >= 0 && !stopped) {
			nr = cli_stop + G.xt;
			if (count > nr) count = nr;
			stopped = 1;
		}

		for (unsigned i = 0; i < count; i++) {
			if (G_sk) G_sk--;
			else {
				if (GN) apply_g(i);
				O->out(i);
			}
		}
		O->eob();

		if (_ck && (total += count) >= 1000000) {
			eprint("WARN! stop at total=%d\n", total);
			break;
		}
	}
	O->eof();

	if (G.it) goto restart;
	return 0;
}
