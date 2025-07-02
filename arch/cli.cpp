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

#define FAUSTFLOAT double

static struct {
	unsigned sr = 44100, nr = 10, bs = 512, sk, xt;
	unsigned no;
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

#define die(fmt, ...) do {					\
	fprintf(stderr, "ERR!! " fmt "\n", ##__VA_ARGS__);	\
	exit(1);						\
} while (0)

// ----------------------------------------------------------------------------
static void fifo_run(const char *fifo, const char *comm, const char *argv[])
{
	struct stat st;
	int fd;

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

	start: fprintf(stderr, "CLI: starting '%s' ...\n", comm);

	if (!fork()) {
		if (!fork()) {
			fd = open(fifo, O_RDONLY);
			assert(fd > 0);
			assert(dup2(fd, 0) == 0);
			close(fd);
			execvp(comm, (char**)argv);
			kill(getppid(), SIGKILL);
			die("exec '%s' failed: %m", comm);
		}

		fd = open(fifo, O_WRONLY);
		assert(fd > 0);
		wait(NULL);
		exit(0);
	}

	fd = open(fifo, O_WRONLY);
	assert(fd > 0);
out:
	assert(dup2(fd, 1) == 1);
	close(fd);
}

static struct O_N {
	virtual void ini(void) {}
	virtual void out(unsigned) {};
	virtual void eob(void) {}
	virtual void eof(void) {}
} __o_n;

static struct O_T : public O_N {
	void out(unsigned i)
	{
		for (unsigned o = 0; o < G.no; o++) {
			printf("%#-+18.12g%c", _outputs[o][i],
				o == G.no-1 ? '\n' : '\t');
		}
	}
	void eof()
	{
		fclose(stdout);
	}
} __o_t;

static struct O_B : public O_N {
	float buf[NOUTS * BUFSZ];
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
	void ini(void)
	{
		const char *argv[] = { "CLI-gnuplot", NULL };
		fifo_run("/tmp/gp.fifo", "gnuplot", argv);

		ofd = open("/tmp/gp.data", O_CREAT|O_TRUNC|O_WRONLY, 0666);
		assert(ofd >= 0);
	}

	void eof(void)
	{
		dprintf(1, "NO = %d; FN = '%s'\n%s", G.no, "/tmp/gp.data",
			"BF = ''; do for [O=1:NO] { BF = BF . '%float' }\n"
			"plot for [O=1:NO] FN volatile binary format=BF "
			"u O w l t sprintf('%d',O)\n"
		);

		int pgp[2]; char ack[32] = {};

		assert(!pipe(pgp));
		dprintf(1, "set print '/proc/%d/fd/%d'; "
			   "print 'ACK'; set print\n", getpid(), pgp[1]);
		read(pgp[0], ack, sizeof(ack) - 1);
		close(pgp[0]); close(pgp[1]);

		if (strcmp(ack, "ACK\n"))
			die("bad ack from gnuplot: %s", ack);
		unlink("/tmp/gp.data");
	}
} __o_gp;

struct _O_SOX : public O_B {
	int pid;

	void __ini(const char *file)
	{
		int fds[2];
		assert(!pipe(fds));

		if ((pid = fork())) {
			ofd = fds[1];
			close(fds[0]);
		} else {
			close(fds[1]);
			assert(dup2(fds[0], 0) == 0);

			char o_r[64], o_c[64];
			sprintf(o_r, "-r%d", G.sr);
			sprintf(o_c, "-c%d", G.no);

			const char *argv[] = {
				file ? "sox" : "play",
				"-q", "-traw", "-ef", "-b32",
				o_r, o_c,
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
};

static struct O_P : public _O_SOX {
	void ini() { __ini(NULL); }
} __o_p;
static struct O_F : public _O_SOX {
	void ini() { __ini("-"); }
} __o_f;

static struct O_N *O = &__o_t;

#define	IF(a)	if (!strcmp(n, #a))

static void parse_o(const char *n)
{
	IF (t)
		O = &__o_t;
	else IF (n)
		O = &__o_n;
	else IF (b)
		O = &__o_b;
	else IF (p)
		O = &__o_p;
	else IF (f)
		O = &__o_f;
	else IF (gp)
		O = &__o_gp;
	else
		die("bad -o value '%s'", n);
}

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

static void cli_add_opt(const char *n, const char *p)
{
	*(char *)p = 0; __add_opt(n, atof(p+1), true);
}

// ----------------------------------------------------------------------------
static void parse_args(const char* argv[])
{
	for (;;) {
		const char *n, *v;
		unsigned *p_u;

		if (!(n = *++argv))
			break;
		else IF (-i)
			G.it = 1;
		else if (const char *p = strchr(n, '='))
			cli_add_opt(n, p);
		else if (!(v = *++argv))
			die("no value for '%s'", n);
		else IF (-o)
			parse_o(v);
		else IF (-r)
			{ p_u = &G.sr; goto set_u; }
		else IF (-n)
			{ p_u = &G.nr; goto set_u; }
		else IF (-s)
			{ p_u = &G.sk; goto set_u; }
		else IF (-b)
			{ p_u = &G.bs; goto set_u; }
		else IF (-x)
			{ p_u = &G.xt; goto set_u; }
		else
			die("bad option '%s'", n);
		continue;
 set_u:
		*p_u = atoi(v);
	}
}

// ----------------------------------------------------------------------------
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
	char *line = NULL;
	size_t size = 0;

	for (;;) {
		char *inp;
		char *p, c;  int eat;
		char n[128]; float v;

		fprintf(stderr, ": ");
		if (getline(&line, &size, stdin) < 0)
			exit(0);

		inp = line;
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
							 ARGV[i].n, *ARGV[i].v);
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
			fprintf(stderr, "  %-16s % -.8g\n", ARGV[i].n, *ARGV[i].v);
		fprintf(stderr, "\n");
	}
}

int main(int argc, const char* argv[])
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
			else   O->out(i);
		}
		O->eob();
	}
	O->eof();

	return 0;
}
