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

MODULE_LICENSE		("GPL");
MODULE_AUTHOR		("Ameer Hamza");
MODULE_DESCRIPTION	("NIC-C Model Description");
MODULE_VERSION		("0.1");

/* Commands */
#define CASE_NOTIFY_STACK_TX   	123
#define CASE_NOTIFY_STACK_RX   	456
#define PROCESS_RX 				  1
#define PROCESS_TX 				  2

/* Request/Response Macros */
#define TYPE_REQUEST 	0
#define TYPE_RESPONSE	1

/* CPUs/Commands */
#define NUM_CPUS 	4
#define THOUSAND	1000
#define MILLION		THOUSAND*THOUSAND
#define NUM_CMDS	4096 //4*MILLION

/* Commands */
#define POLL_IF_RESPONSE_READ   0
#define POLL_END_RESPONSE_READ	1

char flag[NUM_CMDS] = {'n'};
u8 	 response_thread_exit = 0;
u64  global_skbuff_pass = 0xDEADBEEFBEEFDEAD;
int  cmd_send = 0;
int  cmd_rcv = 0;
int  response_total = 0;

static DEFINE_MUTEX(push_request_lock);
static DEFINE_MUTEX(pop_response_lock);
static DEFINE_MUTEX(driver_request_lock);

/* Global data types */
static wait_queue_head_t  my_wait_queue[NUM_CPUS];
static struct semaphore   wait_sem[NUM_CPUS];
static struct task_struct *thread_st_c_model_worker;
static struct task_struct *thread_st_response[NUM_CPUS];
static struct task_struct *thread_st_request[NUM_CPUS];
static struct list_head   head_request;
static struct list_head   head_response;

/* Meta data for NIC-C Model */
struct meta_skbuff {
	u32 command;
	u32 response_flag;
	volatile u8 cpu;
	volatile u8 poll_flag;
};

/* Main Structure for NIC-C Model */
struct skbuff_nic_c {
	u64 *skbuff;
	u32 len;
	struct meta_skbuff meta;
};

/* Queue to keep track of NIC-C Commands & SKBUFFS */
struct queue_ll{
     struct list_head list;
     struct skbuff_nic_c *skbuff_struct;
};

/* Buffer that driver will use */
/* This structure later will be passed by the net stack */
struct skbuff_nic_c skbuff_struct_driver[NUM_CPUS][NUM_CMDS];

//TODO: Find some creative method of allocation
//struct queue_ll pool_queue[NUM_CMDS];

#define RESPONSE_QUEUE_SIZE	128
int mem_allocator_push_idx = 0;
int mem_allocator_pop_idx = 0;
static  struct queue_ll *response_queue_ptr;

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
*   Type will tell if Request or Response
*/ 
static int pop_request(struct skbuff_nic_c **skbuff_struct, int type) {

	struct queue_ll *temp_node;

	/* Check if there is something in the queue */
	if(list_empty(&head_request)) {
		/* Return -1, no element is found */
		return -1;
	}
	else {
		temp_node = list_first_entry(&head_request,struct queue_ll ,list);
	}


	/* This structure needs to be passed to thread */
	*skbuff_struct = temp_node->skbuff_struct;

	/* Clear the node */
	list_del(&temp_node->list);

	kfree(temp_node);

	/* Return 0, element is found */
	return 0;
}

static int pop_response(struct skbuff_nic_c **skbuff_struct, int type) {

	struct queue_ll *temp_node;


	/* Check if there is something in the queue */
	if(list_empty(&head_response)) {

		/* Return -1, no element is found */
		return -1;
	}
	else {
		mutex_lock(&pop_response_lock);
		temp_node = list_first_entry(&head_response,struct queue_ll ,list);
		mem_allocator_pop_idx = (mem_allocator_pop_idx + 1) % RESPONSE_QUEUE_SIZE;
	}

	/* This structure needs to be passed to thread */
	*skbuff_struct = temp_node->skbuff_struct;

	/* Clear the node */
	list_del(&temp_node->list);

	mutex_unlock(&pop_response_lock);

	/* Return 0, element is found */
	return 0;
}

/* 
*	Push element in queue head
*	Element will be passed by reference
*/ 
void push_request(struct skbuff_nic_c **skbuff_struct, int type) {

	struct queue_ll *temp_node;

	/* Allocate Node */
	temp_node=kmalloc(sizeof(struct queue_ll),GFP_ATOMIC);

	/* skbuff needs to be add to link list */
	temp_node->skbuff_struct = *skbuff_struct;

	mutex_lock(&push_request_lock);

	/* Add element to link list */
	list_add_tail(&temp_node->list,&head_request);

	mutex_unlock(&push_request_lock);
}

void push_response(struct skbuff_nic_c **skbuff_struct, int type) {
//	struct queue_ll *temp_node = (struct queue_ll*)&pool_queue[alloc_index++];

	struct queue_ll *temp_node;

	if (((mem_allocator_push_idx) % RESPONSE_QUEUE_SIZE) != ((mem_allocator_pop_idx + 1) % RESPONSE_QUEUE_SIZE))
	{
		temp_node = (struct queue_ll*) (response_queue_ptr + mem_allocator_push_idx);
		mem_allocator_push_idx = (mem_allocator_push_idx + 1) % RESPONSE_QUEUE_SIZE;
	}
	else
	{
		while(((mem_allocator_push_idx) % RESPONSE_QUEUE_SIZE) == ((mem_allocator_pop_idx + 1) % RESPONSE_QUEUE_SIZE));
		mem_allocator_push_idx = (mem_allocator_push_idx + 1) % RESPONSE_QUEUE_SIZE;
	}

	/* Allocate Node */

	/* skbuff needs to be add to link list */
	temp_node->skbuff_struct = *skbuff_struct;
	
	/* Add element to link list */
	list_add_tail(&temp_node->list,&head_response);
}

/*
*	Main NIC-C Model Thread
*	This thread will schedule process request 
*	as soon some element push into the queue
*/
static int c_model_worker_thread(void *unused)
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
			
			/* Check if some command is in queue */
			/* If found, element will be point to skbuff_ptr */
        	if (pop_request(&skbuff_ptr, TYPE_REQUEST) != -1) {

				cmd_rcv++;

				switch (skbuff_ptr->meta.command)
				{
					/* Dummy RX Command */
					case PROCESS_RX:
					{
						/* Print Information */
						printk(KERN_ALERT "RX Command | Len = %d | CPU = %d\n", skbuff_ptr->len, skbuff_ptr->meta.cpu);

						/* Update response flag */
						skbuff_ptr->meta.response_flag = CASE_NOTIFY_STACK_RX;
						skbuff_ptr->meta.poll_flag = POLL_IF_RESPONSE_READ;

						/* Pass skbuff to response queue */
						push_response(&skbuff_ptr, TYPE_RESPONSE);


						flag[skbuff_ptr->meta.cpu] = 'y';

						wake_up(&my_wait_queue[skbuff_ptr->meta.cpu]);

						/* Release semaphore to wake per CPU thread to pass command to stack */
	    				down (&wait_sem[skbuff_ptr->meta.cpu]);

						while (skbuff_ptr->meta.poll_flag == POLL_IF_RESPONSE_READ){}

						break;
					}
					case PROCESS_TX:
					{
						/* Print Information */
						printk(KERN_ALERT "TX Command | Len = %d | CPU = %d\n", skbuff_ptr->len, skbuff_ptr->meta.cpu);

						/* Update response flag */
						skbuff_ptr->meta.response_flag = CASE_NOTIFY_STACK_TX;

						skbuff_ptr->meta.poll_flag = POLL_IF_RESPONSE_READ;

						/* Pass skbuff to response queue */
						push_response(&skbuff_ptr, TYPE_RESPONSE);

						flag[skbuff_ptr->meta.cpu] = 'y';

						wake_up(&my_wait_queue[skbuff_ptr->meta.cpu]);

						/* Release semaphore to wake per CPU thread to pass command to stack */
	    				down (&wait_sem[skbuff_ptr->meta.cpu]);

	    				while (skbuff_ptr->meta.poll_flag == POLL_IF_RESPONSE_READ){}

						break;
					}
				}
        	}
			clk_cycles_start = 0;
        }
    	else
    	{
			//TODO: Add counter to keep track of cycles spend here
			/* This is necessary as we have to unschedule this
			   thread after some rdtsc for a very short amount
			   of time, for the sake of load balancing. Otherwise
			   we get system gets stuck if a core continously
			   spend its cycle in a while loop */
    		schedule_timeout (0); // Sleep for 500 clock cycles
    	}
    }

    printk(KERN_ALERT "c_model_worker_thread Exit!!!\n");

    return 0;
}

/*
*	Main NIC-C Model Thread
*	This thread will schedule process request
*	as soon some element push into the queue
*/
static int response_per_cpu_thread(void *unused)
{
	struct skbuff_nic_c *skbuff_ptr;

	int response_per_cpu = 0;
	int cpu = get_cpu();
	while (1)
	{	
	    wait_event(my_wait_queue[cpu], flag[cpu] != 'n');

		flag[cpu] = 'n';

		up (&wait_sem[cpu]);

		if (pop_response(&skbuff_ptr, TYPE_RESPONSE) != -1)
		{
			skbuff_ptr->meta.poll_flag = POLL_END_RESPONSE_READ;
			response_total++;
			response_per_cpu++;

			switch (skbuff_ptr->meta.response_flag)
			{
				case CASE_NOTIFY_STACK_RX:
				{
					/* Parse the thread data */
					printk(KERN_ALERT "\nResponse | Core-%d | Total->%d\n", cpu, response_per_cpu);

					break;
				}
				case CASE_NOTIFY_STACK_TX:
				{
					/* Parse the thread data */
					printk(KERN_ALERT "\nResponse | Core-%d | Total->%d\n", cpu, response_per_cpu);

					break;
				}
			}
		}

		if (response_thread_exit)
			break;
	}

	printk(KERN_ALERT "Core-%d | Responses => %d\n", cpu, response_per_cpu);

    return 0;
}

static int request_per_cpu_thread(void *unused)
{
	int i = 0;
	struct skbuff_nic_c *skbuff_struc_temp;
	/* Push Dummy RX Command */
	for (i=0; i<NUM_CMDS/NUM_CPUS; i++)
	{
		skbuff_struct_driver[get_cpu()][i].skbuff = &global_skbuff_pass;//(u8*) kmalloc(4,GFP_KERNEL);
		skbuff_struct_driver[get_cpu()][i].len = i + 1;
		skbuff_struct_driver[get_cpu()][i].meta.cpu = get_cpu();
		skbuff_struct_driver[get_cpu()][i].meta.response_flag = 0;

		// Half should be TX commands and half should be RX
		if ((i % 2) == 0)
			skbuff_struct_driver[get_cpu()][i].meta.command = PROCESS_RX;
		else
			skbuff_struct_driver[get_cpu()][i].meta.command = PROCESS_TX;

		skbuff_struc_temp = &skbuff_struct_driver[get_cpu()][i];
		push_request(&skbuff_struc_temp, TYPE_REQUEST);
		mutex_lock(&driver_request_lock);
		cmd_send++;
		mutex_unlock(&driver_request_lock);
	}

    return 0;
}

static int __init nic_c_init(void) {

	int i = 0;

	/* Initilize Queue */
	printk(KERN_INFO "NIC-C Model Init!\n");

	INIT_LIST_HEAD(&head_request);
	INIT_LIST_HEAD(&head_response);

	response_queue_ptr = kmalloc(sizeof(struct queue_ll) * RESPONSE_QUEUE_SIZE, GFP_ATOMIC);

	// Create and bind and execute thread to core-2
	thread_st_c_model_worker = kthread_create(c_model_worker_thread, NULL, "kthread_c_model_worker");
	kthread_bind(thread_st_c_model_worker, NUM_CPUS - 1);

	wake_up_process(thread_st_c_model_worker);

	for (i=0; i<NUM_CPUS; i++)
	{
		init_waitqueue_head(&my_wait_queue[i]);
		thread_st_response[i] = kthread_create(response_per_cpu_thread, NULL, "kthread_response");
		kthread_bind(thread_st_response[i], i);
		wake_up_process(thread_st_response[i]);
		sema_init(&wait_sem[i], 1);
		/* Release semaphore to wake per CPU thread to pass command to stack */
		down (&wait_sem[i]);
		flag[i] = 'n';
	}

	for (i=0; i<NUM_CPUS; i++)
	{
		thread_st_request[i] = kthread_create(request_per_cpu_thread, NULL, "kthread_request");
		kthread_bind(thread_st_request[i], i);
		wake_up_process(thread_st_request[i]);
	}

	/* Wait for a second to let the thread being schedule */
	printk(KERN_INFO "NIC-C Model Init Ends | CPU = %d!\n", num_online_cpus());
	return 0;
}

static void __exit nic_c_exit(void) {

	int i = 0;

	kthread_stop(thread_st_c_model_worker);

	response_thread_exit = 1;

	for (i=0; i<NUM_CPUS; i++)
	{

		flag[i] = 'y';

		wake_up(&my_wait_queue[i]);

		/* Release semaphore to wake per CPU thread to pass command to stack */
		down (&wait_sem[i]);
	}
	kfree (response_queue_ptr);

	printk(KERN_ALERT "CMD Send => %d\n", cmd_send);
	printk(KERN_ALERT "CMD Receive C-Model => %d\n", cmd_rcv);
	printk(KERN_ALERT "Response Receive Driver=> %d\n", response_total);
}

module_init(nic_c_init);
module_exit(nic_c_exit);
