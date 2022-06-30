
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

#define SEND_SYSFS_DIR_NAME "send"
#define RECV_SYSFS_DIR_NAME "recv"

#define MAX_BUFFERED_MSG 1024 

#define BIN_ATTR(_name, _mode, _show, _store) \
struct bin_attribute  bin_attr_##_name = { \
	.attr = {.name = __stringify(_name),				\
		.mode = VERIFY_OCTAL_PERMISSIONS(_mode) },		\
	.read	= _show,						\
	.write	= _store,						\
}

/* This structure will represent single device */
struct mcuspi_dev {
	struct spi_device * spid;
	struct miscdevice mcu_spi_miscdevice;
	struct mcu_message_queue * recv_msg_queue; /* msg buff to store received msgs from ext interrupt*/
	struct mcu_message * send_msg;	/* store the send_msg being processed by userspace*/
	struct mcu_message * recv_msg;  /* store the recv_msg being processed by userspace*/
	struct kobject *send_subdir;
	struct kobject *recv_subdir;
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

void dev_dump_hex(const void* data, size_t size) {
	char ascii[17];
	size_t i, j;
	ascii[16] = '\0';
	for (i = 0; i < size; ++i) {
		printk( KERN_CONT  "%02X ", ((unsigned char*)data)[i]);
		if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
			ascii[i % 16] = ((unsigned char*)data)[i];
		} else {
			ascii[i % 16] = '.';
		}
		if ((i+1) % 8 == 0 || i+1 == size) {
			printk( KERN_CONT  " ");
			if ((i+1) % 16 == 0) {
				printk( KERN_CONT  "|  %s \n", ascii);
			} else if (i+1 == size) {
				ascii[(i+1) % 16] = '\0';
				if ((i+1) % 16 <= 8) {
					printk( KERN_CONT " ");
				}
				for (j = (i+1) % 16; j < 16; ++j) {
					printk( KERN_CONT "   ");
				}
				printk( KERN_CONT "|  %s \n", ascii);
			}
		}
	}
}

bool is_mcu_message_queue_full(mcu_message_queue *msg_queue) 
{
	return msg_queue->msg_count >= MAX_BUFFERED_MSG;
}

bool is_mcu_message_queue_empty(mcu_message_queue *msg_queue) 
{
	return msg_queue->msg_count <= 0;
}

int get_payload_len_in_next_mcu_msg(mcu_message_queue *msg_queue) 
{
	return msg_queue->mcu_msg[msg_queue->read_msg_idx]->payload_length;
}

int store_one_mcu_message_to_queue(mcu_message_queue *msg_queue, 
			uint16_t payload_length, uint8_t *payload_desc, uint8_t *payload)
{
	//TODO: rewrite it use mcu_message instead of seperated payload_xxx
	printk("store_one_mcu_message_to_queue\n");
	if (msg_queue->msg_count >= MAX_BUFFERED_MSG) {
		return -ENOSPC;
	}

	mcu_message * mcu_msg = kzalloc(sizeof(mcu_message), GFP_KERNEL);
	if (!mcu_msg) {
		return -ENOMEM;
	}
	
	memcpy(mcu_msg->payload_desc, payload_desc, PAYLOAD_DESC_LENGTH);
	mcu_msg->payload = NULL;
	mcu_msg->payload_length = payload_length;
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

int load_one_mcu_message_from_queue(mcu_message_queue *msg_queue, mcu_message *mcu_msg)
{
	if (msg_queue->msg_count <= 0) {
		return -EAGAIN;
	}

	//clean up memory of previous msg
	struct mcu_message *prev_mcu_msg = msg_queue->mcu_msg[msg_queue->read_msg_idx];
	if (prev_mcu_msg) {
		if (prev_mcu_msg->payload_length > 0 && prev_mcu_msg->payload) {
			//memcpy(payload, mcu_msg->payload, mcu_msg->payload_length);
			kfree (prev_mcu_msg->payload);
			prev_mcu_msg->payload = NULL;
		}
		//memcpy(payload_desc, mcu_msg->payload_desc, PAYLOAD_DESC_LENGTH);
		//*payload_length = mcu_msg->payload_length;
		kfree(prev_mcu_msg);
		msg_queue->mcu_msg[msg_queue->read_msg_idx] = NULL;
	}

	(msg_queue->read_msg_idx)++; //TODO: check if can add it at every load_msg step.
	if ((msg_queue->read_msg_idx) >= MAX_BUFFERED_MSG) {
		msg_queue->read_msg_idx = 0;
	}
	mcu_message * this_mcu_msg = msg_queue->mcu_msg[msg_queue->read_msg_idx];
	if (!this_mcu_msg) {
		return -EFAULT; 
	}
	memcpy(mcu_msg->payload_desc, this_mcu_msg->payload_desc, PAYLOAD_DESC_LENGTH);
	mcu_msg->payload_length = this_mcu_msg->payload_length;
	if (this_mcu_msg->payload_length > 0) {
		if (!this_mcu_msg->payload) {
			return -EFAULT;
		}
		memcpy(mcu_msg->payload, this_mcu_msg->payload, this_mcu_msg->payload_length);
		kfree (this_mcu_msg->payload);
		this_mcu_msg->payload = NULL;
	}
	kfree(this_mcu_msg);
	msg_queue->mcu_msg[msg_queue->read_msg_idx] = NULL;

	(msg_queue->msg_count)--;
	return 0;
}

int drop_one_mcu_message_from_queue(mcu_message_queue *msg_queue)
{
	if (msg_queue->msg_count <= 0) {
		return -EAGAIN;
	}

	//clean up memory of previous msg
	struct mcu_message *prev_mcu_msg = msg_queue->mcu_msg[msg_queue->read_msg_idx];
	if (prev_mcu_msg) {
		if (prev_mcu_msg->payload_length > 0 && prev_mcu_msg->payload) {
			//memcpy(payload, mcu_msg->payload, mcu_msg->payload_length);
			kfree (prev_mcu_msg->payload);
			prev_mcu_msg->payload = NULL;
		}
		//memcpy(payload_desc, mcu_msg->payload_desc, PAYLOAD_DESC_LENGTH);
		//*payload_length = mcu_msg->payload_length;
		kfree(prev_mcu_msg);
		msg_queue->mcu_msg[msg_queue->read_msg_idx] = NULL;
	}

	(msg_queue->read_msg_idx)++;
	if ((msg_queue->read_msg_idx) >= MAX_BUFFERED_MSG) {
		msg_queue->read_msg_idx = 0;
	}
	mcu_message * mcu_msg = msg_queue->mcu_msg[msg_queue->read_msg_idx];
	if (!mcu_msg) {
		return -EFAULT; 
	}
	if (mcu_msg->payload_length > 0) {
		if (!mcu_msg->payload) {
			return -EFAULT;
		}
		kfree (mcu_msg->payload);
		mcu_msg->payload = NULL;
	}
	kfree(mcu_msg);
	msg_queue->mcu_msg[msg_queue->read_msg_idx] = NULL;
	
	(msg_queue->msg_count)--;
	return 0;
}

int pack_one_mcu_message(mcu_message *mcu_msg, uint8_t *buf)
{
	
	uint16_t payload_length;
	static uint8_t serial_no = 0;
	uint32_t checksum;

	printk("pack_one_mcu_message\n");

	// pre_head 0xAA + serial no(1 Byte) + custom data descriptor(64 Bytes) + payload length(2 bytes, count by bytes) + payload(0~1024 Bytes) + CRC32
	
	buf[0] = 0xAA;
	buf[PREAMBLE_LENGTH] = serial_no++;


	// use little endian to store payload length 
	 *(uint16_t *)(buf + PAYLOAD_SHIFT - 2) = mcu_msg->payload_length; //check align!
	//buf[PAYLOAD_SHIFT - 2] = (uint8_t)(mcu_msg->payload_length & 0xFF);
	//buf[PAYLOAD_SHIFT - 1] = (uint8_t)((mcu_msg->payload_length >> 8) & 0xFF);

	//for test
	uint16_t datacount;
	uint8_t *dataptr = buf + PAYLOAD_SHIFT;
	uint8_t data = 0;
	for (datacount = 0 ; datacount < MAX_PAYLOAD_LENGTH; datacount++)
	{
			*dataptr++ = data++;
	}
	
	if (mcu_msg->payload_length > 0) {
		memcpy(buf + PAYLOAD_SHIFT, mcu_msg->payload, mcu_msg->payload_length);
	}
	
	checksum = ~crc32(0xFFFFFFFF, buf, HEAD_LENGTH + mcu_msg->payload_length);
	*(uint32_t *)(buf + PAYLOAD_SHIFT + mcu_msg->payload_length) = checksum; //check align!



	return 0;
}

int init_mcu_message_queue(mcu_message_queue **msg_queue)
{
	*msg_queue = kzalloc(sizeof(mcu_message_queue), GFP_KERNEL);
	if (!msg_queue) {
		return -ENOMEM;
	}
	(*msg_queue)->read_msg_idx = MAX_BUFFERED_MSG - 1;
	(*msg_queue)->write_msg_idx = 0;
	(*msg_queue)->msg_count = 0;
	printk("init_mcu_message_queue:%08x\n", *msg_queue);
	return 0;
}

int deinit_mcu_message_queue(mcu_message_queue *msg_queue)
{
	if (!msg_queue) {
		return -EFAULT;
	}
	while (!is_mcu_message_queue_empty(msg_queue)) {
		drop_one_mcu_message_from_queue(msg_queue);
	}
	kfree(msg_queue);
	return 0;
}

int init_mcu_message(mcu_message **msg)
{
	*msg = kzalloc(sizeof(mcu_message), GFP_KERNEL);
	if (!msg) {
		return -ENOMEM;
	}

	(*msg)->payload = kzalloc(MAX_PAYLOAD_LENGTH, GFP_KERNEL);
	if (!(*msg)->payload) {
		kfree(msg);
		return -ENOMEM;
	}

	printk("init_mcu_message:%08x\n", *msg);
	return 0;
}

int deinit_mcu_message(mcu_message *msg)
{
	if (!msg) {
		return -EFAULT;
	}
	kfree(msg->payload);
	kfree(msg);
	return 0;
}


/* User is reading data from /dev/mcuspiX */
static ssize_t mcuspi_read_file(struct file *file, char __user *userbuf,
                               size_t count, loff_t *ppos)
{
	int expval, size;
	//char *recvbuf;
	struct mcuspi_dev * mcuspi;
	struct mcu_message * mcu_msg = NULL;
	struct mcu_message_queue * mcu_msg_queue = NULL;
	int offset = 0;
	int ret = 0;


	/* calc mcuspi addr by miscdevice addr. miscdevice addr fill into
	 * file->private_data by misc_open()*/
	mcuspi = container_of(file->private_data, 
			     struct mcuspi_dev, 
			     mcu_spi_miscdevice);
	mcu_msg = mcuspi->send_msg;
	mcu_msg_queue = mcuspi->recv_msg_queue;
	
	dev_info(&mcuspi->spid->dev, 
		 "mcuspi_read_file entered on %s\n", mcuspi->name);

	if (!mcu_msg || !mcu_msg_queue) {
		dev_info(&mcuspi->spid->dev, 
		    "mcu_msg: %08x, mcu_msg_queue: %08x\n", mcu_msg, mcu_msg_queue);
		return -EFAULT; 
	}

	ret |= load_one_mcu_message_from_queue(mcu_msg_queue, mcu_msg);
	if (ret < 0) {
		dev_info(&mcuspi->spid->dev, 
			"load_one_mcu_message_from_queue Failed with %d\n", ret);
		return -EFAULT;
	}
	//recvbuf = kzalloc(MAX_PAYLOAD_LENGTH, GFP_KERNEL);
	
	count = min(mcu_msg->payload_length, count);
	offset = min(*ppos, mcu_msg->payload_length - count);
	offset = max(offset, 0);

	
	dev_info(&mcuspi->spid->dev, 
		 "count:%d, offset: %d, buf_ptr: %08x\n", count, offset, mcu_msg->payload + offset);

	if(copy_to_user(userbuf, mcu_msg->payload + offset, count)) {
		pr_info("Failed to copy payload content to user space\n");
		return -EFAULT;
	}

	dev_info(&mcuspi->spid->dev, 
		 "mcuspi_read_file exited on %s\n", mcuspi->name);


	//kfree(recvbuf);

	return count;
}



/* Writing from the terminal command line, \n is added */
static ssize_t mcuspi_write_file(struct file *file, const char __user *userbuf,
                                   size_t count, loff_t *ppos)
{
	int ret = 0;
	int offset = 0;
	struct mcuspi_dev * mcuspi;
	struct mcu_message * mcu_msg;
	uint8_t * sendbuf = NULL;

	mcuspi = container_of(file->private_data,
			     struct mcuspi_dev, 
			     mcu_spi_miscdevice);

	dev_info(&mcuspi->spid->dev, 
		 "mcuspi_write_file entered on %s\n", mcuspi->name);

	dev_info(&mcuspi->spid->dev,
		 "we have written %zu characters to file\n", count); 

	mcu_msg = mcuspi->send_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	mcu_msg->payload_length = max(count, 0);
	mcu_msg->payload_length = min(count, MAX_PAYLOAD_LENGTH);
	offset = min(*ppos, MAX_PAYLOAD_LENGTH - mcu_msg->payload_length);
	offset = max(offset, 0);
	if (mcu_msg->payload_length > 0) {
		if(copy_from_user(mcu_msg->payload, userbuf, mcu_msg->payload_length)) {
			dev_err(&mcuspi->spid->dev, "Bad copied value\n");
			return -EFAULT;
		}
	}

	sendbuf = kzalloc(MAX_PACKET_LENGTH, GFP_KERNEL);
	if (!sendbuf) {
		return -ENOMEM; 
	}
	pack_one_mcu_message(mcu_msg, sendbuf);
	
	ret |= spi_write(mcuspi->spid, sendbuf, MAX_PACKET_LENGTH); //it use a fixed length(MAX_PACKET_LENGTH) in PHY.

	kfree(sendbuf);

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
	uint8_t *buf;
	int status = 0;
	int payload_length = 0;
	uint32_t checksum;

	dev_info(&mcuspi->spid->dev, "interrupt received. device: %s\n", mcuspi->name);
	buf = kzalloc(MAX_PACKET_LENGTH, GFP_KERNEL);
	if (!buf) {
		dev_info(&mcuspi->spid->dev, "allocate memory fail in isr. device: %s\n", mcuspi->name);
		return IRQ_HANDLED;
	}
	status = spi_read(mcuspi->spid, buf, MAX_PACKET_LENGTH); 
	if (status) {
		dev_info(&mcuspi->spid->dev, "spi read fail in isr. device: %s\n", mcuspi->name);
		kfree(buf);
		return IRQ_HANDLED;
	}
	
	
	dev_dump_hex(buf, MAX_PACKET_LENGTH);
	//TODO: write a unpack func.
	payload_length = *(uint16_t *)(buf + PAYLOAD_SHIFT - 2);
	payload_length = max(payload_length, 0);
	payload_length = min(payload_length, MAX_PAYLOAD_LENGTH);
	checksum = ~crc32(0xFFFFFFFF, buf, HEAD_LENGTH + payload_length);
	dev_info(&mcuspi->spid->dev, "crc32 checksum = %08x, msg_checksum = %08x\n", checksum, *(uint32_t *)(buf + PAYLOAD_SHIFT + payload_length));
	if (checksum != *(uint32_t *)(buf + PAYLOAD_SHIFT + payload_length)) {
		dev_info(&mcuspi->spid->dev, "crc32 checksum mismatch in isr. device: %s\nchecksum = %08x, msg_checksum = %08x\n", 
					mcuspi->name, checksum, *(uint32_t *)(buf + PAYLOAD_SHIFT + payload_length));
		kfree(buf);
		return IRQ_HANDLED;
	}
	
	dev_info(&mcuspi->spid->dev,
		 "store_one_mcu_message_to_queue, payload_length:%d, PREAMBLE_LENGTH + SERIAL_NO_LENGTH:%d, PAYLOAD_SHIFT:%d\n", 
		 		payload_length, PREAMBLE_LENGTH + SERIAL_NO_LENGTH, PAYLOAD_SHIFT);

	dev_info(&mcuspi->spid->dev,
		 "store_one_mcu_message_to_queue, buf:%ld, payload_desc:%ld, payload:%ld\n", 
		 		buf, buf + PREAMBLE_LENGTH + SERIAL_NO_LENGTH, buf + PAYLOAD_SHIFT);

	printk("mcuspi->recv_msg_queue:%p\n", mcuspi->recv_msg_queue); 
	status = store_one_mcu_message_to_queue(mcuspi->recv_msg_queue, 
			payload_length, buf + PREAMBLE_LENGTH + SERIAL_NO_LENGTH, buf + PAYLOAD_SHIFT);
	kfree(buf);

	if (status) {
		dev_info(&mcuspi->spid->dev, "store msg fail in isr. errno:%d device: %s\n", status, mcuspi->name);
	}
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

static ssize_t recv_payload_show(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{	
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message * mcu_msg;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	mcu_msg = mcuspi->recv_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	count = min(count, mcu_msg->payload_length);
	off = min(off, mcu_msg->payload_length - count);
	if (count > 0) {
		memcpy(buf, mcu_msg->payload + off, count);
	}
	return count;
}
static BIN_ATTR(recv_payload, S_IRUGO, recv_payload_show, NULL);


static ssize_t recv_payload_len_show(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message * mcu_msg;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	mcu_msg = mcuspi->recv_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	dev_info(&mcuspi->spid->dev,
		 "recv_payload_len_show, spid:%p, mcuspi:%p, payload_len:%d\n", 
	 		spid, mcuspi, mcu_msg->payload_length);
	count = min(count, sizeof(mcu_msg->payload_length));
	off = min(off, sizeof(mcu_msg->payload_length) - count);
	memcpy(buf, &(mcu_msg->payload_length) + off, count);
	return count;
}
static BIN_ATTR(recv_payload_len, S_IRUGO, recv_payload_len_show, NULL);

static ssize_t recv_payload_desc_show(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message * mcu_msg;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	mcu_msg = mcuspi->recv_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	count = min(count, PAYLOAD_DESC_LENGTH);
	off = min(off, PAYLOAD_DESC_LENGTH - count);
	memcpy(buf, mcu_msg->payload_desc + off, count);
	return count;
}
static BIN_ATTR(recv_payload_desc, S_IRUGO, recv_payload_desc_show, NULL);

static ssize_t recv_remain_msg_count_show(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message_queue * msg_queue;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	msg_queue = mcuspi->recv_msg_queue;

	dev_info(&mcuspi->spid->dev,
		 "recv_remain_msg_count_show, spid:%p, mcuspi:%p, msg_queue:%p, msg_cnt:%d\n", 
	 		spid, mcuspi, msg_queue, msg_queue->msg_count);
	count = min(count, sizeof(msg_queue->msg_count));
	off = min(off, sizeof(msg_queue->msg_count) - count);
	memcpy(buf, &(msg_queue->msg_count) + off, count);
	return count;
}
static BIN_ATTR(remain_msg_count, S_IRUGO, recv_remain_msg_count_show, NULL);

static ssize_t recv_get_msg_store(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message_queue * msg_queue;
	//struct mcu_message * prev_mcu_msg;
	struct mcu_message * mcu_msg;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	msg_queue = mcuspi->recv_msg_queue;
	mcu_msg = mcuspi->recv_msg;

	load_one_mcu_message_from_queue(msg_queue, mcu_msg);

	return count;
}
static BIN_ATTR(get_msg, S_IWUSR|S_IWGRP, NULL, recv_get_msg_store);

static struct bin_attribute *recv_msg_attributes[] = {
	&bin_attr_recv_payload,
	&bin_attr_recv_payload_len,
	&bin_attr_recv_payload_desc,
	&bin_attr_remain_msg_count,
	&bin_attr_get_msg,
	NULL
};

static const struct attribute_group recv_msg_attr_group = {
	.bin_attrs = recv_msg_attributes,
	.name = RECV_SYSFS_DIR_NAME,
};


static ssize_t send_payload_store(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{	
	
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message * mcu_msg;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	mcu_msg = mcuspi->send_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	mcu_msg->payload_length = (off == 0 ? count : mcu_msg->payload_length + count);
	mcu_msg->payload_length = max(mcu_msg->payload_length, 0);
	mcu_msg->payload_length  = min(mcu_msg->payload_length , MAX_PAYLOAD_LENGTH);
	off = min(off, mcu_msg->payload_length - count);

	if (mcu_msg->payload_length > 0) {
		memcpy(mcu_msg->payload + off, buf, mcu_msg->payload_length);
	}
	
	return mcu_msg->payload_length;
}
static BIN_ATTR(send_payload, S_IWUSR|S_IWGRP, NULL, send_payload_store);


static ssize_t send_payload_len_store(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message * mcu_msg;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	mcu_msg = mcuspi->send_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	if (off == 0) {
		mcu_msg->payload_length = *(uint32_t *)count;
		mcu_msg->payload_length = max(mcu_msg->payload_length, 0);
		mcu_msg->payload_length  = min(mcu_msg->payload_length , MAX_PAYLOAD_LENGTH);
	}
	
	return sizeof(mcu_msg->payload_length);
}
static BIN_ATTR(send_payload_len, S_IWUSR|S_IWGRP, NULL, send_payload_len_store);

static ssize_t send_payload_desc_store(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message * mcu_msg;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	mcu_msg = mcuspi->send_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	count = min(count, PAYLOAD_DESC_LENGTH);
	off = min(off, PAYLOAD_DESC_LENGTH - count);
	memcpy(mcu_msg->payload_desc + off, buf, count);
	return count;
}
static BIN_ATTR(send_payload_desc, S_IWUSR|S_IWGRP, NULL, send_payload_desc_store);

static ssize_t send_put_msg_store(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct mcuspi_dev * mcuspi;
	struct spi_device * spid;
	struct mcu_message * mcu_msg;
	uint8_t * sendbuf = NULL;
	int ret = 0;

	spid = to_spi_device(kobj_to_dev(kobj->parent));
	mcuspi = spi_get_drvdata(spid);
	mcu_msg = mcuspi->send_msg;
	if (!mcu_msg) {
		return -EFAULT; 
	}
	sendbuf = kzalloc(MAX_PACKET_LENGTH, GFP_KERNEL);
	if (!sendbuf) {
		return -ENOMEM; 
	}
	pack_one_mcu_message(mcu_msg, sendbuf);
	
	ret |= spi_write(mcuspi->spid, sendbuf, MAX_PACKET_LENGTH); //it use a fixed length(MAX_PACKET_LENGTH) in PHY.

	kfree(sendbuf);

	if (ret) {
		return -EFAULT;
	} else {
		return count;
	}
}
static BIN_ATTR(put_msg, S_IWUSR|S_IWGRP, NULL, send_put_msg_store);


static struct bin_attribute *send_msg_attributes[] = {
	&bin_attr_send_payload,
	&bin_attr_send_payload_len,
	&bin_attr_send_payload_desc,
	&bin_attr_put_msg,
	NULL
};

static const struct attribute_group send_msg_attr_group = {
	.bin_attrs = send_msg_attributes,
	.name = SEND_SYSFS_DIR_NAME,
};

static const struct attribute_group *msg_attr_groups[] = {
	&recv_msg_attr_group,
	&send_msg_attr_group,
	NULL,
};

/* declare a file_operations structure */
static const struct file_operations mcuspi_fops = {
	.owner = THIS_MODULE,
	.read = mcuspi_read_file,
	.write = mcuspi_write_file,
};

static int mcu_spi_init_sysfs(struct spi_device *spid) 
{
	int ret = 0;
	int i = 0;
	struct mcuspi_dev * mcuspi = spi_get_drvdata(spid);
	
	mcuspi->send_subdir = kobject_create_and_add(send_msg_attr_group.name,
                                             &spid->dev.kobj);
	if (!mcuspi->send_subdir) {
		dev_err(&spid->dev, "create send_subdir failed\n");
		return -EFAULT;
	}
	for (i = 0; send_msg_attributes[i]; i++) {
		ret |= sysfs_create_bin_file(mcuspi->send_subdir, send_msg_attributes[i]);
	}
	mcuspi->recv_subdir = kobject_create_and_add(recv_msg_attr_group.name,
                                             &spid->dev.kobj);
	if (!mcuspi->recv_subdir) {
		dev_err(&spid->dev, "create recv_subdir failed\n");
		return -EFAULT;
	}
	for (i = 0; recv_msg_attributes[i]; i++) {
		ret |= sysfs_create_bin_file(mcuspi->recv_subdir, recv_msg_attributes[i]);
	}
	return ret;
}


static int mcu_spi_deinit_sysfs(struct spi_device *spid) 
{
	int i = 0;
	int ret = 0;
	struct mcuspi_dev * mcuspi = spi_get_drvdata(spid);
	
	
	/* Find the kobj from the path and parent kset */
	//seems kset_find_obj cannot be used with this scenario.
	//struct kobject *send_subdir = kset_find_obj(spid->dev.kobj.kset, send_msg_attr_group.name);
	//struct kobject *recv_subdir = kset_find_obj(spid->dev.kobj.kset, recv_msg_attr_group.name);

	/* check kobj is not null etc. */
	if (mcuspi->send_subdir) {
		for (i = 0; send_msg_attributes[i]; i++) {
			sysfs_remove_bin_file(mcuspi->send_subdir, send_msg_attributes[i]);
		}
		/* Remove the sysfs entry */
		kobject_put(mcuspi->send_subdir);
	} else {
		dev_err(&spid->dev, "mcuspi send_subdir remove failed!\n");
		ret |= -EFAULT;
	}
	if (mcuspi->recv_subdir) {
		for (i = 0; send_msg_attributes[i]; i++) {
			sysfs_remove_bin_file(mcuspi->recv_subdir, recv_msg_attributes[i]);
		}
		kobject_put(mcuspi->recv_subdir);
	} else {
		dev_err(&spid->dev, "mcuspi recv_subdir remove failed!\n");
		ret |= -EFAULT;
	}
	return ret;
}

static int mcu_spi_probe(struct spi_device *spid)
{
	int ret = 0;
	int err = 0;

	static int counter = 0;

	struct mcuspi_dev * mcuspi;

	struct gpio_desc *interrupt_gpio;
	int irq_no;

	pr_info("mcu_spi probe\n");

	/* Allocate new structure representing device */
	mcuspi = devm_kzalloc(&spid->dev, sizeof(struct mcuspi_dev), GFP_KERNEL); //should free automatic.
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


	/* Register sysfs hooks */
	//ret |= sysfs_create_groups(&spid->dev.kobj, msg_attr_groups);
	mcu_spi_init_sysfs(spid);
	

	
	dev_info(&spid->dev, "spid->dev.kobj: %s", spid->dev.kobj.name);
	//sysfs file may under /sys/class/spi_master/spix/spix.y/ZZZ

	/* Register misc device */
	ret |=  misc_register(&mcuspi->mcu_spi_miscdevice);

	ret |= init_mcu_message_queue(&mcuspi->recv_msg_queue);
	ret |= init_mcu_message(&mcuspi->send_msg);
	ret |= init_mcu_message(&mcuspi->recv_msg);
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

	deinit_mcu_message_queue(mcuspi->recv_msg_queue);
	deinit_mcu_message(mcuspi->send_msg);
	deinit_mcu_message(mcuspi->recv_msg);
	/* Deregister misc device */
	misc_deregister(&mcuspi->mcu_spi_miscdevice);

	/* Deregister sysfs hooks */
	//sysfs_remove_groups(&spid->dev.kobj, msg_attr_groups);
	mcu_spi_deinit_sysfs(spid);

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


