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
MODULE_DESCRIPTION("NIC-C Model Description");
MODULE_VERSION("0.1");
#define CASE_WAIT_IDLE			'a'
#define CASE_WAIT_EXIT   		'b'
#define CASE_NOTIFY_STACK_TX   	123
#define CASE_NOTIFY_STACK_RX   	456

#define RESPONSE_NEEDED

#define TYPE_REQUEST 	0
#define TYPE_RESPONSE	1

#define NUM_CPUS 	4
#define THOUSAND	1000
#define MILLION		THOUSAND*THOUSAND
#define NUM_CMDS	128//1*MILLION

int cnt_resp = 0;

volatile char flag[NUM_CMDS] = {'n'};
volatile u8   helper_flag[NUM_CMDS] = {0};

u8 response_thread_exit = 0;

static DEFINE_MUTEX(dealloc_lock);
static DEFINE_MUTEX(alloc_lock);
static DEFINE_MUTEX(push_lock);
static DEFINE_MUTEX(pop_lock);
static DEFINE_MUTEX(push_resp_lock);
static DEFINE_MUTEX(pop_resp_lock);
static DEFINE_MUTEX(response_lock);
static DEFINE_MUTEX(req_lock);

/* Commands */
#define STATE_IN_POLLING    1
#define STATE_END_POLLED	2
#define PROCESS_RX 			1
#define PROCESS_TX 			2

/* Global data types */
static wait_queue_head_t my_wait_queue[NUM_CPUS];
static struct semaphore   wait_sem[NUM_CPUS];
static struct task_struct *thread_st_nic;
static struct task_struct *thread_per_cpu[NUM_CPUS];
static struct task_struct *req_thread_per_cpu[NUM_CPUS];
static struct list_head   head;
static struct list_head   head_response;

static int 	  thread_fn(void *unused);

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

/* Queue to keep track of NIC-C Commands & SKBUFFS */
struct queue_ll_resp{
     struct list_head list;
     struct skbuff_nic_c *skbuff_struct;
};

//int alloc_limit = NUM_CMDS;
int alloc_index = 0;
int alloc_index_2 = 0;
struct queue_ll pool_queue[NUM_CMDS];
struct queue_ll_resp pool_queue_2[NUM_CMDS];

//TODO: Make it allocate at runtime
/* Buffer that driver will use */
struct skbuff_nic_c skbuff_driver[NUM_CPUS][NUM_CMDS];

/* 
*	Get CPU Cycles from Read RDTSC Function
*/ 
static inline u64 read_rdtsc(void)
{
    u64 lo, hi;

    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) :: "ecx" );

    return (u64) ((hi << 32) | lo);
}
static inline void *kvmalloc_custom(size_t size)
{
	void *ret_alloc;
	mutex_lock(&alloc_lock);
	ret_alloc = kvmalloc(size, GFP_ATOMIC);
	mutex_unlock(&alloc_lock);
	return ret_alloc;
}

void kvfree_custom(void *addr)
{
	mutex_lock(&dealloc_lock);
	kvfree(addr);
	mutex_unlock(&dealloc_lock);
}

/* 
*	Pop last element from the queue
*	Return-> 0  if found
*	Return-> -1 if empty queue
*	Element will be get by reference
*   Type will tell if Request or Response
*/ 
static int pop_queue(struct skbuff_nic_c **skbuff_struct, int type) {

	struct queue_ll *temp_node;

	/* Check if there is something in the queue */
	if(list_empty(&head)) {
		/* Return -1, no element is found */
		return -1;
	}
	else {
//		mutex_lock(&pop_lock);
		temp_node = list_first_entry(&head,struct queue_ll ,list);
//		mutex_unlock(&pop_lock);
	}


	/* This structure needs to be passed to thread */
	*skbuff_struct = temp_node->skbuff_struct;

	/* Clear the node */
	list_del(&temp_node->list);
	kfree(temp_node);

	/* Return 0, element is found */
	return 0;
}

#ifdef RESPONSE_NEEDED
static int pop_queue_response(struct skbuff_nic_c **skbuff_struct, int type) {

	struct queue_ll_resp *temp_node;

	mutex_lock(&pop_resp_lock);

	/* Check if there is something in the queue */
	if(list_empty(&head_response)) {
		/* Return -1, no element is found */
		mutex_unlock(&pop_resp_lock);
		return -1;
	}
	else {
		temp_node = list_first_entry(&head_response,struct queue_ll_resp ,list);
	}

	/* This structure needs to be passed to thread */
	*skbuff_struct = temp_node->skbuff_struct;

	/* Clear the node */
	list_del(&temp_node->list);
//	kvfree(temp_node);

	mutex_unlock(&pop_resp_lock);
	/* Return 0, element is found */
	return 0;
}
#endif

/* 
*	Push element in queue head
*	Element will be passed by reference
*/ 
void push_queue(struct skbuff_nic_c **skbuff_struct, int type) {

	struct queue_ll *temp_node;// = (struct queue_ll*)&pool_queue[alloc_index++];

	mutex_lock(&push_lock);

	/* Allocate Node */
//	temp_node=kmalloc(sizeof(struct queue_ll));
	temp_node=kmalloc(sizeof(struct queue_ll),GFP_ATOMIC);
//	pool_queue[alloc_index] =

	/* skbuff needs to be add to link list */
	temp_node->skbuff_struct = *skbuff_struct;
	
	/* Add element to link list */
	list_add_tail(&temp_node->list,&head);
	mutex_unlock(&push_lock);
}
#ifdef RESPONSE_NEEDED

void push_queue_response(struct skbuff_nic_c **skbuff_struct, int type) {
//	static struct queue_ll_resp *temp_node;
	struct queue_ll_resp *temp_node = (struct queue_ll_resp*)&pool_queue_2[alloc_index_2++];

	/* Allocate Node */
//	temp_node=kvmalloc_custom(sizeof(struct queue_ll_resp));
//	temp_node=kvmalloc(sizeof(struct queue_ll_resp), GFP_ATOMIC);

	/* skbuff needs to be add to link list */
	temp_node->skbuff_struct = *skbuff_struct;
	
	/* Add element to link list */
//	mutex_lock(&push_resp_lock);
	list_add_tail(&temp_node->list,&head_response);
//	mutex_unlock(&push_resp_lock);
}
#endif
/*
*	Main NIC-C Model Thread
*	This thread will schedule process request 
*	as soon some element push into the queue
*/
int cmd_rcv = 0;
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
			
			/* Check if some command is in queue */
			/* If found, element will be point to skbuff_ptr */
        	if (pop_queue(&skbuff_ptr, TYPE_REQUEST) != -1) {

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
#ifdef RESPONSE_NEEDED
						skbuff_ptr->meta.poll_flag = 0;

//						printk(KERN_ALERT "RX Command_2\n");
						/* Pass skbuff to response queue */
						push_queue_response(&skbuff_ptr, TYPE_RESPONSE);


//						printk(KERN_ALERT "RX Command_3 | Flag -> %c\n", flag[skbuff_ptr->meta.cpu]);
						flag[skbuff_ptr->meta.cpu] = 'y';

						wake_up(&my_wait_queue[skbuff_ptr->meta.cpu]);


//						printk(KERN_ALERT "RX Command_4 | Flag -> %c\n", flag[skbuff_ptr->meta.cpu]);

						/* Release semaphore to wake per CPU thread to pass command to stack */
	    				down (&wait_sem[skbuff_ptr->meta.cpu]);

//						printk(KERN_ALERT "RX Command_5 | Flag -> %c\n", flag[skbuff_ptr->meta.cpu]);

						while (skbuff_ptr->meta.poll_flag == 0);

//						printk(KERN_ALERT "RX Command_6\n");
//
//						skbuff_ptr->meta.poll_flag = 1;
#endif
						break;
					}
					case PROCESS_TX:
					{
						/* Print Information */
						printk(KERN_ALERT "TX Command | Len = %d | CPU = %d\n", skbuff_ptr->len, skbuff_ptr->meta.cpu);

						/* Update response flag */
						skbuff_ptr->meta.response_flag = CASE_NOTIFY_STACK_TX;

#ifdef RESPONSE_NEEDED
						skbuff_ptr->meta.poll_flag = 0;

						/* Pass skbuff to response queue */
						push_queue_response(&skbuff_ptr, TYPE_RESPONSE);

						flag[skbuff_ptr->meta.cpu] = 'y';

						wake_up(&my_wait_queue[skbuff_ptr->meta.cpu]);

						/* Release semaphore to wake per CPU thread to pass command to stack */
	    				down (&wait_sem[skbuff_ptr->meta.cpu]);

	    				while (skbuff_ptr->meta.poll_flag == 0);
//						skbuff_ptr->meta.poll_flag = 1;
#endif
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

    printk(KERN_ALERT "thread_fn Exit!!!\n");

    return 0;
}

/*
*	Main NIC-C Model Thread
*	This thread will schedule process request
*	as soon some element push into the queue
*/
int repsonse_cnt = 0;
static int response_thread_per_cpu(void *unused)
{
#ifdef RESPONSE_NEEDED
	struct skbuff_nic_c *skbuff_ptr;
#endif
	int repsonse_cnt_local = 0;
	int cpu = get_cpu();
	while (1)
	{	
//		printk(KERN_ALERT "Response_1[%d]\n",cpu);
	    wait_event(my_wait_queue[cpu], flag[cpu] != 'n');
//		printk(KERN_ALERT "Response_2[%d]\n",cpu);
		flag[cpu] = 'n';
		up (&wait_sem[cpu]);
#ifdef RESPONSE_NEEDED
//		printk(KERN_ALERT "Response_3[%d]\n",cpu);
		if (pop_queue_response(&skbuff_ptr, TYPE_RESPONSE) != -1)
		{
//			printk(KERN_ALERT "Response_4[%d]\n",cpu);
			skbuff_ptr->meta.poll_flag = 1;
			repsonse_cnt++;
			repsonse_cnt_local++;
//			printk(KERN_ALERT "Responses => [%d]\n", repsonse_cnt);
			switch (skbuff_ptr->meta.response_flag)
			{
				case CASE_NOTIFY_STACK_RX:
				{
//					printk(KERN_ALERT "Response_5[%d]\n",cpu);
					/* Parse the thread data */
					printk(KERN_ALERT "\nResponse | Core-%d | Total->%d\n", cpu, repsonse_cnt_local);

//					printk(KERN_ALERT "Response_6[%d]\n",cpu);
					break;
				}
				case CASE_NOTIFY_STACK_TX:
				{
					/* Parse the thread data */
					printk(KERN_ALERT "\nResponse | Core-%d | Total->%d\n", cpu, repsonse_cnt_local);

					break;
				}
			}
		}
#endif
		if (response_thread_exit)
			break;
	}

	printk(KERN_ALERT "Core-%d | Responses => %d\n", cpu, repsonse_cnt_local);
//	printk("Thread-%d exitting...\n", get_cpu());

    return 0;
}


u64 global_skbuff_pass = 0xDEADBEEFBEEFDEAD;
int cmd_send = 0;
static int request_thread_per_cpu(void *unused)
{
	int i = 0;
	struct skbuff_nic_c *skbuff_struc_temp;
	/* Push Dummy RX Command */
	for (i=0; i<NUM_CMDS/NUM_CPUS; i++)
	{
//		mutex_lock(&req_lock);
		skbuff_driver[get_cpu()][i].skbuff = &global_skbuff_pass;//(u8*) kmalloc(4,GFP_KERNEL);
		skbuff_driver[get_cpu()][i].len = i + 1;
		skbuff_driver[get_cpu()][i].meta.cpu = get_cpu();
		skbuff_driver[get_cpu()][i].meta.response_flag = 0;
		// Half should be TX commands and half should be RX
		if ((i % 2) == 0)
			skbuff_driver[get_cpu()][i].meta.command = PROCESS_RX;
		else
			skbuff_driver[get_cpu()][i].meta.command = PROCESS_TX;
		skbuff_struc_temp = &skbuff_driver[get_cpu()][i];
		push_queue(&skbuff_struc_temp, TYPE_REQUEST);
//		printk(KERN_ALERT "Driver Cmd[%d]\n", i);
		cmd_send++;
//		mutex_unlock(&req_lock);
//		msleep(1);
	}

    return 0;
}

static int __init nic_c_init(void) {
	int i = 0;
	/* Initilize Queue */
	printk(KERN_INFO "NIC-C Model Init!\n");
//	head=kmalloc(sizeof(struct list_head *),GFP_KERNEL);
	INIT_LIST_HEAD(&head);

//	head_response=kmalloc(sizeof(struct list_head *),GFP_KERNEL);
	INIT_LIST_HEAD(&head_response);

	// Create and bind and execute thread to core-2
	thread_st_nic = kthread_create(thread_fn, NULL, "kthread");

	kthread_bind(thread_st_nic, 3);
	wake_up_process(thread_st_nic);

	for (i=0; i<NUM_CPUS; i++)
	{
		init_waitqueue_head(&my_wait_queue[i]);
		thread_per_cpu[i] = kthread_create(response_thread_per_cpu, NULL, "kthread_cpu");
		kthread_bind(thread_per_cpu[i], i);
		wake_up_process(thread_per_cpu[i]);
		sema_init(&wait_sem[i], 1);
		/* Release semaphore to wake per CPU thread to pass command to stack */
		down (&wait_sem[i]);
		flag[i] = 'n';
	}

	for (i=0; i<4; i++)
	{
		req_thread_per_cpu[i] = kthread_create(request_thread_per_cpu, NULL, "kthread_cpu_req");
		kthread_bind(req_thread_per_cpu[i], i);
		wake_up_process(req_thread_per_cpu[i]);
	}

	/* Wait for a second to let the thread being schedule */
//	ssleep(10);
	printk(KERN_INFO "NIC-C Model Init Ends | CPU = %d!\n", num_online_cpus());
	ssleep (1);
	return 0;
}

static void __exit nic_c_exit(void) {
	int i = 0;
#if 0
   struct queue_ll *temp1, *temp2;
   int count = 0;
#endif
   kthread_stop(thread_st_nic);

#if 0
   list_for_each_entr y_safe(temp1, temp2, head, list) {
	   printk(KERN_INFO "Node %d data = %d\n" , count++, temp1->skbuff_struct->len);

			list_del(&temp1->list);list_first_entry(head,struct queue_ll ,list);
			kfree(temp1);
   }
#endif

	response_thread_exit = 1;

	ssleep (1);

	for (i=0; i<NUM_CPUS; i++)
	{

		flag[i] = 'y';

		wake_up(&my_wait_queue[i]);

		/* Release semaphore to wake per CPU thread to pass command to stack */
		down (&wait_sem[i]);
	}

	//TODO: Do something better than sleep
	/* Wait until threads to exit */

	/* Dealocate all memories */
//	kfree(head);
//	kfree(head_response);


	printk(KERN_ALERT "CMD Send => %d\n", cmd_send);
	printk(KERN_ALERT "CMD Receive C-Model => %d\n", cmd_rcv);
	printk(KERN_ALERT "Response Receive Driver=> %d\n", repsonse_cnt);
//   	printk(KERN_INFO "NIC-C Model Exit!\n");
}

module_init(nic_c_init);
module_exit(nic_c_exit);
