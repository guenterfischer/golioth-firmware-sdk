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
 * @brief Add request the stored requests list
 *
 * @param[inout] req Coap request to be added
 */
void golioth_req_list_append(struct golioth_coap_req *req);

/**
 * @brief Remove request from the store requests list
 *
 * @param[inout] req Coap request to be removed
 */
void golioth_req_list_remove(struct golioth_coap_req *req);

#ifdef __cplusplus
}
#endif
