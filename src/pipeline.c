#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <termbox.h>
#include "config.h"
#include "subprocess.h"
#include "email/headers.h"
#include "util/unicode.h"
#include "util/list.h"
#include "state.h"
#include "worker.h"
#include "pipeline.h"
#include "log.h"
#include "ui.h"

// TODO: Global
static struct subprocess *header_subp;

static int header_cmp(const void *_a, const void *_b) {
	const char *a = _a;
	const char *b = _b;
	return strcmp(a, b);
}

static void add_header(void *_header) {
	struct email_header *header = _header;
	if (list_seq_find(config->ui.show_headers, header_cmp, header->key) == -1) {
		return;
	}
	int len = snprintf(NULL, 0, "%s: %s\n", header->key, header->value);
	char *h = malloc(len + 1);
	snprintf(h, len + 1, "%s: %s\n", header->key, header->value);
	subprocess_queue_stdin(header_subp, (uint8_t *)h, len);
}

struct pipeline_state {
	struct account_state *account;
	struct aerc_message *msg;
	struct aerc_message_part *part;
};

static void subp_complete(struct subprocess *subp) {
	worker_log(L_DEBUG, "Message preprocessing complete, launching viewer");
	struct pipeline_state *state = subp->user;
	struct io_capture *capture = subp->io_stdout;

	char *argv[] = { "sh", "-c", config->viewer.pager, NULL };
	subp = subprocess_init(argv, true);
	header_subp = subp;
	list_foreach(state->msg->headers, add_header);
	if (capture) {
		unsigned char *data = malloc(capture->len);
		memcpy(data, capture->data, capture->len);
		subprocess_queue_stdin(subp, data, capture->len);
	}
	state->account->viewer.term = subp;
	subprocess_start(subp);
	request_rerender(PANEL_MESSAGE_VIEW);

	free(state);
}

static void spawn_subprocess(struct account_state *account,
		struct aerc_message *msg, struct aerc_message_part *part) {
	worker_log(L_DEBUG, "Preprocessing message part %p (%s/%s)",
			part, part->type, part->subtype);
	struct geometry geo;
	message_view_geometry(&geo);
	char width[20] = { 0 }, height[20] = { 0 }, mimetype[64];
	snprintf(width, sizeof(width) - 1, "%d", geo.width);
	snprintf(height, sizeof(height) - 1, "%d", geo.height);
	snprintf(mimetype, sizeof(mimetype) - 1, "%s/%s", part->type, part->subtype);
	setenv("WIDTH", width, 1);
	setenv("HEIGHT", height, 1);
	setenv("MIMETYPE", mimetype, 1);

	struct pipeline_state *state = calloc(1, sizeof(struct pipeline_state));
	state->account = account;
	state->msg = msg;
	state->part = part;

	// Strip non-printable characters from message
	// TODO: this actually mutates the "official" copy of the message in memory
	// maybe we want to copy it somewhere else first or translate it on the fly
	// or something
	char *data = (char *)part->content;
	int i = 0;
	while (i < part->size) {
		uint32_t ch = utf8_decode((const char **)&data);
		int usize = utf8_chsize(ch);
		i += usize;
		if ((ch <= 0x1F && ch != '\n' && ch != '\r') || ch == 0x7F || ch == 0x200B) {
			// Delete these characters
			memmove(&data[-usize], data, part->size - i);
			part->size -= usize - 1;
			data -= usize - 1;
		}
	}

	char *argv[] = { "sh", "-c", "cat", NULL };
	for (size_t i = 0; i < config->viewer.mime_handlers->length; ++i) {
		struct mime_handler *handler = config->viewer.mime_handlers->items[i];
		if (strcmp(part->type, handler->mime.type) == 0) {
			if (strcmp(handler->mime.subtype, "*") == 0 ||
					strcmp(handler->mime.subtype, part->subtype) == 0) {
				argv[2] = handler->command;
				break;
			}
		}
	}

	struct subprocess *subp = subprocess_init(argv, false);
	subp->user = state;
	subp->complete = subp_complete;
	if (part->size == 0) {
		// Don't actually run preprocessor on empty input
		subp_complete(subp);
		return;
	}
	subprocess_queue_stdin(subp, part->content, part->size);
	subprocess_capture_stdout(subp);
	subprocess_capture_stderr(subp);

	list_add(account->viewer.processes, subp);
	subprocess_start(subp);
}

void spawn_email_handler(struct account_state *account,
		struct aerc_message *msg) {
	struct aerc_message_part *part = NULL;
	for (size_t i = 0; i < config->viewer.alternatives->length; ++i) {
		struct mimetype *mime = config->viewer.alternatives->items[i];
		for (size_t j = 0; j < msg->parts->length; ++j) {
			struct aerc_message_part *_part = msg->parts->items[j];
			if (strcmp(_part->type, mime->type) == 0) {
				if (strcmp(mime->subtype, "*") == 0 ||
						strcmp(mime->subtype, _part->type) == 0) {
					part = _part;
					break;
				}
			}
		}
	}
	if (!part) {
		for (size_t i = 0; i < msg->parts->length; ++i) {
			part = msg->parts->items[i];
			if (strcasecmp(part->type, "text") == 0) {
				spawn_subprocess(account, msg, part);
				break;
			} 
		}
		return;
	}
	spawn_subprocess(account, msg, part);
}
