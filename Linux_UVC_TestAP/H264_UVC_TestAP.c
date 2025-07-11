/*
 *      H.264 USB Video Class test application
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/videodev2.h>
#include <linux/version.h>
#include <sys/utsname.h>
#include <pthread.h>

#include "v4l2uvc.h"
#include "h264_xu_ctrls.h"
#include "nalu.h"
#include "debug.h"

#define TESTAP_VERSION		"v1.0.14.0_H264_UVC_TestAP_Multi"

#define V4L2_PIX_FMT_H264 v4l2_fourcc('H','2','6','4') /* H264 */
#define V4L2_PIX_FMT_MP2T v4l2_fourcc('M','P','2','T') /* MPEG-2 TS */

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) ((a)<<16+(b)<<8+(c))
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION (2,6,32)
#define V4L_BUFFERS_DEFAULT	6//16
#define V4L_BUFFERS_MAX		16//32
#else
#define V4L_BUFFERS_DEFAULT	3
#define V4L_BUFFERS_MAX		3
#endif

#define H264_SIZE_HD				((1280<<16)|720)
#define H264_SIZE_VGA				((640<<16)|480)
#define H264_SIZE_QVGA				((320<<16)|240)
#define H264_SIZE_QQVGA				((160<<16)|112)

#define MULTI_STREAM_HD_QVGA		1
#define MULTI_STREAM_HD_QQVGA		2
#define MULTI_STREAM_HD_QVGA_QQVGA	4
#define MULTI_STREAM_HD_VGA			8
#define MULTI_STREAM_HD_VGA_QVGA	16

struct H264Format *gH264fmt = NULL;
int Dbg_Param = 0xf;

struct thread_parameter
{
	struct v4l2_buffer *buf;
	void *mem[16];
	int *dev;
	unsigned int *nframes ;
	unsigned char multi_stream_mjpg_enable;
};

#if 0
static void pantilt(int dev, char *dir, char *length)
{
	struct v4l2_ext_control xctrls[2];
	struct v4l2_ext_controls ctrls;
	unsigned int angle = atoi(length);

	char directions[9][2] = {
		{ -1,  1 },
		{  0,  1 },
		{  1,  1 },
		{ -1,  0 },
		{  0,  0 },
		{  1,  0 },
		{ -1, -1 },
		{  0, -1 },
		{  1, -1 },
	};

	if (dir[0] == '5') {
		xctrls[0].id = V4L2_CID_PANTILT_RESET;
		xctrls[0].value = angle;

		ctrls.count = 1;
		ctrls.controls = xctrls;
	} else {
		xctrls[0].id = V4L2_CID_PAN_RELATIVE;
		xctrls[0].value = directions[dir[0] - '1'][0] * angle;
		xctrls[1].id = V4L2_CID_TILT_RELATIVE;
		xctrls[1].value = directions[dir[0] - '1'][1] * angle;

		ctrls.count = 2;
		ctrls.controls = xctrls;
	}

	ioctl(dev, VIDIOC_S_EXT_CTRLS, &ctrls);
}
#endif

static int CheckKernelVersion(void)
{
	struct utsname KernelInfo;
	int kernelRelease = 0, version[3];
	char *endptr;
	uname(&KernelInfo);
	//TestAp_Printf(TESTAP_DBG_FLOW, "kernel version:%s,0x%x \n",KernelInfo.release, LINUX_VERSION_CODE);
	version[0]= strtol(KernelInfo.release, &endptr, 16);
	version[1]= strtol(endptr+1, &endptr, 16);
	version[2]= strtol(endptr+1, &endptr, 16);
	kernelRelease = (version[0]<<16) |  (version[1]<<8) | version[2];
	
	if((kernelRelease&0xffff00) == (LINUX_VERSION_CODE&0xffff00))		//value of LINUX_VERSION_CODE is 2.6.24 in 2.6.36.4 kernel
		return true;
	else
	{
		TestAp_Printf(TESTAP_DBG_ERR, "your kernel version: 0x%x \nTestAP support kernel version: 0x%x\n",kernelRelease, LINUX_VERSION_CODE);
		return false;
	}

}
static int GetFreeRam(int* freeram)
{
    FILE *meminfo = fopen("/proc/meminfo", "r");
	char line[256];
    if(meminfo == NULL)
	{
		TestAp_Printf(TESTAP_DBG_ERR, "/proc/meminfo can't open\n");
        return 0;
	}
    while(fgets(line, sizeof(line), meminfo))
    {
        if(sscanf(line, "MemFree: %d kB", freeram) == 1)
        {
			*freeram <<= 10;
            fclose(meminfo);
            return 1;
        }
    }
	
    fclose(meminfo);
    return 0;
}

static int video_open(const char *devname)
{
	struct v4l2_capability cap;
	int dev, ret;

	dev = open(devname, O_RDWR);
	if (dev < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Error opening device %s: %d.\n", devname, errno);
		return dev;
	}

	memset(&cap, 0, sizeof cap);
	ret = ioctl(dev, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Error opening device %s: unable to query device.\n",
			devname);
		close(dev);
		return ret;
	}

#if 0
	if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Error opening device %s: video capture not supported.\n",
			devname);
		close(dev);
		return -EINVAL;
	}
#endif

	TestAp_Printf(TESTAP_DBG_FLOW, "Device %s opened: %s.\n", devname, cap.card);
	return dev;
}

static void uvc_set_control(int dev, unsigned int id, int value)
{
	struct v4l2_control ctrl;
	int ret;

	ctrl.id = id;
	ctrl.value = value;

	ret = ioctl(dev, VIDIOC_S_CTRL, &ctrl);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "unable to set gain control: %s (%d).\n",
			strerror(errno), errno);
		return;
	}
}

static int video_set_format(int dev, unsigned int w, unsigned int h, unsigned int format)
{
	struct v4l2_format fmt;
	int ret;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = format;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	ret = ioctl(dev, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to set format: %d.\n", errno);
		return ret;
	}

	TestAp_Printf(TESTAP_DBG_FLOW, "Video format set: width: %u height: %u buffer size: %u\n",
		fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);
	return 0;
}

static int video_set_framerate(int dev, int framerate)
{
	struct v4l2_streamparm parm;
	int ret;

	memset(&parm, 0, sizeof parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(dev, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to get frame rate: %d.\n", errno);
		return ret;
	}

	TestAp_Printf(TESTAP_DBG_FLOW, "Current frame rate: %u/%u\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);

	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = framerate;

	ret = ioctl(dev, VIDIOC_S_PARM, &parm);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to set frame rate: %d.\n", errno);
		return ret;
	}

	ret = ioctl(dev, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to get frame rate: %d.\n", errno);
		return ret;
	}

	TestAp_Printf(TESTAP_DBG_FLOW, "Frame rate set: %u/%u\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);
	return 0;
}

static int video_reqbufs(int dev, int nbufs)
{
	struct v4l2_requestbuffers rb;
	int ret;

	memset(&rb, 0, sizeof rb);
	rb.count = nbufs;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(dev, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to allocate buffers: %d.\n", errno);
		return ret;
	}

	TestAp_Printf(TESTAP_DBG_FLOW, "%u buffers allocated.\n", rb.count);
	return rb.count;
}

static int video_enable(int dev, int enable)
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int ret;

	ret = ioctl(dev, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to %s capture: %d.\n",
			enable ? "start" : "stop", errno);
		return ret;
	}

	return 0;
}

static void video_query_menu(int dev, unsigned int id)
{
	struct v4l2_querymenu menu;
	int ret;

	menu.index = 0;
	while (1) {
		menu.id = id;
		ret = ioctl(dev, VIDIOC_QUERYMENU, &menu);
		if (ret < 0)
			break;

		TestAp_Printf(TESTAP_DBG_FLOW, "  %u: %.32s\n", menu.index, menu.name);
		menu.index++;
	};
}

static void video_list_controls(int dev)
{
	struct v4l2_queryctrl query;
	struct v4l2_control ctrl;
	char value[12];
	int ret;

#ifndef V4L2_CTRL_FLAG_NEXT_CTRL
	unsigned int i;

	for (i = V4L2_CID_BASE; i <= V4L2_CID_LASTP1; ++i) {
		query.id = i;
#else
	query.id = 0;
	while (1) {
		query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
#endif
		ret = ioctl(dev, VIDIOC_QUERYCTRL, &query);
		if (ret < 0)
			break;

		if (query.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;

		ctrl.id = query.id;
		ret = ioctl(dev, VIDIOC_G_CTRL, &ctrl);
		if (ret < 0)
			strcpy(value, "n/a");
		else
			sprintf(value, "%d", ctrl.value);

		TestAp_Printf(TESTAP_DBG_FLOW, "control 0x%08x %s min %d max %d step %d default %d current %s.\n",
			query.id, query.name, query.minimum, query.maximum,
			query.step, query.default_value, value);

		if (query.type == V4L2_CTRL_TYPE_MENU)
			video_query_menu(dev, query.id);

	}
}

static void video_enum_inputs(int dev)
{
	struct v4l2_input input;
	unsigned int i;
	int ret;

	for (i = 0; ; ++i) {
		memset(&input, 0, sizeof input);
		input.index = i;
		ret = ioctl(dev, VIDIOC_ENUMINPUT, &input);
		if (ret < 0)
			break;

		if (i != input.index)
			TestAp_Printf(TESTAP_DBG_FLOW, "Warning: driver returned wrong input index "
				"%u.\n", input.index);

		TestAp_Printf(TESTAP_DBG_FLOW, "Input %u: %s.\n", i, input.name);
	}
}

static int video_get_input(int dev)
{
	__u32 input;
	int ret;

	ret = ioctl(dev, VIDIOC_G_INPUT, &input);
	if (ret < 0) {
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to get current input: %s.\n", strerror(errno));
		return ret;
	}

	return input;
}

static int video_set_input(int dev, unsigned int input)
{
	__u32 _input = input;
	int ret;

	ret = ioctl(dev, VIDIOC_S_INPUT, &_input);
	if (ret < 0)
		TestAp_Printf(TESTAP_DBG_ERR, "Unable to select input %u: %s.\n", input,
			strerror(errno));

	return ret;
}

static void usage(const char *argv0)
{
	TestAp_Printf(TESTAP_DBG_USAGE, "Usage: %s [options] device\n", argv0);
	TestAp_Printf(TESTAP_DBG_USAGE, "Supported options:\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-c, --capture[=nframes]	Capture frames\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-d, --delay		Delay (in ms) before requeuing buffers\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-f, --format format	Set the video format (mjpg or yuyv)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-h, --help		Show this help screen\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-i, --input input	Select the video input\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-l, --list-controls	List available controls\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-n, --nbufs n		Set the number of video buffers\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-s, --size WxH		Set the frame size\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "    --fr framerate	Set framerate\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-S, --save		Save captured images to disk\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "    --enum-inputs	Enumerate inputs\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "    --skip n		Skip the first n frames\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-r, --record		Record H264 file\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "--bri-set values	Set brightness values\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "--bri-get		Get brightness values\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "--shrp-set values	Set sharpness values\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "--shrp-get		Get sharpness values\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "--dbg value		Set level of debug message(bit0:usage, bit1:error, bit2:flow, bit3:frame)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "RERVISION XU supported options:\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "-a,  --add-xuctrl			Add Extension Unit Ctrl into Driver\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget id cs datasize d0 d1 ...	XU Get command: xu_id control_selector data_size data_0 data_1 ...\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset id cs datasize d0 d1 ...	XU Set command: xu_id control_selector data_size data_0 data_1 ...\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-chip			Read RERVISION Chip ID\n");
#if(0)
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-fmt			List H.264 format\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-fmt fmt-fps		Set H.264 format - fps index\n");
#endif
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-qp				Get H.264 QP values\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-qp val			Set H.264 QP values: val\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-br				Get H.264 bit rate (bps)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-br val			Set H.264 bit rate (bps) \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --asic-r	addr			[Hex] Read register address data\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --asic-w	addr data		[Hex] Write register address data\n");
#if(CARCAM_PROJECT == 0)
	TestAp_Printf(TESTAP_DBG_USAGE, "     --mf val			Set Multi-Stream format:[1]HD+QVGA [2]HD+QQVGA [4]HD+QVGA+QQVGA [8]HD+VGA [16]HD+VGA+QVGA \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --mgs				Get Multi-Stream Status. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --mgi				Get Multi-Stream Info. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --msqp StreamID QP    Set Multi-Stream QP. StreamID = 0 ~ 2 \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --mgqp StreamID	   Get Multi-Stream QP. StreamID = 0 ~ 2 \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --msbr StreamID Bitrate         	Set Multi-Stream Bitrate (bps). StreamID = 0 ~ 2 \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --mgbr StreamID			Get Multi-Stream BitRate (bps). StreamID = 0 ~ 2 \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --mse Enable		Set Multi-Stream Enable : [0]Disable [1]H264  [3]H264+Mjpg. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --mge				Get Multi-Stream Enable. \n");
#endif
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-timer Enable		Set OSD Timer Counting  1:enable   0:disable\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-rtc year month day hour min sec	Set OSD RTC\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-rtc 			Get OSD RTC\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-os Line Block 		Set OSD Line and Block Size (0~4)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-os				Get OSD Line and Block Size (0~4)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-oc Font Border            	Set OSD Font and Border Color   0:Black  1:Red  2:Green  3:Blue  4:White\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-oc				Get OSD Font and Border Color   0:Black  1:Red  2:Green  3:Blue  4:White\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-oe Line Block		Set OSD Show  1:enable  0:disable\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-oe				Get OSD Show  1:enable  0:disable\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-oas Line Block		Set OSD Auto Scale  1:enable  0:disable\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-oas			Get OSD Auto Scale  1:enable  0:disable\n");
#if(CARCAM_PROJECT == 0)	
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-oms Stream0 Stream1 Stream2	Set OSD MultiStream Size  (0~4)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-oms			Get OSD MultiStream Size  (0~4)\n");
#endif	
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-osp Type Row Col		Set OSD Start Row and Col (unit:16)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-osp			Get OSD Start Row and Col (unit:16)\n");
#if(CARCAM_PROJECT == 0)
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-ostr Group '.....'		Set OSD 2nd String.Group from 0 to 2.8 words per 1 Group.\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-ostr Group 		Get OSD 2nd String. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-omssp StreamID Row Col	Set OSD Multi stream start row and col. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-omssp			Get OSD Multi stream start raw and col. \n");
#endif
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-mde Enable			Set Motion detect enable\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-mde			Get Motion detect enable\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-mdt Thd			Set Motion detect threshold (0~65535)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-mdt			Get Motion detect threshold\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-mdm  m1 m2 ... m24		Set Motion detect mask\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-mdm			Get Motion detect mask\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-mdr  m1 m2 ... m24		Set Motion detect result\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-mdr			Get Motion detect result\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-mjb Bitrate		Set MJPG Bitrate (bps) \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-mjb			Get MJPG Bitrate (bps) \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-if	nframe			Set H264 reset to IFrame.  nframe : reset per nframe.\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-sei			Set H264 SEI Header Enable.\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-sei			Get H264 SEI Header Enable. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-gop			Set H264 GOP. (1 ~ 4095)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-gop			Get H264 GOP. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-cvm			Set H264 CBR/VBR mode(1:CBR 2:VBR)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-cvm			Get H264 CBR/VBR mode(1:CBR 2:VBR)\n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-mir			Set Image mirror. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-mir			Get Image mirror. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-flip			Set Image flip. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-flip			Get Image flip. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-gpio enable out_value      Set GPIO ctrl(hex). \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-gpio                       Get GPIO ctrl. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-clr			Set Image color. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-clr			Get Image color. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-fde s1 s2			Set Frame drop enable. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-fde			Get Frame drop enable. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-fdc s1 s2			Set Frame drop value. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-fdc			Get Frame drop value. \n");
	
#if(CARCAM_PROJECT == 1)
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-car SpeedEn CoordinateEn CoordinateCtrl		Set Car control . \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-car			Get Car control. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-spd Speed			Set Speed info. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-spd			Get Speed info. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-coor1 Dir v1 v2 v3 v4 v5 v6	Set Coordinate info. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-coor1			Get Coordinate info. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuset-coor2 Dir v1 v2 v3 v4	Set Coordinate info. \n");
	TestAp_Printf(TESTAP_DBG_USAGE, "     --xuget-coor2			Get Coordinate info. \n");
#endif
}

#define OPT_ENUM_INPUTS			256
#define OPT_SKIP_FRAMES			OPT_ENUM_INPUTS + 1
#define OPT_XU_ADD_CTRL			OPT_ENUM_INPUTS + 2
#define OPT_XU_GET_CHIPID		OPT_ENUM_INPUTS + 3
#define OPT_XU_GET_FMT 			OPT_ENUM_INPUTS + 4
#define OPT_XU_SET_FMT 			OPT_ENUM_INPUTS + 5
#define OPT_XU_SET_FPS 			OPT_ENUM_INPUTS + 6
#define OPT_XU_GET_QP			OPT_ENUM_INPUTS + 7
#define OPT_XU_SET_QP			OPT_ENUM_INPUTS + 8
#define OPT_XU_GET_BITRATE		OPT_ENUM_INPUTS + 9
#define OPT_XU_SET_BITRATE		OPT_ENUM_INPUTS + 10
#define OPT_XU_GET				OPT_ENUM_INPUTS + 11
#define OPT_XU_SET				OPT_ENUM_INPUTS + 12
#define OPT_BRIGHTNESS_GET		OPT_ENUM_INPUTS + 13
#define OPT_BRIGHTNESS_SET		OPT_ENUM_INPUTS + 14
#define OPT_SHARPNESS_GET		OPT_ENUM_INPUTS + 15
#define OPT_SHARPNESS_SET		OPT_ENUM_INPUTS + 16
#define OPT_ASIC_READ			OPT_ENUM_INPUTS + 17
#define OPT_ASIC_WRITE			OPT_ENUM_INPUTS + 18
#define OPT_MULTI_FORMAT		OPT_ENUM_INPUTS + 19
#define OPT_MULTI_SET_BITRATE	OPT_ENUM_INPUTS + 20
#define OPT_MULTI_GET_BITRATE	OPT_ENUM_INPUTS + 21
#define OPT_OSD_TIMER_CTRL_SET	OPT_ENUM_INPUTS + 22
#define OPT_OSD_RTC_SET			OPT_ENUM_INPUTS + 23
#define OPT_OSD_RTC_GET			OPT_ENUM_INPUTS + 24
#define OPT_OSD_SIZE_SET		OPT_ENUM_INPUTS + 25
#define OPT_OSD_SIZE_GET		OPT_ENUM_INPUTS + 26
#define OPT_OSD_COLOR_SET		OPT_ENUM_INPUTS + 27
#define OPT_OSD_COLOR_GET		OPT_ENUM_INPUTS + 28
#define OPT_OSD_SHOW_SET		OPT_ENUM_INPUTS + 29
#define OPT_OSD_SHOW_GET		OPT_ENUM_INPUTS + 30
#define OPT_OSD_AUTOSCALE_SET	OPT_ENUM_INPUTS + 31
#define OPT_OSD_AUTOSCALE_GET	OPT_ENUM_INPUTS + 32
#define OPT_OSD_MS_SIZE_SET		OPT_ENUM_INPUTS + 33
#define OPT_OSD_MS_SIZE_GET		OPT_ENUM_INPUTS + 34
#define OPT_OSD_POSITION_SET	OPT_ENUM_INPUTS + 35
#define OPT_OSD_POSITION_GET	OPT_ENUM_INPUTS + 36
#define OPT_MD_MODE_SET			OPT_ENUM_INPUTS + 37
#define OPT_MD_MODE_GET			OPT_ENUM_INPUTS + 38
#define OPT_MD_THRESHOLD_SET	OPT_ENUM_INPUTS + 39
#define OPT_MD_THRESHOLD_GET	OPT_ENUM_INPUTS + 40
#define OPT_MD_MASK_SET			OPT_ENUM_INPUTS + 41
#define OPT_MD_MASK_GET			OPT_ENUM_INPUTS + 42
#define OPT_MD_RESULT_SET		OPT_ENUM_INPUTS + 43
#define OPT_MD_RESULT_GET		OPT_ENUM_INPUTS + 44
#define OPT_MJPG_BITRATE_SET	OPT_ENUM_INPUTS + 45
#define OPT_MJPG_BITRATE_GET	OPT_ENUM_INPUTS + 46
#define OPT_H264_IFRAME_SET		OPT_ENUM_INPUTS + 47
#define OPT_H264_SEI_SET		OPT_ENUM_INPUTS + 48
#define OPT_H264_SEI_GET		OPT_ENUM_INPUTS + 49
#define OPT_IMG_MIRROR_SET		OPT_ENUM_INPUTS + 50
#define OPT_IMG_MIRROR_GET		OPT_ENUM_INPUTS + 51
#define OPT_IMG_FLIP_SET		OPT_ENUM_INPUTS + 52
#define OPT_IMG_FLIP_GET		OPT_ENUM_INPUTS + 53
#define OPT_IMG_COLOR_SET		OPT_ENUM_INPUTS + 54
#define OPT_IMG_COLOR_GET		OPT_ENUM_INPUTS + 55
#define OPT_OSD_STRING_SET		OPT_ENUM_INPUTS + 56
#define OPT_OSD_STRING_GET		OPT_ENUM_INPUTS + 57
#define OPT_MULTI_GET_STATUS	OPT_ENUM_INPUTS + 58
#define OPT_MULTI_GET_INFO		OPT_ENUM_INPUTS + 59
#define OPT_MULTI_SET_ENABLE	OPT_ENUM_INPUTS + 60
#define OPT_MULTI_GET_ENABLE	OPT_ENUM_INPUTS + 61
#define OPT_MULTI_SET_QP		OPT_ENUM_INPUTS + 62
#define OPT_MULTI_GET_QP		OPT_ENUM_INPUTS + 63
#define OPT_H264_GOP_SET		OPT_ENUM_INPUTS + 64
#define OPT_H264_GOP_GET		OPT_ENUM_INPUTS + 65
#define OPT_H264_MODE_SET		OPT_ENUM_INPUTS + 66
#define OPT_H264_MODE_GET		OPT_ENUM_INPUTS + 67
#define OPT_OSD_MS_POSITION_SET	OPT_ENUM_INPUTS + 68
#define OPT_OSD_MS_POSITION_GET	OPT_ENUM_INPUTS + 69
#define OPT_OSD_CARCAM_SET		OPT_ENUM_INPUTS + 70
#define OPT_OSD_CARCAM_GET		OPT_ENUM_INPUTS + 71
#define OPT_OSD_SPEED_SET		OPT_ENUM_INPUTS + 72
#define OPT_OSD_SPEED_GET		OPT_ENUM_INPUTS + 73
#define OPT_OSD_COORDINATE_SET1	OPT_ENUM_INPUTS + 74
#define OPT_OSD_COORDINATE_SET2	OPT_ENUM_INPUTS + 75
#define OPT_OSD_COORDINATE_GET1	OPT_ENUM_INPUTS + 76
#define OPT_OSD_COORDINATE_GET2	OPT_ENUM_INPUTS + 77
#define OPT_GPIO_CTRL_SET   	OPT_ENUM_INPUTS + 78
#define OPT_GPIO_CTRL_GET   	OPT_ENUM_INPUTS + 79
#define OPT_FRAMERATE			OPT_ENUM_INPUTS + 80
#define OPT_FRAME_DROP_EN_SET	OPT_ENUM_INPUTS + 81
#define OPT_FRAME_DROP_EN_GET	OPT_ENUM_INPUTS + 82
#define OPT_FRAME_DROP_CTRL_SET	OPT_ENUM_INPUTS + 83
#define OPT_FRAME_DROP_CTRL_GET	OPT_ENUM_INPUTS + 84
#define OPT_DEBUG_LEVEL			OPT_ENUM_INPUTS + 85

static struct option opts[] = {
	{"capture", 2, 0, 'c'},
	{"delay", 1, 0, 'd'},
	{"enum-inputs", 0, 0, OPT_ENUM_INPUTS},
	{"format", 1, 0, 'f'},
	{"help", 0, 0, 'h'},
	{"input", 1, 0, 'i'},
	{"list-controls", 0, 0, 'l'},
	{"save", 0, 0, 'S'},
	{"size", 1, 0, 's'},
	{"fr", 1, 0, OPT_FRAMERATE},
	{"skip", 1, 0, OPT_SKIP_FRAMES},
	{"record", 0, 0, 'r'},
	{"bri-get", 0, 0, OPT_BRIGHTNESS_GET},
	{"bri-set", 1, 0, OPT_BRIGHTNESS_SET},
	{"shrp-get", 0, 0, OPT_SHARPNESS_GET},
	{"shrp-set", 1, 0, OPT_SHARPNESS_SET},
	{"add-xuctrl", 0, 0, OPT_XU_ADD_CTRL},
	{"xuget", 1, 0, OPT_XU_GET},
	{"xuset", 1, 0, OPT_XU_SET},
	{"xuget-chip", 0, 0, OPT_XU_GET_CHIPID},
	{"xuget-fmt", 0, 0, OPT_XU_GET_FMT},
	{"xuset-fmt", 1, 0, OPT_XU_SET_FMT},
	{"xuget-qp", 0, 0, OPT_XU_GET_QP},
	{"xuset-qp", 1, 0, OPT_XU_SET_QP},
	{"xuget-br", 0, 0, OPT_XU_GET_BITRATE},
	{"xuset-br", 1, 0, OPT_XU_SET_BITRATE},
	{"asic-r", 1, 0, OPT_ASIC_READ},
	{"asic-w", 1, 0, OPT_ASIC_WRITE},
	{"msqp", 1, 0, OPT_MULTI_SET_QP},
	{"mgqp", 1, 0, OPT_MULTI_GET_QP},
	{"mf", 1, 0, OPT_MULTI_FORMAT},
	{"msbr", 1, 0, OPT_MULTI_SET_BITRATE},
	{"mgbr", 1, 0, OPT_MULTI_GET_BITRATE},
	{"mgs", 0, 0, OPT_MULTI_GET_STATUS},
	{"mgi", 0, 0, OPT_MULTI_GET_INFO},
	{"mse", 1, 0, OPT_MULTI_SET_ENABLE},
	{"mge", 0, 0, OPT_MULTI_GET_ENABLE},
	{"xuset-timer", 1, 0, OPT_OSD_TIMER_CTRL_SET},
	{"xuset-rtc", 1, 0, OPT_OSD_RTC_SET},
	{"xuget-rtc", 0, 0, OPT_OSD_RTC_GET},
	{"xuset-os", 1, 0, OPT_OSD_SIZE_SET},
	{"xuget-os", 0, 0, OPT_OSD_SIZE_GET},
	{"xuset-oc", 1, 0, OPT_OSD_COLOR_SET},
	{"xuget-oc", 0, 0, OPT_OSD_COLOR_GET},
	{"xuset-oe", 1, 0, OPT_OSD_SHOW_SET},
	{"xuget-oe", 0, 0, OPT_OSD_SHOW_GET},
	{"xuset-oas", 1, 0, OPT_OSD_AUTOSCALE_SET},
	{"xuget-oas", 0, 0, OPT_OSD_AUTOSCALE_GET},
	{"xuset-oms", 1, 0, OPT_OSD_MS_SIZE_SET},
	{"xuget-oms", 0, 0, OPT_OSD_MS_SIZE_GET},
	{"xuset-ostr", 1, 0, OPT_OSD_STRING_SET},
	{"xuget-ostr", 1, 0, OPT_OSD_STRING_GET},
	{"xuset-osp", 1, 0, OPT_OSD_POSITION_SET},
	{"xuget-osp", 0, 0, OPT_OSD_POSITION_GET},
	{"xuset-omssp", 1, 0, OPT_OSD_MS_POSITION_SET},
	{"xuget-omssp", 0, 0, OPT_OSD_MS_POSITION_GET},
	{"xuset-mde", 1, 0, OPT_MD_MODE_SET},
	{"xuget-mde", 0, 0, OPT_MD_MODE_GET},
	{"xuset-mdt", 1, 0, OPT_MD_THRESHOLD_SET},
	{"xuget-mdt", 0, 0, OPT_MD_THRESHOLD_GET},
	{"xuset-mdm", 1, 0, OPT_MD_MASK_SET},
	{"xuget-mdm", 0, 0, OPT_MD_MASK_GET},
	{"xuset-mdr", 1, 0, OPT_MD_RESULT_SET},
	{"xuget-mdr", 0, 0, OPT_MD_RESULT_GET},
	{"xuset-mjb", 1, 0, OPT_MJPG_BITRATE_SET},
	{"xuget-mjb", 0, 0, OPT_MJPG_BITRATE_GET},
	{"xuset-if", 1, 0, OPT_H264_IFRAME_SET},
	{"xuset-sei", 1, 0, OPT_H264_SEI_SET},
	{"xuget-sei", 0, 0, OPT_H264_SEI_GET},
	{"xuset-gop", 1, 0, OPT_H264_GOP_SET},
	{"xuget-gop", 0, 0, OPT_H264_GOP_GET},
	{"xuset-cvm", 1, 0, OPT_H264_MODE_SET},
	{"xuget-cvm", 0, 0, OPT_H264_MODE_GET},
	{"xuset-mir", 1, 0, OPT_IMG_MIRROR_SET},
	{"xuget-mir", 0, 0, OPT_IMG_MIRROR_GET},
	{"xuset-flip", 1, 0, OPT_IMG_FLIP_SET},
	{"xuget-flip", 0, 0, OPT_IMG_FLIP_GET},
	{"xuset-clr", 1, 0, OPT_IMG_COLOR_SET},
	{"xuget-clr", 0, 0, OPT_IMG_COLOR_GET},
	{"xuset-car", 1, 0, OPT_OSD_CARCAM_SET},
	{"xuget-car", 0, 0, OPT_OSD_CARCAM_GET},
	{"xuset-spd", 1, 0, OPT_OSD_SPEED_SET},
	{"xuget-spd", 0, 0, OPT_OSD_SPEED_GET},
	{"xuset-coor1", 1, 0, OPT_OSD_COORDINATE_SET1},
	{"xuset-coor2", 1, 0, OPT_OSD_COORDINATE_SET2},
	{"xuget-coor1", 0, 0, OPT_OSD_COORDINATE_GET1},
	{"xuget-coor2", 0, 0, OPT_OSD_COORDINATE_GET2},
	{"xuset-gpio", 1, 0, OPT_GPIO_CTRL_SET},
	{"xuget-gpio", 0, 0, OPT_GPIO_CTRL_GET},
	{"xuset-fde", 1, 0, OPT_FRAME_DROP_EN_SET},
	{"xuget-fde", 0, 0, OPT_FRAME_DROP_EN_GET},
	{"xuset-fdc", 1, 0, OPT_FRAME_DROP_CTRL_SET},
	{"xuget-fdc", 0, 0, OPT_FRAME_DROP_CTRL_GET},
	{"dbg", 1, 0, OPT_DEBUG_LEVEL},
	{0, 0, 0, 0}
};

void *thread_capture(void *par)
{

	unsigned int mjpg_resolution = 0;//(width << 16) | (height);
	unsigned int mjpg_width = 0;
	unsigned int mjpg_height = 0;
	unsigned int unknow_size = 0;
	unsigned int skip = 0;
	struct thread_parameter thread_par = * (struct thread_parameter*) par;
	int i = 0;
	int ret;
	FILE *file = NULL;
	char filename[] = "";
/*	
	TestAp_Printf(TESTAP_DBG_FLOW, "  +++ thread_par.dev = %d \n",*thread_par.dev);
	TestAp_Printf(TESTAP_DBG_FLOW, "  +++ thread_par.nframes = %d \n",*thread_par.nframes);
	TestAp_Printf(TESTAP_DBG_FLOW, "  +++ thread_par.multi_stream_mjpg_enable = %d \n",(unsigned int)thread_par.multi_stream_mjpg_enable);
	TestAp_Printf(TESTAP_DBG_FLOW, "  +++ thread_par.mem = 0x%x \n",thread_par.mem[0]);
	TestAp_Printf(TESTAP_DBG_FLOW, "  +++ thread_par.mem = 0x%x \n",thread_par.mem[1]);
	TestAp_Printf(TESTAP_DBG_FLOW, "  +++ thread_par.mem = 0x%x \n",thread_par.mem[2]);
	TestAp_Printf(TESTAP_DBG_FLOW, "  +++ thread_par.mem = 0x%x \n",thread_par.mem[3]);
*/
	if(thread_par.multi_stream_mjpg_enable)
	{
		skip = 6;
	}
	
	for (i = 0; i < *thread_par.nframes; ++i) 
	{
		unknow_size = 0;
		
		/* Dequeue a buffer. */
		memset(thread_par.buf, 0, sizeof *thread_par.buf);
		thread_par.buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		thread_par.buf->memory = V4L2_MEMORY_MMAP;
		ret = ioctl(*thread_par.dev, VIDIOC_DQBUF, thread_par.buf);
		if (ret < 0) {
			TestAp_Printf(TESTAP_DBG_ERR, "Unable to dequeue -thread- buffer (%d).\n", errno);
			close(*thread_par.dev);
			return;
		}
		
		/* Save the image. */
		if(thread_par.multi_stream_mjpg_enable)
		{
			//mjpg_width = (unsigned int)((*(unsigned char *)(thread_par.mem[(thread_par.buf->index)]+9)) << 8) | (*(unsigned char *)(thread_par.mem[(thread_par.buf->index)]+10));
			//mjpg_height = (unsigned int)((*(unsigned char *)(thread_par.mem[(thread_par.buf->index)]+7)) << 8) | (*(unsigned char *)(thread_par.mem[(thread_par.buf->index)]+8));
			//mjpg_resolution = (unsigned int)((mjpg_width << 16) | (mjpg_height));
			mjpg_resolution = thread_par.buf->reserved;
			
			if(mjpg_resolution == H264_SIZE_HD)
				sprintf(filename, "[   HD]frame-%06u.jpg", i);
			else if(mjpg_resolution == H264_SIZE_VGA)
				sprintf(filename, "[  VGA]frame-%06u.jpg", i);
			else if(mjpg_resolution == H264_SIZE_QVGA)
				sprintf(filename, "[ QVGA]frame-%06u.jpg", i);
			else if(mjpg_resolution == H264_SIZE_QQVGA)
				sprintf(filename, "[QQVGA]frame-%06u.jpg", i);
			else
			{
				unknow_size = 1;
				//TestAp_Printf(TESTAP_DBG_FRAME, "  ### [frame %4d] mjpg  unknow size  w=%d  h=%d  \n",i , mjpg_width,mjpg_height);
				//sprintf(filename, "[%4d*%4d]frame-%06u.jpg", mjpg_width, mjpg_height , i);
			}
		}
		else
		{
			sprintf(filename, "frame-%06u.jpg", i);		
		}
		
		//TestAp_Printf(TESTAP_DBG_FRAME, "  +++ unknow_size=%d  skip=%d    %x  \n", unknow_size,skip,mjpg_resolution);
		
		if((!unknow_size)&&(!skip))
		{
			file = fopen(filename, "wb");
			if (file != NULL) 
			{
				fwrite(thread_par.mem[thread_par.buf->index], thread_par.buf->bytesused, 1, file);
				fclose(file);
			}
		}
		
		if(skip)
			--skip;
	
		ret = ioctl(*thread_par.dev, VIDIOC_QBUF, thread_par.buf);
		if (ret < 0) {
			TestAp_Printf(TESTAP_DBG_ERR, "Unable to requeue -thread- buffer (%d).\n", errno);
			close(*thread_par.dev);			
			return;
		}
	}

	pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
	char filename[] = "quickcam-0000.jpg";
	char rec_filename[] = "RecordH264.h264";			/*"H264.ts"*/
	char rec_filename1[] = "RecordH264HD.h264";		/*"H264.ts"*/
	char rec_filename2[] = "RecordH264QVGA.h264";	/*"H264.ts"*/
	char rec_filename3[] = "RecordH264QQVGA.h264";	/*"H264.ts"*/
	char rec_filename4[] = "RecordH264VGA.h264";		/*"H264.ts"*/
	int dev, ret;
	int fake_dev; // chris
	int freeram;

	/* Options parsings */
	char do_save = 0, do_enum_inputs = 0, do_capture = 0;
	char do_list_controls = 0, do_set_input = 0;
	char *endptr;
	int c;
	int framerate = 30;  
	
	/* Video buffers */
	void *mem0[V4L_BUFFERS_MAX];
	void *mem1[V4L_BUFFERS_MAX];
	unsigned int pixelformat = V4L2_PIX_FMT_MJPEG;
	unsigned int width = 1280;
	unsigned int height = 720;
	unsigned int nbufs = V4L_BUFFERS_DEFAULT;
	unsigned int input = 0;
	unsigned int skip = 0;

	/* Capture loop */
	struct timeval start, end, ts;
	unsigned int delay = 0, nframes = (unsigned int)-1;
	FILE *file = NULL;
	FILE *rec_fp1 = NULL;
	FILE *rec_fp2 = NULL;
	FILE *rec_fp3 = NULL;
	FILE *rec_fp4 = NULL;
	double fps;

	struct v4l2_buffer buf0;
	struct v4l2_buffer buf1;
	unsigned int i;

	char do_record 			= 0;	
	char do_add_xu_ctrl 	= 0;
	char do_xu_get_chip 	= 0;
	char do_xu_get_fmt   	= 0;
	char do_xu_set_fmt   	= 0;
	char do_xu_get_qp  		= 0;
	char do_xu_set_qp  		= 0;
	char do_xu_get_br  		= 0;
	char do_xu_set_br  		= 0;
	int m_QP_Val = 0;
	double m_BitRate = 0.0;
	struct Cur_H264Format cur_H264fmt;
	char do_xu_get 			= 0;
	char do_xu_set 			= 0;
	unsigned char GetXU_ID = 0;
	unsigned char SetXU_ID = 0;
	unsigned char GetCS = 0;
	unsigned char SetCS = 0;
	unsigned char GetCmdDataNum = 0;
	unsigned char SetCmdDataNum = 0;
	__u8 GetData[11] = {0};
	__u8 SetData[11] = {0};

	char do_bri_set = 0;
	char do_bri_get = 0;
	int m_bri_val = 0;
	char do_shrp_set = 0;
	char do_shrp_get = 0;
	int m_shrp_val = 0;
	char do_asic_r = 0;
	char do_asic_w = 0;
	unsigned int rAsicAddr = 0x0;
	unsigned char rAsicData = 0x0;
	unsigned int wAsicAddr = 0x0;
	unsigned char wAsicData = 0x0;

	/* multi-stream */
	unsigned char multi_stream_format = 0;
	unsigned char multi_stream_enable = 0;
	unsigned int multi_stream_width = 0;//1280;
	unsigned int multi_stream_height = 0;//720;
	unsigned int multi_stream_resolution = 0;//(multi_stream_width << 16) | (multi_stream_height);
	unsigned int MS_bitrate = 0;
	unsigned int MS_qp = 0;
	unsigned int streamID_set = 0;
	unsigned int streamID_get = 0;
	char do_multi_stream_set_bitrate = 0;
	char do_multi_stream_get_bitrate = 0;
	char do_multi_stream_set_qp = 0;
	char do_multi_stream_get_qp = 0;
	char do_multi_stream_set_type = 0;
	struct Multistream_Info TestInfo;
	char do_multi_stream_get_status = 0;
	char do_multi_stream_get_info = 0;
	char do_multi_stream_set_enable = 0;
	char do_multi_stream_get_enable = 0;
	char do_osd_timer_ctrl_set = 0;
	char do_osd_rtc_set = 0;
	char do_osd_rtc_get = 0;
	char do_osd_size_set = 0;
	char do_osd_size_get = 0;
	char do_osd_color_set = 0;
	char do_osd_color_get = 0;
	char do_osd_show_set = 0;
	char do_osd_show_get = 0;
	char do_osd_autoscale_set = 0;
	char do_osd_autoscale_get = 0;
	char do_osd_ms_size_set = 0;
	char do_osd_ms_size_get = 0;
	char do_osd_position_set = 0;
	char do_osd_position_get = 0;
	char do_osd_ms_position_set = 0;
	char do_osd_ms_position_get = 0;
	char do_md_mode_set = 0;
	char do_md_mode_get = 0;
	char do_md_threshold_set = 0;
	char do_md_threshold_get = 0;
	char do_md_mask_set = 0;
	char do_md_mask_get = 0;
	char do_md_result_set = 0;
	char do_md_result_get = 0;
	char do_mjpg_bitrate_set = 0;
	char do_mjpg_bitrate_get = 0;
	char do_h264_iframe_set = 0;
	char do_h264_sei_set = 0;
	char do_h264_sei_get = 0;
	char do_h264_gop_set = 0;
	char do_h264_gop_get = 0;
	char do_h264_mode_get = 0;
	char do_h264_mode_set = 0;
	char do_img_mirror_set = 0;
	char do_img_mirror_get = 0;
	char do_img_flip_set = 0;
	char do_img_flip_get = 0;
	char do_img_color_set = 0;
	char do_img_color_get = 0;
	char do_osd_string_set = 0;
	char do_osd_string_get = 0;
	char do_osd_carcam_set = 0;
	char do_osd_carcam_get = 0;
	char do_osd_speed_set = 0;
	char do_osd_speed_get = 0;
	char do_osd_coordinate_set = 0;
	char do_osd_coordinate_get = 0;
    char do_gpio_ctrl_set = 0;
    char do_gpio_ctrl_get = 0;
	char do_frame_drop_en_set = 0;
	char do_frame_drop_en_get = 0;
	char do_frame_drop_ctrl_set = 0;
	char do_frame_drop_ctrl_get = 0;	
	unsigned char osd_timer_count = 0;
	unsigned int osd_rtc_year = 0;
	unsigned char osd_rtc_month = 0;
	unsigned char osd_rtc_day = 0;
	unsigned char osd_rtc_hour = 0;
	unsigned char osd_rtc_minute = 0;
	unsigned char osd_rtc_second = 0;
	unsigned char osd_size_line = 0;
	unsigned char osd_size_block = 0;
	unsigned char osd_color_font = 0;
	unsigned char osd_color_border = 0;
	unsigned char osd_show_line = 0;
	unsigned char osd_show_block = 0;
	unsigned char osd_autoscale_line = 0;
	unsigned char osd_autoscale_block = 0;
	unsigned char osd_ms_size_stream0 = 0;
	unsigned char osd_ms_size_stream1 = 0;
	unsigned char osd_ms_size_stream2 = 0;
	unsigned char osd_type = 0;
	unsigned int osd_start_row = 0;
	unsigned int osd_start_col = 0;
	unsigned char osd_ms_position_streamid = 0;
	unsigned char osd_ms_start_row = 0;
	unsigned char osd_ms_start_col = 0;
	unsigned char osd_ms_s0_start_row = 0;
	unsigned char osd_ms_s0_start_col = 0;
	unsigned char osd_ms_s1_start_row = 0;
	unsigned char osd_ms_s1_start_col = 0;
	unsigned char osd_ms_s2_start_row = 0;
	unsigned char osd_ms_s2_start_col = 0;
	unsigned int osd_line_start_row = 0;
	unsigned int osd_line_start_col = 0;
	unsigned int osd_block_start_row = 0;
	unsigned int osd_block_start_col = 0;	
	unsigned char md_mode = 0;
	unsigned int md_threshold = 0;
	unsigned char md_mask[24] = {0};
	unsigned char md_result[24] = {0};
	unsigned int mjpg_bitrate = 0;
	unsigned char h264_sei_en = 0;
	unsigned int h264_gop = 0;
	unsigned int h264_mode = 0;
	unsigned char img_mirror = 0;
	unsigned char img_flip = 0;
	unsigned char img_color = 0;
	unsigned char h264_iframe_reset = 0;
	unsigned char osd_2nd_string_group = 0;
	unsigned char osd_speed_en = 0;
	unsigned char osd_coordinate_en = 0;
	unsigned char osd_coordinate_ctrl = 0;
	unsigned int osd_speed = 0;
	unsigned char osd_coordinate_direction = 0;
	unsigned char osd_direction_value[6] = {0};
	unsigned char osd_direction_value1 = 0;
	unsigned long osd_direction_value2 = 0;
	unsigned char osd_direction_value3 = 0;
	unsigned long osd_direction_value4 = 0;
    unsigned char gpio_ctrl_en = 0;
    unsigned char gpio_ctrl_output_value = 0;
    unsigned char gpio_ctrl_input_value = 0;
	unsigned char stream1_frame_drop_en = 0;
	unsigned char stream2_frame_drop_en = 0;
	unsigned char stream1_frame_drop_ctrl = 0;
	unsigned char stream2_frame_drop_ctrl = 0;
	char osd_string[12] = {"0"};
	pthread_t thread_capture_id;
#if(CARCAM_PROJECT == 1)
	printf("%s   ******  for Carcam  ******\n",TESTAP_VERSION);
#else
	printf("%s\n",TESTAP_VERSION);	
#endif

	if(!CheckKernelVersion())
	{
		TestAp_Printf(TESTAP_DBG_ERR, "TestAP didn't match current kernel version, please rebuild TestAP\n");
		return 0;
	}

	opterr = 0;
	while ((c = getopt_long(argc, argv, "c::d:f:hi:ln:s:Sagr", opts, NULL)) != -1) {
		
		TestAp_Printf(TESTAP_DBG_FLOW, "optind:%d  optopt:%d\n",optind,optopt);

		switch (c) {
		case 'c':
			do_capture = 1;
			if (optarg)
				nframes = atoi(optarg);
			break;
		case 'd':
			delay = atoi(optarg);
			break;
		case 'f':
			if (strcmp(optarg, "mjpg") == 0)
				pixelformat = V4L2_PIX_FMT_MJPEG;
			else if (strcmp(optarg, "yuyv") == 0)
				pixelformat = V4L2_PIX_FMT_YUYV;
			else if (strcmp(optarg, "MPEG") == 0)
				pixelformat = V4L2_PIX_FMT_MPEG;
			else if (strcmp(optarg, "H264") == 0)
				pixelformat = V4L2_PIX_FMT_H264;
			else if (strcmp(optarg, "MP2T") == 0)
				pixelformat = V4L2_PIX_FMT_MP2T;
			else {
				TestAp_Printf(TESTAP_DBG_ERR, "Unsupported video format '%s'\n", optarg);
				return 1;
			}
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'i':
			do_set_input = 1;
			input = atoi(optarg);
			break;
		case 'l':
			do_list_controls = 1;
			break;
		case 'n':
			nbufs = atoi(optarg);
			if (nbufs > V4L_BUFFERS_MAX)
				nbufs = V4L_BUFFERS_MAX;
			break;
		case 's':
			width = strtol(optarg, &endptr, 10);
			if (*endptr != 'x' || endptr == optarg) {
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid size '%s'\n", optarg);
				return 1;
			}
			height = strtol(endptr + 1, &endptr, 10);
			if (*endptr != 0) {
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid size '%s'\n", optarg);
				return 1;
			}
			break;
		case 'S':
			do_save = 1;
			break;
		case OPT_FRAMERATE:
			framerate = strtol(optarg, &endptr, 10);
			break;
		case OPT_ENUM_INPUTS:
			do_enum_inputs = 1;
			break;
		case OPT_SKIP_FRAMES:
			skip = atoi(optarg);
			break;		
		case 'r':			// record H.264 video sequence
			do_record = 1;
			break;
		case 'a':
			do_add_xu_ctrl = 1;
			break;
		case OPT_XU_GET_CHIPID:
			do_xu_get_chip = 1;
			break;
		case OPT_XU_GET_FMT:
			do_xu_get_fmt = 1;
			break;
		case OPT_XU_SET_FMT:
			
			do_xu_set_fmt = 1;

			cur_H264fmt.FmtId = strtol(optarg, &endptr, 10) - 1;
			if (*endptr != '-' || endptr == optarg) {
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			cur_H264fmt.FrameRateId = strtol(endptr + 1, &endptr, 10) - 1;
			if (*endptr != 0) {
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			
			break;
		case OPT_XU_GET_QP:
			do_xu_get_qp = 1;
			break;
		case OPT_XU_SET_QP:
			do_xu_set_qp = 1;
			m_QP_Val = atoi(optarg);
			break;
		case OPT_XU_GET_BITRATE:
			do_xu_get_br = 1;
			break;
		case OPT_XU_SET_BITRATE:
			do_xu_set_br = 1;
			m_BitRate = atoi(optarg);
			break;

		case OPT_XU_GET:
			do_xu_get = 1;

			GetXU_ID = strtol(optarg, &endptr, 16);				// Unit ID Number
			GetCS = strtol(endptr+1, &endptr, 16);				// Control Selector Number
			GetCmdDataNum = strtol(endptr+1, &endptr, 10);		// Command Data Number
			for(i=0; i<GetCmdDataNum; i++)
			{
				GetData[i] = strtol(endptr+1, &endptr, 16);		// Command Data
			}

			break;
			
		case OPT_XU_SET:
			do_xu_set = 1;

			SetXU_ID = strtol(optarg, &endptr, 16);				// Unit ID Number
			SetCS = strtol(endptr+1, &endptr, 16);				// Control Selector Number
			SetCmdDataNum = strtol(endptr+1, &endptr, 10);		// Command Data Number
			for(i=0; i<SetCmdDataNum; i++)
			{
				SetData[i] = strtol(endptr+1, &endptr, 16);		// Command Data
			}
				
			break;

		case OPT_BRIGHTNESS_GET:
			do_bri_get = 1;
			break;

		case OPT_BRIGHTNESS_SET:
			do_bri_set = 1;
			m_bri_val = atoi(optarg);
			break;

		case OPT_SHARPNESS_GET:
			do_shrp_get = 1;
			break;

		case OPT_SHARPNESS_SET:
			do_shrp_set = 1;
			m_shrp_val = atoi(optarg);
			break;
		
		case OPT_ASIC_READ:
			TestAp_Printf(TESTAP_DBG_FLOW, "== Asic Read: input command ==\n");
			do_asic_r = 1;
			
			rAsicAddr = strtol(optarg, &endptr, 16);		// Asic address
			TestAp_Printf(TESTAP_DBG_FLOW, "  Asic Address = 0x%x \n", rAsicAddr);
			break;
			
		case OPT_ASIC_WRITE:
			TestAp_Printf(TESTAP_DBG_FLOW, "== Asic Write: input command ==\n");
			do_asic_w = 1;
			
			wAsicAddr = strtol(optarg, &endptr, 16);		// Asic Address
			wAsicData = strtol(endptr+1, &endptr, 16);		// Asic Data
			TestAp_Printf(TESTAP_DBG_FLOW, "  Asic Address = 0x%x \n", wAsicAddr);
			TestAp_Printf(TESTAP_DBG_FLOW, "  Data         = 0x%x \n", wAsicData);
				
			break;	

#if(CARCAM_PROJECT == 0)
		case OPT_MULTI_FORMAT:
			multi_stream_format = atoi(optarg);

			if((multi_stream_format != MULTI_STREAM_HD_QVGA)&&(multi_stream_format != MULTI_STREAM_HD_QQVGA)
				&&(multi_stream_format != MULTI_STREAM_HD_QVGA_QQVGA)&&(multi_stream_format != MULTI_STREAM_HD_VGA)
				&&(multi_stream_format != MULTI_STREAM_HD_VGA_QVGA))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_multi_stream_set_type = 1;
			break;

		case OPT_MULTI_SET_BITRATE:
			streamID_set = strtol(optarg, &endptr, 10);
			MS_bitrate = strtol(endptr+1, &endptr, 10);

			if(streamID_set > 2)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			
			do_multi_stream_set_bitrate = 1;
			break;
			
		case OPT_MULTI_GET_BITRATE:
			streamID_get = strtol(optarg, &endptr, 10);

			if(streamID_get > 2)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}

			do_multi_stream_get_bitrate = 1;
			break;

		case OPT_MULTI_SET_QP:
			streamID_set = strtol(optarg, &endptr, 10);
			MS_qp = strtol(endptr+1, &endptr, 10);

			if(streamID_set > 2)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			
			do_multi_stream_set_qp = 1;
			break;
			
		case OPT_MULTI_GET_QP:
			streamID_get = strtol(optarg, &endptr, 10);

			if(streamID_get > 2)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}

			do_multi_stream_get_qp = 1;
			break;

		case OPT_MULTI_GET_STATUS:
			do_multi_stream_get_status = 1;
			break;

		case OPT_MULTI_GET_INFO:
			do_multi_stream_get_info = 1;
			break;

		case OPT_MULTI_SET_ENABLE:
			multi_stream_enable = atoi(optarg);
			if((multi_stream_enable != 0)&&(multi_stream_enable != 1)&&(multi_stream_enable != 3))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}

			do_multi_stream_set_enable = 1;
			break;

		case OPT_MULTI_GET_ENABLE:
			do_multi_stream_get_enable = 1;
			break;
#endif			
		case OPT_OSD_TIMER_CTRL_SET:
			osd_timer_count = atoi(optarg);
			if(osd_timer_count > 1)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_osd_timer_ctrl_set = 1;
			break;
			
		case OPT_OSD_RTC_SET:
			osd_rtc_year = strtol(optarg, &endptr, 10);
			osd_rtc_month = strtol(endptr+1, &endptr, 10);
			osd_rtc_day = strtol(endptr+1, &endptr, 10);
			osd_rtc_hour = strtol(endptr+1, &endptr, 10);
			osd_rtc_minute = strtol(endptr+1, &endptr, 10);
			osd_rtc_second = strtol(endptr+1, &endptr, 10);
			if((osd_rtc_year > 9999)||(osd_rtc_month > 12)||(osd_rtc_day > 31)||(osd_rtc_hour > 24)||(osd_rtc_minute > 59)||(osd_rtc_second > 59))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}			
			do_osd_rtc_set = 1;
			break;
			
		case OPT_OSD_RTC_GET:
			do_osd_rtc_get = 1;
			break;
			
		case OPT_OSD_SIZE_SET:
			osd_size_line = strtol(optarg, &endptr, 10);
			osd_size_block = strtol(endptr+1, &endptr, 10);
			if((osd_size_line > 4)||(osd_size_block > 4))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_osd_size_set = 1;
			break;
			
		case OPT_OSD_SIZE_GET:
			do_osd_size_get = 1;
			break;
			
		case OPT_OSD_COLOR_SET:
			osd_color_font = strtol(optarg, &endptr, 10);
			osd_color_border = strtol(endptr+1, &endptr, 10);
			if((osd_color_font > 4)||(osd_color_border > 4))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_osd_color_set = 1;
			break;
			
		case OPT_OSD_COLOR_GET:
			do_osd_color_get = 1;
			break;
			
		case OPT_OSD_SHOW_SET:
			osd_show_line = strtol(optarg, &endptr, 10);
			osd_show_block = strtol(endptr+1, &endptr, 10);
			if((osd_show_line > 1)||(osd_show_block > 1))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_osd_show_set = 1;
			break;
			
		case OPT_OSD_SHOW_GET:
			do_osd_show_get = 1;
			break;
			
		case OPT_OSD_AUTOSCALE_SET:
			osd_autoscale_line = strtol(optarg, &endptr, 10);
			osd_autoscale_block = strtol(endptr+1, &endptr, 10);
			if((osd_autoscale_line > 1)||(osd_autoscale_block > 1))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_osd_autoscale_set = 1;
			break;
			
		case OPT_OSD_AUTOSCALE_GET:
			do_osd_autoscale_get = 1;
			break;
#if(CARCAM_PROJECT == 0)		
		case OPT_OSD_MS_SIZE_SET:
			osd_ms_size_stream0 = strtol(optarg, &endptr, 10);
			osd_ms_size_stream1 = strtol(endptr+1, &endptr, 10);
			osd_ms_size_stream2 = strtol(endptr+1, &endptr, 10);
			if((osd_ms_size_stream0 > 4)||(osd_ms_size_stream1 > 4)||(osd_ms_size_stream2 > 4))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_osd_ms_size_set = 1;
			break;
			
		case OPT_OSD_MS_SIZE_GET:
			do_osd_ms_size_get = 1;
			break;

		case OPT_OSD_STRING_SET:
			osd_2nd_string_group = strtol(optarg, &endptr, 10);
			
			if(osd_2nd_string_group > 2)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			
			for(i=0; i<8; i++)
			{
				osd_string[i] = *(endptr+1+i);
			}

			do_osd_string_set = 1;
			break;

		case OPT_OSD_STRING_GET:
			osd_2nd_string_group = strtol(optarg, &endptr, 10);
			
			do_osd_string_get = 1;
			break;
#endif
		case OPT_OSD_POSITION_SET:
			osd_type = strtol(optarg, &endptr, 10);
			osd_start_row = strtol(endptr+1, &endptr, 10);
			osd_start_col = strtol(endptr+1, &endptr, 10);
			if((osd_type <= 0)|(osd_type > 3))
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_osd_position_set = 1;
			break;
			
		case OPT_OSD_POSITION_GET:
			do_osd_position_get = 1;
			break;
#if(CARCAM_PROJECT == 0)			
		case OPT_OSD_MS_POSITION_SET:
			osd_ms_position_streamid = strtol(optarg, &endptr, 10);
			osd_ms_start_row = strtol(endptr+1, &endptr, 10);
			osd_ms_start_col = strtol(endptr+1, &endptr, 10);
			do_osd_ms_position_set = 1;
			break;
			
		case OPT_OSD_MS_POSITION_GET:
			do_osd_ms_position_get = 1;
			break;			
#endif		
		case OPT_MD_MODE_SET:
			md_mode = atoi(optarg);
			if(md_mode > 1)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_md_mode_set = 1;
			break;
			
		case OPT_MD_MODE_GET:
			do_md_mode_get = 1;
			break;
			
		case OPT_MD_THRESHOLD_SET:
			md_threshold = atoi(optarg);
			if(md_threshold > 65535)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}
			do_md_threshold_set = 1;
			break;
			
		case OPT_MD_THRESHOLD_GET:
			do_md_threshold_get = 1;
			break;
			
		case OPT_MD_MASK_SET:
			md_mask[0] = strtol(optarg, &endptr, 16);

			for(i=1; i<24; i++)
			{
				md_mask[i] = strtol(endptr+1, &endptr, 16);
			}
			do_md_mask_set = 1;
			break;
			
		case OPT_MD_MASK_GET:
			do_md_mask_get = 1;
			break;

		case OPT_MD_RESULT_SET:
			md_result[0] = strtol(optarg, &endptr, 16);

			for(i=1; i<24; i++)
			{
				md_result[i] = strtol(endptr+1, &endptr, 16);
			}
			do_md_result_set = 1;
			break;
			
		case OPT_MD_RESULT_GET:
			do_md_result_get = 1;
			break;
			
		case OPT_MJPG_BITRATE_SET:
			mjpg_bitrate = strtol(optarg, &endptr, 10);
			do_mjpg_bitrate_set = 1;
			break;
			
		case OPT_MJPG_BITRATE_GET:
			do_mjpg_bitrate_get = 1;
			break;

		case OPT_H264_IFRAME_SET:
			h264_iframe_reset = atoi(optarg);
			do_h264_iframe_set = 1;
			break;

		case OPT_H264_SEI_SET:
			h264_sei_en = atoi(optarg);
			do_h264_sei_set = 1;
			break;

		case OPT_H264_SEI_GET:
			do_h264_sei_get = 1;
			break;

		case OPT_H264_GOP_SET:
			h264_gop = atoi(optarg);
			if(h264_gop > 4095)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}			
			do_h264_gop_set = 1;
			break;

		case OPT_H264_GOP_GET:
			do_h264_gop_get = 1;
			break;
			
		case OPT_H264_MODE_SET:
			h264_mode = atoi(optarg);
			if(h264_mode<1 || h264_mode>2)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "Invalid arguments '%s'\n", optarg);
				return 1;
			}	
			do_h264_mode_set = 1;
			break;
		
		case OPT_H264_MODE_GET:
			do_h264_mode_get = 1;
			break;

		case OPT_IMG_MIRROR_SET:
			img_mirror = atoi(optarg);
			do_img_mirror_set = 1;
			break;

		case OPT_IMG_MIRROR_GET:
			do_img_mirror_get = 1;
			break;

		case OPT_IMG_FLIP_SET:
			img_flip = atoi(optarg);
			do_img_flip_set = 1;
			break;

		case OPT_IMG_FLIP_GET:
			do_img_flip_get = 1;
			break;

		case OPT_IMG_COLOR_SET:
			img_color = atoi(optarg);
			do_img_color_set = 1;
			break;

		case OPT_IMG_COLOR_GET:
			do_img_color_get = 1;
			break;

        case OPT_GPIO_CTRL_SET:
            gpio_ctrl_en = strtol(optarg, &endptr, 16);
			gpio_ctrl_output_value = strtol(endptr+1, &endptr, 16);
            do_gpio_ctrl_set = 1;
            break;

        case OPT_GPIO_CTRL_GET:
            do_gpio_ctrl_get = 1;
            break;

		case OPT_FRAME_DROP_EN_SET:
			stream1_frame_drop_en = strtol(optarg, &endptr, 10);
			stream2_frame_drop_en = strtol(endptr+1, &endptr, 10);
			do_frame_drop_en_set = 1;
			break;

		case OPT_FRAME_DROP_EN_GET:
			do_frame_drop_en_get = 1;			
			break;			
			
		case OPT_FRAME_DROP_CTRL_SET:
			stream1_frame_drop_ctrl = strtol(optarg, &endptr, 10);
			stream2_frame_drop_ctrl = strtol(endptr+1, &endptr, 10);		
			do_frame_drop_ctrl_set = 1;
			break;

		case OPT_FRAME_DROP_CTRL_GET:
			do_frame_drop_ctrl_get = 1;
			break;	
			
#if(CARCAM_PROJECT == 1)
		case OPT_OSD_CARCAM_SET:
			osd_speed_en = strtol(optarg, &endptr, 10);
			osd_coordinate_en = strtol(endptr+1, &endptr, 10);
			osd_coordinate_ctrl = strtol(endptr+1, &endptr, 10);			
			do_osd_carcam_set = 1;
			break;

		case OPT_OSD_CARCAM_GET:
			do_osd_carcam_get = 1;
			break;

		case OPT_OSD_SPEED_SET:
			osd_speed = atoi(optarg);
			do_osd_speed_set = 1;
			break;

		case OPT_OSD_SPEED_GET:
			do_osd_speed_get = 1;
			break;

		case OPT_OSD_COORDINATE_SET1:
			osd_coordinate_direction = strtol(optarg, &endptr, 10);
			osd_direction_value[0] = strtol(endptr+1, &endptr, 10);
			osd_direction_value[1] = strtol(endptr+1, &endptr, 10);
			osd_direction_value[2] = strtol(endptr+1, &endptr, 10);
			osd_direction_value[3] = strtol(endptr+1, &endptr, 10);
			osd_direction_value[4] = strtol(endptr+1, &endptr, 10);
			osd_direction_value[5] = strtol(endptr+1, &endptr, 10);
			do_osd_coordinate_set = 1;
			break;

		case OPT_OSD_COORDINATE_GET1:
			do_osd_coordinate_get = 1;	
			break;

		case OPT_OSD_COORDINATE_SET2:
			osd_coordinate_direction = strtol(optarg, &endptr, 10);
			osd_direction_value1 = strtol(endptr+1, &endptr, 10);
			osd_direction_value2 = strtol(endptr+1, &endptr, 10);
			osd_direction_value3 = strtol(endptr+1, &endptr, 10);
			osd_direction_value4 = strtol(endptr+1, &endptr, 10);
			do_osd_coordinate_set = 2;
			break;

		case OPT_OSD_COORDINATE_GET2:
			do_osd_coordinate_get = 2;	
			break;
#endif

		case OPT_DEBUG_LEVEL:
			Dbg_Param = strtol(optarg, &endptr, 16);
			break;
		default:
			TestAp_Printf(TESTAP_DBG_ERR, "Invalid option -%c\n", c);
			TestAp_Printf(TESTAP_DBG_ERR, "Run %s -h for help.\n", argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}

	/* Open the video device. */
	dev = video_open(argv[optind]);
	if (dev < 0)
		return 1;
	
	v4l2ResetControl (dev, V4L2_CID_BRIGHTNESS);
  	v4l2ResetControl (dev, V4L2_CID_CONTRAST);
  	v4l2ResetControl (dev, V4L2_CID_SATURATION);
  	v4l2ResetControl (dev, V4L2_CID_GAIN);

	// RERVISION XU Ctrl ++++++++++++++++++++++++++++++++++++++++++++++++++++++

	ret = XU_Ctrl_ReadChipID(dev);
	if(ret<0)
		TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Ctrl_ReadChipID Failed\n");

	/* Add XU ctrls */
	if(do_add_xu_ctrl)
	{
		ret = XU_Init_Ctrl(dev);
		if(ret<0)
		{
			if(ret == -EEXIST)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : Initial XU Ctrls");
			}
			else
			{
				TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : Initial XU Ctrls Failed (%i)\n",ret);
			}
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : ");//Initial XU Ctrls ignored, uvc driver had already supported\n");//\t\t\t No need to Add Extension Unit Ctrls into Driver\n");

		}
	}

	if (do_list_controls)
		video_list_controls(dev);
	
	if(do_xu_get_chip)
	{
		ret = XU_Ctrl_ReadChipID(dev);
		if(ret<0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Ctrl_ReadChipID Failed\n");
	}

	if(do_xu_get_fmt)
	{
		if(!H264_GetFormat(dev))
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : H264 Get Format Failed\n");
	}

	if(do_xu_set_fmt)
	{
		if(XU_H264_SetFormat(dev, cur_H264fmt) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : H264 Set Format Failed\n");
	}
	
	//bit rate can only work in CBR mode, QP value can only work in VBR mode
	if(do_h264_mode_get)
	{
		XU_H264_Get_Mode(dev, &h264_mode);
	}

	if(do_h264_mode_set)
	{
		XU_H264_Set_Mode(dev, h264_mode);
	}

	if(do_xu_set_qp)
	{
		if(XU_H264_Set_QP(dev, m_QP_Val) < 0 )
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_H264_Set_QP Failed\n");
	}

	if(do_xu_get_qp)
	{
		if(XU_H264_Get_QP(dev, &m_QP_Val))
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_H264_Get_QP Failed\n");
	}

	if(do_xu_get_br)
	{
		XU_H264_Get_BitRate(dev, &m_BitRate);
		if(m_BitRate < 0 )
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_H264_Get_BitRate Failed\n");
		TestAp_Printf(TESTAP_DBG_FLOW, "Current bit rate: %.2f Kbps\n",m_BitRate);
	}

	if(do_xu_set_br)
	{
		if(XU_H264_Set_BitRate(dev, m_BitRate) < 0 )
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_H264_Set_BitRate Failed\n");
	}

	if(do_xu_set)
	{
		TestAp_Printf(TESTAP_DBG_FLOW, "== XU Set: input command ==\n");		
		TestAp_Printf(TESTAP_DBG_FLOW, "  XU ID = 0x%x \n", SetXU_ID);
		TestAp_Printf(TESTAP_DBG_FLOW, "  Control Selector = 0x%x \n", SetCS);
		TestAp_Printf(TESTAP_DBG_FLOW, "  Cmd Data Number  = %d \n", SetCmdDataNum);
			
		if(XU_Set_Cur(dev, SetXU_ID, SetCS, SetCmdDataNum, &SetData) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Set Failed\n");
	}

	if(do_xu_get)
	{
		TestAp_Printf(TESTAP_DBG_FLOW, "== XU Get: input command ==\n");
		TestAp_Printf(TESTAP_DBG_FLOW, "  XU ID = 0x%x \n", GetXU_ID);
		TestAp_Printf(TESTAP_DBG_FLOW, "  Control Selector = 0x%x \n", GetCS);
		TestAp_Printf(TESTAP_DBG_FLOW, "  Cmd Data Number  = %d \n", GetCmdDataNum);
		for(i=0; i<GetCmdDataNum; i++)
			TestAp_Printf(TESTAP_DBG_FLOW, "  Cmd Data[%d] = 0x%x\n", i, GetData[i]);		

		if(XU_Get_Cur(dev, GetXU_ID, GetCS, GetCmdDataNum, &GetData) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Get Failed\n");
	}

	if(do_multi_stream_set_type)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}

		// Set multi stream format to device
		if(XU_Multi_Set_Type(dev, multi_stream_format) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Set_Type Failed\n");
	}

	if(do_multi_stream_set_bitrate)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Set multi stream bitRate
		if(XU_Multi_Set_BitRate(dev, streamID_set, MS_bitrate) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Set_Bitrate Failed\n");
	}
	
	if(do_multi_stream_get_bitrate)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Get multi stream bitRate
		if(XU_Multi_Get_BitRate(dev, streamID_get, &MS_bitrate) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Get_Bitrate Failed\n");
	}

	if(do_multi_stream_set_qp)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Set multi stream QP
		if(XU_Multi_Set_QP(dev, streamID_set, MS_qp) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Set_QP Failed\n");
	}
	
	if(do_multi_stream_get_qp)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Get multi stream QP
		if(XU_Multi_Get_QP(dev, streamID_get, &MS_qp) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Get_QP Failed\n");
	}


	if(do_multi_stream_get_status)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Get multi stream status
		if(XU_Multi_Get_status(dev, &TestInfo) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Get_status Failed\n");
	}

	if(do_multi_stream_get_info)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Get multi stream info
		if(XU_Multi_Get_Info(dev, &TestInfo) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Get_Info Failed\n");
	}

	if(do_multi_stream_get_enable)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Get multi stream enable
		if(XU_Multi_Get_Enable(dev, &multi_stream_enable) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Get_Enable Failed\n");
	}
	
	if(do_multi_stream_set_enable)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		// Set multi stream enable
		if(XU_Multi_Set_Enable(dev, multi_stream_enable) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Set_Enable Failed\n");
	}
	else
	{
		// Set multi stream disable
		//if(XU_Multi_Set_Enable(dev, 0) < 0)
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Set_Enable Failed\n");
		if(XU_Multi_Get_Enable(dev, &multi_stream_enable) < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Multi_Get_Enable Failed\n");			
	}
	
	if(do_osd_timer_ctrl_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Timer_Ctrl(dev, osd_timer_count) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Timer_Ctrl Failed\n");
	}

	if(do_osd_rtc_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Set_RTC(dev, osd_rtc_year, osd_rtc_month, osd_rtc_day, osd_rtc_hour, osd_rtc_minute, osd_rtc_second) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_RTC Failed\n");
	}

	if(do_osd_rtc_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_RTC(dev, &osd_rtc_year, &osd_rtc_month, &osd_rtc_day, &osd_rtc_hour, &osd_rtc_minute, &osd_rtc_second) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_RTC Failed\n");
	}

	if(do_osd_size_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Set_Size(dev, osd_size_line, osd_size_block) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Size Failed\n");
	}

	if(do_osd_size_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_Size(dev, &osd_size_line, &osd_size_block) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Size Failed\n");
	}

	if(do_osd_color_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Set_Color(dev, osd_color_font, osd_color_border) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Color Failed\n");
	}

	if(do_osd_color_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_Color(dev, &osd_color_font, &osd_color_border) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Color Failed\n");
	}

	if(do_osd_show_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Set_Enable(dev, osd_show_line, osd_show_block) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Enable Failed\n");	
	}

	if(do_osd_show_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_Enable(dev, &osd_show_line, &osd_show_block) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Enable Failed\n");
	}

	if(do_osd_autoscale_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Set_AutoScale(dev, osd_autoscale_line, osd_autoscale_block) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_AutoScale Failed\n");
	}

	if(do_osd_autoscale_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_AutoScale(dev, &osd_autoscale_line, &osd_autoscale_block) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_AutoScale Failed\n");
	}

	if(do_osd_ms_size_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Set_Multi_Size(dev, osd_ms_size_stream0, osd_ms_size_stream1, osd_ms_size_stream2) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Multi_Size Failed\n");
	}

	if(do_osd_ms_size_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_Multi_Size(dev, &osd_ms_size_stream0, &osd_ms_size_stream1, &osd_ms_size_stream2) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Multi_Size Failed\n");
	}

	if(do_osd_position_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Set_Start_Position(dev, osd_type, osd_start_row, osd_start_col) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Start_Position Failed\n");
	}

	if(do_osd_position_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_Start_Position(dev, &osd_line_start_row, &osd_line_start_col, &osd_block_start_row, &osd_block_start_col) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Start_Position Failed\n");
	}

	if((do_osd_ms_position_set)&&(!do_capture))
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}

		if(XU_OSD_Set_MS_Start_Position(dev, osd_ms_position_streamid, osd_ms_start_row, osd_ms_start_col) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Start_Position Failed\n");
	}

	if(do_osd_ms_position_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_OSD_Get_MS_Start_Position(dev, &osd_ms_s0_start_row, &osd_ms_s0_start_col, &osd_ms_s1_start_row, &osd_ms_s1_start_col, &osd_ms_s2_start_row, &osd_ms_s2_start_col) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Start_Position Failed\n");
	}

	if(do_md_mode_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_MD_Set_Mode(dev, md_mode) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Set_Mode Failed\n");
	}
	
	if(do_md_mode_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_MD_Get_Mode(dev, &md_mode) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Get_Mode Failed\n");
	}

	if(do_md_threshold_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_MD_Set_Threshold(dev, md_threshold) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Set_Threshold Failed\n");
	}

	if(do_md_threshold_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_MD_Get_Threshold(dev, &md_threshold) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Get_Threshold Failed\n");
	}

	if(do_md_mask_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_MD_Set_Mask(dev, md_mask) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Set_Mask Failed\n");
	}

	if(do_md_mask_get)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_MD_Get_Mask(dev, md_mask) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Get_Mask Failed\n");
	}

	if(do_md_result_set)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		if(XU_MD_Set_RESULT(dev, md_mask) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Set_RESULT Failed\n");
	}

	if(do_mjpg_bitrate_set)
	{
		if(XU_MJPG_Set_Bitrate(dev, mjpg_bitrate) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MJPG_Set_Bitrate Failed\n");
	}

	if(do_mjpg_bitrate_get)
	{
		if(XU_MJPG_Get_Bitrate(dev, &mjpg_bitrate) <0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MJPG_Get_Bitrate Failed\n");
	}
	
	if(do_h264_sei_set)
	{
		XU_H264_Set_SEI(dev, h264_sei_en);
	}

	if(do_h264_sei_get)
	{
		XU_H264_Get_SEI(dev, &h264_sei_en);
	}
	
	if(do_h264_gop_set)
	{
		XU_H264_Set_GOP(dev, h264_gop);
	}

	if(do_h264_gop_get)
	{
		XU_H264_Get_GOP(dev, &h264_gop);
	}	

	if(do_img_mirror_set)
	{
		XU_IMG_Set_Mirror(dev, img_mirror);
	}

	if(do_img_mirror_get)
	{
		XU_IMG_Get_Mirror(dev, &img_mirror);
	}
	
	if(do_img_flip_set)
	{
		XU_IMG_Set_Flip(dev, img_flip);
	}

	if(do_img_flip_get)
	{
		XU_IMG_Get_Flip(dev, &img_flip);
	}
	
	if(do_img_color_set)
	{
		XU_IMG_Set_Color(dev, img_color);
	}

	if(do_img_color_get)
	{
		XU_IMG_Get_Color(dev, &img_color);
	}

	if(do_osd_string_set)
	{
		XU_OSD_Set_String(dev, osd_2nd_string_group, osd_string);
	}
	
	if(do_osd_string_get)
	{
		XU_OSD_Get_String(dev, osd_2nd_string_group, osd_string);
	}
	
	if(do_osd_carcam_set)
	{
		XU_OSD_Set_CarcamCtrl(dev, osd_speed_en, osd_coordinate_en, osd_coordinate_ctrl);
	}

	if(do_osd_carcam_get)
	{
		XU_OSD_Get_CarcamCtrl(dev, &osd_speed_en, &osd_coordinate_en, &osd_coordinate_ctrl);
	}

	if(do_osd_speed_set)
	{
		XU_OSD_Set_Speed(dev, osd_speed);
	}

	if(do_osd_speed_get)
	{
		XU_OSD_Get_Speed(dev, &osd_speed);
	}

	if(do_osd_coordinate_set == 1)
	{
		XU_OSD_Set_Coordinate1(dev, osd_coordinate_direction, osd_direction_value);
	}

	if(do_osd_coordinate_set == 2)
	{
		XU_OSD_Set_Coordinate2(dev, osd_coordinate_direction, osd_direction_value1, osd_direction_value2, osd_direction_value3, osd_direction_value4);
	}
	
	if(do_osd_coordinate_get == 1)
	{
		XU_OSD_Get_Coordinate1(dev, &osd_coordinate_direction, osd_direction_value);
	}

	if(do_osd_coordinate_get == 2)
	{
		XU_OSD_Get_Coordinate2(dev, &osd_coordinate_direction, &osd_direction_value1, &osd_direction_value2, &osd_direction_value3, &osd_direction_value4);
	}

    if(do_gpio_ctrl_set == 1)
    {
        XU_GPIO_Ctrl_Set(dev, gpio_ctrl_en, gpio_ctrl_output_value);
    }

    if(do_gpio_ctrl_get == 1)
    {
        XU_GPIO_Ctrl_Get(dev, &gpio_ctrl_en, &gpio_ctrl_output_value, &gpio_ctrl_input_value);
    }
    
	if(do_frame_drop_en_set == 1)
	{
		XU_Frame_Drop_En_Set(dev, stream1_frame_drop_en, stream2_frame_drop_en);
	}
	
	if(do_frame_drop_en_get == 1)
	{
		XU_Frame_Drop_En_Get(dev, &stream1_frame_drop_en, &stream2_frame_drop_en);
	}

	if(do_frame_drop_ctrl_set == 1)
	{
		XU_Frame_Drop_Ctrl_Set(dev, stream1_frame_drop_ctrl, stream2_frame_drop_ctrl);
	}
	
	if(do_frame_drop_ctrl_get == 1)
	{
		XU_Frame_Drop_Ctrl_Get(dev, &stream1_frame_drop_ctrl, &stream2_frame_drop_ctrl);
	}	
	
	// RERVISION XU Ctrl -------------------------------------------------------	

	// Standard UVC image properties setting +++++++++++++++++++++++++++++++
	if (do_bri_set) 
	{
	    if (v4l2SetControl (dev, V4L2_CID_BRIGHTNESS, m_bri_val)<0)
		TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : Set Brightness (%d) Failed\n", m_bri_val);
	} 

	if (do_bri_get) 
	{
		m_bri_val = v4l2GetControl (dev, V4L2_CID_BRIGHTNESS);
		if(m_bri_val < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : Get Brightness Failed\n");
		TestAp_Printf(TESTAP_DBG_FLOW, "RERVISION_UVC_TestAP @main : Get Brightness (%d)\n", m_bri_val);
	}

	if (do_shrp_set) 
	{
	    if (v4l2SetControl (dev, V4L2_CID_SHARPNESS, m_shrp_val)<0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : Set Sharpness (%d) Failed\n", m_shrp_val);
	} 

	if (do_shrp_get) 
	{
		m_shrp_val = v4l2GetControl (dev, V4L2_CID_SHARPNESS);
		if(m_shrp_val < 0)
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : Get Sharpness Failed\n");
		TestAp_Printf(TESTAP_DBG_FLOW, "RERVISION_UVC_TestAP @main : Get Sharpness (%d)\n", m_shrp_val);
	}
	// Standard UVC image properties setting -------------------------------
	
	if(do_asic_r)
	{
		if(XU_Asic_Read(dev, rAsicAddr, &rAsicData)<0)		
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Asic_Read(0x%x) Failed\n", rAsicAddr);
	}

	if(do_asic_w)
	{
		if(XU_Asic_Write(dev, wAsicAddr, wAsicData)<0 )
			TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_Asic_Write(0x%x) Failed\n",wAsicAddr);
	}

	if (do_enum_inputs)
		video_enum_inputs(dev);

	if (do_set_input)
		video_set_input(dev, input);

	//ret = video_get_input(dev);
	//TestAp_Printf(TESTAP_DBG_FLOW, "Input %d selected\n", ret);

	if (!do_capture) {
		close(dev);	
		return 0;
	}

	/* Set the video format. */
	if (video_set_format(dev, width, height, pixelformat) < 0) {		
		if(pixelformat == V4L2_PIX_FMT_H264) {
			TestAp_Printf(TESTAP_DBG_ERR, " === Set Format Failed : skip for H264 ===  \n");
		}
		else {
			close(dev);		
			return 1;
		}
	}

	/* Set the frame rate. */
	if (video_set_framerate(dev, framerate) < 0) {
		close(dev);		
		return 1;
	}

	
	if(GetFreeRam(&freeram) && freeram<1843200*nbufs+4194304)
	{
		TestAp_Printf(TESTAP_DBG_ERR, "free memory isn't enough(%d)\n",freeram);		
		return 1;
	}

	/* Allocate buffers. */
	if ((int)(nbufs = video_reqbufs(dev, nbufs)) < 0) {
		close(dev);		
		return 1;
	}

	/* Map the buffers. */
	for (i = 0; i < nbufs; ++i) {
		memset(&buf0, 0, sizeof buf0);
		buf0.index = i;
		buf0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf0.memory = V4L2_MEMORY_MMAP;
		ret = ioctl(dev, VIDIOC_QUERYBUF, &buf0);
		if (ret < 0) {
			TestAp_Printf(TESTAP_DBG_ERR, "Unable to query buffer %u (%d).\n", i, errno);
			close(dev);			
			return 1;
		}
		TestAp_Printf(TESTAP_DBG_FLOW, "length: %u offset: %10u     --  ", buf0.length, buf0.m.offset);

		mem0[i] = mmap(0, buf0.length, PROT_READ, MAP_SHARED, dev, buf0.m.offset);
		if (mem0[i] == MAP_FAILED) {
			TestAp_Printf(TESTAP_DBG_ERR, "Unable to map buffer %u (%d)\n", i, errno);
			close(dev);			
			return 1;
		}
		TestAp_Printf(TESTAP_DBG_FLOW, "Buffer %u mapped at address %p.\n", i, mem0[i]);
	}

	/* Queue the buffers. */
	for (i = 0; i < nbufs; ++i) {
		memset(&buf0, 0, sizeof buf0);
		buf0.index = i;
		buf0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf0.memory = V4L2_MEMORY_MMAP;
		ret = ioctl(dev, VIDIOC_QBUF, &buf0);
		if (ret < 0) {
			TestAp_Printf(TESTAP_DBG_ERR, "Unable to queue buffer0(%d).\n", errno);
			close(dev);			
			return 1;
		}
	}

	/* Start streaming. */
	video_enable(dev, 1);

	if((multi_stream_enable)&&(pixelformat == V4L2_PIX_FMT_H264))
	{
		char fake_filename[12];
		strcpy(fake_filename, argv[optind]);
		if(fake_filename[10]>0x30)	//	for string "/dev/videoX", if X>0, X--
			fake_filename[10] -= 1;
		/* Open the video device. */
		fake_dev = video_open(fake_filename);
		if (fake_dev < 0)
			return 1;

		/* Set the video format. */
		if (video_set_format(fake_dev, 640, 480, V4L2_PIX_FMT_MJPEG) < 0) {
			close(fake_dev);
			return 1;
		}

		if(do_save)
		{
			/* Allocate buffers. */
			if(GetFreeRam(&freeram) && freeram<307200*nbufs+4+4194304)
			{
				TestAp_Printf(TESTAP_DBG_ERR, "do_save:free memory isn't enough(%d)\n",freeram);
				close(fake_dev);
				return 1;
			}
				
			if ((int)(nbufs = video_reqbufs(fake_dev, nbufs)) < 0) {
				close(fake_dev);
				return 1;
			}

			/* Map the buffers. */
			for (i = 0; i < nbufs; ++i) {
				memset(&buf1, 0, sizeof buf1);
				buf1.index = i;
				buf1.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				buf1.memory = V4L2_MEMORY_MMAP;
				ret = ioctl(fake_dev, VIDIOC_QUERYBUF, &buf1);
				if (ret < 0) {
					TestAp_Printf(TESTAP_DBG_ERR, "Unable to query buffer %u (%d).\n", i, errno);
					close(dev);
					close(fake_dev);
					return 1;
				}
				TestAp_Printf(TESTAP_DBG_FLOW, "length: %u offset: %10u     --  ", buf1.length, buf1.m.offset);

				mem1[i] = mmap(0, buf1.length, PROT_READ, MAP_SHARED, fake_dev, buf1.m.offset);
				if (mem1[i] == MAP_FAILED) {
					TestAp_Printf(TESTAP_DBG_ERR, "Unable to map buffer %u (%d)\n", i, errno);
					close(dev);
					close(fake_dev);
					return 1;
				}
				TestAp_Printf(TESTAP_DBG_FLOW, "Buffer %u mapped at address %p.\n", i, mem1[i]);
			}

			/* Queue the buffers. */
			for (i = 0; i < nbufs; ++i) {
				memset(&buf1, 0, sizeof buf1);
				buf1.index = i;
				buf1.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				buf1.memory = V4L2_MEMORY_MMAP;
				ret = ioctl(fake_dev, VIDIOC_QBUF, &buf1);
				if (ret < 0) {
					TestAp_Printf(TESTAP_DBG_ERR, "Unable to queue buffer (%d).\n", errno);
					close(dev);
					close(fake_dev);
					return 1;
				}
			}
		}
		/* Start streaming. */
		video_enable(fake_dev, 1);
	}

	if(multi_stream_enable != 0)
	{
		if((chip_id != CHIP_RER9421)&&(chip_id != CHIP_RER9422))
		{
			close(dev);
			close(fake_dev);			
			TestAp_Printf(TESTAP_DBG_ERR, "This command only for 9421 & 9422'\n");
			return 1;			
		}
		
		//set multi stream osd start position
		//if(XU_OSD_Get_MS_Start_Position(dev, &osd_ms_s0_start_row, &osd_ms_s0_start_col, &osd_ms_s1_start_row, &osd_ms_s1_start_col, &osd_ms_s2_start_row, &osd_ms_s2_start_col) <0)
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Start_Position Failed\n");

		//if(XU_OSD_Set_MS_Start_Position(dev, 0, osd_ms_s0_start_row, osd_ms_s0_start_col) <0)
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Start_Position Failed\n");

		//if(XU_OSD_Set_MS_Start_Position(dev, 1, osd_ms_s1_start_row, osd_ms_s1_start_col) <0)
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Start_Position Failed\n");
			
		//if(XU_OSD_Set_MS_Start_Position(dev, 2, osd_ms_s2_start_row, osd_ms_s2_start_col) <0)
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Start_Position Failed\n");

		//set multi stream osd size
		//if(XU_OSD_Get_Multi_Size(dev, &osd_ms_size_stream0, &osd_ms_size_stream1, &osd_ms_size_stream2) <0)
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Get_Multi_Size Failed\n");

		//if(XU_OSD_Set_Multi_Size(dev, osd_ms_size_stream0, osd_ms_size_stream1, osd_ms_size_stream2) <0)
			//TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_OSD_Set_Multi_Size Failed\n");
	}	

	if((do_record)&&(do_save)&&(multi_stream_enable!=0)&&(pixelformat == V4L2_PIX_FMT_H264))
	{
		struct thread_parameter par;
		par.buf = &buf1;
		for(i=0;i < nbufs;i++)
			par.mem[i] = mem1[i];

		par.dev = &fake_dev;
		par.nframes = &nframes;
		par.multi_stream_mjpg_enable = (multi_stream_enable & 0x02) >> 1;
		
		ret = pthread_create(&thread_capture_id,NULL,thread_capture,(void*)&par);
		if(ret != 0)
		{
			close(dev);
			close(fake_dev);
			TestAp_Printf(TESTAP_DBG_ERR, "Create pthread error!\n");
			return 1;
		}
	}

	for (i = 0; i < nframes; ++i) {
		if((do_h264_iframe_set) && (i%h264_iframe_reset == 0))
		{
			XU_H264_Set_IFRAME(dev);
		}

		/* Dequeue a buffer. */
		memset(&buf0, 0, sizeof buf0);
		buf0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf0.memory = V4L2_MEMORY_MMAP;
		ret = ioctl(dev, VIDIOC_DQBUF, &buf0);
		if (ret < 0) {
			TestAp_Printf(TESTAP_DBG_ERR, "Unable to dequeue buffer0 (%d).\n", errno);
			close(dev);
			if(multi_stream_enable)
				close(fake_dev);
			return 1;
		}

		gettimeofday(&ts, NULL);

		if(multi_stream_enable)
		{
			h264_decode_seq_parameter_set(mem0[buf0.index]+4, buf0.bytesused, &multi_stream_width, &multi_stream_height);
			
			multi_stream_resolution = (multi_stream_width << 16) | (multi_stream_height);
			if(multi_stream_resolution == H264_SIZE_HD)
			{
				TestAp_Printf(TESTAP_DBG_FRAME, "[   HD]  ");		
			}
			else if(multi_stream_resolution == H264_SIZE_VGA)
			{
				TestAp_Printf(TESTAP_DBG_FRAME, "[  VGA]  ");
			}
			else if(multi_stream_resolution == H264_SIZE_QVGA)
			{
				TestAp_Printf(TESTAP_DBG_FRAME, "[ QVGA]  ");
			}
			else if(multi_stream_resolution == H264_SIZE_QQVGA)
			{
				TestAp_Printf(TESTAP_DBG_FRAME, "[QQVGA]  ");
			}
			else
			{
				TestAp_Printf(TESTAP_DBG_FRAME, "[unknow size w:%d h:%d ]  ",multi_stream_width, multi_stream_height);
			}

		}

		TestAp_Printf(TESTAP_DBG_FRAME, "Frame[%4u] %u bytes %ld.%06ld %ld.%06ld\n ", i, buf0.bytesused, buf0.timestamp.tv_sec, buf0.timestamp.tv_usec, ts.tv_sec, ts.tv_usec);

		if(do_md_result_get)
		{
			if(XU_MD_Get_RESULT(dev, md_mask) <0)
				TestAp_Printf(TESTAP_DBG_ERR, "RERVISION_UVC_TestAP @main : XU_MD_Get_RESULT Failed\n");
		}

		if (i == 0)
			start = ts;

		/* Save the image. */
		if ((do_save && !skip)&&(pixelformat == V4L2_PIX_FMT_MJPEG))
		{
			sprintf(filename, "frame-%06u.jpg", i);
			file = fopen(filename, "wb");
			if (file != NULL) {
				fwrite(mem0[buf0.index], buf0.bytesused, 1, file);
					
				fclose(file);
			}
		}
		if (skip)
			--skip;

		/* Record the H264 video file */
		if(do_record)
		{
			if((multi_stream_enable & 0x01) == 1)
			{
				if(multi_stream_resolution == H264_SIZE_HD)
				{
					if(rec_fp1 == NULL)
						rec_fp1 = fopen(rec_filename1, "a+b");
					
					if(rec_fp1 != NULL)
						fwrite(mem0[buf0.index], buf0.bytesused, 1, rec_fp1);
				}

				if(multi_stream_resolution == H264_SIZE_VGA)
				{
					if(rec_fp4 == NULL)
						rec_fp4 = fopen(rec_filename4, "a+b");
					
					if(rec_fp4 != NULL)
						fwrite(mem0[buf0.index], buf0.bytesused, 1, rec_fp4);
				}

				if(multi_stream_resolution == H264_SIZE_QVGA)
				{
					if(rec_fp2 == NULL)
						rec_fp2 = fopen(rec_filename2, "a+b");
					
					if(rec_fp2 != NULL)
						fwrite(mem0[buf0.index], buf0.bytesused, 1, rec_fp2);
				}

				if(multi_stream_resolution == H264_SIZE_QQVGA)
				{
					if(rec_fp3 == NULL)
						rec_fp3 = fopen(rec_filename3, "a+b");
					
					if(rec_fp3 != NULL)
						fwrite(mem0[buf0.index], buf0.bytesused, 1, rec_fp3);
				}
			}
			else
			{
				if(rec_fp1 == NULL)
					rec_fp1 = fopen(rec_filename, "a+b");

				if(rec_fp1 != NULL)
				{
					fwrite(mem0[buf0.index], buf0.bytesused, 1, rec_fp1);
				}
			}
		}

		/* Requeue the buffer. */
		if (delay > 0)
			usleep(delay * 1000);

		ret = ioctl(dev, VIDIOC_QBUF, &buf0);
		if (ret < 0) {
			TestAp_Printf(TESTAP_DBG_ERR, "Unable to requeue buffer0 (%d).\n", errno);
			close(dev);
			if(multi_stream_enable)
				close(fake_dev);			
			return 1;
		}

		fflush(stdout);
	}
	gettimeofday(&end, NULL);

	if((do_record)&&(do_save)&&(multi_stream_enable == 1)&&(pixelformat == V4L2_PIX_FMT_H264))
		pthread_join(thread_capture_id,NULL);
	
	/* Stop streaming. */
	video_enable(dev, 0);
	
	if(multi_stream_enable)
		video_enable(fake_dev, 0);

	if(do_record && rec_fp1 != NULL)
		fclose(rec_fp1);
	
	if(do_record && rec_fp2 != NULL)
		fclose(rec_fp2);

	if(do_record && rec_fp3 != NULL)
		fclose(rec_fp3);		

	end.tv_sec -= start.tv_sec;
	end.tv_usec -= start.tv_usec;

	if (end.tv_usec < 0) {
		end.tv_sec--;
		end.tv_usec += 1000000;
	}
	fps = (i-1)/(end.tv_usec+1000000.0*end.tv_sec)*1000000.0;

	printf("Captured %u frames in %lu.%06lu seconds (%f fps).\n",
		i-1, end.tv_sec, end.tv_usec, fps);

	if(gH264fmt)
	{
		free(gH264fmt);
		gH264fmt = NULL;
	}

	close(dev);
	if(multi_stream_enable)
		close(fake_dev);	
	return 0;
}
