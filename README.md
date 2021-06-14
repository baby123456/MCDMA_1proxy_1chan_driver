# dma-proxy
The driver code is written based on the xilinx wiki as below and it relays on the Linux kernel xilinx AXI MCDMA driver
* https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842418/Linux+DMA+From+User+Space

# The harware has 2 AXI MCDMA,each of wich has a MM2S DMA channel
* The DMA channel named `h2c` is for moving data from PS DDR to PL DDR
* The DMA channel named `c2h` is for moving data from PL DDR to PS DDR

# ARM dtb should contain a node pointing to 2 MM2S channels

dma_proxy_h2c {  
     compatible ="xlnx,dma_proxy";  
     dmas = <&axi_mcdma_PS_RD 0>;  
     dma-names = "h2c";  
}  

dma_proxy_c2h {  
     compatible ="xlnx,dma_proxy";  
     dmas = <&axi_mcdma_PL_RD 0>;  
     dma-names = "c2h";  
} 
# Notice
  becaue of the datamover btt field 23 bits limitation, although a AXI MCDMA buffer descripter support 26 bits(64MB) as well as a packet can have many bds, but all of the bds in a packet only have a SOF, so it can only contruct a command for datamover, therefore one time call device_prep_slave_sg can transfer not more than 8MB. 
# MCDMA_1proxy_1chan_driver
