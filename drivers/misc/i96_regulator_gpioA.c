#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>

#include <linux/regulator/consumer.h>
#include <mach/regulator.h>

#define DEV_NAME "buddy_test"

static int up_gpio(void)
{
    struct regulator *lcd_reg;

    lcd_reg = regulator_get(NULL, LDO_LCD);
    if (IS_ERR(lcd_reg)) {
        printk(KERN_ERR "Can't find lcd regulator devices\n");
	return 0;
    }

    regulator_set_voltage(lcd_reg, 2560000, 2560000);
    printk("Get LCD regulator\n");
    if (regulator_enable(lcd_reg) < 0) {
        printk(KERN_ERR "Can't enable lcd regulator\n");
	return 0;
    }
    printk("Enable regulator\n");
    
    return 0;
}

/*
 * open operation
 */
static int buddy_open(struct inode *inode,struct file *filp)
{
    printk(KERN_INFO "Open device\n");
    return 0;
}
/*
 *release operation
 */
static int buddy_release(struct inode *inode,struct file *filp)
{
    printk(KERN_INFO "Release device\n");
    return 0;
}
/*
 * write operation
 */
static ssize_t buddy_write(struct file *filp,const char __user *buf,size_t count,loff_t *offset)
{
    printk(KERN_INFO "Write device\n");
    return 0;
}
/*
 * read operation
 */
static ssize_t buddy_read(struct file *filp,char __user *buf,size_t count,
		loff_t *offset)
{
    printk(KERN_INFO "Read device\n");
    up_gpio();
    return 0;
}
/*
 * file_operations
 */
static struct file_operations buddy_fops = {
    .owner     = THIS_MODULE,
    .open      = buddy_open,
    .release   = buddy_release,
    .write     = buddy_write,
    .read      = buddy_read,
};
/*
 * misc struct 
 */

static struct miscdevice buddy_misc = {
    .minor    = MISC_DYNAMIC_MINOR,
    .name     = DEV_NAME,
    .fops     = &buddy_fops,
};
/*
 * Init module
 */
static __init int buddy_init(void)
{
    misc_register(&buddy_misc);
    printk("buddy_test\n");
    return 0;
}
/*
 * Exit module
 */
static __exit void buddy_exit(void)
{
    misc_deregister(&buddy_misc);
}
/*
 * module information
 */
module_init(buddy_init);
module_exit(buddy_exit);

MODULE_LICENSE("GPL");

