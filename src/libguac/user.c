/*
 * Copyright (C) 2014 Glyptodon LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"

#include "client.h"
#include "encode-jpeg.h"
#include "encode-png.h"
#include "id.h"
#include "object.h"
#include "pool.h"
#include "protocol.h"
#include "socket.h"
#include "stream.h"
#include "timestamp.h"
#include "user.h"
#include "user-handlers.h"

#include <stdlib.h>
#include <string.h>

guac_user* guac_user_alloc() {

    guac_user* user = calloc(1, sizeof(guac_user));
    int i;

    /* Generate ID */
    user->user_id = guac_generate_id(GUAC_USER_ID_PREFIX);
    if (user->user_id == NULL) {
        free(user);
        return NULL;
    }

    user->last_received_timestamp = guac_timestamp_current();
    user->last_frame_duration = 0;
    user->processing_lag = 0;
    user->active = 1;

    /* Allocate stream pool */
    user->__stream_pool = guac_pool_alloc(0);

    /* Initialze streams */
    user->__input_streams = malloc(sizeof(guac_stream) * GUAC_USER_MAX_STREAMS);
    user->__output_streams = malloc(sizeof(guac_stream) * GUAC_USER_MAX_STREAMS);

    for (i=0; i<GUAC_USER_MAX_STREAMS; i++) {
        user->__input_streams[i].index = GUAC_USER_CLOSED_STREAM_INDEX;
        user->__output_streams[i].index = GUAC_USER_CLOSED_STREAM_INDEX;
    }

    /* Allocate object pool */
    user->__object_pool = guac_pool_alloc(0);

    /* Initialize objects */
    user->__objects = malloc(sizeof(guac_object) * GUAC_USER_MAX_OBJECTS);
    for (i=0; i<GUAC_USER_MAX_OBJECTS; i++)
        user->__objects[i].index = GUAC_USER_UNDEFINED_OBJECT_INDEX;

    return user;

}

void guac_user_free(guac_user* user) {

    /* Free streams */
    free(user->__input_streams);
    free(user->__output_streams);

    /* Free stream pool */
    guac_pool_free(user->__stream_pool);

    /* Free objects */
    free(user->__objects);

    /* Free object pool */
    guac_pool_free(user->__object_pool);

    /* Clean up user */
    free(user->user_id);
    free(user);

}

guac_stream* guac_user_alloc_stream(guac_user* user) {

    guac_stream* allocd_stream;
    int stream_index;

    /* Refuse to allocate beyond maximum */
    if (user->__stream_pool->active == GUAC_USER_MAX_STREAMS)
        return NULL;

    /* Allocate stream */
    stream_index = guac_pool_next_int(user->__stream_pool);

    /* Initialize stream */
    allocd_stream = &(user->__output_streams[stream_index]);
    allocd_stream->index = stream_index;
    allocd_stream->data = NULL;
    allocd_stream->ack_handler = NULL;
    allocd_stream->blob_handler = NULL;
    allocd_stream->end_handler = NULL;

    return allocd_stream;

}

void guac_user_free_stream(guac_user* user, guac_stream* stream) {

    /* Release index to pool */
    guac_pool_free_int(user->__stream_pool, stream->index);

    /* Mark stream as closed */
    stream->index = GUAC_USER_CLOSED_STREAM_INDEX;

}

guac_object* guac_user_alloc_object(guac_user* user) {

    guac_object* allocd_object;
    int object_index;

    /* Refuse to allocate beyond maximum */
    if (user->__object_pool->active == GUAC_USER_MAX_OBJECTS)
        return NULL;

    /* Allocate object */
    object_index = guac_pool_next_int(user->__object_pool);

    /* Initialize object */
    allocd_object = &(user->__objects[object_index]);
    allocd_object->index = object_index;
    allocd_object->data = NULL;
    allocd_object->get_handler = NULL;
    allocd_object->put_handler = NULL;

    return allocd_object;

}

void guac_user_free_object(guac_user* user, guac_object* object) {

    /* Release index to pool */
    guac_pool_free_int(user->__object_pool, object->index);

    /* Mark object as undefined */
    object->index = GUAC_USER_UNDEFINED_OBJECT_INDEX;

}

int guac_user_handle_instruction(guac_user* user, const char* opcode, int argc, char** argv) {

    /* For each defined instruction */
    __guac_instruction_handler_mapping* current = __guac_instruction_handler_map;
    while (current->opcode != NULL) {

        /* If recognized, call handler */
        if (strcmp(opcode, current->opcode) == 0)
            return current->handler(user, argc, argv);

        current++;
    }

    /* If unrecognized, ignore */
    return 0;

}

void guac_user_stop(guac_user* user) {
    user->active = 0;
}

void vguac_user_abort(guac_user* user, guac_protocol_status status,
        const char* format, va_list ap) {

    /* Only relevant if user is active */
    if (user->active) {

        /* Log detail of error */
        vguac_user_log(user, GUAC_LOG_ERROR, format, ap);

        /* Send error immediately, limit information given */
        guac_protocol_send_error(user->socket, "Aborted. See logs.", status);
        guac_socket_flush(user->socket);

        /* Stop user */
        guac_user_stop(user);

    }

}

void guac_user_abort(guac_user* user, guac_protocol_status status,
        const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_user_abort(user, status, format, args);

    va_end(args);

}

void vguac_user_log(guac_user* user, guac_client_log_level level,
        const char* format, va_list ap) {

    vguac_client_log(user->client, level, format, ap);

}

void guac_user_log(guac_user* user, guac_client_log_level level,
        const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_client_log(user->client, level, format, args);

    va_end(args);

}

void guac_user_stream_png(guac_user* user, guac_socket* socket,
        guac_composite_mode mode, const guac_layer* layer, int x, int y,
        cairo_surface_t* surface) {

    /* Allocate new stream for image */
    guac_stream* stream = guac_user_alloc_stream(user);

    /* Declare stream as containing image data */
    guac_protocol_send_img(socket, stream, mode, layer, "image/png", x, y);

    /* Write PNG data */
    guac_png_write(socket, stream, surface);

    /* Terminate stream */
    guac_protocol_send_end(socket, stream);

    /* Free allocated stream */
    guac_user_free_stream(user, stream);

}

void guac_user_stream_jpeg(guac_user* user, guac_socket* socket,
        guac_composite_mode mode, const guac_layer* layer, int x, int y,
        cairo_surface_t* surface, int quality) {

    /* Allocate new stream for image */
    guac_stream* stream = guac_user_alloc_stream(user);

    /* Declare stream as containing image data */
    guac_protocol_send_img(socket, stream, mode, layer, "image/jpeg", x, y);

    /* Write JPEG data */
    guac_jpeg_write(socket, stream, surface, quality);

    /* Terminate stream */
    guac_protocol_send_end(socket, stream);

    /* Free allocated stream */
    guac_user_free_stream(user, stream);

}
