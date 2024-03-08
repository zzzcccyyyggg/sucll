#ifndef SCULL_MAJOR
#define SCULL_MAJOR 0   /* 默认为0 即采取动态获取设备号策略 */
#endif

#ifndef SCULL_NUM_DEVS
#define SCULL_NUM_DEVS 4    /* scull 设备的数量 */
#endif

#ifndef SCULL_MINOR
#define SCULL_MINOR 0	/* 次设备号起始 默认为零 */
#endif

#ifndef SCULL_QUANTUM
#define SCULL_QUANTUM 4000
#endif

#ifndef SCULL_QSET
#define SCULL_QSET    1000
#endif

struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
    struct scull_qset *data;  /* 指向第一个量子集的指针 */
    int quantum;              /* 当前量子的大小 */
    int qset;                 /* 当前数组的大小 */
    unsigned long size;       /* 存储在这里的数据量 */
    unsigned int access_key;  /* 由 sculluid 和 scullpriv 使用 */
    struct semaphore sem;     /* 互斥信号量 */
    struct cdev cdev;         /* 字符设备结构 */
};

int scull_trim_mem(struct scull_dev *dev);

ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
                   loff_t *f_pos);
ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
                    loff_t *f_pos);
static loff_t scull_llseek(struct file *filp, loff_t off, int whence);

int scull_open(struct inode *inode, struct file *filp);

int scull_release(struct inode *inode, struct file *filp);