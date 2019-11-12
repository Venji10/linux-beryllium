/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2018, The Linux Foundation
 */

#include <linux/iopoll.h>

#include "dsi_phy.h"
#include "dsi.xml.h"

static int dsi_phy_hw_v4_0_is_pll_on(struct msm_dsi_phy *phy)
{
	void __iomem *base = phy->base;
	u32 data = 0;

	data = dsi_phy_read(base + REG_DSI_7nm_PHY_CMN_PLL_CNTRL);
	mb(); /* make sure read happened */

	return (data & BIT(0));
}

static void dsi_phy_hw_v4_0_config_lpcdrx(struct msm_dsi_phy *phy, bool enable)
{
	void __iomem *lane_base = phy->lane_base;
	int phy_lane_0 = 0;	/* TODO: Support all lane swap configs */

	/*
	 * LPRX and CDRX need to enabled only for physical data lane
	 * corresponding to the logical data lane 0
	 */
	if (enable)
		dsi_phy_write(lane_base +
			      REG_DSI_7nm_PHY_LN_LPRX_CTRL(phy_lane_0), 0x3);
	else
		dsi_phy_write(lane_base +
			      REG_DSI_7nm_PHY_LN_LPRX_CTRL(phy_lane_0), 0);
}

static void dsi_phy_hw_v4_0_lane_settings(struct msm_dsi_phy *phy)
{
	int i;
	u8 cfg2[] = { 0x0a, 0x0a, 0x0a, 0x0a, 0x8a };
	u8 tx_dctrl[] = { 0x00, 0x00, 0x00, 0x04, 0x01 };
	void __iomem *lane_base = phy->lane_base;

	/* Strength ctrl settings */
	for (i = 0; i < 5; i++) {
		/*
		 * Disable LPRX and CDRX for all lanes. And later on, it will
		 * be only enabled for the physical data lane corresponding
		 * to the logical data lane 0
		 */
		dsi_phy_write(lane_base + REG_DSI_7nm_PHY_LN_LPRX_CTRL(i), 0);
		dsi_phy_write(lane_base + REG_DSI_7nm_PHY_LN_PIN_SWAP(i), 0x0);
	}

	dsi_phy_hw_v4_0_config_lpcdrx(phy, true);

	/* other settings */
	for (i = 0; i < 5; i++) {
		dsi_phy_write(lane_base + REG_DSI_7nm_PHY_LN_CFG0(i), 0x0);
		dsi_phy_write(lane_base + REG_DSI_7nm_PHY_LN_CFG1(i), 0x0);
		dsi_phy_write(lane_base + REG_DSI_7nm_PHY_LN_CFG2(i), cfg2[i]);
		dsi_phy_write(lane_base + REG_DSI_7nm_PHY_LN_TX_DCTRL(i),
			      tx_dctrl[i]);
	}

	// TODO: if force_clk_lane_hs?
}

static int dsi_7nm_phy_enable(struct msm_dsi_phy *phy, int src_pll_id,
			      struct msm_dsi_phy_clk_request *clk_req)
{
	int ret;
	u32 status;
	u32 const delay_us = 5;
	u32 const timeout_us = 1000;
	struct msm_dsi_dphy_timing *timing = &phy->timing;
	void __iomem *base = phy->base;
	u32 data;

	DBG("");

	if (msm_dsi_dphy_timing_calc_v3(timing, clk_req)) {
		DRM_DEV_ERROR(&phy->pdev->dev,
			"%s: D-PHY timing calculation failed\n", __func__);
		return -EINVAL;
	}

	if (dsi_phy_hw_v4_0_is_pll_on(phy))
		pr_warn("PLL turned on before configuring PHY\n");

	/* wait for REFGEN READY */
	ret = readl_poll_timeout_atomic(base + REG_DSI_7nm_PHY_CMN_PHY_STATUS,
					status, (status & BIT(0)),
					delay_us, timeout_us);
	if (ret) {
		pr_err("Ref gen not ready. Aborting\n");
		return -EINVAL;
	}

	/* de-assert digital and pll power down */
	data = BIT(6) | BIT(5);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_CTRL_0, data);

	/* Assert PLL core reset */
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_PLL_CNTRL, 0x00);

	/* turn off resync FIFO */
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_RBUF_CTRL, 0x00);

	/* Configure PHY lane swap (TODO: we need to calculate this) */
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_LANE_CFG0, 0x21);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_LANE_CFG1, 0x84);

	/* Enable LDO */
	// TODO: less than 1500mhz check
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_VREG_CTRL_0, 0x59); // 0x5b
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_VREG_CTRL_1, 0x5c);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_CTRL_3, 0x00);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_GLBL_STR_SWI_CAL_SEL_CTRL, 0x00); // 0x03
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_GLBL_HSTX_STR_CTRL_0, 0x88); // 0x66
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_GLBL_PEMPH_CTRL_0, 0x00);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_GLBL_RESCODE_OFFSET_TOP_CTRL, 0x03);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_GLBL_RESCODE_OFFSET_BOT_CTRL, 0x3c);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_GLBL_LPTX_STR_CTRL, 0x55);

	/* Remove power down from all blocks */
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_CTRL_0, 0x7f);

	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_LANE_CTRL0, 0x1F);

	/* Select full-rate mode */
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_CTRL_2, 0x40);

	ret = msm_dsi_pll_set_usecase(phy->pll, phy->usecase);
	if (ret) {
		DRM_DEV_ERROR(&phy->pdev->dev, "%s: set pll usecase failed, %d\n",
			__func__, ret);
		return ret;
	}

	/* DSI PHY timings */
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_0,
		      timing->hs_halfbyte_en);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_1,
		      timing->clk_zero);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_2,
		      timing->clk_prepare);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_3,
		      timing->clk_trail);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_4,
		      timing->hs_exit);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_5,
		      timing->hs_zero);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_6,
		      timing->hs_prepare);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_7,
		      timing->hs_trail);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_8,
		      timing->hs_rqst);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_9,
		      2); // TODO timing->ta_go | (timing->ta_sure << 3));
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_10,
		      timing->ta_get);
	dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_11, 0x00);
	// TODO: WHY DOES THIS BREAK IT?
	//dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_12, 0x00);
	//dsi_phy_write(base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_13, 0x00);

	/* DSI lane settings */
	dsi_phy_hw_v4_0_lane_settings(phy);

	DBG("DSI%d PHY enabled", phy->id);

	return 0;
}

static void dsi_7nm_phy_disable(struct msm_dsi_phy *phy)
{
}

static int dsi_7nm_phy_init(struct msm_dsi_phy *phy)
{
	struct platform_device *pdev = phy->pdev;

	phy->lane_base = msm_ioremap(pdev, "dsi_phy_lane",
				     "DSI_PHY_LANE");
	if (IS_ERR(phy->lane_base)) {
		DRM_DEV_ERROR(&pdev->dev, "%s: failed to map phy lane base\n",
			__func__);
		return -ENOMEM;
	}

	return 0;
}

const struct msm_dsi_phy_cfg dsi_phy_7nm_cfgs = {
	.type = MSM_DSI_PHY_7NM,
	.src_pll_truthtable = { {false, false}, {true, false} },
	.reg_cfg = {
		.num = 1,
		.regs = {
			{"vdds", 36000, 32},
		},
	},
	.ops = {
		.enable = dsi_7nm_phy_enable,
		.disable = dsi_7nm_phy_disable,
		.init = dsi_7nm_phy_init,
	},
	.io_start = { 0xae94400, 0xae96400 },
	.num_dsi_phy = 2,
};
