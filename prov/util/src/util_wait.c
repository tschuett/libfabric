/*
 * Copyright (c) 2014-2016 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <ofi_enosys.h>
#include <ofi_util.h>
#include <ofi_epoll.h>


int ofi_trywait(struct fid_fabric *fabric, struct fid **fids, int count)
{
	struct util_cq *cq;
	struct util_eq *eq;
	struct util_cntr *cntr;
	struct util_wait *wait;
	int i, ret;

	for (i = 0; i < count; i++) {
		switch (fids[i]->fclass) {
		case FI_CLASS_CQ:
			cq = container_of(fids[i], struct util_cq, cq_fid.fid);
			wait = cq->wait;
			break;
		case FI_CLASS_EQ:
			eq = container_of(fids[i], struct util_eq, eq_fid.fid);
			wait = eq->wait;
			break;
		case FI_CLASS_CNTR:
			cntr = container_of(fids[i], struct util_cntr, cntr_fid.fid);
			wait = cntr->wait;
			break;
		case FI_CLASS_WAIT:
			wait = container_of(fids[i], struct util_wait, wait_fid.fid);
			break;
		default:
			return -FI_EINVAL;
		}

		ret = wait->wait_try(wait);
		if (ret)
			return ret;
	}
	return 0;
}

int ofi_check_wait_attr(const struct fi_provider *prov,
		        const struct fi_wait_attr *attr)
{
	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
	case FI_WAIT_POLLFD:
	case FI_WAIT_MUTEX_COND:
	case FI_WAIT_YIELD:
		break;
	default:
		FI_WARN(prov, FI_LOG_FABRIC, "invalid wait object type\n");
		return -FI_EINVAL;
	}

	if (attr->flags) {
		FI_WARN(prov, FI_LOG_FABRIC, "invalid flags\n");
		return -FI_EINVAL;
	}

	return 0;
}

int fi_wait_cleanup(struct util_wait *wait)
{
	struct ofi_wait_fid_entry *fid_entry;
	int ret;

	if (ofi_atomic_get32(&wait->ref))
		return -FI_EBUSY;

	ret = fi_close(&wait->pollset->poll_fid.fid);
	if (ret)
		return ret;

	while (!dlist_empty(&wait->fid_list)) {
		dlist_pop_front(&wait->fid_list, struct ofi_wait_fid_entry,
				fid_entry, entry);
		free(fid_entry);
	}

	fastlock_destroy(&wait->lock);
	ofi_atomic_dec32(&wait->fabric->ref);
	return 0;
}

int ofi_wait_init(struct util_fabric *fabric, struct fi_wait_attr *attr,
		  struct util_wait *wait)
{
	struct fid_poll *poll_fid;
	struct fi_poll_attr poll_attr;
	int ret;

	wait->prov = fabric->prov;
	ofi_atomic_initialize32(&wait->ref, 0);
	wait->wait_fid.fid.fclass = FI_CLASS_WAIT;

	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
		wait->wait_obj = FI_WAIT_FD;
		break;
	case FI_WAIT_FD:
	case FI_WAIT_POLLFD:
	case FI_WAIT_MUTEX_COND:
	case FI_WAIT_YIELD:
		wait->wait_obj = attr->wait_obj;
		break;
	default:
		assert(0);
		return -FI_EINVAL;
	}

	memset(&poll_attr, 0, sizeof poll_attr);
	ret = fi_poll_create_(fabric->prov, NULL, &poll_attr, &poll_fid);
	if (ret)
		return ret;

	wait->pollset = container_of(poll_fid, struct util_poll, poll_fid);
	fastlock_init(&wait->lock);
	dlist_init(&wait->fid_list);
	wait->fabric = fabric;
	ofi_atomic_inc32(&fabric->ref);
	return 0;
}

static int ofi_wait_match_fd(struct dlist_entry *item, const void *arg)
{
	struct ofi_wait_fd_entry *fd_entry;

	fd_entry = container_of(item, struct ofi_wait_fd_entry, entry);
	return fd_entry->fd == *(int *) arg;
}

int ofi_wait_del_fd(struct util_wait *wait, int fd)
{
	struct ofi_wait_fd_entry *fd_entry;
	struct dlist_entry *entry;
	struct util_wait_fd *wait_fd;
	int ret = 0;

	wait_fd = container_of(wait, struct util_wait_fd, util_wait);
	fastlock_acquire(&wait->lock);
	entry = dlist_find_first_match(&wait_fd->fd_list, ofi_wait_match_fd, &fd);
	if (!entry) {
		FI_INFO(wait->prov, FI_LOG_FABRIC,
			"Given fd (%d) not found in wait list - %p\n",
			fd, wait_fd);
		ret = -FI_EINVAL;
		goto out;
	}

	fd_entry = container_of(entry, struct ofi_wait_fd_entry, entry);
	if (ofi_atomic_dec32(&fd_entry->ref))
		goto out;

	dlist_remove(&fd_entry->entry);

	if (wait->wait_obj == FI_WAIT_FD)
		ofi_epoll_del(wait_fd->epoll_fd, fd_entry->fd);
	else
		ofi_pollfds_del(wait_fd->pollfds, fd_entry->fd);
	free(fd_entry);
	wait_fd->change_index++;
out:
	fastlock_release(&wait->lock);
	return ret;
}

int ofi_wait_add_fd(struct util_wait *wait, int fd, uint32_t events,
		    ofi_wait_try_func wait_try, void *arg, void *context)
{
	struct ofi_wait_fd_entry *fd_entry;
	struct dlist_entry *entry;
	struct util_wait_fd *wait_fd;
	int ret = 0;

	wait_fd = container_of(wait, struct util_wait_fd, util_wait);
	fastlock_acquire(&wait->lock);
	entry = dlist_find_first_match(&wait_fd->fd_list, ofi_wait_match_fd, &fd);
	if (entry) {
		FI_DBG(wait->prov, FI_LOG_EP_CTRL,
		       "Given fd (%d) already added to wait list - %p \n",
		       fd, wait_fd);
		fd_entry = container_of(entry, struct ofi_wait_fd_entry, entry);
		ofi_atomic_inc32(&fd_entry->ref);
		goto out;
	}

	ret = (wait->wait_obj == FI_WAIT_FD) ?
	      ofi_epoll_add(wait_fd->epoll_fd, fd, events, context) :
	      ofi_pollfds_add(wait_fd->pollfds, fd, events, context);
	if (ret) {
		FI_WARN(wait->prov, FI_LOG_FABRIC, "Unable to add fd to epoll\n");
		goto out;
	}

	fd_entry = calloc(1, sizeof *fd_entry);
	if (!fd_entry) {
		ret = -FI_ENOMEM;
		if (wait->wait_obj == FI_WAIT_FD)
			ofi_epoll_del(wait_fd->epoll_fd, fd);
		else
			ofi_pollfds_del(wait_fd->pollfds, fd);
		goto out;
	}

	fd_entry->fd = fd;
	fd_entry->wait_try = wait_try;
	fd_entry->arg = arg;
	ofi_atomic_initialize32(&fd_entry->ref, 1);

	dlist_insert_tail(&fd_entry->entry, &wait_fd->fd_list);
	wait_fd->change_index++;
out:
	fastlock_release(&wait->lock);
	return ret;
}

static void util_wait_fd_signal(struct util_wait *util_wait)
{
	struct util_wait_fd *wait;
	wait = container_of(util_wait, struct util_wait_fd, util_wait);
	fd_signal_set(&wait->signal);
}

static int util_wait_fd_try(struct util_wait *wait)
{
	struct ofi_wait_fid_entry *fid_entry;
	struct ofi_wait_fd_entry *fd_entry;
	struct util_wait_fd *wait_fd;
	void *context;
	int ret;

	wait_fd = container_of(wait, struct util_wait_fd, util_wait);
	fd_signal_reset(&wait_fd->signal);
	fastlock_acquire(&wait->lock);
	dlist_foreach_container(&wait_fd->fd_list, struct ofi_wait_fd_entry,
				fd_entry, entry) {
		ret = fd_entry->wait_try(fd_entry->arg);
		if (ret != FI_SUCCESS) {
			fastlock_release(&wait->lock);
			return ret;
		}
	}

	dlist_foreach_container(&wait->fid_list,
				struct ofi_wait_fid_entry, fid_entry, entry) {
		ret = fid_entry->wait_try(fid_entry->fid);
		if (ret != FI_SUCCESS) {
			fastlock_release(&wait->lock);
			return ret;
		}
	}

	fastlock_release(&wait->lock);
	ret = fi_poll(&wait->pollset->poll_fid, &context, 1);
	return (ret > 0) ? -FI_EAGAIN : (ret == -FI_EAGAIN) ? FI_SUCCESS : ret;
}

static int util_wait_fd_run(struct fid_wait *wait_fid, int timeout)
{
	struct util_wait_fd *wait;
	uint64_t endtime;
	void *ep_context[1];
	int ret;

	wait = container_of(wait_fid, struct util_wait_fd, util_wait.wait_fid);
	endtime = ofi_timeout_time(timeout);

	while (1) {
		ret = wait->util_wait.wait_try(&wait->util_wait);
		if (ret)
			return ret == -FI_EAGAIN ? 0 : ret;

		if (ofi_adjust_timeout(endtime, &timeout))
			return -FI_ETIMEDOUT;

		ret = (wait->util_wait.wait_obj == FI_WAIT_FD) ?
		      ofi_epoll_wait(wait->epoll_fd, ep_context, 1, timeout) :
		      ofi_pollfds_wait(wait->pollfds, ep_context, 1, timeout);
		if (ret > 0)
			return FI_SUCCESS;

		if (ret < 0) {
			FI_WARN(wait->util_wait.prov, FI_LOG_FABRIC,
				"poll failed\n");
			return ret;
		}
	}
}

static int util_wait_fd_control(struct fid *fid, int command, void *arg)
{
	struct util_wait_fd *wait;
	struct fi_wait_pollfd *pollfd;
	int ret;

	wait = container_of(fid, struct util_wait_fd, util_wait.wait_fid.fid);
	switch (command) {
	case FI_GETWAIT:
		if (wait->util_wait.wait_obj == FI_WAIT_FD) {
#ifdef HAVE_EPOLL
			*(int *) arg = wait->epoll_fd;
			return 0;
#else
			return -FI_ENODATA;
#endif
		}

		pollfd = arg;
		fastlock_acquire(&wait->util_wait.lock);
		if (pollfd->nfds >= wait->pollfds->nfds) {
			memcpy(pollfd->fd, &wait->pollfds->fds[0],
			       wait->pollfds->nfds * sizeof(*wait->pollfds->fds));
			ret = 0;
		} else {
			ret = -FI_ETOOSMALL;
		}
		pollfd->change_index = wait->change_index;
		pollfd->nfds = wait->pollfds->nfds;
		fastlock_release(&wait->util_wait.lock);
		break;
	case FI_GETWAITOBJ:
		*(enum fi_wait_obj *) arg = wait->util_wait.wait_obj;
		ret = 0;
		break;
	default:
		FI_INFO(wait->util_wait.prov, FI_LOG_FABRIC,
			"unsupported command\n");
		ret = -FI_ENOSYS;
		break;
	}
	return ret;
}

static int util_wait_fd_close(struct fid *fid)
{
	struct util_wait_fd *wait;
	struct ofi_wait_fd_entry *fd_entry;
	int ret;

	wait = container_of(fid, struct util_wait_fd, util_wait.wait_fid.fid);

	fastlock_acquire(&wait->util_wait.lock);
	while (!dlist_empty(&wait->fd_list)) {
		dlist_pop_front(&wait->fd_list, struct ofi_wait_fd_entry,
				fd_entry, entry);
		if (wait->util_wait.wait_obj == FI_WAIT_FD)
			ofi_epoll_del(wait->epoll_fd, fd_entry->fd);
		else
			ofi_pollfds_del(wait->pollfds, fd_entry->fd);
		free(fd_entry);
	}
	fastlock_release(&wait->util_wait.lock);

	ret = fi_wait_cleanup(&wait->util_wait);
	if (ret)
		return ret;

	ofi_epoll_del(wait->epoll_fd, wait->signal.fd[FI_READ_FD]);
	fd_signal_free(&wait->signal);

	if (wait->util_wait.wait_obj == FI_WAIT_FD)
		ofi_epoll_close(wait->epoll_fd);
	else
		ofi_epoll_close(wait->epoll_fd);
	free(wait);
	return 0;
}

static struct fi_ops_wait util_wait_fd_ops = {
	.size = sizeof(struct fi_ops_wait),
	.wait = util_wait_fd_run,
};

static struct fi_ops util_wait_fd_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = util_wait_fd_close,
	.bind = fi_no_bind,
	.control = util_wait_fd_control,
	.ops_open = fi_no_ops_open,
};

static int util_verify_wait_fd_attr(const struct fi_provider *prov,
				    const struct fi_wait_attr *attr)
{
	int ret;

	ret = ofi_check_wait_attr(prov, attr);
	if (ret)
		return ret;

	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
	case FI_WAIT_POLLFD:
		break;
	default:
		FI_WARN(prov, FI_LOG_FABRIC, "unsupported wait object\n");
		return -FI_EINVAL;
	}

	return 0;
}

int ofi_wait_fd_open(struct fid_fabric *fabric_fid, struct fi_wait_attr *attr,
		    struct fid_wait **waitset)
{
	struct util_fabric *fabric;
	struct util_wait_fd *wait;
	int ret;

	fabric = container_of(fabric_fid, struct util_fabric, fabric_fid);
	ret = util_verify_wait_fd_attr(fabric->prov, attr);
	if (ret)
		return ret;

	wait = calloc(1, sizeof(*wait));
	if (!wait)
		return -FI_ENOMEM;

	ret = ofi_wait_init(fabric, attr, &wait->util_wait);
	if (ret)
		goto err1;

	wait->util_wait.signal = util_wait_fd_signal;
	wait->util_wait.wait_try = util_wait_fd_try;
	ret = fd_signal_init(&wait->signal);
	if (ret)
		goto err2;

	ret = (wait->util_wait.wait_obj == FI_WAIT_FD) ?
	      ofi_epoll_create(&wait->epoll_fd) :
	      ofi_pollfds_create(&wait->pollfds);
	if (ret)
		goto err3;

	ret = (wait->util_wait.wait_obj == FI_WAIT_FD) ?
	      ofi_epoll_add(wait->epoll_fd, wait->signal.fd[FI_READ_FD],
			OFI_EPOLL_IN, &wait->util_wait.wait_fid.fid) :
	      ofi_pollfds_add(wait->pollfds, wait->signal.fd[FI_READ_FD],
			POLLIN, &wait->util_wait.wait_fid.fid);
	if (ret)
		goto err4;

	wait->util_wait.wait_fid.fid.ops = &util_wait_fd_fi_ops;
	wait->util_wait.wait_fid.ops = &util_wait_fd_ops;

	dlist_init(&wait->fd_list);

	*waitset = &wait->util_wait.wait_fid;
	return 0;

err4:
	if (wait->util_wait.wait_obj == FI_WAIT_FD)
		ofi_epoll_close(wait->epoll_fd);
	else
		ofi_pollfds_close(wait->pollfds);
err3:
	fd_signal_free(&wait->signal);
err2:
	fi_wait_cleanup(&wait->util_wait);
err1:
	free(wait);
	return ret;
}

static void util_wait_yield_signal(struct util_wait *util_wait)
{
	struct util_wait_yield *wait_yield;

	wait_yield = container_of(util_wait, struct util_wait_yield, util_wait);

	fastlock_acquire(&wait_yield->signal_lock);
	wait_yield->signal = 1;
	fastlock_release(&wait_yield->signal_lock);
}

static int util_wait_yield_run(struct fid_wait *wait_fid, int timeout)
{
	struct util_wait_yield *wait;
	struct ofi_wait_fid_entry *fid_entry;
	int ret = 0;

	wait = container_of(wait_fid, struct util_wait_yield, util_wait.wait_fid);
	while (!wait->signal) {
		fastlock_acquire(&wait->util_wait.lock);
		dlist_foreach_container(&wait->util_wait.fid_list,
					struct ofi_wait_fid_entry,
					fid_entry, entry) {
			ret = fid_entry->wait_try(fid_entry->fid);
			if (ret) {
				fastlock_release(&wait->util_wait.lock);
				return ret;
			}
		}
		fastlock_release(&wait->util_wait.lock);
		pthread_yield();
	}

	fastlock_acquire(&wait->signal_lock);
	wait->signal = 0;
	fastlock_release(&wait->signal_lock);

	return FI_SUCCESS;
}

static int util_wait_yield_close(struct fid *fid)
{
	struct util_wait_yield *wait;
	int ret;

	wait = container_of(fid, struct util_wait_yield, util_wait.wait_fid.fid);
	ret = fi_wait_cleanup(&wait->util_wait);
	if (ret)
		return ret;

	fastlock_destroy(&wait->signal_lock);
	free(wait);
	return 0;
}

static struct fi_ops_wait util_wait_yield_ops = {
	.size = sizeof(struct fi_ops_wait),
	.wait = util_wait_yield_run,
};

static struct fi_ops util_wait_yield_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = util_wait_yield_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static int util_verify_wait_yield_attr(const struct fi_provider *prov,
				       const struct fi_wait_attr *attr)
{
	int ret;

	ret = ofi_check_wait_attr(prov, attr);
	if (ret)
		return ret;

	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
	case FI_WAIT_YIELD:
		break;
	default:
		FI_WARN(prov, FI_LOG_FABRIC, "unsupported wait object\n");
		return -FI_EINVAL;
	}

	return 0;
}

int ofi_wait_yield_open(struct fid_fabric *fabric_fid, struct fi_wait_attr *attr,
			struct fid_wait **waitset)
{
	struct util_fabric *fabric;
	struct util_wait_yield *wait;
	int ret;

	fabric = container_of(fabric_fid, struct util_fabric, fabric_fid);
	ret = util_verify_wait_yield_attr(fabric->prov, attr);
	if (ret)
		return ret;

	attr->wait_obj = FI_WAIT_YIELD;
	wait = calloc(1, sizeof(*wait));
	if (!wait)
		return -FI_ENOMEM;

	ret = ofi_wait_init(fabric, attr, &wait->util_wait);
	if (ret) {
		free(wait);
		return ret;
	}

	wait->util_wait.signal = util_wait_yield_signal;
	wait->signal = 0;

	wait->util_wait.wait_fid.fid.ops = &util_wait_yield_fi_ops;
	wait->util_wait.wait_fid.ops = &util_wait_yield_ops;

	fastlock_init(&wait->signal_lock);

	*waitset = &wait->util_wait.wait_fid;

	return 0;
}

static int ofi_wait_match_fid(struct dlist_entry *item, const void *arg)
{
	struct ofi_wait_fid_entry *fid_entry;

	fid_entry = container_of(item, struct ofi_wait_fid_entry, entry);
	return fid_entry->fid == arg;
}

static int ofi_wait_del_fds(struct util_wait *wait,
			    struct ofi_wait_fid_entry *fid_entry)
{
	struct util_wait_fd *wait_fd;
	int fd, ret;

	/* TODO: support fid being a pollfd wait set */
	ret = fi_control(fid_entry->fid, FI_GETWAIT, &fd);
	if (ret) {
		FI_WARN(wait->prov, FI_LOG_EP_CTRL,
			"unable to get wait fd %d\n", ret);
		return ret;
	}

	wait_fd = container_of(wait, struct util_wait_fd, util_wait);
	ret = (wait->wait_obj == FI_WAIT_FD) ?
	      ofi_epoll_del(wait_fd->epoll_fd, fd) :
	      ofi_pollfds_del(wait_fd->pollfds, fd);

	return ret;
}

int ofi_wait_del_fid(struct util_wait *wait, fid_t fid)
{
	struct ofi_wait_fid_entry *fid_entry;
	struct dlist_entry *entry;
	int ret = 0;

	fastlock_acquire(&wait->lock);
	entry = dlist_find_first_match(&wait->fid_list,
				       ofi_wait_match_fid, fid);
	if (!entry) {
		FI_INFO(wait->prov, FI_LOG_EP_CTRL,
			"Given fid (%p) not found in wait list - %p\n",
			fid, wait);
		ret = -FI_EINVAL;
		goto out;
	}

	fid_entry = container_of(entry, struct ofi_wait_fid_entry, entry);
	if (ofi_atomic_dec32(&fid_entry->ref))
		goto out;

	if (wait->wait_obj == FI_WAIT_FD || wait->wait_obj == FI_WAIT_POLLFD) {
		ret = ofi_wait_del_fds(wait, fid_entry);
		if (ret) {
			FI_WARN(wait->prov, FI_LOG_EP_CTRL,
				"Failed to delete fd's\n");
			ofi_atomic_inc32(&fid_entry->ref);
			goto out;
		}
	}

	dlist_remove(&fid_entry->entry);
	free(fid_entry);
out:
	fastlock_release(&wait->lock);
	return ret;
}

static int ofi_wait_add_fds(struct util_wait *wait,
			    struct ofi_wait_fid_entry *fid_entry)
{
	struct util_wait_fd *wait_fd;
	int fd, ret;

	/* TODO: support fid being a pollfd wait set */
	ret = fi_control(fid_entry->fid, FI_GETWAIT, &fd);
	if (ret) {
		FI_WARN(wait->prov, FI_LOG_EP_CTRL,
			"unable to get wait fd %d\n", ret);
		return ret;
	}

	wait_fd = container_of(wait, struct util_wait_fd, util_wait);
	if (wait->wait_obj == FI_WAIT_FD) {
		ret = ofi_epoll_add(wait_fd->epoll_fd, fd,
				    fid_entry->events, fid_entry->fid->context);
	} else {
		ret = ofi_pollfds_add(wait_fd->pollfds, fd,
				      fid_entry->events,
				      fid_entry->fid->context);
	}

	return ret;
}

int ofi_wait_add_fid(struct util_wait *wait, fid_t fid, uint32_t events,
		     ofi_wait_try_func wait_try)
{
	struct ofi_wait_fid_entry *fid_entry;
	struct dlist_entry *entry;
	int ret = 0;

	fastlock_acquire(&wait->lock);
	entry = dlist_find_first_match(&wait->fid_list,
				       ofi_wait_match_fid, fid);
	if (entry) {
		FI_DBG(wait->prov, FI_LOG_EP_CTRL,
		       "Given fid (%p) already added to wait list - %p \n",
		       fid, wait);
		fid_entry = container_of(entry, struct ofi_wait_fid_entry, entry);
		ofi_atomic_inc32(&fid_entry->ref);
		goto out;
	}

	fid_entry = calloc(1, sizeof *fid_entry);
	if (!fid_entry) {
		ret = -FI_ENOMEM;
		goto out;
	}

	fid_entry->fid = fid;
	fid_entry->wait_try = wait_try;
	fid_entry->events = events;
	ofi_atomic_initialize32(&fid_entry->ref, 1);

	if (wait->wait_obj == FI_WAIT_FD || wait->wait_obj == FI_WAIT_POLLFD) {
		ret = ofi_wait_add_fds(wait, fid_entry);
		if (ret) {
			free(fid_entry);
			goto out;
		}
	}
	dlist_insert_tail(&fid_entry->entry, &wait->fid_list);
out:
	fastlock_release(&wait->lock);
	return ret;
}
