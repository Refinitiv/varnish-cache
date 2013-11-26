/*-
 * Copyright (c) 2013 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Interaction with the linvgz (zlib) library.
 *
 * The zlib library pollutes namespace a LOT when you include the "vgz.h"
 * (aka (zlib.h") file so we contain the damage by vectoring all access
 * to libz through this source file.
 *
 * The API defined by this file, will also insulate the rest of the code,
 * should we find a better gzip library at a later date.
 *
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache.h"

#include "vgz.h"

struct vgz {
	unsigned		magic;
#define VGZ_MAGIC		0x162df0cb
	enum {VGZ_GZ,VGZ_UN}	dir;
	struct vsl_log		*vsl;
	const char		*id;
	struct ws		*tmp;
	char			*tmp_snapshot;
	int			last_i;

	/* Wrw stuff */
	char			*m_buf;
	ssize_t			m_sz;
	ssize_t			m_len;

	z_stream		vz;
};

/*--------------------------------------------------------------------
 * Set up a gunzip instance
 */

static struct vgz *
vgz_alloc_vgz(struct vsl_log *vsl, const char *id)
{
	struct vgz *vg;

	ALLOC_OBJ(vg, VGZ_MAGIC);
	AN(vg);
	vg->vsl = vsl;
	vg->id = id;
	return (vg);
}

struct vgz *
VGZ_NewUngzip(struct vsl_log *vsl, const char *id)
{
	struct vgz *vg;

	vg = vgz_alloc_vgz(vsl, id);
	vg->dir = VGZ_UN;
	VSC_C_main->n_gunzip++;

	/*
	 * Max memory usage according to zonf.h:
	 *	mem_needed = "a few kb" + (1 << (windowBits))
	 * Since we don't control windowBits, we have to assume
	 * it is 15, so 34-35KB or so.
	 */
	assert(Z_OK == inflateInit2(&vg->vz, 31));
	return (vg);
}

struct vgz *
VGZ_NewGzip(struct vsl_log *vsl, const char *id)
{
	struct vgz *vg;
	int i;

	vg = vgz_alloc_vgz(vsl, id);
	vg->dir = VGZ_GZ;
	VSC_C_main->n_gzip++;

	/*
	 * From zconf.h:
	 *
	 *	mem_needed = "a few kb"
	 *		+ (1 << (windowBits+2))
	 *		+  (1 << (memLevel+9))
	 *
	 * windowBits [8..15] (-> 1K..128K)
	 * memLevel [1..9] (-> 1K->256K)
	 *
	 * XXX: They probably needs to be params...
	 *
	 * XXX: It may be more efficent to malloc them, rather than have
	 * XXX: too many worker threads grow the stacks.
	 */
	i = deflateInit2(&vg->vz,
	    cache_param->gzip_level,		/* Level */
	    Z_DEFLATED,				/* Method */
	    16 + 15,				/* Window bits (16=gzip) */
	    cache_param->gzip_memlevel,		/* memLevel */
	    Z_DEFAULT_STRATEGY);
	assert(Z_OK == i);
	return (vg);
}

/*--------------------------------------------------------------------
 */

static int
vgz_getmbuf(struct vgz *vg)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AZ(vg->m_sz);
	AZ(vg->m_len);
	AZ(vg->m_buf);

	vg->m_sz = cache_param->gzip_buffer;
	vg->m_buf = malloc(vg->m_sz);
	if (vg->m_buf == NULL) {
		vg->m_sz = 0;
		return (-1);
	}
	return (0);
}

/*--------------------------------------------------------------------*/

void
VGZ_Ibuf(struct vgz *vg, const void *ptr, ssize_t len)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	AZ(vg->vz.avail_in);
	vg->vz.next_in = TRUST_ME(ptr);
	vg->vz.avail_in = len;
}

int
VGZ_IbufEmpty(const struct vgz *vg)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	return (vg->vz.avail_in == 0);
}

/*--------------------------------------------------------------------*/

void
VGZ_Obuf(struct vgz *vg, void *ptr, ssize_t len)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	vg->vz.next_out = TRUST_ME(ptr);
	vg->vz.avail_out = len;
}

int
VGZ_ObufFull(const struct vgz *vg)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	return (vg->vz.avail_out == 0);
}

/*--------------------------------------------------------------------
 * Keep the outbuffer supplied with storage
 */

int
VGZ_ObufStorage(struct busyobj *bo, struct vgz *vg)
{
	struct storage *st;

	st = VFP_GetStorage(bo, 0);
	if (st == NULL)
		return (-1);

	VGZ_Obuf(vg, st->ptr + st->len, st->space - st->len);

	return (0);
}

/*--------------------------------------------------------------------*/

enum vgzret_e
VGZ_Gunzip(struct vgz *vg, const void **pptr, size_t *plen)
{
	int i;
	ssize_t l;
	const uint8_t *before;

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	*pptr = NULL;
	*plen = 0;
	AN(vg->vz.next_out);
	AN(vg->vz.avail_out);
	before = vg->vz.next_out;
	i = inflate(&vg->vz, 0);
	if (i == Z_OK || i == Z_STREAM_END) {
		*pptr = before;
		l = (const uint8_t *)vg->vz.next_out - before;
		*plen = l;
	}
	vg->last_i = i;
	if (i == Z_OK)
		return (VGZ_OK);
	if (i == Z_STREAM_END)
		return (VGZ_END);
	if (i == Z_BUF_ERROR)
		return (VGZ_STUCK);
	VSLb(vg->vsl, SLT_Gzip, "Gunzip error: %d (%s)", i, vg->vz.msg);
	return (VGZ_ERROR);
}

/*--------------------------------------------------------------------*/

enum vgzret_e
VGZ_Gzip(struct vgz *vg, const void **pptr, size_t *plen, enum vgz_flag flags)
{
	int i;
	int zflg;
	ssize_t l;
	const uint8_t *before;

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	*pptr = NULL;
	*plen = 0;
	AN(vg->vz.next_out);
	AN(vg->vz.avail_out);
	before = vg->vz.next_out;
	switch(flags) {
	case VGZ_NORMAL:	zflg = Z_NO_FLUSH; break;
	case VGZ_ALIGN:		zflg = Z_SYNC_FLUSH; break;
	case VGZ_RESET:		zflg = Z_FULL_FLUSH; break;
	case VGZ_FINISH:	zflg = Z_FINISH; break;
	default:		INCOMPL();
	}
	i = deflate(&vg->vz, zflg);
	if (i == Z_OK || i == Z_STREAM_END) {
		*pptr = before;
		l = (const uint8_t *)vg->vz.next_out - before;
		*plen = l;
	}
	vg->last_i = i;
	if (i == Z_OK)
		return (VGZ_OK);
	if (i == Z_STREAM_END)
		return (VGZ_END);
	if (i == Z_BUF_ERROR)
		return (VGZ_STUCK);
	VSLb(vg->vsl, SLT_Gzip, "Gzip error: %d (%s)", i, vg->vz.msg);
	return (VGZ_ERROR);
}

/*--------------------------------------------------------------------
 */

int
VGZ_WrwInit(struct vgz *vg)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	if (vgz_getmbuf(vg))
		return (-1);

	VGZ_Obuf(vg, vg->m_buf, vg->m_sz);
	return (0);
}

/*--------------------------------------------------------------------
 * VDP for gunzip'ing
 */

int __match_proto__(vdp_bytes)
VDP_gunzip(struct req *req, enum vdp_action act, const void *ptr, ssize_t len)
{
	enum vgzret_e vr;
	size_t dl;
	const void *dp;
	struct worker *wrk;
	struct vgz *vg;

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	wrk = req->wrk;
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	vg = req->vgz;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AN(vg->m_buf);

	if (len == 0) {
		AN(act > VDP_NULL);
		return (VDP_bytes(req, act, vg->m_buf, vg->m_len));
	}

	VGZ_Ibuf(vg, ptr, len);
	do {
		if (vg->m_len == vg->m_sz)
			vr = VGZ_STUCK;
		else {
			vr = VGZ_Gunzip(vg, &dp, &dl);
			vg->m_len += dl;
		}
		if (vr < VGZ_OK)
			return (-1);
		if (vg->m_len == vg->m_sz || vr == VGZ_STUCK) {
			if (VDP_bytes(req, VDP_FLUSH, vg->m_buf, vg->m_len))
				return (-1);
			vg->m_len = 0;
			VGZ_Obuf(vg, vg->m_buf, vg->m_sz);
		}
	} while (!VGZ_IbufEmpty(vg));
	assert(vr == VGZ_STUCK || vr == VGZ_OK || vr == VGZ_END);
	return (0);
}

/*--------------------------------------------------------------------
 * Gunzip ibuf into outb, if it runs full, emit it with WRW.
 * Leave flushing to caller, more data may be coming.
 */

enum vgzret_e
VGZ_WrwGunzip(struct req *req, struct vgz *vg, const void *ibuf,
    ssize_t ibufl)
{
	enum vgzret_e vr;
	size_t dl;
	const void *dp;
	struct worker *wrk;

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	wrk = req->wrk;
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AN(vg->m_buf);
	VGZ_Ibuf(vg, ibuf, ibufl);
	if (ibufl == 0)
		return (VGZ_OK);
	do {
		if (vg->m_len == vg->m_sz)
			vr = VGZ_STUCK;
		else {
			vr = VGZ_Gunzip(vg, &dp, &dl);
			vg->m_len += dl;
		}
		if (vr < VGZ_OK)
			return (vr);
		if (vg->m_len == vg->m_sz || vr == VGZ_STUCK) {
			(void)VDP_bytes(req, VDP_FLUSH, vg->m_buf, vg->m_len);
			vg->m_len = 0;
			VGZ_Obuf(vg, vg->m_buf, vg->m_sz);
		}
	} while (!VGZ_IbufEmpty(vg));
	if (vr == VGZ_STUCK)
		vr = VGZ_OK;
	return (vr);
}

/*--------------------------------------------------------------------*/

void
VGZ_WrwFlush(struct req *req, struct vgz *vg)
{
	struct worker *wrk;

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	wrk = req->wrk;
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	if (vg->m_len ==  0)
		return;

	(void)VDP_bytes(req, VDP_FLUSH, vg->m_buf, vg->m_len);
	vg->m_len = 0;
	VGZ_Obuf(vg, vg->m_buf, vg->m_sz);
}

/*--------------------------------------------------------------------*/

void
VGZ_UpdateObj(const struct vgz *vg, struct object *obj)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	CHECK_OBJ_NOTNULL(obj, OBJECT_MAGIC);
	obj->gzip_start	= vg->vz.start_bit;
	obj->gzip_last	= vg->vz.last_bit;
	obj->gzip_stop	= vg->vz.stop_bit;
}

/*--------------------------------------------------------------------
 */

enum vgzret_e
VGZ_Destroy(struct vgz **vgp)
{
	struct vgz *vg;
	enum vgzret_e vr;
	int i;

	vg = *vgp;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	*vgp = NULL;

	VSLb(vg->vsl, SLT_Gzip, "%s %jd %jd %jd %jd %jd",
	    vg->id,
	    (intmax_t)vg->vz.total_in,
	    (intmax_t)vg->vz.total_out,
	    (intmax_t)vg->vz.start_bit,
	    (intmax_t)vg->vz.last_bit,
	    (intmax_t)vg->vz.stop_bit);
	if (vg->tmp != NULL)
		WS_Reset(vg->tmp, vg->tmp_snapshot);
	if (vg->dir == VGZ_GZ)
		i = deflateEnd(&vg->vz);
	else
		i = inflateEnd(&vg->vz);
	if (vg->last_i == Z_STREAM_END && i == Z_OK)
		i = Z_STREAM_END;
	if (vg->m_buf)
		free(vg->m_buf);
	if (i == Z_OK)
		vr = VGZ_OK;
	else if (i == Z_STREAM_END)
		vr = VGZ_END;
	else if (i == Z_BUF_ERROR)
		vr = VGZ_STUCK;
	else {
		VSLb(vg->vsl, SLT_Gzip, "G(un)zip error: %d (%s)", i, vg->vz.msg);
		vr = VGZ_ERROR;
	}
	FREE_OBJ(vg);
	return (vr);
}

/*--------------------------------------------------------------------
 * VFP_GUNZIP
 *
 * A VFP for gunzip'ing an object as we receive it from the backend
 */

static void __match_proto__(vfp_begin_f)
vfp_gunzip_begin(struct busyobj *bo, size_t estimate)
{

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	(void)estimate;
	AZ(bo->vgz_rx);
	bo->vgz_rx = VGZ_NewUngzip(bo->vsl, "U F -");
	XXXAZ(vgz_getmbuf(bo->vgz_rx));
}

static int __match_proto__(vfp_bytes_f)
vfp_gunzip_bytes(struct busyobj *bo, struct http_conn *htc, ssize_t bytes)
{
	struct vgz *vg;
	ssize_t l, wl;
	int i = -100;
	size_t dl;
	const void *dp;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vg = bo->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AZ(vg->vz.avail_in);
	while (bytes > 0 || vg->vz.avail_in > 0) {
		if (vg->vz.avail_in == 0 && bytes > 0) {
			l = vg->m_sz;
			if (l > bytes)
				l = bytes;
			wl = htc->read(htc, vg->m_buf, l);
			if (wl <= 0)
				return (wl);
			VGZ_Ibuf(vg, vg->m_buf, wl);
			bytes -= wl;
		}

		if (VGZ_ObufStorage(bo, vg))
			return(-1);
		i = VGZ_Gunzip(vg, &dp, &dl);
		if (i != VGZ_OK && i != VGZ_END)
			return(VFP_Error(bo, "Gunzip data error"));
		VBO_extend(bo, dl);
	}
	assert(i == Z_OK || i == Z_STREAM_END);
	return (1);
}

static int __match_proto__(vfp_end_f)
vfp_gunzip_end(struct busyobj *bo)
{
	struct vgz *vg;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vg = bo->vgz_rx;
	bo->vgz_rx = NULL;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	if (bo->state == BOS_FAILED) {
		(void)VGZ_Destroy(&vg);
		return(0);
	}
	if (VGZ_Destroy(&vg) != VGZ_END)
		return(VFP_Error(bo, "Gunzip error at the very end"));
	return (0);
}

struct vfp vfp_gunzip = {
        .begin  =       vfp_gunzip_begin,
        .bytes  =       vfp_gunzip_bytes,
        .end    =       vfp_gunzip_end,
};

/*--------------------------------------------------------------------
 * VFP_GZIP
 *
 * A VFP for gzip'ing an object as we receive it from the backend
 */

static void __match_proto__(vfp_begin_f)
vfp_gzip_begin(struct busyobj *bo, size_t estimate)
{
	(void)estimate;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	AZ(bo->vgz_rx);
	bo->vgz_rx = VGZ_NewGzip(bo->vsl, "G F -");
	XXXAZ(vgz_getmbuf(bo->vgz_rx));
}

static int __match_proto__(vfp_bytes_f)
vfp_gzip_bytes(struct busyobj *bo, struct http_conn *htc, ssize_t bytes)
{
	struct vgz *vg;
	ssize_t l, wl;
	int i = -100;
	size_t dl;
	const void *dp;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vg = bo->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AZ(vg->vz.avail_in);
	while (bytes > 0 || !VGZ_IbufEmpty(vg)) {
		if (VGZ_IbufEmpty(vg) && bytes > 0) {
			l = vg->m_sz;
			if (l > bytes)
				l = bytes;
			wl = htc->read(htc, vg->m_buf, l);
			if (wl <= 0)
				return (wl);
			VGZ_Ibuf(vg, vg->m_buf, wl);
			bytes -= wl;
		}
		if (VGZ_ObufStorage(bo, vg))
			return(-1);
		i = VGZ_Gzip(vg, &dp, &dl, VGZ_NORMAL);
		assert(i == Z_OK);
		VBO_extend(bo, dl);
	}
	return (1);
}

static int __match_proto__(vfp_end_f)
vfp_gzip_end(struct busyobj *bo)
{
	struct vgz *vg;
	size_t dl;
	const void *dp;
	int i;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vg = bo->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	bo->vgz_rx = NULL;
	if (bo->state == BOS_FAILED) {
		(void)VGZ_Destroy(&vg);
		return(0);
	}
	do {
		VGZ_Ibuf(vg, "", 0);
		if (VGZ_ObufStorage(bo, vg))
			return(-1);
		i = VGZ_Gzip(vg, &dp, &dl, VGZ_FINISH);
		VBO_extend(bo, dl);
	} while (i != Z_STREAM_END);
	VGZ_UpdateObj(vg, bo->fetch_obj);
	if (VGZ_Destroy(&vg) != VGZ_END)
		return(VFP_Error(bo, "Gzip error at the very end"));
	return (0);
}

struct vfp vfp_gzip = {
        .begin  =       vfp_gzip_begin,
        .bytes  =       vfp_gzip_bytes,
        .end    =       vfp_gzip_end,
};

/*--------------------------------------------------------------------
 * VFP_TESTGZIP
 *
 * A VFP for testing that received gzip data is valid, and for
 * collecting the magic bits while we're at it.
 */

static void __match_proto__(vfp_begin_f)
vfp_testgzip_begin(struct busyobj *bo, size_t estimate)
{

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	(void)estimate;
	bo->vgz_rx = VGZ_NewUngzip(bo->vsl, "u F -");
	CHECK_OBJ_NOTNULL(bo->vgz_rx, VGZ_MAGIC);
	XXXAZ(vgz_getmbuf(bo->vgz_rx));
}

static int __match_proto__(vfp_bytes_f)
vfp_testgzip_bytes(struct busyobj *bo, struct http_conn *htc, ssize_t bytes)
{
	struct vgz *vg;
	ssize_t l, wl;
	int i = -100;
	size_t dl;
	const void *dp;
	struct storage *st;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vg = bo->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AZ(vg->vz.avail_in);
	while (bytes > 0) {
		st = VFP_GetStorage(bo, 0);
		if (st == NULL)
			return(-1);
		l = st->space - st->len;
		if (l > bytes)
			l = bytes;
		wl = htc->read(htc, st->ptr + st->len, l);
		if (wl <= 0)
			return (wl);
		bytes -= wl;
		VGZ_Ibuf(vg, st->ptr + st->len, wl);
		VBO_extend(bo, wl);

		while (!VGZ_IbufEmpty(vg)) {
			VGZ_Obuf(vg, vg->m_buf, vg->m_sz);
			i = VGZ_Gunzip(vg, &dp, &dl);
			if (i == VGZ_END && !VGZ_IbufEmpty(vg))
				return(VFP_Error(bo, "Junk after gzip data"));
			if (i != VGZ_OK && i != VGZ_END)
				return(VFP_Error2(bo,
				    "Invalid Gzip data", vg->vz.msg));
		}
	}
	assert(i == VGZ_OK || i == VGZ_END);
	return (1);
}

static int __match_proto__(vfp_end_f)
vfp_testgzip_end(struct busyobj *bo)
{
	struct vgz *vg;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vg = bo->vgz_rx;
	bo->vgz_rx = NULL;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	if (bo->state == BOS_FAILED) {
		(void)VGZ_Destroy(&vg);
		return(0);
	}
	VGZ_UpdateObj(vg, bo->fetch_obj);
	if (VGZ_Destroy(&vg) != VGZ_END)
		return(VFP_Error(bo, "TestGunzip error at the very end"));
	return (0);
}

struct vfp vfp_testgzip = {
        .begin  =       vfp_testgzip_begin,
        .bytes  =       vfp_testgzip_bytes,
        .end    =       vfp_testgzip_end,
};
