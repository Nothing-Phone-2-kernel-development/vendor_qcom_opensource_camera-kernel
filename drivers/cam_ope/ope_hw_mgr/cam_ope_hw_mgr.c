// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <media/cam_defs.h>
#include <media/cam_ope.h>
#include <media/cam_cpas.h>

#include "cam_sync_api.h"
#include "cam_packet_util.h"
#include "cam_hw.h"
#include "cam_hw_mgr_intf.h"
#include "cam_ope_hw_mgr_intf.h"
#include "cam_ope_hw_mgr.h"
#include "ope_hw.h"
#include "cam_smmu_api.h"
#include "cam_mem_mgr.h"
#include "cam_req_mgr_workq.h"
#include "cam_mem_mgr.h"
#include "cam_debug_util.h"
#include "cam_soc_util.h"
#include "cam_trace.h"
#include "cam_cpas_api.h"
#include "cam_common_util.h"
#include "cam_cdm_intf_api.h"
#include "cam_cdm_util.h"
#include "cam_cdm.h"
#include "ope_dev_intf.h"

static struct cam_ope_hw_mgr *ope_hw_mgr;

static int cam_ope_mgr_get_rsc_idx(struct cam_ope_ctx *ctx_data,
	struct ope_io_buf_info *in_io_buf)
{
	int k = 0;
	int rsc_idx = -EINVAL;

	if (in_io_buf->direction == CAM_BUF_INPUT) {
		for (k = 0; k < OPE_IN_RES_MAX; k++) {
			if (ctx_data->ope_acquire.in_res[k].res_id ==
				in_io_buf->resource_type)
				break;
		}
		if (k == OPE_IN_RES_MAX) {
			CAM_ERR(CAM_OPE, "Invalid res_id %d",
				in_io_buf->resource_type);
			goto end;
		}
		rsc_idx = k;
	} else if (in_io_buf->direction == CAM_BUF_OUTPUT) {
		for (k = 0; k < OPE_OUT_RES_MAX; k++) {
			if (ctx_data->ope_acquire.out_res[k].res_id ==
				in_io_buf->resource_type)
				break;
		}
		if (k == OPE_OUT_RES_MAX) {
			CAM_ERR(CAM_OPE, "Invalid res_id %d",
				in_io_buf->resource_type);
			goto end;
		}
		rsc_idx = k;
	}

end:
	return rsc_idx;
}

static int cam_ope_mgr_process_cmd(void *priv, void *data)
{
	int rc;
	struct ope_cmd_work_data *task_data = NULL;
	struct cam_ope_ctx *ctx_data;
	struct cam_cdm_bl_request *cdm_cmd;

	if (!data || !priv) {
		CAM_ERR(CAM_OPE, "Invalid params%pK %pK", data, priv);
		return -EINVAL;
	}

	ctx_data = priv;
	task_data = (struct ope_cmd_work_data *)data;
	cdm_cmd = task_data->data;

	CAM_DBG(CAM_OPE, "cam_cdm_submit_bls: handle = %u",
		ctx_data->ope_cdm.cdm_handle);
	rc = cam_cdm_submit_bls(ctx_data->ope_cdm.cdm_handle, cdm_cmd);

	if (!rc)
		ctx_data->req_cnt++;
	else
		CAM_ERR(CAM_OPE, "submit failed for %lld", cdm_cmd->cookie);

	return rc;
}

static int cam_ope_mgr_reset_hw(void)
{
	struct cam_ope_hw_mgr *hw_mgr = ope_hw_mgr;
	int i, rc = 0;

	for (i = 0; i < ope_hw_mgr->num_ope; i++) {
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_RESET,
			NULL, 0);
		if (rc) {
			CAM_ERR(CAM_OPE, "OPE Reset failed: %d", rc);
			return rc;
		}
	}

	return rc;
}

static int cam_ope_req_timer_modify(struct cam_ope_ctx *ctx_data,
	int32_t expires)
{
	if (ctx_data->req_watch_dog) {
		CAM_DBG(CAM_ICP, "stop timer : ctx_id = %d", ctx_data->ctx_id);
		crm_timer_modify(ctx_data->req_watch_dog, expires);
	}
	return 0;
}

static int cam_ope_req_timer_stop(struct cam_ope_ctx *ctx_data)
{
	if (ctx_data->req_watch_dog) {
		CAM_DBG(CAM_ICP, "stop timer : ctx_id = %d", ctx_data->ctx_id);
		crm_timer_exit(&ctx_data->req_watch_dog);
		ctx_data->req_watch_dog = NULL;
	}
	return 0;
}

static int cam_ope_req_timer_reset(struct cam_ope_ctx *ctx_data)
{
	if (ctx_data && ctx_data->req_watch_dog)
		crm_timer_reset(ctx_data->req_watch_dog);

	return 0;
}


static int cam_ope_mgr_reapply_config(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_ope_ctx *ctx_data,
	struct cam_ope_request *ope_req)
{
	int rc = 0;
	uint64_t request_id = 0;
	struct crm_workq_task *task;
	struct ope_cmd_work_data *task_data;

	request_id = ope_req->request_id;
	CAM_DBG(CAM_OPE, "reapply req_id = %lld", request_id);

	task = cam_req_mgr_workq_get_task(ope_hw_mgr->cmd_work);
	if (!task) {
		CAM_ERR(CAM_OPE, "no empty task");
		return -ENOMEM;
	}

	task_data = (struct ope_cmd_work_data *)task->payload;
	task_data->data = (void *)ope_req->cdm_cmd;
	task_data->req_id = request_id;
	task_data->type = OPE_WORKQ_TASK_CMD_TYPE;
	task->process_cb = cam_ope_mgr_process_cmd;
	rc = cam_req_mgr_workq_enqueue_task(task, ctx_data,
		CRM_TASK_PRIORITY_0);

	return rc;
}

static bool cam_ope_is_pending_request(struct cam_ope_ctx *ctx_data)
{
	return !bitmap_empty(ctx_data->bitmap, CAM_CTX_REQ_MAX);
}

static int32_t cam_ope_process_request_timer(void *priv, void *data)
{
	struct ope_clk_work_data *task_data = (struct ope_clk_work_data *)data;
	struct cam_ope_ctx *ctx_data = (struct cam_ope_ctx *)task_data->data;

	if (cam_ope_is_pending_request(ctx_data)) {
		CAM_DBG(CAM_OPE, "pending requests means, issue is with HW");
		cam_cdm_handle_error(ctx_data->ope_cdm.cdm_handle);
		cam_ope_req_timer_reset(ctx_data);
	} else {
		cam_ope_req_timer_modify(ctx_data, ~0);
	}
	return 0;
}

static void cam_ope_req_timer_cb(struct timer_list *timer_data)
{
	unsigned long flags;
	struct crm_workq_task *task;
	struct ope_clk_work_data *task_data;
	struct cam_req_mgr_timer *timer =
	container_of(timer_data, struct cam_req_mgr_timer, sys_timer);

	spin_lock_irqsave(&ope_hw_mgr->hw_mgr_lock, flags);
	task = cam_req_mgr_workq_get_task(ope_hw_mgr->timer_work);
	if (!task) {
		CAM_ERR(CAM_OPE, "no empty task");
		spin_unlock_irqrestore(&ope_hw_mgr->hw_mgr_lock, flags);
		return;
	}

	task_data = (struct ope_clk_work_data *)task->payload;
	task_data->data = timer->parent;
	task_data->type = OPE_WORKQ_TASK_MSG_TYPE;
	task->process_cb = cam_ope_process_request_timer;
	cam_req_mgr_workq_enqueue_task(task, ope_hw_mgr,
		CRM_TASK_PRIORITY_0);
	spin_unlock_irqrestore(&ope_hw_mgr->hw_mgr_lock, flags);
}

static int cam_ope_start_req_timer(struct cam_ope_ctx *ctx_data)
{
	int rc = 0;

	rc = crm_timer_init(&ctx_data->req_watch_dog,
		200, ctx_data, &cam_ope_req_timer_cb);
	if (rc)
		CAM_ERR(CAM_ICP, "Failed to start timer");

	return rc;
}

static int cam_get_valid_ctx_id(void)
{
	struct cam_ope_hw_mgr *hw_mgr = ope_hw_mgr;
	int i;


	for (i = 0; i < OPE_CTX_MAX; i++) {
		if (hw_mgr->ctx[i].ctx_state == OPE_CTX_STATE_ACQUIRED)
			break;
	}

	return i;
}


static void cam_ope_ctx_cdm_callback(uint32_t handle, void *userdata,
	enum cam_cdm_cb_status status, uint64_t cookie)
{
	int rc = 0;
	struct cam_ope_ctx *ctx;
	struct cam_ope_request *ope_req;
	struct cam_hw_done_event_data buf_data;
	bool flag = false;

	if (!userdata) {
		CAM_ERR(CAM_OPE, "Invalid ctx from CDM callback");
		return;
	}

	CAM_DBG(CAM_FD, "CDM hdl=%x, udata=%pK, status=%d, cookie=%llu",
		handle, userdata, status, cookie);

	ctx = userdata;
	ope_req = ctx->req_list[cookie];

	mutex_lock(&ctx->ctx_mutex);
	if (ctx->ctx_state != OPE_CTX_STATE_ACQUIRED) {
		CAM_DBG(CAM_OPE, "ctx %u is in %d state",
			ctx->ctx_id, ctx->ctx_state);
		mutex_unlock(&ctx->ctx_mutex);
		return;
	}

	if (status == CAM_CDM_CB_STATUS_BL_SUCCESS) {
		CAM_DBG(CAM_OPE,
			"hdl=%x, udata=%pK, status=%d, cookie=%d  req_id=%llu ctx_id=%d",
			handle, userdata, status, cookie,
			ope_req->request_id, ctx->ctx_id);
		cam_ope_req_timer_reset(ctx);
	} else if (status == CAM_CDM_CB_STATUS_HW_RESUBMIT) {
		CAM_INFO(CAM_OPE, "After reset of CDM and OPE, reapply req");
		rc = cam_ope_mgr_reapply_config(ope_hw_mgr, ctx, ope_req);
		if (!rc)
			goto end;
	} else {
		CAM_ERR(CAM_OPE,
			"CDM hdl=%x, udata=%pK, status=%d, cookie=%d req_id = %llu",
			 handle, userdata, status, cookie, ope_req->request_id);
		CAM_ERR(CAM_OPE, "Rst of CDM and OPE for error reqid = %lld",
			ope_req->request_id);
		rc = cam_ope_mgr_reset_hw();
		flag = true;
	}

	ctx->req_cnt--;

	buf_data.request_id = ope_req->request_id;
	ope_req->request_id = 0;
	kzfree(ctx->req_list[cookie]->cdm_cmd);
	ctx->req_list[cookie]->cdm_cmd = NULL;
	kzfree(ctx->req_list[cookie]);
	ctx->req_list[cookie] = NULL;
	clear_bit(cookie, ctx->bitmap);
	ctx->ctxt_event_cb(ctx->context_priv, flag, &buf_data);

end:
	mutex_unlock(&ctx->ctx_mutex);
}

static int32_t cam_ope_mgr_process_msg(void *priv, void *data)
{
	struct ope_msg_work_data *task_data;
	struct cam_ope_hw_mgr *hw_mgr;
	struct cam_ope_ctx *ctx;
	uint32_t irq_status;
	int32_t ctx_id;
	int rc = 0, i;

	if (!data || !priv) {
		CAM_ERR(CAM_OPE, "Invalid data");
		return -EINVAL;
	}

	task_data = data;
	hw_mgr = priv;
	irq_status = task_data->irq_status;
	ctx_id = cam_get_valid_ctx_id();
	if (ctx_id < 0) {
		CAM_ERR(CAM_OPE, "No valid context to handle error");
		return ctx_id;
	}

	ctx = &hw_mgr->ctx[ctx_id];

	/* Indicate about this error to CDM and reset OPE*/
	rc = cam_cdm_handle_error(ctx->ope_cdm.cdm_handle);

	for (i = 0; i < hw_mgr->num_ope; i++) {
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_RESET,
			NULL, 0);
		if (rc)
			CAM_ERR(CAM_OPE, "OPE Dev acquire failed: %d", rc);
	}

	return rc;
}

int32_t cam_ope_hw_mgr_cb(uint32_t irq_status, void *data)
{
	int32_t rc = 0;
	unsigned long flags;
	struct cam_ope_hw_mgr *hw_mgr = data;
	struct crm_workq_task *task;
	struct ope_msg_work_data *task_data;

	if (!data) {
		CAM_ERR(CAM_OPE, "irq cb data is NULL");
		return rc;
	}

	spin_lock_irqsave(&hw_mgr->hw_mgr_lock, flags);
	task = cam_req_mgr_workq_get_task(ope_hw_mgr->msg_work);
	if (!task) {
		CAM_ERR(CAM_OPE, "no empty task");
		spin_unlock_irqrestore(&hw_mgr->hw_mgr_lock, flags);
		return -ENOMEM;
	}

	task_data = (struct ope_msg_work_data *)task->payload;
	task_data->data = hw_mgr;
	task_data->irq_status = irq_status;
	task_data->type = OPE_WORKQ_TASK_MSG_TYPE;
	task->process_cb = cam_ope_mgr_process_msg;
	rc = cam_req_mgr_workq_enqueue_task(task, ope_hw_mgr,
		CRM_TASK_PRIORITY_0);
	spin_unlock_irqrestore(&hw_mgr->hw_mgr_lock, flags);

	return rc;
}

static int cam_ope_mgr_create_kmd_buf(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_packet *packet,
	struct cam_hw_prepare_update_args *prepare_args,
	struct cam_ope_ctx *ctx_data, uint32_t req_idx,
	uintptr_t   ope_cmd_buf_addr)
{
	int i, rc = 0;
	struct cam_ope_dev_prepare_req prepare_req;

	prepare_req.ctx_data = ctx_data;
	prepare_req.hw_mgr = hw_mgr;
	prepare_req.packet = packet;
	prepare_req.prepare_args = prepare_args;
	prepare_req.req_idx = req_idx;
	prepare_req.kmd_buf_offset = 0;
	prepare_req.frame_process =
		(struct ope_frame_process *)ope_cmd_buf_addr;

	for (i = 0; i < ope_hw_mgr->num_ope; i++)
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv,
			OPE_HW_PREPARE, &prepare_req, sizeof(prepare_req));
		if (rc) {
			CAM_ERR(CAM_OPE, "OPE Dev prepare failed: %d", rc);
			goto end;
		}

end:
	return rc;
}

static int cam_ope_mgr_process_io_cfg(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_packet *packet,
	struct cam_hw_prepare_update_args *prep_args,
	struct cam_ope_ctx *ctx_data, uint32_t req_idx)
{

	int i, j = 0, k = 0, l, rc = 0;
	struct ope_io_buf *io_buf;
	int32_t sync_in_obj[CAM_MAX_IN_RES];
	int32_t merged_sync_in_obj;
	struct cam_ope_request *ope_request;

	ope_request = ctx_data->req_list[req_idx];
	prep_args->num_out_map_entries = 0;
	prep_args->num_in_map_entries = 0;

	ope_request = ctx_data->req_list[req_idx];
	CAM_DBG(CAM_OPE, "E: req_idx = %u %x", req_idx, packet);

	for (i = 0; i < ope_request->num_batch; i++) {
		for (l = 0; l < ope_request->num_io_bufs[i]; l++) {
			io_buf = &ope_request->io_buf[i][l];
			if (io_buf->direction == CAM_BUF_INPUT) {
				if (io_buf->fence != -1) {
					sync_in_obj[j++] = io_buf->fence;
					prep_args->num_in_map_entries++;
				} else {
					CAM_ERR(CAM_OPE, "Invalid fence %d %d",
						io_buf->resource_type,
						ope_request->request_id);
				}
			} else {
				if (io_buf->fence != -1) {
					prep_args->out_map_entries[k].sync_id =
						io_buf->fence;
					k++;
					prep_args->num_out_map_entries++;
				} else {
					CAM_ERR(CAM_OPE, "Invalid fence %d %d",
						io_buf->resource_type,
						ope_request->request_id);
				}
			}
			CAM_DBG(CAM_REQ,
				"ctx_id: %u req_id: %llu dir[%d] %u, fence: %d",
				ctx_data->ctx_id, packet->header.request_id, i,
				io_buf->direction, io_buf->fence);
			CAM_DBG(CAM_REQ, "rsc_type = %u fmt = %d",
				io_buf->resource_type,
				io_buf->format);
		}
	}

	if (prep_args->num_in_map_entries > 1)
		prep_args->num_in_map_entries =
			cam_common_util_remove_duplicate_arr(
			sync_in_obj, prep_args->num_in_map_entries);

	if (prep_args->num_in_map_entries > 1) {
		rc = cam_sync_merge(&sync_in_obj[0],
			prep_args->num_in_map_entries, &merged_sync_in_obj);
		if (rc) {
			prep_args->num_out_map_entries = 0;
			prep_args->num_in_map_entries = 0;
			return rc;
		}

		ope_request->in_resource = merged_sync_in_obj;

		prep_args->in_map_entries[0].sync_id = merged_sync_in_obj;
		prep_args->num_in_map_entries = 1;
		CAM_DBG(CAM_REQ, "ctx_id: %u req_id: %llu Merged Sync obj: %d",
			ctx_data->ctx_id, packet->header.request_id,
			merged_sync_in_obj);
	} else if (prep_args->num_in_map_entries == 1) {
		prep_args->in_map_entries[0].sync_id = sync_in_obj[0];
		prep_args->num_in_map_entries = 1;
		ope_request->in_resource = 0;
		CAM_DBG(CAM_OPE, "fence = %d", sync_in_obj[0]);
	} else {
		CAM_DBG(CAM_OPE, "No input fences");
		prep_args->num_in_map_entries = 0;
		ope_request->in_resource = 0;
		rc = -EINVAL;
	}
	return rc;
}

static void cam_ope_mgr_print_stripe_info(uint32_t batch,
	uint32_t io_buf, uint32_t plane, uint32_t stripe,
	struct ope_stripe_io *stripe_info, uint64_t iova_addr)
{
	CAM_DBG(CAM_OPE, "b:%d io:%d p:%d s:%d: E",
		batch, io_buf, plane, stripe);
	CAM_DBG(CAM_OPE, "width: %d s_w: %u s_h: %u s_s: %u",
		stripe_info->width, stripe_info->width,
		stripe_info->height, stripe_info->stride);
	CAM_DBG(CAM_OPE, "s_xinit = %u iova = %x s_loc = %u",
		 stripe_info->s_location, stripe_info->x_init,
		 iova_addr);
	CAM_DBG(CAM_OPE, "s_off = %u s_format = %u s_len = %u",
		stripe_info->offset, stripe_info->format,
		stripe_info->len);
	CAM_DBG(CAM_OPE, "s_align = %u s_pack = %u s_unpack = %u",
		stripe_info->alignment, stripe_info->pack_format,
		stripe_info->unpack_format);
	CAM_DBG(CAM_OPE, "b:%d io:%d p:%d s:%d: E",
		batch, io_buf, plane, stripe);
}

static int cam_ope_mgr_process_cmd_io_buf_req(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_packet *packet, struct cam_ope_ctx *ctx_data,
	uintptr_t frame_process_addr, size_t length, uint32_t req_idx)
{
	int rc = 0;
	int i, j, k, l;
	uint64_t iova_addr;
	size_t len;
	struct ope_frame_process *in_frame_process;
	struct ope_frame_set *in_frame_set;
	struct ope_io_buf_info *in_io_buf;
	struct ope_stripe_info *in_stripe_info;
	struct cam_ope_request *ope_request;
	struct ope_io_buf *io_buf;
	struct ope_stripe_io *stripe_info;
	uint32_t alignment;
	uint32_t rsc_idx;
	uint32_t pack_format;
	uint32_t unpack_format;
	struct ope_in_res_info *in_res;
	struct ope_out_res_info *out_res;

	in_frame_process = (struct ope_frame_process *)frame_process_addr;

	ope_request = ctx_data->req_list[req_idx];
	ope_request->num_batch = in_frame_process->batch_size;

	for (i = 0; i < in_frame_process->batch_size; i++) {
		in_frame_set = &in_frame_process->frame_set[i];
		for (j = 0; j < in_frame_set->num_io_bufs; j++) {
			in_io_buf = &in_frame_set->io_buf[j];
			CAM_DBG(CAM_OPE, "i:%d j:%d dir: %x rsc: %u plane: %d",
				i, j, in_io_buf->direction,
				in_io_buf->resource_type,
				in_io_buf->num_planes);
			for (k = 0; k < in_io_buf->num_planes; k++) {
				CAM_DBG(CAM_OPE, "i:%d j:%d k:%d numstripe: %d",
					i, j, k, in_io_buf->num_stripes[k]);
				CAM_DBG(CAM_OPE, "m_hdl: %d len: %d",
					in_io_buf->mem_handle[k],
					in_io_buf->length[k]);
				for (l = 0; l < in_io_buf->num_stripes[k];
					l++) {
					in_stripe_info =
						&in_io_buf->stripe_info[k][l];
					CAM_DBG(CAM_OPE, "i:%d j:%d k:%d l:%d",
						i, j, k, l);
					CAM_DBG(CAM_OPE, "%d s_loc:%d w:%d",
						in_stripe_info->x_init,
						in_stripe_info->stripe_location,
						in_stripe_info->width);
					CAM_DBG(CAM_OPE,  "s_off: %d d_bus: %d",
						in_stripe_info->offset,
						in_stripe_info->disable_bus);
				}
			}
		}
	}

	for (i = 0; i < ope_request->num_batch; i++) {
		in_frame_set = &in_frame_process->frame_set[i];
		ope_request->num_io_bufs[i] = in_frame_set->num_io_bufs;
		if (in_frame_set->num_io_bufs > OPE_MAX_IO_BUFS) {
			CAM_ERR(CAM_OPE, "Wrong number of io buffers: %d",
				in_frame_set->num_io_bufs);
			return -EINVAL;
		}

		for (j = 0; j < in_frame_set->num_io_bufs; j++) {
			in_io_buf = &in_frame_set->io_buf[j];
			io_buf = &ope_request->io_buf[i][j];
			if (in_io_buf->num_planes > OPE_MAX_PLANES) {
				CAM_ERR(CAM_OPE, "wrong number of planes: %u",
					in_io_buf->num_planes);
				return -EINVAL;
			}

			io_buf->num_planes = in_io_buf->num_planes;
			io_buf->resource_type = in_io_buf->resource_type;
			io_buf->direction = in_io_buf->direction;
			io_buf->fence = in_io_buf->fence;
			io_buf->format = in_io_buf->format;

			rc = cam_ope_mgr_get_rsc_idx(ctx_data, in_io_buf);
			if (rc < 0) {
				CAM_ERR(CAM_OPE, "Invalid rsc idx = %d", rc);
				return rc;
			}
			rsc_idx = rc;
			if (in_io_buf->direction == CAM_BUF_INPUT) {
				in_res =
					&ctx_data->ope_acquire.in_res[rsc_idx];
				alignment = in_res->alignment;
				unpack_format = in_res->unpacker_format;
				pack_format = 0;
			} else if (in_io_buf->direction == CAM_BUF_OUTPUT) {
				out_res =
					&ctx_data->ope_acquire.out_res[rsc_idx];
				alignment = out_res->alignment;
				pack_format = out_res->packer_format;
				unpack_format = 0;
			}

			CAM_DBG(CAM_OPE, "i:%d j:%d dir:%d rsc type:%d fmt:%d",
				i, j, io_buf->direction, io_buf->resource_type,
				io_buf->format);
			for (k = 0; k < in_io_buf->num_planes; k++) {
				io_buf->num_stripes[k] =
					in_io_buf->num_stripes[k];
				rc = cam_mem_get_io_buf(
					in_io_buf->mem_handle[k],
					hw_mgr->iommu_hdl, &iova_addr, &len);
				if (rc) {
					CAM_ERR(CAM_OPE, "get buf failed: %d",
						rc);
					return -EINVAL;
				}
				if (len < in_io_buf->length[k]) {
					CAM_ERR(CAM_OPE, "Invalid length");
					return -EINVAL;
				}
				iova_addr += in_io_buf->plane_offset[k];
				for (l = 0; l < in_io_buf->num_stripes[k];
					l++) {
					in_stripe_info =
						&in_io_buf->stripe_info[k][l];
					stripe_info = &io_buf->s_io[k][l];
					stripe_info->offset =
						in_stripe_info->offset;
					stripe_info->format = in_io_buf->format;
					stripe_info->s_location =
						in_stripe_info->stripe_location;
					stripe_info->iova_addr =
						iova_addr + stripe_info->offset;
					stripe_info->width =
						in_stripe_info->width;
					stripe_info->height =
						in_io_buf->height[k];
					stripe_info->stride =
						in_io_buf->plane_stride[k];
					stripe_info->x_init =
						in_stripe_info->x_init;
					stripe_info->len = len;
					stripe_info->alignment = alignment;
					stripe_info->pack_format = pack_format;
					stripe_info->unpack_format =
						unpack_format;
					cam_ope_mgr_print_stripe_info(i, j,
						k, l, stripe_info, iova_addr);
				}
			}
		}
	}

	return rc;
}

static int cam_ope_mgr_process_cmd_buf_req(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_packet *packet, struct cam_ope_ctx *ctx_data,
	uintptr_t frame_process_addr, size_t length, uint32_t req_idx)
{
	int rc = 0;
	int i, j;
	uint64_t iova_addr;
	uint64_t iova_cdm_addr;
	uintptr_t cpu_addr;
	size_t len;
	struct ope_frame_process *frame_process;
	struct ope_cmd_buf_info *cmd_buf;
	struct cam_ope_request *ope_request;
	bool is_kmd_buf_valid = false;

	frame_process = (struct ope_frame_process *)frame_process_addr;

	if (frame_process->batch_size > OPE_MAX_BATCH_SIZE) {
		CAM_ERR(CAM_OPE, "Invalid batch: %d",
			frame_process->batch_size);
		return -EINVAL;
	}

	for (i = 0; i < frame_process->batch_size; i++) {
		if (frame_process->num_cmd_bufs[i] > OPE_MAX_CMD_BUFS) {
			CAM_ERR(CAM_OPE, "Invalid cmd bufs for batch %d %d",
				i, frame_process->num_cmd_bufs[i]);
			return -EINVAL;
		}
	}

	CAM_DBG(CAM_OPE, "cmd buf for req id = %lld b_size = %d",
		packet->header.request_id, frame_process->batch_size);

	for (i = 0; i < frame_process->batch_size; i++) {
		CAM_DBG(CAM_OPE, "batch: %d count %d", i,
			frame_process->num_cmd_bufs[i]);
		for (j = 0; j < frame_process->num_cmd_bufs[i]; j++) {
			CAM_DBG(CAM_OPE, "batch: %d cmd_buf_idx :%d mem_hdl:%x",
				i, j, frame_process->cmd_buf[i][j].mem_handle);
			CAM_DBG(CAM_OPE, "size = %u scope = %d buf_type = %d",
				frame_process->cmd_buf[i][j].size,
				frame_process->cmd_buf[i][j].cmd_buf_scope,
				frame_process->cmd_buf[i][j].type);
			CAM_DBG(CAM_OPE, "usage = %d buffered = %d s_idx = %d",
			frame_process->cmd_buf[i][j].cmd_buf_usage,
			frame_process->cmd_buf[i][j].cmd_buf_buffered,
			frame_process->cmd_buf[i][j].stripe_idx);
		}
	}

	ope_request = ctx_data->req_list[req_idx];
	ope_request->num_batch = frame_process->batch_size;

	for (i = 0; i < frame_process->batch_size; i++) {
		for (j = 0; j < frame_process->num_cmd_bufs[i]; j++) {
			cmd_buf = &frame_process->cmd_buf[i][j];

			switch (cmd_buf->cmd_buf_scope) {
			case OPE_CMD_BUF_SCOPE_FRAME: {
				rc = cam_mem_get_io_buf(cmd_buf->mem_handle,
					hw_mgr->iommu_hdl, &iova_addr, &len);
				if (rc) {
					CAM_ERR(CAM_OPE, "get cmd buffailed %x",
						hw_mgr->iommu_hdl);
					goto end;
				}
				iova_addr = iova_addr + cmd_buf->offset;

				rc = cam_mem_get_io_buf(cmd_buf->mem_handle,
					hw_mgr->iommu_cdm_hdl,
					&iova_cdm_addr, &len);
				if (rc) {
					CAM_ERR(CAM_OPE, "get cmd buffailed %x",
						hw_mgr->iommu_hdl);
					goto end;
				}
				iova_cdm_addr = iova_cdm_addr + cmd_buf->offset;

				rc = cam_mem_get_cpu_buf(cmd_buf->mem_handle,
					&cpu_addr, &len);
				if (rc || !cpu_addr) {
					CAM_ERR(CAM_OPE, "get cmd buffailed %x",
						hw_mgr->iommu_hdl);
					goto end;
				}
				cpu_addr = cpu_addr +
					frame_process->cmd_buf[i][j].offset;
				CAM_DBG(CAM_OPE, "Hdl %x size %d len %d off %d",
					cmd_buf->mem_handle, cmd_buf->size,
					cmd_buf->length,
					cmd_buf->offset);
				if (cmd_buf->cmd_buf_usage == OPE_CMD_BUF_KMD) {
					ope_request->ope_kmd_buf.mem_handle =
						cmd_buf->mem_handle;
					ope_request->ope_kmd_buf.cpu_addr =
						cpu_addr;
					ope_request->ope_kmd_buf.iova_addr =
						iova_addr;
					ope_request->ope_kmd_buf.iova_cdm_addr =
						iova_cdm_addr;
					ope_request->ope_kmd_buf.len = len;
					ope_request->ope_kmd_buf.size =
						cmd_buf->size;
					is_kmd_buf_valid = true;
					CAM_DBG(CAM_OPE, "kbuf:%x io:%x cdm:%x",
					ope_request->ope_kmd_buf.cpu_addr,
					ope_request->ope_kmd_buf.iova_addr,
					ope_request->ope_kmd_buf.iova_cdm_addr);
					break;
				} else if (cmd_buf->cmd_buf_usage ==
					OPE_CMD_BUF_DEBUG) {
					ope_request->ope_debug_buf.cpu_addr =
						cpu_addr;
					ope_request->ope_debug_buf.iova_addr =
						iova_addr;
					ope_request->ope_debug_buf.len =
						len;
					ope_request->ope_debug_buf.size =
						cmd_buf->size;
					CAM_DBG(CAM_OPE, "dbg buf = %x",
					ope_request->ope_debug_buf.cpu_addr);
					break;
				}
				break;
			}
			case OPE_CMD_BUF_SCOPE_STRIPE: {
				uint32_t num_cmd_bufs = 0;
				uint32_t s_idx = 0;

				s_idx = cmd_buf->stripe_idx;
				num_cmd_bufs =
				ope_request->num_stripe_cmd_bufs[i][s_idx];

				if (!num_cmd_bufs)
					ope_request->num_stripes[i]++;

				ope_request->num_stripe_cmd_bufs[i][s_idx]++;
				break;
			}

			default:
				break;
			}
		}
	}


	for (i = 0; i < frame_process->batch_size; i++) {
		CAM_DBG(CAM_OPE, "num of stripes for batch %d is %d",
			i, ope_request->num_stripes[i]);
		for (j = 0; j < ope_request->num_stripes[i]; j++) {
			CAM_DBG(CAM_OPE, "cmd buffers for stripe: %d:%d is %d",
				i, j, ope_request->num_stripe_cmd_bufs[i][j]);
		}
	}

	if (!is_kmd_buf_valid) {
		CAM_DBG(CAM_OPE, "Invalid kmd buffer");
		rc = -EINVAL;
	}
end:
	return rc;
}

static int cam_ope_mgr_process_cmd_desc(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_packet *packet, struct cam_ope_ctx *ctx_data,
	uintptr_t *ope_cmd_buf_addr, uint32_t req_idx)
{
	int rc = 0;
	int i;
	int num_cmd_buf = 0;
	size_t len;
	struct cam_cmd_buf_desc *cmd_desc = NULL;
	uintptr_t cpu_addr = 0;
	struct cam_ope_request *ope_request;

	cmd_desc = (struct cam_cmd_buf_desc *)
		((uint32_t *) &packet->payload + packet->cmd_buf_offset/4);

	*ope_cmd_buf_addr = 0;
	for (i = 0; i < packet->num_cmd_buf; i++, num_cmd_buf++) {
		if (cmd_desc[i].type != CAM_CMD_BUF_GENERIC ||
			cmd_desc[i].meta_data == OPE_CMD_META_GENERIC_BLOB)
			continue;

		rc = cam_mem_get_cpu_buf(cmd_desc[i].mem_handle,
			&cpu_addr, &len);
		if (rc || !cpu_addr) {
			CAM_ERR(CAM_OPE, "get cmd buf failed %x",
				hw_mgr->iommu_hdl);
			num_cmd_buf = (num_cmd_buf > 0) ?
				num_cmd_buf-- : 0;
			goto end;
		}
		if ((len <= cmd_desc[i].offset) ||
			(cmd_desc[i].size < cmd_desc[i].length) ||
			((len - cmd_desc[i].offset) <
			cmd_desc[i].length)) {
			CAM_ERR(CAM_OPE, "Invalid offset or length");
			goto end;
		}
		cpu_addr = cpu_addr + cmd_desc[i].offset;
		*ope_cmd_buf_addr = cpu_addr;
	}

	if (!cpu_addr) {
		CAM_ERR(CAM_OPE, "invalid number of cmd buf");
		*ope_cmd_buf_addr = 0;
		return -EINVAL;
	}

	ope_request = ctx_data->req_list[req_idx];
	ope_request->request_id = packet->header.request_id;
	ope_request->req_idx = req_idx;

	rc = cam_ope_mgr_process_cmd_buf_req(hw_mgr, packet, ctx_data,
		cpu_addr, len, req_idx);
	if (rc) {
		CAM_ERR(CAM_OPE, "Process OPE cmd request is failed: %d", rc);
		goto end;
	}

	rc = cam_ope_mgr_process_cmd_io_buf_req(hw_mgr, packet, ctx_data,
		cpu_addr, len, req_idx);
	if (rc) {
		CAM_ERR(CAM_OPE, "Process OPE cmd io request is failed: %d",
			rc);
		goto end;
	}

	return rc;

end:
	*ope_cmd_buf_addr = 0;
	return rc;
}

static bool cam_ope_mgr_is_valid_inconfig(struct cam_packet *packet)
{
	int i, num_in_map_entries = 0;
	bool in_config_valid = false;
	struct cam_buf_io_cfg *io_cfg_ptr = NULL;

	io_cfg_ptr = (struct cam_buf_io_cfg *) ((uint32_t *) &packet->payload +
					packet->io_configs_offset/4);

	for (i = 0 ; i < packet->num_io_configs; i++)
		if (io_cfg_ptr[i].direction == CAM_BUF_INPUT)
			num_in_map_entries++;

	if (num_in_map_entries <= OPE_IN_RES_MAX) {
		in_config_valid = true;
	} else {
		CAM_ERR(CAM_OPE, "In config entries(%u) more than allowed(%u)",
				num_in_map_entries, OPE_IN_RES_MAX);
	}

	CAM_DBG(CAM_OPE, "number of in_config info: %u %u %u %u",
			packet->num_io_configs, OPE_MAX_IO_BUFS,
			num_in_map_entries, OPE_IN_RES_MAX);

	return in_config_valid;
}

static bool cam_ope_mgr_is_valid_outconfig(struct cam_packet *packet)
{
	int i, num_out_map_entries = 0;
	bool out_config_valid = false;
	struct cam_buf_io_cfg *io_cfg_ptr = NULL;

	io_cfg_ptr = (struct cam_buf_io_cfg *) ((uint32_t *) &packet->payload +
					packet->io_configs_offset/4);

	for (i = 0 ; i < packet->num_io_configs; i++)
		if (io_cfg_ptr[i].direction == CAM_BUF_OUTPUT)
			num_out_map_entries++;

	if (num_out_map_entries <= OPE_OUT_RES_MAX) {
		out_config_valid = true;
	} else {
		CAM_ERR(CAM_OPE, "Out config entries(%u) more than allowed(%u)",
				num_out_map_entries, OPE_OUT_RES_MAX);
	}

	CAM_DBG(CAM_OPE, "number of out_config info: %u %u %u %u",
			packet->num_io_configs, OPE_MAX_IO_BUFS,
			num_out_map_entries, OPE_OUT_RES_MAX);

	return out_config_valid;
}

static int cam_ope_mgr_pkt_validation(struct cam_packet *packet)
{
	if ((packet->header.op_code & 0xff) !=
		OPE_OPCODE_CONFIG) {
		CAM_ERR(CAM_OPE, "Invalid Opcode in pkt: %d",
			packet->header.op_code & 0xff);
		return -EINVAL;
	}

	if (packet->num_io_configs > OPE_MAX_IO_BUFS) {
		CAM_ERR(CAM_OPE, "Invalid number of io configs: %d %d",
			OPE_MAX_IO_BUFS, packet->num_io_configs);
		return -EINVAL;
	}

	if (packet->num_cmd_buf > OPE_PACKET_MAX_CMD_BUFS) {
		CAM_ERR(CAM_OPE, "Invalid number of cmd buffers: %d %d",
			OPE_PACKET_MAX_CMD_BUFS, packet->num_cmd_buf);
		return -EINVAL;
	}

	if (!cam_ope_mgr_is_valid_inconfig(packet) ||
		!cam_ope_mgr_is_valid_outconfig(packet)) {
		return -EINVAL;
	}

	CAM_DBG(CAM_OPE, "number of cmd/patch info: %u %u %u %u",
			packet->num_cmd_buf,
			packet->num_io_configs, OPE_MAX_IO_BUFS,
			packet->num_patches);
	return 0;
}

static int cam_ope_get_acquire_info(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_hw_acquire_args *args,
	struct cam_ope_ctx *ctx)
{
	int i = 0;

	if (args->num_acq > 1) {
		CAM_ERR(CAM_OPE, "Invalid number of resources: %d",
			args->num_acq);
		return -EINVAL;
	}

	if (args->acquire_info_size <
		sizeof(struct ope_acquire_dev_info)) {
		CAM_ERR(CAM_OPE, "Invalid acquire size = %d",
			args->acquire_info_size);
		return -EINVAL;
	}

	if (copy_from_user(&ctx->ope_acquire,
		(void __user *)args->acquire_info,
		sizeof(struct ope_acquire_dev_info))) {
		CAM_ERR(CAM_OPE, "Failed in acquire");
		return -EFAULT;
	}

	if (ctx->ope_acquire.secure_mode > CAM_SECURE_MODE_SECURE) {
		CAM_ERR(CAM_OPE, "Invalid mode:%d",
			ctx->ope_acquire.secure_mode);
		return -EINVAL;
	}

	if (ctx->ope_acquire.num_out_res > OPE_OUT_RES_MAX) {
		CAM_ERR(CAM_OPE, "num of out resources exceeding : %u",
			ctx->ope_acquire.num_out_res);
		return -EINVAL;
	}

	if (ctx->ope_acquire.num_in_res > OPE_IN_RES_MAX) {
		CAM_ERR(CAM_OPE, "num of in resources exceeding : %u",
			ctx->ope_acquire.num_in_res);
		return -EINVAL;
	}

	if (ctx->ope_acquire.dev_type >= OPE_DEV_TYPE_MAX) {
		CAM_ERR(CAM_OPE, "Invalid device type: %d",
			ctx->ope_acquire.dev_type);
		return -EFAULT;
	}

	if (ctx->ope_acquire.hw_type >= OPE_HW_TYPE_MAX) {
		CAM_ERR(CAM_OPE, "Invalid HW type: %d",
			ctx->ope_acquire.hw_type);
		return -EFAULT;
	}

	CAM_DBG(CAM_OPE, "top: %u %u %s %u %u %u %u %u",
		ctx->ope_acquire.hw_type, ctx->ope_acquire.dev_type,
		ctx->ope_acquire.dev_name,
		ctx->ope_acquire.nrt_stripes_for_arb,
		ctx->ope_acquire.secure_mode, ctx->ope_acquire.batch_size,
		ctx->ope_acquire.num_in_res, ctx->ope_acquire.num_out_res);

	for (i = 0; i < ctx->ope_acquire.num_in_res; i++) {
		CAM_DBG(CAM_OPE, "IN: %u %u %u %u %u %u %u %u",
		ctx->ope_acquire.in_res[i].res_id,
		ctx->ope_acquire.in_res[i].format,
		ctx->ope_acquire.in_res[i].width,
		ctx->ope_acquire.in_res[i].height,
		ctx->ope_acquire.in_res[i].alignment,
		ctx->ope_acquire.in_res[i].unpacker_format,
		ctx->ope_acquire.in_res[i].max_stripe_size,
		ctx->ope_acquire.in_res[i].fps);
	}

	for (i = 0; i < ctx->ope_acquire.num_out_res; i++) {
		CAM_DBG(CAM_OPE, "OUT: %u %u %u %u %u %u %u %u",
		ctx->ope_acquire.out_res[i].res_id,
		ctx->ope_acquire.out_res[i].format,
		ctx->ope_acquire.out_res[i].width,
		ctx->ope_acquire.out_res[i].height,
		ctx->ope_acquire.out_res[i].alignment,
		ctx->ope_acquire.out_res[i].packer_format,
		ctx->ope_acquire.out_res[i].subsample_period,
		ctx->ope_acquire.out_res[i].subsample_pattern);
	}

	return 0;
}

static int cam_ope_get_free_ctx(struct cam_ope_hw_mgr *hw_mgr)
{
	int i;

	i = find_first_zero_bit(hw_mgr->ctx_bitmap, hw_mgr->ctx_bits);
	if (i >= OPE_CTX_MAX || i < 0) {
		CAM_ERR(CAM_OPE, "Invalid ctx id = %d", i);
		return -EINVAL;
	}

	mutex_lock(&hw_mgr->ctx[i].ctx_mutex);
	if (hw_mgr->ctx[i].ctx_state != OPE_CTX_STATE_FREE) {
		CAM_ERR(CAM_OPE, "Invalid ctx %d state %d",
			i, hw_mgr->ctx[i].ctx_state);
		mutex_unlock(&hw_mgr->ctx[i].ctx_mutex);
		return -EINVAL;
	}
	set_bit(i, hw_mgr->ctx_bitmap);
	mutex_unlock(&hw_mgr->ctx[i].ctx_mutex);

	return i;
}


static int cam_ope_put_free_ctx(struct cam_ope_hw_mgr *hw_mgr, uint32_t ctx_id)
{
	if (ctx_id >= OPE_CTX_MAX) {
		CAM_ERR(CAM_OPE, "Invalid ctx_id: %d", ctx_id);
		return 0;
	}

	hw_mgr->ctx[ctx_id].ctx_state = OPE_CTX_STATE_FREE;
	clear_bit(ctx_id, hw_mgr->ctx_bitmap);

	return 0;
}

static int cam_ope_mgr_get_hw_caps(void *hw_priv, void *hw_caps_args)
{
	struct cam_ope_hw_mgr *hw_mgr;
	struct cam_query_cap_cmd *query_cap = hw_caps_args;
	struct ope_hw_ver hw_ver;
	int rc = 0, i;

	if (!hw_priv || !hw_caps_args) {
		CAM_ERR(CAM_OPE, "Invalid args: %x %x", hw_priv, hw_caps_args);
		return -EINVAL;
	}

	hw_mgr = hw_priv;
	mutex_lock(&hw_mgr->hw_mgr_mutex);
	if (copy_from_user(&hw_mgr->ope_caps,
		u64_to_user_ptr(query_cap->caps_handle),
		sizeof(struct ope_query_cap_cmd))) {
		CAM_ERR(CAM_OPE, "copy_from_user failed: size = %d",
			sizeof(struct ope_query_cap_cmd));
		rc = -EFAULT;
		goto end;
	}

	for (i = 0; i < hw_mgr->num_ope; i++) {
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.get_hw_caps(
			hw_mgr->ope_dev_intf[i]->hw_priv,
			&hw_ver, sizeof(hw_ver));
		if (rc)
			goto end;

		hw_mgr->ope_caps.hw_ver[i] = hw_ver;
	}

	hw_mgr->ope_caps.dev_iommu_handle.non_secure = hw_mgr->iommu_hdl;
	hw_mgr->ope_caps.dev_iommu_handle.secure = hw_mgr->iommu_sec_hdl;
	hw_mgr->ope_caps.cdm_iommu_handle.non_secure = hw_mgr->iommu_cdm_hdl;
	hw_mgr->ope_caps.cdm_iommu_handle.secure = hw_mgr->iommu_sec_cdm_hdl;
	hw_mgr->ope_caps.num_ope = hw_mgr->num_ope;

	CAM_DBG(CAM_OPE, "iommu sec %d iommu ns %d cdm s %d cdm ns %d",
		hw_mgr->ope_caps.dev_iommu_handle.secure,
		hw_mgr->ope_caps.dev_iommu_handle.non_secure,
		hw_mgr->ope_caps.cdm_iommu_handle.secure,
		hw_mgr->ope_caps.cdm_iommu_handle.non_secure);

	if (copy_to_user(u64_to_user_ptr(query_cap->caps_handle),
		&hw_mgr->ope_caps, sizeof(struct ope_query_cap_cmd))) {
		CAM_ERR(CAM_OPE, "copy_to_user failed: size = %d",
			sizeof(struct ope_query_cap_cmd));
		rc = -EFAULT;
	}

end:
	mutex_unlock(&hw_mgr->hw_mgr_mutex);
	return rc;
}

static int cam_ope_mgr_acquire_hw(void *hw_priv, void *hw_acquire_args)
{
	int rc = 0, i;
	int ctx_id;
	struct cam_ope_hw_mgr *hw_mgr = hw_priv;
	struct cam_ope_ctx *ctx;
	struct cam_hw_acquire_args *args = hw_acquire_args;
	struct cam_ope_dev_acquire ope_dev_acquire;
	struct cam_ope_dev_release ope_dev_release;
	struct cam_cdm_acquire_data cdm_acquire;
	struct cam_ope_dev_init init;
	struct cam_ope_dev_clk_update clk_update;
	struct cam_ope_dev_bw_update bw_update;
	struct cam_ope_set_irq_cb irq_cb;

	if ((!hw_priv) || (!hw_acquire_args)) {
		CAM_ERR(CAM_OPE, "Invalid args: %x %x",
			hw_priv, hw_acquire_args);
		return -EINVAL;
	}

	mutex_lock(&hw_mgr->hw_mgr_mutex);
	ctx_id = cam_ope_get_free_ctx(hw_mgr);
	if (ctx_id < 0) {
		CAM_ERR(CAM_OPE, "No free ctx");
		mutex_unlock(&hw_mgr->hw_mgr_mutex);
		return ctx_id;
	}

	ctx = &hw_mgr->ctx[ctx_id];
	ctx->ctx_id = ctx_id;
	mutex_lock(&ctx->ctx_mutex);
	rc = cam_ope_get_acquire_info(hw_mgr, args, ctx);
	if (rc < 0) {
		CAM_ERR(CAM_OPE, "get_acquire info failed: %d", rc);
		goto end;
	}


	if (!hw_mgr->ope_ctx_cnt) {
		for (i = 0; i < ope_hw_mgr->num_ope; i++) {
			init.hfi_en = ope_hw_mgr->hfi_en;
			rc = hw_mgr->ope_dev_intf[i]->hw_ops.init(
				hw_mgr->ope_dev_intf[i]->hw_priv, &init,
				sizeof(init));
			if (rc) {
				CAM_ERR(CAM_OPE, "OPE Dev init failed: %d", rc);
				goto end;
			}
		}

		/* Install IRQ CB */
		irq_cb.ope_hw_mgr_cb = cam_ope_hw_mgr_cb;
		irq_cb.data = hw_mgr;
		for (i = 0; i < ope_hw_mgr->num_ope; i++) {
			init.hfi_en = ope_hw_mgr->hfi_en;
			rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
				hw_mgr->ope_dev_intf[i]->hw_priv,
				OPE_HW_SET_IRQ_CB,
				&irq_cb, sizeof(irq_cb));
			if (rc) {
				CAM_ERR(CAM_OPE, "OPE Dev init failed: %d", rc);
				goto ope_irq_set_failed;
			}
		}
	}

	ope_dev_acquire.ctx_id = ctx_id;
	ope_dev_acquire.ope_acquire = &ctx->ope_acquire;

	for (i = 0; i < ope_hw_mgr->num_ope; i++) {
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_ACQUIRE,
			&ope_dev_acquire, sizeof(ope_dev_acquire));
		if (rc) {
			CAM_ERR(CAM_OPE, "OPE Dev acquire failed: %d", rc);
			goto ope_dev_acquire_failed;
		}
	}

	memset(&cdm_acquire, 0, sizeof(cdm_acquire));
	strlcpy(cdm_acquire.identifier, "ope", sizeof("ope"));
	if (ctx->ope_acquire.dev_type == OPE_DEV_TYPE_OPE_RT)
		cdm_acquire.priority = CAM_CDM_BL_FIFO_3;
	else if (ctx->ope_acquire.dev_type ==
		OPE_DEV_TYPE_OPE_NRT)
		cdm_acquire.priority = CAM_CDM_BL_FIFO_0;
	else
		goto ope_dev_acquire_failed;

	cdm_acquire.cell_index = 0;
	cdm_acquire.handle = 0;
	cdm_acquire.userdata = ctx;
	cdm_acquire.cam_cdm_callback = cam_ope_ctx_cdm_callback;
	cdm_acquire.id = CAM_CDM_VIRTUAL;
	cdm_acquire.base_array_cnt = 1;
	cdm_acquire.base_array[0] = hw_mgr->cdm_reg_map[OPE_DEV_OPE][0];

	rc = cam_cdm_acquire(&cdm_acquire);
	if (rc) {
		CAM_ERR(CAM_OPE, "cdm_acquire is failed: %d", rc);
		goto cdm_acquire_failed;
	}

	ctx->ope_cdm.cdm_ops = cdm_acquire.ops;
	ctx->ope_cdm.cdm_handle = cdm_acquire.handle;

	rc = cam_cdm_stream_on(cdm_acquire.handle);
	if (rc) {
		CAM_ERR(CAM_OPE, "cdm stream on failure: %d", rc);
		goto cdm_stream_on_failure;
	}

	for (i = 0; i < ope_hw_mgr->num_ope; i++) {
		clk_update.clk_rate = 600000000;
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_CLK_UPDATE,
			&clk_update, sizeof(clk_update));
		if (rc) {
			CAM_ERR(CAM_OPE, "OPE Dev clk update failed: %d", rc);
			goto ope_clk_update_failed;
		}
	}

	for (i = 0; i < ope_hw_mgr->num_ope; i++) {
		bw_update.axi_vote.num_paths = 1;
		bw_update.axi_vote_valid = true;
		bw_update.axi_vote.axi_path[0].camnoc_bw = 600000000;
		bw_update.axi_vote.axi_path[0].mnoc_ab_bw = 600000000;
		bw_update.axi_vote.axi_path[0].mnoc_ib_bw = 600000000;
		bw_update.axi_vote.axi_path[0].ddr_ab_bw = 600000000;
		bw_update.axi_vote.axi_path[0].ddr_ib_bw = 600000000;
		bw_update.axi_vote.axi_path[0].transac_type =
			CAM_AXI_TRANSACTION_WRITE;
		bw_update.axi_vote.axi_path[0].path_data_type =
			CAM_AXI_PATH_DATA_ALL;
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_BW_UPDATE,
			&bw_update, sizeof(bw_update));
		if (rc) {
			CAM_ERR(CAM_OPE, "OPE Dev clk update failed: %d", rc);
			goto ope_bw_update_failed;
		}
	}

	cam_ope_start_req_timer(ctx);
	hw_mgr->ope_ctx_cnt++;
	ctx->context_priv = args->context_data;
	args->ctxt_to_hw_map = ctx;
	ctx->ctxt_event_cb = args->event_cb;
	ctx->ctx_state = OPE_CTX_STATE_ACQUIRED;

	mutex_unlock(&ctx->ctx_mutex);
	mutex_unlock(&hw_mgr->hw_mgr_mutex);

	return rc;

ope_clk_update_failed:
ope_bw_update_failed:
cdm_stream_on_failure:
	cam_cdm_release(cdm_acquire.handle);
	ctx->ope_cdm.cdm_ops = NULL;
	ctx->ope_cdm.cdm_handle = 0;
cdm_acquire_failed:
	ope_dev_release.ctx_id = ctx_id;
	for (i = 0; i < ope_hw_mgr->num_ope; i++) {
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_RELEASE,
			&ope_dev_release, sizeof(ope_dev_release));
		if (rc)
			CAM_ERR(CAM_OPE, "OPE Dev release failed: %d", rc);
	}

ope_dev_acquire_failed:
	if (!hw_mgr->ope_ctx_cnt) {
		irq_cb.ope_hw_mgr_cb = NULL;
		irq_cb.data = hw_mgr;
		for (i = 0; i < ope_hw_mgr->num_ope; i++) {
			init.hfi_en = ope_hw_mgr->hfi_en;
			rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
				hw_mgr->ope_dev_intf[i]->hw_priv,
				OPE_HW_SET_IRQ_CB,
				&irq_cb, sizeof(irq_cb));
			CAM_ERR(CAM_OPE, "OPE IRQ de register failed");
		}
	}
ope_irq_set_failed:
	if (!hw_mgr->ope_ctx_cnt) {
		for (i = 0; i < ope_hw_mgr->num_ope; i++) {
			rc = hw_mgr->ope_dev_intf[i]->hw_ops.deinit(
				hw_mgr->ope_dev_intf[i]->hw_priv, NULL, 0);
			if (rc)
				CAM_ERR(CAM_OPE, "OPE deinit fail: %d", rc);
		}
	}
end:
	cam_ope_put_free_ctx(hw_mgr, ctx_id);
	mutex_unlock(&ctx->ctx_mutex);
	mutex_unlock(&hw_mgr->hw_mgr_mutex);
	return rc;
}

static int cam_ope_mgr_release_ctx(struct cam_ope_hw_mgr *hw_mgr, int ctx_id)
{
	int i = 0, rc = 0;
	struct cam_ope_dev_release ope_dev_release;

	if (ctx_id >= OPE_CTX_MAX) {
		CAM_ERR(CAM_OPE, "ctx_id is wrong: %d", ctx_id);
		return -EINVAL;
	}

	mutex_lock(&hw_mgr->ctx[ctx_id].ctx_mutex);
	if (hw_mgr->ctx[ctx_id].ctx_state !=
		OPE_CTX_STATE_ACQUIRED) {
		mutex_unlock(&hw_mgr->ctx[ctx_id].ctx_mutex);
		CAM_DBG(CAM_OPE, "ctx id: %d not in right state: %d",
			ctx_id, hw_mgr->ctx[ctx_id].ctx_state);
		return 0;
	}

	hw_mgr->ctx[ctx_id].ctx_state = OPE_CTX_STATE_RELEASE;

	for (i = 0; i < ope_hw_mgr->num_ope; i++) {
		ope_dev_release.ctx_id = ctx_id;
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_RELEASE,
			&ope_dev_release, sizeof(ope_dev_release));
		if (rc)
			CAM_ERR(CAM_OPE, "OPE Dev release failed: %d", rc);
	}

	rc = cam_cdm_stream_off(hw_mgr->ctx[ctx_id].ope_cdm.cdm_handle);
	if (rc)
		CAM_ERR(CAM_OPE, "OPE CDM streamoff failed: %d", rc);

	rc = cam_cdm_release(hw_mgr->ctx[ctx_id].ope_cdm.cdm_handle);
	if (rc)
		CAM_ERR(CAM_OPE, "OPE CDM relase failed: %d", rc);


	for (i = 0; i < CAM_CTX_REQ_MAX; i++) {
		if (!hw_mgr->ctx[ctx_id].req_list[i])
			continue;

		if (hw_mgr->ctx[ctx_id].req_list[i]->cdm_cmd) {
			kzfree(hw_mgr->ctx[ctx_id].req_list[i]->cdm_cmd);
			hw_mgr->ctx[ctx_id].req_list[i]->cdm_cmd = NULL;
		}
		kzfree(hw_mgr->ctx[ctx_id].req_list[i]);
		hw_mgr->ctx[ctx_id].req_list[i] = NULL;
		clear_bit(i, hw_mgr->ctx[ctx_id].bitmap);
	}

	cam_ope_req_timer_stop(&hw_mgr->ctx[ctx_id]);
	hw_mgr->ctx[ctx_id].ope_cdm.cdm_handle = 0;
	hw_mgr->ctx[ctx_id].req_cnt = 0;
	cam_ope_put_free_ctx(hw_mgr, ctx_id);
	hw_mgr->ope_ctx_cnt--;
	mutex_unlock(&hw_mgr->ctx[ctx_id].ctx_mutex);
	CAM_DBG(CAM_OPE, "X: ctx_id = %d", ctx_id);

	return 0;
}

static int cam_ope_mgr_release_hw(void *hw_priv, void *hw_release_args)
{
	int i, rc = 0;
	int ctx_id = 0;
	struct cam_hw_release_args *release_hw = hw_release_args;
	struct cam_ope_hw_mgr *hw_mgr = hw_priv;
	struct cam_ope_ctx *ctx_data = NULL;
	struct cam_ope_set_irq_cb irq_cb;
	struct cam_hw_intf *dev_intf;

	if (!release_hw || !hw_mgr) {
		CAM_ERR(CAM_OPE, "Invalid args: %pK %pK", release_hw, hw_mgr);
		return -EINVAL;
	}

	ctx_data = release_hw->ctxt_to_hw_map;
	if (!ctx_data) {
		CAM_ERR(CAM_OPE, "NULL ctx data");
		return -EINVAL;
	}

	ctx_id = ctx_data->ctx_id;
	if (ctx_id < 0 || ctx_id >= OPE_CTX_MAX) {
		CAM_ERR(CAM_OPE, "Invalid ctx id: %d", ctx_id);
		return -EINVAL;
	}

	mutex_lock(&hw_mgr->ctx[ctx_id].ctx_mutex);
	if (hw_mgr->ctx[ctx_id].ctx_state != OPE_CTX_STATE_ACQUIRED) {
		CAM_DBG(CAM_OPE, "ctx is not in use: %d", ctx_id);
		mutex_unlock(&hw_mgr->ctx[ctx_id].ctx_mutex);
		return -EINVAL;
	}
	mutex_unlock(&hw_mgr->ctx[ctx_id].ctx_mutex);

	mutex_lock(&hw_mgr->hw_mgr_mutex);
	rc = cam_ope_mgr_release_ctx(hw_mgr, ctx_id);
	if (!hw_mgr->ope_ctx_cnt) {
		CAM_DBG(CAM_OPE, "Last Release");
		if (!hw_mgr->ope_ctx_cnt) {
			for (i = 0; i < ope_hw_mgr->num_ope; i++) {
				dev_intf = hw_mgr->ope_dev_intf[i];
				irq_cb.ope_hw_mgr_cb = NULL;
				irq_cb.data = NULL;
				rc = dev_intf->hw_ops.process_cmd(
					hw_mgr->ope_dev_intf[i]->hw_priv,
					OPE_HW_SET_IRQ_CB,
					&irq_cb, sizeof(irq_cb));
				if (rc)
					CAM_ERR(CAM_OPE, "IRQ dereg failed: %d",
						rc);
			}
			for (i = 0; i < ope_hw_mgr->num_ope; i++) {
				dev_intf = hw_mgr->ope_dev_intf[i];
				rc = dev_intf->hw_ops.deinit(
					hw_mgr->ope_dev_intf[i]->hw_priv,
					NULL, 0);
				if (rc)
					CAM_ERR(CAM_OPE, "deinit failed: %d",
						rc);
			}
		}
	}

	mutex_unlock(&hw_mgr->hw_mgr_mutex);

	CAM_DBG(CAM_OPE, "Release done for ctx_id %d", ctx_id);
	return rc;
}

static int cam_ope_mgr_prepare_hw_update(void *hw_priv,
	void *hw_prepare_update_args)
{
	int rc = 0;
	struct cam_packet *packet = NULL;
	struct cam_ope_hw_mgr *hw_mgr = hw_priv;
	struct cam_hw_prepare_update_args *prepare_args =
		hw_prepare_update_args;
	struct cam_ope_ctx *ctx_data = NULL;
	uintptr_t   ope_cmd_buf_addr;
	uint32_t request_idx = 0;
	struct cam_ope_request *ope_req;

	if ((!prepare_args) || (!hw_mgr) || (!prepare_args->packet)) {
		CAM_ERR(CAM_OPE, "Invalid args: %x %x",
			prepare_args, hw_mgr);
		return -EINVAL;
	}

	ctx_data = prepare_args->ctxt_to_hw_map;
	if (!ctx_data) {
		CAM_ERR(CAM_OPE, "Invalid Context");
		return -EINVAL;
	}

	mutex_lock(&ctx_data->ctx_mutex);
	if (ctx_data->ctx_state != OPE_CTX_STATE_ACQUIRED) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "ctx id %u is not acquired state: %d",
			ctx_data->ctx_id, ctx_data->ctx_state);
		return -EINVAL;
	}

	packet = prepare_args->packet;
	rc = cam_packet_util_validate_packet(packet, prepare_args->remain_len);
	if (rc) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "packet validation is failed: %d", rc);
		return rc;
	}

	rc = cam_ope_mgr_pkt_validation(packet);
	if (rc) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "ope packet validation is failed");
		return -EINVAL;
	}

	rc = cam_packet_util_process_patches(packet, hw_mgr->iommu_cdm_hdl,
		hw_mgr->iommu_sec_cdm_hdl);
	if (rc) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "Patching is failed: %d", rc);
		return -EINVAL;
	}

	request_idx  = find_first_zero_bit(ctx_data->bitmap, ctx_data->bits);
	if (request_idx >= CAM_CTX_REQ_MAX || request_idx < 0) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "Invalid ctx req slot = %d", request_idx);
		return -EINVAL;
	}
	set_bit(request_idx, ctx_data->bitmap);

	ctx_data->req_list[request_idx] =
		kzalloc(sizeof(struct cam_ope_request), GFP_KERNEL);
	if (!ctx_data->req_list[request_idx]) {
		rc = -ENOMEM;
		mutex_unlock(&ctx_data->ctx_mutex);
		goto req_mem_alloc_failed;
	}

	ope_req = ctx_data->req_list[request_idx];
	ope_req->cdm_cmd =
		kzalloc(((sizeof(struct cam_cdm_bl_request)) +
			((OPE_MAX_CDM_BLS - 1) *
			sizeof(struct cam_cdm_bl_cmd))),
			GFP_KERNEL);
	if (!ope_req->cdm_cmd) {
		rc = -ENOMEM;
		mutex_unlock(&ctx_data->ctx_mutex);
		goto req_cdm_mem_alloc_failed;
	}

	rc = cam_ope_mgr_process_cmd_desc(hw_mgr, packet,
		ctx_data, &ope_cmd_buf_addr, request_idx);
	if (rc) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "cmd desc processing failed: %d", rc);
		goto end;
	}

	rc = cam_ope_mgr_process_io_cfg(hw_mgr, packet, prepare_args,
		ctx_data, request_idx);
	if (rc) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "IO cfg processing failed: %d", rc);
		goto end;
	}

	rc = cam_ope_mgr_create_kmd_buf(hw_mgr, packet, prepare_args,
		ctx_data, request_idx, ope_cmd_buf_addr);
	if (rc) {
		mutex_unlock(&ctx_data->ctx_mutex);
		CAM_ERR(CAM_OPE, "cam_ope_mgr_create_kmd_buf failed: %d", rc);
		goto end;
	}

	prepare_args->num_hw_update_entries = 1;
	prepare_args->hw_update_entries[0].addr =
		(uintptr_t)ctx_data->req_list[request_idx]->cdm_cmd;
	prepare_args->priv = ctx_data->req_list[request_idx];

	mutex_unlock(&ctx_data->ctx_mutex);

	return rc;

end:
	kzfree(ctx_data->req_list[request_idx]->cdm_cmd);
	ctx_data->req_list[request_idx]->cdm_cmd = NULL;
req_cdm_mem_alloc_failed:
	kzfree(ctx_data->req_list[request_idx]);
	ctx_data->req_list[request_idx] = NULL;
req_mem_alloc_failed:
	clear_bit(request_idx, ctx_data->bitmap);
	return rc;
}

static int cam_ope_mgr_handle_config_err(
	struct cam_hw_config_args *config_args,
	struct cam_ope_ctx *ctx_data)
{
	struct cam_hw_done_event_data buf_data;
	struct cam_ope_request *ope_req;
	uint32_t req_idx;

	ope_req = config_args->priv;

	buf_data.request_id = ope_req->request_id;
	ctx_data->ctxt_event_cb(ctx_data->context_priv, false, &buf_data);

	req_idx = ope_req->req_idx;
	ope_req->request_id = 0;
	kzfree(ctx_data->req_list[req_idx]->cdm_cmd);
	ctx_data->req_list[req_idx]->cdm_cmd = NULL;
	kzfree(ctx_data->req_list[req_idx]);
	ctx_data->req_list[req_idx] = NULL;
	clear_bit(req_idx, ctx_data->bitmap);

	return 0;
}

static int cam_ope_mgr_enqueue_config(struct cam_ope_hw_mgr *hw_mgr,
	struct cam_ope_ctx *ctx_data,
	struct cam_hw_config_args *config_args)
{
	int rc = 0;
	uint64_t request_id = 0;
	struct crm_workq_task *task;
	struct ope_cmd_work_data *task_data;
	struct cam_hw_update_entry *hw_update_entries;
	struct cam_ope_request *ope_req = NULL;

	ope_req = config_args->priv;
	request_id = ope_req->request_id;
	hw_update_entries = config_args->hw_update_entries;
	CAM_DBG(CAM_OPE, "req_id = %lld %pK", request_id, config_args->priv);

	task = cam_req_mgr_workq_get_task(ope_hw_mgr->cmd_work);
	if (!task) {
		CAM_ERR(CAM_OPE, "no empty task");
		return -ENOMEM;
	}

	task_data = (struct ope_cmd_work_data *)task->payload;
	task_data->data = (void *)hw_update_entries->addr;
	task_data->req_id = request_id;
	task_data->type = OPE_WORKQ_TASK_CMD_TYPE;
	task->process_cb = cam_ope_mgr_process_cmd;
	rc = cam_req_mgr_workq_enqueue_task(task, ctx_data,
		CRM_TASK_PRIORITY_0);

	return rc;
}

static int cam_ope_mgr_config_hw(void *hw_priv, void *hw_config_args)
{
	int rc = 0;
	struct cam_ope_hw_mgr *hw_mgr = hw_priv;
	struct cam_hw_config_args *config_args = hw_config_args;
	struct cam_ope_ctx *ctx_data = NULL;
	struct cam_ope_request *ope_req = NULL;
	struct cam_cdm_bl_request *cdm_cmd;

	CAM_DBG(CAM_OPE, "E");
	if (!hw_mgr || !config_args) {
		CAM_ERR(CAM_OPE, "Invalid arguments %pK %pK",
			hw_mgr, config_args);
		return -EINVAL;
	}

	if (!config_args->num_hw_update_entries) {
		CAM_ERR(CAM_OPE, "No hw update enteries are available");
		return -EINVAL;
	}

	ctx_data = config_args->ctxt_to_hw_map;
	mutex_lock(&hw_mgr->hw_mgr_mutex);
	mutex_lock(&ctx_data->ctx_mutex);
	if (ctx_data->ctx_state != OPE_CTX_STATE_ACQUIRED) {
		mutex_unlock(&ctx_data->ctx_mutex);
		mutex_unlock(&hw_mgr->hw_mgr_mutex);
		CAM_ERR(CAM_OPE, "ctx id :%u is not in use",
			ctx_data->ctx_id);
		return -EINVAL;
	}

	ope_req = config_args->priv;
	cdm_cmd = (struct cam_cdm_bl_request *)
		config_args->hw_update_entries->addr;
	cdm_cmd->cookie = ope_req->req_idx;

	rc = cam_ope_mgr_enqueue_config(hw_mgr, ctx_data, config_args);
	if (rc)
		goto config_err;

	CAM_DBG(CAM_OPE, "req_id %llu, io config", ope_req->request_id);

	cam_ope_req_timer_modify(ctx_data, 200);
	mutex_unlock(&ctx_data->ctx_mutex);
	mutex_unlock(&hw_mgr->hw_mgr_mutex);

	return rc;
config_err:
	cam_ope_mgr_handle_config_err(config_args, ctx_data);
	mutex_unlock(&ctx_data->ctx_mutex);
	mutex_unlock(&hw_mgr->hw_mgr_mutex);
	return rc;
}

static int cam_ope_mgr_hw_open_u(void *hw_priv, void *fw_download_args)
{
	struct cam_ope_hw_mgr *hw_mgr;
	int rc = 0;

	if (!hw_priv) {
		CAM_ERR(CAM_OPE, "Invalid args: %pK", hw_priv);
		return -EINVAL;
	}

	hw_mgr = hw_priv;
	if (!hw_mgr->open_cnt) {
		hw_mgr->open_cnt++;
	} else {
		rc = -EBUSY;
		CAM_ERR(CAM_OPE, "Multiple opens are not supported");
	}

	return rc;
}

static cam_ope_mgr_hw_close_u(void *hw_priv, void *hw_close_args)
{
	struct cam_ope_hw_mgr *hw_mgr;
	int rc = 0;

	if (!hw_priv) {
		CAM_ERR(CAM_OPE, "Invalid args: %pK", hw_priv);
		return -EINVAL;
	}

	hw_mgr = hw_priv;
	if (!hw_mgr->open_cnt) {
		rc = -EINVAL;
		CAM_ERR(CAM_OPE, "device is already closed");
	} else {
		hw_mgr->open_cnt--;
	}

	return rc;
}

static int cam_ope_mgr_flush_req(struct cam_ope_ctx *ctx_data,
	struct cam_hw_flush_args *flush_args)
{
	int idx;
	int64_t request_id;

	request_id = *(int64_t *)flush_args->flush_req_pending[0];
	for (idx = 0; idx < CAM_CTX_REQ_MAX; idx++) {
		if (!ctx_data->req_list[idx])
			continue;

		if (ctx_data->req_list[idx]->request_id != request_id)
			continue;

		ctx_data->req_list[idx]->request_id = 0;
		kzfree(ctx_data->req_list[idx]->cdm_cmd);
		ctx_data->req_list[idx]->cdm_cmd = NULL;
		kzfree(ctx_data->req_list[idx]);
		ctx_data->req_list[idx] = NULL;
		clear_bit(idx, ctx_data->bitmap);
	}

	return 0;
}

static int cam_ope_mgr_flush_all(struct cam_ope_ctx *ctx_data,
	struct cam_hw_flush_args *flush_args)
{
	int i, rc;
	struct cam_ope_hw_mgr *hw_mgr = ope_hw_mgr;

	rc = cam_cdm_flush_hw(ctx_data->ope_cdm.cdm_handle);

	for (i = 0; i < hw_mgr->num_ope; i++) {
		rc = hw_mgr->ope_dev_intf[i]->hw_ops.process_cmd(
			hw_mgr->ope_dev_intf[i]->hw_priv, OPE_HW_RESET,
			NULL, 0);
		if (rc)
			CAM_ERR(CAM_OPE, "OPE Dev reset failed: %d", rc);
	}

	for (i = 0; i < CAM_CTX_REQ_MAX; i++) {
		if (!ctx_data->req_list[i])
			continue;

		ctx_data->req_list[i]->request_id = 0;
		kzfree(ctx_data->req_list[i]->cdm_cmd);
		ctx_data->req_list[i]->cdm_cmd = NULL;
		kzfree(ctx_data->req_list[i]);
		ctx_data->req_list[i] = NULL;
		clear_bit(i, ctx_data->bitmap);
	}

	return rc;
}

static int cam_ope_mgr_hw_flush(void *hw_priv, void *hw_flush_args)
{
	struct cam_hw_flush_args *flush_args = hw_flush_args;
	struct cam_ope_ctx *ctx_data;

	if ((!hw_priv) || (!hw_flush_args)) {
		CAM_ERR(CAM_OPE, "Input params are Null");
		return -EINVAL;
	}

	ctx_data = flush_args->ctxt_to_hw_map;
	if (!ctx_data) {
		CAM_ERR(CAM_OPE, "Ctx data is NULL");
		return -EINVAL;
	}

	if ((flush_args->flush_type >= CAM_FLUSH_TYPE_MAX) ||
		(flush_args->flush_type < CAM_FLUSH_TYPE_REQ)) {
		CAM_ERR(CAM_OPE, "Invalid flush type: %d",
			flush_args->flush_type);
		return -EINVAL;
	}

	CAM_DBG(CAM_REQ, "ctx_id %d Flush type %d",
			ctx_data->ctx_id, flush_args->flush_type);

	switch (flush_args->flush_type) {
	case CAM_FLUSH_TYPE_ALL:
		mutex_lock(&ctx_data->ctx_mutex);
		cam_ope_mgr_flush_all(ctx_data, flush_args);
		mutex_unlock(&ctx_data->ctx_mutex);
		break;
	case CAM_FLUSH_TYPE_REQ:
		mutex_lock(&ctx_data->ctx_mutex);
		if (flush_args->num_req_active) {
			CAM_ERR(CAM_OPE, "Flush request is not supported");
			mutex_unlock(&ctx_data->ctx_mutex);
			return -EINVAL;
		}
		if (flush_args->num_req_pending)
			cam_ope_mgr_flush_req(ctx_data, flush_args);
		mutex_unlock(&ctx_data->ctx_mutex);
		break;
	default:
		CAM_ERR(CAM_OPE, "Invalid flush type: %d",
				flush_args->flush_type);
		return -EINVAL;
	}

	return 0;
}

static int cam_ope_mgr_alloc_devs(struct device_node *of_node)
{
	int rc;
	uint32_t num_dev;

	rc = of_property_read_u32(of_node, "num-ope", &num_dev);
	if (rc) {
		CAM_ERR(CAM_OPE, "getting num of ope failed: %d", rc);
		return -EINVAL;
	}

	ope_hw_mgr->devices[OPE_DEV_OPE] = kzalloc(
		sizeof(struct cam_hw_intf *) * num_dev, GFP_KERNEL);
	if (!ope_hw_mgr->devices[OPE_DEV_OPE])
		return -ENOMEM;

	return 0;
}

static int cam_ope_mgr_init_devs(struct device_node *of_node)
{
	int rc = 0;
	int count, i;
	const char *name = NULL;
	struct device_node *child_node = NULL;
	struct platform_device *child_pdev = NULL;
	struct cam_hw_intf *child_dev_intf = NULL;
	struct cam_hw_info *ope_dev;
	struct cam_hw_soc_info *soc_info = NULL;

	rc = cam_ope_mgr_alloc_devs(of_node);
	if (rc)
		return rc;

	count = of_property_count_strings(of_node, "compat-hw-name");
	if (!count) {
		CAM_ERR(CAM_OPE, "no compat hw found in dev tree, cnt = %d",
			count);
		rc = -EINVAL;
		goto compat_hw_name_failed;
	}

	for (i = 0; i < count; i++) {
		rc = of_property_read_string_index(of_node, "compat-hw-name",
			i, &name);
		if (rc) {
			CAM_ERR(CAM_OPE, "getting dev object name failed");
			goto compat_hw_name_failed;
		}

		child_node = of_find_node_by_name(NULL, name);
		if (!child_node) {
			CAM_ERR(CAM_OPE, "Cannot find node in dtsi %s", name);
			rc = -ENODEV;
			goto compat_hw_name_failed;
		}

		child_pdev = of_find_device_by_node(child_node);
		if (!child_pdev) {
			CAM_ERR(CAM_OPE, "failed to find device on bus %s",
				child_node->name);
			rc = -ENODEV;
			of_node_put(child_node);
			goto compat_hw_name_failed;
		}

		child_dev_intf = (struct cam_hw_intf *)platform_get_drvdata(
			child_pdev);
		if (!child_dev_intf) {
			CAM_ERR(CAM_OPE, "no child device");
			of_node_put(child_node);
			goto compat_hw_name_failed;
		}
		ope_hw_mgr->devices[child_dev_intf->hw_type]
			[child_dev_intf->hw_idx] = child_dev_intf;

		if (!child_dev_intf->hw_ops.process_cmd)
			goto compat_hw_name_failed;

		of_node_put(child_node);
	}

	ope_hw_mgr->num_ope = count;
	for (i = 0; i < count; i++) {
		ope_hw_mgr->ope_dev_intf[i] =
			ope_hw_mgr->devices[OPE_DEV_OPE][i];
			ope_dev = ope_hw_mgr->ope_dev_intf[i]->hw_priv;
			soc_info = &ope_dev->soc_info;
			ope_hw_mgr->cdm_reg_map[i][0] =
				soc_info->reg_map[0].mem_base;
	}

	ope_hw_mgr->hfi_en = of_property_read_bool(of_node, "hfi_en");

	return 0;
compat_hw_name_failed:
	kfree(ope_hw_mgr->devices[OPE_DEV_OPE]);
	ope_hw_mgr->devices[OPE_DEV_OPE] = NULL;
	return rc;
}

static int cam_ope_mgr_create_wq(void)
{

	int rc;
	int i;

	rc = cam_req_mgr_workq_create("ope_command_queue", OPE_WORKQ_NUM_TASK,
		&ope_hw_mgr->cmd_work, CRM_WORKQ_USAGE_NON_IRQ,
		0);
	if (rc) {
		CAM_ERR(CAM_OPE, "unable to create a command worker");
		goto cmd_work_failed;
	}

	rc = cam_req_mgr_workq_create("ope_message_queue", OPE_WORKQ_NUM_TASK,
		&ope_hw_mgr->msg_work, CRM_WORKQ_USAGE_IRQ, 0);
	if (rc) {
		CAM_ERR(CAM_OPE, "unable to create a message worker");
		goto msg_work_failed;
	}

	rc = cam_req_mgr_workq_create("ope_timer_queue", OPE_WORKQ_NUM_TASK,
		&ope_hw_mgr->timer_work, CRM_WORKQ_USAGE_IRQ, 0);
	if (rc) {
		CAM_ERR(CAM_OPE, "unable to create a timer worker");
		goto timer_work_failed;
	}

	ope_hw_mgr->cmd_work_data =
		kzalloc(sizeof(struct ope_cmd_work_data) * OPE_WORKQ_NUM_TASK,
		GFP_KERNEL);
	if (!ope_hw_mgr->cmd_work_data) {
		rc = -ENOMEM;
		goto cmd_work_data_failed;
	}

	ope_hw_mgr->msg_work_data =
		kzalloc(sizeof(struct ope_msg_work_data) * OPE_WORKQ_NUM_TASK,
		GFP_KERNEL);
	if (!ope_hw_mgr->msg_work_data) {
		rc = -ENOMEM;
		goto msg_work_data_failed;
	}

	ope_hw_mgr->timer_work_data =
		kzalloc(sizeof(struct ope_clk_work_data) * OPE_WORKQ_NUM_TASK,
		GFP_KERNEL);
	if (!ope_hw_mgr->timer_work_data) {
		rc = -ENOMEM;
		goto timer_work_data_failed;
	}

	for (i = 0; i < OPE_WORKQ_NUM_TASK; i++)
		ope_hw_mgr->msg_work->task.pool[i].payload =
				&ope_hw_mgr->msg_work_data[i];

	for (i = 0; i < OPE_WORKQ_NUM_TASK; i++)
		ope_hw_mgr->cmd_work->task.pool[i].payload =
				&ope_hw_mgr->cmd_work_data[i];

	for (i = 0; i < OPE_WORKQ_NUM_TASK; i++)
		ope_hw_mgr->timer_work->task.pool[i].payload =
				&ope_hw_mgr->timer_work_data[i];
	return 0;


timer_work_data_failed:
	kfree(ope_hw_mgr->msg_work_data);
msg_work_data_failed:
	kfree(ope_hw_mgr->cmd_work_data);
cmd_work_data_failed:
	cam_req_mgr_workq_destroy(&ope_hw_mgr->timer_work);
timer_work_failed:
	cam_req_mgr_workq_destroy(&ope_hw_mgr->msg_work);
msg_work_failed:
	cam_req_mgr_workq_destroy(&ope_hw_mgr->cmd_work);
cmd_work_failed:
	return rc;
}


int cam_ope_hw_mgr_init(struct device_node *of_node, uint64_t *hw_mgr_hdl,
	int *iommu_hdl)
{
	int i, rc = 0, j;
	struct cam_hw_mgr_intf *hw_mgr_intf;
	struct cam_iommu_handle cdm_handles;

	if (!of_node || !hw_mgr_hdl) {
		CAM_ERR(CAM_OPE, "Invalid args of_node %pK hw_mgr %pK",
			of_node, hw_mgr_hdl);
		return -EINVAL;
	}
	hw_mgr_intf = (struct cam_hw_mgr_intf *)hw_mgr_hdl;

	ope_hw_mgr = kzalloc(sizeof(struct cam_ope_hw_mgr), GFP_KERNEL);
	if (!ope_hw_mgr) {
		CAM_ERR(CAM_OPE, "Unable to allocate mem for: size = %d",
			sizeof(struct cam_ope_hw_mgr));
		return -ENOMEM;
	}

	hw_mgr_intf->hw_mgr_priv = ope_hw_mgr;
	hw_mgr_intf->hw_get_caps = cam_ope_mgr_get_hw_caps;
	hw_mgr_intf->hw_acquire = cam_ope_mgr_acquire_hw;
	hw_mgr_intf->hw_release = cam_ope_mgr_release_hw;
	hw_mgr_intf->hw_start   = NULL;
	hw_mgr_intf->hw_stop    = NULL;
	hw_mgr_intf->hw_prepare_update = cam_ope_mgr_prepare_hw_update;
	hw_mgr_intf->hw_config_stream_settings = NULL;
	hw_mgr_intf->hw_config = cam_ope_mgr_config_hw;
	hw_mgr_intf->hw_read   = NULL;
	hw_mgr_intf->hw_write  = NULL;
	hw_mgr_intf->hw_cmd = NULL;
	hw_mgr_intf->hw_open = cam_ope_mgr_hw_open_u;
	hw_mgr_intf->hw_close = cam_ope_mgr_hw_close_u;
	hw_mgr_intf->hw_flush = cam_ope_mgr_hw_flush;

	ope_hw_mgr->secure_mode = false;
	mutex_init(&ope_hw_mgr->hw_mgr_mutex);
	spin_lock_init(&ope_hw_mgr->hw_mgr_lock);

	for (i = 0; i < OPE_CTX_MAX; i++) {
		ope_hw_mgr->ctx[i].bitmap_size =
			BITS_TO_LONGS(CAM_CTX_REQ_MAX) *
			sizeof(long);
		ope_hw_mgr->ctx[i].bitmap = kzalloc(
			ope_hw_mgr->ctx[i].bitmap_size, GFP_KERNEL);
		if (!ope_hw_mgr->ctx[i].bitmap) {
			CAM_ERR(CAM_OPE, "bitmap allocation failed: size = %d",
				ope_hw_mgr->ctx[i].bitmap_size);
			rc = -ENOMEM;
			goto ope_ctx_bitmap_failed;
		}
		ope_hw_mgr->ctx[i].bits = ope_hw_mgr->ctx[i].bitmap_size *
			BITS_PER_BYTE;
		mutex_init(&ope_hw_mgr->ctx[i].ctx_mutex);
	}

	rc = cam_ope_mgr_init_devs(of_node);
	if (rc)
		goto dev_init_failed;

	ope_hw_mgr->ctx_bitmap_size =
		BITS_TO_LONGS(OPE_CTX_MAX) * sizeof(long);
	ope_hw_mgr->ctx_bitmap = kzalloc(ope_hw_mgr->ctx_bitmap_size,
		GFP_KERNEL);
	if (!ope_hw_mgr->ctx_bitmap) {
		rc = -ENOMEM;
		goto ctx_bitmap_alloc_failed;
	}

	ope_hw_mgr->ctx_bits = ope_hw_mgr->ctx_bitmap_size *
		BITS_PER_BYTE;

	rc = cam_smmu_get_handle("ope", &ope_hw_mgr->iommu_hdl);
	if (rc) {
		CAM_ERR(CAM_OPE, "get mmu handle failed: %d", rc);
		goto ope_get_hdl_failed;
	}

	rc = cam_smmu_get_handle("cam-secure", &ope_hw_mgr->iommu_sec_hdl);
	if (rc) {
		CAM_ERR(CAM_OPE, "get secure mmu handle failed: %d", rc);
		goto secure_hdl_failed;
	}

	rc = cam_cdm_get_iommu_handle("ope", &cdm_handles);
	if (rc) {
		CAM_ERR(CAM_OPE, "ope cdm handle get is failed: %d", rc);
		goto ope_cdm_hdl_failed;
	}

	ope_hw_mgr->iommu_cdm_hdl = cdm_handles.non_secure;
	ope_hw_mgr->iommu_sec_cdm_hdl = cdm_handles.secure;
	CAM_DBG(CAM_OPE, "iommu hdls %x %x cdm %x %x",
		ope_hw_mgr->iommu_hdl, ope_hw_mgr->iommu_sec_hdl,
		ope_hw_mgr->iommu_cdm_hdl,
		ope_hw_mgr->iommu_sec_cdm_hdl);

	rc = cam_ope_mgr_create_wq();
	if (rc)
		goto ope_wq_create_failed;

	if (iommu_hdl)
		*iommu_hdl = ope_hw_mgr->iommu_hdl;

	return rc;

ope_wq_create_failed:
	ope_hw_mgr->iommu_cdm_hdl = -1;
	ope_hw_mgr->iommu_sec_cdm_hdl = -1;
ope_cdm_hdl_failed:
	cam_smmu_destroy_handle(ope_hw_mgr->iommu_sec_hdl);
	ope_hw_mgr->iommu_sec_hdl = -1;
secure_hdl_failed:
	cam_smmu_destroy_handle(ope_hw_mgr->iommu_hdl);
	ope_hw_mgr->iommu_hdl = -1;
ope_get_hdl_failed:
	kzfree(ope_hw_mgr->ctx_bitmap);
	ope_hw_mgr->ctx_bitmap = NULL;
	ope_hw_mgr->ctx_bitmap_size = 0;
	ope_hw_mgr->ctx_bits = 0;
ctx_bitmap_alloc_failed:
	kzfree(ope_hw_mgr->devices[OPE_DEV_OPE]);
	ope_hw_mgr->devices[OPE_DEV_OPE] = NULL;
dev_init_failed:
ope_ctx_bitmap_failed:
	mutex_destroy(&ope_hw_mgr->hw_mgr_mutex);
	for (j = i - 1; j >= 0; j--) {
		mutex_destroy(&ope_hw_mgr->ctx[j].ctx_mutex);
		kzfree(ope_hw_mgr->ctx[j].bitmap);
		ope_hw_mgr->ctx[j].bitmap = NULL;
		ope_hw_mgr->ctx[j].bitmap_size = 0;
		ope_hw_mgr->ctx[j].bits = 0;
	}
	kzfree(ope_hw_mgr);
	ope_hw_mgr = NULL;

	return rc;
}
