#include <linux/fscrypt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
//#include <linux/moduleparam.h> 
//#include <linux/gpio.h> 
//#include <linux/interrupt.h>
//#include <linux/random.h>

#define BUFFER_SIZE 4096

typedef struct s_word t_word;

struct s_word 
{
    char *word;
    struct list_head list;
};

static char *buffer;
static size_t buffer_len;
static DEFINE_MUTEX(buffer_lock);
static LIST_HEAD(word_list);

static ssize_t module_write(struct file *file, const char __user *buf, size_t len, loff_t *off)
{
	char *tmp;
	char *token;
	char *cursor;

	if (!len || len >= BUFFER_SIZE)
		return -EINVAL;
	tmp = kmalloc(len + 2, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	if (copy_from_user(tmp, buf, len))
	{
		kfree(tmp);
		return -EFAULT;
	}
	tmp[len] = '\n';
	tmp[len + 1] = '\0';
	mutex_lock(&buffer_lock);
	kfree(buffer);
	buffer = tmp;
	buffer_len = len;
	cursor = buffer;
	while ((token = strsep(&cursor, " \n\t")) != NULL)
	{
		if (*token == '\0')
			continue;
		t_word *node = kmalloc(sizeof(t_word), GFP_KERNEL);
		if (!node)
			break;
		node->word = kstrdup(token, GFP_KERNEL);
		if (!node->word)
		{
			kfree(node);
			break;
		}
		list_add_tail(&node->list, &word_list);
	}
	mutex_unlock(&buffer_lock);
	return len;
}

static ssize_t module_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
	mutex_lock(&buffer_lock);
	if (!buffer || *off >= buffer_len)
	{
		mutex_unlock(&buffer_lock);
		return 0;
	}
	if (len > buffer_len - *off)
		len = buffer_len - *off;
	if (copy_to_user(buf, buffer + *off, len))
	{
		mutex_unlock(&buffer_lock);
		return -EFAULT;
	}
	*off += len;
	mutex_unlock(&buffer_lock);
	return len;
}

static const struct file_operations module_fops = {
	.owner = THIS_MODULE,
	.read = module_read,
	.write = module_write,
};

static struct miscdevice module_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "module_device",
	.fops = &module_fops,
};

static int __init module_init_function(void)
{
	int ret;
	ret = misc_register(&module_misc_device);
	if (ret)
	{
		pr_info("Failed to register misc device\n");
		return ret;
	}
	pr_info("Module loaded\n");
	return 0;
}

static void __exit module_exit_function(void)
{
	t_word *node, *tmp;
	mutex_lock(&buffer_lock);
	list_for_each_entry_safe(node, tmp, &word_list, list)
	{
		list_del(&node->list);
		kfree(node->word);
		kfree(node);
	}
	kfree(buffer);
	mutex_unlock(&buffer_lock);
	misc_deregister(&module_misc_device);
	pr_info("Module unloaded\n");
}

module_init(module_init_function);
module_exit(module_exit_function);
MODULE_LICENSE("GPL"); 
