#include <linux/module.h>
// #include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/seq_file.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>

#include "scull.h"		/* local definitions */
static int scull_major = SCULL_MAJOR;
static int scull_minor = SCULL_MINOR;
static int scull_num_devs = SCULL_NUM_DEVS;

static int scull_quantum = SCULL_QUANTUM;
static int scull_qset    = SCULL_QSET;

struct scull_dev *scull_devices;
module_param(scull_major, int, 0664);
MODULE_PARM_DESC(scull_major, "Major number for the scull driver");

module_param(scull_minor, int, 0664);
MODULE_PARM_DESC(scull_minor, "Minor number for the scull driver");

module_param(scull_quantum, int, 0664);
MODULE_PARM_DESC(scull_quantum, "Quantum size for the scull driver");

module_param(scull_qset, int, 0664);
MODULE_PARM_DESC(scull_qset, "Quantum set size for the scull driver");
struct file_operations scull_fops = { 
    .owner = THIS_MODULE,
    .llseek = scull_llseek,
    .read   = scull_read,
    .write  = scull_write,
    .open   = scull_open,
    .release= scull_release,
};

int scull_trim_mem(struct scull_dev *dev){
    struct scull_qset *next,*dptr;
    int qset = dev->qset;
    int i;
    for(dptr = dev->data;dptr;dptr = next){
        if(dptr->data){
            for(i = 0;i < qset;i++){
                kfree(dptr->data[i]);
            }
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }
    dev->size = 0;
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    dev->data = NULL;
    return 0;
}

/*将 c_dev 结构到这个 scull_devices */
static void scull_setup_cdev(struct scull_dev *dev,int index){

    int err,dev_t = MKDEV(scull_major,scull_minor + index);
    cdev_init(&dev->cdev,&scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops   = &scull_fops;
    err = cdev_add(&dev->cdev,dev_t,1);
    if(err){
        printk("error %d adding scull%d",err,index);
    }

}

int scull_open(struct inode* inode,struct file *filp){
	struct scull_dev *dev; /* device information */
	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev; /* 保存设备状态 供其他方法使用 */

	/* 如果是只写打开模式，则将设备长度修剪为 0  */
    // 只写模式、读写模式、只读模式都不同 只写模式一般会清空空间
	if ( (filp->f_flags & O_ACCMODE) == O_WRONLY) {
        //这里采取了简单的信号量并发机制
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
		scull_trim_mem(dev); /* ignore errors */
		up(&dev->sem);
	}
	return 0;          /* success */
}


// 内核会帮助自动清除 filp->private_data
int scull_release(struct inode * inode,struct file*filp){
    return 0;
}

/**
 * @brief 遍历量子集链表,返回指定位置的量子集指针
 *
 * @param dev 设备结构体指针
 * @param n 要访问的量子集索引
 *
 * @return 指向第 n 个量子集的指针,如果分配失败则返回 NULL
 */
struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data; // 获取第一个量子集指针
    // 能够在这里去实现内存的分配是因为 scull_read 做出了相关的限制，使其并不会越界访问
    /* 如果第一个量子集未分配,则显式分配 */
    if (!qs) {
        qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        if (qs == NULL)
            return NULL; // 分配失败,返回 NULL
        memset(qs, 0, sizeof(struct scull_qset)); // 初始化量子集
    }

    /* 然后遍历量子集链表 */
    while (n--) {
        if (!qs->next) { // 如果下一个量子集未分配
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if (qs->next == NULL)
                return NULL; // 分配失败,返回 NULL
            memset(qs->next, 0, sizeof(struct scull_qset)); // 初始化量子集
        }
        qs = qs->next; // 移动到下一个量子集
    }
    return qs; // 返回第 n 个量子集指针
}

ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    struct scull_dev *dev = filp->private_data; /* 获取设备结构体指针 */
    struct scull_qset *dptr;    /* 指向第一个量子集的指针 */
    int quantum = dev->quantum, qset = dev->qset; /* 获取量子大小和量子集数量 */
    int itemsize = quantum * qset; /* 计算每个量子集的总大小 */
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem)) /* 获取信号量锁 */
        return -ERESTARTSYS;

    if (*f_pos >= dev->size) /* 如果文件位置超出设备大小,直接返回 */
        goto out;

    if (*f_pos + count > dev->size) /* 如果要读取的数据超出设备大小,则调整读取大小 */
        count = dev->size - *f_pos;

    /* 计算要读取的数据在哪个量子集中,以及在量子集中的偏移位置 */
    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum; q_pos = rest % quantum;

    /* 遍历量子集链表,找到要读取的量子集 */
    dptr = scull_follow(dev, item);

    if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
        goto out; /* 如果量子集为空,则不读取数据 */

    /* 这里较书本浅浅改了下 实现能够读取多个量子集 而不是只读一个 */
    while (count > quantum - q_pos){
        size_t temp_count = quantum - q_pos;
        count = count - temp_count;
    /* 从量子集中复制数据到用户空间缓冲区 */
        if (copy_to_user(buf, dptr->data[s_pos] + q_pos, temp_count)) {
            retval = -EFAULT;
            goto out;
        }

        *f_pos += temp_count; /* 更新文件位置 */
        retval += temp_count; /* 设置返回值为实际读取的字节数 */
    }
out:
    up(&dev->sem); /* 释放信号量锁 */
    return retval;
}

/**
 * @brief 向 scull 设备写入数据
 *
 * @param filp 文件结构体指针
 * @param buf 用户空间缓冲区
 * @param count 要写入的字节数
 * @param f_pos 文件位置指针
 *
 * @return 实际写入的字节数,或者出错时返回错误码
 */
ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
    struct scull_dev *dev = filp->private_data; // 获取设备结构体指针
    struct scull_qset *dptr; // 用于指向目标量子集
    int quantum = dev->quantum, qset = dev->qset; // 获取量子大小和量子集数量
    int itemsize = quantum * qset; // 计算每个量子集的总大小
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0; /* 用于 "goto out" 语句的值 */
    size_t temp_count = count;
    if (down_interruptible(&dev->sem)) // 获取信号量锁
        return -ERESTARTSYS;
    //一点改动 可以写多个量子集
    while(count>0){
        /* 计算要写入的数据在哪个量子集中,以及在量子集中的偏移位置 */
        item = (long)*f_pos / itemsize;
        rest = (long)*f_pos % itemsize;
        s_pos = rest / quantum; q_pos = rest % quantum;
        temp_count = count;
        /* 遍历量子集链表,找到要写入的量子集 */
        dptr = scull_follow(dev, item);
        if (dptr == NULL) // 如果量子集不存在,则跳转到 out 标签
            goto out;
        if (!dptr->data) { // 如果量子集中没有数据指针数组
            dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL); // 分配数据指针数组
            if (!dptr->data)
                goto out;
            memset(dptr->data, 0, qset * sizeof(char *)); // 初始化数据指针数组
        }
        if (!dptr->data[s_pos]) { // 如果目标量子未分配
            dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL); // 分配目标量子
            if (!dptr->data[s_pos])
                goto out;
        }
        
        if (count > quantum - q_pos){
            temp_count = quantum - q_pos;
            count = count - temp_count;
        }

        if (copy_from_user(dptr->data[s_pos] + q_pos, buf, temp_count)) { // 从用户空间复制数据到设备内存
            retval = -EFAULT;
            goto out;
        }
        *f_pos += temp_count; // 更新文件位置指针
        retval += temp_count; // 设置返回值为实际写入的字节数

        /* 更新设备大小 */
        if (dev->size < *f_pos)
            dev->size = *f_pos;

    }

out:
    up(&dev->sem); // 释放信号量锁
    return retval;
}

/**
 * @brief 更新文件位置指针
 *
 * @param filp 文件结构体指针
 * @param off 偏移量
 * @param whence 偏移起始位置标志
 *
 * @return 新的文件位置,或者出错时返回错误码
 */
static loff_t scull_llseek(struct file *filp, loff_t off, int whence)
{
    struct scull_dev *dev = filp->private_data; // 获取设备结构体指针
    struct scull_qset *qset; // 用于遍历量子集链表
    loff_t newpos; // 存储新的文件位置

    if (down_interruptible(&dev->sem)) // 获取信号量锁
        return -ERESTARTSYS;

    switch (whence) { // 根据 whence 参数计算新的文件位置
    case 0: /* SEEK_SET */
        newpos = off; // 新位置为偏移量 off
        break;
    case 1: /* SEEK_CUR */
        newpos = filp->f_pos + off; // 新位置为当前位置加上偏移量 off
        if(newpos > dev->size){
            newpos = dev->size;
        }
        break;
    case 2: /* SEEK_END */
        newpos = dev->size - off;
        break;
    default:
        up(&dev->sem); // 释放信号量锁
        return -EINVAL; // 无效的 whence 参数
    }

    if (newpos < 0 || newpos > dev->size) { // 检查新位置是否超出设备大小限制
        up(&dev->sem); // 释放信号量锁
        return -EINVAL; // 返回 EINVAL 错误码
    }

    filp->f_pos = newpos; // 更新文件位置指针
    up(&dev->sem); // 释放信号量锁
    return newpos; // 返回新的文件位置
}

/**
 * @brief 清理和卸载 scull 驱动程序
 */
void scull_cleanup_module(void)
{
    int i;
    dev_t devno = MKDEV(scull_major, scull_minor); // 构造设备号

    /* 删除字符设备条目 */
    if (scull_devices) {
        for (i = 0; i < scull_num_devs; i++) {
            scull_trim_mem(scull_devices + i); // 修剪每个设备的内存
            cdev_del(&scull_devices[i].cdev); // 删除字符设备条目
        }
        kfree(scull_devices); // 释放设备数组的内存
    }

    /* 如果注册失败,cleanup_module 就不会被调用 */
    unregister_chrdev_region(devno, scull_num_devs); // 注销字符设备驱动程序

    /* 调用其他模块的清理函数 */
    //scull_p_cleanup();
    //scull_access_cleanup();
}

int scull_init_module(void)   /*获取主设备号，或者创建设备编号*/
{
    int result ,i;
    dev_t dev = 0;
    if(scull_major){
        dev = MKDEV(scull_major,scull_minor);     /*将两个设备号转换为dev_t类型*/
        result = register_chrdev_region(dev,scull_num_devs,"scull");/*申请设备编号*/
    }else{
        result = alloc_chrdev_region(&dev,scull_minor,scull_num_devs,"scull");/*分配主设备号*/
        scull_major = MAJOR(dev);
    }

    if(result < 0){
        printk("scull : cant get major %d\n",scull_major);
        return result;
    }else{
        printk("register a dev_t %d %d\n",scull_major,scull_minor);
    }
    
    /*分配设备的结构体*/
    scull_devices = kmalloc(scull_num_devs * sizeof(struct scull_dev),GFP_KERNEL);

    if(!scull_devices){
        result = -1;
        goto fail;
    }
    memset(scull_devices,0,scull_num_devs * sizeof(struct scull_dev));

    for(i = 0;i < scull_num_devs;i++){
        scull_devices[i].quantum = scull_quantum;
        scull_devices[i].qset = scull_qset;
        sema_init(&scull_devices[i].sem,1);
        /*sema_init  是内核用来新代替dev_INIT 的函数，初始化互斥量*/
        scull_setup_cdev(&scull_devices[i],i);
        /*注册每一个设备到总控结构体*/
    }

    return 0;

fail:
    scull_cleanup_module();
    return result;

}

module_init(scull_init_module);
module_exit(scull_cleanup_module);


