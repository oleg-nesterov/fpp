#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
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
	unsigned no, __no;
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
static struct { FAUSTFLOAT g,s; } GV[NOUTS];
static unsigned GN;

#define die(fmt, ...) do {					\
	fprintf(stderr, "ERR!! " fmt "\n", ##__VA_ARGS__);	\
	exit(1);						\
} while (0)

static void check_g(void)
{
	G.__no = G.no;
	for (unsigned o = 0; o < G.__no; o++)
		if (GV[o].g == FAUSTFLOAT(0)) --G.no;
	if (!G.no) die("-g: no outputs");
}
static void apply_g(unsigned i)
{
	for (unsigned __o = 0, o = 0; o < G.__no; o++) {
		if (GV[o].g == FAUSTFLOAT(0)) continue;
		_outputs[__o++][i] = _outputs[o][i] * GV[o].g + GV[o].s;
	}
}

// ----------------------------------------------------------------------------
static bool fifo_run(const char *fifo, const char *comm, const char *argv[])
{
	struct stat st;
	int r = 0, fd;

	if (stat(fifo, &st) == 0) {
		if ((st.st_mode & S_IFMT) != S_IFIFO)
			die("'%s' is not fifo.", fifo);
	} else {
		fprintf(stderr, "CLI: create '%s' ...\n", fifo);
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

start:	fprintf(stderr, "CLI: starting '%s' ...\n", comm);
	r = 1;

	if (!fork()) {
		if (!fork()) {
			fd = open(fifo, O_RDONLY);
			dup2(fd, 0); close(fd);
			open(fifo, O_WRONLY);
			execvp(comm, (char**)argv);
			die("exec '%s' failed: %m", comm);
		}
		// sync with the child's O_RDONLY
		fd = open(fifo, O_WRONLY);
		exit(0);
	}

	wait(NULL);
	fd = open(fifo, O_WRONLY);
	assert(fd > 0);
out:
	assert(dup2(fd, 1) == 1);
	close(fd);
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
	close(pfd[0]); close(pfd[1]);			\
	unlink(tmpf)

//-----------------------------------------------------------------------------
static struct O_N {
	virtual bool cli(char*) { return false; }
	virtual void ini(void) {}
	virtual void out(unsigned) {};
	virtual void eob(void) {}
	virtual void eof(void) {}
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
		fclose(stdout);
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
	const char *ylims = "";

	bool cli(char *arg)
	{
		ylims = arg;
		return true;
	}

	void ini(void)
	{
		const char *argv[] = { "CLI-gnuplot", NULL };
		const char *icmd =
			"do for [c=0:9] { bind sprintf('%d', c) sprintf('toggle %d', c+1) }\n"
			"set grid\n";

		if (fifo_run("/tmp/gp.fifo", "gnuplot", argv))
			write(1, icmd, strlen(icmd));

		ofd = open("/tmp/gp.data", O_CREAT|O_TRUNC|O_WRONLY, 0666);
		assert(ofd >= 0);
	}

	void eof(void)
	{
		const char *dt	= sizeof(FLOAT) == 4 ? "%float"
				: sizeof(FLOAT) == 8 ? "%double"
				: NULL;
		if (!dt) die("unsupported gnuplot datasize");

		dprintf(1, "FN='/tmp/gp.data'; DT='%s'; NO=%d\n"
			"BF = ''; do for [O=1:NO] { BF = BF . DT }\n"
			"plot [][%s] for [O=1:NO] FN volatile binary format=BF "
			"u O w l t sprintf('%%d',O)\n",
			dt, G.no, ylims);

		PROC_OPEN();
		dprintf(1, "set print '%s'; print 'ACK'; set print\n", chan);
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
		dprintf(1, "/tmp/ir.data %d %ld %d %d %s\n",
				norm, sizeof(FLOAT), G.no, G.sr, chan);
		PROC_CLOSE("/tmp/ir.data");
	}
} __o_ir;

struct O_SOX : public O_B {
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

static struct O_N *O = &__o_t;

// ----------------------------------------------------------------------------
#define NARG	32
static struct {
	const char *n;
	FAUSTFLOAT __v, *v;
} 		ARGV[NARG];
static int	ARGN;

__typeof__(ARGV+0) cli_get_opt(const char *n)
{
	for (int i = 0; i < ARGN; ++i)
		if (!strcmp(n,  ARGV[i].n))
			return &ARGV[i];
	return NULL;
}

static auto __add_opt(const char *n, FAUSTFLOAT v, bool f)
{
	auto o = cli_get_opt(n);
	if (!o) {
		assert(ARGN < NARG);
		o = ARGV + ARGN++;
		o->n = n;
		f = true;
	}

	if (f) o->__v = v;
	return o;
}

static void ui_add_opt(const char *n, FAUSTFLOAT *e, FAUSTFLOAT v)
{
	auto o = __add_opt(n, v, false);
	assert(o->v == NULL);
	*(o->v = e) = o->__v;
}
static void ui_ck_opts(void)
{
	const char *w = "WARN! unused opts:";
	for (int i = 0; i < ARGN; ++i) {
		auto o = ARGV + i;
		if (o->v) continue;

		if (w) { fputs(w, stderr); w = NULL; }
		fprintf(stderr, " %s", o->n);

		for (int d = 0; d < ARGN-i-1; ++d) o[d] = o[d+1];
		ARGN--; i--;
	}
	if (!w) fputs("\n", stderr);
}

static void cli_add_opt(char *n, char *p)
{
	*p++ = 0;
	char *e; FAUSTFLOAT v = strtod(p, &e);
	if (e == p || *e) die("bad number: '%s'", p);
	__add_opt(n, v, true);
}

// ----------------------------------------------------------------------------
#define	IF(a)	if (!strcmp(n, #a))

static void parse_o(char *n)
{
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

	if (p && (!*p || !O->cli(p)))
		die("bad arg '%s' for -o %s", p, n);
}

static void parse_g(const char *p)
{
	if (p) for (char *e;; p = e + 1) {
		assert(GN < NOUTS);
		GV[GN].g = strtod(p, &e);
		if (e != p) {
			if (*e == '+' || *e == '-')
				GV[GN].s = strtod(p = e, &e);
			GN++;
			if (*e == ',') continue;
			if (*e == '\0') return;
		}
		break;
	}
	die("bad number: '%s'", p);
}

static void parse_G(const char *n, const char *v)
{
	const  char *o = n;
	int x; char *e = NULL;

	if (v) x = strtoll(v, &e, 0);
	if (e == v || *e)
		die("bad number: '%s'", v);

	if (*o++ == '-') for (;;)
		switch (*o++) {
		case   0: return;
		case 'r': G.sr = x; break;
		case 'n': G.nr = x; break;
		case 's': G.sk = x; break;
		case 'x': G.xt = x; break;
		case 'b': G.bs = x; break;
		default: goto err;
		};

err:	die("bad option '%s'", n);
}

static void parse_args(char* argv[])
{
	for (char *n; (n = *++argv);) {
		if (char *p = strchr(n, '='))
			cli_add_opt(n, p);
		else IF (-i)
			G.it = 1;
		else IF (-o)
			parse_o(*++argv);
		else IF (-g)
			parse_g(*++argv);
		else
			parse_G(n, *++argv);
	}
}

// ----------------------------------------------------------------------------
static char *__it_getline(void)
{
	static char *line = NULL;
	static size_t size = 0;

	fprintf(stderr, ": ");
	return getline(&line, &size, stdin) >= 0 ? line : NULL;
}

#include <readline/readline.h>
#include <readline/history.h>
static char *rl_next_match(const char *inp, int state)
{
	static int idx, len;

	if (!state) { idx = 0; len = strlen(inp); }

	while (idx < ARGN) {
		auto arg = ARGV + idx++;
		if (!strncmp(arg->n, inp, len))
			return strdup(arg->n);
	}

	return NULL;
}
static char **rl_cmpl_func(const char *inp, int start, int end)
{
	rl_attempted_completion_over = 1;
	return rl_completion_matches(inp, rl_next_match);
}
static char *__it_readline(void)
{
	static char *inp;

	if (inp) free(inp);
	else {
		assert((rl_instream  = fopen("/dev/tty", "r")));
		assert((rl_outstream = fopen("/dev/tty", "w")));
		rl_attempted_completion_function = rl_cmpl_func;
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

static char *map(const char *k, const char *v)
{
	static struct { char *k, *v; } map[128];
	__typeof__(map + 0) kv = NULL;

	for (unsigned i = 0; i < sizeof(map)/sizeof(map[0]); ++i)
		if (!map[i].k || !strcmp(map[i].k, k)) {
			kv = map + i;
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

void *it_loop(void *)
{
	for (;;) {
		char *inp;
		char *p, c;  int eat;
		char n[128]; float v;

		if(!(inp = it_readline())) _exit(0);
		if ((p = strchr(inp, '\n'))) *p = 0;
		if ((p = strchr(inp,  '#'))) *p = 0;
		if (!*inp) goto dump;

		if (sscanf(inp, " %127[^=: ] %[:] %n", n,&c,&eat) == 2) {
			char *v = inp + eat;
			if (!*v) {
				static char dump[1024] = ""; // for ARGN == 0
				int sz = sizeof(dump), wr = 0;
				for (int i = 0; i < ARGN; ++i) {
					int w = snprintf(dump+wr,sz,"%s=%.16g ",
						ARGV[i].n,  double(*ARGV[i].v));
					wr += w; sz -= w;
					if (sz <= 0)
						die("dump[] overflow.\n");
				}
				v = dump;
			}

			if (!map(n, v))
				fprintf(stderr, "ERR!! map is full.\n");
			continue;
		}
		if (sscanf(inp, " %127[^=: ] %c", n,&c) == 1) {
			if (!(inp = map(n, NULL))) {
				fprintf(stderr, "ERR!! '%s' wasn't defined.\n", n);
				continue;
			}
		}

		for (; *inp; inp += eat) {
			if (sscanf(inp, " %127[^= ] %*[=] %f %n", n,&v,&eat) != 2)
				goto dump;
			auto o = cli_get_opt(n);
			if (!o)
				goto dump;
			*o->v = v;
		}
		continue;

dump:		fprintf(stderr, "\n");
		for (int i = 0; i < ARGN; ++i)
			fprintf(stderr, "  %-16s % -.8g\n",
				ARGV[i].n, double(*ARGV[i].v));
		fprintf(stderr, "\n");
	}
}

int main(int argc, char* argv[])
{
	#ifdef CLI_INIT
	CLI_INIT
	#endif
	parse_args(argv);

	if (DSP.getNumInputs() > 0)
		die("no inputs allowed");

	G.no = DSP.getNumOutputs();
	assert(G.no <= NOUTS);
	assert(G.bs <= BUFSZ);
	if (GN) check_g();

	DSP.init(G.sr);
	DSP.buildUserInterface((UI*)0);
	ui_ck_opts();

	if (G.it) {
		pthread_t t;
		pthread_create(&t, NULL, it_loop, NULL);
	}

	FAUSTFLOAT *outputs[NOUTS];
	for (int o = 0; o < NOUTS; o++)
		outputs[o] = _outputs[o];

	O->ini();
	if (G.nr <= G.nr + G.sk) G.nr += G.sk; // avoid overflow
	for (unsigned count, stopped = 0, nr = G.nr; nr; nr -= count) {
		count = G.bs;
		if (count > nr) count = nr;

		DSP.compute(count, 0, outputs);
		if (cli_stop >= 0 && !stopped) {
			nr = cli_stop + G.xt;
			if (count > nr) count = nr;
			stopped = 1;
		}

		for (unsigned i = 0; i < count; i++) {
			if (G.sk) G.sk--;
			else {
				if (GN) apply_g(i);
				O->out(i);
			}
		}
		O->eob();
	}
	O->eof();

	return 0;
}
