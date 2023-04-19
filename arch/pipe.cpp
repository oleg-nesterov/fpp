#include <stdio.h>
#include <unistd.h>
#include <assert.h>

// FPP(n) { FILE: extern float pipe_args[]; exec: (pipe_args[$n]) };
float pipe_args[16];

// compile with 'faust -scn "" -nvi', otherwise need to provide the
// "empty" class dsp and openVerticalBox/closeBox definitions.
struct Meta {
	void declare(const char *k, const char *v);
};
struct UI {
	void openVerticalBox(const char *n);
	void closeBox(void);
};

<<includeIntrinsic>>
<<includeclass>>

static mydsp DSP;
static FAUSTFLOAT ibuf[16384], obuf[16384];

int main(int argc, char *argv[])
{
	DSP.init(argv[1] ? atoi(argv[1]) : 44100);
	for (int i = 0; i < 16; ++i) {
		const char *arg = argv[i+2];
		if (arg)
			pipe_args[i] = atof(arg);
		else
			break;
	}

	bool ck_ios = DSP.getNumInputs() == 1 && DSP.getNumOutputs() == 1;
	if (!ck_ios)
		fprintf(stderr, "ERR!! inp/out != 1\n");

	FAUSTFLOAT *inp = ibuf, *out = obuf;
	for (;;) {
		int r = read(0, ibuf, sizeof(ibuf));
		if (r <= 0) {
			if (r < 0) fprintf(stderr, "ERR!! read failed: %m\n");
			break;
		}

		assert(r % sizeof(FAUSTFLOAT) == 0);
		if (ck_ios)
			DSP.compute(r / sizeof(FAUSTFLOAT), &inp, &out);
		assert(write(1, obuf, r) == r);
	}

	return 0;
}
