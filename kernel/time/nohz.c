
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/module.h>
//#include <linux/percpu_defs.h>

#include "tick-internal.h"

int ncpus_per_dom = 4;
extern ktime_t tick_period;
ktime_t tick_nohz_period = { .tv64 = 100 * NSEC_PER_SEC / HZ }; // signed long long

DEFINE_PER_CPU(int, nohz_on) = 0;

bool cpu_nohz_on(int cpu)
{
	return per_cpu(nohz_on, cpu) > 0;
}

int cpu_get_nohz_target(int cpu)
{

	if(cpu >= 0 && cpu < NR_CPUS)
		cpu = !per_cpu(nohz_on, cpu) ? cpu : ((cpu / ncpus_per_dom) * ncpus_per_dom);

	return cpu;
}

ktime_t get_cpu_tick_period(int cpu)
{
	return per_cpu(nohz_on, cpu) > 0 ? tick_nohz_period : tick_period;
}

static ssize_t cpd_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d\n", ncpus_per_dom);
}

static ssize_t cpd_set(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int val;
	sscanf(buf, "%d", &val);
	if(val > 0)
		ncpus_per_dom = val;
	return count;
}

static struct kobj_attribute cpd_attribute =
	__ATTR(cpd, 0666, cpd_show, cpd_set);


static ssize_t tnp_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%lld\n", tick_nohz_period.tv64);
}

static ssize_t tnp_set(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	sscanf(buf, "%lld", &tick_nohz_period.tv64);
	return count;
}

static struct kobj_attribute tnp_attribute =
	__ATTR(tnp, 0666, tnp_show, tnp_set);


static struct attribute *attrs[] = {
	&cpd_attribute.attr,
	&tnp_attribute.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *nohz_kobj;

static int __init nohz_kobj_init(void)
{
	int retval;

	nohz_kobj = kobject_create_and_add("nohz", nohz_kobj);
	if (!nohz_kobj)
		return -ENOMEM;

	retval = sysfs_create_group(nohz_kobj, &attr_group);
	if (retval)
		kobject_put(nohz_kobj);
	printk(KERN_INFO "nohz kernel object created.");
	return retval;
}

static void __exit nohz_kobj_exit(void)
{
	printk(KERN_INFO "nohz kernel object destroyed.");
	kobject_put(nohz_kobj);
}

module_init(nohz_kobj_init);
module_exit(nohz_kobj_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hakan Akkan <hakkan@nmt.edu>");



