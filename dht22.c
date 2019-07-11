/*
 * DHT22 Humidity And Temperature Sensor Driver
 * 
 * Copyright (c) Edward Lin <edwardlin.tw@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/time64.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/current.h>
#include <linux/uaccess.h>
#define _INCLUDE_DHT22_DECL
#include "dht22.h"

static const int    high = 1;
static const int    low  = 0;

/*
 * module parameters, please refer to README.md
 */
static int gpio = DEFAULT_GPIO;
module_param(gpio, int, S_IRUGO);
MODULE_PARM_DESC(gpio, "Assigned GPIO number of DHT22 data pin, default is 4");

static bool autoupdate = true;
module_param(autoupdate, bool, S_IRUGO);
MODULE_PARM_DESC(autoupdate, "automatically trigger or not, default is 1(int); "
                             "0 is off, others are all on");

static int autoupdate_sec = DEFAULT_AUTOUPDATE_SEC;
module_param(autoupdate_sec, int, S_IRUGO);
MODULE_PARM_DESC(autoupdate_sec, 
                 "Seconds between two trigger events (if autoupdate is on), "
                 "default is 10 seconds; "
                 "the value must be >= 3(sec) and <= 60000(10min)");

/*
 * module's attributes; please refer to README.md
 */
static ATTR_RO(gpio);
static ATTR_RW(autoupdate);
static ATTR_RW(autoupdate_sec);
static ATTR_RO(humidity);
static ATTR_RO(temperature);
static ATTR_WO(trigger);
static ATTR_WO(debug);

static struct attribute* dht22_attrs[] = {
    &gpio_attr.attr,
    &autoupdate_attr.attr,
    &autoupdate_sec_attr.attr,
    &humidity_attr.attr,
    &temperature_attr.attr,
    &trigger_attr.attr,
    &debug_attr.attr,
    NULL
};

static struct attribute_group attr_group = {
    .attrs = dht22_attrs,
};

/*
 * device node
 * must add a rule file to RPi '/etc/udev/rules.d/51-dht22.rules' 
 * content:
 * KERNEL="dht22:[0-9]*", GROUP="root", MODE="0444"
 */
static int                      device_major = 0;
static int                      num_devs = 2;
static struct cdev              dht22_cdev;
static dev_t                    dht22_dev;
static struct class*            dht22_class = NULL;
static struct file_operations   dht22_fops = {
    .open       = dev_open,
};
static struct file_operations   dht22_fops_h = {
    .open       = dev_open_h,
    .read       = dev_read_h,
    .release    = dev_close,
};
static struct file_operations   dht22_fops_t = {
    .open       = dev_open_t,
    .read       = dev_read_t,
    .release    = dev_close,
};

/* 
 * other global static vars
 */
static rwlock_t             lock;
static int                  irq_number;
static struct hrtimer       autoupdate_timer;
static struct hrtimer       timeout_timer;
static struct timespec64    prev_high_low_time;
static const int            timeout_time = 1;  /* 1 second */
static const int            timeout_time_ms = 500; /* 0.5 second */
static int                  low_irq_count = 0;
static int                  irq_count  = 0;
static int                  humidity = 0;      /* cache last humidity */
static int                  temperature = 0;   /* cache last temperature */
static struct kobject*      dht22_kobj;
static bool                 dbg_flag = false;  /* log more info if true */
/*
 * the following will be printed if dbg_flag is true
 */
static int                  dbg_fail_read  = 0;
static int                  dbg_total_read = 0;

/*
 * int[40] to record signal HIGH time duration, for calculating bit 0/1
 * DHT22 spec: 22~30us HIGH is 0, 68~75us HIGH is 1
 * DHT22 sends out 2-byte humidity, 2-byte temperature and 1-byte parity(CRC)
 */
static int high_time[40] = { 0 };
static enum { dht22_idle, dht22_working } dht22_state = dht22_idle;

/* 
 * queue work to calculate humidity/temperature/crc(parity check) 
 * make IRQ handler to return as soon as possible
 */
static DECLARE_WORK(process_work, process_results);

static int __init dht22_init(void)
{
    int     ret;

    pr_err("Loading dht22 module...\n");
    rwlock_init(&lock);

    /* device node */
    ret = dht22_dev_init();
    if (ret)
        return ret;

    /* setup GPIO */
    if (!gpio_is_valid(gpio)) {
        pr_err("dht22 can't validate GPIO %d; unloaded\n", gpio);
        return -EINVAL;
    }
    ret = gpio_request(gpio, "sysfs");
    if (ret < 0) {
        pr_err("dht22 failed to request GPIO %d, unloaded\n",gpio);
        return ret;
    }

    gpio_export(gpio, true);
    gpio_direction_output(gpio, high);

    /* setup interrupt handler */
    irq_number = gpio_to_irq(gpio);
    if (irq_number < 0) {
        pr_err("dht22 failed to get IRQ for GPIO %d, unloaded\n", gpio);
        ret = irq_number;
        goto free_gpio;
    }
    pr_err("dht22 assign IRQ %d to GPIO %d.\n", irq_number, gpio);
    ret = request_irq(irq_number,
            dht22_irq_handler,
            IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
            "dht22_irq_handler",
            NULL);
    if (ret < 0) {
        pr_err("idht22 failed to request IRQ, unloaded.\n");
        goto free_gpio;
    }

    /* kobject */
    dht22_kobj = kobject_create_and_add("dht22", kernel_kobj);
    if (NULL == dht22_kobj) {
        pr_err("DHT22 failed to create kobject mapping\n");
        ret = -EINVAL;
        goto free_gpio;
    }

    /* sysfs attribute */
    ret = sysfs_create_group(dht22_kobj, &attr_group);
    if (ret) {
        pr_err("DHT22 failed to create sysfs group.\n");
        goto sysfs_err;
    }

    /*
     * wait for 2sec (for DHT22 warming up) for the first trigger
     * no matter autoupdate is ON or OFF
     * at least DHT22 will be triggered once
     */
    dht22_timer_init(&autoupdate_timer, autoupdate_func, true, 2);
    /*
     * setup timeout timer, but not start yet
     * it'll start when triggering DHT22 to request data
     */
    dht22_timer_init(&timeout_timer, timeout_func, false, 0);

    pr_err("dht22 loaded.\n");

    return 0;

sysfs_err:
    kobject_put(dht22_kobj);

free_gpio:
    gpio_unexport(gpio);
    gpio_free(gpio);
    return ret;
}

static void __exit dht22_exit(void)
{
    free_irq(irq_number, NULL);
    gpio_unexport(gpio);
    gpio_free(gpio);
    hrtimer_cancel(&autoupdate_timer);
    hrtimer_cancel(&timeout_timer);
    cancel_work_sync(&process_work);
    kobject_put(dht22_kobj);
    dht22_dev_exit();
    pr_err("dht22 unloaded.\n");
}

/* to create
 * "/dev/dht22:0" for reading humidity, and
 * "/dev/dht22:1" for reading temperature
 */
static int dht22_dev_init(void)
{
    dev_t dev = MKDEV(device_major, 0); /* dynamic allocation of 'major' */
    int   alloc_ret = 0;
    int   cdev_err = 0;
    int   i;

    alloc_ret = alloc_chrdev_region(&dev, 0, num_devs, "dht22");
    if (alloc_ret)
        goto error;

    device_major = MAJOR(dev);
    cdev_init(&dht22_cdev, &dht22_fops);
    dht22_cdev.owner = THIS_MODULE;
    dht22_cdev.ops  = &dht22_fops;

    cdev_err = cdev_add(&dht22_cdev, MKDEV(device_major, 0), num_devs);
    if (cdev_err)
        goto error;

    /* 
     * create class first, and device files associated to this class
     */
    dht22_class = class_create(THIS_MODULE, "dht22");
    if (IS_ERR(dht22_class))
        goto error;

    /*
     * must add a file to RPi '/etc/udev/rules.d/51-dht22.rules' 
     * content:
     * KERNEL="dht22:[0-9]*", GROUP="root", MODE="0444"
     *
     * NOET: the following 5th param is
     * "dht22:%d", NOT "dht22%d"
     */
    for (i = 0; i < num_devs; ++i) {
        dht22_dev = MKDEV(device_major, i);
        device_create(dht22_class, NULL, dht22_dev, NULL, "dht22:%d", i);
    }
    
    pr_err("dht22 driver (major %d) installed\n", device_major);
    return 0;

error:
    if (cdev_err == 0)
        cdev_del(&dht22_cdev);
    if (0 == alloc_ret)
        unregister_chrdev_region(dev, num_devs);

    return -1;    
}

static void dht22_dev_exit(void)
{
    dev_t   dev = MKDEV(device_major, 0);

    device_destroy(dht22_class, dht22_dev);
    class_destroy(dht22_class);
    cdev_del(&dht22_cdev);
    unregister_chrdev_region(dev, num_devs);
}

static int dev_open(struct inode* inode, struct file* file)
{
    if (dbg_flag) {
        pr_info("dht22:%s major %d, minor %d (pid %d)\n", __func__,
                                                         imajor(inode),
                                                         iminor(inode),
                                                         current->pid);
    }

    switch (iminor(inode)) {
        case 0: /* humidity */
            file->f_op = &dht22_fops_h;
            break;
        case 1: /* temperature */
            file->f_op = &dht22_fops_t;
            break;
        default:
            return -ENXIO;
    }

    file->private_data = NULL;
    if (file->f_op && file->f_op->open)
        return file->f_op->open(inode, file);

    return 0;
}

static int dev_open_h(struct inode* inode, struct file* file)
{
    return 0;
}
static int dev_open_t(struct inode* inode, struct file* file)
{
    return 0;
}

static int dev_close(struct inode* inode, struct file* file)
{
    if (dbg_flag)
        pr_info("dht22:%s\n", __func__);
    return 0;
}

static ssize_t dev_read_h(struct file* file, char __user* buf, 
                          size_t count, loff_t* f_pos)
{
    int data;
    read_lock(&lock);
    data = humidity;
    read_unlock(&lock);
    return read_data(file, buf, count, f_pos, data, 1000);
}

static ssize_t dev_read_t(struct file* file, char __user* buf, 
                          size_t count, loff_t* f_pos)
{
    int data;
    read_lock(&lock);
    data = temperature;
    read_unlock(&lock);
    return read_data(file, buf, count, f_pos, data, 10);
}

static ssize_t read_data(struct file* file, char __user* buf, size_t count, 
                         loff_t* f_pos,  int data, int factor)
{
    char                  tmp[IO_BUF_MAX];
    size_t                len;
    int                   retval = 0;

    if (*f_pos > 0)
        return 0;

    sprintf( tmp, "%d.%d\n", data/factor, abs(data)%factor);

    len = strlen(tmp);
    len = min(len, count-1);
    tmp[len] = '\0';
    if (copy_to_user(buf, tmp, len))
        retval = -EFAULT;
    else {
        retval = len+1;
        *f_pos += retval;
    }

    return retval;
}

static void dht22_timer_init(struct hrtimer* timer, 
                             enum hrtimer_restart (*func)(struct hrtimer*),
                             bool start_now,
                             int  wait_sec)
{
    hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    timer->function = func;
    if (start_now)
        hrtimer_start(timer, ktime_set(wait_sec,0), HRTIMER_MODE_REL);
}

static void to_trigger_dht22(void)
{
    /* DHT22 working in progress, ignore this event */
    if (dht22_working == dht22_state) {
        pr_info("DHT22 is busy, ignore trigger event.....\n");
        return;
    }

    low_irq_count = 0;
    irq_count = 0;
    dht22_state   = dht22_working;

    hrtimer_start(&timeout_timer, 
                  ktime_set(timeout_time, NSEC_PER_MSEC * timeout_time_ms), 
                  HRTIMER_MODE_REL);

    trigger_dht22();
}

static void trigger_dht22(void)
{
    getnstimeofday64(&prev_high_low_time);
    /*
     * pull down bus at least 1ms
     * to signal DHT22 for preparing humidity/temperature data
     */
    gpio_direction_output(gpio, low);
    udelay(1000);

    /*
     * release bus (bus return to HIGH, due to pull-up resistor)
     * switch GPIO to input mode to receive data from DHT22
     * let the interrupt handler to process the followings
     */
    gpio_direction_input(gpio);
}

static enum hrtimer_restart timeout_func(struct hrtimer* hrtimer)
{
    ++dbg_total_read;
    /* pull high, and wait for next trigger */
    gpio_direction_output(gpio, high);

    if (dht22_idle != dht22_state) {
        /*
         * host receive fewer yhan 86 interrupts
         * no results were produced
         * reset state to 'dht22_idle' and wait for next trigger (if autoupdate)
         */
        pr_info("Failed to fetch DHT22 data\n");
        ++dbg_fail_read;
        dht22_state = dht22_idle;
    }
    if (dbg_flag) {
        pr_info("total read %d, fail %d\n", dbg_total_read, dbg_fail_read);
        pr_info("last IRQ count (should be 86) %d\n", irq_count);
    }
    return HRTIMER_NORESTART;
}

static enum hrtimer_restart autoupdate_func(struct hrtimer *hrtimer)
{
    if (autoupdate)
        to_trigger_dht22();

    /*
     * only trigger DHT22 when 'autoupdate' is enabled
     * so keep the timerr continue flying
     */ 
    hrtimer_forward(hrtimer, ktime_get(), ktime_set(autoupdate_sec, 0));
    return HRTIMER_RESTART;
}

static void process_results(struct work_struct* work)
{
    int data[5] = { 0 }; /* 2-byte humidity, 2-byte temperature, 1-byte CRC */
    int i;
    int raw_humidity;
    int raw_temp;
    int byte;

    /* 
     * determine bit value 0 or 1
     * DHT22 spec: 22-30us is 0, 68~75us is 1
     * since DHT22's condition may be not as precise as spec, 
     * threshoud 50us is taken for decision making
     */
    for (i = 0; i < 40; i++) {
        data[(byte=(i>>3))] <<= 1;
        data[byte         ]  |= (high_time[i] > 50); 
    }

    raw_humidity = (data[0] << 8) | data[1];
    raw_temp     = (data[2] << 8) | data[3];

    /* be aware of temperature below 0°C */
    if (1 == (data[2] & 0x8000))
        raw_temp = -(raw_temp & 0x7FFF);
   
    pr_info("humidity    = %d.%d\n", raw_humidity/10, raw_humidity%10);
    pr_info("temperature = %d.%d\n", raw_temp/10, abs(raw_temp)%10);
    
    if (dbg_flag) {
        pr_info("DHT22 raw data 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X",
                data[0], data[1], data[2], data[3], data[4]);
    }

    if (data[4] == ((data[0]+data[1]+data[2]+data[3]) & 0x00FF)) {
        write_lock(&lock);
        humidity    = raw_humidity;
        temperature = raw_temp;
        write_unlock(&lock);
        /*
         * notify all user processes which called poll() to fetch
         * humidity (via /dev/dht22:0) and/or temperature (via /dev/dht22:1)
         * refer to sample user space application: poll.c
         */
        sysfs_notify(dht22_kobj, NULL, "humidity");
        sysfs_notify(dht22_kobj, NULL, "temperature");
        if (dbg_flag)
            pr_info("CRC: OK\n");
    }
    else if (dbg_flag)
        pr_info("CRC: Error\n");
    else
        ;
}

static irqreturn_t dht22_irq_handler(int irq, void* data)
{
    int               val = gpio_get_value(gpio);
    static const int  h_pos = 3;      /* 2nd bit humidity low */
    static const int  f_pos = 42;     /* DHT22 final (last) low */
    struct timespec64 now;
    struct timespec64 diff;

    getnstimeofday64(&now);
    diff = timespec64_sub(now, prev_high_low_time);

    /* 
     * capture falling-edge interrupt and calculating
     * previous one high signal time duration, record it down
     * for calculating individual bit is 0 or 1 later
     */
    if (0 == val) {
        if (low_irq_count >= h_pos && low_irq_count <= f_pos) {
            /* 
             * to minimize IRQ CPU time,
             * only to record time duration;
             * calcalute bit 0/1 later via work queue
             * 22~30us is low (bit is 0), 68~75us is high (bit is 1)
             */
            high_time[low_irq_count-h_pos] = (int)(diff.tv_nsec/NSEC_PER_USEC);

            /*
             * no more data to receive;
             * calculating 40 bits' value (0 or 1) via queue work
             */
            if (low_irq_count == f_pos)
                queue_work(system_highpri_wq, &process_work);
        }
        ++low_irq_count;
    }

    if (86 == ++irq_count) {
        dht22_state = dht22_idle;
        if (dbg_flag)
            pr_info("DHT22 received 86 interrupts\n");
    }
    
    prev_high_low_time = now;

    return IRQ_HANDLED;
}

/*
 * sysfs attributes
 * the followings two declared in dht22.h
 *
 * (lazy macros)
 *
#define DECL_ATTR_SHOW(F)  ssize_t F ## _show (struct kobject* kobj,\
                                               struct kobj_attribute* attr,\
                                               char* buf)
#define DECL_ATTR_STORE(F) ssize_t F ## _store(struct kobject* kobj,\
                                               struct kobj_attribute* attr,\
                                               const char* buf,\
                                               size_t count)
*/

/* cat gpio */
static DECL_ATTR_SHOW (gpio)
{
    return sprintf(buf, "%d\n", gpio);
}

/* cat autoupdate */
static DECL_ATTR_SHOW (autoupdate)
{
    return sprintf(buf, "%d\n", autoupdate);
}

/* echo 1 > autoupdate */
static DECL_ATTR_STORE(autoupdate)
{
    int tmp;
    int new_auto;

    sscanf(buf, "%d\n", &tmp);
    new_auto = 0 != tmp;

    /*
     * the autoupdate timer is still flying
     * just update 'autoupdate' flag
     */
    if (new_auto != autoupdate)
        autoupdate = new_auto;

    if (dbg_flag)
        pr_info("DHT22 autoupdate : %d\n", autoupdate);

    return count;
}

/* cat autoupdate_sec */
static DECL_ATTR_SHOW (autoupdate_sec)
{
    return sprintf(buf, "%d\n", autoupdate_sec);
}

/* echo 10 > autoupdate_sec */
static DECL_ATTR_STORE(autoupdate_sec)
{
    int tmp;

    sscanf(buf, "%d\n", &tmp);

    if (tmp >= AUTOUPDATE_SEC_MIN && tmp <= AUTOUPDATE_SEC_MAX)
        autoupdate_sec = tmp;

    if (dbg_flag)
        pr_info("autoupdate duration %d sec\n", autoupdate_sec);

    return count;
}

/* cat humidity */
static DECL_ATTR_SHOW (humidity)
{
    int data;
    read_lock(&lock);
    data = humidity;
    read_unlock(&lock);
    return sprintf(buf, "%d.%d%%\n", data/10, data%10);
}

/* cat temperature */
static DECL_ATTR_SHOW (temperature)
{
    int data;
    read_lock(&lock);
    data = temperature;
    read_unlock(&lock);
    return sprintf(buf, "%d.%d°C\n", data/10, abs(data)%10);
}

/* echo 1 > trigger */
static DECL_ATTR_STORE(trigger)
{
    to_trigger_dht22();
    if (dbg_flag)
        pr_info("Now trigger DHT22.\n");
    return count;
}

/* echo 1 > debug */
static DECL_ATTR_STORE(debug)
{
    int tmp;

    sscanf(buf, "%d\n", &tmp);
    dbg_flag = 0 != tmp;

    return count;
}

module_init(dht22_init);
module_exit(dht22_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Edward Lin <edwardlin.tw@gmail.com");
MODULE_DESCRIPTION("A driver for DHT22 humidity/temperature sensor");
MODULE_VERSION("0.1");

