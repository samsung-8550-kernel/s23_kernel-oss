/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _DT_BINDINGS_CLK_QCOM_CAM_CC_SC8180X_H
#define _DT_BINDINGS_CLK_QCOM_CAM_CC_SC8180X_H

/* CAM_CC clocks */
#define CAM_CC_PLL0						0
#define CAM_CC_PLL0_OUT_EVEN					1
#define CAM_CC_PLL0_OUT_ODD					2
#define CAM_CC_PLL1						3
#define CAM_CC_PLL2						4
#define CAM_CC_PLL2_OUT_MAIN					5
#define CAM_CC_PLL3						6
#define CAM_CC_PLL4						7
#define CAM_CC_PLL5						8
#define CAM_CC_PLL6						9
#define CAM_CC_BPS_AHB_CLK					10
#define CAM_CC_BPS_AREG_CLK					11
#define CAM_CC_BPS_AXI_CLK					12
#define CAM_CC_BPS_CLK						13
#define CAM_CC_BPS_CLK_SRC					14
#define CAM_CC_CAMNOC_AXI_CLK					15
#define CAM_CC_CAMNOC_AXI_CLK_SRC				16
#define CAM_CC_CAMNOC_DCD_XO_CLK				17
#define CAM_CC_CCI_0_CLK					18
#define CAM_CC_CCI_0_CLK_SRC					19
#define CAM_CC_CCI_1_CLK					20
#define CAM_CC_CCI_1_CLK_SRC					21
#define CAM_CC_CCI_2_CLK					22
#define CAM_CC_CCI_2_CLK_SRC					23
#define CAM_CC_CCI_3_CLK					24
#define CAM_CC_CCI_3_CLK_SRC					25
#define CAM_CC_CORE_AHB_CLK					26
#define CAM_CC_CPAS_AHB_CLK					27
#define CAM_CC_CPHY_RX_CLK_SRC					28
#define CAM_CC_CSI0PHYTIMER_CLK					29
#define CAM_CC_CSI0PHYTIMER_CLK_SRC				30
#define CAM_CC_CSI1PHYTIMER_CLK					31
#define CAM_CC_CSI1PHYTIMER_CLK_SRC				32
#define CAM_CC_CSI2PHYTIMER_CLK					33
#define CAM_CC_CSI2PHYTIMER_CLK_SRC				34
#define CAM_CC_CSI3PHYTIMER_CLK					35
#define CAM_CC_CSI3PHYTIMER_CLK_SRC				36
#define CAM_CC_CSIPHY0_CLK					37
#define CAM_CC_CSIPHY1_CLK					38
#define CAM_CC_CSIPHY2_CLK					39
#define CAM_CC_CSIPHY3_CLK					40
#define CAM_CC_FAST_AHB_CLK_SRC					41
#define CAM_CC_FD_CORE_CLK					42
#define CAM_CC_FD_CORE_CLK_SRC					43
#define CAM_CC_FD_CORE_UAR_CLK					44
#define CAM_CC_GDSC_CLK						45
#define CAM_CC_ICP_AHB_CLK					46
#define CAM_CC_ICP_CLK						47
#define CAM_CC_ICP_CLK_SRC					48
#define CAM_CC_IFE_0_AXI_CLK					49
#define CAM_CC_IFE_0_CLK					50
#define CAM_CC_IFE_0_CLK_SRC					51
#define CAM_CC_IFE_0_CPHY_RX_CLK				52
#define CAM_CC_IFE_0_CSID_CLK					53
#define CAM_CC_IFE_0_CSID_CLK_SRC				54
#define CAM_CC_IFE_0_DSP_CLK					55
#define CAM_CC_IFE_1_AXI_CLK					56
#define CAM_CC_IFE_1_CLK					57
#define CAM_CC_IFE_1_CLK_SRC					58
#define CAM_CC_IFE_1_CPHY_RX_CLK				59
#define CAM_CC_IFE_1_CSID_CLK					60
#define CAM_CC_IFE_1_CSID_CLK_SRC				61
#define CAM_CC_IFE_1_DSP_CLK					62
#define CAM_CC_IFE_2_AXI_CLK					63
#define CAM_CC_IFE_2_CLK					64
#define CAM_CC_IFE_2_CLK_SRC					65
#define CAM_CC_IFE_2_CPHY_RX_CLK				66
#define CAM_CC_IFE_2_CSID_CLK					67
#define CAM_CC_IFE_2_CSID_CLK_SRC				68
#define CAM_CC_IFE_2_DSP_CLK					69
#define CAM_CC_IFE_3_AXI_CLK					70
#define CAM_CC_IFE_3_CLK					71
#define CAM_CC_IFE_3_CLK_SRC					72
#define CAM_CC_IFE_3_CPHY_RX_CLK				73
#define CAM_CC_IFE_3_CSID_CLK					74
#define CAM_CC_IFE_3_CSID_CLK_SRC				75
#define CAM_CC_IFE_3_DSP_CLK					76
#define CAM_CC_IFE_LITE_0_CLK					77
#define CAM_CC_IFE_LITE_0_CLK_SRC				78
#define CAM_CC_IFE_LITE_0_CPHY_RX_CLK				79
#define CAM_CC_IFE_LITE_0_CSID_CLK				80
#define CAM_CC_IFE_LITE_0_CSID_CLK_SRC				81
#define CAM_CC_IFE_LITE_1_CLK					82
#define CAM_CC_IFE_LITE_1_CLK_SRC				83
#define CAM_CC_IFE_LITE_1_CPHY_RX_CLK				84
#define CAM_CC_IFE_LITE_1_CSID_CLK				85
#define CAM_CC_IFE_LITE_1_CSID_CLK_SRC				86
#define CAM_CC_IFE_LITE_2_CLK					87
#define CAM_CC_IFE_LITE_2_CLK_SRC				88
#define CAM_CC_IFE_LITE_2_CPHY_RX_CLK				89
#define CAM_CC_IFE_LITE_2_CSID_CLK				90
#define CAM_CC_IFE_LITE_2_CSID_CLK_SRC				91
#define CAM_CC_IFE_LITE_3_CLK					92
#define CAM_CC_IFE_LITE_3_CLK_SRC				93
#define CAM_CC_IFE_LITE_3_CPHY_RX_CLK				94
#define CAM_CC_IFE_LITE_3_CSID_CLK				95
#define CAM_CC_IFE_LITE_3_CSID_CLK_SRC				96
#define CAM_CC_IPE_0_AHB_CLK					97
#define CAM_CC_IPE_0_AREG_CLK					98
#define CAM_CC_IPE_0_AXI_CLK					99
#define CAM_CC_IPE_0_CLK					100
#define CAM_CC_IPE_0_CLK_SRC					101
#define CAM_CC_IPE_1_AHB_CLK					102
#define CAM_CC_IPE_1_AREG_CLK					103
#define CAM_CC_IPE_1_AXI_CLK					104
#define CAM_CC_IPE_1_CLK					105
#define CAM_CC_JPEG_CLK						106
#define CAM_CC_JPEG_CLK_SRC					107
#define CAM_CC_LRME_CLK						108
#define CAM_CC_LRME_CLK_SRC					109
#define CAM_CC_MCLK0_CLK					110
#define CAM_CC_MCLK0_CLK_SRC					111
#define CAM_CC_MCLK1_CLK					112
#define CAM_CC_MCLK1_CLK_SRC					113
#define CAM_CC_MCLK2_CLK					114
#define CAM_CC_MCLK2_CLK_SRC					115
#define CAM_CC_MCLK3_CLK					116
#define CAM_CC_MCLK3_CLK_SRC					117
#define CAM_CC_MCLK4_CLK					118
#define CAM_CC_MCLK4_CLK_SRC					119
#define CAM_CC_MCLK5_CLK					120
#define CAM_CC_MCLK5_CLK_SRC					121
#define CAM_CC_MCLK6_CLK					122
#define CAM_CC_MCLK6_CLK_SRC					123
#define CAM_CC_MCLK7_CLK					124
#define CAM_CC_MCLK7_CLK_SRC					125
#define CAM_CC_SLEEP_CLK					126
#define CAM_CC_SLEEP_CLK_SRC					127
#define CAM_CC_SLOW_AHB_CLK_SRC					128
#define CAM_CC_XO_CLK_SRC					129

#endif