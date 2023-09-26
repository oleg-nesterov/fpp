#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <assert.h>

#define FAUSTFLOAT double
static void ui_add_opt(const char *, FAUSTFLOAT*, FAUSTFLOAT);

struct Meta {
	void declare(const char *k, const char *v);
};
struct UI {
	inline void openVerticalBox(const char *n) {}
	inline void closeBox(void) {}

	inline void addNumEntry(const char *n, FAUSTFLOAT *e, FAUSTFLOAT v, ...)
	{
		ui_add_opt(n, e, v);
	}
};

<<includeIntrinsic>>
<<includeclass>>

static mydsp DSP;

#define NOUTS	16
#define BUFSZ	1024
static FAUSTFLOAT _outputs[NOUTS][BUFSZ];

static struct {
	unsigned sr = 44100, nr = 10, bs = 512, sk;
	unsigned no;
	int it;
} G;

#define die(fmt, ...) do {					\
	fprintf(stderr, "ERR!! " fmt "\n", ##__VA_ARGS__);	\
	exit(1);						\
} while (0)

// ----------------------------------------------------------------------------
struct _O {
	virtual void ini(void) {}
	virtual void out(unsigned) = 0;
	virtual void eob(void) {}
	virtual void eof(void) {}
};

struct O_T : public _O {
	void out(unsigned i)
	{
		for (unsigned o = 0; o < G.no; o++) {
			printf("%#-+18.12g%c", _outputs[o][i],
				o == G.no-1 ? '\n' : '\t');
		}
	}
} __o_t;

struct O_B : public _O {
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
} __o_b;

struct _O_SOX : public O_B {
	int pid;

	void eof(void)
	{
		close(ofd); waitpid(pid, NULL, 0);
	}

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
};

struct O_P : public _O_SOX {
	void ini() { __ini(NULL); }
} __o_p;
struct O_F : public _O_SOX {
	void ini() { __ini("-"); }
} __o_f;

struct _O *O = &__o_t;

#define	IF(a)	if (!strcmp(n, #a))

static void parse_o(const char *n)
{
	IF (t)
		O = &__o_t;
	else IF (b)
		O = &__o_b;
	else IF (p)
		O = &__o_p;
	else IF (f)
		O = &__o_f;
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
		else
			die("bad option '%s'", n);
		continue;
 set_u:
		*p_u = atoi(v);
	}
}

void *it_loop(void *)
{
	goto dump; for (;;) {
		char *inp; size_t sz;
		char n[128]; float v;

		printf(": ");
		getline(&inp, &sz, stdin);

		if (sscanf(inp, "%s %f", n, &v) == 2) {
			if (auto o = cli_get_opt(n))
				{ *o->v = v; continue; }
		}
 dump:
		printf("\n");
		for (int i = 0; i < ARGN; ++i)
			printf("  %-8s %f\n", ARGV[i].n, *ARGV[i].v);
		printf("\n");
	}
}

int main(int argc, const char* argv[])
{
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
	for (unsigned nr = G.nr + G.sk; nr; ) {
		unsigned count = G.bs;
		if (count > nr) count = nr;
		nr -= count;

		DSP.compute(count, 0, outputs);
		for (unsigned i = 0; i < count; i++) {
			if (G.sk) G.sk--;
			else   O->out(i);
		}
		O->eob();
	}
	O->eof();

	return 0;
}
