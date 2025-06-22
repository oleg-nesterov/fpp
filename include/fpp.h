#ifndef FPP_H
#define FPP_H

template<class T> static inline int signum(T x)
{
	return (x > 0) - (x < 0);
}

#endif
