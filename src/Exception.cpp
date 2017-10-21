#include "Exception.h"
#include <stdio.h>
#include <stdarg.h>

#ifndef NDEBUG
void print_debug(char const* fmt, ...) {
	va_list vp;
	va_start(vp, fmt);
	vfprintf(stdout, fmt, vp);
	va_end(vp);
}
#endif

void print_error(char const* fmt, ...) {
	va_list vp;
	va_start(vp, fmt);
	vfprintf(stderr, fmt, vp);
	va_end(vp);
}
