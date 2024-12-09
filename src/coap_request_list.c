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

static void req_execute_callback(struct golioth_coap_req *req,
                                 enum golioth_status status,
                                 struct golioth_req_rsp *rsp)
{
    struct golioth_req_rsp *safe_rsp = rsp;
    bool generate_rsp = (rsp == NULL);

    if (generate_rsp)
    {
        safe_rsp = golioth_sys_malloc(sizeof(struct golioth_req_rsp));
        if (!safe_rsp)
        {
            GLTH_LOGE(TAG, "Enable to allocate rsp");
            return;
        }

        safe_rsp->user_data = req->user_data;
        safe_rsp->status = status;
    }

    (void) req->cb(safe_rsp);

    if (generate_rsp)
    {
        free(safe_rsp);
    }
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

/* Reordering according to RFC7641 section 3.4 */
static inline bool sequence_number_is_newer(int v1, int v2)
{
    return (v1 < v2 && v2 - v1 < (1 << 23)) || (v1 > v2 && v1 - v2 > (1 << 23));
}

static bool golioth_coap_reply_is_newer(struct golioth_coap_reply *reply, int seq, int64_t uptime)
{
    /* FIXME: What is this COAP_OBSERVE_TS_DIFF_NEWER? It needs to be abstracted. */
    return (uptime > reply->ts + COAP_OBSERVE_TS_DIFF_NEWER
            || sequence_number_is_newer(reply->seq, seq));
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
            req_execute_callback(req, reason, NULL);
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
    struct golioth_coap_req *next;

    int64_t min_timeout = INT64_MAX;

    while(req)
    {
        /* Copy next node pointer in case we remove this request */
        next = req->next;

        if (req->is_observe && !req->is_pending)
        {
            req = next;
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
                req_execute_callback(req, GOLIOTH_ERR_TIMEOUT, NULL);
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
        req = next;
    }

    golioth_sys_mutex_unlock(client->coap_reqs_lock);

    return min_timeout;
}

void golioth_request_list_process_response(struct golioth_client *client,
                                           const struct coap_packet *response,
                                           uint16_t rx_id,
                                           uint8_t rx_token[COAP_TOKEN_MAX_LEN],
                                           uint8_t rx_tkl,
                                           int observe_seq)
{
    golioth_sys_mutex_lock(client->coap_reqs_lock, GOLIOTH_SYS_WAIT_FOREVER);

    struct golioth_coap_req *req = client->coap_reqs;

    while(req)
    {
        uint16_t req_id = coap_header_get_id(&req->request);
        uint8_t req_token[COAP_TOKEN_MAX_LEN];
        uint8_t req_tkl = coap_header_get_token(&req->request, req_token);

        if (req_id == 0U && req_tkl == 0U)
        {
            req = req->next;
            continue;
        }

        /* Piggybacked must match id when token is empty */
        if (req_id != rx_id && rx_tkl == 0U)
        {
            req = req->next;
            continue;
        }

        if (rx_tkl > 0 && memcmp(req_token, rx_token, rx_tkl))
        {
            req = req->next;
            continue;
        }

        if (observe_seq == -ENOENT)
        {
            enum golioth_status status;
            struct golioth_req_rsp rsp;
            bool run_callback_and_remove;

            status = golioth_coap_req_reply_handler(req, response, &rsp, &run_callback_and_remove);

            if (run_callback_and_remove)
            {
                req_execute_callback(req, status, &rsp);
                req_list_remove_unsafe(req);
                golioth_coap_req_free(req);
            }
        }
        else
        {
            int64_t uptime = k_uptime_get();

            /* handle observed requests only if received in order */
            if (golioth_coap_reply_is_newer(&req->reply, observe_seq, uptime))
            {
                req->reply.seq = observe_seq;
                req->reply.ts = uptime;

                enum golioth_status status;
                struct golioth_req_rsp rsp;
                bool run_callback_and_remove;

                status =
                    golioth_coap_req_reply_handler(req, response, &rsp, &run_callback_and_remove);

                if (run_callback_and_remove)
                {
                    req_execute_callback(req, status, &rsp);
                    req_list_remove_unsafe(req);
                    golioth_coap_req_free(req);
                }
            }
        }

        break;
    }

    golioth_sys_mutex_unlock(client->coap_reqs_lock);
}
