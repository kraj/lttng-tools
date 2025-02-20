/*
 * Copyright (C) 2011 EfficiOS Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include <popt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <lttng/lttng.h>

#include "../command.hpp"

#include <common/mi-lttng.hpp>
#include <common/sessiond-comm/sessiond-comm.hpp>
#include <common/utils.hpp>

static int opt_destroy_all;
static int opt_no_wait;

#ifdef LTTNG_EMBED_HELP
static const char help_msg[] =
#include <lttng-destroy.1.h>
;
#endif

/* Mi writer */
static struct mi_writer *writer;

enum {
	OPT_HELP = 1,
	OPT_LIST_OPTIONS,
};

static struct poptOption long_options[] = {
	/* longName, shortName, argInfo, argPtr, value, descrip, argDesc */
	{"help",      'h', POPT_ARG_NONE, 0, OPT_HELP, 0, 0},
	{"all",       'a', POPT_ARG_VAL, &opt_destroy_all, 1, 0, 0},
	{"list-options", 0, POPT_ARG_NONE, NULL, OPT_LIST_OPTIONS, NULL, NULL},
	{"no-wait",   'n', POPT_ARG_VAL, &opt_no_wait, 1, 0, 0},
	{0, 0, 0, 0, 0, 0, 0}
};

/*
 * destroy_session
 *
 * Unregister the provided session to the session daemon. On success, removes
 * the default configuration.
 */
static int destroy_session(struct lttng_session *session)
{
	int ret;
	char *session_name = NULL;
	bool session_was_already_stopped;
	enum lttng_error_code ret_code;
	struct lttng_destruction_handle *handle = NULL;
	enum lttng_destruction_handle_status status;
	bool newline_needed = false, printed_destroy_msg = false;
	enum lttng_rotation_state rotation_state;
	char *stats_str = NULL;

	ret = lttng_stop_tracing_no_wait(session->name);
	if (ret < 0 && ret != -LTTNG_ERR_TRACE_ALREADY_STOPPED) {
		ERR("%s", lttng_strerror(ret));
	}

	session_was_already_stopped = ret == -LTTNG_ERR_TRACE_ALREADY_STOPPED;
	if (!opt_no_wait) {
		do {
			ret = lttng_data_pending(session->name);
			if (ret < 0) {
				/* Return the data available call error. */
				goto error;
			}

			/*
			 * Data sleep time before retrying (in usec). Don't
			 * sleep if the call returned value indicates
			 * availability.
			 */
			if (ret) {
				if (!printed_destroy_msg) {
					_MSG("Destroying session %s",
							session->name);
					newline_needed = true;
					printed_destroy_msg = true;
					fflush(stdout);
				}

				usleep(DEFAULT_DATA_AVAILABILITY_WAIT_TIME_US);
				_MSG(".");
				fflush(stdout);
			}
		} while (ret != 0);
	}

	if (!session_was_already_stopped) {
		/*
		 * Don't print the event and packet loss warnings since the user
		 * already saw them when stopping the trace.
		 */
		ret = get_session_stats_str(session->name, &stats_str);
		if (ret < 0) {
			goto error;
		}
	}

	ret_code = lttng_destroy_session_ext(session->name, &handle);
	if (ret_code != LTTNG_OK) {
		ret = -ret_code;
		goto error;
	}

	if (opt_no_wait) {
		goto skip_wait_rotation;
	}

	do {
		status = lttng_destruction_handle_wait_for_completion(
				handle, DEFAULT_DATA_AVAILABILITY_WAIT_TIME_US /
							USEC_PER_MSEC);
		switch (status) {
		case LTTNG_DESTRUCTION_HANDLE_STATUS_TIMEOUT:
			if (!printed_destroy_msg) {
				_MSG("Destroying session %s", session->name);
				newline_needed = true;
				printed_destroy_msg = true;
			}
			_MSG(".");
			fflush(stdout);
			break;
		case LTTNG_DESTRUCTION_HANDLE_STATUS_COMPLETED:
			break;
		default:
			ERR("%sFailed to wait for the completion of the destruction of session \"%s\"",
					newline_needed ? "\n" : "",
					session->name);
			newline_needed = false;
			ret = -1;
			goto error;
		}
	} while (status == LTTNG_DESTRUCTION_HANDLE_STATUS_TIMEOUT);

	status = lttng_destruction_handle_get_result(handle, &ret_code);
	if (status != LTTNG_DESTRUCTION_HANDLE_STATUS_OK) {
		ERR("%sFailed to get the result of session destruction",
				newline_needed ? "\n" : "");
		ret = -1;
		newline_needed = false;
		goto error;
	}
	if (ret_code != LTTNG_OK) {
		ret = -ret_code;
		goto error;
	}

	status = lttng_destruction_handle_get_rotation_state(
			handle, &rotation_state);
	if (status != LTTNG_DESTRUCTION_HANDLE_STATUS_OK) {
		ERR("%sFailed to get rotation state from destruction handle",
				newline_needed ? "\n" : "");
		newline_needed = false;
		goto skip_wait_rotation;
	}

	switch (rotation_state) {
	case LTTNG_ROTATION_STATE_NO_ROTATION:
		break;
	case LTTNG_ROTATION_STATE_COMPLETED:
	{
		const struct lttng_trace_archive_location *location;

		status = lttng_destruction_handle_get_archive_location(
				handle, &location);
		if (status == LTTNG_DESTRUCTION_HANDLE_STATUS_OK) {
			ret = print_trace_archive_location(
					location, session->name);
			if (ret) {
				ERR("%sFailed to print the location of trace archive",
						newline_needed ? "\n" : "");
				newline_needed = false;
				goto skip_wait_rotation;
			}
			break;
		}
	}
	/* fall-through. */
	default:
		ERR("%sFailed to get the location of the rotation performed during the session's destruction",
				newline_needed ? "\n" : "");
		newline_needed = false;
		goto skip_wait_rotation;
	}
skip_wait_rotation:
	MSG("%sSession %s destroyed", newline_needed ? "\n" : "",
			session->name);
	newline_needed = false;
	if (stats_str) {
		MSG("%s", stats_str);
	}

	session_name = get_session_name_quiet();
	if (session_name && !strncmp(session->name, session_name, NAME_MAX)) {
		config_destroy_default();
	}

	if (lttng_opt_mi) {
		ret = mi_lttng_session(writer, session, 0);
		if (ret) {
			ret = CMD_ERROR;
			goto error;
		}
	}

	ret = CMD_SUCCESS;
error:
	if (newline_needed) {
		MSG("");
	}
	lttng_destruction_handle_destroy(handle);
	free(session_name);
	free(stats_str);
	return ret;
}

/*
 * destroy_all_sessions
 *
 * Call destroy_sessions for each registered sessions
 */
static int destroy_all_sessions(struct lttng_session *sessions, int count)
{
	int i;
	bool error_occurred = false;

	LTTNG_ASSERT(count >= 0);
	if (count == 0) {
		MSG("No session found, nothing to do.");
	}

	for (i = 0; i < count; i++) {
		int ret = destroy_session(&sessions[i]);

		if (ret < 0) {
			ERR("%s during the destruction of session \"%s\"",
					lttng_strerror(ret),
					sessions[i].name);
			/* Continue to next session. */
			error_occurred = true;
		}
	}

	return error_occurred ? CMD_ERROR : CMD_SUCCESS;
}

/*
 * The 'destroy <options>' first level command
 */
int cmd_destroy(int argc, const char **argv)
{
	int opt;
	int ret = CMD_SUCCESS , i, command_ret = CMD_SUCCESS, success = 1;
	static poptContext pc;
	char *session_name = NULL;
	const char *arg_session_name = NULL;
	const char *leftover = NULL;

	struct lttng_session *sessions = NULL;
	int count;
	int found;

	pc = poptGetContext(NULL, argc, argv, long_options, 0);
	poptReadDefaultConfig(pc, 0);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		case OPT_HELP:
			SHOW_HELP();
			break;
		case OPT_LIST_OPTIONS:
			list_cmd_options(stdout, long_options);
			break;
		default:
			ret = CMD_UNDEFINED;
			break;
		}
		goto end;
	}

	/* Mi preparation */
	if (lttng_opt_mi) {
		writer = mi_lttng_writer_create(fileno(stdout), lttng_opt_mi);
		if (!writer) {
			ret = -LTTNG_ERR_NOMEM;
			goto end;
		}

		/* Open command element */
		ret = mi_lttng_writer_command_open(writer,
				mi_lttng_element_command_destroy);
		if (ret) {
			ret = CMD_ERROR;
			goto end;
		}

		/* Open output element */
		ret = mi_lttng_writer_open_element(writer,
				mi_lttng_element_command_output);
		if (ret) {
			ret = CMD_ERROR;
			goto end;
		}

		/* For validation and semantic purpose we open a sessions element */
		ret = mi_lttng_sessions_open(writer);
		if (ret) {
			ret = CMD_ERROR;
			goto end;
		}
	}

	/* Recuperate all sessions for further operation */
	count = lttng_list_sessions(&sessions);
	if (count < 0) {
		ERR("%s", lttng_strerror(count));
		command_ret = CMD_ERROR;
		success = 0;
		goto mi_closing;
	}

	/* Ignore session name in case all sessions are to be destroyed */
	if (opt_destroy_all) {
		command_ret = destroy_all_sessions(sessions, count);
		if (command_ret) {
			success = 0;
		}
	} else {
		arg_session_name = poptGetArg(pc);

		if (!arg_session_name) {
			/* No session name specified, lookup default */
			session_name = get_session_name();
		} else {
			session_name = strdup(arg_session_name);
			if (session_name == NULL) {
				PERROR("Failed to copy session name");
			}
		}

		if (session_name == NULL) {
			command_ret = CMD_ERROR;
			success = 0;
			goto mi_closing;
		}

		/* Find the corresponding lttng_session struct */
		found = 0;
		for (i = 0; i < count; i++) {
			if (strncmp(sessions[i].name, session_name, NAME_MAX) == 0) {
				found = 1;
				command_ret = destroy_session(&sessions[i]);
				if (command_ret) {
					success = 0;
					ERR("%s during the destruction of session \"%s\"",
							lttng_strerror(command_ret),
							sessions[i].name);
				}
			}
		}

		if (!found) {
			ERR("Session name %s not found", session_name);
			command_ret = LTTNG_ERR_SESS_NOT_FOUND;
			success = 0;
			goto mi_closing;
		}
	}

	leftover = poptGetArg(pc);
	if (leftover) {
		ERR("Unknown argument: %s", leftover);
		ret = CMD_ERROR;
		success = 0;
		goto mi_closing;
	}

mi_closing:
	/* Mi closing */
	if (lttng_opt_mi) {
		/* Close sessions and output element element */
		ret = mi_lttng_close_multi_element(writer, 2);
		if (ret) {
			ret = CMD_ERROR;
			goto end;
		}

		/* Success ? */
		ret = mi_lttng_writer_write_element_bool(writer,
				mi_lttng_element_command_success, success);
		if (ret) {
			ret = CMD_ERROR;
			goto end;
		}

		/* Command element close */
		ret = mi_lttng_writer_command_close(writer);
		if (ret) {
			ret = CMD_ERROR;
			goto end;
		}
	}
end:
	/* Mi clean-up */
	if (writer && mi_lttng_writer_destroy(writer)) {
		/* Preserve original error code */
		ret = ret ? ret : -LTTNG_ERR_MI_IO_FAIL;
	}

	free(session_name);
	free(sessions);

	/* Overwrite ret if an error occurred during destroy_session/all */
	ret = command_ret ? command_ret : ret;

	poptFreeContext(pc);
	return ret;
}
