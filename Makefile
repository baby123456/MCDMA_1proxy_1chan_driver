export ARCH=arm64
export CROSS_COMPILE=/opt/Xilinx_2019.1/SDK/2019.1/gnu/aarch64/lin/aarch64-linux/bin/aarch64-linux-gnu-
KERNEL_DIR=/home/ruanjinjie/NF_card_hw_sw_dev/software/arm-linux/phy_os
#KERNEL_DIR=/home/ruanjinjie/NF_card_hw_sw_dev/software/arm-linux/virt

all:
	make -C $(KERNEL_DIR) M=`pwd` modules
clean:
	rm -rf *.o *.order Module.symvers *.ko *.mod* .*.cmd *.cmd*
obj-m += dma-proxy.o
