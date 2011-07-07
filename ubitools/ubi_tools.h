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

#ifndef UBI_TOOLS_H_
#define UBI_TOOLS_H_

int ubi_attach(const char *ubi_ctrl, int mtd_num, int dev_num);
int ubi_detach(const char *ubi_ctrl, int dev_num);
int ubi_mkvol(const char *ubi_ctrl, int dev_num, int vol_id, int size_bytes,
			int alignment, const char *vol_name, int vol_static);
int ubi_rmvol(const char *ubi_ctrl, int vol_id);
int ubi_rsvol(const char *ubi_ctrl, int vol_id, int size_bytes);
int ubi_updatevol(const char *ubi_ctrl, const char *image);

#endif /* UBI_TOOLS_H_ */
