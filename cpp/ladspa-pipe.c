#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ladspa.h>
#include <assert.h>

struct plug {
	int ofd, ifd;
};

static void pipe_start(struct plug *plug, int rate, int narg, float *arg)
{
	int pid, ofd[2], ifd[2];

	assert(!pipe(ofd) && !pipe(ifd));
	plug->ofd = ofd[1];
	plug->ifd = ifd[0];

	if ((pid = fork())) {
		close(ofd[0]);
		close(ifd[1]);
		assert(pid > 0);
		assert(waitpid(pid, 0, 0) == pid);
	} else {
		close(ofd[1]);
		close(ifd[0]);
		assert(dup2(ofd[0], 0) == 0);
		assert(dup2(ifd[1], 1) == 1);
		for (int fd = 3; fd < 1024; ++fd)
			close(fd);

		if (!fork()) {
			const char *path = "/tmp/test-pipe";
			char *argv[narg+3];

			argv[0] = "pipe";
			asprintf(&argv[1], "%d", rate);
			for (int i = 0; i < narg; ++i)
				asprintf(&argv[i+2], "%f", arg[i]);
			argv[narg+2] = NULL;

			execv(path, argv);
			fprintf(stderr, "\nERR!! exec '%s' failed: %m\n", path);
			execlp("cat", "cat", NULL);
			fprintf(stderr, "ERR!! exec cat failed! %m\n");
		}
		_exit(0);
	}
}

static inline void pipe_stop(struct plug *plug)
{
	close(plug->ofd);
	close(plug->ifd);
}

static inline void pipe_run(struct plug *plug, const void *ibuf, void *obuf, int count)
{
	for (int c; count; count -= c, ibuf += c, obuf += c) {
		c = count > 4096 ? 4096 : count;
		// FIXME. Happens to work but obviously wrong. I am lazy.
		assert(write(plug->ofd, ibuf, c) == c);
		assert(read(plug->ifd, obuf, c) == c);
	}
}

// ----------------------------------------------------------------------------
#define NR_CTL	2

#define AMP_INPUT1	0
#define AMP_OUTPUT1	1
#define AMP_CONTR0	2
#define NR_PORTS	(AMP_CONTR0 + NR_CTL)

struct test_struct {
	struct plug	plug;
	int		rate, init;
	LADSPA_Data	*ports[NR_PORTS];
};

static LADSPA_Handle instantiate(const LADSPA_Descriptor *desc, unsigned long rate)
{
	struct test_struct *test = calloc(1, sizeof(struct test_struct));
	test->rate = rate;
	return test;
}

static void cleanup(LADSPA_Handle inst)
{
	free(inst);
}

static void run(LADSPA_Handle inst, unsigned long count)
{
	struct test_struct *test = inst;

	if (!test->init) {
		float arg[NR_CTL];
		for (int i = 0; i < NR_CTL; ++i)
			arg[i] = *test->ports[AMP_CONTR0 + i];
		pipe_start(&test->plug, test->rate, NR_CTL, arg);
		test->init = 1;
	}

	pipe_run(&test->plug,
		test->ports[AMP_INPUT1], test->ports[AMP_OUTPUT1],
		count * sizeof(LADSPA_Data));
}

static void deactivate(LADSPA_Handle inst)
{
	struct test_struct *test = inst;

	if (test->init) {
		pipe_stop(&test->plug);
		test->init = 0;
	}
}

#define AMP_CTR_RANGE	AMP_CONTR0 ... (AMP_CONTR0 + NR_CTL - 1)

static void connect_port(LADSPA_Handle inst, unsigned long port, LADSPA_Data *data)
{
	struct test_struct *test = inst;

	if (port < NR_PORTS)
		test->ports[port] = data;
	else
		fprintf(stderr, "ERR!! connect_port: bad port=%ld\n", port);
}

static const LADSPA_PortDescriptor port_desc[] = {
	[AMP_INPUT1]	= LADSPA_PORT_INPUT  | LADSPA_PORT_AUDIO,
	[AMP_OUTPUT1]	= LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
	[AMP_CTR_RANGE]	= LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
};
static const char *const port_name[] = {
	[AMP_INPUT1]	= "input",
	[AMP_OUTPUT1]	= "output",

	[AMP_CONTR0 + 0]	= "arg0",
	[AMP_CONTR0 + 1]	= "arg1",
};
static const LADSPA_PortRangeHint port_hint[] = {
	[AMP_INPUT1]	= {},
	[AMP_OUTPUT1]	= {},
	[AMP_CTR_RANGE]	= {
		.HintDescriptor	=
				LADSPA_HINT_BOUNDED_BELOW |
				LADSPA_HINT_BOUNDED_ABOVE |
				LADSPA_HINT_DEFAULT_MIDDLE,
		.LowerBound	= -1000,
		.UpperBound	= +1000,
	},
};

static const LADSPA_Descriptor desc = {
	.Label		= "test",
	.Name		= "test plugin",
	.Maker		= "Oleg Nesterov",
	.Copyright	= "",

	.PortCount		= sizeof(port_desc) / sizeof(port_desc[0]),
	.PortDescriptors	= port_desc,
	.PortNames		= port_name,
	.PortRangeHints		= port_hint,

	.instantiate		= instantiate,
	.cleanup		= cleanup,
	.connect_port		= connect_port,
	.run			= run,
	.deactivate		= deactivate,
};

const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
	switch (index) {
	case 0:
		return &desc;
	default:
		return NULL;
	}
}
