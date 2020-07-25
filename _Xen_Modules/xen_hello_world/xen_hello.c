#include <linux/init.h>             // Macros used to mark up functions e.g., __init __exit
#include <linux/module.h>           // Core header for loading LKMs into the kernel
#include <linux/kernel.h>           // Contains types, macros, functions for the kernel
#include <linux/printk.h>

MODULE_LICENSE("GPL");              ///< The license type -- this affects runtime behavior
MODULE_AUTHOR("Ameer Hamza");      ///< The author -- visible when you use modinfo
MODULE_DESCRIPTION("Xen Hello Module");  ///< The description -- see modinfo
MODULE_VERSION("0.1");              ///< The version of the module

static int __init xen_hello_init(void){
   printk(KERN_INFO "Hello XEN!\n");
   return 0;
}

static void __exit xen_hello_exit(void){
   printk(KERN_INFO "Goodbye XEN!\n");
}

module_init(xen_hello_init);
module_exit(xen_hello_exit);

