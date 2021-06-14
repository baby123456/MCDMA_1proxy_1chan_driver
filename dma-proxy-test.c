/* DMA Proxy Test Application
 *
 * This application is intended to be used with the DMA Proxy device driver. It provides
 * an example application showing how to use the device driver to do user space DMA
 * operations.
 *
 * The driver allocates coherent memory which is non-cached in a s/w coherent system.
 * Transmit and receive buffers in that memory are mapped to user space such that the
 * application can send and receive data using DMA channels (transmit and receive).
 *
 * It has been tested with an AXI DMA system with transmit looped back to receive.
 * Since the AXI DMA transmit is a stream without any buffering it is throttled until
 * the receive channel is running.  
 * 
 * More complete documentation is contained in the device driver (dma-proxy.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <string.h>
#include <inttypes.h>
#include "dma-proxy.h"

#define MEM_BASE_ADDR               0x4880000000
#define MEM_TOTAL_SIZE     (1 << 27)
static int fd = -1;
void *mem_map_base;
volatile uint8_t *mem_map_base_mem;

static struct dma_proxy_channel_interface *tx_proxy_interface_p;
static int tx_proxy_fd;
static int test_size; 

void init_map() {
    fd = open("/dev/mem", O_RDWR|O_SYNC);
    if (fd == -1){
        perror("init_map open failed:");
        exit(1);
    }

    //physical mapping to virtual memory
    mem_map_base = mmap(NULL, MEM_TOTAL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, MEM_BASE_ADDR);
                                    
    if (mem_map_base == NULL) {
        perror("init_map mmap failed:");
        close(fd);
        exit(1);
    }
    mem_map_base_mem = (uint8_t *)mem_map_base;
}
 
void finish_map() {
    mem_map_base_mem = NULL;
    munmap(mem_map_base, MEM_TOTAL_SIZE);
    mem_map_base = NULL;
    close(fd);
}


int tolower(int c){
    if (c >= 'A' && c <= 'Z')
        return c + 'a' - 'A';
    else
        return c;
}

int str2hex(char s[])
{
    int i;
    int n = 0;
    if (s[0] == '0' && (s[1]=='x' || s[1]=='X'))
        i = 2;
    else
        i = 0;
    for (; (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >='A' && s[i] <= 'Z');++i){
        if (tolower(s[i]) > '9')
            n = 16 * n + (10 + tolower(s[i]) - 'a');
        else
            n = 16 * n + (tolower(s[i]) - '0');
    }
    return n;
}


/* The following function is the transmit thread to allow the transmit and the
 * receive channels to be operating simultaneously. The ioctl calls are blocking
 * such that a thread is needed.
 */
void *tx_thread(int dma_count)
{
	int dummy, i, counter;

	/* Set up the length for the DMA transfer and initialize the transmit
 	 * buffer to a known pattern.
 	 */
	tx_proxy_interface_p->length = test_size;

	for (counter = 0; counter < dma_count; counter++) {
    		for (i = 0; i < test_size; i++)
       			tx_proxy_interface_p->buffer[i] = counter + i;

		/* Perform the DMA transfer and the check the status after it completes
	 	 * as the call blocks til the transfer is done.
 		 */
		ioctl(tx_proxy_fd, 0, &dummy);

		if (tx_proxy_interface_p->status != PROXY_NO_ERROR)
			printf("Proxy tx transfer error\n");
	}
}


int main(int argc, char *argv[])
{
	struct dma_proxy_channel_interface *rx_proxy_interface_p;
	int rx_proxy_fd, i;
	int dummy;
	int counter;
    struct timeval start, end;
    int time_diff, mb_sec;
    int num_transfer,verify=0;
    int memcpy_dma = 1;    //default dma

	printf("DMA proxy test\n");

	if (argc!=3 && argc!=4 && argc!=5) {
		printf("Usage: dma-proxy-test <# of DMA transfers to perform> <# of bytes in each transfer (< 3MB)>\n");
		exit(EXIT_FAILURE);
	}

	/* Get the size of the test to run, making sure it's not bigger than the statically configured memory size)
	 */
    num_transfer = atoi(argv[1]);
	//test_size = atoi(argv[2]);
    test_size = str2hex(argv[2]);
	if (test_size > TEST_SIZE)
		test_size = TEST_SIZE;

    if (argc >= 4)
        verify = atoi(argv[3]);
    printf("Verify = %d\n", verify);

    if(argc == 5){
        if (strcmp(argv[4], "nodma") == 0)
                memcpy_dma = 0;
        else if (strcmp(argv[4], "dma") == 0)
                memcpy_dma = 1;
    }

	/* Open the DMA proxy device for the transmit and receive channels
 	 */
	tx_proxy_fd = open("/dev/h2c", O_RDWR);

	if (tx_proxy_fd < 1) {
		printf("Unable to open DMA proxy device file");
		exit(EXIT_FAILURE);
	}

	rx_proxy_fd = open("/dev/c2h", O_RDWR);
	if (tx_proxy_fd < 1) {
		printf("Unable to open DMA proxy device file");
		exit(EXIT_FAILURE);
	}

	/* Map the transmit and receive channels memory into user space so it's accessible
 	 */
	tx_proxy_interface_p = (struct dma_proxy_channel_interface *)mmap(NULL, sizeof(struct dma_proxy_channel_interface),
									PROT_READ | PROT_WRITE, MAP_SHARED, tx_proxy_fd, 0);

	rx_proxy_interface_p = (struct dma_proxy_channel_interface *)mmap(NULL, sizeof(struct dma_proxy_channel_interface),
									PROT_READ | PROT_WRITE, MAP_SHARED, rx_proxy_fd, 0);
	if ((rx_proxy_interface_p == MAP_FAILED) || (tx_proxy_interface_p == MAP_FAILED)) {
		printf("Failed to mmap\n");
		exit(EXIT_FAILURE);
	}

	/* Set up the length for the DMA transfer and initialize the transmit buffer to a known pattern. Since
	 * transmit channel is using cyclic mode the same data will be received every time a transfer is done.
 	 */
	tx_proxy_interface_p->length = test_size; 

	for (i = 0; i < test_size; i++)
		tx_proxy_interface_p->buffer[i] = i;

	/* Create the thread for the transmit processing passing the number of transactions to it
	 */
	//pthread_create(&tid, NULL, tx_thread, atoi(argv[1]));
    init_map();
    gettimeofday( &start, NULL );
	for (counter = 0; counter < num_transfer; counter++) {
        tx_proxy_interface_p->length = test_size;

        if(verify){
            for (i = 0; i < test_size; i++)
                tx_proxy_interface_p->buffer[i] = counter + i;
		
            /* Initialize the receive buffer so that it can be verified after the transfer is done
            * and setup the size of the transfer for the receive channel
            */
	        for (i = 0; i < test_size; i++)
		        rx_proxy_interface_p->buffer[i] = 0;
        }
        rx_proxy_interface_p->length = test_size;
        
        if(memcpy_dma){
            /* Perform the DMA transfer and the check the status after it completes
	 	    * as the call blocks til the transfer is done.
 		    */
		    ioctl(tx_proxy_fd, 0, &dummy);
		    if (tx_proxy_interface_p->status != PROXY_NO_ERROR)
			    printf("Proxy tx transfer error,%d\n",tx_proxy_interface_p->status);

		    /* Perform a receive DMA transfer and after it finishes check the status
		    */
		    ioctl(rx_proxy_fd, 0, &dummy);
		    if (rx_proxy_interface_p->status != PROXY_NO_ERROR)
			    printf("Proxy rx transfer error,%d\n",rx_proxy_interface_p->status);
        }
        else{
            for (i = 0; i < test_size; i++)
                mem_map_base_mem[i] = tx_proxy_interface_p->buffer[i];
            for (i = 0; i < test_size; i++)
                rx_proxy_interface_p->buffer[i] = mem_map_base_mem[i];
        }

        /* Verify the data recieved matchs what was sent (tx is looped back to tx)
		*/
        if(verify){
		    for (i = 0; i < test_size; i++)
			    if (rx_proxy_interface_p->buffer[i] != (unsigned char)(counter + i))
				    printf("buffer not equal, index = %d, data = %d expected data = %d\n", i, 
					rx_proxy_interface_p->buffer[i], (unsigned char)(counter + i));
	            //printf("%dst test ok!\n",counter);
        }
    }
    gettimeofday( &end, NULL );
    finish_map();
    time_diff = 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec;
    mb_sec = ((1000000 / (double)time_diff) * (num_transfer*(double)test_size)) / 1000000;
    printf("Time: %d us\n", time_diff);
    printf("Time: %d ms\n", time_diff/1000);
    printf("Transfer size: %d KB\n", (long long)(num_transfer)*(test_size / 1024));
    printf("Throughput: %d MB / sec \n", mb_sec);

	/* Unmap the proxy channel interface memory and close the device files before leaving
	 */
	munmap(tx_proxy_interface_p, sizeof(struct dma_proxy_channel_interface));
	munmap(rx_proxy_interface_p, sizeof(struct dma_proxy_channel_interface));

	close(tx_proxy_fd);
	close(rx_proxy_fd);

	printf("DMA proxy test complete\n");

	return 0;
}
