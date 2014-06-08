/*
 * Copyright 2011 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */

#include <linux/dma-mapping.h>

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>

#include <nouveau_drm.h>
#include <nouveau_dma.h>
#include <nouveau_gem.h>
#include <nouveau_connector.h>
#include <nouveau_encoder.h>
#include <nouveau_crtc.h>
#include <nouveau_fence.h>

#include <core/client.h>
#include <core/gpuobj.h>
#include <core/class.h>

#include <subdev/timer.h>
#include <subdev/bar.h>
#include <subdev/fb.h>
#include <subdev/i2c.h>

#define EVO_DMA_NR 9

#define EVO_MASTER  (0x00)
#define EVO_FLIP(c) (0x01 + (c))
#define EVO_OVLY(c) (0x05 + (c))
#define EVO_OIMM(c) (0x09 + (c))
#define EVO_CURS(c) (0x0d + (c))

#define EVO_CORE_HANDLE      (0xd1500000)
#define EVO_CHAN_HANDLE(t,i) (0xd15c0000 | (((t) & 0x00ff) << 8) | (i))
#define EVO_CHAN_OCLASS(t,c) ((nv_hclass(c) & 0xff00) | ((t) & 0x00ff))
#define EVO_PUSH_HANDLE(t,i) (0xd15b0000 | (i) |                               \
			      (((NV50_DISP_##t##_CLASS) & 0x00ff) << 8))

/******************************************************************************
 * EVO channel
 *****************************************************************************/

int
nv50_chan_create(struct nouveau_object *core, u32 bclass, u8 head,
		 void *data, u32 size, struct nv50_chan *chan)
{
	struct nouveau_object *client = nv_pclass(core, NV_CLIENT_CLASS);
	const u32 oclass = EVO_CHAN_OCLASS(bclass, core);
	const u32 handle = EVO_CHAN_HANDLE(bclass, head);
	int ret;

	ret = nouveau_object_new(client, EVO_CORE_HANDLE, handle,
				 oclass, data, size, &chan->user);
	if (ret)
		return ret;

	chan->handle = handle;
	return 0;
}

void
nv50_chan_destroy(struct nouveau_object *core, struct nv50_chan *chan)
{
	struct nouveau_object *client = nv_pclass(core, NV_CLIENT_CLASS);
	if (chan->handle)
		nouveau_object_del(client, EVO_CORE_HANDLE, chan->handle);
}

/******************************************************************************
 * EVO channel helpers
 *****************************************************************************/
u32 *
evo_wait(void *evoc, int nr)
{
	struct nv50_dmac *dmac = evoc;
	u32 put = nv_ro32(dmac->base.user, 0x0000) / 4;

	mutex_lock(&dmac->lock);
	if (put + nr >= (PAGE_SIZE / 4) - 8) {
		dmac->ptr[put] = 0x20000000;

		nv_wo32(dmac->base.user, 0x0000, 0x00000000);
		if (!nv_wait(dmac->base.user, 0x0004, ~0, 0x00000000)) {
			mutex_unlock(&dmac->lock);
			NV_ERROR(dmac->base.user, "channel stalled\n");
			return NULL;
		}

		put = 0;
	}

	return dmac->ptr + put;
}

void
evo_kick(u32 *push, void *evoc)
{
	struct nv50_dmac *dmac = evoc;
	nv_wo32(dmac->base.user, 0x0000, (push - dmac->ptr) << 2);
	mutex_unlock(&dmac->lock);
}

bool
evo_sync_wait(void *data)
{
	if (nouveau_bo_rd32(data, EVO_MAST_NTFY) != 0x00000000)
		return true;
	usleep_range(1, 2);
	return false;
}

int
evo_sync(struct drm_device *dev)
{
	struct nouveau_device *device = nouveau_dev(dev);
	struct nv50_disp *disp = nv50_disp(dev);
	struct nv50_mast *mast = nv50_mast(dev);
	u32 *push = evo_wait(mast, 8);
	if (push) {
		nouveau_bo_wr32(disp->sync, EVO_MAST_NTFY, 0x00000000);
		evo_mthd(push, 0x0084, 1);
		evo_data(push, 0x80000000 | EVO_MAST_NTFY);
		evo_mthd(push, 0x0080, 2);
		evo_data(push, 0x00000000);
		evo_data(push, 0x00000000);
		evo_kick(push, mast);
		if (nv_wait_cb(device, evo_sync_wait, disp->sync))
			return 0;
	}

	return -EBUSY;
}
