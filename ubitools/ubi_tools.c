/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "ubi-user.h"
#include "ubi_tools.h"

int ubi_attach(const char *ubi_ctrl, int mtd_num, int dev_num)
{
	int fd, ret;
	struct ubi_attach_req req;

	fd = open(ubi_ctrl, O_RDWR);
	if (fd < 0)
		return -1;

	if (dev_num < 0)
		dev_num = UBI_DEV_NUM_AUTO;

	memset(&req, 0, sizeof(req));
	req.mtd_num = mtd_num;
	req.ubi_num = dev_num;

	ret = ioctl(fd, UBI_IOCATT, &req);

	close(fd);
	return ret;
}

int ubi_detach(const char *ubi_ctrl, int dev_num)
{
	int fd, ret;

	fd = open(ubi_ctrl, O_RDWR);
	if (fd < 0)
		return -1;

	ret = ioctl(fd, UBI_IOCDET, &dev_num);

	close(fd);
	return ret;
}

int ubi_mkvol(const char *ubi_ctrl, int dev_num, int vol_id, int size_bytes,
			int alignment, const char *vol_name, int vol_static)
{
	struct ubi_mkvol_req req;
	int vol_name_len;
	int fd, ret;

	vol_name_len = strlen(vol_name);
	if (vol_name_len > UBI_MAX_VOLUME_NAME)
		return -1;

	fd = open(ubi_ctrl, O_RDWR);
	if (fd < 0)
		return -1;

	if (vol_id < 0)
		vol_id = UBI_VOL_NUM_AUTO;

	memset(&req, 0, sizeof(req));
	req.vol_id = vol_id;
	if (vol_static)
		req.vol_type = UBI_STATIC_VOLUME;
	else
		req.vol_type = UBI_DYNAMIC_VOLUME;
	req.alignment = alignment;
	req.bytes = size_bytes;
	strncpy(req.name, vol_name, UBI_MAX_VOLUME_NAME);
	req.name_len = vol_name_len;

	ret = ioctl(fd, UBI_IOCMKVOL, &req);

	close(fd);
	return ret;
}

int ubi_rmvol(const char *ubi_ctrl, int vol_id)
{
	int fd, ret;

	fd = open(ubi_ctrl, O_RDWR);
	if (fd < 0)
		return -1;

	ret = ioctl(fd, UBI_IOCRMVOL, &vol_id);

	close(fd);
	return ret;
}

int ubi_rsvol(const char *ubi_ctrl, int vol_id, int size_bytes)
{
	struct ubi_rsvol_req req;
	int fd, ret;

	fd = open(ubi_ctrl, O_RDWR);
	if (fd < 0)
		return -1;

	memset(&req, 0, sizeof(req));
	req.bytes = size_bytes;
	req.vol_id = vol_id;

	ret = ioctl(fd, UBI_IOCRSVOL, &req);

	close(fd);
	return ret;
}

int ubi_updatevol(const char *ubi_ctrl, const char *image)
{
	long long bytes;
	int fd, ret;
	struct stat st;
	char buf[sizeof("/sys/class/ubi/ubi%d_%d/usable_eb_size") + 2 * sizeof(int)*3];
	int input_fd;
	unsigned ubinum, volnum;
	unsigned leb_size;
	ssize_t len;
	char *input_data;

	fd = open(ubi_ctrl, O_RDWR);
	if (fd < 0)
		return -1;

	if (image == NULL) {
		// truncate the volume by starting an update for size 0
		bytes = 0;
		ret = ioctl(fd, UBI_IOCVOLUP, &bytes);
		close(fd);
		return ret;
	}

	// Make assumption that device not is in normal format.
	// Removes need for scanning sysfs tree as full libubi does
	if (sscanf(ubi_ctrl, "/dev/ubi%u_%u", &ubinum, &volnum) != 2) {
		close(fd);
		return -1;
	}

	sprintf(buf, "/sys/class/ubi/ubi%u_%u/usable_eb_size", ubinum, volnum);
	input_fd = open(buf, O_RDONLY);
	if (input_fd < 0) {
		close(fd);
		return -1;
	}
	len = read(input_fd, buf, sizeof(buf));
	close(input_fd);
	if (len <= 0) {
		close(fd);
		return -1;
	}
	if (sscanf(buf, "%u", &leb_size) != 1) {
		close(fd);
		return -1;
	}

	stat(image, &st);
	bytes = st.st_size;
	input_fd = open(image, O_RDONLY);
	if (input_fd < 0) {
		close(fd);
		return -1;
	}

	ret = ioctl(fd, UBI_IOCVOLUP, &bytes);
	if (ret < 0) {
		close(input_fd);
		close(fd);
		return -1;
	}

	input_data = malloc(leb_size);
	while ((len = read(input_fd, input_data, leb_size)) > 0) {
		ret = write(fd, input_data, len);
		if (ret < 0)
			break;
	}
	if (len < 0)
		ret = -1;

	close(input_fd);
	close(fd);
	return ret;
}
