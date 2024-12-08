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

void golioth_req_list_remove(struct golioth_coap_req *req)
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

static void golioth_reqs_connected_set_unsafe(struct golioth_client *client, bool is_connected)
{
    client->coap_reqs_connected = is_connected;
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
