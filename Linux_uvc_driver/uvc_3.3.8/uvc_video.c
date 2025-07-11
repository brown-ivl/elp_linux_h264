/*
 *      uvc_video.c  --  USB Video Class driver - Video handling
 *
 *      Copyright (C) 2005-2010
 *          Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/time64.h>
#include <asm/unaligned.h>

#include <media/v4l2-common.h>

#include "uvcvideo.h"
#include "nalu.h"
#define PATCH_OF_RER9420_MJPG_EOF_LOST

/* ------------------------------------------------------------------------
 * UVC Controls
 */

static int __uvc_query_ctrl(struct uvc_device *dev, __u8 query, __u8 unit,
			__u8 intfnum, __u8 cs, void *data, __u16 size,
			int timeout)
{
	__u8 type = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
	unsigned int pipe;

	pipe = (query & 0x80) ? usb_rcvctrlpipe(dev->udev, 0)
			      : usb_sndctrlpipe(dev->udev, 0);
	type |= (query & 0x80) ? USB_DIR_IN : USB_DIR_OUT;

	return usb_control_msg(dev->udev, pipe, query, type, cs << 8,
			unit << 8 | intfnum, data, size, timeout);
}

static const char *uvc_query_name(__u8 query)
{
	switch (query) {
	case UVC_SET_CUR:
		return "SET_CUR";
	case UVC_GET_CUR:
		return "GET_CUR";
	case UVC_GET_MIN:
		return "GET_MIN";
	case UVC_GET_MAX:
		return "GET_MAX";
	case UVC_GET_RES:
		return "GET_RES";
	case UVC_GET_LEN:
		return "GET_LEN";
	case UVC_GET_INFO:
		return "GET_INFO";
	case UVC_GET_DEF:
		return "GET_DEF";
	default:
		return "<invalid>";
	}
}

int uvc_query_ctrl(struct uvc_device *dev, __u8 query, __u8 unit,
			__u8 intfnum, __u8 cs, void *data, __u16 size)
{
	int ret;

	ret = __uvc_query_ctrl(dev, query, unit, intfnum, cs, data, size,
				UVC_CTRL_CONTROL_TIMEOUT);
	if (ret != size) {
		uvc_printk(KERN_ERR, "Failed to query (%s) UVC control %u on "
			"unit %u: %d (exp. %u).\n", uvc_query_name(query), cs,
			unit, ret, size);
		return -EIO;
	}

	return 0;
}

static void uvc_fixup_video_ctrl(struct uvc_streaming *stream,
	struct uvc_streaming_control *ctrl)
{
	struct uvc_format *format = NULL;
	struct uvc_frame *frame = NULL;
	unsigned int i;

	for (i = 0; i < stream->nformats; ++i) {
		if (stream->format[i].index == ctrl->bFormatIndex) {
			format = &stream->format[i];
			break;
		}
	}

	if (format == NULL)
		return;

	for (i = 0; i < format->nframes; ++i) {
		if (format->frame[i].bFrameIndex == ctrl->bFrameIndex) {
			frame = &format->frame[i];
			break;
		}
	}

	if (frame == NULL)
		return;

	if (!(format->flags & UVC_FMT_FLAG_COMPRESSED) ||
	     (ctrl->dwMaxVideoFrameSize == 0 &&
	      stream->dev->uvc_version < 0x0110))
		ctrl->dwMaxVideoFrameSize =
			frame->dwMaxVideoFrameBufferSize;

	if (!(format->flags & UVC_FMT_FLAG_COMPRESSED) &&
	    stream->dev->quirks & UVC_QUIRK_FIX_BANDWIDTH &&
	    stream->intf->num_altsetting > 1) {
		u32 interval;
		u32 bandwidth;

		interval = (ctrl->dwFrameInterval > 100000)
			 ? ctrl->dwFrameInterval
			 : frame->dwFrameInterval[0];

		/* Compute a bandwidth estimation by multiplying the frame
		 * size by the number of video frames per second, divide the
		 * result by the number of USB frames (or micro-frames for
		 * high-speed devices) per second and add the UVC header size
		 * (assumed to be 12 bytes long).
		 */
		bandwidth = frame->wWidth * frame->wHeight / 8 * format->bpp;
		bandwidth *= 10000000 / interval + 1;
		bandwidth /= 1000;
		if (stream->dev->udev->speed == USB_SPEED_HIGH)
			bandwidth /= 8;
		bandwidth += 12;

		/* The bandwidth estimate is too low for many cameras. Don't use
		 * maximum packet sizes lower than 1024 bytes to try and work
		 * around the problem. According to measurements done on two
		 * different camera models, the value is high enough to get most
		 * resolutions working while not preventing two simultaneous
		 * VGA streams at 15 fps.
		 */
		bandwidth = max_t(u32, bandwidth, 1024);

		ctrl->dwMaxPayloadTransferSize = bandwidth;
	}
}

static int uvc_get_video_ctrl(struct uvc_streaming *stream,
	struct uvc_streaming_control *ctrl, int probe, __u8 query)
{
	__u8 *data;
	__u16 size;
	int ret;

	size = stream->dev->uvc_version >= 0x0110 ? 34 : 26;
	if ((stream->dev->quirks & UVC_QUIRK_PROBE_DEF) &&
			query == UVC_GET_DEF)
		return -EIO;

	data = kmalloc(size, GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	ret = __uvc_query_ctrl(stream->dev, query, 0, stream->intfnum,
		probe ? UVC_VS_PROBE_CONTROL : UVC_VS_COMMIT_CONTROL, data,
		size, uvc_timeout_param);

	if ((query == UVC_GET_MIN || query == UVC_GET_MAX) && ret == 2) {
		/* Some cameras, mostly based on Bison Electronics chipsets,
		 * answer a GET_MIN or GET_MAX request with the wCompQuality
		 * field only.
		 */
		uvc_warn_once(stream->dev, UVC_WARN_MINMAX, "UVC non "
			"compliance - GET_MIN/MAX(PROBE) incorrectly "
			"supported. Enabling workaround.\n");
		memset(ctrl, 0, sizeof *ctrl);
		ctrl->wCompQuality = le16_to_cpup((__le16 *)data);
		ret = 0;
		goto out;
	} else if (query == UVC_GET_DEF && probe == 1 && ret != size) {
		/* Many cameras don't support the GET_DEF request on their
		 * video probe control. Warn once and return, the caller will
		 * fall back to GET_CUR.
		 */
		uvc_warn_once(stream->dev, UVC_WARN_PROBE_DEF, "UVC non "
			"compliance - GET_DEF(PROBE) not supported. "
			"Enabling workaround.\n");
		ret = -EIO;
		goto out;
	} else if (ret != size) {
		uvc_printk(KERN_ERR, "Failed to query (%u) UVC %s control : "
			"%d (exp. %u).\n", query, probe ? "probe" : "commit",
			ret, size);
		ret = -EIO;
		goto out;
	}

	ctrl->bmHint = le16_to_cpup((__le16 *)&data[0]);
	ctrl->bFormatIndex = data[2];
	ctrl->bFrameIndex = data[3];
	ctrl->dwFrameInterval = le32_to_cpup((__le32 *)&data[4]);
	ctrl->wKeyFrameRate = le16_to_cpup((__le16 *)&data[8]);
	ctrl->wPFrameRate = le16_to_cpup((__le16 *)&data[10]);
	ctrl->wCompQuality = le16_to_cpup((__le16 *)&data[12]);
	ctrl->wCompWindowSize = le16_to_cpup((__le16 *)&data[14]);
	ctrl->wDelay = le16_to_cpup((__le16 *)&data[16]);
	ctrl->dwMaxVideoFrameSize = get_unaligned_le32(&data[18]);
	ctrl->dwMaxPayloadTransferSize = get_unaligned_le32(&data[22]);

	if (size == 34) {
		ctrl->dwClockFrequency = get_unaligned_le32(&data[26]);
		ctrl->bmFramingInfo = data[30];
		ctrl->bPreferedVersion = data[31];
		ctrl->bMinVersion = data[32];
		ctrl->bMaxVersion = data[33];
	} else {
		ctrl->dwClockFrequency = stream->dev->clock_frequency;
		ctrl->bmFramingInfo = 0;
		ctrl->bPreferedVersion = 0;
		ctrl->bMinVersion = 0;
		ctrl->bMaxVersion = 0;
	}

	/* Some broken devices return null or wrong dwMaxVideoFrameSize and
	 * dwMaxPayloadTransferSize fields. Try to get the value from the
	 * format and frame descriptors.
	 */
	uvc_fixup_video_ctrl(stream, ctrl);
	ret = 0;

out:
	kfree(data);
	return ret;
}

static int uvc_set_video_ctrl(struct uvc_streaming *stream,
	struct uvc_streaming_control *ctrl, int probe)
{
	__u8 *data;
	__u16 size;
	int ret;

	size = stream->dev->uvc_version >= 0x0110 ? 34 : 26;
	data = kzalloc(size, GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	*(__le16 *)&data[0] = cpu_to_le16(ctrl->bmHint);
	data[2] = ctrl->bFormatIndex;
	data[3] = ctrl->bFrameIndex;
	*(__le32 *)&data[4] = cpu_to_le32(ctrl->dwFrameInterval);
	*(__le16 *)&data[8] = cpu_to_le16(ctrl->wKeyFrameRate);
	*(__le16 *)&data[10] = cpu_to_le16(ctrl->wPFrameRate);
	*(__le16 *)&data[12] = cpu_to_le16(ctrl->wCompQuality);
	*(__le16 *)&data[14] = cpu_to_le16(ctrl->wCompWindowSize);
	*(__le16 *)&data[16] = cpu_to_le16(ctrl->wDelay);
	put_unaligned_le32(ctrl->dwMaxVideoFrameSize, &data[18]);
	put_unaligned_le32(ctrl->dwMaxPayloadTransferSize, &data[22]);

	if (size == 34) {
		put_unaligned_le32(ctrl->dwClockFrequency, &data[26]);
		data[30] = ctrl->bmFramingInfo;
		data[31] = ctrl->bPreferedVersion;
		data[32] = ctrl->bMinVersion;
		data[33] = ctrl->bMaxVersion;
	}

	ret = __uvc_query_ctrl(stream->dev, UVC_SET_CUR, 0, stream->intfnum,
		probe ? UVC_VS_PROBE_CONTROL : UVC_VS_COMMIT_CONTROL, data,
		size, uvc_timeout_param);
	if (ret != size) {
		uvc_printk(KERN_ERR, "Failed to set UVC %s control : "
			"%d (exp. %u).\n", probe ? "probe" : "commit",
			ret, size);
		ret = -EIO;
	}

	kfree(data);
	return ret;
}

int uvc_probe_video(struct uvc_streaming *stream,
	struct uvc_streaming_control *probe)
{
	struct uvc_streaming_control probe_min, probe_max;
	__u16 bandwidth;
	unsigned int i;
	int ret;

	/* Perform probing. The device should adjust the requested values
	 * according to its capabilities. However, some devices, namely the
	 * first generation UVC Logitech webcams, don't implement the Video
	 * Probe control properly, and just return the needed bandwidth. For
	 * that reason, if the needed bandwidth exceeds the maximum available
	 * bandwidth, try to lower the quality.
	 */
	ret = uvc_set_video_ctrl(stream, probe, 1);
	if (ret < 0)
		goto done;

	/* Get the minimum and maximum values for compression settings. */
	if (!(stream->dev->quirks & UVC_QUIRK_PROBE_MINMAX)) {
		ret = uvc_get_video_ctrl(stream, &probe_min, 1, UVC_GET_MIN);
		if (ret < 0)
			goto done;
		ret = uvc_get_video_ctrl(stream, &probe_max, 1, UVC_GET_MAX);
		if (ret < 0)
			goto done;

		probe->wCompQuality = probe_max.wCompQuality;
	}

	for (i = 0; i < 2; ++i) {
		ret = uvc_set_video_ctrl(stream, probe, 1);
		if (ret < 0)
			goto done;
		ret = uvc_get_video_ctrl(stream, probe, 1, UVC_GET_CUR);
		if (ret < 0)
			goto done;

		if (stream->intf->num_altsetting == 1)
			break;

		bandwidth = probe->dwMaxPayloadTransferSize;
		if (bandwidth <= stream->maxpsize)
			break;

		if (stream->dev->quirks & UVC_QUIRK_PROBE_MINMAX) {
			ret = -ENOSPC;
			goto done;
		}

		/* TODO: negotiate compression parameters */
		probe->wKeyFrameRate = probe_min.wKeyFrameRate;
		probe->wPFrameRate = probe_min.wPFrameRate;
		probe->wCompQuality = probe_max.wCompQuality;
		probe->wCompWindowSize = probe_min.wCompWindowSize;
	}

done:
	return ret;
}

static int uvc_commit_video(struct uvc_streaming *stream,
			    struct uvc_streaming_control *probe)
{
	return uvc_set_video_ctrl(stream, probe, 0);
}

/* -----------------------------------------------------------------------------
 * Clocks and timestamps
 */

static void
uvc_video_clock_decode(struct uvc_streaming *stream, struct uvc_buffer *buf,
		       const __u8 *data, int len)
{
	struct uvc_clock_sample *sample;
	unsigned int header_size;
	bool has_pts = false;
	bool has_scr = false;
	unsigned long flags;
	struct timespec64 ts;
	u16 host_sof;
	u16 dev_sof;

	switch (data[1] & (UVC_STREAM_PTS | UVC_STREAM_SCR)) {
	case UVC_STREAM_PTS | UVC_STREAM_SCR:
		header_size = 12;
		has_pts = true;
		has_scr = true;
		break;
	case UVC_STREAM_PTS:
		header_size = 6;
		has_pts = true;
		break;
	case UVC_STREAM_SCR:
		header_size = 8;
		has_scr = true;
		break;
	default:
		header_size = 2;
		break;
	}

	/* Check for invalid headers. */
	if (len < header_size)
		return;

	/* Extract the timestamps:
	 *
	 * - store the frame PTS in the buffer structure
	 * - if the SCR field is present, retrieve the host SOF counter and
	 *   kernel timestamps and store them with the SCR STC and SOF fields
	 *   in the ring buffer
	 */
	if (has_pts && buf != NULL)
		buf->pts = get_unaligned_le32(&data[2]);

	if (!has_scr)
		return;

	/* To limit the amount of data, drop SCRs with an SOF identical to the
	 * previous one.
	 */
	dev_sof = get_unaligned_le16(&data[header_size - 2]);
	if (dev_sof == stream->clock.last_sof)
		return;

	stream->clock.last_sof = dev_sof;

	host_sof = usb_get_current_frame_number(stream->dev->udev);
	ktime_get_ts64(&ts);

	/* The UVC specification allows device implementations that can't obtain
	 * the USB frame number to keep their own frame counters as long as they
	 * match the size and frequency of the frame number associated with USB
	 * SOF tokens. The SOF values sent by such devices differ from the USB
	 * SOF tokens by a fixed offset that needs to be estimated and accounted
	 * for to make timestamp recovery as accurate as possible.
	 *
	 * The offset is estimated the first time a device SOF value is received
	 * as the difference between the host and device SOF values. As the two
	 * SOF values can differ slightly due to transmission delays, consider
	 * that the offset is null if the difference is not higher than 10 ms
	 * (negative differences can not happen and are thus considered as an
	 * offset). The video commit control wDelay field should be used to
	 * compute a dynamic threshold instead of using a fixed 10 ms value, but
	 * devices don't report reliable wDelay values.
	 *
	 * See uvc_video_clock_host_sof() for an explanation regarding why only
	 * the 8 LSBs of the delta are kept.
	 */
	if (stream->clock.sof_offset == (u16)-1) {
		u16 delta_sof = (host_sof - dev_sof) & 255;
		if (delta_sof >= 10)
			stream->clock.sof_offset = delta_sof;
		else
			stream->clock.sof_offset = 0;
	}

	dev_sof = (dev_sof + stream->clock.sof_offset) & 2047;

	spin_lock_irqsave(&stream->clock.lock, flags);

	sample = &stream->clock.samples[stream->clock.head];
	sample->dev_stc = get_unaligned_le32(&data[header_size - 6]);
	sample->dev_sof = dev_sof;
	sample->host_sof = host_sof;
	sample->host_ts = ts;

	/* Update the sliding window head and count. */
	stream->clock.head = (stream->clock.head + 1) % stream->clock.size;

	if (stream->clock.count < stream->clock.size)
		stream->clock.count++;

	spin_unlock_irqrestore(&stream->clock.lock, flags);
}

static void uvc_video_clock_reset(struct uvc_streaming *stream)
{
	struct uvc_clock *clock = &stream->clock;

	clock->head = 0;
	clock->count = 0;
	clock->last_sof = -1;
	clock->sof_offset = -1;
}

static int uvc_video_clock_init(struct uvc_streaming *stream)
{
	struct uvc_clock *clock = &stream->clock;

	spin_lock_init(&clock->lock);
	clock->size = 32;

	clock->samples = kmalloc(clock->size * sizeof(*clock->samples),
				 GFP_KERNEL);
	if (clock->samples == NULL)
		return -ENOMEM;

	uvc_video_clock_reset(stream);

	return 0;
}

static void uvc_video_clock_cleanup(struct uvc_streaming *stream)
{
	kfree(stream->clock.samples);
	stream->clock.samples = NULL;
}

/*
 * uvc_video_clock_host_sof - Return the host SOF value for a clock sample
 *
 * Host SOF counters reported by usb_get_current_frame_number() usually don't
 * cover the whole 11-bits SOF range (0-2047) but are limited to the HCI frame
 * schedule window. They can be limited to 8, 9 or 10 bits depending on the host
 * controller and its configuration.
 *
 * We thus need to recover the SOF value corresponding to the host frame number.
 * As the device and host frame numbers are sampled in a short interval, the
 * difference between their values should be equal to a small delta plus an
 * integer multiple of 256 caused by the host frame number limited precision.
 *
 * To obtain the recovered host SOF value, compute the small delta by masking
 * the high bits of the host frame counter and device SOF difference and add it
 * to the device SOF value.
 */
static u16 uvc_video_clock_host_sof(const struct uvc_clock_sample *sample)
{
	/* The delta value can be negative. */
	s8 delta_sof;

	delta_sof = (sample->host_sof - sample->dev_sof) & 255;

	return (sample->dev_sof + delta_sof) & 2047;
}

/*
 * uvc_video_clock_update - Update the buffer timestamp
 *
 * This function converts the buffer PTS timestamp to the host clock domain by
 * going through the USB SOF clock domain and stores the result in the V4L2
 * buffer timestamp field.
 *
 * The relationship between the device clock and the host clock isn't known.
 * However, the device and the host share the common USB SOF clock which can be
 * used to recover that relationship.
 *
 * The relationship between the device clock and the USB SOF clock is considered
 * to be linear over the clock samples sliding window and is given by
 *
 * SOF = m * PTS + p
 *
 * Several methods to compute the slope (m) and intercept (p) can be used. As
 * the clock drift should be small compared to the sliding window size, we
 * assume that the line that goes through the points at both ends of the window
 * is a good approximation. Naming those points P1 and P2, we get
 *
 * SOF = (SOF2 - SOF1) / (STC2 - STC1) * PTS
 *     + (SOF1 * STC2 - SOF2 * STC1) / (STC2 - STC1)
 *
 * or
 *
 * SOF = ((SOF2 - SOF1) * PTS + SOF1 * STC2 - SOF2 * STC1) / (STC2 - STC1)   (1)
 *
 * to avoid loosing precision in the division. Similarly, the host timestamp is
 * computed with
 *
 * TS = ((TS2 - TS1) * PTS + TS1 * SOF2 - TS2 * SOF1) / (SOF2 - SOF1)	     (2)
 *
 * SOF values are coded on 11 bits by USB. We extend their precision with 16
 * decimal bits, leading to a 11.16 coding.
 *
 * TODO: To avoid surprises with device clock values, PTS/STC timestamps should
 * be normalized using the nominal device clock frequency reported through the
 * UVC descriptors.
 *
 * Both the PTS/STC and SOF counters roll over, after a fixed but device
 * specific amount of time for PTS/STC and after 2048ms for SOF. As long as the
 * sliding window size is smaller than the rollover period, differences computed
 * on unsigned integers will produce the correct result. However, the p term in
 * the linear relations will be miscomputed.
 *
 * To fix the issue, we subtract a constant from the PTS and STC values to bring
 * PTS to half the 32 bit STC range. The sliding window STC values then fit into
 * the 32 bit range without any rollover.
 *
 * Similarly, we add 2048 to the device SOF values to make sure that the SOF
 * computed by (1) will never be smaller than 0. This offset is then compensated
 * by adding 2048 to the SOF values used in (2). However, this doesn't prevent
 * rollovers between (1) and (2): the SOF value computed by (1) can be slightly
 * lower than 4096, and the host SOF counters can have rolled over to 2048. This
 * case is handled by subtracting 2048 from the SOF value if it exceeds the host
 * SOF value at the end of the sliding window.
 *
 * Finally we subtract a constant from the host timestamps to bring the first
 * timestamp of the sliding window to 1s.
 */
void uvc_video_clock_update(struct uvc_streaming *stream,
			    struct v4l2_buffer *v4l2_buf,
			    struct uvc_buffer *buf)
{
	struct uvc_clock *clock = &stream->clock;
	struct uvc_clock_sample *first;
	struct uvc_clock_sample *last;
	unsigned long flags;
	struct timespec64 ts;
	u32 delta_stc;
	u32 y1, y2;
	u32 x1, x2;
	u32 mean;
	u32 sof;
	u32 div;
	u32 rem;
	u64 y;

	spin_lock_irqsave(&clock->lock, flags);

	if (clock->count < clock->size)
		goto done;

	first = &clock->samples[clock->head];
	last = &clock->samples[(clock->head - 1) % clock->size];

	/* First step, PTS to SOF conversion. */
	delta_stc = buf->pts - (1UL << 31);
	x1 = first->dev_stc - delta_stc;
	x2 = last->dev_stc - delta_stc;
	if (x1 == x2)
		goto done;

	y1 = (first->dev_sof + 2048) << 16;
	y2 = (last->dev_sof + 2048) << 16;
	if (y2 < y1)
		y2 += 2048 << 16;

	y = (u64)(y2 - y1) * (1ULL << 31) + (u64)y1 * (u64)x2
	  - (u64)y2 * (u64)x1;
	y = div_u64(y, x2 - x1);

	sof = y;

	uvc_trace(UVC_TRACE_CLOCK, "%s: PTS %u y %llu.%06llu SOF %u.%06llu "
		  "(x1 %u x2 %u y1 %u y2 %u SOF offset %u)\n",
		  stream->dev->name, buf->pts,
		  y >> 16, div_u64((y & 0xffff) * 1000000, 65536),
		  sof >> 16, div_u64(((u64)sof & 0xffff) * 1000000LLU, 65536),
		  x1, x2, y1, y2, clock->sof_offset);

	/* Second step, SOF to host clock conversion. */
	x1 = (uvc_video_clock_host_sof(first) + 2048) << 16;
	x2 = (uvc_video_clock_host_sof(last) + 2048) << 16;
	if (x2 < x1)
		x2 += 2048 << 16;
	if (x1 == x2)
		goto done;

	ts = timespec64_sub(last->host_ts, first->host_ts);
	y1 = NSEC_PER_SEC;
	y2 = (ts.tv_sec + 1) * NSEC_PER_SEC + ts.tv_nsec;

	/* Interpolated and host SOF timestamps can wrap around at slightly
	 * different times. Handle this by adding or removing 2048 to or from
	 * the computed SOF value to keep it close to the SOF samples mean
	 * value.
	 */
	mean = (x1 + x2) / 2;
	if (mean - (1024 << 16) > sof)
		sof += 2048 << 16;
	else if (sof > mean + (1024 << 16))
		sof -= 2048 << 16;

	y = (u64)(y2 - y1) * (u64)sof + (u64)y1 * (u64)x2
	  - (u64)y2 * (u64)x1;
	y = div_u64(y, x2 - x1);

	div = div_u64_rem(y, NSEC_PER_SEC, &rem);
	ts.tv_sec = first->host_ts.tv_sec - 1 + div;
	ts.tv_nsec = first->host_ts.tv_nsec + rem;
	if (ts.tv_nsec >= NSEC_PER_SEC) {
		ts.tv_sec++;
		ts.tv_nsec -= NSEC_PER_SEC;
	}

	uvc_trace(UVC_TRACE_CLOCK, "%s: SOF %u.%06llu y %llu ts %lld.%06lld "
		  "buf ts %lld.%06lld (x1 %u/%u/%u x2 %u/%u/%u y1 %u y2 %u)\n",
		  stream->dev->name,
		  sof >> 16, div_u64(((u64)sof & 0xffff) * 1000000LLU, 65536),
		  y, (long long)ts.tv_sec, (long long)(ts.tv_nsec / NSEC_PER_USEC),
		  (long long)v4l2_buf->timestamp.tv_sec, (long long)v4l2_buf->timestamp.tv_usec,
		  x1, first->host_sof, first->dev_sof,
		  x2, last->host_sof, last->dev_sof, y1, y2);

	/* Update the V4L2 buffer. */
	v4l2_buf->timestamp.tv_sec = ts.tv_sec;
	v4l2_buf->timestamp.tv_usec = ts.tv_nsec / NSEC_PER_USEC;

done:
	spin_unlock_irqrestore(&stream->clock.lock, flags);
}

/* ------------------------------------------------------------------------
 * Stream statistics
 */

static void uvc_video_stats_decode(struct uvc_streaming *stream,
		const __u8 *data, int len)
{
	unsigned int header_size;
	bool has_pts = false;
	bool has_scr = false;
	u16 scr_sof = 0;
	u32 scr_stc = 0;
	u32 pts = 0;

	if (stream->stats.stream.nb_frames == 0 &&
	    stream->stats.frame.nb_packets == 0)
		ktime_get_ts64(&stream->stats.stream.start_ts);

	switch (data[1] & (UVC_STREAM_PTS | UVC_STREAM_SCR)) {
	case UVC_STREAM_PTS | UVC_STREAM_SCR:
		header_size = 12;
		has_pts = true;
		has_scr = true;
		break;
	case UVC_STREAM_PTS:
		header_size = 6;
		has_pts = true;
		break;
	case UVC_STREAM_SCR:
		header_size = 8;
		has_scr = true;
		break;
	default:
		header_size = 2;
		break;
	}

	/* Check for invalid headers. */
	if (len < header_size || data[0] < header_size) {
		stream->stats.frame.nb_invalid++;
		return;
	}

	/* Extract the timestamps. */
	if (has_pts)
		pts = get_unaligned_le32(&data[2]);

	if (has_scr) {
		scr_stc = get_unaligned_le32(&data[header_size - 6]);
		scr_sof = get_unaligned_le16(&data[header_size - 2]);
	}

	/* Is PTS constant through the whole frame ? */
	if (has_pts && stream->stats.frame.nb_pts) {
		if (stream->stats.frame.pts != pts) {
			stream->stats.frame.nb_pts_diffs++;
			stream->stats.frame.last_pts_diff =
				stream->stats.frame.nb_packets;
		}
	}

	if (has_pts) {
		stream->stats.frame.nb_pts++;
		stream->stats.frame.pts = pts;
	}

	/* Do all frames have a PTS in their first non-empty packet, or before
	 * their first empty packet ?
	 */
	if (stream->stats.frame.size == 0) {
		if (len > header_size)
			stream->stats.frame.has_initial_pts = has_pts;
		if (len == header_size && has_pts)
			stream->stats.frame.has_early_pts = true;
	}

	/* Do the SCR.STC and SCR.SOF fields vary through the frame ? */
	if (has_scr && stream->stats.frame.nb_scr) {
		if (stream->stats.frame.scr_stc != scr_stc)
			stream->stats.frame.nb_scr_diffs++;
	}

	if (has_scr) {
		/* Expand the SOF counter to 32 bits and store its value. */
		if (stream->stats.stream.nb_frames > 0 ||
		    stream->stats.frame.nb_scr > 0)
			stream->stats.stream.scr_sof_count +=
				(scr_sof - stream->stats.stream.scr_sof) % 2048;
		stream->stats.stream.scr_sof = scr_sof;

		stream->stats.frame.nb_scr++;
		stream->stats.frame.scr_stc = scr_stc;
		stream->stats.frame.scr_sof = scr_sof;

		if (scr_sof < stream->stats.stream.min_sof)
			stream->stats.stream.min_sof = scr_sof;
		if (scr_sof > stream->stats.stream.max_sof)
			stream->stats.stream.max_sof = scr_sof;
	}

	/* Record the first non-empty packet number. */
	if (stream->stats.frame.size == 0 && len > header_size)
		stream->stats.frame.first_data = stream->stats.frame.nb_packets;

	/* Update the frame size. */
	stream->stats.frame.size += len - header_size;

	/* Update the packets counters. */
	stream->stats.frame.nb_packets++;
	if (len > header_size)
		stream->stats.frame.nb_empty++;

	if (data[1] & UVC_STREAM_ERR)
		stream->stats.frame.nb_errors++;
}

static void uvc_video_stats_update(struct uvc_streaming *stream)
{
	struct uvc_stats_frame *frame = &stream->stats.frame;

	uvc_trace(UVC_TRACE_STATS, "frame %u stats: %u/%u/%u packets, "
		  "%u/%u/%u pts (%searly %sinitial), %u/%u scr, "
		  "last pts/stc/sof %u/%u/%u\n",
		  stream->sequence, frame->first_data,
		  frame->nb_packets - frame->nb_empty, frame->nb_packets,
		  frame->nb_pts_diffs, frame->last_pts_diff, frame->nb_pts,
		  frame->has_early_pts ? "" : "!",
		  frame->has_initial_pts ? "" : "!",
		  frame->nb_scr_diffs, frame->nb_scr,
		  frame->pts, frame->scr_stc, frame->scr_sof);

	stream->stats.stream.nb_frames++;
	stream->stats.stream.nb_packets += stream->stats.frame.nb_packets;
	stream->stats.stream.nb_empty += stream->stats.frame.nb_empty;
	stream->stats.stream.nb_errors += stream->stats.frame.nb_errors;
	stream->stats.stream.nb_invalid += stream->stats.frame.nb_invalid;

	if (frame->has_early_pts)
		stream->stats.stream.nb_pts_early++;
	if (frame->has_initial_pts)
		stream->stats.stream.nb_pts_initial++;
	if (frame->last_pts_diff <= frame->first_data)
		stream->stats.stream.nb_pts_constant++;
	if (frame->nb_scr >= frame->nb_packets - frame->nb_empty)
		stream->stats.stream.nb_scr_count_ok++;
	if (frame->nb_scr_diffs + 1 == frame->nb_scr)
		stream->stats.stream.nb_scr_diffs_ok++;

	memset(&stream->stats.frame, 0, sizeof(stream->stats.frame));
}

size_t uvc_video_stats_dump(struct uvc_streaming *stream, char *buf,
			    size_t size)
{
	unsigned int scr_sof_freq;
	unsigned int duration;
	struct timespec64 ts;
	size_t count = 0;

	ts.tv_sec = stream->stats.stream.stop_ts.tv_sec
		  - stream->stats.stream.start_ts.tv_sec;
	ts.tv_nsec = stream->stats.stream.stop_ts.tv_nsec
		   - stream->stats.stream.start_ts.tv_nsec;
	if (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000;
	}

	/* Compute the SCR.SOF frequency estimate. At the nominal 1kHz SOF
	 * frequency this will not overflow before more than 1h.
	 */
	duration = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	if (duration != 0)
		scr_sof_freq = stream->stats.stream.scr_sof_count * 1000
			     / duration;
	else
		scr_sof_freq = 0;

	count += scnprintf(buf + count, size - count,
			   "frames:  %u\npackets: %u\nempty:   %u\n"
			   "errors:  %u\ninvalid: %u\n",
			   stream->stats.stream.nb_frames,
			   stream->stats.stream.nb_packets,
			   stream->stats.stream.nb_empty,
			   stream->stats.stream.nb_errors,
			   stream->stats.stream.nb_invalid);
	count += scnprintf(buf + count, size - count,
			   "pts: %u early, %u initial, %u ok\n",
			   stream->stats.stream.nb_pts_early,
			   stream->stats.stream.nb_pts_initial,
			   stream->stats.stream.nb_pts_constant);
	count += scnprintf(buf + count, size - count,
			   "scr: %u count ok, %u diff ok\n",
			   stream->stats.stream.nb_scr_count_ok,
			   stream->stats.stream.nb_scr_diffs_ok);
	count += scnprintf(buf + count, size - count,
			   "sof: %u <= sof <= %u, freq %u.%03u kHz\n",
			   stream->stats.stream.min_sof,
			   stream->stats.stream.max_sof,
			   scr_sof_freq / 1000, scr_sof_freq % 1000);

	return count;
}

static void uvc_video_stats_start(struct uvc_streaming *stream)
{
	memset(&stream->stats, 0, sizeof(stream->stats));
	stream->stats.stream.min_sof = 2048;
}

static void uvc_video_stats_stop(struct uvc_streaming *stream)
{
	ktime_get_ts64(&stream->stats.stream.stop_ts);
}

/* ------------------------------------------------------------------------
 * Video codecs
 */

/* Video payload decoding is handled by uvc_video_decode_start(),
 * uvc_video_decode_data() and uvc_video_decode_end().
 *
 * uvc_video_decode_start is called with URB data at the start of a bulk or
 * isochronous payload. It processes header data and returns the header size
 * in bytes if successful. If an error occurs, it returns a negative error
 * code. The following error codes have special meanings.
 *
 * - EAGAIN informs the caller that the current video buffer should be marked
 *   as done, and that the function should be called again with the same data
 *   and a new video buffer. This is used when end of frame conditions can be
 *   reliably detected at the beginning of the next frame only.
 *
 * If an error other than -EAGAIN is returned, the caller will drop the current
 * payload. No call to uvc_video_decode_data and uvc_video_decode_end will be
 * made until the next payload. -ENODATA can be used to drop the current
 * payload if no other error code is appropriate.
 *
 * uvc_video_decode_data is called for every URB with URB data. It copies the
 * data to the video buffer.
 *
 * uvc_video_decode_end is called with header data at the end of a bulk or
 * isochronous payload. It performs any additional header data processing and
 * returns 0 or a negative error code if an error occurred. As header data have
 * already been processed by uvc_video_decode_start, this functions isn't
 * required to perform sanity checks a second time.
 *
 * For isochronous transfers where a payload is always transferred in a single
 * URB, the three functions will be called in a row.
 *
 * To let the decoder process header data and update its internal state even
 * when no video buffer is available, uvc_video_decode_start must be prepared
 * to be called with a NULL buf parameter. uvc_video_decode_data and
 * uvc_video_decode_end will never be called with a NULL buffer.
 */
static int uvc_video_decode_start(struct uvc_streaming *stream,
		struct uvc_buffer *buf, const __u8 *data, int len)
{
	__u8 fid;

	/* Sanity checks:
	 * - packet must be at least 2 bytes long
	 * - bHeaderLength value must be at least 2 bytes (see above)
	 * - bHeaderLength value can't be larger than the packet size.
	 */
	if (len < 2 || data[0] < 2 || data[0] > len) {
		stream->stats.frame.nb_invalid++;
		return -EINVAL;
	}

	fid = data[1] & UVC_STREAM_FID;

	/* Increase the sequence number regardless of any buffer states, so
	 * that discontinuous sequence numbers always indicate lost frames.
	 */
	if (stream->last_fid != fid) {
		stream->sequence++;
		if (stream->sequence)
			uvc_video_stats_update(stream);
	}

	uvc_video_clock_decode(stream, buf, data, len);
	uvc_video_stats_decode(stream, data, len);

	/* Store the payload FID bit and return immediately when the buffer is
	 * NULL.
	 */
	if (buf == NULL) {
		stream->last_fid = fid;
		return -ENODATA;
	}

	/* Mark the buffer as bad if the error bit is set. */
	if (data[1] & UVC_STREAM_ERR) {
		uvc_trace(UVC_TRACE_FRAME, "Marking buffer as bad (error bit "
			  "set).\n");
		buf->error = 1;
	}

	/* Synchronize to the input stream by waiting for the FID bit to be
	 * toggled when the the buffer state is not UVC_BUF_STATE_ACTIVE.
	 * stream->last_fid is initialized to -1, so the first isochronous
	 * frame will always be in sync.
	 *
	 * If the device doesn't toggle the FID bit, invert stream->last_fid
	 * when the EOF bit is set to force synchronisation on the next packet.
	 */
	if (buf->state != UVC_BUF_STATE_ACTIVE) {
		struct timespec64 ts;

		if (fid == stream->last_fid) {
			uvc_trace(UVC_TRACE_FRAME, "Dropping payload (out of "
				"sync).\n");
			if ((stream->dev->quirks & UVC_QUIRK_STREAM_NO_FID) &&
			    (data[1] & UVC_STREAM_EOF))
				stream->last_fid ^= UVC_STREAM_FID;
			return -ENODATA;
		}

		if (uvc_clock_param == CLOCK_MONOTONIC)
			ktime_get_ts64(&ts);
		else
			ktime_get_real_ts64(&ts);

		/* Set sequence number and timestamp directly on vb2_buffer */
		buf->buf.timestamp = (u64)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;

		/* TODO: Handle PTS and SCR. */
		buf->state = UVC_BUF_STATE_ACTIVE;
	}

	/* Mark the buffer as done if we're at the beginning of a new frame.
	 * End of frame detection is better implemented by checking the EOF
	 * bit (FID bit toggling is delayed by one frame compared to the EOF
	 * bit), but some devices don't set the bit at end of frame (and the
	 * last payload can be lost anyway). We thus must check if the FID has
	 * been toggled.
	 *
	 * stream->last_fid is initialized to -1, so the first isochronous
	 * frame will never trigger an end of frame detection.
	 *
	 * Empty buffers (bytesused == 0) don't trigger end of frame detection
	 * as it doesn't make sense to return an empty buffer. This also
	 * avoids detecting end of frame conditions at FID toggling if the
	 * previous payload had the EOF bit set.
	 */
	if (fid != stream->last_fid && buf->bytesused != 0) {
		uvc_trace(UVC_TRACE_FRAME, "Frame complete (FID bit "
				"toggled).\n");
		buf->state = UVC_BUF_STATE_READY;
		return -EAGAIN;
	}

	stream->last_fid = fid;

	return data[0];
}

static void uvc_video_decode_data(struct uvc_streaming *stream,
		struct uvc_buffer *buf, const __u8 *data, int len)
{
	unsigned int maxlen, nbytes;
	void *mem;

	if (len <= 0)
		return;

	/* Copy the video data to the buffer. */
	maxlen = buf->length - buf->bytesused;
	mem = buf->mem + buf->bytesused;
	nbytes = min((unsigned int)len, maxlen);
	memcpy(mem, data, nbytes);
	buf->bytesused += nbytes;

	/* Complete the current frame if the buffer size was exceeded. */
	if (len > maxlen) {
		uvc_trace(UVC_TRACE_FRAME, "Frame complete (overflow).\n");
		buf->state = UVC_BUF_STATE_READY;
	}
}

static void uvc_video_decode_end(struct uvc_streaming *stream,
		struct uvc_buffer *buf, const __u8 *data, int len)
{
	/* Mark the buffer as done if the EOF marker is set. */
	if (data[1] & UVC_STREAM_EOF && buf->bytesused != 0) {
		uvc_trace(UVC_TRACE_FRAME, "Frame complete (EOF found).\n");
		if (data[0] == len)
			uvc_trace(UVC_TRACE_FRAME, "EOF in empty payload.\n");
		buf->state = UVC_BUF_STATE_READY;
		if (stream->dev->quirks & UVC_QUIRK_STREAM_NO_FID)
			stream->last_fid ^= UVC_STREAM_FID;
	}
}

/* Video payload encoding is handled by uvc_video_encode_header() and
 * uvc_video_encode_data(). Only bulk transfers are currently supported.
 *
 * uvc_video_encode_header is called at the start of a payload. It adds header
 * data to the transfer buffer and returns the header size. As the only known
 * UVC output device transfers a whole frame in a single payload, the EOF bit
 * is always set in the header.
 *
 * uvc_video_encode_data is called for every URB and copies the data from the
 * video buffer to the transfer buffer.
 */
static int uvc_video_encode_header(struct uvc_streaming *stream,
		struct uvc_buffer *buf, __u8 *data, int len)
{
	data[0] = 2;	/* Header length */
	data[1] = UVC_STREAM_EOH | UVC_STREAM_EOF
		| (stream->last_fid & UVC_STREAM_FID);
	return 2;
}

static int uvc_video_encode_data(struct uvc_streaming *stream,
		struct uvc_buffer *buf, __u8 *data, int len)
{
	struct uvc_video_queue *queue = &stream->queue;
	unsigned int nbytes;
	void *mem;

	/* Copy video data to the URB buffer. */
	mem = buf->mem + queue->buf_used;
	nbytes = min((unsigned int)len, buf->bytesused - queue->buf_used);
	nbytes = min(stream->bulk.max_payload_size - stream->bulk.payload_size,
			nbytes);
	memcpy(data, mem, nbytes);

	queue->buf_used += nbytes;

	return nbytes;
}

/* ------------------------------------------------------------------------
 * URB handling
 */

/*
 * Completion handler for video URBs.
 */
static void uvc_video_decode_isoc(struct urb *urb, struct uvc_streaming *stream,
	struct uvc_buffer *buf)
{
	u8 *mem;
	int ret, i;

	int framesize;
	__u8 uLast1, uLast2;
	u8 *uLast5Data;
	bool bIsFound = false;
	int width,height;

	for (i = 0; i < urb->number_of_packets; ++i) {
		if (urb->iso_frame_desc[i].status < 0) {
			uvc_trace(UVC_TRACE_FRAME, "USB isochronous frame "
				"lost (%d).\n", urb->iso_frame_desc[i].status);
			/* Mark the buffer as faulty. */
			if (buf != NULL)
				buf->error = 1;
			continue;
		}

		/* Decode the payload header. */
		mem = urb->transfer_buffer + urb->iso_frame_desc[i].offset;
		do {
			ret = uvc_video_decode_start(stream, buf, mem,
				urb->iso_frame_desc[i].actual_length);
			if (ret == -EAGAIN)
				buf = uvc_queue_next_buffer(&stream->queue,
							    buf);
		} while (ret == -EAGAIN);

		if (ret < 0)
			continue;

		/* Decode the payload data. */
		uvc_video_decode_data(stream, buf, mem + ret,
			urb->iso_frame_desc[i].actual_length - ret);

		/* Process the header again. */
		uvc_video_decode_end(stream, buf, mem,
			urb->iso_frame_desc[i].actual_length);

		if (buf->state == UVC_BUF_STATE_READY)		
		{
			if (buf->length != buf->bytesused &&
			    !(stream->cur_format->flags &
			      UVC_FMT_FLAG_COMPRESSED))
				buf->error = 1;

			if(((stream->dev->RER_Chip == CHIP_RER9421)||(stream->dev->RER_Chip == CHIP_RER9422))&& 
				stream->cur_format->fcc == V4L2_PIX_FMT_H264)
			{
				mem = buf->mem+4;
				h264_decode_seq_parameter_set(mem, buf->bytesused, &width, &height);
				//printk("[w,h]=[%d,%d](%d)\n", width, height,  buf->bytesused);
				/* TODO: Store width/height metadata in kernel 5.10 compatible way */
				//buf->buf.v4l2_buf.reserved = ((width & 0xFFFF) << 16) | (height & 0xFFFF);
			}
#ifdef PATCH_OF_RER9420_MJPG_EOF_LOST 
			if(stream->dev->RER_Chip == CHIP_RER9420 && 
			   stream->cur_format->fcc == V4L2_PIX_FMT_MJPEG)
			  {				
				framesize  = buf->bytesused;				
				mem = buf->mem;
				uLast5Data = (mem + framesize-5);
				uLast1 = *(mem + framesize-1);
				uLast2 = *(mem + framesize-2);
				
				bIsFound = false;
				for(i=0; i<4 ; i++)
				{
					if(uLast5Data[i] == 0xFF &&
					   uLast5Data[i+1]== 0xD9)
					{
						bIsFound = true;
						break;
					}
				}
				if(!bIsFound)
				{
					uvc_trace(UVC_TRACE_FRAME, "Frame's EOI not found!!\n");
					if(uLast1 == 0xff) // append last EOF byte: 0xd9
					{
						*(mem+framesize) = 0xd9;
						framesize++;
					}
					else
					{
#define APPENDNUMBER 3 	// 2
#if (APPENDNUMBER == 3)	// Method 3.1
						__u8 EOF[APPENDNUMBER] = {0, 0xFF, 0xD9};
#else                   // Method 3.2
						__u8 EOF[APPENDNUMBER] = {0xFF, 0xD9};
#endif
						memcpy(mem+framesize, EOF, APPENDNUMBER);
						framesize += APPENDNUMBER;
					}
				}
				buf->bytesused = framesize;
			}
#if 1
			
			else if(((stream->dev->RER_Chip == CHIP_RER9421)||(stream->dev->RER_Chip == CHIP_RER9422))&& 
				stream->cur_format->fcc == V4L2_PIX_FMT_MJPEG)
			{
				u8 Y_Remove_Size = 0, *UV_Quant_Start, *Next_Marker; //yiling 2013-11-27
				#define MJPG_HEADER_9422_REMOVE_MAX_LEN		8
				mem = buf->mem;
				framesize=0;
				while((mem[0] != 0xFF) || (mem[1] != 0xD8))
				{
					mem++;
					framesize++;
					if(framesize>=MJPG_HEADER_9422_REMOVE_MAX_LEN)
					{
						framesize=0;
						break;
					} 
				}
				// Get JPEG Width and Height
				width = (*(mem+9)<<8)|(*(mem+10));
				height = (*(mem+7)<<8)|(*(mem+8));
				/* TODO: Store width/height metadata in kernel 5.10 compatible way */
				//buf->buf.v4l2_buf.reserved = ((width & 0xFFFF) << 16) | (height & 0xFFFF);

				if(framesize>0)
				{
					buf->bytesused -= framesize;
					memcpy(buf->mem, mem, buf->bytesused);
				}
				
				
				mem = buf->mem;
				while((mem[0] != 0xFF) || (mem[1] != 0xDB))
					mem++;
				UV_Quant_Start = mem + 0x2 + 0x43;
				Next_Marker = mem + 0x2 + 0x84;
				if(UV_Quant_Start[0]!=0x01)
				{
					mem = UV_Quant_Start;
					while((mem[0]!=0x01) && (mem<= (UV_Quant_Start+ 10)))
						mem++;
					if(mem[0]==0x01)
					{
						memcpy(UV_Quant_Start,  mem, 0x41);
						Y_Remove_Size = mem - UV_Quant_Start;
					}
				}
				if((Next_Marker[0]!=0xFF) || (Next_Marker[1]!=0xC4))
				{
					mem = Next_Marker;
					while(((mem[0] != 0xFF) || (mem[1] != 0xC4))&& mem<= (Next_Marker+ 20))
						mem++;
					if((mem[0] == 0xFF) && (mem[1] == 0xC4))
					{
						memcpy(Next_Marker,  mem, buf->bytesused - ((uintptr_t)mem - (uintptr_t)buf->mem)  );
						buf->bytesused -= (mem - Next_Marker);
					}
				}
				else
					buf->bytesused -= Y_Remove_Size;
			
				#define MJPG_EOF_9422_REMOVE_MAX_LEN 10
				if(buf->bytesused<MJPG_EOF_9422_REMOVE_MAX_LEN)
					/* TODO: Store error flag in kernel 5.10 compatible way */
					/*buf->buf.v4l2_buf.reserved |= 0x80000000*/;
				else
				{
					mem = buf->mem + buf->bytesused - 2 ;
					framesize = buf->bytesused;
					while(((mem[0] != 0xFF) || (mem[1] != 0xD9))&& (buf->bytesused-framesize)< MJPG_EOF_9422_REMOVE_MAX_LEN)
					{
						mem--;	 
						framesize --;
					}
					if(mem[0]==0xFF && mem[1]==0xD9)
						 buf->bytesused = framesize;
					mem = buf->mem;
					if(mem[0]!=0xFF || mem[1]!=0xD8 || mem[buf->bytesused-2]!=0xFF || mem[buf->bytesused-1]!=0xD9)
						/* TODO: Store error flag in kernel 5.10 compatible way */
						/*buf->buf.v4l2_buf.reserved |= 0x80000000*/;
				}
			
				
			}
			
#endif

#endif 
			buf = uvc_queue_next_buffer(&stream->queue, buf);
		}
	}
}

static void uvc_video_decode_bulk(struct urb *urb, struct uvc_streaming *stream,
	struct uvc_buffer *buf)
{
	u8 *mem;
	int len, ret;

	if (urb->actual_length == 0)
		return;

	mem = urb->transfer_buffer;
	len = urb->actual_length;
	stream->bulk.payload_size += len;

	/* If the URB is the first of its payload, decode and save the
	 * header.
	 */
	if (stream->bulk.header_size == 0 && !stream->bulk.skip_payload) {
		do {
			ret = uvc_video_decode_start(stream, buf, mem, len);
			if (ret == -EAGAIN)
				buf = uvc_queue_next_buffer(&stream->queue,
							    buf);
		} while (ret == -EAGAIN);

		/* If an error occurred skip the rest of the payload. */
		if (ret < 0 || buf == NULL) {
			stream->bulk.skip_payload = 1;
		} else {
			memcpy(stream->bulk.header, mem, ret);
			stream->bulk.header_size = ret;

			mem += ret;
			len -= ret;
		}
	}

	/* The buffer queue might have been cancelled while a bulk transfer
	 * was in progress, so we can reach here with buf equal to NULL. Make
	 * sure buf is never dereferenced if NULL.
	 */

	/* Process video data. */
	if (!stream->bulk.skip_payload && buf != NULL)
		uvc_video_decode_data(stream, buf, mem, len);

	/* Detect the payload end by a URB smaller than the maximum size (or
	 * a payload size equal to the maximum) and process the header again.
	 */
	if (urb->actual_length < urb->transfer_buffer_length ||
	    stream->bulk.payload_size >= stream->bulk.max_payload_size) {
		if (!stream->bulk.skip_payload && buf != NULL) {
			uvc_video_decode_end(stream, buf, stream->bulk.header,
				stream->bulk.payload_size);
			if (buf->state == UVC_BUF_STATE_READY)
				buf = uvc_queue_next_buffer(&stream->queue,
							    buf);
		}

		stream->bulk.header_size = 0;
		stream->bulk.skip_payload = 0;
		stream->bulk.payload_size = 0;
	}
}

static void uvc_video_encode_bulk(struct urb *urb, struct uvc_streaming *stream,
	struct uvc_buffer *buf)
{
	u8 *mem = urb->transfer_buffer;
	int len = stream->urb_size, ret;

	if (buf == NULL) {
		urb->transfer_buffer_length = 0;
		return;
	}

	/* If the URB is the first of its payload, add the header. */
	if (stream->bulk.header_size == 0) {
		ret = uvc_video_encode_header(stream, buf, mem, len);
		stream->bulk.header_size = ret;
		stream->bulk.payload_size += ret;
		mem += ret;
		len -= ret;
	}

	/* Process video data. */
	ret = uvc_video_encode_data(stream, buf, mem, len);

	stream->bulk.payload_size += ret;
	len -= ret;

	if (buf->bytesused == stream->queue.buf_used ||
	    stream->bulk.payload_size == stream->bulk.max_payload_size) {
		if (buf->bytesused == stream->queue.buf_used) {
			stream->queue.buf_used = 0;
			buf->state = UVC_BUF_STATE_READY;
			/* TODO: Handle sequence number in kernel 5.10 compatible way */
			/*buf->buf.v4l2_buf.sequence = ++stream->sequence;*/
			++stream->sequence;
			uvc_queue_next_buffer(&stream->queue, buf);
			stream->last_fid ^= UVC_STREAM_FID;
		}

		stream->bulk.header_size = 0;
		stream->bulk.payload_size = 0;
	}

	urb->transfer_buffer_length = stream->urb_size - len;
}

static void uvc_video_complete(struct urb *urb)
{
	struct uvc_streaming *stream = urb->context;
	struct uvc_video_queue *queue = &stream->queue;
	struct uvc_buffer *buf = NULL;
	unsigned long flags;
	int ret;

	switch (urb->status) {
	case 0:
		break;

	default:
		uvc_printk(KERN_WARNING, "Non-zero status (%d) in video "
			"completion handler.\n", urb->status);
		fallthrough;

	case -ENOENT:		/* usb_kill_urb() called. */
		if (stream->frozen)
			return;
		fallthrough;

	case -ECONNRESET:	/* usb_unlink_urb() called. */
	case -ESHUTDOWN:	/* The endpoint is being disabled. */
		uvc_queue_cancel(queue, urb->status == -ESHUTDOWN);
		return;
	}

	spin_lock_irqsave(&queue->irqlock, flags);
	if (!list_empty(&queue->irqqueue))
		buf = list_first_entry(&queue->irqqueue, struct uvc_buffer,
				       queue);
	spin_unlock_irqrestore(&queue->irqlock, flags);

	stream->decode(urb, stream, buf);

	if ((ret = usb_submit_urb(urb, GFP_ATOMIC)) < 0) {
		uvc_printk(KERN_ERR, "Failed to resubmit video URB (%d).\n",
			ret);
	}
}

/*
 * Free transfer buffers.
 */
static void uvc_free_urb_buffers(struct uvc_streaming *stream)
{
	unsigned int i;

	for (i = 0; i < UVC_URBS; ++i) {
		if (stream->urb_buffer[i]) {
#ifndef CONFIG_DMA_NONCOHERENT
			usb_free_coherent(stream->dev->udev, stream->urb_size,
				stream->urb_buffer[i], stream->urb_dma[i]);
#else
			kfree(stream->urb_buffer[i]);
#endif
			stream->urb_buffer[i] = NULL;
		}
	}

	stream->urb_size = 0;
}

/*
 * Allocate transfer buffers. This function can be called with buffers
 * already allocated when resuming from suspend, in which case it will
 * return without touching the buffers.
 *
 * Limit the buffer size to UVC_MAX_PACKETS bulk/isochronous packets. If the
 * system is too low on memory try successively smaller numbers of packets
 * until allocation succeeds.
 *
 * Return the number of allocated packets on success or 0 when out of memory.
 */
static int uvc_alloc_urb_buffers(struct uvc_streaming *stream,
	unsigned int size, unsigned int psize, gfp_t gfp_flags)
{
	unsigned int npackets;
	unsigned int i;

	/* Buffers are already allocated, bail out. */
	if (stream->urb_size)
		return stream->urb_size / psize;

	/* Compute the number of packets. Bulk endpoints might transfer UVC
	 * payloads across multiple URBs.
	 */
	npackets = DIV_ROUND_UP(size, psize);
	if (npackets > UVC_MAX_PACKETS)
		npackets = UVC_MAX_PACKETS;

	/* Retry allocations until one succeed. */
	for (; npackets > 1; npackets /= 2) {
		for (i = 0; i < UVC_URBS; ++i) {
			stream->urb_size = psize * npackets;
#ifndef CONFIG_DMA_NONCOHERENT
			stream->urb_buffer[i] = usb_alloc_coherent(
				stream->dev->udev, stream->urb_size,
				gfp_flags | __GFP_NOWARN, &stream->urb_dma[i]);
#else
			stream->urb_buffer[i] =
			    kmalloc(stream->urb_size, gfp_flags | __GFP_NOWARN);
#endif
			if (!stream->urb_buffer[i]) {
				uvc_free_urb_buffers(stream);
				break;
			}
		}

		if (i == UVC_URBS) {
			uvc_trace(UVC_TRACE_VIDEO, "Allocated %u URB buffers "
				"of %ux%u bytes each.\n", UVC_URBS, npackets,
				psize);
			return npackets;
		}
	}

	uvc_trace(UVC_TRACE_VIDEO, "Failed to allocate URB buffers (%u bytes "
		"per packet).\n", psize);
	return 0;
}

/*
 * Uninitialize isochronous/bulk URBs and free transfer buffers.
 */
static void uvc_uninit_video(struct uvc_streaming *stream, int free_buffers)
{
	struct urb *urb;
	unsigned int i;

	uvc_video_stats_stop(stream);

	for (i = 0; i < UVC_URBS; ++i) {
		urb = stream->urb[i];
		if (urb == NULL)
			continue;

		usb_kill_urb(urb);
		usb_free_urb(urb);
		stream->urb[i] = NULL;
	}

	if (free_buffers)
		uvc_free_urb_buffers(stream);
}

/*
 * Initialize isochronous URBs and allocate transfer buffers. The packet size
 * is given by the endpoint.
 */
static int uvc_init_video_isoc(struct uvc_streaming *stream,
	struct usb_host_endpoint *ep, gfp_t gfp_flags)
{
	struct urb *urb;
	unsigned int npackets, i, j;
	u16 psize;
	u32 size;

	psize = le16_to_cpu(ep->desc.wMaxPacketSize);
	psize = (psize & 0x07ff) * (1 + ((psize >> 11) & 3));
	size = stream->ctrl.dwMaxVideoFrameSize;

	npackets = uvc_alloc_urb_buffers(stream, size, psize, gfp_flags);
	if (npackets == 0)
		return -ENOMEM;

	size = npackets * psize;

	for (i = 0; i < UVC_URBS; ++i) {
		urb = usb_alloc_urb(npackets, gfp_flags);
		if (urb == NULL) {
			uvc_uninit_video(stream, 1);
			return -ENOMEM;
		}

		urb->dev = stream->dev->udev;
		urb->context = stream;
		urb->pipe = usb_rcvisocpipe(stream->dev->udev,
				ep->desc.bEndpointAddress);
#ifndef CONFIG_DMA_NONCOHERENT
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_dma = stream->urb_dma[i];
#else
		urb->transfer_flags = URB_ISO_ASAP;
#endif
		urb->interval = ep->desc.bInterval;
		urb->transfer_buffer = stream->urb_buffer[i];
		urb->complete = uvc_video_complete;
		urb->number_of_packets = npackets;
		urb->transfer_buffer_length = size;

		for (j = 0; j < npackets; ++j) {
			urb->iso_frame_desc[j].offset = j * psize;
			urb->iso_frame_desc[j].length = psize;
		}

		stream->urb[i] = urb;
	}

	return 0;
}

/*
 * Initialize bulk URBs and allocate transfer buffers. The packet size is
 * given by the endpoint.
 */
static int uvc_init_video_bulk(struct uvc_streaming *stream,
	struct usb_host_endpoint *ep, gfp_t gfp_flags)
{
	struct urb *urb;
	unsigned int npackets, pipe, i;
	u16 psize;
	u32 size;

	psize = le16_to_cpu(ep->desc.wMaxPacketSize) & 0x07ff;
	size = stream->ctrl.dwMaxPayloadTransferSize;
	stream->bulk.max_payload_size = size;

	npackets = uvc_alloc_urb_buffers(stream, size, psize, gfp_flags);
	if (npackets == 0)
		return -ENOMEM;

	size = npackets * psize;

	if (usb_endpoint_dir_in(&ep->desc))
		pipe = usb_rcvbulkpipe(stream->dev->udev,
				       ep->desc.bEndpointAddress);
	else
		pipe = usb_sndbulkpipe(stream->dev->udev,
				       ep->desc.bEndpointAddress);

	if (stream->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		size = 0;

	for (i = 0; i < UVC_URBS; ++i) {
		urb = usb_alloc_urb(0, gfp_flags);
		if (urb == NULL) {
			uvc_uninit_video(stream, 1);
			return -ENOMEM;
		}

		usb_fill_bulk_urb(urb, stream->dev->udev, pipe,
			stream->urb_buffer[i], size, uvc_video_complete,
			stream);
#ifndef CONFIG_DMA_NONCOHERENT
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_dma = stream->urb_dma[i];
#endif

		stream->urb[i] = urb;
	}

	return 0;
}

/*
 * Initialize isochronous/bulk URBs and allocate transfer buffers.
 */
static int uvc_init_video(struct uvc_streaming *stream, gfp_t gfp_flags)
{
	struct usb_interface *intf = stream->intf;
	struct usb_host_endpoint *ep;
	unsigned int i;
	int ret;

	stream->sequence = -1;
	stream->last_fid = -1;
	stream->bulk.header_size = 0;
	stream->bulk.skip_payload = 0;
	stream->bulk.payload_size = 0;

	uvc_video_stats_start(stream);

	if (intf->num_altsetting > 1) {
		struct usb_host_endpoint *best_ep = NULL;
		unsigned int best_psize = 3 * 1024;
		unsigned int bandwidth;
		unsigned int altsetting = 0;
		int intfnum = stream->intfnum;

		/* Isochronous endpoint, select the alternate setting. */
		bandwidth = stream->ctrl.dwMaxPayloadTransferSize;

		if (bandwidth == 0) {
			uvc_trace(UVC_TRACE_VIDEO, "Device requested null "
				"bandwidth, defaulting to lowest.\n");
			bandwidth = 1;
		} else {
			uvc_trace(UVC_TRACE_VIDEO, "Device requested %u "
				"B/frame bandwidth.\n", bandwidth);
		}

		for (i = 0; i < intf->num_altsetting; ++i) {
			struct usb_host_interface *alts;
			unsigned int psize;

			alts = &intf->altsetting[i];
			ep = uvc_find_endpoint(alts,
				stream->header.bEndpointAddress);
			if (ep == NULL)
				continue;

			/* Check if the bandwidth is high enough. */
			psize = le16_to_cpu(ep->desc.wMaxPacketSize);
			psize = (psize & 0x07ff) * (1 + ((psize >> 11) & 3));
			if (psize >= bandwidth && psize <= best_psize) {
				altsetting = i;
				best_psize = psize;
				best_ep = ep;
			}
		}

		if (best_ep == NULL) {
			uvc_trace(UVC_TRACE_VIDEO, "No fast enough alt setting "
				"for requested bandwidth.\n");
			return -EIO;
		}

		uvc_trace(UVC_TRACE_VIDEO, "Selecting alternate setting %u "
			"(%u B/frame bandwidth).\n", altsetting, best_psize);

		ret = usb_set_interface(stream->dev->udev, intfnum, altsetting);
		if (ret < 0)
			return ret;

		ret = uvc_init_video_isoc(stream, best_ep, gfp_flags);
	} else {
		/* Bulk endpoint, proceed to URB initialization. */
		ep = uvc_find_endpoint(&intf->altsetting[0],
				stream->header.bEndpointAddress);
		if (ep == NULL)
			return -EIO;

		ret = uvc_init_video_bulk(stream, ep, gfp_flags);
	}

	if (ret < 0)
		return ret;

	/* Submit the URBs. */
	for (i = 0; i < UVC_URBS; ++i) {
		ret = usb_submit_urb(stream->urb[i], gfp_flags);
		if (ret < 0) {
			uvc_printk(KERN_ERR, "Failed to submit URB %u "
					"(%d).\n", i, ret);
			uvc_uninit_video(stream, 1);
			return ret;
		}
	}

	return 0;
}

/* --------------------------------------------------------------------------
 * Suspend/resume
 */

/*
 * Stop streaming without disabling the video queue.
 *
 * To let userspace applications resume without trouble, we must not touch the
 * video buffers in any way. We mark the device as frozen to make sure the URB
 * completion handler won't try to cancel the queue when we kill the URBs.
 */
int uvc_video_suspend(struct uvc_streaming *stream)
{
	if (!uvc_queue_streaming(&stream->queue))
		return 0;

	stream->frozen = 1;
	uvc_uninit_video(stream, 0);
	usb_set_interface(stream->dev->udev, stream->intfnum, 0);
	return 0;
}

/*
 * Reconfigure the video interface and restart streaming if it was enabled
 * before suspend.
 *
 * If an error occurs, disable the video queue. This will wake all pending
 * buffers, making sure userspace applications are notified of the problem
 * instead of waiting forever.
 */
int uvc_video_resume(struct uvc_streaming *stream, int reset)
{
	int ret;

	/* If the bus has been reset on resume, set the alternate setting to 0.
	 * This should be the default value, but some devices crash or otherwise
	 * misbehave if they don't receive a SET_INTERFACE request before any
	 * other video control request.
	 */
	if (reset)
		usb_set_interface(stream->dev->udev, stream->intfnum, 0);

	stream->frozen = 0;

	uvc_video_clock_reset(stream);

	ret = uvc_commit_video(stream, &stream->ctrl);
	if (ret < 0) {
		uvc_queue_enable(&stream->queue, 0);
		return ret;
	}

	if (!uvc_queue_streaming(&stream->queue))
		return 0;

	ret = uvc_init_video(stream, GFP_NOIO);
	if (ret < 0)
		uvc_queue_enable(&stream->queue, 0);

	return ret;
}

/* ------------------------------------------------------------------------
 * Video device
 */

/*
 * Initialize the UVC video device by switching to alternate setting 0 and
 * retrieve the default format.
 *
 * Some cameras (namely the Fuji Finepix) set the format and frame
 * indexes to zero. The UVC standard doesn't clearly make this a spec
 * violation, so try to silently fix the values if possible.
 *
 * This function is called before registering the device with V4L.
 */
int uvc_video_init(struct uvc_streaming *stream)
{
	struct uvc_streaming_control *probe = &stream->ctrl;
	struct uvc_format *format = NULL;
	struct uvc_frame *frame = NULL;
	unsigned int i;
	int ret;

	if (stream->nformats == 0) {
		uvc_printk(KERN_INFO, "No supported video formats found.\n");
		return -EINVAL;
	}

	atomic_set(&stream->active, 0);

	/* Initialize the video buffers queue. */
	uvc_queue_init(&stream->queue, stream->type, !uvc_no_drop_param);

	/* Alternate setting 0 should be the default, yet the XBox Live Vision
	 * Cam (and possibly other devices) crash or otherwise misbehave if
	 * they don't receive a SET_INTERFACE request before any other video
	 * control request.
	 */
	usb_set_interface(stream->dev->udev, stream->intfnum, 0);

	/* Set the streaming probe control with default streaming parameters
	 * retrieved from the device. Webcams that don't suport GET_DEF
	 * requests on the probe control will just keep their current streaming
	 * parameters.
	 */
	if (uvc_get_video_ctrl(stream, probe, 1, UVC_GET_DEF) == 0)
		uvc_set_video_ctrl(stream, probe, 1);

	/* Initialize the streaming parameters with the probe control current
	 * value. This makes sure SET_CUR requests on the streaming commit
	 * control will always use values retrieved from a successful GET_CUR
	 * request on the probe control, as required by the UVC specification.
	 */
	ret = uvc_get_video_ctrl(stream, probe, 1, UVC_GET_CUR);
	if (ret < 0)
		return ret;

	/* Check if the default format descriptor exists. Use the first
	 * available format otherwise.
	 */
	for (i = stream->nformats; i > 0; --i) {
		format = &stream->format[i-1];
		if (format->index == probe->bFormatIndex)
			break;
	}

	if (format->nframes == 0) {
		uvc_printk(KERN_INFO, "No frame descriptor found for the "
			"default format.\n");
		return -EINVAL;
	}

	/* Zero bFrameIndex might be correct. Stream-based formats (including
	 * MPEG-2 TS and DV) do not support frames but have a dummy frame
	 * descriptor with bFrameIndex set to zero. If the default frame
	 * descriptor is not found, use the first available frame.
	 */
	for (i = format->nframes; i > 0; --i) {
		frame = &format->frame[i-1];
		if (frame->bFrameIndex == probe->bFrameIndex)
			break;
	}

	probe->bFormatIndex = format->index;
	probe->bFrameIndex = frame->bFrameIndex;

	stream->cur_format = format;
	stream->cur_frame = frame;

	/* Select the video decoding function */
	if (stream->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		if (stream->dev->quirks & UVC_QUIRK_BUILTIN_ISIGHT)
			stream->decode = uvc_video_decode_isight;
		else if (stream->intf->num_altsetting > 1)
			stream->decode = uvc_video_decode_isoc;
		else
			stream->decode = uvc_video_decode_bulk;
	} else {
		if (stream->intf->num_altsetting == 1)
			stream->decode = uvc_video_encode_bulk;
		else {
			uvc_printk(KERN_INFO, "Isochronous endpoints are not "
				"supported for video output devices.\n");
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * Enable or disable the video stream.
 */
int uvc_video_enable(struct uvc_streaming *stream, int enable)
{
	int ret;

	if (!enable) {
		uvc_uninit_video(stream, 1);
		usb_set_interface(stream->dev->udev, stream->intfnum, 0);
		uvc_queue_enable(&stream->queue, 0);
		uvc_video_clock_cleanup(stream);
		return 0;
	}

	ret = uvc_video_clock_init(stream);
	if (ret < 0)
		return ret;

	ret = uvc_queue_enable(&stream->queue, 1);
	if (ret < 0)
		goto error_queue;

	/* Commit the streaming parameters. */
	ret = uvc_commit_video(stream, &stream->ctrl);
	if (ret < 0)
		goto error_commit;

	ret = uvc_init_video(stream, GFP_KERNEL);
	if (ret < 0)
		goto error_video;

	return 0;

error_video:
	usb_set_interface(stream->dev->udev, stream->intfnum, 0);
error_commit:
	uvc_queue_enable(&stream->queue, 0);
error_queue:
	uvc_video_clock_cleanup(stream);

	return ret;
}
