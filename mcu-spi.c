
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/uaccess.h>


/* This structure will represent single device */
struct mcuspi_dev {
	struct spi_device * device;
	struct miscdevice mcu_spi_miscdevice;
	char name[8]; /* mcuspiX */
};


/* User is reading data from /dev/mcuspiX */
static ssize_t mcuspi_read_file(struct file *file, char __user *userbuf,
                               size_t count, loff_t *ppos)
{
	int expval, size;
	//char buf[1024];
	char *buf;
	struct mcuspi_dev * mcuspi;


	/* calc mcuspi addr by miscdevice addr. miscdevice addr fill into
	 * file->private_data by misc_open()*/
	mcuspi = container_of(file->private_data, 
			     struct mcuspi_dev, 
			     mcu_spi_miscdevice);

	buf = kzalloc(count + 1, GFP_KERNEL);
	/* read IO expander input to expval */
	expval = spi_read(mcuspi->device, buf, count);
	if (expval < 0)
		return -EFAULT;

	/* 
         * converts expval in 2 characters (2bytes) + null value (1byte)
	 * The values converted are char values (FF) that match with the hex
	 * int(s32) value of the expval variable.
	 * if we want to get the int value again, we have to
	 * do Kstrtoul(). We convert 1 byte int value to
	 * 2 bytes char values. For instance 255 (1 int byte) = FF (2 char bytes).
	 */
	size = sprintf(buf, "%04x", expval);

	/* 
         * replace NULL by \n. It is not needed to have the char array
	 * ended with \0 character.
	 */
	buf[size] = '\n';

	/* send size+1 to include the \n character */
	if(*ppos == 0){
		if(copy_to_user(userbuf, buf, size+1)){
			pr_info("Failed to return led_value to user space\n");
			return -EFAULT;
		}
		*ppos+=1;
		return size+1;
	}

	return 0;
}

/* Writing from the terminal command line, \n is added */
static ssize_t mcuspi_write_file(struct file *file, const char __user *userbuf,
                                   size_t count, loff_t *ppos)
{
	int ret = 0;

	//char buf[1024];
	char *buf;
	struct mcuspi_dev * mcuspi;

	mcuspi = container_of(file->private_data,
			     struct mcuspi_dev, 
			     mcu_spi_miscdevice);

	dev_info(&mcuspi->device->dev, 
		 "mcuspi_write_file entered on %s\n", mcuspi->name);

	dev_info(&mcuspi->device->dev,
		 "we have written %zu characters to file\n", count); 


	buf = kzalloc(count + 1, GFP_KERNEL);

	if(copy_from_user(buf, userbuf, count)) {
		dev_err(&mcuspi->device->dev, "Bad copied value\n");
		return -EFAULT;
	}

	buf[count-1] = '\0';

	ret |= spi_write(mcuspi->device, buf, count);

	if (ret < 0)
		dev_err(&mcuspi->device->dev, "the device is not found, ERRNO: %d\n", ret);
	else
		dev_info(&mcuspi->device->dev, "we have written characters to spi bus.\n"); 

	dev_info(&mcuspi->device->dev, 
		 "mcuspi_write_file exited on %s\n", mcuspi->name);

	return count;
}

/* declare a file_operations structure */
static const struct file_operations mcuspi_fops = {
	.owner = THIS_MODULE,
	.read = mcuspi_read_file,
	.write = mcuspi_write_file,
};

static int mcu_spi_probe(struct spi_device *spid)
{
	int ret;

	static int counter = 0;

	struct mcuspi_dev * mcuspi;

	pr_info("mcu_spi probe\n");

	/* Allocate new structure representing device */
	mcuspi = devm_kzalloc(&spid->dev, sizeof(struct mcuspi_dev), GFP_KERNEL);
	if (!mcuspi) {
		dev_err(&spid->dev, "mcuspi mem allocation failed!\n");
		return -ENOMEM;
	}
	/* Store pointer to the device-structure in bus device context */
	spi_set_drvdata(spid, mcuspi);

	/* Store pointer to SPI device/client */
	mcuspi->device = spid;

	/* Initialize the misc device, mcuspi incremented after each probe call */
	sprintf(mcuspi->name, "mcuspi%01d", counter++); 
	dev_info(&spid->dev, 
		 "mcu_spi_probe is entered on %s\n", mcuspi->name);

	mcuspi->mcu_spi_miscdevice.name = mcuspi->name;
	mcuspi->mcu_spi_miscdevice.minor = MISC_DYNAMIC_MINOR;
	mcuspi->mcu_spi_miscdevice.fops = &mcuspi_fops;

	/* Register misc device */
	ret =  misc_register(&mcuspi->mcu_spi_miscdevice);

	dev_info(&spid->dev, 
		 "mcu_spi_probe is exited on %s\n", mcuspi->name);

	return ret;
}



static int mcu_spi_remove(struct spi_device *spid)
{

	struct mcuspi_dev * mcuspi;
	/* Get device structure from bus device context */	
	mcuspi = spi_get_drvdata(spid);

	dev_info(&spid->dev, 
		 "mcu_spi_remove is entered on %s\n", mcuspi->name);

	/* Deregister misc device */
	misc_deregister(&mcuspi->mcu_spi_miscdevice);

	dev_info(&spid->dev, 
		 "mcu_spi_remove is exited on %s\n", mcuspi->name);

	return 0;

}

/* Declare a list of devices supported by the driver */
static const struct of_device_id mcu_spi_of_ids[] = {
	{ .compatible = "dozh,mcu-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, mcu_spi_of_ids);


static const struct spi_device_id mcu_spi_id[] = {
	{ "mcu_spi", 0 },
	{ "stm32f405rg", 1 },
	{}
};
MODULE_DEVICE_TABLE(spi, mcu_spi_id);



static struct spi_driver mcu_spi_driver = {
	.driver = {
		.name = "mcu_spi_if",
		.owner = THIS_MODULE,
		.of_match_table = mcu_spi_of_ids,
	},
	.probe =            mcu_spi_probe,
	.remove = 			mcu_spi_remove,
	.id_table =         mcu_spi_id,
};

module_spi_driver(mcu_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DoZh <TATQAQTAT@gmail.com>");
MODULE_DESCRIPTION("This is a test mcu interface module over spi");


