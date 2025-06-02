/* 
 * Copyright (c) 2018-2025, Extrems' Corner.org
 * All rights reserved.
 */

#include <malloc.h>
#include <string.h>
#include <ctype.h>
#include <gccore.h>
#include <fcntl.h>
#include <zlib.h>
#include "state.h"
#include "stub.h"
#include "wiiload.h"

#define STUB_ADDR  0x80001000
#define STUB_STACK 0x80003000

static lwp_t thread = LWP_THREAD_NULL;

wiiload_state_t wiiload;

static bool wiiload_read_file(int fd, int size)
{
	void *buf = wiiload.task.buf;
	wiiload.task.buf    = NULL;
	wiiload.task.bufpos = 0;
	wiiload.task.buflen = size;
	buf = realloc(buf, size);

	if (!buf)
		goto fail;
	if (read(fd, buf, size) < size)
		goto fail;

	wiiload.task.buf = buf;
	return true;

fail:
	free(buf);
	return false;
}

static bool wiiload_read(int chn, int insize, int outsize)
{
	Byte inbuf[4096];
	z_stream zstream = {0};
	int ret, len = 0;

	void *buf = wiiload.task.buf;
	wiiload.task.buf    = NULL;
	wiiload.task.bufpos = 0;
	wiiload.task.buflen = outsize;
	buf = realloc(buf, outsize);

	if (!buf)
		goto fail;
	if (inflateInit(&zstream) < 0)
		goto fail;

	zstream.next_out  = buf;
	zstream.avail_out = outsize;

	while (len < insize) {
		ret = usb_recvbuffer_safe_ex(chn, inbuf, MIN(insize - len, sizeof(inbuf)), 65536);
		if (ret < 0) goto fail;
		else len += ret;

		zstream.next_in  = inbuf;
		zstream.avail_in = ret;
		ret = inflate(&zstream, Z_NO_FLUSH);
		wiiload.task.bufpos = zstream.total_out;
		if (ret < 0) goto fail;
	}

	inflateEnd(&zstream);

	wiiload.task.buf = buf;
	return true;

fail:
	inflateEnd(&zstream);

	free(buf);
	return false;
}

static bool wiiload_read_args(int chn, int size)
{
	void *arg = wiiload.task.arg;
	wiiload.task.arg    = NULL;
	wiiload.task.arglen = size;
	arg = realloc(arg, size);

	if (!arg)
		goto fail;
	if (usb_recvbuffer_safe_ex(chn, arg, size, 65536) < size)
		goto fail;

	wiiload.task.arg = arg;
	return true;

fail:
	free(arg);
	return false;
}

static bool is_type_tpl(void *buffer, int size)
{
	tpl_header_t *header = buffer;

	if (size < sizeof(*header))
		return false;
	if (header->version != 2142000)
		return false;
	if (header->count == 0)
		return false;
	if (header->size != sizeof(*header))
		return false;

	return true;
}

static bool is_type_gci(void *buffer, int size)
{
	gci_header_t *header = buffer;

	if (size < sizeof(*header))
		return false;
	if (size != sizeof(*header) + header->length * 8192)
		return false;
	if (header->length < 1 || header->length > 2043)
		return false;
	if (header->padding0 != 0xFF || header->padding1 != 0xFFFF)
		return false;
	if (header->icon_offset != -1 && header->icon_offset > 512)
		return false;
	if (header->comment_offset != -1 && header->comment_offset > 8128)
		return false;

	for (int i = 0; i < 4; i++)
		if (!isalnum(header->gamecode[i]))
			return false;

	for (int i = 0; i < 2; i++)
		if (!isalnum(header->company[i]))
			return false;

	return true;
}

static bool is_type_dol(void *buffer, int size)
{
	dol_header_t *header = buffer;

	if (size < sizeof(*header))
		return false;

	for (int i = 0; i < 7; i++)
		if (header->padding[i])
			return false;

	for (int i = 0; i < 7; i++) {
		if (header->text_size[i]) {
			if (header->text_offset[i] < sizeof(*header))
				return false;
			if ((header->text_address[i] & SYS_BASE_UNCACHED) != SYS_BASE_CACHED)
				return false;
		}
	}

	for (int i = 0; i < 11; i++) {
		if (header->data_size[i]) {
			if (header->data_offset[i] < sizeof(*header))
				return false;
			if ((header->data_address[i] & SYS_BASE_UNCACHED) != SYS_BASE_CACHED)
				return false;
		}
	}

	if (header->bss_size) {
		if ((header->bss_address & SYS_BASE_UNCACHED) != SYS_BASE_CACHED)
			return false;
	}

	if ((header->entrypoint & SYS_BASE_UNCACHED) != SYS_BASE_CACHED)
		return false;

	return true;
}

static bool handle_dev(int chn)
{
	wiiload_header_t header;

	if (usb_recvbuffer_safe_ex(chn, &header, sizeof(header), 65536) < sizeof(header))
		return false;
	if (header.magic != 'HAXX')
		return false;
	if (header.version != 5)
		return false;

	LWP_SetThreadPriority(thread, LWP_PRIO_NORMAL);

	wiiload.task.type = TYPE_NONE;

	if (wiiload_read(chn, header.deflate_size, header.inflate_size) &&
		wiiload_read_args(chn, header.args_size)) {

		if (is_type_tpl(wiiload.task.buf, wiiload.task.buflen)) {
			wiiload.task.type = TYPE_TPL;
		} else if (is_type_gci(wiiload.task.buf, wiiload.task.buflen)) {
			wiiload.task.type = TYPE_GCI;
		} else if (is_type_dol(wiiload.task.buf, wiiload.task.buflen)) {
			wiiload.task.type = TYPE_DOL;
			state.quit |= KEY_QUIT;
		}
	}

	LWP_SetThreadPriority(thread, LWP_PRIO_IDLE);

	return true;
}

static void *thread_func(void *arg)
{
	do {
		int chn = EXI_CHANNEL_1;
		handle_dev(chn);
		usb_flush(chn);
	} while (!state.quit);

	return NULL;
}

void WIILOADInit(void)
{
	LWP_CreateThread(&thread, thread_func, NULL, NULL, 0, LWP_PRIO_IDLE);
}

void WIILOADBusy(void) {
	thread_func(NULL);
}

void WIILOADLoad(void)
{
	LWP_JoinThread(thread, NULL);
	thread = LWP_THREAD_NULL;

	if (wiiload.task.type != TYPE_DOL)
		WIILOADReadFile("/AUTOEXEC.DOL");

	if (wiiload.task.type == TYPE_DOL) {
		memcpy((void *)STUB_ADDR, stub, stub_size);
		DCStoreRange((void *)STUB_ADDR, stub_size);

		SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
		SYS_SwitchFiber((intptr_t)wiiload.task.buf, wiiload.task.buflen,
		                (intptr_t)wiiload.task.arg, wiiload.task.arglen,
		                STUB_ADDR, STUB_STACK);
	}
}

bool WIILOADReadFile(const char *file)
{
	int fd = open(file, O_RDONLY);
	struct stat st;

	if (fd < 0)
		return false;
	if (fstat(fd, &st) < 0)
		return false;

	wiiload.task.type = TYPE_NONE;

	if (wiiload_read_file(fd, st.st_size))
		wiiload.task.type = TYPE_DOL;

	close(fd);
	return true;
}
