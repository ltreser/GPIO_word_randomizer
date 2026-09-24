#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define BUFFER_SIZE 4096

typedef struct s_word			t_word;

struct					s_word
{
	char				*word;
	struct list_head		list;
};

static char				*output;
static size_t				output_len;
static					DEFINE_MUTEX(buffer_lock);
static					LIST_HEAD(word_list);
static int				gpio_pin = -1;  // default gpio pin number when not specified
module_param(gpio_pin, int, 0444);
MODULE_PARM_DESC(gpio_pin, "GPIO pin number to watch via interrupt");
static int				irq_number;

static irqreturn_t	gpio_irq_handler(int irq, void *dev_id)
{
	int		i;
	int		count;
	t_word		*node;

	count = 0;
	mutex_lock(&buffer_lock);
	list_for_each_entry(node, &word_list, list)
	{
		count++;
	}
	if (count == 0)
	{
		mutex_unlock(&buffer_lock);
		return (IRQ_HANDLED);
	}
	i = get_random_u32() % count;
	list_for_each_entry(node, &word_list, list)
	{
		if (i == 0)
		{
			pr_info("Random word: %s\n", node->word);
			break ;
		}
		i--;
	}
	mutex_unlock(&buffer_lock);
	return (IRQ_HANDLED);
}

static ssize_t	module_write(struct file *file, const char __user *buf,
		size_t len, loff_t *off)
{
	char	*tmp;
	char	*token;
	char	*cursor;
	t_word	*node;

	if (!len || len >= BUFFER_SIZE)
		return (-EINVAL);
	tmp = kmalloc(len + 2, GFP_KERNEL);
	if (!tmp)
		return (-ENOMEM);
	if (copy_from_user(tmp, buf, len))
	{
		kfree(tmp);
		return (-EFAULT);
	}
	tmp[len] = '\0';
	mutex_lock(&buffer_lock);
	cursor = tmp;
	while ((token = strsep(&cursor, " \n\t")) != NULL)
	{
		if (*token == '\0')
			continue ;
		node = kmalloc(sizeof(t_word), GFP_KERNEL);
		if (!node)
			break ;
		node->word = kstrdup(token, GFP_KERNEL);
		if (!node->word)
		{
			kfree(node);
			break ;
		}
		list_add_tail(&node->list, &word_list);
	}
	mutex_unlock(&buffer_lock);
	kfree(tmp);
	return (len);
}

static void	build_output(void)
{
	t_word	*node;

	kfree(output);
	output = NULL;
	output_len = 0;
	list_for_each_entry(node, &word_list, list)
	{
		output_len += strlen(node->word) + 1;
	}
	if (output_len == 0)
		return ;
	output = kzalloc(output_len + 1, GFP_KERNEL); // sets memory to zero, slower than kmalloc
	if (!output)
		return ;
	list_for_each_entry(node, &word_list, list)
	{
		strcat(output, node->word);
		strcat(output, "\n");
	}
}

static ssize_t	module_read(struct file *file, char __user *buf, size_t len,
		loff_t *off)
{
	mutex_lock(&buffer_lock);
	if (*off == 0)
		build_output();
	if (!output || *off >= output_len)
	{
		mutex_unlock(&buffer_lock);
		return (0);
	}
	if (len > output_len - *off)
		len = output_len - *off;
	if (copy_to_user(buf, output + *off, len))
	{
		mutex_unlock(&buffer_lock);
		return (-EFAULT);
	}
	*off += len;
	mutex_unlock(&buffer_lock);
	return (len);
}

static const struct file_operations	module_fops = {
	.owner = THIS_MODULE,
	.read = module_read,
	.write = module_write,
};

static struct miscdevice			module_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "module_device",
	.fops = &module_fops,
};

static int __init	module_init_function(void)
{
	int	ret;

	if (gpio_pin < 0)
	{
		pr_err("Error: GPIO pin number not specified.\n");
		return (-EINVAL);
	}
	ret = misc_register(&module_misc_device);
	if (ret)
	{
		pr_err("Error: Failed to register misc device\n");
		return (ret);
	}
	ret = gpio_request(gpio_pin, "randomizer_gpio");
	if (ret)
	{
		pr_err("Error: Failed to request GPIO pin %d\n", gpio_pin);
		goto err_misc;
	}
	ret = gpio_direction_input(gpio_pin);
	if (ret)
	{
		pr_err("Error: Failed to set GPIO pin %d as input\n", gpio_pin);
		goto err_gpio;
	}
	irq_number = gpio_to_irq(gpio_pin);
	if (irq_number < 0)
	{
		pr_err("Error: Failed to get IRQ number for GPIO pin %d\n", gpio_pin);
		ret = irq_number;
		goto err_gpio;
	}
	ret = request_threaded_irq(irq_number, NULL, gpio_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT, "randomizer_gpio_irq", NULL);
	if (ret)
	{
		pr_err("Error: Failed to request IRQ %d for GPIO pin %d\n", irq_number,
			gpio_pin);
		goto err_gpio;
	}
	pr_info("Module loaded\n");
	return (0);
err_gpio:
	gpio_free(gpio_pin);
err_misc:
	misc_deregister(&module_misc_device);
	return (ret);
}

static void __exit	module_exit_function(void)
{
	t_word *node, *tmp;
	free_irq(irq_number, NULL);
	gpio_free(gpio_pin);
	mutex_lock(&buffer_lock);
	list_for_each_entry_safe(node, tmp, &word_list, list)
	{
		list_del(&node->list);
		kfree(node->word);
		kfree(node);
	}
	kfree(output);
	mutex_unlock(&buffer_lock);
	misc_deregister(&module_misc_device);
	pr_info("Module unloaded\n");
}

module_init(module_init_function);
module_exit(module_exit_function);
MODULE_LICENSE("GPL");
