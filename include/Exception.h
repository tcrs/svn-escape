#ifndef EXCEPTION_H__
#define EXCEPTION_H__

#include <stdarg.h>
#include <stdio.h>
#include <exception>
#include <string>

/**
 * To use these macros call with double brackets; e.g
 * DEBUG(("A three headed monkey was not found in the %s", location));
 * This avoids having to use variadic macros, which are not supported in C++
 */

/* Used to convert __LINE__ to a string */
#define QUOTE2(x) #x
#define QUOTE(x) QUOTE2(x)

void print_error(char const*, ...);

#define ERROR(x)	do {\
				fprintf(stderr, "** ERROR[" __FILE__ ":" QUOTE(__LINE__) "] "); \
				print_error x; fprintf(stderr, "\n"); \
			} while(0)

#define WARN(x)		do {\
				fprintf(stderr, "WARNING[" __FILE__ ":" QUOTE(__LINE__) "] "); \
				print_error x; fprintf(stderr, "\n"); \
			} while(0)

#ifndef NDEBUG
void print_debug(char const*, ...);
#	define DEBUG(x)	do {\
				fprintf(stdout, "DEBUG[" __FILE__ ":" QUOTE(__LINE__) "] "); \
				print_debug x; fprintf(stdout, "\n"); \
			} while(0)
#else
#	define DEBUG(x)		do {} while(0)
#endif

#define MAX_EXCEPTION_LEN 1024

class Exception : public std::exception {
protected:
	std::string msg;
public:
	virtual ~Exception() throw() { }

	Exception(std::string const& m) : msg(m) {
	}
	void append(std::string const& extra) {
		msg.append(extra);
	}

	char const* what() const throw() {
		return msg.c_str();
	}
};

class ExceptionBuilder {
protected:
	Exception ex;
public:
	ExceptionBuilder(std::string const& pre) : ex(pre) { }
	Exception operator()(char const* fmt, ...) {
		char buf[MAX_EXCEPTION_LEN];
		va_list vp;
		va_start(vp, fmt);
		vsnprintf(buf, MAX_EXCEPTION_LEN, fmt, vp);
		va_end(vp);
		ex.append(buf);
		return ex;
	}
};

#define EXCEPTION(x)		ExceptionBuilder("[" __FILE__ ":" QUOTE(__LINE__) "] ") x

#endif
