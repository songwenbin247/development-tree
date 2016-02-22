#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/seq_file.h>
struct proc_dir_entry *proc_microDB;
struct proc_dir_entry *proc_list;
struct proc_dir_entry *proc_rbtree;
struct proc_dir_entry *proc_hash;

struct proc_dir_entry *proc_add;
struct proc_dir_entry *proc_del;
struct proc_dir_entry *proc_info;

LIST_HEAD(head_file);

struct file_type
{
	const char *type;
	char name[NAME_MAX];
	struct list_head list;
};

const char type_list[][10] = {{"list"}, {"rbtree"}, {"hash"}, {"none"}};

static int file_name_parse (char *file_name)
{
	char *index;
	int i;
	index = strrchr(file_name, '.');
	printk("--->%s:%s:%d\n",__FILE__,__func__,__LINE__);
	if (!index)
		goto file_error;
	
	*index = '\0';
	index++;
	for (i = 0; i < sizeof(type_list)/sizeof(*type_list); ++i){
	printk("--->%s:%s:%d\n",__FILE__,__func__,__LINE__);
		if(!strncmp(type_list[i],index, strlen(type_list[i])))
			break;
	}
	
	if (i >= sizeof(type_list) / sizeof(*type_list) - 1)
		goto file_error;
	
	return i;

file_error:
	printk(KERN_ERR "File name error, must end of ");
	for (i = 0; i < sizeof(type_list)/sizeof(*type_list) - 1; ++i){
		printk(KERN_ERR ".%s ",type_list[i]);
	}
	printk(KERN_ERR "\n");
	return sizeof(type_list)/sizeof(*type_list) - 1;
}

static int add_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t add_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos)
{
	char file_name[NAME_MAX];       			// = file->private_data;
	int type;
	struct file_type *entry;

	if (unlikely(size > NAME_MAX))
		size = NAME_MAX;
	
	if (copy_from_user(file_name,buf,size)){
		return -EFAULT;
	}

	type = file_name_parse(file_name);
	entry = kmalloc(sizeof(struct file_type), GFP_KERNEL);
	if (!entry){
		return -ENOMEM;
	}

	strcpy(entry->name,file_name);
	entry->type = type_list[type];
	list_add(&entry->list, &head_file);

	switch(type)
	{
		case 0:
			printk(KERN_INFO "Add a list file: %s\n",file_name);
			break;
		case 1:
			printk(KERN_INFO "Add a rbtree file: %s\n",file_name);
			break;			
		case 2:
			printk(KERN_INFO "Add a hashfile: %s\n",file_name);
			break;	
		default:
			printk(KERN_INFO "Add i = %d\n",type);
	}

	return size;
}

static int add_release(struct inode *inode, struct file *file)
{
	return 0;
}
static const struct file_operations proc_add_operations = {
	.owner = THIS_MODULE,
	.open = add_open,
	.write = add_write,
	.release = add_release,
};

static int del_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t del_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos)
{
	char file_name[NAME_MAX];       			// = file->private_data;
	int type;
	struct file_type *entry;
	if (unlikely(size > NAME_MAX))
		size = NAME_MAX;

	if (copy_from_user(file_name,buf,size))
		return -EFAULT;

	type = file_name_parse(file_name);
	list_for_each_entry(entry, &head_file, list){
		if (!strcmp(file_name,entry->name) && type_list[type] == entry->type){
			break;	
		}
	}
	if (&entry->list == &head_file){
		printk(KERN_INFO "file name error\n");
		return -EFAULT;
	}

	list_del(&entry->list); 
	kfree(entry);

	switch(type)
	{
		case 0:
			printk(KERN_INFO "Remove a list file: %s\n",file_name);
			break;
		case 1:
			printk(KERN_INFO "Remove a rbtree file: %s\n",file_name);
			break;
		case 2:
			printk(KERN_INFO "Remove a hash file: %s\n",file_name);
			break;
		default:
			printk(KERN_INFO "Remove a none file: %s\n",file_name);
	}
	return size;
}

static int del_release(struct inode *inode, struct file *file)
{
	return 0;
}
static const struct file_operations proc_remove_operations = {
	.owner = THIS_MODULE,
	.open = del_open,
	.write = del_write,
	.release = del_release,
};
static void *int_seq_read_start(struct seq_file *f, loff_t *pos)
{
	return  seq_list_start(&head_file, *pos);
}

static void *int_seq_read_next(struct seq_file *f, void *v, loff_t *pos)
{
	return seq_list_next(v, &head_file, pos);
}

static void int_seq_read_stop(struct seq_file *f, void *v)
{
	seq_printf(f, "---------END-------------\n");
	/* Nothing to do */
}

static int show_read(struct seq_file *f, void *v)
{
	seq_printf(f, "%s            %s\n",list_entry(v, struct file_type, list)->name, list_entry(v, struct file_type, list)->type);
	return 0;
}

static const struct seq_operations int_seq_read_ops = {
	.start = int_seq_read_start,
	.next  = int_seq_read_next,
	.stop  = int_seq_read_stop,
	.show  = show_read,
};

static int read_open(struct inode *inode, struct file *filp)
{
	printk("-->%s: %d\n",__FUNCTION__,__LINE__);
	return seq_open(filp, &int_seq_read_ops);
}

static const struct file_operations proc_read_operations = {
	.owner = THIS_MODULE,
	.open = read_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};


static int __init rbdev_init(void)
{
	proc_microDB = proc_mkdir("microDB", NULL);
	if (!proc_microDB){
		goto microDB_error;
	}
	proc_add = proc_create("add", S_IWUGO, proc_microDB, &proc_add_operations);
	proc_del = proc_create("remove", S_IWUGO, proc_microDB, &proc_remove_operations);
	proc_info = proc_create("info", S_IRUGO, proc_microDB, &proc_read_operations);
	
	proc_list = proc_mkdir("list", proc_microDB); 
	proc_rbtree = proc_mkdir("rbtree", proc_microDB);
	proc_hash = proc_mkdir("hash", proc_microDB);

	if (!(proc_list && proc_rbtree && proc_hash)){
		goto sub_dir_error;
	}

	return 0;
sub_dir_error:
	proc_remove(proc_microDB);
microDB_error:
	return -EFAULT;
} 

void __exit rbdev_exit(void)
{
	proc_remove(proc_microDB);
}

MODULE_DESCRIPTION("rbdev dummy driver");
MODULE_LICENSE("GPL");
module_init(rbdev_init);
module_exit(rbdev_exit);
