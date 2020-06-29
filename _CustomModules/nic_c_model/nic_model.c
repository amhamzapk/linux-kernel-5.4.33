/* Include Libraries*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/skbuff.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ameer Hamza");
MODULE_DESCRIPTION("A simple Linux driver for the BBB.");
MODULE_VERSION("0.1");
wait_queue_head_t         wait_queue[8];
static struct semaphore   wait_sem[8];
#define CASE_WAIT_IDLE			'a'
#define CASE_WAIT_EXIT   		'b'
#define CASE_NOTIFY_STACK_TX   	'c'
#define CASE_NOTIFY_STACK_RX   	'd'
char flag_queue[8] = {CASE_WAIT_IDLE};
int global_vals[8] = {0, 1, 2, 3, 4, 5, 6, 7};

static DEFINE_MUTEX(push_lock);
static DEFINE_MUTEX(pop_lock);
static DEFINE_MUTEX(main_lock);
static DEFINE_MUTEX(temp_lock);
volatile u64 element_idx = 0;
/* Commands */
#define STATE_IN_POLLING    1
#define STATE_END_POLLED	2
#define PROCESS_RX 			1
#define PROCESS_TX 			2

/* Global data types */
static struct task_struct *thread_st_nic;
static struct task_struct *thread_per_cpu[8];
static int thread_fn(void *unused);
static struct list_head   *head;

/* Meta data for NIC-C Model */
struct meta_skbuff {
	u32 command;
	volatile u8 cpu;
};

/* Main Structure for NIC-C Model */
struct skbuff_nic_c {
	u8 *skbuff;
	u32 len;
	struct meta_skbuff meta;
};

/* Queue to keep track of NIC-C Commands & SKBUFFS */
struct queue_ll{
     struct list_head list;
     struct skbuff_nic_c *skbuff_struct;
//     u64 idx;//Later use to do memcpy in push()
};

/* 
*	Get CPU Cycles from Read RDTSC Function
*/ 
static inline u64 read_rdtsc(void)
{
    u64 lo, hi;

    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) :: "ecx" );

    return (u64) ((hi << 32) | lo);
}
/* 
*	Pop last element from the queue
*	Return-> 0  if found
*	Return-> -1 if empty queue
*	Element will be get by reference
*/ 
static int pop_queue(struct queue_ll *temp_node){
	
	int ret_status = -1;
	struct queue_ll *temp_node_orig;

	/* Check if there is something in the queue */
	if(list_empty(head)) {
		return ret_status;
	}
	else {
    	mutex_lock(&pop_lock);
    	--element_idx;
		temp_node_orig = list_first_entry(head,struct queue_ll ,list);
    	mutex_unlock(&pop_lock);
		memcpy(temp_node, temp_node_orig, sizeof(struct queue_ll));
		ret_status = 0;
	}

	list_del(&temp_node_orig->list);
	kfree(temp_node_orig);
	return ret_status;
}

/* 
*	Push element in queue head
*	Element will be passed by reference
*/ 
void push_queue(struct skbuff_nic_c *skbuff_struct){
	// TODO: Protect by using mutex
	static struct queue_ll *temp_node;

	temp_node=kmalloc(sizeof(struct queue_ll),GFP_KERNEL);

	temp_node->skbuff_struct = skbuff_struct;

	mutex_lock(&push_lock);
	++element_idx;
	list_add_tail(&temp_node->list,head);
	mutex_unlock(&push_lock);
}

/*
*	Main NIC-C Model Thread
*	This thread will schedule process request 
*	as soon some element push into the queue
*/
static int thread_fn(void *unused)
{
    struct skbuff_nic_c *skbuff_ptr;

	/* Local variables to keep track of CPU Cycles */
    u64 clk_cycles_start = 0;
    u64 clk_cycles_end = 0;
    u64 clk_cycles_exp = 0;
    u64 clk_cycles_div = 500;

	printk(KERN_ALERT "Thread Enter - get_cpu() = %d\n", get_cpu());

	/* Run until module is not unloaded */
    while (!kthread_should_stop()) {

		/* Keep track of RDTSC */
    	if (!clk_cycles_start)
    		clk_cycles_start = read_rdtsc();

        clk_cycles_end = read_rdtsc ();

        clk_cycles_exp = (clk_cycles_end - clk_cycles_start);

		/* Check if queue needs to be processed */
        if ((clk_cycles_exp/clk_cycles_div) >= 1) {
        	static struct queue_ll temp_node;

			/* Check if some command is in queue */
        	if (pop_queue(&temp_node) != -1) {

				/* Parse skbuff data*/
        		skbuff_ptr = temp_node.skbuff_struct;

				down(&wait_sem[skbuff_ptr->meta.cpu]);

				switch (skbuff_ptr->meta.command)
				{

					case PROCESS_RX:
					{
						/* Parse the thread data */
						printk(KERN_ALERT "RX Command | Len = %d | CPU = %d\n", skbuff_ptr->len, skbuff_ptr->meta.cpu);

						flag_queue[skbuff_ptr->meta.cpu] = CASE_NOTIFY_STACK_RX;

						// Send Command
						wake_up_interruptible(&wait_queue[skbuff_ptr->meta.cpu]);


						break;
					}
					case PROCESS_TX:
					{
						/* Parse the thread data */
						printk(KERN_ALERT "TX Command | Len = %d | CPU = %d\n", skbuff_ptr->len, skbuff_ptr->meta.cpu);

						flag_queue[skbuff_ptr->meta.cpu] = CASE_NOTIFY_STACK_TX;

						// Send Command
						wake_up_interruptible(&wait_queue[skbuff_ptr->meta.cpu]);

						break;
					}
				}

				// TODO: Implement IPI
				// skbuff_ptr->meta.cpu = IPI;
        	}
			clk_cycles_start = 0;
        }
    	else
    	{
			/* This is necessary as we have to unschedule this
			   thread after some rdtsc for a very short amount
			   of time, for the sake of load balancing. Otherwise
			   we get system gets stuck if a core continously
			   spend its cycle in a while loop */
    		schedule_timeout (0); // Sleep for 500 clock cycles
    	}
    }

    printk(KERN_ALERT "thread_fn Exit!!!\n");

    return 0;
}
/*
*	Main NIC-C Model Thread
*	This thread will schedule process request
*	as soon some element push into the queue
*/
int temp = 0;
static int thread_per_cpu_fn(void *unused)
{
	int cpu = get_cpu();
	u8 is_exit = 0;
	char flag_alias;

	printk("Get CPU = %d", get_cpu());
	while (1)
	{	
		if (temp == 0)
		{
			temp = 1;
		}
		else
		{
			up (&wait_sem[cpu]);
		}
		// Get the command from the queue
	    wait_event_interruptible(wait_queue[cpu], flag_queue[cpu] != CASE_WAIT_IDLE);

//		mutex_lock(&temp_lock);
	    // printk(KERN_ALERT "First - %d\n", cpu);
	    flag_alias = flag_queue[cpu];
//	    printk(KERN_ALERT "Second - %d\n", cpu);
	    flag_queue[cpu] = CASE_WAIT_IDLE;
//	    printk(KERN_ALERT "Third - %d\n", cpu);


//	    printk(KERN_ALERT "Fourth - %d\n", cpu);
	    switch (flag_alias)
	    {
			case CASE_NOTIFY_STACK_RX:
			{
				printk(KERN_ALERT "Notify Stack RX | CPU - %d\n", cpu);
				ssleep(1);
//			    printk(KERN_ALERT "Fifth - %d\n", cpu);
				break;
			}
			case CASE_NOTIFY_STACK_TX:
			{
				printk(KERN_ALERT "Notify Stack TX | CPU - %d\n", cpu);
				ssleep(1);
//			    printk(KERN_ALERT "Sixth - %d\n", cpu);
				break;
			}
			case CASE_WAIT_EXIT:
			{
				is_exit = 1;
				break;
			}
//		    printk(KERN_ALERT "Seventh - %d\n", cpu);
	    }

//	    printk(KERN_ALERT "Eighth - %d\n", cpu);
//		mutex_unlock(&temp_lock);

	    if (is_exit)
	    	break;
	}

	printk("Thread-%d exitting...\n", get_cpu());
    return 0;
}

struct skbuff_nic_c skbuff_struc[2];
int i = 0;
static int __init nic_c_init(void) {

	/* Initilize Queue */
	printk(KERN_INFO "NIC-C Model Init!\n");
	head=kmalloc(sizeof(struct list_head *),GFP_KERNEL);
	INIT_LIST_HEAD(head);

	// Create and bind and execute thread to core-2
	thread_st_nic = kthread_create(thread_fn, NULL, "kthread");

	kthread_bind(thread_st_nic, 2);
	wake_up_process(thread_st_nic);

	for (i=0; i<8; i++)
	{
		init_waitqueue_head(&wait_queue[i]);
		thread_per_cpu[i] = kthread_create(thread_per_cpu_fn, NULL, "kthread_cpu");
		kthread_bind(thread_per_cpu[i], i);
		wake_up_process(thread_per_cpu[i]);
		sema_init(&wait_sem[i], 0);
	}

	/* Wait for a second to let the thread being schedule */
	ssleep(1);

//	for (i=0; i<8; i++)
//	{
//		flag_queue[i] = CASE_NOTIFY_STACK;
//		// Send Command
//		wake_up_interruptible(&wait_queue[i]);
//	}

	/* Push Dummy RX Command */
	for (i=0; i<2; i++)
	{
		skbuff_struc[i].skbuff = (u8*) kmalloc(sizeof(u8) * 128,GFP_KERNEL);
		skbuff_struc[i].len = 100 + (i * 10);
		skbuff_struc[i].meta.cpu = get_cpu();
		if ((i % 2) == 0)
			skbuff_struc[i].meta.command = PROCESS_RX;
		else
			skbuff_struc[i].meta.command = PROCESS_TX;
		push_queue(&skbuff_struc[i]);
	}
	printk(KERN_INFO "NIC-C Model Init Ends | CPU = %d!\n", num_online_cpus());

	return 0;
}

static void __exit nic_c_exit(void) {

   struct queue_ll *temp1, *temp2;
   int count = 0;

   kthread_stop(thread_st_nic);

   list_for_each_entry_safe(temp1, temp2, head, list) {
	   printk(KERN_INFO "Node %d data = %d\n" , count++, temp1->skbuff_struct->len);

			list_del(&temp1->list);list_first_entry(head,struct queue_ll ,list);
			kfree(temp1);
   }

	for (i=0; i<8; i++)
	{
		flag_queue[i] = CASE_WAIT_EXIT;
		// Send Command
		wake_up_interruptible(&wait_queue[i]);

//	    down (&wait_sem[i]);
	}

	ssleep (2);
   printk(KERN_INFO "NIC-C Model Exit!\n");
}

module_init(nic_c_init);
module_exit(nic_c_exit);
