KERNELDIR ?= /lib/modules/`uname -r`/build
PWD := $(shell pwd)
INSTALLDIR ?= ../
CROSS_COMPILE ?=
CC ?= $(CROSS_COMPILE)gcc

MODULE_NAME := uvcvideo_h264
RESMAIN_GLUE_OBJS := uvc_driver.o uvc_queue.o uvc_v4l2.o uvc_video.o uvc_ctrl.o uvc_status.o uvc_isight.o nalu.o
$(MODULE_NAME)-objs := $(RESMAIN_GLUE_OBJS)
obj-m := $(MODULE_NAME).o

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

install: all
	cp *.ko $(INSTALLDIR)

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.markers *.symvers *.order

.PHONY:
	modules modules_install clean

