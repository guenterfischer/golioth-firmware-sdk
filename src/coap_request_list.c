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

void golioth_req_list_append(struct golioth_coap_req *req)
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
