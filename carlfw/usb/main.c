/*
 * carl9170 firmware - used by the ar9170 wireless device
 *
 * Copyright (c) 2000-2005 ZyDAS Technology Corporation
 * Copyright (c) 2007-2009 Atheros Communications, Inc.
 * Copyright	2009	Johannes Berg <johannes@sipsolutions.net>
 * Copyright	2009	Christian Lamparter <chunkeey@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

#include "carl9170.h"

#include "hostif.h"
#include "printf.h"
#include "timer.h"
#include "rom.h"
#include "gpio.h"
#include "shared/phy.h"

#ifdef CONFIG_CARL9170FW_DEBUG_USB
void usb_putc(const char c)
{
	fw.usb.put_buffer[fw.usb.put_index++] = (uint8_t) c;

	if (fw.usb.put_index == CARL9170_MAX_CMD_PAYLOAD_LEN || c == '\0') {
		fw.usb.put_buffer[fw.usb.put_index] = 0;

		send_cmd_to_host(__roundup(fw.usb.put_index, 4),
				 CARL9170_RSP_TEXT, fw.usb.put_index,
				 fw.usb.put_buffer);
		fw.usb.put_index = 0;
	}
}

void usb_print_hex_dump(const void *buf, int len)
{
	unsigned int offset = 0, block = 0;
	while (len > 0) {
		block = min(__roundup(len, 4), CARL9170_MAX_CMD_PAYLOAD_LEN);

		send_cmd_to_host(block, CARL9170_RSP_HEXDUMP, len,
				 (const uint8_t *) buf + offset);

		offset += block;
		len -= block;
	}
}
#endif /* CONFIG_CARL9170FW_DEBUG_USB */

/* grab a buffer from the interrupt in queue ring-buffer */
static struct carl9170_rsp *get_int_buf(void)
{
	struct carl9170_rsp *tmp;

	tmp = &fw.usb.int_buf[fw.usb.int_tail_index++];
	fw.usb.int_tail_index %= CARL9170_INT_RQ_CACHES;
	if (fw.usb.int_pending != CARL9170_INT_RQ_CACHES)
		fw.usb.int_pending++;

	return tmp;
}

/* Pop up data from Interrupt IN Queue to USB Response buffer */
static struct carl9170_rsp *dequeue_int_buf(unsigned int space)
{
	struct carl9170_rsp *tmp = NULL;

	if (fw.usb.int_pending > 0) {
		tmp = &fw.usb.int_buf[fw.usb.int_head_index];

		if ((unsigned int)(tmp->hdr.len + 8) > space)
			return NULL;

		fw.usb.int_head_index++;
		fw.usb.int_head_index %= CARL9170_INT_RQ_CACHES;
		fw.usb.int_pending--;
	}

	return tmp;
}

static void usb_data_in(void)
{
}

static void usb_reg_out(void)
{
	uint32_t *regaddr = (uint32_t *) &dma_mem.reserved.cmd;
	uint16_t usbfifolen, i;

	usb_reset_out();

	usbfifolen = getb(AR9170_USB_REG_EP4_BYTE_COUNT_LOW) |
		     getb(AR9170_USB_REG_EP4_BYTE_COUNT_HIGH) << 8;

	if (usbfifolen & 0x3)
		usbfifolen = (usbfifolen >> 2) + 1;
	else
		usbfifolen = usbfifolen >> 2;

	for (i = 0; i < usbfifolen; i++)
		*regaddr++ = get(AR9170_USB_REG_EP4_DATA);

	handle_cmd(get_int_buf());

	usb_trigger_in();
}

static void usb_status_in(void)
{
	struct carl9170_rsp *rsp;
	unsigned int rem, tlen, elen;

	if (!fw.usb.int_desc_available)
		return ;

	fw.usb.int_desc_available = 0;

	rem = AR9170_BLOCK_SIZE - AR9170_INT_MAGIC_HEADER_SIZE;
	tlen = AR9170_INT_MAGIC_HEADER_SIZE;

	usb_reset_in();

	while (fw.usb.int_pending) {
		rsp = dequeue_int_buf(rem);
		if (!rsp)
			break;

		elen = rsp->hdr.len + 4;

		memcpy(DESC_PAYLOAD_OFF(fw.usb.int_desc, tlen), rsp, elen);

		rem -= elen;
		tlen += elen;
	}

	if (tlen == AR9170_INT_MAGIC_HEADER_SIZE) {
		DBG("attempted to send an empty int response!\n");
		goto reclaim;
	}

	fw.usb.int_desc->totalLen = tlen;
	fw.usb.int_desc->dataSize = tlen;

	/* Put to UpQ */
	dma_put(&fw.pta.up_queue, fw.usb.int_desc);

	/* Trigger PTA UP DMA */
	set(AR9170_PTA_REG_UP_DMA_TRIGGER, 1);
	usb_trigger_out();

	return ;

reclaim:
	/* TODO: not sure what to do here */
	fw.usb.int_desc_available = 1;
}

void send_cmd_to_host(const uint8_t len, const uint8_t type,
		      const uint8_t ext, const uint8_t *body)
{
	struct carl9170_cmd *resp;

#ifdef CONFIG_CARL9170FW_DEBUG
	if (unlikely(len > sizeof(resp->data))) {
		DBG("CMD too long:%x %d\n", type, len);
		return ;
	}

	/* Element length must be a multiple of 4. */
	if (unlikely(len & 0x3)) {
		DBG("CMD length not mult. of 4:%x %d\n", type, len);
		return ;
	}
#endif /* CONFIG_CARL9170FW_DEBUG */

	resp = (struct carl9170_cmd *) get_int_buf();
	if (unlikely(resp == NULL)) {
		/* not very helpful for NON UART users */
		DBG("out of msg buffers\n");
		return ;
	}

	resp->hdr.len = len;
	resp->hdr.cmd = type;
	resp->hdr.ext = ext;

	memcpy(resp->data, body, len);
	usb_trigger_in();
}

/* Reset all the USB FIFO used for WLAN */
static void usb_reset_FIFO(void)
{
	uint32_t val;

	/*
	 * of course,
	 * simpley ORing AR9170_MAC_POWER_STATE_CTRL_RESET
	 * would be... I dunno, maybe: just to simple?
	 */

	val = get(AR9170_MAC_REG_POWER_STATE_CTRL);
	val |= AR9170_MAC_POWER_STATE_CTRL_RESET;
	set(AR9170_MAC_REG_POWER_STATE_CTRL, val);

	/* Reset USB FIFO */
	set(AR9170_PWR_REG_ADDA_BB, AR9170_PWR_ADDA_BB_USB_FIFO_RESET);
	set(AR9170_PWR_REG_ADDA_BB, 0x0);
}

/* Turn off ADDA/RF power, PLL */
static void turn_power_off(void)
{
	set(AR9170_PHY_REG_ACTIVE, AR9170_PHY_ACTIVE_DIS);
	set(AR9170_PHY_REG_ADC_CTL, 0xa0000000 |
	    AR9170_PHY_ADC_CTL_OFF_PWDADC | AR9170_PHY_ADC_CTL_OFF_PWDDAC);

	set(AR9170_GPIO_REG_PORT_DATA, 0);
	set(AR9170_GPIO_REG_PORT_TYPE, 0xf);

	set(AR9170_PWR_REG_BASE, 0x40021);
	set(AR9170_PWR_REG_ADDA_BB, 0);

	clock_set(false, AHB_20_22MHZ);

	set(AR9170_PWR_REG_PLL_ADDAC, 0x5163);	/* 0x502b; */
	set(AR9170_PHY_REG_ADC_SERIAL_CTL, AR9170_PHY_ADC_SCTL_SEL_EXTERNAL_RADIO);
	set(0x1c589c, 0);	/* 7-0 */
	set(0x1c589c, 0);	/* 15-8 */
	set(0x1c589c, 0);	/* 23-16 */
	set(0x1c589c, 0);	/* 31- */
	set(0x1c589c, 0);	/* 39- */
	set(0x1c589c, 0);	/* 47- */
	set(0x1c589c, 0);	/* 55- */
	set(0x1c589c, 0xf8);	/* 63- */
	set(0x1c589c, 0x27);	/* 0x24;	71-	modified */
	set(0x1c589c, 0xf9);	/* 79- */
	set(0x1c589c, 0x90);	/* 87- */
	set(0x1c589c, 0x04);	/* 95- */
	set(0x1c589c, 0x48);	/* 103- */
	set(0x1c589c, 0x19);	/* 0;		111-	modified */
	set(0x1c589c, 0);	/* 119- */
	set(0x1c589c, 0);	/* 127- */
	set(0x1c589c, 0);	/* 135- */
	set(0x1c589c, 0);	/* 143- */
	set(0x1c589c, 0);	/* 151- */
	set(0x1c589c, 0x70);	/* 159- */
	set(0x1c589c, 0x0c);	/* 167- */
	set(0x1c589c, 0);	/* 175- */
	set(0x1c589c, 0);	/* 183-176 */
	set(0x1c589c, 0);	/* 191-184 */
	set(0x1c589c, 0);	/* 199- */
	set(0x1c589c, 0);	/* 207- */
	set(0x1c589c, 0);	/* 215- */
	set(0x1c589c, 0);	/* 223- */
	set(0x1c589c, 0);	/* 231- */
	set(0x1c58c4, 0);	/* 233- 232 */
	set(AR9170_PHY_REG_ADC_SERIAL_CTL, AR9170_PHY_ADC_SCTL_SEL_INTERNAL_ADDAC);
}

void __attribute__((noreturn)) reboot(void)
{
	/* turn off leds */
	led_set(0);

	/* write watchdog magic pattern for suspend  */
	andl(AR9170_PWR_REG_WATCH_DOG_MAGIC, 0xffff);
	orl(AR9170_PWR_REG_WATCH_DOG_MAGIC, 0x98760000);

	/* Disable watchdog */
	orl(AR9170_TIMER_REG_WATCH_DOG, 0xffff);

	/* Reset USB FIFO */
	usb_reset_FIFO();

	/* Turn off power */
	turn_power_off();

	/* add by ygwei for work around USB PHY chirp sequence problem */
	set(0x10f100, 0x12345678);

	/* Jump to boot code */
	jump_to_bootcode();
}

/* service USB events and re-enable USB interrupt */
static void usb_handler(uint8_t usb_interrupt_level1)
{
	uint8_t usb_interrupt_level2;

	if (usb_interrupt_level1 & BIT(5))
		usb_data_in();

	if (usb_interrupt_level1 & BIT(4))
		usb_reg_out();

	if (usb_interrupt_level1 & BIT(6))
		usb_status_in();

	if (usb_interrupt_level1 & BIT(0)) {
		usb_interrupt_level2 = getb(AR9170_USB_REG_INTR_SOURCE_0);

		if (usb_interrupt_level2 & BIT(0))
			usb_ep0setup();

		if (usb_interrupt_level2 & BIT(1))
			usb_ep0tx();

		if (usb_interrupt_level2 & BIT(2))
			usb_ep0rx();

		if (usb_interrupt_level2 & BIT(7)) {
			/* Clear the command abort interrupt */
			andb(AR9170_USB_REG_INTR_SOURCE_0, 0x7f);
		}

		if (usb_interrupt_level2 & BIT(3) ||
		    fw.usb.ep0_action & CARL9170_EP0_STALL) {
			/*
			 * transmission failure.
			 * stall ep 0
			 */
			setb(AR9170_USB_REG_CX_CONFIG_STATUS, BIT(2));
			fw.usb.ep0_action &= ~CARL9170_EP0_STALL;
		}

		if (usb_interrupt_level2 & BIT(4) ||
		    fw.usb.ep0_action & CARL9170_EP0_TRIGGER) {
			/*
			 * transmission done.
			 * set DONE bit.
			 */
			setb(AR9170_USB_REG_CX_CONFIG_STATUS, BIT(0));
			fw.usb.ep0_action &= ~CARL9170_EP0_TRIGGER;
		}
	}

	if (usb_interrupt_level1 & BIT(7)) {
		usb_interrupt_level2 = getb(AR9170_USB_REG_INTR_SOURCE_7);

		if (usb_interrupt_level2 & BIT(7))
			usb_data_out0Byte();

		if (usb_interrupt_level2 & BIT(6))
			usb_data_in0Byte();

		if (usb_interrupt_level2 & BIT(1)) {
			usb_reset_ack();
			reboot();
		}

		if (usb_interrupt_level2 & BIT(2)) {
			/* ACK USB suspend interrupt */
			usb_suspend_ack();

			/* Set GO_TO_SUSPEND bit to USB main control register */
			setb(AR9170_USB_REG_MAIN_CTRL, BIT(3));

			/* add by ygwei for work around USB PHY chirp sequence problem */
			set(0x10f100, 0x12345678);

			reboot();
		}

		if (usb_interrupt_level2 & BIT(3))
			usb_resume_ack();
	}
}

void handle_usb(void)
{
	uint8_t usb_interrupt_level1;

	usb_interrupt_level1 = getb(AR9170_USB_REG_INTR_GROUP);

	if (usb_interrupt_level1)
		usb_handler(usb_interrupt_level1);

	if (fw.usb.int_pending > 0)
		usb_trigger_in();
}

#ifdef CONFIG_CARL9170FW_USB_WATCHDOG
void usb_watchdog_timer(void)
{
	if (fw.usb.watchdog.state == cpu_to_le32(CARL9170_USB_WATCHDOG_INACTIVE))
		return;

	fw.usb.watchdog.state++;

	if (le32_to_cpu(fw.usb.watchdog.state) >= CARL9170_USB_WATCHDOG_TRIGGER_THRESHOLD) {
		for (;;) {
			/*
			 * Simply wait until the HW watchdog
			 * timer has elapsed.
			 */
		}
	}

	send_cmd_to_host(sizeof(fw.usb.watchdog), CARL9170_RSP_USB_WD,
			 0x80, (uint8_t *) &fw.usb.watchdog);
}
#endif /* CONFIG_CARL9170FW_USB_WATCHDOG */

