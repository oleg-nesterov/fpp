#ifndef FPP_H
#define FPP_H

#include <stdio.h>
#include <stdlib.h>

#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

template<class T> static inline int signum(T x)
{
	return (x > 0) - (x < 0);
}

//------------------------------------------------------------------------------
#define LOG(fmt, ...) fprintf(stderr, fmt "\n", __VA_ARGS__)

#define __DEBUG(condition, fatal, ...) ({					\
	int __ret_warn_on = !!(condition);					\
	if (unlikely(__ret_warn_on)) {						\
		fprintf(stderr, fatal ? "BUG!! %d: " : "WARN! %d: ", __LINE__);	\
		fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");		\
		if (fatal)							\
			exit(1);						\
	}									\
	unlikely(__ret_warn_on);						\
})

#define WARN(condition, ...)	__DEBUG((condition), 0, __VA_ARGS__)
#define WARN_ON(condition)	__DEBUG((condition), 0, # condition)

#define BUG(condition, ...)	__DEBUG((condition), 1, __VA_ARGS__)
#define BUG_ON(condition)	__DEBUG((condition), 1, # condition)

#endif
