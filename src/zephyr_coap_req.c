/*
 * Copyright (c) 2022 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "golioth/golioth_status.h"
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(golioth_coap_client);

#include <stdlib.h>

#include <zephyr/random/random.h>

#include "coap_client.h"
#include "coap_client_zephyr.h"
#include "coap_request_list.h"
#include "zephyr_coap_req.h"
#include "zephyr_coap_utils.h"

const int64_t COAP_OBSERVE_TS_DIFF_NEWER = 128 * (int64_t) MSEC_PER_SEC;

#define COAP_RESPONSE_CODE_CLASS(code) ((code) >> 5)

enum golioth_status golioth_coap_req_reply_handler(struct golioth_coap_req *req,
                                                   const struct coap_packet *response,
                                                   struct golioth_req_rsp *rsp,
                                                   bool *run_callback_and_remove)
{
    uint16_t payload_len;
    uint8_t code;
    const uint8_t *payload;
    int block2;
    int err = 0;

    code = coap_header_get_code(response);

    rsp->coap_rsp_code.code_class = (uint8_t) (code >> 5);
    rsp->coap_rsp_code.code_detail = (uint8_t) (code & 0x1f);
    *run_callback_and_remove = true;

    LOG_DBG("CoAP response code: 0x%x (class %u detail %u)",
            (unsigned int) code,
            rsp->coap_rsp_code.code_class,
            rsp->coap_rsp_code.code_detail);

    if (code == COAP_RESPONSE_CODE_BAD_REQUEST)
    {
        LOG_WRN("Server reports CoAP Bad Request. (Check payload formatting)");
    }

    /* Check for 2.xx style CoAP response success code */
    if (rsp->coap_rsp_code.code_class != 2)
    {
        rsp->user_data = req->user_data;
        rsp->status = GOLIOTH_ERR_COAP_RESPONSE;

        LOG_DBG("cancel and free req: %p", (void *) req);

        err = rsp->status;
        goto cancel_and_free;
    }
    else
    {
        rsp->status = GOLIOTH_OK;
    }

    payload = coap_packet_get_payload(response, &payload_len);

    block2 = coap_get_option_int(response, COAP_OPTION_BLOCK2);
    if (block2 != -ENOENT)
    {
        size_t want_offset = req->block_ctx.current;
        size_t cur_offset;
        size_t new_offset;

        err = coap_update_from_block(response, &req->block_ctx);
        if (err)
        {
            rsp->user_data = req->user_data;
            rsp->status = GOLIOTH_ERR_INVALID_FORMAT;

            LOG_ERR("Failed to parse get response: %d", err);

            err = rsp->status;
            goto cancel_and_free;
        }

        cur_offset = req->block_ctx.current;
        if (cur_offset < want_offset)
        {
            LOG_WRN("Block at %zu already received, ignoring", cur_offset);

            req->block_ctx.current = want_offset;

            *run_callback_and_remove = false;

            err = GOLIOTH_OK;
            goto cancel_and_free;
        }

        new_offset = coap_next_block_for_option(response, &req->block_ctx, COAP_OPTION_BLOCK2);
        if (new_offset < 0)
        {
            rsp->user_data = req->user_data;
            rsp->status = GOLIOTH_ERR_FAIL;

            LOG_ERR("Failed to move to next block: %zu", new_offset);

            err = rsp->status;
            goto cancel_and_free;
        }
        else if (new_offset == 0)
        {
            rsp->data = payload;
            rsp->len = payload_len;
            rsp->off = cur_offset;
            rsp->total = req->block_ctx.total_size;
            rsp->is_last = true;
            rsp->user_data = req->user_data;

            LOG_DBG("Blockwise transfer is finished!");

            err = GOLIOTH_OK;
            goto cancel_and_free;
        }
        else
        {
            rsp->data = payload;
            rsp->len = payload_len;
            rsp->off = cur_offset;
            rsp->total = req->block_ctx.total_size;
            rsp->is_last = false;
            rsp->user_data = req->user_data;
            rsp->status = req->is_observe ? GOLIOTH_ERR_NOT_IMPLEMENTED : GOLIOTH_OK;

            if (req->is_observe)
            {
                LOG_ERR("TODO: blockwise observe is not supported");
            }

            err = rsp->status;
            goto cancel_and_free;
        }
    }
    else
    {
        if (coap_get_option_int(response, COAP_OPTION_BLOCK1) >= 0)
        {
            /* This response has block1 */
            if (coap_update_from_block(response, &req->block_ctx) == 0)
            {
                golioth_coap_request_msg_t *rmsg = req->user_data;

                if (req->block_ctx.block_size < rmsg->post_block.block_szx)
                {

                    LOG_DBG("Server wants blocksize: %i intead of: %i",
                            coap_block_size_to_bytes(req->block_ctx.block_size),
                            coap_block_size_to_bytes(rmsg->post_block.block_szx));

                    rmsg->post_block.block_szx = req->block_ctx.block_size;
                }
            }
        }

        rsp->data = payload;
        rsp->len = payload_len;
        rsp->off = 0;

        /* Is it the same as 'req->block_ctx.total_size' ? */
        rsp->total = payload_len;

        rsp->is_last = true;
        rsp->user_data = req->user_data;

        err = GOLIOTH_OK;
        goto cancel_and_free;
    }

cancel_and_free:
    if (req->is_observe && !err)
    {
        req->is_pending = false;
        run_callback_and_remove = false;
        return GOLIOTH_OK;
    }

    return err;
}

void golioth_coap_req_process_rx(struct golioth_client *client, const struct coap_packet *rx)
{
    uint8_t rx_token[COAP_TOKEN_MAX_LEN];
    uint16_t rx_id;
    uint8_t rx_tkl;
    int observe_seq;

    rx_id = coap_header_get_id(rx);
    rx_tkl = coap_header_get_token(rx, rx_token);
    observe_seq = coap_get_option_int(rx, COAP_OPTION_OBSERVE);

    golioth_request_list_process_response(client, rx, rx_id, rx_token, rx_tkl, observe_seq);
}

/**
 * @brief Default response handler
 *
 * Default response handler, which generates error logs in case of errors and debug logs in case of
 * success.
 *
 * @param rsp Response information
 *
 * @retval 0 In every case
 */
static int golioth_req_rsp_default_handler(struct golioth_req_rsp *rsp)
{
    const char *info = rsp->user_data;

    if (rsp->status)
    {
        char coap_ret_code[16] = {0};
        if (rsp->status == GOLIOTH_ERR_COAP_RESPONSE)
        {
            snprintk(coap_ret_code,
                     sizeof(coap_ret_code),
                     "CoAP: %u.%02u",
                     rsp->coap_rsp_code.code_class,
                     rsp->coap_rsp_code.code_detail);
        }

        LOG_ERR("Error response (%s): %d %s", info ? info : "app", rsp->status, coap_ret_code);
        return 0;
    }

    LOG_HEXDUMP_DBG(rsp->data, rsp->len, info ? info : "RSP");

    return 0;
}

static enum coap_block_size max_block_size_from_payload_len(uint16_t payload_len)
{
    enum coap_block_size block_size = COAP_BLOCK_16;

    payload_len /= 16;

    while (payload_len > 1 && block_size < COAP_BLOCK_1024)
    {
        block_size++;
        payload_len /= 2;
    }

    return block_size;
}

static enum coap_block_size golioth_estimated_coap_block_size(struct golioth_client *client)
{
    return max_block_size_from_payload_len(client->rx_buffer_len);
}

static int golioth_coap_req_init(struct golioth_coap_req *req,
                                 struct golioth_client *client,
                                 enum coap_method method,
                                 enum coap_msgtype msg_type,
                                 uint8_t *buffer,
                                 size_t buffer_len,
                                 golioth_req_cb_t cb,
                                 void *user_data)
{
    int err;

    err = coap_packet_init(&req->request,
                           buffer,
                           buffer_len,
                           COAP_VERSION_1,
                           msg_type,
                           COAP_TOKEN_MAX_LEN,
                           coap_next_token(),
                           method,
                           coap_next_id());
    if (err)
    {
        return err;
    }

    req->client = client;
    req->cb = (cb ? cb : golioth_req_rsp_default_handler);
    req->user_data = user_data;
    req->request_wo_block2.data = NULL;
    req->request_wo_block1.data = NULL;
    req->reply.seq = 0;
    req->reply.ts = -COAP_OBSERVE_TS_DIFF_NEWER;

    coap_block_transfer_init(&req->block_ctx, golioth_estimated_coap_block_size(client), 0);

    return 0;
}

int golioth_coap_req_schedule(struct golioth_coap_req *req)
{
    struct golioth_client *client = req->client;
    int err;

    golioth_coap_pending_init(&req->pending, 3);

    err = golioth_coap_req_submit(req);
    if (err)
    {
        return err;
    }

    if (client->wakeup)
    {
        client->wakeup(client);
    }

    return 0;
}

int golioth_coap_req_new(struct golioth_coap_req **req,
                         struct golioth_client *client,
                         enum coap_method method,
                         enum coap_msgtype msg_type,
                         size_t buffer_len,
                         golioth_req_cb_t cb,
                         void *user_data)
{
    uint8_t *buffer;
    int err;

    *req = calloc(1, sizeof(**req));
    if (!(*req))
    {
        LOG_ERR("Failed to allocate request");
        return -ENOMEM;
    }

    buffer = malloc(buffer_len);
    if (!buffer)
    {
        LOG_ERR("Failed to allocate packet buffer");
        err = -ENOMEM;
        goto free_req;
    }

    err = golioth_coap_req_init(*req, client, method, msg_type, buffer, buffer_len, cb, user_data);
    if (err)
    {
        LOG_ERR("Failed to initialize CoAP GET request: %d", err);
        goto free_buffer;
    }

    return 0;

free_buffer:
    free(buffer);

free_req:
    free(*req);

    return err;
}

void golioth_coap_req_free(struct golioth_coap_req *req)
{
    free(req->request.data); /* buffer */
    free(req);
}

int golioth_coap_req_cb(struct golioth_client *client,
                        enum coap_method method,
                        const uint8_t **pathv,
                        enum coap_content_format format,
                        const uint8_t *data,
                        size_t data_len,
                        golioth_req_cb_t cb,
                        void *user_data,
                        int flags)
{
    size_t path_len = coap_pathv_estimate_alloc_len(pathv);
    struct golioth_coap_req *req;
    int err;

    err = golioth_coap_req_new(&req,
                               client,
                               method,
                               COAP_TYPE_CON,
                               GOLIOTH_COAP_MAX_NON_PAYLOAD_LEN + path_len + data_len,
                               cb,
                               user_data);
    if (err)
    {
        LOG_ERR("Failed to create new CoAP GET request: %d", err);
        return err;
    }

    if (method == COAP_METHOD_GET && (flags & GOLIOTH_COAP_REQ_OBSERVE))
    {
        req->is_observe = true;
        req->is_pending = true;

        err = coap_append_option_int(&req->request, COAP_OPTION_OBSERVE, 0 /* register */);
        if (err)
        {
            LOG_ERR("Unable add observe option");
            goto free_req;
        }
    }

    err = coap_packet_append_uri_path_from_pathv(&req->request, pathv);
    if (err)
    {
        LOG_ERR("Unable add uri path to packet");
        goto free_req;
    }

    if (method != COAP_METHOD_GET && method != COAP_METHOD_DELETE)
    {
        err = coap_append_option_int(&req->request, COAP_OPTION_CONTENT_FORMAT, format);
        if (err)
        {
            LOG_ERR("Unable add content format to packet");
            goto free_req;
        }
    }

    if (!(flags & GOLIOTH_COAP_REQ_NO_RESP_BODY))
    {
        err = coap_append_option_int(&req->request, COAP_OPTION_ACCEPT, format);
        if (err)
        {
            LOG_ERR("Unable add content format to packet");
            goto free_req;
        }
    }

    if (data && data_len)
    {
        err = coap_packet_append_payload_marker(&req->request);
        if (err)
        {
            LOG_ERR("Unable add payload marker to packet");
            goto free_req;
        }

        err = coap_packet_append_payload(&req->request, data, data_len);
        if (err)
        {
            LOG_ERR("Unable add payload to packet");
            goto free_req;
        }
    }

    err = golioth_coap_req_schedule(req);
    if (err)
    {
        goto free_req;
    }

    return 0;

free_req:
    golioth_coap_req_free(req);

    return err;
}

uint32_t init_ack_timeout(void)
{
#if defined(CONFIG_COAP_RANDOMIZE_ACK_TIMEOUT)
    const uint32_t max_ack = CONFIG_COAP_INIT_ACK_TIMEOUT_MS * CONFIG_COAP_ACK_RANDOM_PERCENT / 100;
    const uint32_t min_ack = CONFIG_COAP_INIT_ACK_TIMEOUT_MS;

    /* Randomly generated initial ACK timeout
     * ACK_TIMEOUT < INIT_ACK_TIMEOUT < ACK_TIMEOUT * ACK_RANDOM_FACTOR
     * Ref: https://tools.ietf.org/html/rfc7252#section-4.8
     */
    return min_ack + (sys_rand32_get() % (max_ack - min_ack));
#else
    return CONFIG_COAP_INIT_ACK_TIMEOUT_MS;
#endif /* defined(CONFIG_COAP_RANDOMIZE_ACK_TIMEOUT) */
}

int golioth_coap_req_cancel_observation(struct golioth_coap_req *req)
{
    int err;
    uint8_t coap_token[COAP_TOKEN_MAX_LEN];
    size_t coap_token_len = coap_header_get_token(&req->request, coap_token);
    int coap_content_format = coap_get_option_int(&req->request, COAP_OPTION_ACCEPT);

    if (coap_token_len == 0)
    {
        LOG_ERR("Unable to get coap token from request. Got length: %d", coap_token_len);
        return GOLIOTH_ERR_NO_MORE_DATA;
    }

    if (coap_content_format < 0)
    {
        LOG_ERR("Unable to get coap content format from request: %d", coap_content_format);
        return GOLIOTH_ERR_INVALID_FORMAT;
    }

    golioth_coap_request_msg_t *req_msg = req->user_data;

    /* Enqueue an "eager release" request for this observation */
    err = golioth_coap_client_observe_release(req->client,
                                              req_msg->path_prefix,
                                              req_msg->path,
                                              coap_content_format,
                                              coap_token,
                                              coap_token_len,
                                              NULL);
    if (err)
    {
        LOG_ERR("Error encoding observe release request: %d", err);
    }

    return err;
}

void golioth_coap_reqs_on_connect(struct golioth_client *client)
{
    golioth_coap_reqs_connected_set(client, true);
}

void golioth_coap_reqs_on_disconnect(struct golioth_client *client)
{
    golioth_coap_reqs_connected_set(client, false);
    golioth_coap_reqs_cancel_all_with_reason(client, GOLIOTH_ERR_FAIL);
}
