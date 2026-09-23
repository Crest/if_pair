/* Single waiter in dispatch_group_wait(): reaches _dispatch_wait_on_address()
 * with no lock-owner comparison involved. */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <unistd.h>

static void slow(void *ctx) { (void)ctx; sleep(1); }

int
main(void)
{
	dispatch_group_t g = dispatch_group_create();
	dispatch_group_async_f(g, dispatch_get_global_queue(0, 0), NULL, slow);
	long r = dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
	printf("group wait returned %ld\n", r);
	return (0);
}
