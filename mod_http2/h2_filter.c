/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_connection.h>
#include <scoreboard.h>

#include "h2_private.h"
#include "h2_conn_io.h"
#include "h2_util.h"

#include "h2_filter.h"

#define UNSET       -1
#define H2MIN(x,y) ((x) < (y) ? (x) : (y))

static apr_status_t consume_brigade(h2_filter_cin *cin, 
                                    apr_bucket_brigade *bb, 
                                    apr_read_type_e block)
{
    apr_status_t status = APR_SUCCESS;
    apr_size_t readlen = 0;
    
    while (status == APR_SUCCESS && !APR_BRIGADE_EMPTY(bb)) {
        
        apr_bucket* bucket = APR_BRIGADE_FIRST(bb);
        if (APR_BUCKET_IS_METADATA(bucket)) {
            /* we do nothing regarding any meta here */
        }
        else {
            const char *bucket_data = NULL;
            apr_size_t bucket_length = 0;
            status = apr_bucket_read(bucket, &bucket_data,
                                     &bucket_length, block);
            
            if (status == APR_SUCCESS && bucket_length > 0) {
                apr_size_t consumed = 0;

                status = cin->cb(cin->cb_ctx, bucket_data, bucket_length, &consumed);
                if (status == APR_SUCCESS && bucket_length > consumed) {
                    /* We have data left in the bucket. Split it. */
                    status = apr_bucket_split(bucket, consumed);
                }
                readlen += consumed;
                cin->start_read = apr_time_now();
            }
        }
        apr_bucket_delete(bucket);
    }
    
    if (readlen == 0 && status == APR_SUCCESS && block == APR_NONBLOCK_READ) {
        return APR_EAGAIN;
    }
    return status;
}

h2_filter_cin *h2_filter_cin_create(apr_pool_t *p, h2_filter_cin_cb *cb, void *ctx)
{
    h2_filter_cin *cin;
    
    cin = apr_pcalloc(p, sizeof(*cin));
    cin->pool      = p;
    cin->cb        = cb;
    cin->cb_ctx    = ctx;
    cin->start_read = UNSET;
    return cin;
}

void h2_filter_cin_timeout_set(h2_filter_cin *cin, int timeout_secs)
{
    cin->timeout_secs = timeout_secs;
}

apr_status_t h2_filter_core_input(ap_filter_t* f,
                                  apr_bucket_brigade* brigade,
                                  ap_input_mode_t mode,
                                  apr_read_type_e block,
                                  apr_off_t readbytes) 
{
    h2_filter_cin *cin = f->ctx;
    apr_status_t status = APR_SUCCESS;
    apr_time_t saved_timeout = UNSET;
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, f->c,
                  "core_input(%ld): read, %s, mode=%d, readbytes=%ld, timeout=%d", 
                  (long)f->c->id, (block == APR_BLOCK_READ)? "BLOCK_READ" : "NONBLOCK_READ", 
                  mode, (long)readbytes, cin->timeout_secs);
    
    if (mode == AP_MODE_INIT || mode == AP_MODE_SPECULATIVE) {
        return ap_get_brigade(f->next, brigade, mode, block, readbytes);
    }
    
    if (mode != AP_MODE_READBYTES) {
        return (block == APR_BLOCK_READ)? APR_SUCCESS : APR_EAGAIN;
    }
    
    if (!cin->bb) {
        cin->bb = apr_brigade_create(cin->pool, f->c->bucket_alloc);
    }

    if (!cin->socket) {
        cin->socket = ap_get_conn_socket(f->c);
    }
    
    cin->start_read = apr_time_now();
    if (APR_BRIGADE_EMPTY(cin->bb)) {
        /* We only do a blocking read when we have no streams to process. So,
         * in httpd scoreboard lingo, we are in a KEEPALIVE connection state.
         * When reading non-blocking, we do have streams to process and update
         * child with NULL request. That way, any current request information
         * in the scoreboard is preserved.
         */
        if (block == APR_BLOCK_READ) {
            if (cin->timeout_secs > 0) {
                apr_time_t t = apr_time_from_sec(cin->timeout_secs);
                apr_socket_timeout_get(cin->socket, &saved_timeout);
                apr_socket_timeout_set(cin->socket, H2MIN(t, saved_timeout));
            }
        }
        ap_update_child_status_from_conn(f->c->sbh, SERVER_BUSY_READ, f->c);
        status = ap_get_brigade(f->next, cin->bb, AP_MODE_READBYTES,
                                block, readbytes);
        if (saved_timeout != UNSET) {
            apr_socket_timeout_set(cin->socket, saved_timeout);
        }
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, status, f->c,
                      "core_input(%ld): got_brigade", (long)f->c->id);
    }
    
    switch (status) {
        case APR_SUCCESS:
            status = consume_brigade(cin, cin->bb, block);
            break;
        case APR_EOF:
        case APR_EAGAIN:
        case APR_TIMEUP:
            break;
        default:
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, f->c,
                          "h2_conn_io: error reading");
            break;
    }
    return status;
}
