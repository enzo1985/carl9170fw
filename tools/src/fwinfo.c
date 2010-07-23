/*
 * Copyright 2010, Christian Lamparter <chunkeey@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <error.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#include "carlfw.h"

#include "compiler.h"

struct feature_list {
	unsigned int id;
	char name[64];
	void (*func)(const struct carl9170fw_desc_head *, struct carlfw *fw);
};

#define CHECK_FOR_FEATURE(feature_enum)					\
	{ .id = feature_enum, .name = #feature_enum, .func = NULL }

#define CHECK_FOR_FEATURE_FUNC(feature_enum, _func)			\
	{ .id = feature_enum, .name = #feature_enum, .func = _func }

static const struct feature_list known_otus_features_v1[] = {
	CHECK_FOR_FEATURE(CARL9170FW_DUMMY_FEATURE),
	CHECK_FOR_FEATURE(CARL9170FW_UNUSABLE),
	CHECK_FOR_FEATURE(CARL9170FW_COMMAND_PHY),
	CHECK_FOR_FEATURE(CARL9170FW_COMMAND_CAM),
	CHECK_FOR_FEATURE(CARL9170FW_WLANTX_CAB),
	CHECK_FOR_FEATURE(CARL9170FW_HANDLE_BACK_REQ),
	CHECK_FOR_FEATURE(CARL9170FW_GPIO_INTERRUPT),
	CHECK_FOR_FEATURE(CARL9170FW_PSM),
};

static void check_feature_list(const struct carl9170fw_desc_head *head,
			       const __le32 bitmap,
			       const struct feature_list *list,
			       const unsigned int entries,
			       struct carlfw *fw)
{
	unsigned int i;

	for (i = 0; i < entries; i++) {
		if (!carl9170fw_supports(bitmap, list[i].id))
			continue;

		fprintf(stdout, "\t\t%2d = %s\n", list[i].id, list[i].name);
		if (list[i].func)
			list[i].func(head, fw);
	}
}

static void show_otus_desc(const struct carl9170fw_desc_head *head,
			   struct carlfw *fw)
{
	const struct carl9170fw_otus_desc *otus = (const void *) head;

	BUILD_BUG_ON(ARRAY_SIZE(known_otus_features_v1) != __CARL9170FW_FEATURE_NUM);

	fprintf(stdout, "\tBeacon Address: %x, (reserved:%d Bytes)\n",
		le32_to_cpu(otus->bcn_addr), le16_to_cpu(otus->bcn_len));
	fprintf(stdout, "\tFirmware API Version: %d\n", otus->api_ver);
	fprintf(stdout, "\tSupported Firmware Interfaces: %d\n", otus->vif_num);

	fprintf(stdout, "\tSupported Features: (raw:%.08x)\n",
		le32_to_cpu(otus->fw_feature_set));

	check_feature_list(head, otus->fw_feature_set, known_otus_features_v1,
			   ARRAY_SIZE(known_otus_features_v1), fw);
}

static void show_miniboot_info(const struct carl9170fw_desc_head *head,
			       struct carlfw *fw __unused)
{
	const struct carl9170fw_usb_desc *usb = (const void *) head;

	fprintf(stdout, "\t\t\tminiboot size: %d Bytes\n", usb->miniboot_size);
}

static const struct feature_list known_usb_features_v1[] = {
	CHECK_FOR_FEATURE(CARL9170FW_USB_DUMMY_FEATURE),
	CHECK_FOR_FEATURE_FUNC(CARL9170FW_USB_MINIBOOT, show_miniboot_info),
	CHECK_FOR_FEATURE(CARL9170FW_USB_INIT_FIRMWARE),
	CHECK_FOR_FEATURE(CARL9170FW_USB_RESP_EP2),
	CHECK_FOR_FEATURE(CARL9170FW_USB_DOWN_STREAM),
	CHECK_FOR_FEATURE(CARL9170FW_USB_UP_STREAM),
	CHECK_FOR_FEATURE(CARL9170FW_USB_WATCHDOG),
};

static void show_usb_desc(const struct carl9170fw_desc_head *head,
			  struct carlfw *fw __unused)
{
	const struct carl9170fw_usb_desc *usb = (const void *) head;

	BUILD_BUG_ON(ARRAY_SIZE(known_usb_features_v1) != __CARL9170FW_USB_FEATURE_NUM);

	fprintf(stdout, "\tTX DMA chunk size:%d Bytes, TX DMA chunks:%d\n",
		usb->tx_frag_len, usb->tx_descs);
	fprintf(stdout, "\t=> %d Bytes are reserved for the TX queues\n",
		usb->tx_frag_len * usb->tx_descs);
	fprintf(stdout, "\tMax. RX stream block size:%d Bytes\n",
		usb->rx_max_frame_len);
	fprintf(stdout, "\tFirmware upload pointer: 0x%x\n",
		usb->fw_address);
	fprintf(stdout, "\tSupported Features: (raw:%.08x)\n",
		le32_to_cpu(usb->usb_feature_set));

	check_feature_list(head, usb->usb_feature_set, known_usb_features_v1,
			   ARRAY_SIZE(known_usb_features_v1), fw);
}

static void show_motd_desc(const struct carl9170fw_desc_head *head,
			   struct carlfw *fw __unused)
{
	const struct carl9170fw_motd_desc *motd = (const void *) head;
	char buf[CARL9170FW_MOTD_STRING_LEN];
	unsigned int fw_date;

	fw_date = motd->fw_year_month_day;
	fprintf(stdout, "\tFirmware Build Date (YYYY-MM-DD): 2%03d-%02d-%02d\n",
		CARL9170FW_GET_YEAR(fw_date), CARL9170FW_GET_MONTH(fw_date),
		CARL9170FW_GET_DAY(fw_date));

	strncpy(buf, motd->desc, CARL9170FW_MOTD_STRING_LEN);
	fprintf(stdout, "\tFirmware Text:\"%s\"\n", buf);

	strncpy(buf, motd->release, CARL9170FW_MOTD_STRING_LEN);
	fprintf(stdout, "\tFirmware Release:\"%s\"\n", buf);
}

static void show_fix_desc(const struct carl9170fw_desc_head *head,
			  struct carlfw *fw __unused)
{
	const struct carl9170fw_fix_desc *fix = (const void *) head;
	const struct carl9170fw_fix_entry *iter;
	unsigned int i;

	for (i = 0; i < (head->length - sizeof(*head)) / sizeof(*iter); i++) {
		iter = &fix->data[i];
		fprintf(stdout, "\t\t%d: 0x%.8x := 0x%.8x (0x%.8x)\n", i,
			le32_to_cpu(iter->address), le32_to_cpu(iter->value),
			le32_to_cpu(iter->mask));
	}
}

static void show_dbg_desc(const struct carl9170fw_desc_head *head,
			    struct carlfw *fw __unused)
{
	const struct carl9170fw_dbg_desc *dbg = (const void *) head;

	fprintf(stdout, "\tFirmware Debug Registers/Counters\n");
	fprintf(stdout, "\t\tbogoclock    = 0x%.8x\n",
		le32_to_cpu(dbg->bogoclock_addr));
	fprintf(stdout, "\t\tcounter      = 0x%.8x\n",
		le32_to_cpu(dbg->counter_addr));
	fprintf(stdout, "\t\trx total     = 0x%.8x\n",
		le32_to_cpu(dbg->rx_total_addr));
	fprintf(stdout, "\t\trx overrun   = 0x%.8x\n",
		le32_to_cpu(dbg->rx_overrun_addr));
	/* Nothing interesting here */
}

static void show_chk_desc(const struct carl9170fw_desc_head *head,
			    struct carlfw *fw __unused)
{
	const struct carl9170fw_chk_desc *chk = (const void *) head;

	fprintf(stdout, "\tFirmware Descriptor CRC32: %08x\n",
		le32_to_cpu(chk->hdr_crc32));
	fprintf(stdout, "\tFirmware Image CRC32: %08x\n",
		le32_to_cpu(chk->fw_crc32));
}

static void show_last_desc(const struct carl9170fw_desc_head *head,
			   struct carlfw *fw __unused)

{
	const struct carl9170fw_last_desc *last __unused = (const void *) head;

	/* Nothing here */
}

#define ADD_HANDLER(_magic, _func)				\
	{							\
	  .magic = _magic##_MAGIC,				\
	  .min_ver = CARL9170FW_## _magic##_DESC_CUR_VER,	\
	  .func = _func,					\
	  .size = CARL9170FW_## _magic##_DESC_SIZE,		\
	}

static const struct {
	uint8_t magic[4];
	uint8_t min_ver;
	void (*func)(const struct carl9170fw_desc_head *, struct carlfw *);
	uint16_t size;
} known_magics[] = {
	ADD_HANDLER(OTUS, show_otus_desc),
	ADD_HANDLER(USB, show_usb_desc),
	ADD_HANDLER(MOTD, show_motd_desc),
	ADD_HANDLER(DBG, show_dbg_desc),
	ADD_HANDLER(FIX, show_fix_desc),
	ADD_HANDLER(CHK, show_chk_desc),
	ADD_HANDLER(LAST, show_last_desc),
};

static const uint8_t otus_magic[4] = { OTUS_MAGIC };

static void show_desc_head(struct carl9170fw_desc_head *head)
{
#define P(c) (isprint(c) ? c :  ' ')

	fprintf(stdout, ">\t%c%c%c%c Descriptor: size:%d, compatible:%d, "
			"version:%d\n",
		P(head->magic[0]), P(head->magic[1]), P(head->magic[2]),
		P(head->magic[3]), le16_to_cpu(head->length), head->min_ver,
		head->cur_ver);
}

static void fwinfo_info(void)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\tfwinfo FW-FILE\n");

	fprintf(stderr, "\nDescription:\n");
	fprintf(stderr, "\tDisplay firmware descriptors information in "
			"a human readable form.\n");

	fprintf(stderr, "\nParameteres:\n");
	fprintf(stderr, "\t 'FW-FILE'	= firmware file/base-name\n\n");
}

int main(int argc, char *args[])
{
	struct carlfw *fw = NULL;
	struct carl9170fw_desc_head *fw_desc;
	unsigned int i;
	int err = 0;
	size_t len;

	if (argc != 2) {
		err = -EINVAL;
		goto out;
	}

	fw = carlfw_load(args[1]);
	if (IS_ERR_OR_NULL(fw)) {
		fprintf(stderr, "Failed to open firmware \"%s\" (%d).\n",
			args[1], (int)PTR_ERR(fw));
		goto out;
	}

	carlfw_get_fw(fw, &len);
	fprintf(stdout, "General Firmware Statistics:\n");
	fprintf(stdout, "\tFirmware file size: %u Bytes\n", len);
	fprintf(stdout, "\t%d Descriptors in %d Bytes\n",
		carlfw_get_descs_num(fw), carlfw_get_descs_size(fw));

	fw_desc = NULL;
	fprintf(stdout, "\nDetailed Descriptor Description:\n");
	while ((fw_desc = carlfw_desc_next(fw, fw_desc))) {
		show_desc_head(fw_desc);

		for (i = 0; i < ARRAY_SIZE(known_magics); i++) {
			if (carl9170fw_desc_cmp(fw_desc, known_magics[i].magic,
			    known_magics[i].size, known_magics[i].min_ver)) {
				known_magics[i].func(fw_desc, fw);
				break;
			}
		}

		if (i == ARRAY_SIZE(known_magics))
			fprintf(stderr, "Unknown Descriptor.\n");

		fprintf(stdout, "\n");
	}

out:
	switch (err) {
	case 0:
		break;

	case -EINVAL:
		fwinfo_info();
		break;

	default:
		fprintf(stderr, "%s\n", strerror(-err));
		break;
	}

	carlfw_release(fw);
	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}