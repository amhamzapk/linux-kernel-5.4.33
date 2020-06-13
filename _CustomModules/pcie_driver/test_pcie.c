#include<stdio.h>
#include<stdlib.h>
#include<errno.h>
#include<fcntl.h>
#include<string.h>
#include<unistd.h>

#define DEV_PREFIX  "/dev/"
#define DEV_POSTFIX "custom_pcie"
#define DEV_NAME DEV_PREFIX DEV_POSTFIX
#define BUFFER_LENGTH 256
static char user_buffer_read[BUFFER_LENGTH];
static char user_buffer_write[BUFFER_LENGTH];

int main()
{
	return 0;
}
