/*
 * Copyright (C) 2013 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <urcu/list.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
#include <urcu/compiler.h>
#include <ulimit.h>
#include <inttypes.h>

#include <common/defaults.hpp>
#include <common/common.hpp>
#include <common/consumer/consumer.hpp>
#include <common/consumer/consumer-timer.hpp>
#include <common/compat/poll.hpp>
#include <common/sessiond-comm/sessiond-comm.hpp>
#include <common/utils.hpp>

#include "lttng-consumerd.hpp"
#include "health-consumerd.hpp"

/* Global health check unix path */
static char health_unix_sock_path[PATH_MAX];

int health_quit_pipe[2] = {-1, -1};

/*
 * Send data on a unix socket using the liblttsessiondcomm API.
 *
 * Return lttcomm error code.
 */
static int send_unix_sock(int sock, void *buf, size_t len)
{
	/* Check valid length */
	if (len == 0) {
		return -1;
	}

	return lttcomm_send_unix_sock(sock, buf, len);
}

static
int setup_health_path(void)
{
	int is_root, ret = 0;
	enum lttng_consumer_type type;
	const char *home_path;

	type = lttng_consumer_get_type();
	is_root = !getuid();

	if (is_root) {
		if (strlen(health_unix_sock_path) != 0) {
			goto end;
		}
		switch (type) {
		case LTTNG_CONSUMER_KERNEL:
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
				DEFAULT_GLOBAL_KCONSUMER_HEALTH_UNIX_SOCK);
			break;
		case LTTNG_CONSUMER64_UST:
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
				DEFAULT_GLOBAL_USTCONSUMER64_HEALTH_UNIX_SOCK);
			break;
		case LTTNG_CONSUMER32_UST:
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
				DEFAULT_GLOBAL_USTCONSUMER32_HEALTH_UNIX_SOCK);
			break;
		default:
			ret = -EINVAL;
			goto end;
		}
	} else {
		home_path = utils_get_home_dir();
		if (home_path == NULL) {
			/* TODO: Add --socket PATH option */
			ERR("Can't get HOME directory for sockets creation.");
			ret = -EPERM;
			goto end;
		}

		/* Set health check Unix path */
		if (strlen(health_unix_sock_path) != 0) {
			goto end;
		}
		switch (type) {
		case LTTNG_CONSUMER_KERNEL:
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
				DEFAULT_HOME_KCONSUMER_HEALTH_UNIX_SOCK, home_path);
			break;
		case LTTNG_CONSUMER64_UST:
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
				DEFAULT_HOME_USTCONSUMER64_HEALTH_UNIX_SOCK, home_path);
			break;
		case LTTNG_CONSUMER32_UST:
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
				DEFAULT_HOME_USTCONSUMER32_HEALTH_UNIX_SOCK, home_path);
			break;
		default:
			ret = -EINVAL;
			goto end;
		}
	}
end:
	return ret;
}

/*
 * Thread managing health check socket.
 */
void *thread_manage_health_consumerd(void *data __attribute__((unused)))
{
	int sock = -1, new_sock = -1, ret, i, err = -1;
	uint32_t nb_fd;
	struct lttng_poll_event events;
	struct health_comm_msg msg;
	struct health_comm_reply reply;
	int is_root;

	DBG("[thread] Manage health check started");

	setup_health_path();

	rcu_register_thread();

	/* We might hit an error path before this is created. */
	lttng_poll_init(&events);

	/* Create unix socket */
	sock = lttcomm_create_unix_sock(health_unix_sock_path);
	if (sock < 0) {
		ERR("Unable to create health check Unix socket");
		err = -1;
		goto error;
	}

	is_root = !getuid();
	if (is_root) {
		/* lttng health client socket path permissions */
		gid_t gid;

		ret = utils_get_group_id(tracing_group_name, true, &gid);
		if (ret) {
			/* Default to root group. */
			gid = 0;
		}

		ret = chown(health_unix_sock_path, 0, gid);
		if (ret < 0) {
			ERR("Unable to set group on %s", health_unix_sock_path);
			PERROR("chown");
			err = -1;
			goto error;
		}

		ret = chmod(health_unix_sock_path,
				S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		if (ret < 0) {
			ERR("Unable to set permissions on %s", health_unix_sock_path);
			PERROR("chmod");
			err = -1;
			goto error;
		}
	}

	/*
	 * Set the CLOEXEC flag. Return code is useless because either way, the
	 * show must go on.
	 */
	(void) utils_set_fd_cloexec(sock);

	ret = lttcomm_listen_unix_sock(sock);
	if (ret < 0) {
		goto error;
	}

	/* Size is set to 2 for the quit pipe and registration socket. */
	ret = lttng_poll_create(&events, 2, LTTNG_CLOEXEC);
	if (ret < 0) {
		ERR("Poll set creation failed");
		goto error;
	}

	ret = lttng_poll_add(&events, health_quit_pipe[0], LPOLLIN);
	if (ret < 0) {
		goto error;
	}

	/* Add the application registration socket */
	ret = lttng_poll_add(&events, sock, LPOLLIN | LPOLLPRI);
	if (ret < 0) {
		goto error;
	}

	/* Perform prior memory accesses before decrementing ready */
	cmm_smp_mb__before_uatomic_dec();
	uatomic_dec(&lttng_consumer_ready);

	while (1) {
		DBG("Health check ready");

		/* Inifinite blocking call, waiting for transmission */
restart:
		ret = lttng_poll_wait(&events, -1);
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		}

		nb_fd = ret;

		for (i = 0; i < nb_fd; i++) {
			/* Fetch once the poll data */
			const auto revents = LTTNG_POLL_GETEV(&events, i);
			const auto pollfd = LTTNG_POLL_GETFD(&events, i);

			/* Activity on health quit pipe, exiting. */
			if (pollfd == health_quit_pipe[0]) {
				DBG("Activity on health quit pipe");
				err = 0;
				goto exit;
			}

			/* Event on the registration socket */
			if (pollfd == sock) {
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)
						&& !(revents & LPOLLIN)) {
					ERR("Health socket poll error");
					goto error;
				}
			}
		}

		new_sock = lttcomm_accept_unix_sock(sock);
		if (new_sock < 0) {
			goto error;
		}

		/*
		 * Set the CLOEXEC flag. Return code is useless because either way, the
		 * show must go on.
		 */
		(void) utils_set_fd_cloexec(new_sock);

		DBG("Receiving data from client for health...");
		ret = lttcomm_recv_unix_sock(new_sock, (void *)&msg, sizeof(msg));
		if (ret <= 0) {
			DBG("Nothing recv() from client... continuing");
			ret = close(new_sock);
			if (ret) {
				PERROR("close");
			}
			new_sock = -1;
			continue;
		}

		rcu_thread_online();

		LTTNG_ASSERT(msg.cmd == HEALTH_CMD_CHECK);

		memset(&reply, 0, sizeof(reply));
		for (i = 0; i < NR_HEALTH_CONSUMERD_TYPES; i++) {
			/*
			 * health_check_state return 0 if thread is in
			 * error.
			 */
			if (!health_check_state(health_consumerd, i)) {
				reply.ret_code |= 1ULL << i;
			}
		}

		DBG("Health check return value %" PRIx64, reply.ret_code);

		ret = send_unix_sock(new_sock, (void *) &reply, sizeof(reply));
		if (ret < 0) {
			ERR("Failed to send health data back to client");
		}

		/* End of transmission */
		ret = close(new_sock);
		if (ret) {
			PERROR("close");
		}
		new_sock = -1;
	}

exit:
error:
	if (err) {
		ERR("Health error occurred in %s", __func__);
	}
	DBG("Health check thread dying");
	unlink(health_unix_sock_path);
	if (sock >= 0) {
		ret = close(sock);
		if (ret) {
			PERROR("close");
		}
	}

	lttng_poll_clean(&events);

	rcu_unregister_thread();
	return NULL;
}
