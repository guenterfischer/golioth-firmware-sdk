/*
 * Copyright (c) 2024 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __cplusplus
extern "C"
{
#endif

#pragma once

#include <stdint.h>

/* TODO: Declare golioth_coap_req so ports can define their own and remove this include */
#include "zephyr_coap_req.h"

/**
 * @brief Initialize CoAP requests for client
 *
 * Initializes CoAP requests handling for Golioth client instance.
 *
 * @param[inout] client Client instance
 */
void golioth_coap_reqs_init(struct golioth_client *client);

/**
 * @brief Add request to the stored requests list
 *
 * Requests will only be added if the client is currently connected.
 *
 * @param[inout] req Coap request to be added
 */
int golioth_coap_req_submit(struct golioth_coap_req *req);

/**
 * @brief Remove request from the store requests list
 *
 * @param[inout] req Coap request to be removed
 */
void golioth_req_list_remove(struct golioth_coap_req *req);

/**
 * @brief Store connection state specifically for use with coap_reqs list
 *
 * client->sock is protected by client->lock, so submitting new coap_req
 * requests would potentially block on other thread currently receiving
 * or sending data using golioth_{recv,send} APIs.
 *
 * Hence use another client->coap_reqs_connected to save information
 * whether we are connected or not.
 *
 * @param[inout] client Client instance
 * @param[in] is_connected Connection state to store
 */
void golioth_coap_reqs_connected_set(struct golioth_client *client, bool is_connected);

#ifdef __cplusplus
}
#endif
