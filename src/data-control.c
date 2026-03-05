/*
 * Copyright (c) 2020 Scott Moreau
 * Copyright (c) 2020 - 2026 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <aml.h>
#include <neatvnc.h>

#include "data-control.h"
#include "vec.h"

static const char custom_mime_type_data[] = "wayvnc";

// To not overhelm clients with data, we limit the size of the clipboard to send.
// We could set this theoretically to 1 Mib (tested with 960000 bytes), but when using
// wnvcc with "--hide-cursor" the limit is by 65 KiB. I can't really say for sure why this is the case.
// For 1 MiB we get an error message from wayland protocols, but for 65 KiB nothing
static const long unsigned int MAX_CLIPBOARD_SIZE = 65000;

struct receive_context {
	struct nvnc* server;
	struct aml_handler* handler;
	LIST_ENTRY(receive_context) link;
	int fd;
	struct wv_vec buffer;
};

struct send_context {
	struct aml_handler* handler;
	LIST_ENTRY(send_context) link;
	int fd;
	char* data;
	size_t length;
	size_t index;
};

static void destroy_receive_context(struct receive_context* ctx)
{
	aml_stop(aml_get_default(), ctx->handler);
	aml_unref(ctx->handler);
	close(ctx->fd);
	wv_vec_destroy(&ctx->buffer);
	LIST_REMOVE(ctx, link);
	free(ctx);
}

static void destroy_send_context(struct send_context* ctx)
{
	aml_stop(aml_get_default(), ctx->handler);
	aml_unref(ctx->handler);

	close(ctx->fd);
	free(ctx->data);
	LIST_REMOVE(ctx, link);
	free(ctx);
}

static void on_receive(struct aml_handler* handler)
{
	struct receive_context* ctx = aml_get_userdata(handler);
	int fd = aml_get_fd(handler);
	assert(ctx->fd == fd);

	char buf[4096];

	ssize_t ret = read(fd, &buf, sizeof(buf));
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		nvnc_log(NVNC_LOG_ERROR, "Clipboard read failed: %m");
		destroy_receive_context(ctx);
	} else if (ret > 0) {
		wv_vec_append(&ctx->buffer, buf, ret);
		return;
	}

	if (ctx->buffer.len > MAX_CLIPBOARD_SIZE) {
		nvnc_log(NVNC_LOG_WARNING, "Not sending clipboard content because it exceedes the max size: %zu bytes", ctx->buffer.len);
	} else if (ctx->buffer.len != 0) {
		nvnc_send_cut_text(ctx->server, ctx->buffer.data, ctx->buffer.len);
	}

	wv_vec_clear(&ctx->buffer);
	destroy_receive_context(ctx);
}

static void on_send(struct aml_handler* handler)
{
	struct send_context* ctx = aml_get_userdata(handler);
	int fd = aml_get_fd(handler);
	assert(ctx->fd == fd);

	int ret;
	ret = write(fd, ctx->data + ctx->index, ctx->length - ctx->index);
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		nvnc_log(NVNC_LOG_ERROR, "Clipboard write failed/incomplete: %m");
		destroy_send_context(ctx);
	} else if (ret == (int)(ctx->length - ctx->index)) {
		destroy_send_context(ctx);
	} else {
		ctx->index += ret;
	}
}

static int dont_block(int fd)
{
	int ret = fcntl(fd, F_GETFL);
	if (ret == -1)
		return -1;
	return fcntl(fd, F_SETFL, ret | O_NONBLOCK);
}

static struct receive_context* receive_data_init(int pipe_fd[])
{
	if (pipe(pipe_fd) == -1) {
		nvnc_log(NVNC_LOG_ERROR, "pipe() failed: %m");
		return NULL;
	}

	if (dont_block(pipe_fd[0]) == -1) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to set O_NONBLOCK on clipbooard receive fd");
		goto failure;
	}

	struct receive_context* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		goto failure;
	}

	return ctx;
failure:
	close(pipe_fd[0]);
	close(pipe_fd[1]);
	return NULL;
}

static void receive_data_finish(struct data_control* self,
		struct receive_context* ctx, int pipe_fd[])
{
	close(pipe_fd[1]);

	ctx->fd = pipe_fd[0];
	ctx->server = self->server;
	if (wv_vec_init(&ctx->buffer, 4096) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "open_memstream() failed: %m");
		goto open_memstream_failure;
	}

	ctx->handler = aml_handler_new(ctx->fd, on_receive, ctx, NULL);
	if (!ctx->handler) {
		goto handler_failure;
	}

	if (aml_start(aml_get_default(), ctx->handler) < 0) {
		goto poll_start_failure;
	}

	LIST_INSERT_HEAD(&self->receive_contexts, ctx, link);
	return;

poll_start_failure:
	aml_unref(ctx->handler);
handler_failure:
	wv_vec_destroy(&ctx->buffer);
open_memstream_failure:
	free(ctx);
	close(pipe_fd[0]);
}

static void receive_data_wlr(void* data,
	struct zwlr_data_control_offer_v1* offer)
{
	struct data_control* self = data;
	int pipe_fd[2];

	struct receive_context *ctx = receive_data_init(pipe_fd);
	if (!ctx)
		return;

	zwlr_data_control_offer_v1_receive(offer, self->mime_type, pipe_fd[1]);

	receive_data_finish(self, ctx, pipe_fd);
}

static void data_control_offer_wlr(void* data,
	struct zwlr_data_control_offer_v1* zwlr_data_control_offer_v1,
	const char* mime_type)
{
	struct data_control* self = data;

	if (strcmp(mime_type, self->custom_mime_type_name) == 0) {
		self->is_own_offer = true;
		return;
	}

	if (self->wlr.offer)
		return;

	if (strcmp(mime_type, self->mime_type) == 0)
		self->wlr.offer = zwlr_data_control_offer_v1;
}

static struct
zwlr_data_control_offer_v1_listener data_control_offer_listener_wlr = {
	data_control_offer_wlr
};

static void data_control_device_offer_wlr(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	if (!id)
		return;

	zwlr_data_control_offer_v1_add_listener(id,
			&data_control_offer_listener_wlr, data);
}

static void data_control_device_selection_wlr(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;

	if (!id) {
		if (self->wlr.offer) {
			zwlr_data_control_offer_v1_destroy(self->wlr.offer);
			self->wlr.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (id == self->wlr.offer && !self->is_own_offer)
		receive_data_wlr(data, id);

	zwlr_data_control_offer_v1_destroy(id);
	self->wlr.offer = NULL;
	self->is_own_offer = false;
}

static void data_control_device_finished_wlr(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1)
{
	zwlr_data_control_device_v1_destroy(zwlr_data_control_device_v1);
}

static void data_control_device_primary_selection_wlr(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;

	if (!id) {
		if (self->wlr.offer) {
			zwlr_data_control_offer_v1_destroy(self->wlr.offer);
			self->wlr.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (id == self->wlr.offer && !self->is_own_offer)
		receive_data_wlr(data, id);

	zwlr_data_control_offer_v1_destroy(id);
	self->wlr.offer = NULL;
	self->is_own_offer = false;
}

static struct
zwlr_data_control_device_v1_listener data_control_device_listener_wlr = {
	.data_offer = data_control_device_offer_wlr,
	.selection = data_control_device_selection_wlr,
	.finished = data_control_device_finished_wlr,
	.primary_selection = data_control_device_primary_selection_wlr
};

static void data_control_source_send_common(void* data,
	const char* mime_type, int32_t fd)
{
	struct data_control* self = data;
	const char* d = self->cb_data;
	size_t len = self->cb_len;
	int ret;

	assert(d);
	assert(len);

	if (strcmp(mime_type, self->custom_mime_type_name) == 0) {
		d = custom_mime_type_data;
		len = strlen(custom_mime_type_data);
	}

	if (dont_block(fd) == -1) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to set O_NONBLOCK on clipbooard send fd");
		close(fd);
		return;
	}

	ret = write(fd, d, len);
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			ret = 0;
		} else {
			nvnc_log(NVNC_LOG_ERROR, "Clipboard write failed: %m");
			close(fd);
			return;
		}
	} else if (ret == (int)len) {
		close(fd);
		return;
	}

	/* we did a partial write, so continue sending data asynchronously */

	struct send_context* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		goto ctx_alloc_failure;
		return;
	}

	ctx->fd = fd;
	ctx->length = len - ret;
	ctx->index = 0;
	ctx->data = malloc(ctx->length);
	if (!ctx->data) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		goto ctx_data_alloc_failure;
	}
	memcpy(ctx->data, d + ret, ctx->length);

	ctx->handler = aml_handler_new(ctx->fd, on_send, ctx, NULL);
	if (!ctx->handler)
		goto handler_failure;

	aml_set_event_mask(ctx->handler, AML_EVENT_WRITE);

	if (aml_start(aml_get_default(), ctx->handler) < 0)
		goto poll_start_failure;

	LIST_INSERT_HEAD(&self->send_contexts, ctx, link);
	return;

poll_start_failure:
	aml_unref(ctx->handler);
handler_failure:
	free(ctx->data);
ctx_data_alloc_failure:
	free(ctx);
ctx_alloc_failure:
	close(fd);
	nvnc_log(NVNC_LOG_ERROR, "Clipboard write incomplete");
}

static void data_control_source_send_wlr(void* data,
	struct zwlr_data_control_source_v1* zwlr_data_control_source_v1,
	const char* mime_type, int32_t fd)
{
	data_control_source_send_common(data, mime_type, fd);
}

static void data_control_source_cancelled_wlr(void* data,
	struct zwlr_data_control_source_v1* zwlr_data_control_source_v1)
{
	struct data_control* self = data;

	if (self->wlr.selection == zwlr_data_control_source_v1) {
		self->wlr.selection = NULL;
	}
	if (self->wlr.primary_selection == zwlr_data_control_source_v1) {
		self->wlr.primary_selection = NULL;
	}
	zwlr_data_control_source_v1_destroy(zwlr_data_control_source_v1);
}

static struct zwlr_data_control_source_v1_listener
data_control_source_listener_wlr = {
	.send = data_control_source_send_wlr,
	.cancelled = data_control_source_cancelled_wlr
};

static struct zwlr_data_control_source_v1* set_selection_wlr(
		struct data_control* self, bool primary) {
	struct zwlr_data_control_source_v1* selection;
	selection = zwlr_data_control_manager_v1_create_data_source(
			self->wlr_manager);
	if (selection == NULL) {
		nvnc_log(NVNC_LOG_ERROR, "zwlr_data_control_manager_v1_create_data_source() failed");
		free(self->cb_data);
		self->cb_data = NULL;
		return NULL;
	}

	zwlr_data_control_source_v1_add_listener(selection,
			&data_control_source_listener_wlr, self);
	zwlr_data_control_source_v1_offer(selection, self->mime_type);
	zwlr_data_control_source_v1_offer(selection, self->custom_mime_type_name);

	if (primary)
		zwlr_data_control_device_v1_set_primary_selection(
				self->wlr.device, selection);
	else
		zwlr_data_control_device_v1_set_selection(
				self->wlr.device, selection);

	return selection;
}

static void receive_data_ext(void* data,
	struct ext_data_control_offer_v1* offer)
{
	struct data_control* self = data;
	int pipe_fd[2];

	struct receive_context *ctx = receive_data_init(pipe_fd);
	if (!ctx)
		return;

	ext_data_control_offer_v1_receive(offer, self->mime_type, pipe_fd[1]);

	receive_data_finish(self, ctx, pipe_fd);
}

static void data_control_offer_ext(void* data,
	struct ext_data_control_offer_v1* ext_data_control_offer_v1,
	const char* mime_type)
{
	struct data_control* self = data;

	if (strcmp(mime_type, self->custom_mime_type_name) == 0) {
		self->is_own_offer = true;
		return;
	}

	if (self->ext.offer)
		return;

	if (strcmp(mime_type, self->mime_type) == 0)
		self->ext.offer = ext_data_control_offer_v1;
}

static struct
ext_data_control_offer_v1_listener data_control_offer_listener_ext = {
	data_control_offer_ext
};

static void data_control_device_offer_ext(void* data,
	struct ext_data_control_device_v1* ext_data_control_device_v1,
	struct ext_data_control_offer_v1* id)
{
	if (!id)
		return;

	ext_data_control_offer_v1_add_listener(id,
			&data_control_offer_listener_ext, data);
}

static void data_control_device_selection_ext(void* data,
	struct ext_data_control_device_v1* ext_data_control_device_v1,
	struct ext_data_control_offer_v1* id)
{
	struct data_control* self = data;

	if (!id) {
		if (self->ext.offer) {
			ext_data_control_offer_v1_destroy(self->ext.offer);
			self->ext.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (id == self->ext.offer && !self->is_own_offer)
		receive_data_ext(data, id);

	ext_data_control_offer_v1_destroy(id);
	self->ext.offer = NULL;
	self->is_own_offer = false;
}

static void data_control_device_finished_ext(void* data,
	struct ext_data_control_device_v1* ext_data_control_device_v1)
{
	ext_data_control_device_v1_destroy(ext_data_control_device_v1);
}

static void data_control_device_primary_selection_ext(void* data,
	struct ext_data_control_device_v1* ext_data_control_device_v1,
	struct ext_data_control_offer_v1* id)
{
	struct data_control* self = data;

	if (!id) {
		if (self->ext.offer) {
			ext_data_control_offer_v1_destroy(self->ext.offer);
			self->ext.offer = NULL;
			self->is_own_offer = false;
		}
		return;
	}

	if (id == self->ext.offer && !self->is_own_offer)
		receive_data_ext(data, id);

	ext_data_control_offer_v1_destroy(id);
	self->ext.offer = NULL;
	self->is_own_offer = false;
}

static struct
ext_data_control_device_v1_listener data_control_device_listener_ext = {
	.data_offer = data_control_device_offer_ext,
	.selection = data_control_device_selection_ext,
	.finished = data_control_device_finished_ext,
	.primary_selection = data_control_device_primary_selection_ext
};

static void
data_control_source_send_ext(void* data,
	struct ext_data_control_source_v1* ext_data_control_source_v1,
	const char* mime_type,
	int32_t fd)
{
	data_control_source_send_common(data, mime_type, fd);
}

static void data_control_source_cancelled_ext(void* data,
	struct ext_data_control_source_v1* ext_data_control_source_v1)
{
	struct data_control* self = data;

	if (self->ext.selection == ext_data_control_source_v1) {
		self->ext.selection = NULL;
	}
	if (self->ext.primary_selection == ext_data_control_source_v1) {
		self->ext.primary_selection = NULL;
	}
	ext_data_control_source_v1_destroy(ext_data_control_source_v1);
}

static struct ext_data_control_source_v1_listener
data_control_source_listener_ext = {
	.send = data_control_source_send_ext,
	.cancelled = data_control_source_cancelled_ext
};

static struct ext_data_control_source_v1* set_selection_ext(
		struct data_control* self, bool primary) {
	struct ext_data_control_source_v1* selection;
	selection = ext_data_control_manager_v1_create_data_source(
			self->ext_manager);
	if (selection == NULL) {
		nvnc_log(NVNC_LOG_ERROR, "ext_data_control_manager_v1_create_data_source() failed");
		free(self->cb_data);
		self->cb_data = NULL;
		return NULL;
	}

	ext_data_control_source_v1_add_listener(selection,
			&data_control_source_listener_ext, self);
	ext_data_control_source_v1_offer(selection, self->mime_type);
	ext_data_control_source_v1_offer(selection, self->custom_mime_type_name);

	if (primary)
		ext_data_control_device_v1_set_primary_selection(
				self->ext.device, selection);
	else
		ext_data_control_device_v1_set_selection(
				self->ext.device, selection);

	return selection;
}

void data_control_init(struct data_control* self, struct nvnc* server,
		struct wl_seat* seat)
{
	self->server = server;
	LIST_INIT(&self->receive_contexts);
	LIST_INIT(&self->send_contexts);

	if (self->wlr_manager) {
		self->wlr.device = zwlr_data_control_manager_v1_get_data_device(
				self->wlr_manager, seat);
		zwlr_data_control_device_v1_add_listener(self->wlr.device,
				&data_control_device_listener_wlr, self);
	} else if (self->ext_manager) {
		self->ext.device = ext_data_control_manager_v1_get_data_device(
				self->ext_manager, seat);
		ext_data_control_device_v1_add_listener(self->ext.device,
				&data_control_device_listener_ext, self);
	} else {
		nvnc_log(NVNC_LOG_PANIC, "No data control manager available");
	}

	self->ext.selection = NULL;
	self->ext.primary_selection = NULL;
	self->ext.offer = NULL;
	self->is_own_offer = false;
	self->cb_data = NULL;
	self->cb_len = 0;
	self->mime_type = "text/plain;charset=utf-8";
	snprintf(self->custom_mime_type_name,
			sizeof(self->custom_mime_type_name),
			"x-wayvnc-client-%08x", (unsigned int)rand());
}

void data_control_destroy(struct data_control* self)
{
	while (!LIST_EMPTY(&self->receive_contexts))
		destroy_receive_context(LIST_FIRST(&self->receive_contexts));
	while (!LIST_EMPTY(&self->send_contexts)) {
		nvnc_log(NVNC_LOG_ERROR, "Clipboard write incomplete due to client disconnection");
		destroy_send_context(LIST_FIRST(&self->send_contexts));
	}
	if (self->wlr_manager) {
		if (self->wlr.selection) {
			zwlr_data_control_source_v1_destroy(self->wlr.selection);
			self->wlr.selection = NULL;
		}
		if (self->wlr.primary_selection) {
			zwlr_data_control_source_v1_destroy(
					self->wlr.primary_selection);
			self->wlr.primary_selection = NULL;
		}
		zwlr_data_control_device_v1_destroy(self->wlr.device);
	} else if (self->ext_manager) {
		if (self->ext.selection) {
			ext_data_control_source_v1_destroy(self->ext.selection);
			self->ext.selection = NULL;
		}
		if (self->ext.primary_selection) {
			ext_data_control_source_v1_destroy(
					self->ext.primary_selection);
			self->ext.primary_selection = NULL;
		}
		ext_data_control_device_v1_destroy(self->ext.device);
	}
	free(self->cb_data);
}

void data_control_to_clipboard(struct data_control* self, const char* text, size_t len)
{
	if (!len) {
		nvnc_log(NVNC_LOG_DEBUG, "Ignoring empty clipboard from VNC client");
		return;
	}
	free(self->cb_data);

	self->cb_data = malloc(len);
	if (!self->cb_data) {
		nvnc_log(NVNC_LOG_ERROR, "OOM: %m");
		return;
	}

	memcpy(self->cb_data, text, len);
	self->cb_len = len;
	if (self->wlr_manager) {
		// Set copy/paste buffer
		self->wlr.selection = set_selection_wlr(self, false);
		// Set highlight/middle_click buffer
		self->wlr.primary_selection = set_selection_wlr(self, true);
	} else if (self->ext_manager) {
		// Set copy/paste buffer
		self->ext.selection = set_selection_ext(self, false);
		// Set highlight/middle_click buffer
		self->ext.primary_selection = set_selection_ext(self, true);
	}
}
