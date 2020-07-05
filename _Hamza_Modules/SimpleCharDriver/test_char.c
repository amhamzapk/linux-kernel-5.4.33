#include<stdio.h>
#include<stdlib.h>
#include<errno.h>
#include<fcntl.h>
#include<string.h>
#include<unistd.h>

#define DEV_PREFIX  "/dev/"
#define DEV_POSTFIX "hamza_char_device_simple"
#define DEV_NAME DEV_PREFIX DEV_POSTFIX
#define BUFFER_LENGTH 256
static char user_buffer_read[BUFFER_LENGTH];
static char user_buffer_write[BUFFER_LENGTH];

int main()
{
	int ret = -1;
	printf("Starting Linux test char driver\n");

	int fd = open("/dev/char_dev",	O_RDWR);
	if (fd < 0)
	{
		printf("Failed to open Character Device");
		return 0;
	}

	printf("Type in a short string to send to the kernel module:\n");
	scanf("%[^\n]%*c", user_buffer_write);
	printf("Writing message to the device [%s].\n", user_buffer_write);
	ret = write(fd, user_buffer_write, strlen(user_buffer_write)); // Send the string to the LKM
	if (ret < 0){
	  perror("Failed to write the message to the device.");
	  return errno;
	}

	printf("Press ENTER to read back from the device...\n");
	getchar();

	printf("Reading from the device...\n");
	ret = read(fd, user_buffer_read, BUFFER_LENGTH);        // Read the response from the LKM
	if (ret < 0){
	  perror("Failed to read the message from the device.");
	  return errno;
	}
	printf("The received message is: [%s]\n", user_buffer_read);
	printf("End of the program\n");

	return 0;
}
