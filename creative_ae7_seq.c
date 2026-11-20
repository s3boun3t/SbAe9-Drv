// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sound Blaster AE-7 SB1800 rev. A, DSP and headphone sequence.
 * The CORB sequence numbers refer to the 2026-11-20 Windows capture.
 */

#include <linux/delay.h>
#include <sound/hda_verbs.h>

#include "creative_ae9.h"
#include "ca0132_regs.h"

#define AE7_CAD			1
#define AE7_HEADPHONE_PIN	0x10
#define AE7_HEADPHONE_CTL	(AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN)
#define AE7_DAC_GROUP		0x48
#define AE7_DAC_CONFIG		0x07
#define AE7_DAC_UNMUTED		0x80
#define AE7_DAC_MUTED		0x83
#define AE7_HEADPHONE_ATTN	0x18	/* 12 dB of DAC attenuation */
#define AE7_SR_48000		0x09
#define AE7_SR_96000		0x0b
#define AE7_DSP_INPUT		0x0c
#define AE7_DSP_OUTPUT		0x18

struct ae7_seq {
	struct creative_chip *chip;
	int err;
};

static u32 ae7_verb(struct ae7_seq *seq, u8 nid, u16 cmd, u16 value)
{
	u32 verb, response = 0;

	if (seq->err)
		return 0;
	verb = creative_ae9_verb(AE7_CAD, nid, cmd, value);
	seq->err = creative_ae9_corb_send_verb_sync(seq->chip, verb, &response);
	if (seq->err)
		dev_err(seq->chip->dev, "AE-7 verb %#010x failed: %d\n",
			verb, seq->err);
	return response;
}

static void ae7_chip_verb(struct ae7_seq *seq, u16 cmd, u16 value)
{
	ae7_verb(seq, WIDGET_CHIP_CTRL, cmd, value);
}

static void ae7_chip_write(struct ae7_seq *seq, u32 address, u32 value)
{
	if (seq->err)
		return;
	seq->err = creative_dsp_chipio_write(seq->chip, address, value);
	if (seq->err)
		dev_err(seq->chip->dev, "AE-7 ChipIO write %#x failed: %d\n",
			address, seq->err);
}

static void ae7_set_param_ex(struct ae7_seq *seq, u8 id, u8 value)
{
	ae7_chip_verb(seq, VENDOR_CHIPIO_STATUS, 0);
	ae7_chip_verb(seq, VENDOR_CHIPIO_PARAM_EX_ID_SET, id);
	ae7_chip_verb(seq, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, value);
}

static void ae7_set_param(struct ae7_seq *seq, u8 id, u8 value)
{
	if (value < 8)
		ae7_chip_verb(seq, VENDOR_CHIPIO_PARAM_SET, value << 5 | id);
	else
		ae7_set_param_ex(seq, id, value);
}

static void ae7_conn_rate(struct ae7_seq *seq, u8 conn, u8 rate)
{
	ae7_set_param_ex(seq, CONTROL_PARAM_CONN_POINT_ID, conn);
	ae7_set_param_ex(seq, CONTROL_PARAM_CONN_POINT_SR, rate);
}

static void ae7_stream_route(struct ae7_seq *seq, u8 id, u8 source, u8 dest)
{
	ae7_set_param(seq, CONTROL_PARAM_STREAM_ID, id);
	ae7_set_param(seq, CONTROL_PARAM_STREAM_SOURCE_CONN, source);
	ae7_set_param(seq, CONTROL_PARAM_STREAM_DEST_CONN, dest);
}

static void ae7_stream_start(struct ae7_seq *seq, u8 id)
{
	ae7_set_param_ex(seq, CONTROL_PARAM_STREAM_ID, id);
	ae7_set_param(seq, CONTROL_PARAM_STREAMS_CHANNELS, 6);
	ae7_set_param_ex(seq, CONTROL_PARAM_STREAM_ID, id);
	ae7_set_param(seq, CONTROL_PARAM_STREAM_CONTROL, 1);
}

static void ae7_pll_write(struct ae7_seq *seq, u8 reg, u8 val)
{
	ae7_chip_verb(seq, VENDOR_CHIPIO_8051_ADDRESS_LOW, reg);
	ae7_chip_verb(seq, VENDOR_CHIPIO_PLL_PMU_WRITE, val);
}

static void ae7_xram_write(struct ae7_seq *seq, u16 addr, const u8 *data,
			   size_t len)
{
	size_t i;

	ae7_chip_verb(seq, VENDOR_CHIPIO_8051_ADDRESS_LOW, addr & 0xff);
	ae7_chip_verb(seq, VENDOR_CHIPIO_8051_ADDRESS_HIGH, addr >> 8);
	for (i = 0; i < len; i++)
		ae7_chip_verb(seq, VENDOR_CHIPIO_8051_DATA_WRITE, data[i]);
}

static void ae7_sfr_write(struct ae7_seq *seq, u8 reg, u8 val)
{
	ae7_chip_verb(seq, VENDOR_CHIPIO_8051_WRITE_DIRECT | val, reg);
}

static void ae7_scp_set(struct ae7_seq *seq, u8 module, u8 request, u32 value)
{
	u32 header = BIT(27) | (request << 17) | (0x20 << 8) | module;

	if (seq->err)
		return;
	seq->err = creative_dsp_scp_send(seq->chip, header, &value, 1);
	if (seq->err)
		dev_err(seq->chip->dev, "AE-7 SCP %#x/%#x failed: %d\n",
			module, request, seq->err);
}

static void ae7_bar2_command(struct creative_chip *chip, u8 command, u32 mode)
{
	creative_ext_writel(chip, 0x86c, 0);
	usleep_range(13000, 16000);
	creative_ext_writel(chip, 0x800, command);
	creative_ext_writel(chip, 0x86c, 1);
	usleep_range(15000, 18000);
	creative_ext_writel(chip, 0x800, command);
	creative_ext_writel(chip, 0x804, 0x57);
	creative_ext_writel(chip, 0x20c, mode);
}

/* MMIO lines 7320-7443: BAR2 setup ahead of the first controller reset. */
int creative_ae7_prepare_bar2(struct creative_chip *chip)
{
	unsigned int offset;

	if (creative_ext_readl(chip, 4) == U32_MAX)
		return -ENODEV;

	creative_ext_writel(chip, 0x400, 1);
	for (offset = 0x42c; offset <= 0x4ec; offset += 0x40)
		creative_ext_writel(chip, offset, 0);
	for (offset = 0x43c; offset <= 0x4fc; offset += 0x40)
		creative_ext_writel(chip, offset, 0);
	creative_ext_writel(chip, 0x408, 1);
	creative_ext_writel(chip, 0x100, 0x600);
	creative_ext_writel(chip, 0x410, 0x14);
	creative_ext_writel(chip, 0x40c, 1);
	creative_ext_writel(chip, 0x100, 0x60f);
	creative_ext_writel(chip, 0x100, 0x70f);
	creative_ext_writel(chip, 0x830, 0xaff);
	ae7_bar2_command(chip, 0x7b, 0x800000);
	creative_ext_writel(chip, 0x1c, 0x880680);
	creative_ext_writel(chip, 0x1c, 0x680);
	creative_ext_writel(chip, 0x1c, 0x880680);

	return creative_ext_readl(chip, 4) == U32_MAX ? -ENODEV : 0;
}

static int ae7_check_dac(struct creative_chip *chip, u8 reg, u8 expected)
{
	u32 raw[2];
	u8 value = creative_dsp_ca0113_read(chip, AE7_DAC_GROUP, reg, raw);

	if (value == expected)
		return 0;
	dev_err(chip->dev, "AE-7 DAC register %#x read %#x, expected %#x\n",
		reg, value, expected);
	return -EIO;
}

static void ae7_set_dac_mute(struct creative_chip *chip, bool muted)
{
	const u8 cmd[][3] = {
		{ AE7_DAC_GROUP, AE7_DAC_CONFIG,
		  muted ? AE7_DAC_MUTED : AE7_DAC_UNMUTED },
	};

	chip->dac_filter_bits = AE7_DAC_UNMUTED;
	creative_dsp_ca0113_list(chip, cmd, ARRAY_SIZE(cmd),
				muted ? "AE-7 DAC mute" : "AE-7 DAC unmute");
}

void creative_ae7_mute_headphones(struct creative_chip *chip)
{
	if (!chip->dac48_07 || creative_ext_readl(chip, 4) == U32_MAX)
		return;

	ae7_set_dac_mute(chip, true);
	creative_ext_writew(chip, 0x320, 1);
	creative_ext_writew(chip, 0x320, 0);
}

/* CORB 1358-1444, the same two 8051 patches as the desktop ca0132 path. */
static void ae7_setup_8051(struct ae7_seq *seq)
{
	static const u8 pll_patch[] = {
		0x75, 0xd3, 0x09, 0x53, 0xd4, 0xef, 0x75, 0xd3, 0x09,
		0x02, 0x37, 0x78,
	};
	static const u8 jump_patch[] = { 0x02, 0x68, 0x62 };

	ae7_pll_write(seq, 0x49, 0x88);
	ae7_xram_write(seq, 0xfef0, pll_patch, ARRAY_SIZE(pll_patch));
	ae7_sfr_write(seq, 0xce, 0x3c);
	ae7_sfr_write(seq, 0xc9, 0x75);
	ae7_sfr_write(seq, 0xce, 0x3d);
	ae7_sfr_write(seq, 0xc9, 0xb7);
	ae7_xram_write(seq, 0xfee8, jump_patch, ARRAY_SIZE(jump_patch));
	ae7_sfr_write(seq, 0xce, 0x3a);
	ae7_sfr_write(seq, 0xc9, 0x46);
	ae7_sfr_write(seq, 0xce, 0x3b);
	ae7_sfr_write(seq, 0xc9, 0xe8);
	ae7_xram_write(seq, 0x1920, (const u8[]){ 0x00 }, 1);
	ae7_sfr_write(seq, 0xce, 0x39);
	ae7_sfr_write(seq, 0xc9, 0x46);
	ae7_xram_write(seq, 0x09b7, (const u8[]){ 0x10 }, 1);
	ae7_xram_write(seq, 0x09af, (const u8[]){ 0x01, 0x05 }, 2);
	ae7_xram_write(seq, 0x0973, (const u8[]){ 0x14 }, 1);
}

static void ae7_dsp_init(struct ae7_seq *seq)
{
	unsigned int tries;
	u32 response;

	/* DSP_INIT may return busy (1). Stop if the transport itself failed. */
	for (tries = 0; tries < 100; tries++) {
		response = ae7_verb(seq, WIDGET_DSP_CTRL, VENDOR_DSPIO_DSP_INIT, 0);
		if (seq->err || response == 0)
			return;
		if (response != 1) {
			seq->err = -EIO;
			return;
		}
		msleep(10);
	}
	seq->err = -ETIMEDOUT;
}

int creative_ae7_pre_download(struct creative_chip *chip)
{
	static const u8 ramp[] = { 0x03, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff };
	static const u8 clocks[] = {
		CONTROL_FLAG_DSP_96KHZ, CONTROL_FLAG_DAC_96KHZ,
		CONTROL_FLAG_ADC_B_96KHZ, CONTROL_FLAG_ADC_C_96KHZ,
		CONTROL_FLAG_SRC_RATE_96KHZ,
	};
	static const u8 dac_prep[][3] = { { 0x30, 0x2e, 0x3f } };
	struct ae7_seq seq = { .chip = chip };
	unsigned int i;
	int ret;

	/* MMIO lines 8195-8407: BAR2 ramp, then mute the ESS DAC. */
	for (i = 0; i < ARRAY_SIZE(ramp); i++) {
		creative_ext_writeb(chip, 0x304, ramp[i]);
		if (i >= 3 && i <= 6)
			creative_ext_writeb(chip, 0x100, (0xf << (i - 2)) & 0xf);
	}
	ae7_bar2_command(chip, 0x6b, 0x800001);
	ae7_set_dac_mute(chip, true);
	ret = ae7_check_dac(chip, AE7_DAC_CONFIG, AE7_DAC_MUTED);
	if (ret)
		return ret;
	creative_dsp_ca0113_list(chip, dac_prep, ARRAY_SIZE(dac_prep),
				"AE-7 pre-download route");

	/* CORB 1154-1540: function reset, 8051 setup and DSP_INIT. */
	ae7_verb(&seq, 1, AC_VERB_SET_CODEC_RESET, 0);
	ae7_verb(&seq, 1, AC_VERB_SET_CODEC_RESET, 0);
	ae7_chip_verb(&seq, VENDOR_CHIPIO_CT_EXTENSIONS_ENABLE, 1);
	ae7_setup_8051(&seq);
	ae7_chip_write(&seq, 0x18b008, 0x000000f8);
	ae7_chip_write(&seq, 0x18b008, 0x000000f0);
	ae7_verb(&seq, WIDGET_DSP_CTRL, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN);
	ae7_dsp_init(&seq);
	ae7_set_param(&seq, CONTROL_PARAM_SPDIF1_SOURCE, 0);
	ae7_set_param(&seq, 0, 0);
	ae7_set_param(&seq, CONTROL_PARAM_VIP_SOURCE, 0);
	ae7_set_param(&seq, CONTROL_PARAM_PORTA_160OHM_GAIN, 6);
	ae7_set_param(&seq, CONTROL_PARAM_PORTD_160OHM_GAIN, 6);
	ae7_chip_verb(&seq, VENDOR_CHIPIO_FLAG_SET,
		      BIT(7) | CONTROL_FLAG_PORT_A_10KOHM_LOAD);
	ae7_chip_verb(&seq, VENDOR_CHIPIO_FLAG_SET,
		      BIT(7) | CONTROL_FLAG_PORT_D_10KOHM_LOAD);
	ae7_chip_write(&seq, 0x18b030, 0x00000027);
	ae7_chip_verb(&seq, 0x600, 0xffc4);
	ae7_verb(&seq, 1, 0x793, 0);
	ae7_verb(&seq, 1, 0x794, 0x53);
	ae7_chip_verb(&seq, VENDOR_CHIPIO_FLAG_SET, CONTROL_FLAG_SPDIF2OUT);
	ae7_conn_rate(&seq, 0x0a, AE7_SR_48000);
	ae7_conn_rate(&seq, 0x0b, AE7_SR_48000);

	/* CORB 2158-2196: clock and connection prefix to the FLS DMA load. */
	ae7_pll_write(&seq, 0, 0xff);
	for (i = 0; i < ARRAY_SIZE(clocks); i++)
		ae7_chip_verb(&seq, VENDOR_CHIPIO_FLAG_SET, BIT(7) | clocks[i]);
	ae7_conn_rate(&seq, 0x0a, AE7_SR_48000);
	ae7_conn_rate(&seq, 0x0b, AE7_SR_48000);
	ae7_chip_verb(&seq, VENDOR_CHIPIO_STATUS, 0);
	return seq.err;
}

static void ae7_setup_dsp_route(struct ae7_seq *seq)
{
	static const u32 remap[] = {
		0x1e0c0, 0x1e1c1, 0x1e4c2, 0x1e5c3, 0x1e2c4, 0x1e3c5,
		0x1e8c6, 0x1e9c7, 0x1ecc8, 0x1edc9, 0x1eaca, 0x1ebcb,
	};
	unsigned int i;

	/* CORB 6061-6609: enable and remap the DSP input, set output source. */
	ae7_chip_write(seq, 0x100e34, 0xf1000000);
	ae7_conn_rate(seq, 0x03, AE7_SR_96000);
	ae7_conn_rate(seq, 0x0c, AE7_SR_96000);
	ae7_scp_set(seq, 0x80, 0x00, 0x40400000);	/* 3.0f */
	ae7_conn_rate(seq, 0x05, AE7_SR_96000);
	ae7_conn_rate(seq, 0x0e, AE7_SR_96000);
	ae7_scp_set(seq, 0x80, 0x01, 0x40400000);
	ae7_stream_start(seq, AE7_DSP_INPUT);
	for (i = 0; i < ARRAY_SIZE(remap); i++)
		ae7_chip_write(seq, 0x190030 + i * 4, remap[i]);
	ae7_chip_write(seq, 0x19042c, 1);
	ae7_scp_set(seq, 0x96, 0x29, 0);
	ae7_scp_set(seq, 0x96, 0x2a, 0);
	ae7_scp_set(seq, 0x80, 0x0d, 0);
	ae7_scp_set(seq, 0x80, 0x0e, 0);
	ae7_scp_set(seq, 0x96, 0x3c, 0);
	ae7_scp_set(seq, 0x32, 0x00, 0x40000000);	/* 2.0f */
	ae7_scp_set(seq, 0x80, 0x01, 0);
}

static void ae7_set_asi_clocks(struct ae7_seq *seq)
{
	ae7_pll_write(seq, 0x41, 0xc8);
	ae7_pll_write(seq, 0x45, 0xcc);
	ae7_pll_write(seq, 0x40, 0xcb);
	ae7_pll_write(seq, 0x43, 0xc7);
	ae7_pll_write(seq, 0x51, 0x8d);
}

static void ae7_setup_asi(struct ae7_seq *seq)
{
	static const u8 serial_port[][3] = { { 0x30, 0x2b, 0x00 } };
	static const u8 dac_input[][3] = {
		{ 0x48, 0x01, 0x80 }, { 0x48, 0x0a, 0x05 },
	};
	static const u8 dac_clock[][3] = {
		{ 0x48, 0x0b, 0x12 }, { 0x48, 0x04, 0x00 },
		{ 0x48, 0x06, 0x48 }, { 0x48, 0x0a, 0x05 },
		{ 0x48, 0x0c, 0x5f },
	};
	struct creative_chip *chip = seq->chip;

	/* CORB 6615-6811: a six-slot internal DAC stream at 96 kHz. */
	ae7_sfr_write(seq, 0x93, 0x10);
	ae7_pll_write(seq, 0x44, 0xc2);
	ae7_set_param(seq, 3, 3);
	ae7_chip_verb(seq, VENDOR_CHIPIO_FLAG_SET,
		      BIT(7) | CONTROL_FLAG_ASI_96KHZ);
	ae7_chip_verb(seq, 0x724, 0x83);
	ae7_set_param(seq, CONTROL_PARAM_ASI, 0);
	ae7_verb(seq, 0x17, 0x794, 0);
	ae7_xram_write(seq, 0xfa92, (const u8[]){ 0x22 }, 1);
	ae7_set_asi_clocks(seq);
	ae7_chip_verb(seq, 0x725, 0x81);
	if (seq->err)
		return;
	creative_dsp_ca0113_list(chip, serial_port, ARRAY_SIZE(serial_port),
				"AE-7 DAC serial port");
	ae7_conn_rate(seq, 0x70, AE7_SR_96000);
	ae7_stream_route(seq, 0x05, 0x43, 0);
	ae7_stream_route(seq, AE7_DSP_OUTPUT, 0x09, 0xd0);
	ae7_conn_rate(seq, 0xd0, AE7_SR_96000);
	ae7_stream_start(seq, AE7_DSP_OUTPUT);
	ae7_set_param(seq, CONTROL_PARAM_ASI, 4);
	ae7_pll_write(seq, 0x43, 0xc7);
	ae7_chip_write(seq, 0x189000, 0x0001f101);
	ae7_chip_write(seq, 0x189004, 0x0001f101);
	ae7_chip_write(seq, 0x189024, 0x00014004);
	ae7_chip_write(seq, 0x189028, 0x0002000f);
	if (seq->err)
		return;
	creative_dsp_ca0113_list(chip, dac_input, ARRAY_SIZE(dac_input),
				"AE-7 DAC input");
	ae7_set_asi_clocks(seq);
	ae7_set_param(seq, CONTROL_PARAM_ASI, 7);
	if (seq->err)
		return;
	creative_dsp_ca0113_list(chip, dac_clock, ARRAY_SIZE(dac_clock),
				"AE-7 DAC clock");
}

int creative_ae7_route_headphones(struct creative_chip *chip)
{
	/* Windows headphone route and gain at 20:30:04.748-.943. */
	static const u8 headphones[][3] = {
		{ 0x30, 0x2e, 0x3f }, { 0x30, 0x30, 0x3f },
		{ 0x48, 0x0d, 0x00 }, { 0x48, 0x16, 0x00 },
		{ 0x48, 0x17, 0x00 }, { 0x48, 0x18, 0x00 },
		{ 0x48, 0x19, 0x02 },
		{ 0x48, 0x11, 0xff }, { 0x48, 0x12, 0x2c }, { 0x48, 0x13, 0xf5 },
		{ 0x48, 0x0f, AE7_HEADPHONE_ATTN }, { 0x48, 0x14, 0x32 },
		{ 0x48, 0x10, AE7_HEADPHONE_ATTN }, { 0x30, 0x32, 0x00 },
	};
	static const u8 effects[] = { 0x00, 0x02, 0x04, 0x07 };
	struct ae7_seq seq = { .chip = chip };
	unsigned int i;
	u32 pin;
	int ret;

	ae7_scp_set(&seq, 0x96, 0x3a, 0x3f800000);	/* mute */
	ae7_setup_dsp_route(&seq);
	ae7_setup_asi(&seq);
	for (i = 0; i < ARRAY_SIZE(effects); i++)
		ae7_scp_set(&seq, 0x96, effects[i], 0);

	ae7_verb(&seq, 0x02, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	ae7_verb(&seq, AE7_HEADPHONE_PIN, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	ae7_verb(&seq, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	ae7_verb(&seq, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	ae7_verb(&seq, 0x02, 0x797, 0);
	ae7_verb(&seq, 0x02, AC_VERB_SET_AMP_GAIN_MUTE, 0xa05a);
	ae7_verb(&seq, 0x02, 0x798, 0);
	ae7_verb(&seq, 0x02, AC_VERB_SET_AMP_GAIN_MUTE, 0x905a);
	if (seq.err)
		return seq.err;

	creative_dsp_ca0113_list(chip, headphones, ARRAY_SIZE(headphones),
				"AE-7 low gain headphone route");
	creative_ext_writew(chip, 0x320, 0x100);
	creative_ext_writew(chip, 0x320, 0x101);

	ae7_verb(&seq, AE7_HEADPHONE_PIN, AC_VERB_SET_PIN_WIDGET_CONTROL,
		 AE7_HEADPHONE_CTL);
	pin = ae7_verb(&seq, AE7_HEADPHONE_PIN, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	if (seq.err)
		return seq.err;
	if ((pin & AE7_HEADPHONE_CTL) != AE7_HEADPHONE_CTL) {
		dev_err(chip->dev, "AE-7 headphone pin control read %#x\n", pin);
		return -EIO;
	}

	ret = ae7_check_dac(chip, AE7_DAC_CONFIG, AE7_DAC_MUTED);
	if (ret)
		return ret;
	ret = ae7_check_dac(chip, 0x0f, AE7_HEADPHONE_ATTN);
	if (ret)
		return ret;
	ret = ae7_check_dac(chip, 0x10, AE7_HEADPHONE_ATTN);
	if (ret)
		return ret;

	/* Lift the DSP mute while the DAC is still muted. */
	ae7_scp_set(&seq, 0x96, 0x3a, 0);
	if (seq.err)
		return seq.err;
	ae7_set_dac_mute(chip, false);
	ret = ae7_check_dac(chip, AE7_DAC_CONFIG, AE7_DAC_UNMUTED);
	if (ret) {
		creative_ae7_mute_headphones(chip);
		return ret;
	}
	dev_info(chip->dev, "AE-7 headphone DAC attenuation: %u.%u dB per channel\n",
		 AE7_HEADPHONE_ATTN / 2, (AE7_HEADPHONE_ATTN & 1) * 5);
	return 0;
}
