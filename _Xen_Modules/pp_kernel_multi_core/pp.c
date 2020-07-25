#include <linux/init.h>             // Macros used to mark up functions e.g., __init __exit
#include <linux/module.h>           // Core header for loading LKMs into the kernel
#include <linux/kernel.h>           // Contains types, macros, functions for the kernel
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

MODULE_LICENSE("GPL");              ///< The license type -- this affects runtime behavior
MODULE_AUTHOR("Ameer Hamza");      ///< The author -- visible when you use modinfo
MODULE_DESCRIPTION("Xen Hello Module");  ///< The description -- see modinfo
MODULE_VERSION("0.1");              ///< The version of the module

static struct task_struct *thread_prime;
//static struct task_struct *thread_probe;

/*
*	Get CPU Cycles from Read RDTSC Function
*/
static inline u64 read_rdtsc(void) {
    u64 lo, hi;

    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi) :: "ecx" );

    return (u64) ((hi << 32) | lo);
}
static void rdtscp_coreid(unsigned int* aux)
{
    // For IA32
    unsigned long long x;
    asm volatile("rdtscp" : "=c" (*aux), "=A" (x) ::);
    //return x;
}
/*
*	--Main NIC-C Model Thread--
*	This thread is responsible for scheduling request
*	as soon some element is push into the queue
*/
static int prime_thread(void *unused) {

	/* Local variables to keep track of CPU Cycles */
    u64 clk_cycles_start = 0;
    u64 clk_cycles_end = 0;
    u64 clk_cycles_exp = 0;
    u64 clk_cycles_div = 500;
    volatile int temp_cnt = 0;

    printk(KERN_ALERT "C-Model Thread binded to Core # %d\n", get_cpu());

    /* Run until module is not unloaded */
    while (!kthread_should_stop()) {


		unsigned int  cpuid = -1;
		rdtscp_coreid(&cpuid);
		printk (KERN_ALERT "%d\n", cpuid);

		/* Keep track of RDTSC */
        if (!clk_cycles_start)
            clk_cycles_start = read_rdtsc();

        clk_cycles_end = read_rdtsc ();

        clk_cycles_exp = (clk_cycles_end - clk_cycles_start);

        /* Check if queue needs to be processed */
        if ((clk_cycles_exp/clk_cycles_div) >= 1) {
        	// DO PRIME HERE

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

    return 0;
}
static int __init pp_init(void){
   printk(KERN_INFO "Init PP!\n");
   /* Bind C-Model worker thread to the last core */
   thread_prime = kthread_create(prime_thread, NULL, "kthread_c_model_worker");
   kthread_bind(thread_prime, 3);
   wake_up_process(thread_prime);
   return 0;
}

static void __exit pp_exit(void){
	/* Stop main C-Module thread */
    kthread_stop(thread_prime);
	printk(KERN_INFO "Exit PP!\n");
}

module_init(pp_init);
module_exit(pp_exit);

