/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <golioth/golioth_sys.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "coap_client_zephyr.h"
#include "zephyr_coap_req.h"

#include "coap_request_list.h"

LOG_TAG_DEFINE(golioth_coap_request_list);

#define RESEND_REPORT_TIMEFRAME_S 10

void golioth_coap_reqs_init(struct golioth_client *client)
{
    /* FIXME: what if coap_reqs is not already NULL? */
    client->coap_reqs = NULL;
    client->coap_reqs_connected = false;
    client->coap_reqs_lock = golioth_sys_mutex_create();
}

// Function must not be run unless first guarded by golioth_client->coap_reqs_lock
static void req_list_append_unsafe(struct golioth_coap_req *req)
{
    if (!req)
    {
        return;
    }

    req->prev = NULL;
    req->next = NULL;

    struct golioth_coap_req *cur_node = req->client->coap_reqs;

    if (!cur_node)
    {
        req->client->coap_reqs = req;
        return;
    }

    while(cur_node->next)
    {
        cur_node = cur_node->next;
    }

    req->prev = cur_node;
    cur_node->next = req;
}

static void req_list_remove_unsafe(struct golioth_coap_req *req)
{
    if (!req)
    {
        return;
    }

    if (req->prev)
    {
        req->prev->next = req->next;
    }
    else {
        /* Pop head */
        req->client->coap_reqs = req->next;
    }

    if (req->next)
    {
        req->next->prev = req->prev;
    }
}

static void req_execute_callback(struct golioth_coap_req *req, enum golioth_status status)
{
    struct golioth_req_rsp rsp = {
        .user_data = req->user_data,
        .status = status,
    };

    (void) req->cb(&rsp);
}

static void golioth_reqs_connected_set_unsafe(struct golioth_client *client, bool is_connected)
{
    client->coap_reqs_connected = is_connected;
}

static bool golioth_coap_pending_cycle(struct golioth_coap_pending *pending)
{
    if (pending->timeout == 0)
    {
        /* Initial transmission. */
        pending->timeout = init_ack_timeout();

        return true;
    }

    if (pending->retries == 0)
    {
        return false;
    }

    pending->t0 += pending->timeout;
    pending->timeout = pending->timeout << 1;
    pending->retries--;

    return true;
}

void golioth_coap_pending_init(struct golioth_coap_pending *pending, uint8_t retries)
{
    pending->t0 = k_uptime_get_32();
    pending->timeout = 0;
    pending->retries = retries;
}

void golioth_req_list_remove(struct golioth_coap_req *req)
{
    golioth_sys_mutex_lock(req->client->coap_reqs_lock, GOLIOTH_SYS_WAIT_FOREVER);
    req_list_remove_unsafe(req);
    golioth_sys_mutex_unlock(req->client->coap_reqs_lock);
}

int golioth_coap_req_submit(struct golioth_coap_req *req)
{
    golioth_sys_mutex_lock(req->client->coap_reqs_lock, GOLIOTH_SYS_WAIT_FOREVER);
    if (!req->client->coap_reqs_connected)
    {
        return -ENETDOWN;
    }

    req_list_append_unsafe(req);
    golioth_sys_mutex_unlock(req->client->coap_reqs_lock);

    return GOLIOTH_OK;
}

void golioth_coap_reqs_connected_set(struct golioth_client *client, bool is_connected)
{
    golioth_sys_mutex_lock(client->coap_reqs_lock, GOLIOTH_SYS_WAIT_FOREVER);
    golioth_reqs_connected_set_unsafe(client, is_connected);
    golioth_sys_mutex_unlock(client->coap_reqs_lock);
}

void golioth_coap_reqs_cancel_all_with_reason(struct golioth_client *client,
                                              enum golioth_status reason)
{
    struct golioth_coap_req *req = client->coap_reqs;
    struct golioth_coap_req *next;

    golioth_sys_mutex_lock(client->coap_reqs_lock, GOLIOTH_SYS_WAIT_FOREVER);

    while(req)
    {

        /* Store pointer to next (or NULL) */
        next = req->next;

        /* Call callbacks */
        if (!req->is_observe)
        {
            req_execute_callback(req, reason);
        }

        /* Cancel the request and free memory */
        req_list_remove_unsafe(req);
        golioth_coap_req_free(req);

        /* Use stored pointer (or NULL) for next loop */
        req = next;
    }

    golioth_sys_mutex_unlock(client->coap_reqs_lock);
}

int golioth_coap_req_find_and_cancel_observation(struct golioth_client *client,
                                                 golioth_coap_request_msg_t *cancel_req_msg)
{
    int err;

    golioth_sys_mutex_lock(client->coap_reqs_lock, GOLIOTH_SYS_WAIT_FOREVER);

    struct golioth_coap_req *req = client->coap_reqs;

    while(req)
    {
        if ((req->user_data == cancel_req_msg) && (req->is_observe))
        {
            /* Cancel observation */
            err = golioth_coap_req_cancel_observation(req);

            /* free memory */
            req_list_remove_unsafe(req);
            golioth_coap_req_free(req);

            goto return_from_find_and_cancel_observation;
        }

        req = req->next;
    }

    err = GOLIOTH_ERR_NO_MORE_DATA;

return_from_find_and_cancel_observation:
    golioth_sys_mutex_unlock(client->coap_reqs_lock);
    return err;
}


int64_t golioth_coap_reqs_poll_prepare(struct golioth_client *client, int64_t now)
{
    golioth_sys_mutex_lock(client->coap_reqs_lock, GOLIOTH_SYS_WAIT_FOREVER);

    struct golioth_coap_req *req = client->coap_reqs;
    int64_t min_timeout = INT64_MAX;

    while(req)
    {
        if (req->is_observe && !req->is_pending)
        {
            req = req->next;
            continue;
        }

        int64_t req_timeout;
        bool send = false;
        bool resend = (req->pending.timeout != 0);
        int err;

        while (true)
        {
            req_timeout = (int32_t) (req->pending.t0 + req->pending.timeout) - (int32_t) now;

            if (req_timeout > 0)
            {
                /* Return timeout when packet still waits for response/ack */
                break;
            }

            send = golioth_coap_pending_cycle(&req->pending);
            if (!send)
            {
                LOG_WRN("Packet %p (reply %p) was not replied to", (void *) req, (void *) &req->reply);
                req_execute_callback(req, GOLIOTH_ERR_TIMEOUT);
                req_list_remove_unsafe(req);
                golioth_coap_req_free(req);

                req_timeout = INT64_MAX;
            }
        }

        if (send)
        {
            if (resend)
            {
                LOG_DBG("Resending request %p (reply %p) (retries %d)",
                        (void *) req,
                        (void *) &req->reply,
                        (int) req->pending.retries);

                req->client->resend_report_count++;
            }

            err = golioth_send_coap(req->client, &req->request);
            if (err)
            {
                LOG_ERR("Send error: %d", err);
            }
            err = 0;
        }

        if (req->client->resend_report_count
            && ((now - req->client->resend_report_last_ms) >= (RESEND_REPORT_TIMEFRAME_S * 1000)))
        {
            LOG_WRN("%u resends in last %d seconds",
                    req->client->resend_report_count,
                    RESEND_REPORT_TIMEFRAME_S);
            req->client->resend_report_last_ms = now;
            req->client->resend_report_count = 0;
        }

        min_timeout = MIN(min_timeout, req_timeout);
        req = req->next;
    }

    golioth_sys_mutex_unlock(client->coap_reqs_lock);

    return min_timeout;
}
