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

/* Module Information */
MODULE_LICENSE		("GPL");
MODULE_AUTHOR		("Ameer Hamza");
MODULE_DESCRIPTION	("NIC-C Model Basic Infrastructure");
MODULE_VERSION		("0.1");

/* Commands */
#define CASE_NOTIFY_STACK_TX   	123
#define CASE_NOTIFY_STACK_RX   	456
#define PROCESS_RX                1
#define PROCESS_TX                2

/* CPUs/Commands */
#define NUM_CPUS    4
#define THOUSAND    1000
#define MILLION     THOUSAND*THOUSAND
#define NUM_CMDS    4*MILLION

/* Response Queue Size */
#define RESPONSE_QUEUE_SIZE     8192

/* Response Type */
#define RESPONSE_TYPE_PUSH   0
#define RESPONSE_TYPE_POP    1
#define QUEUE_EMPTY         -1
#define RESPONSE_QUEUE_FULL -2

/* Global Variables */
char exit_flag[NUM_CPUS] = {'n'};
u64  skbuff_dummy_var = 0xDEADBEEFBEEFDEAD;
u32  num_cmd_send = 0;
u32  num_cmd_rcv = 0;
u32  num_total_response = 0;
u64  mem_allocator_push_idx[NUM_CPUS] = {0};
u64  mem_allocator_pop_idx [NUM_CPUS] = {0};

/* Push/Pop response indexers */
volatile u64  num_responses_push[NUM_CPUS] = {0};
volatile u64  num_responses_pop[NUM_CPUS]  = {0};

/* Define Mutex locks */
static DEFINE_MUTEX(push_request_lock);
static DEFINE_MUTEX(push_pop_response_lock);
static DEFINE_MUTEX(driver_request_lock);
static DEFINE_MUTEX(driver_response_lock);

/* Global structures */
static struct task_struct *thread_st_c_model_worker;
static struct task_struct *thread_st_response[NUM_CPUS];
static struct task_struct *thread_st_request[NUM_CPUS];
static struct list_head   head_request;
static struct list_head   head_response[NUM_CPUS];
static wait_queue_head_t  my_wait_queue[NUM_CPUS];

/* Meta data for NIC-C Model */
struct meta_skbuff {
    u32 command;
    u32 response_flag;
    volatile u8 cpu;
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
struct skbuff_nic_c skbuff_struct_driver[NUM_CPUS][NUM_CMDS];

/* Since kmalloc is not correctly working for a C-Model thread, This pointer is responsible for custom memory allocation */
static  struct queue_ll *response_queue_ptr[NUM_CPUS];

/* 
*	Get CPU Cycles from Read RDTSC Function
*/ 
static inline u64 read_rdtsc(void) {
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
static int pop_request(struct skbuff_nic_c **skbuff_struct) {

    struct queue_ll *temp_node;

    /* Check if there is something in the queue */
    if(list_empty(&head_request)) {

        /* Return QUEUE_EMPTY, no element is found */
        return QUEUE_EMPTY;
    }
    else {
        /* Get the node from link list queue */
        temp_node = list_first_entry(&head_request,struct queue_ll ,list);
    }

    /* This structure needs to be passed back to caller */
    *skbuff_struct = temp_node->skbuff_struct;

    /* Clear the node */
    list_del(&temp_node->list);

    kfree(temp_node);

    /* Return 0, element is found */
    return 0;
}

/*
*	Request in this link list will be pushed by the driver
*	Push element in queue head
*	Element will be passed by reference
*/ 
void push_request(struct skbuff_nic_c **skbuff_struct) {

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

/*
*	Response will be pushed/pop in separate queue according to the type
*	Push element in queue tail / Pop element from queue head
*	Elements will be passed by reference
*/ 
static int push_pop_response(struct skbuff_nic_c **skbuff_struct, int cpu, int response_type) {
    struct queue_ll *temp_node;

    mutex_lock(&push_pop_response_lock);
    
    /* If type is push response */
    if (response_type == RESPONSE_TYPE_PUSH) {
        /* Check that queue is not full */
        if (((mem_allocator_push_idx[cpu] + 1) % RESPONSE_QUEUE_SIZE) != ((mem_allocator_pop_idx[cpu]) % RESPONSE_QUEUE_SIZE)) {
            /* Allocate the node and increment push_allocator idx */
            temp_node = (struct queue_ll*) (response_queue_ptr[cpu] + mem_allocator_push_idx[cpu]);
            mem_allocator_push_idx[cpu] = (mem_allocator_push_idx[cpu] + 1) % RESPONSE_QUEUE_SIZE;
        }

    /* Else wait until queue has some space */
    else {
        mutex_unlock(&push_pop_response_lock);
        return RESPONSE_QUEUE_FULL;
    }

    /* skbuff needs to be add to link list */
    temp_node->skbuff_struct = *skbuff_struct;

    /* Add element to link list */
    list_add_tail(&temp_node->list,&head_response[cpu]);
    }
    /* Otherwise pop response type */
    else {
        if(list_empty(&head_response[cpu])) {

            /* Release the lock */
            mutex_unlock(&push_pop_response_lock);

            /* Return -1, no element is found */
            return QUEUE_EMPTY;
        }
        else {
            /* Since this is response list and will be shared by multiple thread, acquire the lock */

            /* Get the node from link list */
            temp_node = list_first_entry(&head_response[cpu],struct queue_ll ,list);

            mem_allocator_pop_idx[cpu] = (mem_allocator_pop_idx[cpu] + 1) % RESPONSE_QUEUE_SIZE;
        }

        /* This structure needs to be passed to thread */
        *skbuff_struct = temp_node->skbuff_struct;

        /* Clear the node */
        list_del(&temp_node->list);

    }

    mutex_unlock(&push_pop_response_lock);

    return 0;
}

/*
*	--Main NIC-C Model Thread--
*	This thread is responsible for scheduling request
*	as soon some element is push into the queue
*/
static int c_model_worker_thread(void *unused) {
    struct skbuff_nic_c *skbuff_ptr;

    /* Local variables to keep track of CPU Cycles */
    u64 clk_cycles_start = 0;
    u64 clk_cycles_end = 0;
    u64 clk_cycles_exp = 0;
    u64 clk_cycles_div = 500;

    printk(KERN_ALERT "C-Model Thread binded to Core # %d\n", get_cpu());

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
            if (pop_request(&skbuff_ptr) != -1) {

                /* Increment total commands received */
                num_cmd_rcv++;

                /* Check what command requested */
                switch (skbuff_ptr->meta.command) {

                    /* Dummy RX Command */
                    case PROCESS_RX:

                        /* Print Information */
                        printk(KERN_ALERT "RX Command | Len = %d | CPU = %d\n", skbuff_ptr->len, skbuff_ptr->meta.cpu);

                        /* Update response flag to schedule task for response thread*/
                        skbuff_ptr->meta.response_flag = CASE_NOTIFY_STACK_RX;

                        /* Pass skbuff to response queue */
                        if (push_pop_response(&skbuff_ptr, skbuff_ptr->meta.cpu, RESPONSE_TYPE_PUSH) == RESPONSE_QUEUE_FULL) {
                            do {
                                /* Some delay so that response queue has some space */
                                udelay (1000);
                            } while  (push_pop_response(&skbuff_ptr, skbuff_ptr->meta.cpu, RESPONSE_TYPE_PUSH) == RESPONSE_QUEUE_FULL);
                        }

                        ++num_responses_push[skbuff_ptr->meta.cpu];

                        wake_up(&my_wait_queue[skbuff_ptr->meta.cpu]);

                        break;

                    case PROCESS_TX:

                        /* Print Information */
                        printk(KERN_ALERT "TX Command | Len = %d | CPU = %d\n", skbuff_ptr->len, skbuff_ptr->meta.cpu);

                        /* Update response flag to schedule task for response thread*/
                        skbuff_ptr->meta.response_flag = CASE_NOTIFY_STACK_TX;

                        /* Pass skbuff to response queue */
                        if (push_pop_response(&skbuff_ptr, skbuff_ptr->meta.cpu, RESPONSE_TYPE_PUSH) == RESPONSE_QUEUE_FULL) {
                            do {
                                /* Some delay so that response queue has some space */
                                udelay (1000);
                            } while  (push_pop_response(&skbuff_ptr, skbuff_ptr->meta.cpu, RESPONSE_TYPE_PUSH) == RESPONSE_QUEUE_FULL);
                        }

                        ++num_responses_push[skbuff_ptr->meta.cpu];

                        wake_up_interruptible(&my_wait_queue[skbuff_ptr->meta.cpu]);

                        break;
                }
            }

            clk_cycles_start = 0;
        }
        else {
            /* This is necessary as we have to unschedule this
               thread after some rdtsc for a very short amount
               of time, for the sake of load balancing. Otherwise
               we get system gets stuck if a core continously
               spend its cycle in a while loop */
            schedule_timeout (0);
        }
    }

    /* Module is exitted */
    printk(KERN_ALERT "C-Model worker thread Exits!!!\n");

    return 0;
}

/*
*	Response thread scheduler
*	This thread will schedule response request
*	as soon some element push in response list
*	and wait queue flag for CPU is signalled
*/
static int response_per_cpu_thread(void *unused) {
    struct skbuff_nic_c *skbuff_ptr;

    int response_per_cpu = 0;
    int cpu = get_cpu();
    while (1) {

    	wait_event_interruptible(my_wait_queue[cpu], (num_responses_push[cpu] != num_responses_pop[cpu]) || (exit_flag[cpu] != 'n'));
        
        /* Exit flag is raised, break from the loop */
        if (exit_flag[cpu] == 'y') {
            break;
        }

        /* If response queue is not empty */
        if (push_pop_response(&skbuff_ptr, cpu, RESPONSE_TYPE_POP) != -1) {
            
            /* Increment response counter */
            ++num_responses_pop[cpu];

            /* Update statistics counter */
            mutex_lock(&driver_response_lock);
            num_total_response++;
            mutex_unlock(&driver_response_lock);

            response_per_cpu++;

            /* Check what response is scheduled by C-Model */
            switch (skbuff_ptr->meta.response_flag) {

                case CASE_NOTIFY_STACK_RX:

                    /* Simply Print the information */
                    printk(KERN_ALERT "Response | Core-%d | Total->%d\n", cpu, response_per_cpu);

                    break;

                case CASE_NOTIFY_STACK_TX:

                    /* Simply Print the information */
                    printk(KERN_ALERT "Response | Core-%d | Total->%d\n", cpu, response_per_cpu);

                    break;
            }
        }
    }

    /* Print per CPU response count */
    printk(KERN_ALERT "Core-%d | Responses => %d\n", cpu, response_per_cpu);

    return 0;
}

/*
*	Request thread scheduler
*	This thread emulating the calls just like driver
*	It will be run on all the CPUs and will generate
*	concurrent requests
*/
static int request_per_cpu_thread(void *unused) {
    int i = 0;
    struct skbuff_nic_c *skbuff_struc_temp;

    /* Divide Number of Commands to send among total number of CPUs */
    for (i=0; i<NUM_CMDS/NUM_CPUS; i++) {

        /* Populate dummy structure */
        skbuff_struct_driver[get_cpu()][i].skbuff = &skbuff_dummy_var;//(u8*) kmalloc(4,GFP_KERNEL);
        skbuff_struct_driver[get_cpu()][i].len = i + 1;
        skbuff_struct_driver[get_cpu()][i].meta.cpu = get_cpu();
        skbuff_struct_driver[get_cpu()][i].meta.response_flag = 0;

        /* Divide half dummy requests as RX, remaining as TX */
        if ((i % 2) == 0)
            skbuff_struct_driver[get_cpu()][i].meta.command = PROCESS_RX;
        else
            skbuff_struct_driver[get_cpu()][i].meta.command = PROCESS_TX;

        /* Push request in the list and return */
        skbuff_struc_temp = &skbuff_struct_driver[get_cpu()][i];
        push_request(&skbuff_struc_temp);

        /* Update request counter */
        mutex_lock(&driver_request_lock);
        num_cmd_send++;
        mutex_unlock(&driver_request_lock);
    }

    return 0;
}

/*
*	This is init_module routine
*	This routine is responsible for allocating
*	and managing resources
*/
static int __init nic_c_init(void) {

    int i = 0;

    /* Initilize Queue */
    printk(KERN_INFO "NIC-C Model Init!\n");

    INIT_LIST_HEAD(&head_request);

    for (i=0; i<NUM_CPUS; i++) {
        INIT_LIST_HEAD(&head_response[i]);
        response_queue_ptr[i] = kmalloc(sizeof(struct queue_ll) * RESPONSE_QUEUE_SIZE, GFP_ATOMIC);
    }

    /* Bind C-Model worker thread to the last core */
    thread_st_c_model_worker = kthread_create(c_model_worker_thread, NULL, "kthread_c_model_worker");
    kthread_bind(thread_st_c_model_worker, NUM_CPUS - 1);
    wake_up_process(thread_st_c_model_worker);

    for (i=0; i<NUM_CPUS; i++) {

        /* Initialization for response thread */
        init_waitqueue_head(&my_wait_queue[i]);
        thread_st_response[i] = kthread_create(response_per_cpu_thread, NULL, "kthread_response");
        kthread_bind(thread_st_response[i], i);
        wake_up_process(thread_st_response[i]);
        /* Release semaphore to wake per CPU thread to pass command to stack */
        exit_flag[i] = 'n';
    }

    for (i=0; i<NUM_CPUS; i++) {

        /* Initialization for request thread */
        thread_st_request[i] = kthread_create(request_per_cpu_thread, NULL, "kthread_request");
        kthread_bind(thread_st_request[i], i);
        wake_up_process(thread_st_request[i]);
    }

    /* Wait for a second to let the thread being schedule */
    printk(KERN_INFO "NIC-C Model Init Ends | CPU = %d!\n", num_online_cpus());
    return 0;
}

/*
*	This is init_module routine
*	This routine is responsible for deallocating
*	resources and stopping services/threads
*/
static void __exit nic_c_exit(void) {

    int i = 0;

    /* Stop main C-Module thread */
    kthread_stop(thread_st_c_model_worker);

    /* Signal per cpu response threads to exit */
    for (i=0; i<NUM_CPUS; i++) {
        exit_flag[i] = 'y';
        wake_up(&my_wait_queue[i]);
    }

    for (i=0; i<NUM_CPUS; i++) {
        kfree (response_queue_ptr[i]);
    }

    /* Print statistics */
    printk(KERN_ALERT "CMD Send => %d\n", num_cmd_send);
    printk(KERN_ALERT "CMD Receive C-Model => %d\n", num_cmd_rcv);
    printk(KERN_ALERT "Response Receive Driver=> %d\n", num_total_response);
}

module_init(nic_c_init);
module_exit(nic_c_exit);
