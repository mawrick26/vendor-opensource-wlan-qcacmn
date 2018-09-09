/*
 * Copyright (c) 2017-2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: wlan_serialization_scan.c
 * This file defines the functions which deals with
 * serialization scan commands.
 */

#include "wlan_serialization_utils_i.h"
#include "wlan_serialization_main_i.h"
#include "wlan_serialization_api.h"
#include "wlan_serialization_scan_i.h"
#include <wlan_objmgr_vdev_obj.h>
#include <wlan_objmgr_pdev_obj.h>
#include <qdf_mc_timer.h>
#include <wlan_utility.h>

void
wlan_serialization_active_scan_cmd_count_handler(struct wlan_objmgr_psoc *psoc,
						 void *obj, void *arg)
{
	struct wlan_objmgr_pdev *pdev = obj;
	struct wlan_ser_pdev_obj *ser_pdev_obj;
	struct wlan_serialization_pdev_queue *pdev_q;
	uint32_t *count = arg;

	if (!pdev) {
		ser_err("invalid pdev");
		return;
	}

	ser_pdev_obj = wlan_objmgr_pdev_get_comp_private_obj(
			pdev, WLAN_UMAC_COMP_SERIALIZATION);

	pdev_q = &ser_pdev_obj->pdev_q[SER_PDEV_QUEUE_COMP_SCAN];
	*count += wlan_serialization_list_size(&pdev_q->active_list);
}

bool
wlan_serialization_is_active_scan_cmd_allowed(
		struct wlan_serialization_command *cmd)
{
	uint32_t count = 0;
	struct wlan_objmgr_pdev *pdev = NULL;
	struct wlan_objmgr_psoc *psoc;
	bool status = false;

	pdev = wlan_serialization_get_pdev_from_cmd(cmd);
	if (!pdev) {
		ser_err("invalid pdev");
		goto error;
	}

	psoc = wlan_pdev_get_psoc(pdev);
	if (!psoc) {
		ser_err("invalid psoc");
		goto error;
	}

	wlan_objmgr_iterate_obj_list(
			psoc, WLAN_PDEV_OP,
			wlan_serialization_active_scan_cmd_count_handler,
			&count, 1, WLAN_SERIALIZATION_ID);
	if (count < ucfg_scan_get_max_active_scans(psoc)) {
		ser_debug("count is [%d]", count);
		status =  true;
	}

error:
	return status;
}

bool wlan_ser_match_cmd_scan_id(
			qdf_list_node_t *nnode,
			struct wlan_serialization_command **cmd,
			uint16_t scan_id, struct wlan_objmgr_vdev *vdev)
{
	struct wlan_serialization_command_list *cmd_list = NULL;
	bool match_found = false;

	cmd_list = qdf_container_of(nnode,
				    struct wlan_serialization_command_list,
				    pdev_node);
	if ((cmd_list->cmd.cmd_id == scan_id) &&
	    (cmd_list->cmd.vdev == vdev)) {
		*cmd = &cmd_list->cmd;
		match_found = true;
	};

	ser_debug("match found: %d", match_found);

	return match_found;
}

enum wlan_serialization_status
wlan_ser_add_scan_cmd(
		struct wlan_ser_pdev_obj *ser_pdev_obj,
		struct wlan_serialization_command_list *cmd_list,
		uint8_t is_cmd_for_active_queue)
{
	enum wlan_serialization_status status;

	ser_debug("add scan cmd: type[%d] id[%d] prio[%d] blocking[%d]",
		  cmd_list->cmd.cmd_type,
		  cmd_list->cmd.cmd_id,
		  cmd_list->cmd.is_high_priority,
		  cmd_list->cmd.is_blocking);

	status = wlan_serialization_add_cmd_to_pdev_queue(
			ser_pdev_obj, cmd_list,
			is_cmd_for_active_queue);

	return status;
}

QDF_STATUS
wlan_ser_remove_scan_cmd(
		struct wlan_ser_pdev_obj *ser_pdev_obj,
		struct wlan_serialization_command_list **pcmd_list,
		struct wlan_serialization_command *cmd,
		uint8_t is_active_cmd)
{
	QDF_STATUS status;

	ser_debug("remove scan cmd: type[%d] id[%d] prio[%d] blocking[%d]",
		  cmd->cmd_type,
		  cmd->cmd_id,
		  cmd->is_high_priority,
		  cmd->is_blocking);

	status = wlan_serialization_remove_cmd_from_pdev_queue(
			ser_pdev_obj, pcmd_list, cmd, is_active_cmd);

	return status;
}

enum wlan_serialization_cmd_status
wlan_ser_cancel_scan_cmd(
		struct wlan_ser_pdev_obj *ser_obj,
		struct wlan_objmgr_pdev *pdev, struct wlan_objmgr_vdev *vdev,
		struct wlan_serialization_command *cmd,
		enum wlan_serialization_cmd_type cmd_type,
		uint8_t is_active_queue)
{
	qdf_list_t *queue;
	struct wlan_serialization_pdev_queue *pdev_q;
	uint32_t qsize;
	struct wlan_serialization_command_list *cmd_list = NULL;
	struct wlan_serialization_command cmd_bkup;
	qdf_list_node_t *nnode = NULL, *pnode = NULL;
	enum wlan_serialization_cmd_status status = WLAN_SER_CMD_NOT_FOUND;
	struct wlan_objmgr_psoc *psoc = NULL;
	QDF_STATUS qdf_status;

	ser_enter();

	pdev_q = &ser_obj->pdev_q[SER_PDEV_QUEUE_COMP_SCAN];

	if (is_active_queue)
		queue = &pdev_q->active_list;
	else
		queue = &pdev_q->pending_list;

	if (pdev)
		psoc = wlan_pdev_get_psoc(pdev);
	else if (vdev)
		psoc = wlan_vdev_get_psoc(vdev);
	else if (cmd && cmd->vdev)
		psoc = wlan_vdev_get_psoc(cmd->vdev);
	else
		ser_debug("Can't find psoc");

	wlan_serialization_acquire_lock(pdev_q);

	qsize = wlan_serialization_list_size(queue);
	while (!wlan_serialization_list_empty(queue) && qsize--) {
		if (wlan_serialization_get_cmd_from_queue(
					queue, &nnode) != QDF_STATUS_SUCCESS) {
			ser_err("can't read cmd from queue");
			status = WLAN_SER_CMD_NOT_FOUND;
			break;
		}
		cmd_list =
			qdf_container_of(nnode,
					 struct wlan_serialization_command_list,
					 pdev_node);

		if (cmd && !wlan_serialization_match_cmd_id_type(
							nnode, cmd,
							WLAN_SER_PDEV_NODE)) {
			pnode = nnode;
			continue;
		}
		if (vdev &&
		    !wlan_serialization_match_cmd_vdev(nnode,
						      vdev,
						      WLAN_SER_PDEV_NODE)) {
			pnode = nnode;
			continue;
		}

		if (pdev &&
		    !wlan_serialization_match_cmd_pdev(nnode,
						       pdev,
						       WLAN_SER_PDEV_NODE)) {
			pnode = nnode;
			continue;
		}

		/*
		 * active queue can't be removed directly, requester needs to
		 * wait for active command response and send remove request for
		 * active command separately
		 */
		if (is_active_queue) {
			if (!psoc || !cmd_list) {
				ser_err("psoc:0x%pK, cmd_list:0x%pK",
					psoc, cmd_list);
				status = WLAN_SER_CMD_NOT_FOUND;
				break;
			}

			/* Cancel request received for a cmd in active
			 * queue which has not been activated yet, we
			 * should assert here
			 */
			if (qdf_atomic_test_bit(CMD_MARKED_FOR_ACTIVATION,
						&cmd_list->cmd_in_use)) {
				wlan_serialization_release_lock(pdev_q);
				status = WLAN_SER_CMD_MARKED_FOR_ACTIVATION;
				goto error;
			}

			qdf_status = wlan_serialization_find_and_stop_timer(
							psoc, &cmd_list->cmd);
			if (QDF_STATUS_SUCCESS != qdf_status) {
				ser_err("Can't fix timer for active cmd");
				status = WLAN_SER_CMD_NOT_FOUND;
				break;
			}
			status = WLAN_SER_CMD_IN_ACTIVE_LIST;
		}

		qdf_mem_copy(&cmd_bkup, &cmd_list->cmd,
			     sizeof(struct wlan_serialization_command));

		qdf_status =
			wlan_serialization_remove_node(queue,
						       &cmd_list->pdev_node);

		if (qdf_status != QDF_STATUS_SUCCESS) {
			ser_err("can't remove cmd from pdev queue");
			status = WLAN_SER_CMD_NOT_FOUND;
			break;
		}

		qdf_mem_zero(&cmd_list->cmd,
			     sizeof(struct wlan_serialization_command));

		qdf_status = wlan_serialization_insert_back(
			&pdev_q->cmd_pool_list,
			&cmd_list->pdev_node);

		if (QDF_STATUS_SUCCESS != qdf_status) {
			ser_err("can't remove cmd from queue");
			status = WLAN_SER_CMD_NOT_FOUND;
			break;
		}
		nnode = pnode;

		wlan_serialization_release_lock(pdev_q);
		/*
		 * call pending cmd's callback to notify that
		 * it is being removed
		 */
		if (cmd_bkup.cmd_cb) {
			ser_debug("cmd cb: type[%d] id[%d]",
				  cmd_bkup.cmd_type,
				  cmd_bkup.cmd_id);
			ser_debug("reason: WLAN_SER_CB_CANCEL_CMD");

			cmd_bkup.cmd_cb(&cmd_bkup,
					WLAN_SER_CB_CANCEL_CMD);

			ser_debug("reason: WLAN_SER_CB_RELEASE_MEM_CMD");
			cmd_bkup.cmd_cb(&cmd_bkup,
					WLAN_SER_CB_RELEASE_MEM_CMD);
		}

		wlan_serialization_acquire_lock(pdev_q);

		if (!is_active_queue)
			status = WLAN_SER_CMD_IN_PENDING_LIST;
	}

	wlan_serialization_release_lock(pdev_q);

error:
	ser_exit();
	return status;
}

static struct wlan_serialization_command_list*
wlan_serialization_get_next_scan_active_cmd(
		struct wlan_ser_pdev_obj *ser_pdev_obj)
{
	qdf_list_t *pending_queue;
	qdf_list_node_t *pending_node = NULL;
	struct wlan_serialization_command_list *pending_cmd_list = NULL;
	struct wlan_serialization_pdev_queue *pdev_q;
	QDF_STATUS status;

	pdev_q = &ser_pdev_obj->pdev_q[SER_PDEV_QUEUE_COMP_SCAN];

	pending_queue = &pdev_q->pending_list;

	if (wlan_serialization_list_empty(pending_queue)) {
		ser_debug("nothing to move from pend to active que");
		goto error;
	}

	status = wlan_serialization_peek_front(pending_queue,
					       &pending_node);
	if (QDF_STATUS_SUCCESS != status) {
		ser_err("can't read from pending queue");
		goto error;
	}

	pending_cmd_list =
		qdf_container_of(pending_node,
				 struct wlan_serialization_command_list,
				 pdev_node);

	ser_debug("next active scan cmd found from pending queue");
error:
	return pending_cmd_list;
}

enum wlan_serialization_status wlan_ser_move_scan_pending_to_active(
		struct wlan_serialization_command_list **pcmd_list,
		struct wlan_ser_pdev_obj *ser_pdev_obj)
{
	struct wlan_serialization_command_list *pending_cmd_list;
	struct wlan_serialization_command_list *active_cmd_list;
	struct wlan_serialization_command cmd_to_remove;
	enum wlan_serialization_status status = WLAN_SER_CMD_DENIED_UNSPECIFIED;
	QDF_STATUS qdf_status;
	struct wlan_serialization_pdev_queue *pdev_q;

	pdev_q = &ser_pdev_obj->pdev_q[SER_PDEV_QUEUE_COMP_SCAN];

	ser_enter();

	if (!ser_pdev_obj) {
		ser_err("Can't find ser_pdev_obj");
		goto error;
	}

	pending_cmd_list =
		wlan_serialization_get_next_scan_active_cmd(ser_pdev_obj);

	if (!pending_cmd_list)
		goto error;

	qdf_mem_copy(&cmd_to_remove, &pending_cmd_list->cmd,
		     sizeof(struct wlan_serialization_command));

	qdf_status =
		wlan_ser_remove_scan_cmd(ser_pdev_obj,
					 &pending_cmd_list,
					 &cmd_to_remove, false);

	if (QDF_STATUS_SUCCESS != qdf_status) {
		ser_err("Can't remove cmd from pendingQ id-%d type-%d",
			pending_cmd_list->cmd.cmd_id,
			pending_cmd_list->cmd.cmd_type);
		QDF_ASSERT(0);
		status = WLAN_SER_CMD_DENIED_UNSPECIFIED;
		goto error;
	}

	active_cmd_list = pending_cmd_list;

	status = wlan_ser_add_scan_cmd(ser_pdev_obj,
				       active_cmd_list, true);

	if (WLAN_SER_CMD_ACTIVE != status) {
		wlan_serialization_insert_back(
			&pdev_q->cmd_pool_list,
			&active_cmd_list->pdev_node);

		status = WLAN_SER_CMD_DENIED_UNSPECIFIED;
		ser_err("Can't add cmd to activeQ id-%d type-%d",
			active_cmd_list->cmd.cmd_id,
			active_cmd_list->cmd.cmd_type);
		QDF_ASSERT(0);
		goto error;
	}

	qdf_atomic_set_bit(CMD_MARKED_FOR_ACTIVATION,
			   &active_cmd_list->cmd_in_use);

	*pcmd_list = active_cmd_list;

error:
	ser_exit();
	return status;
}
