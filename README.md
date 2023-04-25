# fpp

fpp is a standalone Perl script with no dependencies which allows ANY
C/C++ code in .dsp as long as you are targeting C/C++ in scalar mode.

# How to use

Just change your Makefiles / scripts to run fpp instead of faust, fpp
will call faust internally.

See also INSTALL.txt

# Pointless but hopefully illustrative example

$ cat /tmp/test.dsp

```faust
// suboptimal re-implementation of the 'mem' primitive.
z = FPP(input)
{
// declare the state variable.
// '$' at the start of the name ensures that each instance
// gets its own variable.
decl: float $z;

// initialize the state variable in instanceConstants()
init: $z = 0;

// this goes to compute()
exec:
	float ret = $z;
	// another use-case for '$' at the start of name,
	// access the value of the `input` argument.
	$z = $input;
	ret;
};

// similar to process = @(2),@(2);
process = par(i,2, z:z);
```

$ fpp -s /tmp/test.dsp

```c
float z__01;
float z__02;
float z__03;
float z__04;
int fSampleRate;

void instanceConstants(int sample_rate)
{
	fSampleRate = sample_rate;
	z__01 = 0;
	z__02 = 0;
	z__03 = 0;
	z__04 = 0;
}

void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs)
{
	FAUSTFLOAT* input0 = inputs[0];
	FAUSTFLOAT* input1 = inputs[1];
	FAUSTFLOAT* output0 = outputs[0];
	FAUSTFLOAT* output1 = outputs[1];
	for (int i0 = 0; i0 < count; i0 = i0 + 1) {
		output0[i0] = FAUSTFLOAT(({ float ret = z__02; z__02 = ({ float ret = z__01; z__01 = input0[i0]; ret; }); ret; }));
		output1[i0] = FAUSTFLOAT(({ float ret = z__04; z__04 = ({ float ret = z__03; z__03 = input1[i0]; ret; }); ret; }));
	}
}
```

# More realistic example

Suppose we have a 'foo' library with the following API:

```c
// must be called only once
void foo_init_lib(sample_rate);

struct foo_state { ... };

void foo_init_state(struct foo_state *, float fc0);

float foo_compute(struct foo_state *, float inp);
```

To simplify, let's assume that this filter can't be modulated and therefore
**fc0** should be a constant known at init time (e.g. ma.SR/4).

How can we use this lib in faust? My best attempt:

## 1) create faust_foo.h

```c
#include <foo.h>

class faust_foo : public dsp {
	foo_state state;

	float faust_foo_init(int sr, float fc0)
	{
		foo_init_lib(sr);
		foo_init_state(&state, fc0);
		return 0; // unused
	}

	float faust_foo_compute(float inp)
	{
		return foo_compute(&state, inp);
	}
};
```

## 2) create foo.dsp, for example

```faust
import("stdfaust.lib");

foo(fc0, inp) = foo_compute(inp) : attach(foo_init(ma.SR, fc0))
with {
	foo_init = ffunction(float faust_foo_init(int, float), "faust_foo.h","");
	foo_compute = ffunction(float faust_foo_compute(float), "faust_foo.h","");
};

process = foo(ma.SR/4);
```
## 3) compile foo.dsp with the "-scn foo" faust option

Solved? Not really:

- This is cumbersome and error prone.

- faust thinks that **foo()** is a pure function, so for example
  ```faust
  process = 1 : foo(ma.SR/4); // step response
  ```
  won't work.

- But the main problem is that **foo()** can be used only once in .dsp, e.g.
  ```faust
  process = foo(10),foo(20);
  ```
  will be happily compiled and output garbage.

## Now let's use **fpp**

this .dsp code

```faust
import("stdfaust.lib");

// can be used just like any other faust function
foo = FPP(fc0, inp) {
INIT:	foo_init_lib($(ma.SR));
decl:	struct foo_state $state;
init:	foo_init_state(&$state, $fc0);
exec:	foo_compute(&$state, $inp);
};

process = foo(10),foo(20);
```

compiles to

```c
struct foo_state state__01;
struct foo_state state__02;
int fSampleRate;

void instanceConstants(int sample_rate)
{
	fSampleRate = sample_rate;
	float fConst0 = std::min<float>(1.92e+05f, std::max<float>(1.0f, float(fSampleRate)));
	foo_init_lib(fConst0);
	foo_init_state(&state__01, 10);
	foo_init_state(&state__02, 20);
}

void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs)
{
	FAUSTFLOAT* input0 = inputs[0];
	FAUSTFLOAT* input1 = inputs[1];
	FAUSTFLOAT* output0 = outputs[0];
	FAUSTFLOAT* output1 = outputs[1];
	for (int i0 = 0; i0 < count; i0 = i0 + 1) {
		output0[i0] = FAUSTFLOAT(({ foo_compute(&state__01, input0[i0]); }));
		output1[i0] = FAUSTFLOAT(({ foo_compute(&state__02, input1[i0]); }));
	}
}
```

and this is what we need.
