/* Hand-written libusb config.h for the MPC build (armhf Linux, glibc 2.31,
 * no udev: hotplug uses netlink). Mirrors what libusb's configure.ac
 * defines on Linux, so its configure script doesn't have to run. */
#define _GNU_SOURCE 1
#define DEFAULT_VISIBILITY __attribute__ ((visibility ("default")))
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))
#define PLATFORM_POSIX 1
#define ENABLE_LOGGING 1
#define HAVE_ASM_TYPES_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_EVENTFD 1
#define HAVE_TIMERFD 1
#define HAVE_NFDS_T 1
#define HAVE_PIPE2 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#define HAVE_PTHREAD_SETNAME_NP 1
#define HAVE_SYS_TIME_H 1
