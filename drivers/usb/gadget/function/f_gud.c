// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Noralf Trønnes
 */

//#define DEBUG

#include <linux/configfs.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/workqueue.h>

#include <drm/gud.h>

struct f_gud {
	struct usb_function func;
	struct work_struct worker;
	size_t max_buffer_size;

	u8 interface_id;
	struct usb_request *ctrl_req;
	struct usb_request *status_req;

	struct usb_ep *bulk_ep;
	struct usb_request *bulk_req;

	struct gud_gadget *gdg;

	spinlock_t lock; /* Protects the following members: */
	bool ctrl_pending;
	bool status_pending;
	bool bulk_pending;
	bool disable_pending;
	bool host_timeout;
	int errno;
	u16 request;
	u16 value;
};

static inline struct f_gud *func_to_f_gud(struct usb_function *f)
{
	return container_of(f, struct f_gud, func);
}

struct f_gud_opts {
	struct usb_function_instance func_inst;
	struct mutex lock;
	int refcnt;

	unsigned int drm_dev;
	const char *backlight_dev;
	u8 compression;
	u32 connectors;
	u8 formats[GUD_FORMATS_MAX_NUM];
};

static inline struct f_gud_opts *fi_to_f_gud_opts(const struct usb_function_instance *fi)
{
	return container_of(fi, struct f_gud_opts, func_inst);
}

static inline struct f_gud_opts *ci_to_f_gud_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_gud_opts, func_inst.group);
}

#define F_MUD_DEFINE_BULK_ENDPOINT_DESCRIPTOR(name, addr, size)	\
	static struct usb_endpoint_descriptor name = {		\
		.bLength =		USB_DT_ENDPOINT_SIZE,	\
		.bDescriptorType =	USB_DT_ENDPOINT,	\
		.bEndpointAddress =	addr,			\
		.bmAttributes =		USB_ENDPOINT_XFER_BULK,	\
		.wMaxPacketSize =	cpu_to_le16(size),	\
	}

static struct usb_interface_descriptor f_gud_intf = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bNumEndpoints =	1,
	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,
};

F_MUD_DEFINE_BULK_ENDPOINT_DESCRIPTOR(f_gud_fs_out_desc, USB_DIR_OUT, 0);

static struct usb_descriptor_header *f_gud_fs_function[] = {
	(struct usb_descriptor_header *)&f_gud_intf,
	(struct usb_descriptor_header *)&f_gud_fs_out_desc,
	NULL,
};

F_MUD_DEFINE_BULK_ENDPOINT_DESCRIPTOR(f_gud_hs_out_desc, USB_DIR_OUT, 512);

static struct usb_descriptor_header *f_gud_hs_function[] = {
	(struct usb_descriptor_header *)&f_gud_intf,
	(struct usb_descriptor_header *)&f_gud_hs_out_desc,
	NULL,
};

F_MUD_DEFINE_BULK_ENDPOINT_DESCRIPTOR(f_gud_ss_out_desc, USB_DIR_OUT, 1024);

static struct usb_ss_ep_comp_descriptor f_gud_ss_bulk_comp_desc = {
	.bLength =		USB_DT_SS_EP_COMP_SIZE,
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,
};

static struct usb_descriptor_header *f_gud_ss_function[] = {
	(struct usb_descriptor_header *)&f_gud_intf,
	(struct usb_descriptor_header *)&f_gud_ss_out_desc,
	(struct usb_descriptor_header *)&f_gud_ss_bulk_comp_desc,
	NULL,
};

static struct usb_string f_gud_string_defs[] = {
	[0].s = "GUD USB Display",
	{  } /* end of list */
};

static struct usb_gadget_strings f_gud_string_table = {
	.language =	0x0409,	/* en-us */
	.strings =	f_gud_string_defs,
};

static struct usb_gadget_strings *f_gud_strings[] = {
	&f_gud_string_table,
	NULL,
};

static void f_gud_bulk_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_gud *fgd = req->context;
	unsigned long flags;

	if (req->status || req->actual != req->length)
		return;

	spin_lock_irqsave(&fgd->lock, flags);
	fgd->bulk_pending = true;
	spin_unlock_irqrestore(&fgd->lock, flags);

	queue_work(system_long_wq, &fgd->worker);
}

static int f_gud_ctrl_req_set_buffer(struct f_gud *fgd, void *buf, size_t len)
{
	int ret;

	if (len != sizeof(struct gud_set_buffer_req))
		return -EINVAL;

	ret = gud_gadget_req_set_buffer(fgd->gdg, buf);
	if (ret < 0)
		return ret;

	if (ret > fgd->max_buffer_size)
		return -EOVERFLOW;

	fgd->bulk_req->length = ret;

	return usb_ep_queue(fgd->bulk_ep, fgd->bulk_req, GFP_KERNEL);
}

static void f_gud_status_req_complete(struct usb_ep *ep, struct usb_request *req)
{
}

static int f_gud_status_req_queue(struct f_gud *fgd, int errno)
{
	struct usb_composite_dev *cdev = fgd->func.config->cdev;
	u8 *buf = fgd->status_req->buf;
	int ret;

	//pr_debug("%s: errno=%d\n", __func__, errno);

	switch (errno) {
	case 0:
		*buf = GUD_STATUS_OK;
		break;
	case -EBUSY:
		*buf = GUD_STATUS_BUSY;
		break;
	case -EOPNOTSUPP:
		*buf = GUD_STATUS_REQUEST_NOT_SUPPORTED;
		break;
	case -EPROTO:
		*buf = GUD_STATUS_PROTOCOL_ERROR;
		break;
	case -EINVAL:
		*buf = GUD_STATUS_INVALID_PARAMETER;
		break;
	case -EOVERFLOW:
		*buf = GUD_STATUS_PROTOCOL_ERROR;
		break;
	default:
		*buf = GUD_STATUS_ERROR;
		break;
	}

	ret = usb_ep_queue(cdev->gadget->ep0, fgd->status_req, GFP_ATOMIC);
	//if (ret)
	//	pr_debug("%s: ret=%d\n", __func__, ret);

	return ret;
}

static void f_gud_worker(struct work_struct *work)
{
	struct f_gud *fgd = container_of(work, struct f_gud, worker);
	bool ctrl_pending, bulk_pending, disable_pending;
	struct gud_gadget *gdg = fgd->gdg;
	unsigned long flags;
	u16 request, value;
	int ret;

	spin_lock_irqsave(&fgd->lock, flags);
	request = fgd->request;
	value = fgd->value;
	ctrl_pending = fgd->ctrl_pending;
	bulk_pending = fgd->bulk_pending;
	disable_pending = fgd->disable_pending;
	spin_unlock_irqrestore(&fgd->lock, flags);

	pr_debug("%s: bulk_pending=%u ctrl_pending=%u disable_pending=%u\n",
		 __func__, bulk_pending, ctrl_pending, disable_pending);

	if (disable_pending) {
		gud_gadget_disable_pipe(gdg);

		spin_lock_irqsave(&fgd->lock, flags);
		fgd->disable_pending = false;
		spin_unlock_irqrestore(&fgd->lock, flags);
		return;
	}

	if (bulk_pending) {
		struct usb_request *req = fgd->bulk_req;

		ret = gud_gadget_write_buffer(gdg, req->buf, req->actual);
		if (ret)
			pr_err("%s: Failed to write buffer, error=%d\n", __func__, ret);

		spin_lock_irqsave(&fgd->lock, flags);
		fgd->bulk_pending = false;
		spin_unlock_irqrestore(&fgd->lock, flags);
	}

	if (ctrl_pending) {
		unsigned int length = fgd->ctrl_req->length;
		void *buf = fgd->ctrl_req->buf;

		if (request == GUD_REQ_SET_BUFFER)
			ret = f_gud_ctrl_req_set_buffer(fgd, buf, length);
		else
			ret = gud_gadget_req_set(gdg, request, value, buf, length);

		spin_lock_irqsave(&fgd->lock, flags);
		fgd->ctrl_pending = false;
		//pr_debug("%s: status_pending=%u\n", __func__, fgd->status_pending);
		if (fgd->status_pending) {
			fgd->status_pending = false;
			f_gud_status_req_queue(fgd, ret);
		} else {
			fgd->errno = ret;
		}
		spin_unlock_irqrestore(&fgd->lock, flags);
	}
}

static void f_gud_ctrl_req_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_gud *fgd = req->context;
	unsigned long flags;
	int ret = 0;

	//pr_debug("%s: status=%d diff=%u\n", __func__, req->status, req->actual != req->length);

	spin_lock_irqsave(&fgd->lock, flags);

	if (req->status)
		ret = req->status;
	else if (req->actual != req->length)
		ret = -EREMOTEIO;
	if (ret)
		fgd->errno = ret;
	else
		fgd->ctrl_pending = true;

	spin_unlock_irqrestore(&fgd->lock, flags);

	if (!ret)
		queue_work(system_long_wq, &fgd->worker);
}

static int f_gud_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct f_gud *fgd = func_to_f_gud(f);
	bool in = ctrl->bRequestType & USB_DIR_IN;
	u16 length = le16_to_cpu(ctrl->wLength);
	u16 value = le16_to_cpu(ctrl->wValue);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&fgd->lock, flags);

	pr_debug("%s: bRequest=0x%x length=%u ctrl_pending=%u status_pending=%u\n",
		 __func__, ctrl->bRequest, length, fgd->ctrl_pending, fgd->status_pending);

	if (fgd->status_pending) {
		/* Host timed out on the previous status request, worker is running */
		pr_debug("EBUSY: status_pending\n");
		ret = -EBUSY;
		fgd->status_pending = false;
		fgd->host_timeout = true;
	} else if (ctrl->bRequest == GUD_REQ_GET_STATUS) {
		if (!in || length != sizeof(u8)) {
			ret = -EINVAL;
		} else if (fgd->ctrl_pending && !fgd->host_timeout) {
			/* Worker isn't done yet, it will queue up status when done */
			ret = 0;
			fgd->status_pending = true;
		} else {
			ret = f_gud_status_req_queue(fgd, fgd->errno);
		}
	} else if (fgd->ctrl_pending) {
		/* Host timed out on the previous request, worker is running */
		pr_debug("EBUSY: ctrl_pending\n");
		ret = -EBUSY;
	} else if (in) {
		if (length > USB_COMP_EP0_BUFSIZ) {
			ret = -EOVERFLOW;
		} else {
			ret = gud_gadget_req_get(fgd->gdg, ctrl->bRequest, value,
						 cdev->req->buf, length);
			if (ret >= 0) {
				cdev->req->length = ret;
				cdev->req->zero = ret < length;
				ret = usb_ep_queue(cdev->gadget->ep0, cdev->req, GFP_ATOMIC);
			}
		}
	} else {
		if (length > USB_COMP_EP0_BUFSIZ) {
			ret = -EOVERFLOW;
		} else {
			fgd->host_timeout = false;
			fgd->request = ctrl->bRequest;
			fgd->value = value;
			fgd->ctrl_req->length = length;
			/* ->complete() can run inline and takes the lock. Preserve errno */
			fgd->errno = 0;
			spin_unlock_irqrestore(&fgd->lock, flags);
			ret = usb_ep_queue(cdev->gadget->ep0, fgd->ctrl_req, GFP_ATOMIC);
			spin_lock_irqsave(&fgd->lock, flags);
			if (!ret)
				ret = fgd->errno;
		}
	}

	fgd->errno = ret < 0 ? ret : 0;

	spin_unlock_irqrestore(&fgd->lock, flags);

	pr_debug("                 ret=%d\n", ret);

	return ret;
}

static bool f_gud_req_match(struct usb_function *f, const struct usb_ctrlrequest *ctrl,
			    bool config0)
{
	struct f_gud *fgd = func_to_f_gud(f);

	if (config0)
		return false;

	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_VENDOR)
		return false;

	if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE)
		return false;

	return fgd->interface_id == le16_to_cpu(ctrl->wIndex);
}

static int f_gud_set_alt(struct usb_function *f, unsigned int intf, unsigned int alt)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct f_gud *fgd = func_to_f_gud(f);
	unsigned long flags;

	if (alt || intf != fgd->interface_id)
		return -EINVAL;

	if (!fgd->bulk_ep->desc) {
		pr_debug("%s: init\n", __func__);
		if (config_ep_by_speed(cdev->gadget, f, fgd->bulk_ep)) {
			fgd->bulk_ep->desc = NULL;
			return -EINVAL;
		}
	}

	pr_debug("%s: reset\n", __func__);

	usb_ep_disable(fgd->bulk_ep);
	usb_ep_enable(fgd->bulk_ep);

	spin_lock_irqsave(&fgd->lock, flags);
	fgd->ctrl_pending = false;
	fgd->bulk_pending = false;
	fgd->disable_pending = false;
	spin_unlock_irqrestore(&fgd->lock, flags);

	return 0;
}

static void f_gud_disable(struct usb_function *f)
{
	struct f_gud *fgd = func_to_f_gud(f);
	unsigned long flags;

	pr_debug("%s\n", __func__);

	usb_ep_disable(fgd->bulk_ep);

	spin_lock_irqsave(&fgd->lock, flags);
	fgd->ctrl_pending = false;
	fgd->bulk_pending = false;
	fgd->status_pending = false;
	fgd->disable_pending = true;
	fgd->errno = -ESHUTDOWN;
	spin_unlock_irqrestore(&fgd->lock, flags);

	queue_work(system_long_wq, &fgd->worker);
}

static struct usb_request *f_gud_alloc_request(struct usb_ep *ep, size_t length)
{
	struct usb_request *req;

	req = usb_ep_alloc_request(ep, GFP_KERNEL);
	if (!req)
		return NULL;

	req->length = length;
	req->buf = kmalloc(length, GFP_KERNEL);
	if (!req->buf) {
		usb_ep_free_request(ep, req);
		return NULL;
	}

	return req;
}

static void f_gud_free_request(struct usb_ep *ep, struct usb_request **req)
{
	if (!*req)
		return;

	kfree((*req)->buf);
	usb_ep_free_request(ep, *req);
	*req = NULL;
}

static void f_gud_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_gud *fgd = func_to_f_gud(f);
	struct usb_composite_dev *cdev = fgd->func.config->cdev;

	flush_work(&fgd->worker);

	gud_gadget_fini(fgd->gdg);
	fgd->gdg = NULL;

	f_gud_free_request(fgd->bulk_ep, &fgd->bulk_req);
	f_gud_free_request(cdev->gadget->ep0, &fgd->status_req);
	f_gud_free_request(cdev->gadget->ep0, &fgd->ctrl_req);
	fgd->bulk_ep = NULL;

	usb_free_all_descriptors(f);
}

static int f_gud_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_gud_opts *opts = fi_to_f_gud_opts(f->fi);
	struct usb_composite_dev *cdev = c->cdev;
	struct f_gud *fgd = func_to_f_gud(f);
	struct gud_gadget *gdg;
	struct usb_string *us;
	int ret;

	us = usb_gstrings_attach(cdev, f_gud_strings, ARRAY_SIZE(f_gud_string_defs));
	if (IS_ERR(us))
		return PTR_ERR(us);

	f_gud_intf.iInterface = us[0].id;

	ret = usb_interface_id(c, f);
	if (ret < 0)
		return ret;

	fgd->interface_id = ret;
	f_gud_intf.bInterfaceNumber = fgd->interface_id;

	fgd->bulk_ep = usb_ep_autoconfig(cdev->gadget, &f_gud_fs_out_desc);
	if (!fgd->bulk_ep)
		return -ENODEV;

	f_gud_hs_out_desc.bEndpointAddress = f_gud_fs_out_desc.bEndpointAddress;
	f_gud_ss_out_desc.bEndpointAddress = f_gud_fs_out_desc.bEndpointAddress;

	ret = usb_assign_descriptors(f, f_gud_fs_function, f_gud_hs_function,
				     f_gud_ss_function, NULL);
	if (ret)
		return ret;

	fgd->ctrl_req = f_gud_alloc_request(cdev->gadget->ep0, USB_COMP_EP0_BUFSIZ);
	if (!fgd->ctrl_req) {
		ret = -ENOMEM;
		goto fail_free_descs;
	}

	fgd->ctrl_req->complete = f_gud_ctrl_req_complete;
	fgd->ctrl_req->context = fgd;

	fgd->status_req = f_gud_alloc_request(cdev->gadget->ep0, sizeof(u8));
	if (!fgd->status_req) {
		ret = -ENOMEM;
		goto fail_free_reqs;
	}

	fgd->status_req->complete = f_gud_status_req_complete;
	fgd->status_req->context = fgd;

	gdg = gud_gadget_init(opts->drm_dev, opts->backlight_dev, &fgd->max_buffer_size,
			      opts->compression, opts->formats, opts->connectors);
	if (IS_ERR(gdg)) {
		ret = PTR_ERR(gdg);
		goto fail_free_reqs;
	}

	fgd->bulk_req = f_gud_alloc_request(fgd->bulk_ep, fgd->max_buffer_size);
	if (!fgd->bulk_req) {
		ret = -ENOMEM;
		goto fail_free_reqs;
	}

	fgd->bulk_req->complete = f_gud_bulk_complete;
	fgd->bulk_req->context = fgd;

	fgd->gdg = gdg;

	return 0;

fail_free_reqs:
	f_gud_free_request(fgd->bulk_ep, &fgd->bulk_req);
	f_gud_free_request(cdev->gadget->ep0, &fgd->ctrl_req);
	f_gud_free_request(cdev->gadget->ep0, &fgd->status_req);
fail_free_descs:
	usb_free_all_descriptors(f);

	return ret;
}

static void f_gud_free_func(struct usb_function *f)
{
	struct f_gud_opts *opts = container_of(f->fi, struct f_gud_opts, func_inst);
	struct f_gud *fgd = func_to_f_gud(f);

	mutex_lock(&opts->lock);
	opts->refcnt--;
	mutex_unlock(&opts->lock);

	kfree(fgd);
}

static struct usb_function *f_gud_alloc_func(struct usb_function_instance *fi)
{
	struct f_gud_opts *opts = fi_to_f_gud_opts(fi);
	struct usb_function *func;
	struct f_gud *fgd;

	fgd = kzalloc(sizeof(*fgd), GFP_KERNEL);
	if (!fgd)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&fgd->lock);
	INIT_WORK(&fgd->worker, f_gud_worker);

	mutex_lock(&opts->lock);
	opts->refcnt++;
	mutex_unlock(&opts->lock);

	func = &fgd->func;
	func->name = "gud";
	func->bind = f_gud_bind;
	func->unbind = f_gud_unbind;
	func->set_alt = f_gud_set_alt;
	func->req_match = f_gud_req_match;
	func->setup = f_gud_setup;
	func->disable = f_gud_disable;
	func->free_func = f_gud_free_func;

	return func;
}

static ssize_t f_gud_opts_drm_dev_show(struct config_item *item, char *page)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	int result;

	mutex_lock(&opts->lock);
	result = sprintf(page, "%u\n", opts->drm_dev);
	mutex_unlock(&opts->lock);

	return result;
}

static ssize_t f_gud_opts_drm_dev_store(struct config_item *item, const char *page, size_t len)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	unsigned int num;
	int ret;

	mutex_lock(&opts->lock);
	if (opts->refcnt) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = kstrtouint(page, 0, &num);
	if (ret)
		goto unlock;

	opts->drm_dev = num;
	ret = len;
unlock:
	mutex_unlock(&opts->lock);

	return ret;
}

CONFIGFS_ATTR(f_gud_opts_, drm_dev);

static ssize_t f_gud_opts_backlight_dev_show(struct config_item *item, char *page)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	ssize_t ret = 0;

	mutex_lock(&opts->lock);
	if (opts->backlight_dev)
		ret = strscpy(page, opts->backlight_dev, PAGE_SIZE);
	else
		page[0] = '\0';
	mutex_unlock(&opts->lock);

	return ret;
}

static ssize_t f_gud_opts_backlight_dev_store(struct config_item *item,
					      const char *page, size_t len)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	ssize_t ret;
	char *name;

	mutex_lock(&opts->lock);
	if (opts->refcnt) {
		ret = -EBUSY;
		goto unlock;
	}

	name = kstrndup(page, len, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto unlock;
	}

	kfree(opts->backlight_dev);
	opts->backlight_dev = name;
	ret = len;
unlock:
	mutex_unlock(&opts->lock);

	return ret;
}

CONFIGFS_ATTR(f_gud_opts_, backlight_dev);

static ssize_t f_gud_opts_compression_show(struct config_item *item, char *page)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	int ret;

	mutex_lock(&opts->lock);
	ret = sprintf(page, "0x%02x\n", opts->compression);
	mutex_unlock(&opts->lock);

	return ret;
}

static ssize_t f_gud_opts_compression_store(struct config_item *item,
					    const char *page, size_t len)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	int ret;

	mutex_lock(&opts->lock);
	if (opts->refcnt) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = kstrtou8(page, 0, &opts->compression);
	if (ret)
		goto unlock;

	ret = len;
unlock:
	mutex_unlock(&opts->lock);

	return ret;
}

CONFIGFS_ATTR(f_gud_opts_, compression);

static ssize_t f_gud_opts_formats_show(struct config_item *item,
				       char *page)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	int i;

	mutex_lock(&opts->lock);
	memcpy(page, opts->formats, GUD_FORMATS_MAX_NUM);
	for (i = GUD_FORMATS_MAX_NUM; i > 0; i--) {
		if (opts->formats[i - 1])
			break;
	}
	mutex_unlock(&opts->lock);

	return i;
}

static ssize_t f_gud_opts_formats_store(struct config_item *item,
					const char *page, size_t len)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	int ret;

	mutex_lock(&opts->lock);
	if (opts->refcnt) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = len;
	len = min_t(size_t, GUD_FORMATS_MAX_NUM, len);
	memcpy(opts->formats, page, len);
unlock:
	mutex_unlock(&opts->lock);

	return ret;
}

CONFIGFS_ATTR(f_gud_opts_, formats);

static ssize_t f_gud_opts_connectors_show(struct config_item *item, char *page)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	int ret;

	mutex_lock(&opts->lock);
	ret = sprintf(page, "0x%08x\n", opts->connectors);
	mutex_unlock(&opts->lock);

	return ret;
}

static ssize_t f_gud_opts_connectors_store(struct config_item *item,
					   const char *page, size_t len)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);
	int ret;

	mutex_lock(&opts->lock);
	if (opts->refcnt) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = kstrtou32(page, 0, &opts->connectors);
	if (ret)
		goto unlock;

	ret = len;
unlock:
	mutex_unlock(&opts->lock);

	return ret;
}

CONFIGFS_ATTR(f_gud_opts_, connectors);

static struct configfs_attribute *f_gud_attrs[] = {
	&f_gud_opts_attr_drm_dev,
	&f_gud_opts_attr_backlight_dev,
	&f_gud_opts_attr_compression,
	&f_gud_opts_attr_formats,
	&f_gud_opts_attr_connectors,
	NULL,
};

static void f_gud_attr_release(struct config_item *item)
{
	struct f_gud_opts *opts = ci_to_f_gud_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations f_gud_item_ops = {
	.release	= f_gud_attr_release,
};

static const struct config_item_type f_gud_func_type = {
	.ct_item_ops	= &f_gud_item_ops,
	.ct_attrs	= f_gud_attrs,
	.ct_owner	= THIS_MODULE,
};

static void f_gud_free_func_inst(struct usb_function_instance *fi)
{
	struct f_gud_opts *opts = fi_to_f_gud_opts(fi);

	mutex_destroy(&opts->lock);
	kfree(opts->backlight_dev);
	kfree(opts);
}

static struct usb_function_instance *f_gud_alloc_func_inst(void)
{
	struct f_gud_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = f_gud_free_func_inst;
	opts->compression = ~0;
	opts->connectors = ~0U;

	config_group_init_type_name(&opts->func_inst.group, "", &f_gud_func_type);

	return &opts->func_inst;
}

DECLARE_USB_FUNCTION_INIT(gud, f_gud_alloc_func_inst, f_gud_alloc_func);

MODULE_DESCRIPTION("Generic USB Display Gadget");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
