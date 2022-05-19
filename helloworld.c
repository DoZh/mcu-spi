
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/of_device.h>

#define  DEVICE_NAME "mydev"
#define  CLASS_NAME  "hello_class"

static struct class*  helloClass;
static struct cdev my_dev;

int major_no;
dev_t dev;

static int my_dev_open(struct inode *inode, struct file *file)
{
	pr_info("my_dev_open() is called.\n");
	return 0;
}

static int my_dev_close(struct inode *inode, struct file *file)
{
	pr_info("my_dev_close() is called.\n");
	return 0;
}

static long my_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	pr_info("my_dev_ioctl() is called. cmd = %d, arg = %ld\n", cmd, arg);
	return 0;
}

/* declare a file_operations structure */
static const struct file_operations my_dev_fops = {
	.owner			= THIS_MODULE,
	.open 		 	= my_dev_open,
	.release 	 	= my_dev_close,
	.unlocked_ioctl 	= my_dev_ioctl,
};

static int my_probe(struct platform_device *pdev)
{
	int ret;

	struct device* helloDevice;

	pr_info("Hello world probe\n");

	/* Get the device identifiers */
	dev = MKDEV(major_no,0);


	/* Initialize the cdev structure and add it to the kernel space */
	cdev_init(&my_dev, &my_dev_fops); // init cdev struct my_dev
	ret = cdev_add(&my_dev, dev, 1); // add kernel device by dev id
	if (ret < 0){
		unregister_chrdev_region(dev, 1);
		pr_info("Unable to add cdev\n");
		return ret;
	}

	/* Register the device class */
	helloClass = class_create(THIS_MODULE, CLASS_NAME); // make /sys/class/hello_class
	if (IS_ERR(helloClass)){
		unregister_chrdev_region(dev, 1);
		cdev_del(&my_dev);
	    pr_info("Failed to register device class\n");
	    return PTR_ERR(helloClass);
	}
	pr_info("device class registered correctly\n");

	/* Create a device node named DEVICE_NAME associated a dev */
	helloDevice = device_create(helloClass, NULL, dev, NULL, DEVICE_NAME); // make /sys/class/hello_class/mydev & /dev/mydev
	if (IS_ERR(helloDevice)){
	    class_destroy(helloClass);
	    cdev_del(&my_dev);
	    unregister_chrdev_region(dev, 1);
	    pr_info("Failed to create the device\n");
	    return PTR_ERR(helloDevice);
	}
	pr_info("The device is created correctly\n");

	return 0;
}

static int my_remove(struct platform_device *pdev)
{

	device_destroy(helloClass, dev);     /* remove the device */
	class_destroy(helloClass);           /* remove the device class */
	cdev_del(&my_dev);
	unregister_chrdev_region(dev, 1);    /* unregister the device numbers */
	pr_info("Hello world remove\n");
	return 0;
}

/* Declare a list of devices supported by the driver */
static const struct of_device_id my_of_ids[] = {
	{ .compatible = "dozh,hello"},
	{},
};
MODULE_DEVICE_TABLE(of, my_of_ids);

/* 
To device tree file
        helloworld {
                compatible = "dozh,hello";
        };
*/

/* Define platform driver structure */
static struct platform_driver my_platform_driver = {
	.probe = my_probe,
	.remove = my_remove,
	.driver = {
		.name = "helloworld",
		.of_match_table = my_of_ids,
		.owner = THIS_MODULE,
	}
};

static int __init hello_init(void)
{
	dev_t dev_no;
	int ret_val;

	pr_info("Hello world init\n");
		/* Allocate dynamically device numbers */
	ret_val = alloc_chrdev_region(&dev_no, 0, 1, DEVICE_NAME);
	if (ret_val < 0){
		pr_info("Unable to allocate Mayor number \n");
		return ret_val;
	}

	/* Get the major device identifiers */
	major_no = MAJOR(dev_no);


	pr_info("Allocated correctly with major number %d\n", major_no);

	ret_val = platform_driver_register(&my_platform_driver);
	if (ret_val !=0)
	{
		pr_err("platform value returned %d\n", ret_val);
		return ret_val;
	}
	return 0;
}

static void __exit hello_exit(void)
{
	pr_info("Hello world exit\n");
	platform_driver_unregister(&my_platform_driver);
}


/* init driver module */
module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DoZh <TATQAQTAT@gmail.com>");
MODULE_DESCRIPTION("This is a print out Hello World module");


