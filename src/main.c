#define WLR_USE_UNSTABLE

#include "server.h"

#include <stdlib.h>
#include <wlr/util/log.h>

int main(void)
{
	wlr_log_init(WLR_DEBUG, NULL);

	struct hsdwl_server server = {0};
	if (!hsdwl_server_init(&server))
	{
		hsdwl_server_destroy(&server);
		return 1;
	}

	int ret = hsdwl_server_run(&server);
	hsdwl_server_destroy(&server);
	return ret;
}
