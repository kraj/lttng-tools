/*
 * Copyright (C) 2013 Julien Desfossez <jdesfossez@efficios.com>
 * Copyright (C) 2013 David Goulet <dgoulet@efficios.com>
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include <common/common.hpp>
#include <urcu/rculist.h>

#include "lttng-relayd.hpp"
#include "ctf-trace.hpp"
#include "session.hpp"
#include "viewer-session.hpp"
#include "viewer-stream.hpp"
#include "stream.hpp"

struct relay_viewer_session *viewer_session_create(void)
{
	struct relay_viewer_session *vsession;

	vsession = zmalloc<relay_viewer_session>();
	if (!vsession) {
		goto end;
	}
	CDS_INIT_LIST_HEAD(&vsession->session_list);
end:
	return vsession;
}

int viewer_session_set_trace_chunk_copy(struct relay_viewer_session *vsession,
		struct lttng_trace_chunk *relay_session_trace_chunk)
{
	int ret = 0;
	struct lttng_trace_chunk *viewer_chunk;

	lttng_trace_chunk_put(vsession->current_trace_chunk);
	vsession->current_trace_chunk = NULL;

	DBG("Copying relay session's current trace chunk to the viewer session");
	if (!relay_session_trace_chunk) {
		goto end;
	}

	viewer_chunk = lttng_trace_chunk_copy(relay_session_trace_chunk);
	if (!viewer_chunk) {
		ERR("Failed to create a viewer trace chunk from the relay session's current chunk");
		ret = -1;
		goto end;
	}

	vsession->current_trace_chunk = viewer_chunk;
end:
	return ret;
}

/* The existence of session must be guaranteed by the caller. */
enum lttng_viewer_attach_return_code viewer_session_attach(
		struct relay_viewer_session *vsession,
		struct relay_session *session)
{
	enum lttng_viewer_attach_return_code viewer_attach_status =
			LTTNG_VIEWER_ATTACH_OK;

	ASSERT_LOCKED(session->lock);

	/* Will not fail, as per the ownership guarantee. */
	if (!session_get(session)) {
		viewer_attach_status = LTTNG_VIEWER_ATTACH_UNK;
		goto end;
	}
	if (session->viewer_attached) {
		viewer_attach_status = LTTNG_VIEWER_ATTACH_ALREADY;
	} else {
		int ret;

		session->viewer_attached = true;

		ret = viewer_session_set_trace_chunk_copy(vsession,
				session->current_trace_chunk);
		if (ret) {
			/*
			 * The live protocol does not define a generic error
			 * value for the "attach" command. The "unknown"
			 * status is used so that the viewer may handle this
			 * failure as if the session didn't exist anymore.
			 */
			DBG("Failed to create a viewer trace chunk from the current trace chunk of session \"%s\", returning LTTNG_VIEWER_ATTACH_UNK",
					session->session_name);
			viewer_attach_status = LTTNG_VIEWER_ATTACH_UNK;
		}
	}

	if (viewer_attach_status == LTTNG_VIEWER_ATTACH_OK) {
		pthread_mutex_lock(&vsession->session_list_lock);
		/* Ownership is transfered to the list. */
		cds_list_add_rcu(&session->viewer_session_node,
				&vsession->session_list);
		pthread_mutex_unlock(&vsession->session_list_lock);
	} else {
		/* Put our local ref. */
		session_put(session);
	}
end:
	return viewer_attach_status;
}

/* The existence of session must be guaranteed by the caller. */
static int viewer_session_detach(struct relay_viewer_session *vsession,
		struct relay_session *session)
{
	int ret = 0;

	pthread_mutex_lock(&session->lock);
	if (!session->viewer_attached) {
		ret = -1;
	} else {
		session->viewer_attached = false;
	}

	if (!ret) {
		pthread_mutex_lock(&vsession->session_list_lock);
		cds_list_del_rcu(&session->viewer_session_node);
		pthread_mutex_unlock(&vsession->session_list_lock);
		/* Release reference held by the list. */
		session_put(session);
	}
	/* Safe since we know the session exists. */
	pthread_mutex_unlock(&session->lock);
	return ret;
}

void viewer_session_destroy(struct relay_viewer_session *vsession)
{
	lttng_trace_chunk_put(vsession->current_trace_chunk);
	free(vsession);
}

/*
 * Release ownership of all the streams of one session and detach the viewer.
 */
void viewer_session_close_one_session(struct relay_viewer_session *vsession,
		struct relay_session *session)
{
	struct lttng_ht_iter iter;
	struct relay_viewer_stream *vstream;

	/*
	 * TODO: improvement: create more efficient list of
	 * vstream per session.
	 */
	cds_lfht_for_each_entry(viewer_streams_ht->ht, &iter.iter,
			vstream, stream_n.node) {
		if (!viewer_stream_get(vstream)) {
			continue;
		}
		if (vstream->stream->trace->session != session) {
			viewer_stream_put(vstream);
			continue;
		}
		/* Put local reference. */
		viewer_stream_put(vstream);
		/*
		 * We have reached one of the viewer stream's lifetime
		 * end condition. This "put" will cause the proper
		 * teardown of the viewer stream.
		 */
		viewer_stream_put(vstream);
	}
	lttng_trace_chunk_put(vsession->current_trace_chunk);
	vsession->current_trace_chunk = NULL;
	viewer_session_detach(vsession, session);
}

void viewer_session_close(struct relay_viewer_session *vsession)
{
	struct relay_session *session;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(session,
			&vsession->session_list, viewer_session_node) {
		viewer_session_close_one_session(vsession, session);
	}
	rcu_read_unlock();
}

/*
 * Check if a connection is attached to a session.
 * Return 1 if attached, 0 if not attached, a negative value on error.
 */
int viewer_session_is_attached(struct relay_viewer_session *vsession,
		struct relay_session *session)
{
	struct relay_session *iter;
	int found = 0;

	pthread_mutex_lock(&session->lock);
	if (!vsession) {
		goto end;
	}
	if (!session->viewer_attached) {
		goto end;
	}
	rcu_read_lock();
	cds_list_for_each_entry_rcu(iter,
			&vsession->session_list,
			viewer_session_node) {
		if (session == iter) {
			found = 1;
			goto end_rcu_unlock;
		}
	}
end_rcu_unlock:
	rcu_read_unlock();
end:
	pthread_mutex_unlock(&session->lock);
	return found;
}
