/*
 * Main-queue run-loop handle on FreeBSD.  The port builds queue.c's __unix__
 * variant, which packs the wake-up pipe as (rfd << 32) | wfd into a 64-bit
 * handle, but its private.h declares dispatch_runloop_handle_t as int for
 * FreeBSD, so the value is truncated to the WRITE end on the way out (and
 * the read end is shifted with a count >= the type width on dispose).
 *
 * This program calls the 4CF entry point as returning uint64_t.  With the
 * bug, the library returns an int; on amd64/arm64 a 32-bit return zero-
 * extends, so the packed read end reads back as 0.  With the fix, both
 * halves are present and an async to the main queue makes the read end
 * readable.
 */
#include <dispatch/dispatch.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t _dispatch_get_main_queue_handle_4CF(void);
static void noop(void *ctx) { (void)ctx; }

int
main(void)
{
	uint64_t h = _dispatch_get_main_queue_handle_4CF();
	int rfd = (int)(h >> 32), wfd = (int)(h & 0xffffffff);

	printf("handle 0x%llx: read end %d, write end %d\n",
	    (unsigned long long)h, rfd, wfd);
	if (rfd == 0) {
		puts("read end is 0: the handle was truncated to the write end (WRONG)");
		return (1);
	}
	dispatch_async_f(dispatch_get_main_queue(), NULL, noop);
	struct pollfd pfd = { .fd = rfd, .events = POLLIN };
	int r = poll(&pfd, 1, 1000);
	if (r > 0 && (pfd.revents & POLLIN)) {
		puts("read end became readable after dispatch_async: OK");
		return (0);
	}
	puts("read end never became readable (WRONG)");
	return (1);
}
