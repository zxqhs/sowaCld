#ifndef SC_PLATFORM_H
#define SC_PLATFORM_H

#if defined(_WIN32) || defined(_WIN64)
#  ifndef SC_WINDOWS
#    define SC_WINDOWS 1
#  endif
#else
#  ifndef SC_LINUX
#    define SC_LINUX 1
#  endif
#endif

#endif /* SC_PLATFORM_H */
