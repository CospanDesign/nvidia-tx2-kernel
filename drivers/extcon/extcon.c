/*
 *  drivers/extcon/extcon.c - External Connector (extcon) framework.
 *
 *  External connector (extcon) class driver
 *
 * Copyright (C) 2015 Samsung Electronics
 * Author: Chanwoo Choi <cw00.choi@samsung.com>
 *
 * Copyright (C) 2012 Samsung Electronics
 * Author: Donggeun Kim <dg77.kim@samsung.com>
 * Author: MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * based on android/drivers/switch/switch_class.c
 * Copyright (C) 2008 Google, Inc.
 * Author: Mike Lockwood <lockwood@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/suspend.h>

#define SUPPORTED_CABLE_MAX	32
#define CABLE_NAME_MAX		30

static const char *extcon_name[] =  {
	[EXTCON_NONE]			= "NONE",

	/* USB external connector */
	[EXTCON_USB]			= "USB",
	[EXTCON_USB_HOST]		= "USB-HOST",

	/* Charging external connector */
	[EXTCON_CHG_USB_SDP]		= "SDP",
	[EXTCON_CHG_USB_DCP]		= "DCP",
	[EXTCON_CHG_USB_CDP]		= "CDP",
	[EXTCON_CHG_USB_ACA]		= "ACA",
	[EXTCON_CHG_USB_FAST]		= "FAST-CHARGER",
	[EXTCON_CHG_USB_SLOW]		= "SLOW-CHARGER",

	/* Jack external connector */
	[EXTCON_JACK_MICROPHONE]	= "MICROPHONE",
	[EXTCON_JACK_HEADPHONE]		= "HEADPHONE",
	[EXTCON_JACK_LINE_IN]		= "LINE-IN",
	[EXTCON_JACK_LINE_OUT]		= "LINE-OUT",
	[EXTCON_JACK_VIDEO_IN]		= "VIDEO-IN",
	[EXTCON_JACK_VIDEO_OUT]		= "VIDEO-OUT",
	[EXTCON_JACK_SPDIF_IN]		= "SPDIF-IN",
	[EXTCON_JACK_SPDIF_OUT]		= "SPDIF-OUT",

	/* Display external connector */
	[EXTCON_DISP_HDMI]		= "HDMI",
	[EXTCON_DISP_MHL]		= "MHL",
	[EXTCON_DISP_DVI]		= "DVI",
	[EXTCON_DISP_VGA]		= "VGA",

	/* Miscellaneous external connector */
	[EXTCON_DOCK]			= "DOCK",
	[EXTCON_JIG]			= "JIG",
	[EXTCON_MECHANICAL]		= "MECHANICAL",

	[EXTCON_USB_QC2]		= "QC2",
	[EXTCON_USB_MAXIM]		= "MAXIM",
	[EXTCON_USB_APPLE_500mA]	= "APPLE-500mA",
	[EXTCON_USB_APPLE_1A]		= "APPLE-1A",
	[EXTCON_USB_APPLE_2A]		= "APPLE-2A",
	[EXTCON_USB_ACA_NV]		= "ACA-NV",
	[EXTCON_USB_ACA_RIDA]		= "ACA-RIDA",
	[EXTCON_USB_ACA_RIDB]		= "ACA-RIDB",
	[EXTCON_USB_ACA_RIDB]		= "ACA-RIDC",
	[EXTCON_USB_Y_CABLE]		= "Y-CABLE",
	[EXTCON_USB_PD]			= "USB-PD",

	NULL,
};

/**
 * struct extcon_cable - An internal data for each cable of extcon device.
 * @edev:		The extcon device
 * @cable_index:	Index of this cable in the edev
 * @attr_g:		Attribute group for the cable
 * @attr_name:		"name" sysfs entry
 * @attr_state:		"state" sysfs entry
 * @attrs:		Array pointing to attr_name and attr_state for attr_g
 */
struct extcon_cable {
	struct extcon_dev *edev;
	int cable_index;

	struct attribute_group attr_g;
	struct device_attribute attr_name;
	struct device_attribute attr_state;

	struct attribute *attrs[3]; /* to be fed to attr_g.attrs */
};

static struct class *extcon_class;
#if defined(CONFIG_ANDROID)
static struct class_compat *switch_class;
#endif /* CONFIG_ANDROID */

static LIST_HEAD(extcon_dev_list);
static DEFINE_MUTEX(extcon_dev_list_lock);

/**
 * check_mutually_exclusive - Check if new_state violates mutually_exclusive
 *			      condition.
 * @edev:	the extcon device
 * @new_state:	new cable attach status for @edev
 *
 * Returns 0 if nothing violates. Returns the index + 1 for the first
 * violated condition.
 */
static int check_mutually_exclusive(struct extcon_dev *edev, u32 new_state)
{
	int i = 0;

	if (!edev->mutually_exclusive)
		return 0;

	for (i = 0; edev->mutually_exclusive[i]; i++) {
		int weight;
		u32 correspondants = new_state & edev->mutually_exclusive[i];

		/* calculate the total number of bits set */
		weight = hweight32(correspondants);
		if (weight > 1)
			return i + 1;
	}

	return 0;
}

static int find_cable_index_by_id(struct extcon_dev *edev, const unsigned int id)
{
	int i;

	/* Find the the index of extcon cable in edev->supported_cable */
	for (i = 0; i < edev->max_supported; i++) {
		if (edev->supported_cable[i] == id)
			return i;
	}

	return -EINVAL;
}

static bool is_extcon_changed(u32 prev, u32 new, int idx, bool *attached)
{
	if (((prev >> idx) & 0x1) != ((new >> idx) & 0x1)) {
		*attached = ((new >> idx) & 0x1) ? true : false;
		return true;
	}

	return false;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	int i, count = 0;
	struct extcon_dev *edev = dev_get_drvdata(dev);

	if (edev->max_supported == 0)
		return sprintf(buf, "%u\n", edev->state);

	for (i = 0; i < edev->max_supported; i++) {
		count += sprintf(buf + count, "%s=%d\n",
				extcon_name[edev->supported_cable[i]],
				 !!(edev->state & (1 << i)));
	}

	return count;
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	u32 state;
	ssize_t ret = 0;
	struct extcon_dev *edev = dev_get_drvdata(dev);

	ret = sscanf(buf, "0x%4x", &state);
	if (ret == 0)
		ret = -EINVAL;
	else
		ret = extcon_set_state(edev, state);

	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(state);

static ssize_t name_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct extcon_dev *edev = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", edev->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t cable_name_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct extcon_cable *cable = container_of(attr, struct extcon_cable,
						  attr_name);
	int i = cable->cable_index;

	return snprintf(buf, PAGE_SIZE, "%s\n",
			extcon_name[cable->edev->supported_cable[i]]);
}

static ssize_t cable_state_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct extcon_cable *cable = container_of(attr, struct extcon_cable,
						  attr_state);

	int i = cable->cable_index;

	return snprintf(buf, PAGE_SIZE, "%d\n",
		       extcon_get_cable_state_(cable->edev,
					       cable->edev->supported_cable[i]));
}

static ssize_t uevent_in_suspend_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct extcon_dev *edev = dev_get_drvdata(dev);

	return sprintf(buf, "%c\n", edev->uevent_in_suspend ? 'Y' : 'N');
}

static ssize_t uevent_in_suspend_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct extcon_dev *edev = dev_get_drvdata(dev);
	bool uevent_in_suspend;
	int ret;
	unsigned long flags;

	ret = strtobool(buf, &uevent_in_suspend);
	if (ret)
		return ret;

	spin_lock_irqsave(&edev->lock, flags);
	edev->uevent_in_suspend = uevent_in_suspend;
	spin_unlock_irqrestore(&edev->lock, flags);

	return count;
}
DEVICE_ATTR_RW(uevent_in_suspend);

/**
 * extcon_update_state() - Update the cable attach states of the extcon device
 *			   only for the masked bits.
 * @edev:	the extcon device
 * @mask:	the bit mask to designate updated bits.
 * @state:	new cable attach status for @edev
 *
 * Changing the state sends uevent with environment variable containing
 * the name of extcon device (envp[0]) and the state output (envp[1]).
 * Tizen uses this format for extcon device to get events from ports.
 * Android uses this format as well.
 *
 * Note that the notifier provides which bits are changed in the state
 * variable with the val parameter (second) to the callback.
 */
int extcon_update_state(struct extcon_dev *edev, u32 mask, u32 state)
{
	char name_buf[120];
	char state_buf[120];
	char *prop_buf;
	char *envp[3];
	int env_offset = 0;
	int length;
	int index;
	unsigned long flags;
	bool attached;

	if (!edev)
		return -EINVAL;

	spin_lock_irqsave(&edev->lock, flags);
	/* Store a new state in the last_state_in_suspend while suspending.
	 * It will be handled after resume. */
	if (edev->is_suspend && !edev->uevent_in_suspend) {
		edev->last_state_in_suspend = state;
		spin_unlock_irqrestore(&edev->lock, flags);
		dev_info(&edev->dev,
			"%s: didn't send uevent (0x%08x 0x%08x 0x%08x) due to suspending\n",
			__func__, mask, state, edev->state);
		return 0;
	}

	if (edev->state != ((edev->state & ~mask) | (state & mask))) {
		u32 old_state;

		if (check_mutually_exclusive(edev, (edev->state & ~mask) |
						   (state & mask))) {
			spin_unlock_irqrestore(&edev->lock, flags);
			return -EPERM;
		}

		old_state = edev->state;
		edev->state &= ~mask;
		edev->state |= state & mask;

		for (index = 0; index < edev->max_supported; index++) {
			if (is_extcon_changed(old_state, edev->state, index,
					      &attached))
				raw_notifier_call_chain(&edev->nh[index],
							attached, edev);
		}

		/* This could be in interrupt handler */
		prop_buf = (char *)get_zeroed_page(GFP_ATOMIC);
		if (prop_buf) {
			length = name_show(&edev->dev, NULL, prop_buf);
			if (length > 0) {
				if (prop_buf[length - 1] == '\n')
					prop_buf[length - 1] = 0;
				snprintf(name_buf, sizeof(name_buf),
					"NAME=%s", prop_buf);
				envp[env_offset++] = name_buf;
			}
			length = state_show(&edev->dev, NULL, prop_buf);
			if (length > 0) {
				if (prop_buf[length - 1] == '\n')
					prop_buf[length - 1] = 0;
				snprintf(state_buf, sizeof(state_buf),
					"STATE=%s", prop_buf);
				envp[env_offset++] = state_buf;
			}
			envp[env_offset] = NULL;
			/* Unlock early before uevent */
			spin_unlock_irqrestore(&edev->lock, flags);

			kobject_uevent_env(&edev->dev.kobj, KOBJ_CHANGE, envp);
			free_page((unsigned long)prop_buf);
		} else {
			/* Unlock early before uevent */
			spin_unlock_irqrestore(&edev->lock, flags);

			dev_err(&edev->dev, "out of memory in extcon_set_state\n");
			kobject_uevent(&edev->dev.kobj, KOBJ_CHANGE);
		}
	} else {
		/* No changes */
		spin_unlock_irqrestore(&edev->lock, flags);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(extcon_update_state);

/**
 * extcon_set_state() - Set the cable attach states of the extcon device.
 * @edev:	the extcon device
 * @state:	new cable attach status for @edev
 *
 * Note that notifier provides which bits are changed in the state
 * variable with the val parameter (second) to the callback.
 */
int extcon_set_state(struct extcon_dev *edev, u32 state)
{
	if (!edev)
		return -EINVAL;

	return extcon_update_state(edev, 0xffffffff, state);
}
EXPORT_SYMBOL_GPL(extcon_set_state);

/**
 * extcon_get_cable_state_() - Get the status of a specific cable.
 * @edev:	the extcon device that has the cable.
 * @id:		the unique id of each external connector in extcon enumeration.
 */
int extcon_get_cable_state_(struct extcon_dev *edev, const unsigned int id)
{
	int index;

	if (!edev)
		return -EINVAL;

	index = find_cable_index_by_id(edev, id);
	if (index < 0)
		return index;

	if (edev->max_supported && edev->max_supported <= index)
		return -EINVAL;

	return !!(edev->state & (1 << index));
}
EXPORT_SYMBOL_GPL(extcon_get_cable_state_);

/**
 * extcon_set_cable_state_() - Set the status of a specific cable.
 * @edev:		the extcon device that has the cable.
 * @id:			the unique id of each external connector
 *			in extcon enumeration.
 * @state:		the new cable status. The default semantics is
 *			true: attached / false: detached.
 */
int extcon_set_cable_state_(struct extcon_dev *edev, unsigned int id,
				bool cable_state)
{
	u32 state;
	int index;

	if (!edev)
		return -EINVAL;

	index = find_cable_index_by_id(edev, id);
	if (index < 0)
		return index;

	if (edev->max_supported && edev->max_supported <= index)
		return -EINVAL;

	state = cable_state ? (1 << index) : 0;
	return extcon_update_state(edev, 1 << index, state);
}
EXPORT_SYMBOL_GPL(extcon_set_cable_state_);

/**
 * extcon_get_extcon_dev() - Get the extcon device instance from the name
 * @extcon_name:	The extcon name provided with extcon_dev_register()
 */
struct extcon_dev *extcon_get_extcon_dev(const char *extcon_name)
{
	struct extcon_dev *sd;

	if (!extcon_name)
		return ERR_PTR(-EINVAL);

	mutex_lock(&extcon_dev_list_lock);
	list_for_each_entry(sd, &extcon_dev_list, entry) {
		if (!strcmp(sd->name, extcon_name))
			goto out;
	}
	sd = NULL;
out:
	mutex_unlock(&extcon_dev_list_lock);
	return sd;
}
EXPORT_SYMBOL_GPL(extcon_get_extcon_dev);

/**
 * extcon_register_notifier() - Register a notifiee to get notified by
 *				any attach status changes from the extcon.
 * @edev:	the extcon device that has the external connecotr.
 * @id:		the unique id of each external connector in extcon enumeration.
 * @nb:		a notifier block to be registered.
 *
 * Note that the second parameter given to the callback of nb (val) is
 * "old_state", not the current state. The current state can be retrieved
 * by looking at the third pameter (edev pointer)'s state value.
 */
int extcon_register_notifier(struct extcon_dev *edev, unsigned int id,
			     struct notifier_block *nb)
{
	unsigned long flags;
	int ret, idx;

	if (!nb)
		return -EINVAL;

	if (edev) {
		idx = find_cable_index_by_id(edev, id);
		if (idx < 0)
			return idx;

		spin_lock_irqsave(&edev->lock, flags);
		ret = raw_notifier_chain_register(&edev->nh[idx], nb);
		spin_unlock_irqrestore(&edev->lock, flags);
	} else {
		struct extcon_dev *extd;

		mutex_lock(&extcon_dev_list_lock);
		list_for_each_entry(extd, &extcon_dev_list, entry) {
			idx = find_cable_index_by_id(extd, id);
			if (idx >= 0)
				break;
		}
		mutex_unlock(&extcon_dev_list_lock);

		if (idx >= 0) {
			edev = extd;
			return extcon_register_notifier(extd, id, nb);
		} else {
			ret = -ENODEV;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(extcon_register_notifier);

/**
 * extcon_unregister_notifier() - Unregister a notifiee from the extcon device.
 * @edev:	the extcon device that has the external connecotr.
 * @id:		the unique id of each external connector in extcon enumeration.
 * @nb:		a notifier block to be registered.
 */
int extcon_unregister_notifier(struct extcon_dev *edev, unsigned int id,
				struct notifier_block *nb)
{
	unsigned long flags;
	int ret, idx;

	if (!edev || !nb)
		return -EINVAL;

	idx = find_cable_index_by_id(edev, id);
	if (idx < 0)
		return idx;

	spin_lock_irqsave(&edev->lock, flags);
	ret = raw_notifier_chain_unregister(&edev->nh[idx], nb);
	spin_unlock_irqrestore(&edev->lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(extcon_unregister_notifier);

static struct attribute *extcon_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_name.attr,
	&dev_attr_uevent_in_suspend.attr,
	NULL,
};
ATTRIBUTE_GROUPS(extcon);

static int create_extcon_class(void)
{
	if (!extcon_class) {
		extcon_class = class_create(THIS_MODULE, "extcon");
		if (IS_ERR(extcon_class))
			return PTR_ERR(extcon_class);
		extcon_class->dev_groups = extcon_groups;

#if defined(CONFIG_ANDROID)
		switch_class = class_compat_register("switch_extcon");
		if (WARN(!switch_class, "cannot allocate"))
			return -ENOMEM;
#endif /* CONFIG_ANDROID */
	}

	return 0;
}

static void extcon_dev_release(struct device *dev)
{
}

static const char *muex_name = "mutually_exclusive";
static void dummy_sysfs_dev_release(struct device *dev)
{
}

/*
 * extcon_dev_allocate() - Allocate the memory of extcon device.
 * @supported_cable:	Array of supported extcon ending with EXTCON_NONE.
 *			If supported_cable is NULL, cable name related APIs
 *			are disabled.
 *
 * This function allocates the memory for extcon device without allocating
 * memory in each extcon provider driver and initialize default setting for
 * extcon device.
 *
 * Return the pointer of extcon device if success or ERR_PTR(err) if fail
 */
struct extcon_dev *extcon_dev_allocate(const unsigned int *supported_cable)
{
	struct extcon_dev *edev;

	if (!supported_cable)
		return ERR_PTR(-EINVAL);

	edev = kzalloc(sizeof(*edev), GFP_KERNEL);
	if (!edev)
		return ERR_PTR(-ENOMEM);

	edev->max_supported = 0;
	edev->supported_cable = supported_cable;

	return edev;
}

/*
 * extcon_dev_free() - Free the memory of extcon device.
 * @edev:	the extcon device to free
 */
void extcon_dev_free(struct extcon_dev *edev)
{
	kfree(edev);
}
EXPORT_SYMBOL_GPL(extcon_dev_free);

static int extcon_pm_notify(struct notifier_block *nb,
			    unsigned long event, void *data)
{
	struct extcon_dev *edev = container_of(nb, struct extcon_dev, pm_nb);
	unsigned long flags;

	if (event == PM_SUSPEND_PREPARE) {
		spin_lock_irqsave(&edev->lock, flags);
		edev->is_suspend = true;
		if (!edev->uevent_in_suspend)
			edev->last_state_in_suspend = edev->state;
		spin_unlock_irqrestore(&edev->lock, flags);
	} else if (event == PM_POST_SUSPEND) {
		spin_lock_irqsave(&edev->lock, flags);
		edev->is_suspend = false;
		spin_unlock_irqrestore(&edev->lock, flags);
		if (!edev->uevent_in_suspend)
			extcon_set_state(edev, edev->last_state_in_suspend);
	}

	return NOTIFY_OK;
}

static struct notifier_block extcon_pm_nb = {
	.notifier_call = extcon_pm_notify,
	.priority = -1,
};

/**
 * extcon_dev_register() - Register a new extcon device
 * @edev	: the new extcon device (should be allocated before calling)
 *
 * Among the members of edev struct, please set the "user initializing data"
 * in any case and set the "optional callbacks" if required. However, please
 * do not set the values of "internal data", which are initialized by
 * this function.
 */
int extcon_dev_register(struct extcon_dev *edev)
{
	int ret, index = 0;
	static atomic_t edev_no = ATOMIC_INIT(-1);

	if (!extcon_class) {
		ret = create_extcon_class();
		if (ret < 0)
			return ret;
	}

	if (!edev || !edev->supported_cable)
		return -EINVAL;

	for (; edev->supported_cable[index] != EXTCON_NONE; index++);

	edev->max_supported = index;
	if (index > SUPPORTED_CABLE_MAX) {
		dev_err(&edev->dev,
			"exceed the maximum number of supported cables\n");
		return -EINVAL;
	}

	edev->dev.class = extcon_class;
	edev->dev.release = extcon_dev_release;

	edev->name = dev_name(edev->dev.parent);
	if (IS_ERR_OR_NULL(edev->name)) {
		dev_err(&edev->dev,
			"extcon device name is null\n");
		return -EINVAL;
	}
	dev_set_name(&edev->dev, "extcon%lu",
			(unsigned long)atomic_inc_return(&edev_no));

	if (edev->max_supported) {
		char buf[10];
		char *str;
		struct extcon_cable *cable;

		edev->cables = kzalloc(sizeof(struct extcon_cable) *
				       edev->max_supported, GFP_KERNEL);
		if (!edev->cables) {
			ret = -ENOMEM;
			goto err_sysfs_alloc;
		}
		for (index = 0; index < edev->max_supported; index++) {
			cable = &edev->cables[index];

			snprintf(buf, 10, "cable.%d", index);
			str = kzalloc(sizeof(char) * (strlen(buf) + 1),
				      GFP_KERNEL);
			if (!str) {
				for (index--; index >= 0; index--) {
					cable = &edev->cables[index];
					kfree(cable->attr_g.name);
				}
				ret = -ENOMEM;

				goto err_alloc_cables;
			}
			strncpy(str, buf, strlen(buf));

			cable->edev = edev;
			cable->cable_index = index;
			cable->attrs[0] = &cable->attr_name.attr;
			cable->attrs[1] = &cable->attr_state.attr;
			cable->attrs[2] = NULL;
			cable->attr_g.name = str;
			cable->attr_g.attrs = cable->attrs;

			sysfs_attr_init(&cable->attr_name.attr);
			cable->attr_name.attr.name = "name";
			cable->attr_name.attr.mode = 0444;
			cable->attr_name.show = cable_name_show;

			sysfs_attr_init(&cable->attr_state.attr);
			cable->attr_state.attr.name = "state";
			cable->attr_state.attr.mode = 0444;
			cable->attr_state.show = cable_state_show;
		}
	}

	if (edev->max_supported && edev->mutually_exclusive) {
		char buf[80];
		char *name;

		/* Count the size of mutually_exclusive array */
		for (index = 0; edev->mutually_exclusive[index]; index++)
			;

		edev->attrs_muex = kzalloc(sizeof(struct attribute *) *
					   (index + 1), GFP_KERNEL);
		if (!edev->attrs_muex) {
			ret = -ENOMEM;
			goto err_muex;
		}

		edev->d_attrs_muex = kzalloc(sizeof(struct device_attribute) *
					     index, GFP_KERNEL);
		if (!edev->d_attrs_muex) {
			ret = -ENOMEM;
			kfree(edev->attrs_muex);
			goto err_muex;
		}

		for (index = 0; edev->mutually_exclusive[index]; index++) {
			snprintf(buf, sizeof(buf), "0x%x", edev->mutually_exclusive[index]);
			name = kzalloc(sizeof(char) * (strlen(buf) + 1),
				       GFP_KERNEL);
			if (!name) {
				for (index--; index >= 0; index--) {
					kfree(edev->d_attrs_muex[index].attr.
					      name);
				}
				kfree(edev->d_attrs_muex);
				kfree(edev->attrs_muex);
				ret = -ENOMEM;
				goto err_muex;
			}
			strncpy(name, buf, strlen(buf));
			sysfs_attr_init(&edev->d_attrs_muex[index].attr);
			edev->d_attrs_muex[index].attr.name = name;
			edev->d_attrs_muex[index].attr.mode = 0000;
			edev->attrs_muex[index] = &edev->d_attrs_muex[index]
							.attr;
		}
		edev->attr_g_muex.name = muex_name;
		edev->attr_g_muex.attrs = edev->attrs_muex;

	}

	if (edev->max_supported) {
		edev->extcon_dev_type.groups =
			kzalloc(sizeof(struct attribute_group *) *
				(edev->max_supported + 2), GFP_KERNEL);
		if (!edev->extcon_dev_type.groups) {
			ret = -ENOMEM;
			goto err_alloc_groups;
		}

		edev->extcon_dev_type.name = dev_name(&edev->dev);
		edev->extcon_dev_type.release = dummy_sysfs_dev_release;

		for (index = 0; index < edev->max_supported; index++)
			edev->extcon_dev_type.groups[index] =
				&edev->cables[index].attr_g;
		if (edev->mutually_exclusive)
			edev->extcon_dev_type.groups[index] =
				&edev->attr_g_muex;

		edev->dev.type = &edev->extcon_dev_type;
	}

	ret = device_register(&edev->dev);
	if (ret) {
		put_device(&edev->dev);
		goto err_dev;
	}
#if defined(CONFIG_ANDROID)
	if (switch_class)
		ret = class_compat_create_link(switch_class, &edev->dev, NULL);
#endif /* CONFIG_ANDROID */

	spin_lock_init(&edev->lock);

	edev->nh = devm_kzalloc(&edev->dev,
			sizeof(*edev->nh) * edev->max_supported, GFP_KERNEL);
	if (!edev->nh) {
		ret = -ENOMEM;
		goto err_dev;
	}

	for (index = 0; index < edev->max_supported; index++)
		RAW_INIT_NOTIFIER_HEAD(&edev->nh[index]);

	dev_set_drvdata(&edev->dev, edev);
	edev->state = 0;

	edev->uevent_in_suspend = true;
	edev->is_suspend = false;
	edev->pm_nb = extcon_pm_nb;
	register_pm_notifier(&edev->pm_nb);

	mutex_lock(&extcon_dev_list_lock);
	list_add(&edev->entry, &extcon_dev_list);
	mutex_unlock(&extcon_dev_list_lock);

	return 0;

err_dev:
	if (edev->max_supported)
		kfree(edev->extcon_dev_type.groups);
err_alloc_groups:
	if (edev->max_supported && edev->mutually_exclusive) {
		for (index = 0; edev->mutually_exclusive[index]; index++)
			kfree(edev->d_attrs_muex[index].attr.name);
		kfree(edev->d_attrs_muex);
		kfree(edev->attrs_muex);
	}
err_muex:
	for (index = 0; index < edev->max_supported; index++)
		kfree(edev->cables[index].attr_g.name);
err_alloc_cables:
	if (edev->max_supported)
		kfree(edev->cables);
err_sysfs_alloc:
	return ret;
}
EXPORT_SYMBOL_GPL(extcon_dev_register);

/**
 * extcon_dev_unregister() - Unregister the extcon device.
 * @edev:	the extcon device instance to be unregistered.
 *
 * Note that this does not call kfree(edev) because edev was not allocated
 * by this class.
 */
void extcon_dev_unregister(struct extcon_dev *edev)
{
	int index;

	if (!edev)
		return;

	mutex_lock(&extcon_dev_list_lock);
	list_del(&edev->entry);
	mutex_unlock(&extcon_dev_list_lock);

	if (IS_ERR_OR_NULL(get_device(&edev->dev))) {
		dev_err(&edev->dev, "Failed to unregister extcon_dev (%s)\n",
				dev_name(&edev->dev));
		return;
	}

	device_unregister(&edev->dev);
	unregister_pm_notifier(&edev->pm_nb);

	if (edev->mutually_exclusive && edev->max_supported) {
		for (index = 0; edev->mutually_exclusive[index];
				index++)
			kfree(edev->d_attrs_muex[index].attr.name);
		kfree(edev->d_attrs_muex);
		kfree(edev->attrs_muex);
	}

	for (index = 0; index < edev->max_supported; index++)
		kfree(edev->cables[index].attr_g.name);

	if (edev->max_supported) {
		kfree(edev->extcon_dev_type.groups);
		kfree(edev->cables);
	}

#if defined(CONFIG_ANDROID)
	if (switch_class)
		class_compat_remove_link(switch_class, &edev->dev, NULL);
#endif
	put_device(&edev->dev);
}
EXPORT_SYMBOL_GPL(extcon_dev_unregister);

#ifdef CONFIG_OF
/*
 * extcon_get_edev_by_phandle - Get the extcon device from devicetree
 * @dev - instance to the given device
 * @index - index into list of extcon_dev
 *
 * return the instance of extcon device
 */
struct extcon_dev *extcon_get_edev_by_phandle(struct device *dev, int index)
{
	struct device_node *node;
	struct extcon_dev *edev;

	if (!dev)
		return ERR_PTR(-EINVAL);

	if (!dev->of_node) {
		dev_dbg(dev, "device does not have a device node entry\n");
		return ERR_PTR(-EINVAL);
	}

	node = of_parse_phandle(dev->of_node, "extcon", index);
	if (!node) {
		dev_dbg(dev, "failed to get phandle in %s node\n",
			dev->of_node->full_name);
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&extcon_dev_list_lock);
	list_for_each_entry(edev, &extcon_dev_list, entry) {
		if (edev->dev.parent && edev->dev.parent->of_node == node) {
			mutex_unlock(&extcon_dev_list_lock);
			of_node_put(node);
			return edev;
		}
	}
	mutex_unlock(&extcon_dev_list_lock);
	of_node_put(node);

	return ERR_PTR(-EPROBE_DEFER);
}

static struct extcon_dev *of_extcon_dev_get_by_cable_name(
		struct device_node *np, const char *cable_name,
		int *cable_index)
{
	struct extcon_dev *edev = NULL;
	struct of_phandle_args npspec;
	int ret;
	int index = 0;
	int cindex = -1;

	/*
	 * For named cable, first look up the name in the "extcon-names"
	 * property.  If it cannot be found, the index will be an error
	 * code, and of_extcon_dev_get_by_name() will fail.
	 */
	if (cable_name)
		index = of_property_match_string(np, "extcon-cable-names",
					cable_name);

	ret = of_parse_phandle_with_args(np, "extcon-cables", "#extcon-cells",
						index, &npspec);
	if (ret < 0)
		return ERR_PTR(ret);

	mutex_lock(&extcon_dev_list_lock);
	list_for_each_entry(edev, &extcon_dev_list, entry) {
		if (edev->dev.parent && edev->dev.parent->of_node == npspec.np) {
			cindex = npspec.args_count ? npspec.args[0] : 0;
			goto out;
		}
	}
	edev = NULL;
out:
	mutex_unlock(&extcon_dev_list_lock);

	of_node_put(npspec.np);
	if (!edev) {
		if (cable_name && index >= 0) {
			pr_err("ERROR: could not get extcon-dev %s:%s(%i)\n",
					np->full_name,
					cable_name ? cable_name : "", index);
			return ERR_PTR(-EPROBE_DEFER);
		}
	}

	if (cindex >= edev->max_supported) {
		pr_err("ERROR: Cable %s Index %d is not supported\n",
				cable_name ? cable_name : "", cindex);
		return ERR_PTR(-EINVAL);
	}

	*cable_index = cindex;
	return edev;
}

struct extcon_dev *extcon_get_extcon_dev_by_cable(struct device *dev,
			const char *cable_name)
{
	struct extcon_dev *edev;
	int index = -1;

	if (!dev || !dev->of_node || !cable_name)
		return ERR_PTR(-EINVAL);

	edev = of_extcon_dev_get_by_cable_name(dev->of_node,
					cable_name, &index);

	if (IS_ERR(edev) || index < 0)
		return ERR_PTR(-EPROBE_DEFER);
	return edev;
}
EXPORT_SYMBOL_GPL(extcon_get_extcon_dev_by_cable);

#else
struct extcon_dev *extcon_get_edev_by_phandle(struct device *dev, int index)
{
	return ERR_PTR(-ENOSYS);
}
#endif /* CONFIG_OF */
EXPORT_SYMBOL_GPL(extcon_get_edev_by_phandle);

/**
 * extcon_get_edev_name() - Get the name of the extcon device.
 * @edev:	the extcon device
 */
const char *extcon_get_edev_name(struct extcon_dev *edev)
{
	return !edev ? NULL : edev->name;
}

static int __init extcon_class_init(void)
{
	return create_extcon_class();
}
module_init(extcon_class_init);

static void __exit extcon_class_exit(void)
{
#if defined(CONFIG_ANDROID)
	class_compat_unregister(switch_class);
#endif
	class_destroy(extcon_class);
}
module_exit(extcon_class_exit);

MODULE_AUTHOR("Chanwoo Choi <cw00.choi@samsung.com>");
MODULE_AUTHOR("Mike Lockwood <lockwood@android.com>");
MODULE_AUTHOR("Donggeun Kim <dg77.kim@samsung.com>");
MODULE_AUTHOR("MyungJoo Ham <myungjoo.ham@samsung.com>");
MODULE_DESCRIPTION("External connector (extcon) class driver");
MODULE_LICENSE("GPL");
