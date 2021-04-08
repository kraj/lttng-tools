/*
 * Copyright (C) 2019 Simon Marchi <simon.marchi@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include <assert.h>
#include <common/error.h>
#include <common/macros.h>
#include <lttng/action/action-internal.h>
#include <lttng/action/firing-policy-internal.h>
#include <lttng/action/firing-policy.h>
#include <lttng/action/stop-session-internal.h>
#include <lttng/action/stop-session.h>

#define IS_STOP_SESSION_ACTION(action) \
	(lttng_action_get_type(action) == LTTNG_ACTION_TYPE_STOP_SESSION)

struct lttng_action_stop_session {
	struct lttng_action parent;

	/* Owned by this. */
	char *session_name;
	struct lttng_firing_policy *policy;
};

struct lttng_action_stop_session_comm {
	/* Includes the trailing \0. */
	uint32_t session_name_len;

	/*
	 * Variable data:
	 *
	 *  - session name (null terminated)
	 *  - policy
	 */
	char data[];
} LTTNG_PACKED;

static struct lttng_action_stop_session *action_stop_session_from_action(
		struct lttng_action *action)
{
	assert(action);

	return container_of(action, struct lttng_action_stop_session, parent);
}

static const struct lttng_action_stop_session *
action_stop_session_from_action_const(const struct lttng_action *action)
{
	assert(action);

	return container_of(action, struct lttng_action_stop_session, parent);
}

static bool lttng_action_stop_session_validate(struct lttng_action *action)
{
	bool valid;
	struct lttng_action_stop_session *action_stop_session;

	if (!action) {
		valid = false;
		goto end;
	}

	action_stop_session = action_stop_session_from_action(action);

	/* A non-empty session name is mandatory. */
	if (!action_stop_session->session_name ||
			strlen(action_stop_session->session_name) == 0) {
		valid = false;
		goto end;
	}

	valid = true;
end:
	return valid;
}

static bool lttng_action_stop_session_is_equal(
		const struct lttng_action *_a, const struct lttng_action *_b)
{
	bool is_equal = false;
	const struct lttng_action_stop_session *a, *b;

	a = action_stop_session_from_action_const(_a);
	b = action_stop_session_from_action_const(_b);

	/* Action is not valid if this is not true. */
	assert(a->session_name);
	assert(b->session_name);
	if (strcmp(a->session_name, b->session_name)) {
		goto end;
	}

	is_equal = lttng_firing_policy_is_equal(a->policy, b->policy);
end:
	return is_equal;
}

static int lttng_action_stop_session_serialize(
		struct lttng_action *action, struct lttng_payload *payload)
{
	struct lttng_action_stop_session *action_stop_session;
	struct lttng_action_stop_session_comm comm;
	size_t session_name_len;
	int ret;

	assert(action);
	assert(payload);

	action_stop_session = action_stop_session_from_action(action);

	assert(action_stop_session->session_name);

	DBG("Serializing stop session action: session-name: %s",
			action_stop_session->session_name);

	session_name_len = strlen(action_stop_session->session_name) + 1;
	comm.session_name_len = session_name_len;

	ret = lttng_dynamic_buffer_append(
			&payload->buffer, &comm, sizeof(comm));
	if (ret) {
		ret = -1;
		goto end;
	}

	ret = lttng_dynamic_buffer_append(&payload->buffer,
			action_stop_session->session_name, session_name_len);
	if (ret) {
		ret = -1;
		goto end;
	}

	ret = lttng_firing_policy_serialize(
			action_stop_session->policy, payload);
	if (ret) {
		ret = -1;
		goto end;
	}

	ret = 0;
end:
	return ret;
}

static void lttng_action_stop_session_destroy(struct lttng_action *action)
{
	struct lttng_action_stop_session *action_stop_session;

	if (!action) {
		goto end;
	}

	action_stop_session = action_stop_session_from_action(action);

	lttng_firing_policy_destroy(action_stop_session->policy);
	free(action_stop_session->session_name);
	free(action_stop_session);

end:
	return;
}

ssize_t lttng_action_stop_session_create_from_payload(
		struct lttng_payload_view *view,
		struct lttng_action **p_action)
{
	ssize_t consumed_len, ret;
	const struct lttng_action_stop_session_comm *comm;
	const char *session_name;
	struct lttng_action *action = NULL;
	enum lttng_action_status status;
	struct lttng_firing_policy *policy = NULL;

	comm = (typeof(comm)) view->buffer.data;
	session_name = (const char *) &comm->data;

	/* Session name. */
	if (!lttng_buffer_view_contains_string(
			&view->buffer, session_name, comm->session_name_len)) {
		consumed_len = -1;
		goto end;
	}
	consumed_len = sizeof(*comm) + comm->session_name_len;

	/* Firing policy. */
	{
		struct lttng_payload_view policy_view =
				lttng_payload_view_from_view(
						view, consumed_len, -1);
		ret = lttng_firing_policy_create_from_payload(
				&policy_view, &policy);
		if (ret < 0) {
			consumed_len = -1;
			goto end;
		}
		consumed_len += ret;
	}

	action = lttng_action_stop_session_create();
	if (!action) {
		consumed_len = -1;
		goto end;
	}

	status = lttng_action_stop_session_set_session_name(
			action, session_name);
	if (status != LTTNG_ACTION_STATUS_OK) {
		consumed_len = -1;
		goto end;
	}

	assert(policy);
	status = lttng_action_stop_session_set_firing_policy(action, policy);
	if (status != LTTNG_ACTION_STATUS_OK) {
		consumed_len = -1;
		goto end;
	}

	*p_action = action;
	action = NULL;

end:
	lttng_firing_policy_destroy(policy);
	lttng_action_stop_session_destroy(action);

	return consumed_len;
}

struct lttng_action *lttng_action_stop_session_create(void)
{
	struct lttng_action *action = NULL;
	struct lttng_firing_policy *policy = NULL;
	enum lttng_action_status status;

	/* Create a every N = 1 firing policy. */
	policy = lttng_firing_policy_every_n_create(1);
	if (!policy) {
		goto end;
	}

	action = zmalloc(sizeof(struct lttng_action_stop_session));
	if (!action) {
		goto end;
	}

	lttng_action_init(action, LTTNG_ACTION_TYPE_STOP_SESSION,
			lttng_action_stop_session_validate,
			lttng_action_stop_session_serialize,
			lttng_action_stop_session_is_equal,
			lttng_action_stop_session_destroy);

	status = lttng_action_stop_session_set_firing_policy(action, policy);
	if (status != LTTNG_ACTION_STATUS_OK) {
		free(action);
		action = NULL;
		goto end;
	}

end:
	lttng_firing_policy_destroy(policy);
	return action;
}

enum lttng_action_status lttng_action_stop_session_set_session_name(
		struct lttng_action *action, const char *session_name)
{
	struct lttng_action_stop_session *action_stop_session;
	enum lttng_action_status status;

	if (!action || !IS_STOP_SESSION_ACTION(action) || !session_name ||
			strlen(session_name) == 0) {
		status = LTTNG_ACTION_STATUS_INVALID;
		goto end;
	}

	action_stop_session = action_stop_session_from_action(action);

	free(action_stop_session->session_name);

	action_stop_session->session_name = strdup(session_name);
	if (!action_stop_session->session_name) {
		status = LTTNG_ACTION_STATUS_ERROR;
		goto end;
	}

	status = LTTNG_ACTION_STATUS_OK;
end:
	return status;
}

enum lttng_action_status lttng_action_stop_session_get_session_name(
		const struct lttng_action *action, const char **session_name)
{
	const struct lttng_action_stop_session *action_stop_session;
	enum lttng_action_status status;

	if (!action || !IS_STOP_SESSION_ACTION(action) || !session_name) {
		status = LTTNG_ACTION_STATUS_INVALID;
		goto end;
	}

	action_stop_session = action_stop_session_from_action_const(action);

	*session_name = action_stop_session->session_name;

	status = LTTNG_ACTION_STATUS_OK;
end:
	return status;
}

enum lttng_action_status lttng_action_stop_session_set_firing_policy(
		struct lttng_action *action,
		const struct lttng_firing_policy *policy)
{
	enum lttng_action_status status;
	struct lttng_action_stop_session *stop_session_action;
	struct lttng_firing_policy *copy = NULL;

	if (!action || !policy || !IS_STOP_SESSION_ACTION(action)) {
		status = LTTNG_ACTION_STATUS_INVALID;
		goto end;
	}

	copy = lttng_firing_policy_copy(policy);
	if (!copy) {
		status = LTTNG_ACTION_STATUS_ERROR;
		goto end;
	}
	stop_session_action = action_stop_session_from_action(action);

	/* Free the previous firing policy .*/
	lttng_firing_policy_destroy(stop_session_action->policy);

	stop_session_action->policy = copy;
	status = LTTNG_ACTION_STATUS_OK;
	copy = NULL;

end:
	lttng_firing_policy_destroy(copy);
	return status;
}

enum lttng_action_status lttng_action_stop_session_get_firing_policy(
		const struct lttng_action *action,
		const struct lttng_firing_policy **policy)
{
	enum lttng_action_status status;
	const struct lttng_action_stop_session *stop_session_action;

	if (!action || !policy || !IS_STOP_SESSION_ACTION(action)) {
		status = LTTNG_ACTION_STATUS_INVALID;
		goto end;
	}

	stop_session_action = action_stop_session_from_action_const(action);

	*policy = stop_session_action->policy;
	status = LTTNG_ACTION_STATUS_OK;
end:
	return status;
}
