
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/crc32.h>


#define PREAMBLE_LENGTH 1
#define SERIAL_NO_LENGTH 1
#define PAYLOAD_DESC_LENGTH 64
#define PAYLOAD_COUNT_LENGTH 2
#define MAX_PAYLOAD_LENGTH 1024
#define VERIFY_LENGTH 4
#define HEAD_LENGTH (PREAMBLE_LENGTH + SERIAL_NO_LENGTH + PAYLOAD_DESC_LENGTH + PAYLOAD_COUNT_LENGTH) //68
#define PAYLOAD_SHIFT HEAD_LENGTH
#define MAX_PACKET_LENGTH (HEAD_LENGTH + MAX_PAYLOAD_LENGTH + VERIFY_LENGTH) //1096

#define MAX_BUFFERED_MSG 1024 

/* This structure will represent single device */
struct mcuspi_dev {
	struct spi_device * spid;
	struct miscdevice mcu_spi_miscdevice;
	struct mcu_message_queue * msg_queue;
	char name[8]; /* mcuspiX */
};

typedef struct mcu_message_queue {
	struct mcu_message * mcu_msg[MAX_BUFFERED_MSG];
	int16_t read_msg_idx;
	int16_t write_msg_idx;
	int16_t msg_count;
}mcu_message_queue;

/* This structure will save each message that received from MCU */
typedef struct mcu_message {
	uint16_t payload_length;
	uint8_t payload_desc[PAYLOAD_DESC_LENGTH]; 
	uint8_t * payload;
}mcu_message;

int init_mcu_message_queue(mcu_message_queue *msg_queue)
{
	msg_queue = kzalloc(sizeof(mcu_message_queue), GFP_KERNEL);
	if (!msg_queue) {
		return -ENOMEM;
	}
	msg_queue->read_msg_idx = 0;
	msg_queue->write_msg_idx = 0;
	msg_queue->msg_count = 0;
	return 0;
}

int store_one_mcu_message(mcu_message_queue *msg_queue, 
			uint16_t payload_length, uint8_t *payload_desc, uint8_t *payload)
{
	if (msg_queue->msg_count >= MAX_BUFFERED_MSG) {
		return -ENOSPC;
	}
	mcu_message * mcu_msg = kzalloc(sizeof(mcu_message), GFP_KERNEL);
	if (!mcu_msg) {
		return -ENOMEM;
	}
	memcpy(mcu_msg->payload_desc, payload_desc, PAYLOAD_DESC_LENGTH);
	mcu_msg->payload = NULL;
	if (payload_length > 0) {
		mcu_msg->payload = kzalloc(payload_length, GFP_KERNEL);
		if (!mcu_msg->payload) {
			kfree(mcu_msg);
			return -ENOMEM;
		}
		memcpy(mcu_msg->payload, payload, payload_length);
	}
	msg_queue->mcu_msg[msg_queue->write_msg_idx] = mcu_msg;
	(msg_queue->write_msg_idx)++;
	if ((msg_queue->write_msg_idx) >= MAX_BUFFERED_MSG) {
		msg_queue->write_msg_idx = 0;
	}
	(msg_queue->msg_count)++;
	return 0;
}

int load_one_mcu_message(mcu_message_queue *msg_queue,
			uint16_t *payload_length, uint8_t *payload_desc, uint8_t *payload)
{
	if (msg_queue->msg_count <= 0) {
		return -EAGAIN;
	}
	mcu_message * mcu_msg = msg_queue->mcu_msg[msg_queue->read_msg_idx];
	if (!mcu_msg) {
		return -EFAULT; 
	}
	memcpy(payload_desc, mcu_msg->payload_desc, PAYLOAD_DESC_LENGTH);
	*payload_length = mcu_msg->payload_length;
	if (mcu_msg->payload_length > 0) {
		if (!mcu_msg->payload) {
			return -EFAULT;
		}
		memcpy(payload, mcu_msg->payload, mcu_msg->payload_length);
		kfree (mcu_msg->payload);
	}
	kfree(mcu_msg);
	msg_queue->mcu_msg[msg_queue->read_msg_idx] = NULL;
	(msg_queue->read_msg_idx)++;
	if ((msg_queue->read_msg_idx) >= MAX_BUFFERED_MSG) {
		msg_queue->read_msg_idx = 0;
	}
	(msg_queue->msg_count)--;
	return 0;
}

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
	expval = spi_read(mcuspi->spid, buf, count);
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
	kfree(buf);

	return 0;
}

/* Writing from the terminal command line, \n is added */
static ssize_t mcuspi_write_file(struct file *file, const char __user *userbuf,
                                   size_t count, loff_t *ppos)
{
	int ret = 0;

	char *buf;
	struct mcuspi_dev * mcuspi;
	uint16_t datacount;
	static uint8_t serial_no = 0;
	uint32_t checksum;
	

	mcuspi = container_of(file->private_data,
			     struct mcuspi_dev, 
			     mcu_spi_miscdevice);

	dev_info(&mcuspi->spid->dev, 
		 "mcuspi_write_file entered on %s\n", mcuspi->name);

	dev_info(&mcuspi->spid->dev,
		 "we have written %zu characters to file\n", count); 



	// pre_head 0xAA + serial no(1 Byte) + custom data descriptor(64 Bytes) + payload length(2 bytes, count by bytes) + payload(0~1024 Bytes) + CRC32
	buf = kzalloc(MAX_PACKET_LENGTH, GFP_KERNEL);
	buf[0] = 0xAA;
	buf[PREAMBLE_LENGTH] = serial_no++;
	count = min(count, (size_t)MAX_PAYLOAD_LENGTH); // use little endian to save payload length 
	 *(uint16_t *)(buf + PAYLOAD_SHIFT - 2) = count; //check align!
	//buf[PAYLOAD_SHIFT - 2] = (uint8_t)(count & 0xFF);
	//buf[PAYLOAD_SHIFT - 1] = (uint8_t)((count >> 8) & 0xFF);

	//for test
	
	uint8_t *dataptr = buf + PAYLOAD_SHIFT;
	uint8_t data = 0;
	for (datacount = 0 ; datacount < 1024; datacount++)
	{
			*dataptr++ = data++;
	}
	
	
	if(copy_from_user(buf + PAYLOAD_SHIFT, userbuf, count)) {
		dev_err(&mcuspi->spid->dev, "Bad copied value\n");
		return -EFAULT;
	}
	
	checksum = crc32(0xFFFFFFFF, buf, HEAD_LENGTH + count);
	*(uint32_t *)(buf + PAYLOAD_SHIFT + count) = checksum; //check align!



	ret |= spi_write(mcuspi->spid, buf, MAX_PACKET_LENGTH); 

	kfree(buf);
	if (ret < 0)
		dev_err(&mcuspi->spid->dev, "the device is not found, ERRNO: %d\n", ret);
	else
		dev_info(&mcuspi->spid->dev, "we have written %zu characters to spi bus.\n", count); 

	dev_info(&mcuspi->spid->dev, 
		 "mcuspi_write_file exited on %s\n", mcuspi->name);

	return count;
}

static irqreturn_t mcu_spi_isr(int irq_no, void *data)
{
	int val;
	struct mcuspi_dev * mcuspi = data;
	dev_info(&mcuspi->spid->dev, "interrupt received. device: %s\n", mcuspi->name);
/*
	val = gpiod_get_value(priv->gpio);
	dev_info(priv->dev, "Button state: 0x%08X\n", val);

	if (val == 1)
		hello_keys_buf[buf_wr++] = 'P'; 
	else
		hello_keys_buf[buf_wr++] = 'R';

	if (buf_wr >= MAX_KEY_STATES)
		buf_wr = 0;

	// Wake up the process 
	wake_up_interruptible(&priv->wq_data_available);
*/
	return IRQ_HANDLED;
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
	int err = 0;

	static int counter = 0;

	struct mcuspi_dev * mcuspi;

	struct gpio_desc *interrupt_gpio;
	int irq_no;

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
	mcuspi->spid = spid;

	/* Initialize the misc device, mcuspi incremented after each probe call */
	sprintf(mcuspi->name, "mcuspi%01d", counter++); 
	dev_info(&spid->dev, 
		 "mcu_spi_probe is entered on %s\n", mcuspi->name);

	mcuspi->mcu_spi_miscdevice.name = mcuspi->name;
	mcuspi->mcu_spi_miscdevice.minor = MISC_DYNAMIC_MINOR;
	mcuspi->mcu_spi_miscdevice.fops = &mcuspi_fops;

    /* Get GPIO start with "int" in device tree */
	interrupt_gpio = devm_gpiod_get(&spid->dev, "int", GPIOD_IN);
	if (IS_ERR(interrupt_gpio)) {
		dev_err(&spid->dev, "gpio get index failed\n");
		err = PTR_ERR(interrupt_gpio); /* PTR_ERR return an int from a pointer */
		return ERR_PTR(err);
	}

	irq_no = gpiod_to_irq(interrupt_gpio);
	if (irq_no < 0) {
		dev_err(&spid->dev, "gpio get irq failed\n");
		err = irq_no;
		return ERR_PTR(err);
	}
	dev_info(&spid->dev, "The IRQ number is: %d\n", irq_no);

	/* Request threaded interrupt */
	err = devm_request_threaded_irq(&spid->dev, irq_no, NULL,
			mcu_spi_isr, IRQF_TRIGGER_FALLING | IRQF_ONESHOT, mcuspi->name, mcuspi);
	if (err)
		return ERR_PTR(err);


	/* Register misc device */
	ret =  misc_register(&mcuspi->mcu_spi_miscdevice);

	//test crc32
	uint32_t crc32_result = ~crc32(0xFFFFFFFF, "UUUUUUUUUUUUUUUU", 15);
	dev_info(&spid->dev, "The crc32 is: %x\n", crc32_result);

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


