/*-
 * bcm4313_phytbl_lcn.h -- BCM4313 LCN-PHY tuning tables, part 2.
 *
 * GENERATED FILE -- do not edit.  Regenerate with:
 *	perl gen_phytbl.pl > bcm4313_phytbl_lcn.h
 *
 * These arrays are extracted VERBATIM from the Linux brcmsmac driver
 * (drivers/net/wireless/broadcom/brcm80211/brcmsmac/phy/phytbl_lcn.c and
 * phy/phy_lcn.c), which are distributed under the ISC license.  They are
 * hardware RF tuning data with no licensing-independent source, so they are
 * reproduced byte-for-byte rather than hand-retyped.
 *
 * Tables that live in bcm4313_lcntab.h (the 4313 switch-control variants and
 * the rev0 RX-gain set) are not duplicated here; the baseband-init table
 * descriptor array (dot11lcnphytbl_info_rev0) references them by name.
 *
 * $FreeBSD$
 */
#ifndef	_BCM4313_PHYTBL_LCN_H_
#define	_BCM4313_PHYTBL_LCN_H_

#include <sys/types.h>
#include <sys/param.h>

#include "bcm4313_lcntab.h"	/* struct bcm4313_phytbl + RX-gain tables */

/* brcmsmac uses the Linux ARRAY_SIZE(); FreeBSD names it nitems(). */
#ifndef	ARRAY_SIZE
#define	ARRAY_SIZE(a)	nitems(a)
#endif

/* LCN-PHY 2.4GHz TX gain table entry (phytbl_lcn.h). */
struct lcnphy_tx_gain_tbl_entry {
	uint8_t	gm;
	uint8_t	pga;
	uint8_t	pad;
	uint8_t	dac;
	uint8_t	bb_mult;
};

/* Radio 2064 per-channel tuning parameters (phy_lcn.c). */
struct chan_info_2064_lcnphy {
	uint32_t	chan;
	uint32_t	freq;
	uint8_t	logen_buftune;
	uint8_t	logen_rccr_tx;
	uint8_t	txrf_mix_tune_ctrl;
	uint8_t	pa_input_tune_g;
	uint8_t	logen_rccr_rx;
	uint8_t	pa_rxrf_lna1_freq_tune;
	uint8_t	pa_rxrf_lna2_freq_tune;
	uint8_t	rxrf_rxrf_spare1;
};

/* One entry of the radio 2064 init register list (phy_lcn.c). */
struct lcnphy_radio_regs {
	uint16_t	address;
	uint8_t	init_a;
	uint8_t	init_g;
	uint8_t	do_init_a;
	uint8_t	do_init_g;
};

/* SFO center-frequency pair (phy_lcn.c). */
struct lcnphy_sfo_cfg {
	uint16_t	ptcentreTs20;
	uint16_t	ptcentreFactor;
};

/* Sample-playback calibration tone (phy_lcn.c). */
struct lcnphy_spb_tone {
	int16_t	re;
	int16_t	im;
};

/* TX IQ-calibration gain ladder entry (phy_lcn.c). */
typedef uint16_t iqcal_gain_params_lcnphy[9];

/* Radio 2064 register numbers (all of 0x000-0x130, values from
 * brcmsmac/phy/phy_radio.h). */
#define	RADIO_2064_REG000	0x0
#define	RADIO_2064_REG001	0x1
#define	RADIO_2064_REG002	0x2
#define	RADIO_2064_REG003	0x3
#define	RADIO_2064_REG004	0x4
#define	RADIO_2064_REG005	0x5
#define	RADIO_2064_REG006	0x6
#define	RADIO_2064_REG007	0x7
#define	RADIO_2064_REG008	0x8
#define	RADIO_2064_REG009	0x9
#define	RADIO_2064_REG00A	0xa
#define	RADIO_2064_REG00B	0xb
#define	RADIO_2064_REG00C	0xc
#define	RADIO_2064_REG00D	0xd
#define	RADIO_2064_REG00E	0xe
#define	RADIO_2064_REG00F	0xf
#define	RADIO_2064_REG010	0x10
#define	RADIO_2064_REG011	0x11
#define	RADIO_2064_REG012	0x12
#define	RADIO_2064_REG013	0x13
#define	RADIO_2064_REG014	0x14
#define	RADIO_2064_REG015	0x15
#define	RADIO_2064_REG016	0x16
#define	RADIO_2064_REG017	0x17
#define	RADIO_2064_REG018	0x18
#define	RADIO_2064_REG019	0x19
#define	RADIO_2064_REG01A	0x1a
#define	RADIO_2064_REG01B	0x1b
#define	RADIO_2064_REG01C	0x1c
#define	RADIO_2064_REG01D	0x1d
#define	RADIO_2064_REG01E	0x1e
#define	RADIO_2064_REG01F	0x1f
#define	RADIO_2064_REG020	0x20
#define	RADIO_2064_REG021	0x21
#define	RADIO_2064_REG022	0x22
#define	RADIO_2064_REG023	0x23
#define	RADIO_2064_REG024	0x24
#define	RADIO_2064_REG025	0x25
#define	RADIO_2064_REG026	0x26
#define	RADIO_2064_REG027	0x27
#define	RADIO_2064_REG028	0x28
#define	RADIO_2064_REG029	0x29
#define	RADIO_2064_REG02A	0x2a
#define	RADIO_2064_REG02B	0x2b
#define	RADIO_2064_REG02C	0x2c
#define	RADIO_2064_REG02D	0x2d
#define	RADIO_2064_REG02E	0x2e
#define	RADIO_2064_REG02F	0x2f
#define	RADIO_2064_REG030	0x30
#define	RADIO_2064_REG031	0x31
#define	RADIO_2064_REG032	0x32
#define	RADIO_2064_REG033	0x33
#define	RADIO_2064_REG034	0x34
#define	RADIO_2064_REG035	0x35
#define	RADIO_2064_REG036	0x36
#define	RADIO_2064_REG037	0x37
#define	RADIO_2064_REG038	0x38
#define	RADIO_2064_REG039	0x39
#define	RADIO_2064_REG03A	0x3a
#define	RADIO_2064_REG03B	0x3b
#define	RADIO_2064_REG03C	0x3c
#define	RADIO_2064_REG03D	0x3d
#define	RADIO_2064_REG03E	0x3e
#define	RADIO_2064_REG03F	0x3f
#define	RADIO_2064_REG040	0x40
#define	RADIO_2064_REG041	0x41
#define	RADIO_2064_REG042	0x42
#define	RADIO_2064_REG043	0x43
#define	RADIO_2064_REG044	0x44
#define	RADIO_2064_REG045	0x45
#define	RADIO_2064_REG046	0x46
#define	RADIO_2064_REG047	0x47
#define	RADIO_2064_REG048	0x48
#define	RADIO_2064_REG049	0x49
#define	RADIO_2064_REG04A	0x4a
#define	RADIO_2064_REG04B	0x4b
#define	RADIO_2064_REG04C	0x4c
#define	RADIO_2064_REG04D	0x4d
#define	RADIO_2064_REG04E	0x4e
#define	RADIO_2064_REG04F	0x4f
#define	RADIO_2064_REG050	0x50
#define	RADIO_2064_REG051	0x51
#define	RADIO_2064_REG052	0x52
#define	RADIO_2064_REG053	0x53
#define	RADIO_2064_REG054	0x54
#define	RADIO_2064_REG055	0x55
#define	RADIO_2064_REG056	0x56
#define	RADIO_2064_REG057	0x57
#define	RADIO_2064_REG058	0x58
#define	RADIO_2064_REG059	0x59
#define	RADIO_2064_REG05A	0x5a
#define	RADIO_2064_REG05B	0x5b
#define	RADIO_2064_REG05C	0x5c
#define	RADIO_2064_REG05D	0x5d
#define	RADIO_2064_REG05E	0x5e
#define	RADIO_2064_REG05F	0x5f
#define	RADIO_2064_REG060	0x60
#define	RADIO_2064_REG061	0x61
#define	RADIO_2064_REG062	0x62
#define	RADIO_2064_REG063	0x63
#define	RADIO_2064_REG064	0x64
#define	RADIO_2064_REG065	0x65
#define	RADIO_2064_REG066	0x66
#define	RADIO_2064_REG067	0x67
#define	RADIO_2064_REG068	0x68
#define	RADIO_2064_REG069	0x69
#define	RADIO_2064_REG06A	0x6a
#define	RADIO_2064_REG06B	0x6b
#define	RADIO_2064_REG06C	0x6c
#define	RADIO_2064_REG06D	0x6d
#define	RADIO_2064_REG06E	0x6e
#define	RADIO_2064_REG06F	0x6f
#define	RADIO_2064_REG070	0x70
#define	RADIO_2064_REG071	0x71
#define	RADIO_2064_REG072	0x72
#define	RADIO_2064_REG073	0x73
#define	RADIO_2064_REG074	0x74
#define	RADIO_2064_REG075	0x75
#define	RADIO_2064_REG076	0x76
#define	RADIO_2064_REG077	0x77
#define	RADIO_2064_REG078	0x78
#define	RADIO_2064_REG079	0x79
#define	RADIO_2064_REG07A	0x7a
#define	RADIO_2064_REG07B	0x7b
#define	RADIO_2064_REG07C	0x7c
#define	RADIO_2064_REG07D	0x7d
#define	RADIO_2064_REG07E	0x7e
#define	RADIO_2064_REG07F	0x7f
#define	RADIO_2064_REG080	0x80
#define	RADIO_2064_REG081	0x81
#define	RADIO_2064_REG082	0x82
#define	RADIO_2064_REG083	0x83
#define	RADIO_2064_REG084	0x84
#define	RADIO_2064_REG085	0x85
#define	RADIO_2064_REG086	0x86
#define	RADIO_2064_REG087	0x87
#define	RADIO_2064_REG088	0x88
#define	RADIO_2064_REG089	0x89
#define	RADIO_2064_REG08A	0x8a
#define	RADIO_2064_REG08B	0x8b
#define	RADIO_2064_REG08C	0x8c
#define	RADIO_2064_REG08D	0x8d
#define	RADIO_2064_REG08E	0x8e
#define	RADIO_2064_REG08F	0x8f
#define	RADIO_2064_REG090	0x90
#define	RADIO_2064_REG091	0x91
#define	RADIO_2064_REG092	0x92
#define	RADIO_2064_REG093	0x93
#define	RADIO_2064_REG094	0x94
#define	RADIO_2064_REG095	0x95
#define	RADIO_2064_REG096	0x96
#define	RADIO_2064_REG097	0x97
#define	RADIO_2064_REG098	0x98
#define	RADIO_2064_REG099	0x99
#define	RADIO_2064_REG09A	0x9a
#define	RADIO_2064_REG09B	0x9b
#define	RADIO_2064_REG09C	0x9c
#define	RADIO_2064_REG09D	0x9d
#define	RADIO_2064_REG09E	0x9e
#define	RADIO_2064_REG09F	0x9f
#define	RADIO_2064_REG0A0	0xa0
#define	RADIO_2064_REG0A1	0xa1
#define	RADIO_2064_REG0A2	0xa2
#define	RADIO_2064_REG0A3	0xa3
#define	RADIO_2064_REG0A4	0xa4
#define	RADIO_2064_REG0A5	0xa5
#define	RADIO_2064_REG0A6	0xa6
#define	RADIO_2064_REG0A7	0xa7
#define	RADIO_2064_REG0A8	0xa8
#define	RADIO_2064_REG0A9	0xa9
#define	RADIO_2064_REG0AA	0xaa
#define	RADIO_2064_REG0AB	0xab
#define	RADIO_2064_REG0AC	0xac
#define	RADIO_2064_REG0AD	0xad
#define	RADIO_2064_REG0AE	0xae
#define	RADIO_2064_REG0AF	0xaf
#define	RADIO_2064_REG0B0	0xb0
#define	RADIO_2064_REG0B1	0xb1
#define	RADIO_2064_REG0B2	0xb2
#define	RADIO_2064_REG0B3	0xb3
#define	RADIO_2064_REG0B4	0xb4
#define	RADIO_2064_REG0B5	0xb5
#define	RADIO_2064_REG0B6	0xb6
#define	RADIO_2064_REG0B7	0xb7
#define	RADIO_2064_REG0B8	0xb8
#define	RADIO_2064_REG0B9	0xb9
#define	RADIO_2064_REG0BA	0xba
#define	RADIO_2064_REG0BB	0xbb
#define	RADIO_2064_REG0BC	0xbc
#define	RADIO_2064_REG0BD	0xbd
#define	RADIO_2064_REG0BE	0xbe
#define	RADIO_2064_REG0BF	0xbf
#define	RADIO_2064_REG0C0	0xc0
#define	RADIO_2064_REG0C1	0xc1
#define	RADIO_2064_REG0C2	0xc2
#define	RADIO_2064_REG0C3	0xc3
#define	RADIO_2064_REG0C4	0xc4
#define	RADIO_2064_REG0C5	0xc5
#define	RADIO_2064_REG0C6	0xc6
#define	RADIO_2064_REG0C7	0xc7
#define	RADIO_2064_REG0C8	0xc8
#define	RADIO_2064_REG0C9	0xc9
#define	RADIO_2064_REG0CA	0xca
#define	RADIO_2064_REG0CB	0xcb
#define	RADIO_2064_REG0CC	0xcc
#define	RADIO_2064_REG0CD	0xcd
#define	RADIO_2064_REG0CE	0xce
#define	RADIO_2064_REG0CF	0xcf
#define	RADIO_2064_REG0D0	0xd0
#define	RADIO_2064_REG0D1	0xd1
#define	RADIO_2064_REG0D2	0xd2
#define	RADIO_2064_REG0D3	0xd3
#define	RADIO_2064_REG0D4	0xd4
#define	RADIO_2064_REG0D5	0xd5
#define	RADIO_2064_REG0D6	0xd6
#define	RADIO_2064_REG0D7	0xd7
#define	RADIO_2064_REG0D8	0xd8
#define	RADIO_2064_REG0D9	0xd9
#define	RADIO_2064_REG0DA	0xda
#define	RADIO_2064_REG0DB	0xdb
#define	RADIO_2064_REG0DC	0xdc
#define	RADIO_2064_REG0DD	0xdd
#define	RADIO_2064_REG0DE	0xde
#define	RADIO_2064_REG0DF	0xdf
#define	RADIO_2064_REG0E0	0xe0
#define	RADIO_2064_REG0E1	0xe1
#define	RADIO_2064_REG0E2	0xe2
#define	RADIO_2064_REG0E3	0xe3
#define	RADIO_2064_REG0E4	0xe4
#define	RADIO_2064_REG0E5	0xe5
#define	RADIO_2064_REG0E6	0xe6
#define	RADIO_2064_REG0E7	0xe7
#define	RADIO_2064_REG0E8	0xe8
#define	RADIO_2064_REG0E9	0xe9
#define	RADIO_2064_REG0EA	0xea
#define	RADIO_2064_REG0EB	0xeb
#define	RADIO_2064_REG0EC	0xec
#define	RADIO_2064_REG0ED	0xed
#define	RADIO_2064_REG0EE	0xee
#define	RADIO_2064_REG0EF	0xef
#define	RADIO_2064_REG0F0	0xf0
#define	RADIO_2064_REG0F1	0xf1
#define	RADIO_2064_REG0F2	0xf2
#define	RADIO_2064_REG0F3	0xf3
#define	RADIO_2064_REG0F4	0xf4
#define	RADIO_2064_REG0F5	0xf5
#define	RADIO_2064_REG0F6	0xf6
#define	RADIO_2064_REG0F7	0xf7
#define	RADIO_2064_REG0F8	0xf8
#define	RADIO_2064_REG0F9	0xf9
#define	RADIO_2064_REG0FA	0xfa
#define	RADIO_2064_REG0FB	0xfb
#define	RADIO_2064_REG0FC	0xfc
#define	RADIO_2064_REG0FD	0xfd
#define	RADIO_2064_REG0FE	0xfe
#define	RADIO_2064_REG0FF	0xff
#define	RADIO_2064_REG100	0x100
#define	RADIO_2064_REG101	0x101
#define	RADIO_2064_REG102	0x102
#define	RADIO_2064_REG103	0x103
#define	RADIO_2064_REG104	0x104
#define	RADIO_2064_REG105	0x105
#define	RADIO_2064_REG106	0x106
#define	RADIO_2064_REG107	0x107
#define	RADIO_2064_REG108	0x108
#define	RADIO_2064_REG109	0x109
#define	RADIO_2064_REG10A	0x10a
#define	RADIO_2064_REG10B	0x10b
#define	RADIO_2064_REG10C	0x10c
#define	RADIO_2064_REG10D	0x10d
#define	RADIO_2064_REG10E	0x10e
#define	RADIO_2064_REG10F	0x10f
#define	RADIO_2064_REG110	0x110
#define	RADIO_2064_REG111	0x111
#define	RADIO_2064_REG112	0x112
#define	RADIO_2064_REG113	0x113
#define	RADIO_2064_REG114	0x114
#define	RADIO_2064_REG115	0x115
#define	RADIO_2064_REG116	0x116
#define	RADIO_2064_REG117	0x117
#define	RADIO_2064_REG118	0x118
#define	RADIO_2064_REG119	0x119
#define	RADIO_2064_REG11A	0x11a
#define	RADIO_2064_REG11B	0x11b
#define	RADIO_2064_REG11C	0x11c
#define	RADIO_2064_REG11D	0x11d
#define	RADIO_2064_REG11E	0x11e
#define	RADIO_2064_REG11F	0x11f
#define	RADIO_2064_REG120	0x120
#define	RADIO_2064_REG121	0x121
#define	RADIO_2064_REG122	0x122
#define	RADIO_2064_REG123	0x123
#define	RADIO_2064_REG124	0x124
#define	RADIO_2064_REG125	0x125
#define	RADIO_2064_REG126	0x126
#define	RADIO_2064_REG127	0x127
#define	RADIO_2064_REG128	0x128
#define	RADIO_2064_REG129	0x129
#define	RADIO_2064_REG12A	0x12a
#define	RADIO_2064_REG12B	0x12b
#define	RADIO_2064_REG12C	0x12c
#define	RADIO_2064_REG12D	0x12d
#define	RADIO_2064_REG12E	0x12e
#define	RADIO_2064_REG12F	0x12f
#define	RADIO_2064_REG130	0x130

/* Array-dimension constants used by the tables below (from phy_lcn.c). */
#define	LCNPHY_NUM_DIG_FILT_COEFFS	16
#define	LCNPHY_NUM_TX_DIG_FILTERS_CCK	13
#define	LCNPHY_NUM_TX_DIG_FILTERS_OFDM	3

const uint16_t dot11lcn_min_sig_sq_tbl_rev0[] = {
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
	0x014d,
};

const uint16_t dot11lcn_noise_scale_tbl_rev0[] = {
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
};

const uint32_t dot11lcn_fltr_ctrl_tbl_rev0[] = {
	0x000141f8,
	0x000021f8,
	0x000021fb,
	0x000041fb,
	0x0001fe4b,
	0x0000217b,
	0x00002133,
	0x000040eb,
	0x0001fea3,
	0x0000024b,
};

const uint32_t dot11lcn_ps_ctrl_tbl_rev0[] = {
	0x00100001,
	0x00200010,
	0x00300001,
	0x00400010,
	0x00500022,
	0x00600122,
	0x00700222,
	0x00800322,
	0x00900422,
	0x00a00522,
	0x00b00622,
	0x00c00722,
	0x00d00822,
	0x00f00922,
	0x00100a22,
	0x00200b22,
	0x00300c22,
	0x00400d22,
	0x00500e22,
	0x00600f22,
};

const uint16_t dot11lcn_sw_ctrl_tbl_rev0[] = {
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
	0x0004,
	0x0004,
	0x0002,
	0x0002,
};

const uint8_t dot11lcn_nf_table_rev0[] = {
	0x5f,
	0x36,
	0x29,
	0x1f,
	0x5f,
	0x36,
	0x29,
	0x1f,
	0x5f,
	0x36,
	0x29,
	0x1f,
	0x5f,
	0x36,
	0x29,
	0x1f,
};

const uint8_t dot11lcn_spur_tbl_rev0[] = {
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x02,
	0x03,
	0x01,
	0x03,
	0x02,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x02,
	0x03,
	0x01,
	0x03,
	0x02,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
	0x01,
};

const uint16_t dot11lcn_unsup_mcs_tbl_rev0[] = {
	0x001a,
	0x0034,
	0x004e,
	0x0068,
	0x009c,
	0x00d0,
	0x00ea,
	0x0104,
	0x0034,
	0x0068,
	0x009c,
	0x00d0,
	0x0138,
	0x01a0,
	0x01d4,
	0x0208,
	0x004e,
	0x009c,
	0x00ea,
	0x0138,
	0x01d4,
	0x0270,
	0x02be,
	0x030c,
	0x0068,
	0x00d0,
	0x0138,
	0x01a0,
	0x0270,
	0x0340,
	0x03a8,
	0x0410,
	0x0018,
	0x009c,
	0x00d0,
	0x0104,
	0x00ea,
	0x0138,
	0x0186,
	0x00d0,
	0x0104,
	0x0104,
	0x0138,
	0x016c,
	0x016c,
	0x01a0,
	0x0138,
	0x0186,
	0x0186,
	0x01d4,
	0x0222,
	0x0222,
	0x0270,
	0x0104,
	0x0138,
	0x016c,
	0x0138,
	0x016c,
	0x01a0,
	0x01d4,
	0x01a0,
	0x01d4,
	0x0208,
	0x0208,
	0x023c,
	0x0186,
	0x01d4,
	0x0222,
	0x01d4,
	0x0222,
	0x0270,
	0x02be,
	0x0270,
	0x02be,
	0x030c,
	0x030c,
	0x035a,
	0x0036,
	0x006c,
	0x00a2,
	0x00d8,
	0x0144,
	0x01b0,
	0x01e6,
	0x021c,
	0x006c,
	0x00d8,
	0x0144,
	0x01b0,
	0x0288,
	0x0360,
	0x03cc,
	0x0438,
	0x00a2,
	0x0144,
	0x01e6,
	0x0288,
	0x03cc,
	0x0510,
	0x05b2,
	0x0654,
	0x00d8,
	0x01b0,
	0x0288,
	0x0360,
	0x0510,
	0x06c0,
	0x0798,
	0x0870,
	0x0018,
	0x0144,
	0x01b0,
	0x021c,
	0x01e6,
	0x0288,
	0x032a,
	0x01b0,
	0x021c,
	0x021c,
	0x0288,
	0x02f4,
	0x02f4,
	0x0360,
	0x0288,
	0x032a,
	0x032a,
	0x03cc,
	0x046e,
	0x046e,
	0x0510,
	0x021c,
	0x0288,
	0x02f4,
	0x0288,
	0x02f4,
	0x0360,
	0x03cc,
	0x0360,
	0x03cc,
	0x0438,
	0x0438,
	0x04a4,
	0x032a,
	0x03cc,
	0x046e,
	0x03cc,
	0x046e,
	0x0510,
	0x05b2,
	0x0510,
	0x05b2,
	0x0654,
	0x0654,
	0x06f6,
};

const uint16_t dot11lcn_iq_local_tbl_rev0[] = {
	0x0200,
	0x0300,
	0x0400,
	0x0600,
	0x0800,
	0x0b00,
	0x1000,
	0x1001,
	0x1002,
	0x1003,
	0x1004,
	0x1005,
	0x1006,
	0x1007,
	0x1707,
	0x2007,
	0x2d07,
	0x4007,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0200,
	0x0300,
	0x0400,
	0x0600,
	0x0800,
	0x0b00,
	0x1000,
	0x1001,
	0x1002,
	0x1003,
	0x1004,
	0x1005,
	0x1006,
	0x1007,
	0x1707,
	0x2007,
	0x2d07,
	0x4007,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x4000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
	0x0000,
};

const uint32_t dot11lcn_papd_compdelta_tbl_rev0[] = {
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
	0x00080000,
};

const struct bcm4313_phytbl dot11lcnphytbl_info_rev0[] = {
	{&dot11lcn_min_sig_sq_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_min_sig_sq_tbl_rev0), 2, 0, 16}
	,
	{&dot11lcn_noise_scale_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_noise_scale_tbl_rev0), 1, 0, 16}
	,
	{&dot11lcn_fltr_ctrl_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_fltr_ctrl_tbl_rev0), 11, 0, 32}
	,
	{&dot11lcn_ps_ctrl_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_ps_ctrl_tbl_rev0), 12, 0, 32}
	,
	{&dot11lcn_gain_idx_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_gain_idx_tbl_rev0), 13, 0, 32}
	,
	{&dot11lcn_aux_gain_idx_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_aux_gain_idx_tbl_rev0), 14, 0, 16}
	,
	{&dot11lcn_sw_ctrl_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_sw_ctrl_tbl_rev0), 15, 0, 16}
	,
	{&dot11lcn_nf_table_rev0,
	 ARRAY_SIZE(dot11lcn_nf_table_rev0), 16,
	 0, 8}
	,
	{&dot11lcn_gain_val_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_gain_val_tbl_rev0), 17, 0, 8}
	,
	{&dot11lcn_gain_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_gain_tbl_rev0), 18,
	 0, 32}
	,
	{&dot11lcn_spur_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_spur_tbl_rev0), 20,
	 0, 8}
	,
	{&dot11lcn_unsup_mcs_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_unsup_mcs_tbl_rev0), 23, 0, 16}
	,
	{&dot11lcn_iq_local_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_iq_local_tbl_rev0), 0, 0, 16}
	,
	{&dot11lcn_papd_compdelta_tbl_rev0,
	 ARRAY_SIZE(dot11lcn_papd_compdelta_tbl_rev0), 24, 0, 32}
	,
};

const struct lcnphy_tx_gain_tbl_entry dot11lcnphy_2GHz_gaintable_rev0[128] = {
	{15, 0, 31, 0, 72},
	{15, 0, 31, 0, 70},
	{15, 0, 31, 0, 68},
	{15, 0, 30, 0, 68},
	{15, 0, 29, 0, 69},
	{15, 0, 28, 0, 69},
	{15, 0, 27, 0, 70},
	{15, 0, 26, 0, 70},
	{15, 0, 25, 0, 71},
	{15, 0, 24, 0, 72},
	{15, 0, 23, 0, 73},
	{15, 0, 23, 0, 71},
	{15, 0, 22, 0, 72},
	{15, 0, 21, 0, 73},
	{15, 0, 21, 0, 71},
	{15, 0, 21, 0, 69},
	{15, 0, 21, 0, 67},
	{15, 0, 21, 0, 65},
	{15, 0, 21, 0, 63},
	{15, 0, 20, 0, 65},
	{15, 0, 19, 0, 66},
	{15, 0, 19, 0, 64},
	{15, 0, 18, 0, 66},
	{15, 0, 18, 0, 64},
	{15, 0, 17, 0, 66},
	{15, 0, 17, 0, 64},
	{15, 0, 16, 0, 66},
	{15, 0, 16, 0, 64},
	{15, 0, 16, 0, 62},
	{15, 0, 16, 0, 61},
	{15, 0, 16, 0, 59},
	{15, 0, 15, 0, 61},
	{15, 0, 15, 0, 59},
	{15, 0, 14, 0, 62},
	{15, 0, 14, 0, 60},
	{15, 0, 14, 0, 58},
	{15, 0, 13, 0, 61},
	{15, 0, 13, 0, 59},
	{15, 0, 12, 0, 62},
	{15, 0, 12, 0, 61},
	{15, 0, 12, 0, 59},
	{15, 0, 11, 0, 62},
	{15, 0, 11, 0, 61},
	{15, 0, 11, 0, 59},
	{15, 0, 11, 0, 57},
	{15, 0, 10, 0, 61},
	{15, 0, 10, 0, 59},
	{15, 0, 10, 0, 58},
	{15, 0, 9, 0, 62},
	{15, 0, 9, 0, 61},
	{15, 0, 9, 0, 59},
	{15, 0, 9, 0, 57},
	{15, 0, 8, 0, 62},
	{15, 0, 8, 0, 61},
	{15, 0, 8, 0, 59},
	{15, 0, 8, 0, 57},
	{15, 0, 8, 0, 56},
	{15, 0, 8, 0, 54},
	{15, 0, 8, 0, 53},
	{15, 0, 8, 0, 51},
	{15, 0, 8, 0, 50},
	{7, 0, 7, 0, 69},
	{7, 0, 7, 0, 67},
	{7, 0, 7, 0, 65},
	{7, 0, 7, 0, 64},
	{7, 0, 7, 0, 62},
	{7, 0, 7, 0, 60},
	{7, 0, 7, 0, 58},
	{7, 0, 7, 0, 57},
	{7, 0, 7, 0, 55},
	{7, 0, 6, 0, 62},
	{7, 0, 6, 0, 61},
	{7, 0, 6, 0, 59},
	{7, 0, 6, 0, 57},
	{7, 0, 6, 0, 56},
	{7, 0, 6, 0, 54},
	{7, 0, 6, 0, 53},
	{7, 0, 5, 0, 61},
	{7, 0, 5, 0, 60},
	{7, 0, 5, 0, 58},
	{7, 0, 5, 0, 56},
	{7, 0, 5, 0, 55},
	{7, 0, 5, 0, 53},
	{7, 0, 5, 0, 52},
	{7, 0, 5, 0, 50},
	{7, 0, 5, 0, 49},
	{7, 0, 5, 0, 47},
	{7, 0, 4, 0, 57},
	{7, 0, 4, 0, 56},
	{7, 0, 4, 0, 54},
	{7, 0, 4, 0, 53},
	{7, 0, 4, 0, 51},
	{7, 0, 4, 0, 50},
	{7, 0, 4, 0, 48},
	{7, 0, 4, 0, 47},
	{7, 0, 4, 0, 46},
	{7, 0, 4, 0, 44},
	{7, 0, 4, 0, 43},
	{7, 0, 4, 0, 42},
	{7, 0, 4, 0, 41},
	{7, 0, 4, 0, 40},
	{7, 0, 3, 0, 51},
	{7, 0, 3, 0, 50},
	{7, 0, 3, 0, 48},
	{7, 0, 3, 0, 47},
	{7, 0, 3, 0, 46},
	{7, 0, 3, 0, 44},
	{7, 0, 3, 0, 43},
	{7, 0, 3, 0, 42},
	{7, 0, 3, 0, 41},
	{3, 0, 3, 0, 56},
	{3, 0, 3, 0, 54},
	{3, 0, 3, 0, 53},
	{3, 0, 3, 0, 51},
	{3, 0, 3, 0, 50},
	{3, 0, 3, 0, 48},
	{3, 0, 3, 0, 47},
	{3, 0, 3, 0, 46},
	{3, 0, 3, 0, 44},
	{3, 0, 3, 0, 43},
	{3, 0, 3, 0, 42},
	{3, 0, 3, 0, 41},
	{3, 0, 3, 0, 39},
	{3, 0, 3, 0, 38},
	{3, 0, 3, 0, 37},
	{3, 0, 3, 0, 36},
	{3, 0, 3, 0, 35},
	{3, 0, 3, 0, 34},
};

const struct lcnphy_tx_gain_tbl_entry
dot11lcnphy_2GHz_extPA_gaintable_rev0[128] = {
	{3, 0, 31, 0, 72},
	{3, 0, 31, 0, 70},
	{3, 0, 31, 0, 68},
	{3, 0, 30, 0, 67},
	{3, 0, 29, 0, 68},
	{3, 0, 28, 0, 68},
	{3, 0, 27, 0, 69},
	{3, 0, 26, 0, 70},
	{3, 0, 25, 0, 70},
	{3, 0, 24, 0, 71},
	{3, 0, 23, 0, 72},
	{3, 0, 23, 0, 70},
	{3, 0, 22, 0, 71},
	{3, 0, 21, 0, 72},
	{3, 0, 21, 0, 70},
	{3, 0, 21, 0, 68},
	{3, 0, 21, 0, 66},
	{3, 0, 21, 0, 64},
	{3, 0, 21, 0, 63},
	{3, 0, 20, 0, 64},
	{3, 0, 19, 0, 65},
	{3, 0, 19, 0, 64},
	{3, 0, 18, 0, 65},
	{3, 0, 18, 0, 64},
	{3, 0, 17, 0, 65},
	{3, 0, 17, 0, 64},
	{3, 0, 16, 0, 65},
	{3, 0, 16, 0, 64},
	{3, 0, 16, 0, 62},
	{3, 0, 16, 0, 60},
	{3, 0, 16, 0, 58},
	{3, 0, 15, 0, 61},
	{3, 0, 15, 0, 59},
	{3, 0, 14, 0, 61},
	{3, 0, 14, 0, 60},
	{3, 0, 14, 0, 58},
	{3, 0, 13, 0, 60},
	{3, 0, 13, 0, 59},
	{3, 0, 12, 0, 62},
	{3, 0, 12, 0, 60},
	{3, 0, 12, 0, 58},
	{3, 0, 11, 0, 62},
	{3, 0, 11, 0, 60},
	{3, 0, 11, 0, 59},
	{3, 0, 11, 0, 57},
	{3, 0, 10, 0, 61},
	{3, 0, 10, 0, 59},
	{3, 0, 10, 0, 57},
	{3, 0, 9, 0, 62},
	{3, 0, 9, 0, 60},
	{3, 0, 9, 0, 58},
	{3, 0, 9, 0, 57},
	{3, 0, 8, 0, 62},
	{3, 0, 8, 0, 60},
	{3, 0, 8, 0, 58},
	{3, 0, 8, 0, 57},
	{3, 0, 8, 0, 55},
	{3, 0, 7, 0, 61},
	{3, 0, 7, 0, 60},
	{3, 0, 7, 0, 58},
	{3, 0, 7, 0, 56},
	{3, 0, 7, 0, 55},
	{3, 0, 6, 0, 62},
	{3, 0, 6, 0, 60},
	{3, 0, 6, 0, 58},
	{3, 0, 6, 0, 57},
	{3, 0, 6, 0, 55},
	{3, 0, 6, 0, 54},
	{3, 0, 6, 0, 52},
	{3, 0, 5, 0, 61},
	{3, 0, 5, 0, 59},
	{3, 0, 5, 0, 57},
	{3, 0, 5, 0, 56},
	{3, 0, 5, 0, 54},
	{3, 0, 5, 0, 53},
	{3, 0, 5, 0, 51},
	{3, 0, 4, 0, 62},
	{3, 0, 4, 0, 60},
	{3, 0, 4, 0, 58},
	{3, 0, 4, 0, 57},
	{3, 0, 4, 0, 55},
	{3, 0, 4, 0, 54},
	{3, 0, 4, 0, 52},
	{3, 0, 4, 0, 51},
	{3, 0, 4, 0, 49},
	{3, 0, 4, 0, 48},
	{3, 0, 4, 0, 46},
	{3, 0, 3, 0, 60},
	{3, 0, 3, 0, 58},
	{3, 0, 3, 0, 57},
	{3, 0, 3, 0, 55},
	{3, 0, 3, 0, 54},
	{3, 0, 3, 0, 52},
	{3, 0, 3, 0, 51},
	{3, 0, 3, 0, 49},
	{3, 0, 3, 0, 48},
	{3, 0, 3, 0, 46},
	{3, 0, 3, 0, 45},
	{3, 0, 3, 0, 44},
	{3, 0, 3, 0, 43},
	{3, 0, 3, 0, 41},
	{3, 0, 2, 0, 61},
	{3, 0, 2, 0, 59},
	{3, 0, 2, 0, 57},
	{3, 0, 2, 0, 56},
	{3, 0, 2, 0, 54},
	{3, 0, 2, 0, 53},
	{3, 0, 2, 0, 51},
	{3, 0, 2, 0, 50},
	{3, 0, 2, 0, 48},
	{3, 0, 2, 0, 47},
	{3, 0, 2, 0, 46},
	{3, 0, 2, 0, 44},
	{3, 0, 2, 0, 43},
	{3, 0, 2, 0, 42},
	{3, 0, 2, 0, 41},
	{3, 0, 2, 0, 39},
	{3, 0, 2, 0, 38},
	{3, 0, 2, 0, 37},
	{3, 0, 2, 0, 36},
	{3, 0, 2, 0, 35},
	{3, 0, 2, 0, 34},
	{3, 0, 2, 0, 33},
	{3, 0, 2, 0, 32},
	{3, 0, 1, 0, 63},
	{3, 0, 1, 0, 61},
	{3, 0, 1, 0, 59},
	{3, 0, 1, 0, 57},
};


const struct lcnphy_sfo_cfg lcnphy_sfo_cfg[] = {
	{965, 1087},
	{967, 1085},
	{969, 1082},
	{971, 1080},
	{973, 1078},
	{975, 1076},
	{977, 1073},
	{979, 1071},
	{981, 1069},
	{983, 1067},
	{985, 1065},
	{987, 1063},
	{989, 1060},
	{994, 1055}
};

const struct chan_info_2064_lcnphy chan_info_2064_lcnphy[] = {
	{1, 2412, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{2, 2417, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{3, 2422, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{4, 2427, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{5, 2432, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{6, 2437, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{7, 2442, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{8, 2447, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{9, 2452, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{10, 2457, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{11, 2462, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{12, 2467, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{13, 2472, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
	{14, 2484, 0x0B, 0x0A, 0x00, 0x07, 0x0A, 0x88, 0x88, 0x80},
};

const struct lcnphy_radio_regs lcnphy_radio_regs_2064[] = {
	{0x00, 0, 0, 0, 0},
	{0x01, 0x64, 0x64, 0, 0},
	{0x02, 0x20, 0x20, 0, 0},
	{0x03, 0x66, 0x66, 0, 0},
	{0x04, 0xf8, 0xf8, 0, 0},
	{0x05, 0, 0, 0, 0},
	{0x06, 0x10, 0x10, 0, 0},
	{0x07, 0, 0, 0, 0},
	{0x08, 0, 0, 0, 0},
	{0x09, 0, 0, 0, 0},
	{0x0A, 0x37, 0x37, 0, 0},
	{0x0B, 0x6, 0x6, 0, 0},
	{0x0C, 0x55, 0x55, 0, 0},
	{0x0D, 0x8b, 0x8b, 0, 0},
	{0x0E, 0, 0, 0, 0},
	{0x0F, 0x5, 0x5, 0, 0},
	{0x10, 0, 0, 0, 0},
	{0x11, 0xe, 0xe, 0, 0},
	{0x12, 0, 0, 0, 0},
	{0x13, 0xb, 0xb, 0, 0},
	{0x14, 0x2, 0x2, 0, 0},
	{0x15, 0x12, 0x12, 0, 0},
	{0x16, 0x12, 0x12, 0, 0},
	{0x17, 0xc, 0xc, 0, 0},
	{0x18, 0xc, 0xc, 0, 0},
	{0x19, 0xc, 0xc, 0, 0},
	{0x1A, 0x8, 0x8, 0, 0},
	{0x1B, 0x2, 0x2, 0, 0},
	{0x1C, 0, 0, 0, 0},
	{0x1D, 0x1, 0x1, 0, 0},
	{0x1E, 0x12, 0x12, 0, 0},
	{0x1F, 0x6e, 0x6e, 0, 0},
	{0x20, 0x2, 0x2, 0, 0},
	{0x21, 0x23, 0x23, 0, 0},
	{0x22, 0x8, 0x8, 0, 0},
	{0x23, 0, 0, 0, 0},
	{0x24, 0, 0, 0, 0},
	{0x25, 0xc, 0xc, 0, 0},
	{0x26, 0x33, 0x33, 0, 0},
	{0x27, 0x55, 0x55, 0, 0},
	{0x28, 0, 0, 0, 0},
	{0x29, 0x30, 0x30, 0, 0},
	{0x2A, 0xb, 0xb, 0, 0},
	{0x2B, 0x1b, 0x1b, 0, 0},
	{0x2C, 0x3, 0x3, 0, 0},
	{0x2D, 0x1b, 0x1b, 0, 0},
	{0x2E, 0, 0, 0, 0},
	{0x2F, 0x20, 0x20, 0, 0},
	{0x30, 0xa, 0xa, 0, 0},
	{0x31, 0, 0, 0, 0},
	{0x32, 0x62, 0x62, 0, 0},
	{0x33, 0x19, 0x19, 0, 0},
	{0x34, 0x33, 0x33, 0, 0},
	{0x35, 0x77, 0x77, 0, 0},
	{0x36, 0, 0, 0, 0},
	{0x37, 0x70, 0x70, 0, 0},
	{0x38, 0x3, 0x3, 0, 0},
	{0x39, 0xf, 0xf, 0, 0},
	{0x3A, 0x6, 0x6, 0, 0},
	{0x3B, 0xcf, 0xcf, 0, 0},
	{0x3C, 0x1a, 0x1a, 0, 0},
	{0x3D, 0x6, 0x6, 0, 0},
	{0x3E, 0x42, 0x42, 0, 0},
	{0x3F, 0, 0, 0, 0},
	{0x40, 0xfb, 0xfb, 0, 0},
	{0x41, 0x9a, 0x9a, 0, 0},
	{0x42, 0x7a, 0x7a, 0, 0},
	{0x43, 0x29, 0x29, 0, 0},
	{0x44, 0, 0, 0, 0},
	{0x45, 0x8, 0x8, 0, 0},
	{0x46, 0xce, 0xce, 0, 0},
	{0x47, 0x27, 0x27, 0, 0},
	{0x48, 0x62, 0x62, 0, 0},
	{0x49, 0x6, 0x6, 0, 0},
	{0x4A, 0x58, 0x58, 0, 0},
	{0x4B, 0xf7, 0xf7, 0, 0},
	{0x4C, 0, 0, 0, 0},
	{0x4D, 0xb3, 0xb3, 0, 0},
	{0x4E, 0, 0, 0, 0},
	{0x4F, 0x2, 0x2, 0, 0},
	{0x50, 0, 0, 0, 0},
	{0x51, 0x9, 0x9, 0, 0},
	{0x52, 0x5, 0x5, 0, 0},
	{0x53, 0x17, 0x17, 0, 0},
	{0x54, 0x38, 0x38, 0, 0},
	{0x55, 0, 0, 0, 0},
	{0x56, 0, 0, 0, 0},
	{0x57, 0xb, 0xb, 0, 0},
	{0x58, 0, 0, 0, 0},
	{0x59, 0, 0, 0, 0},
	{0x5A, 0, 0, 0, 0},
	{0x5B, 0, 0, 0, 0},
	{0x5C, 0, 0, 0, 0},
	{0x5D, 0, 0, 0, 0},
	{0x5E, 0x88, 0x88, 0, 0},
	{0x5F, 0xcc, 0xcc, 0, 0},
	{0x60, 0x74, 0x74, 0, 0},
	{0x61, 0x74, 0x74, 0, 0},
	{0x62, 0x74, 0x74, 0, 0},
	{0x63, 0x44, 0x44, 0, 0},
	{0x64, 0x77, 0x77, 0, 0},
	{0x65, 0x44, 0x44, 0, 0},
	{0x66, 0x77, 0x77, 0, 0},
	{0x67, 0x55, 0x55, 0, 0},
	{0x68, 0x77, 0x77, 0, 0},
	{0x69, 0x77, 0x77, 0, 0},
	{0x6A, 0, 0, 0, 0},
	{0x6B, 0x7f, 0x7f, 0, 0},
	{0x6C, 0x8, 0x8, 0, 0},
	{0x6D, 0, 0, 0, 0},
	{0x6E, 0x88, 0x88, 0, 0},
	{0x6F, 0x66, 0x66, 0, 0},
	{0x70, 0x66, 0x66, 0, 0},
	{0x71, 0x28, 0x28, 0, 0},
	{0x72, 0x55, 0x55, 0, 0},
	{0x73, 0x4, 0x4, 0, 0},
	{0x74, 0, 0, 0, 0},
	{0x75, 0, 0, 0, 0},
	{0x76, 0, 0, 0, 0},
	{0x77, 0x1, 0x1, 0, 0},
	{0x78, 0xd6, 0xd6, 0, 0},
	{0x79, 0, 0, 0, 0},
	{0x7A, 0, 0, 0, 0},
	{0x7B, 0, 0, 0, 0},
	{0x7C, 0, 0, 0, 0},
	{0x7D, 0, 0, 0, 0},
	{0x7E, 0, 0, 0, 0},
	{0x7F, 0, 0, 0, 0},
	{0x80, 0, 0, 0, 0},
	{0x81, 0, 0, 0, 0},
	{0x82, 0, 0, 0, 0},
	{0x83, 0xb4, 0xb4, 0, 0},
	{0x84, 0x1, 0x1, 0, 0},
	{0x85, 0x20, 0x20, 0, 0},
	{0x86, 0x5, 0x5, 0, 0},
	{0x87, 0xff, 0xff, 0, 0},
	{0x88, 0x7, 0x7, 0, 0},
	{0x89, 0x77, 0x77, 0, 0},
	{0x8A, 0x77, 0x77, 0, 0},
	{0x8B, 0x77, 0x77, 0, 0},
	{0x8C, 0x77, 0x77, 0, 0},
	{0x8D, 0x8, 0x8, 0, 0},
	{0x8E, 0xa, 0xa, 0, 0},
	{0x8F, 0x8, 0x8, 0, 0},
	{0x90, 0x18, 0x18, 0, 0},
	{0x91, 0x5, 0x5, 0, 0},
	{0x92, 0x1f, 0x1f, 0, 0},
	{0x93, 0x10, 0x10, 0, 0},
	{0x94, 0x3, 0x3, 0, 0},
	{0x95, 0, 0, 0, 0},
	{0x96, 0, 0, 0, 0},
	{0x97, 0xaa, 0xaa, 0, 0},
	{0x98, 0, 0, 0, 0},
	{0x99, 0x23, 0x23, 0, 0},
	{0x9A, 0x7, 0x7, 0, 0},
	{0x9B, 0xf, 0xf, 0, 0},
	{0x9C, 0x10, 0x10, 0, 0},
	{0x9D, 0x3, 0x3, 0, 0},
	{0x9E, 0x4, 0x4, 0, 0},
	{0x9F, 0x20, 0x20, 0, 0},
	{0xA0, 0, 0, 0, 0},
	{0xA1, 0, 0, 0, 0},
	{0xA2, 0, 0, 0, 0},
	{0xA3, 0, 0, 0, 0},
	{0xA4, 0x1, 0x1, 0, 0},
	{0xA5, 0x77, 0x77, 0, 0},
	{0xA6, 0x77, 0x77, 0, 0},
	{0xA7, 0x77, 0x77, 0, 0},
	{0xA8, 0x77, 0x77, 0, 0},
	{0xA9, 0x8c, 0x8c, 0, 0},
	{0xAA, 0x88, 0x88, 0, 0},
	{0xAB, 0x78, 0x78, 0, 0},
	{0xAC, 0x57, 0x57, 0, 0},
	{0xAD, 0x88, 0x88, 0, 0},
	{0xAE, 0, 0, 0, 0},
	{0xAF, 0x8, 0x8, 0, 0},
	{0xB0, 0x88, 0x88, 0, 0},
	{0xB1, 0, 0, 0, 0},
	{0xB2, 0x1b, 0x1b, 0, 0},
	{0xB3, 0x3, 0x3, 0, 0},
	{0xB4, 0x24, 0x24, 0, 0},
	{0xB5, 0x3, 0x3, 0, 0},
	{0xB6, 0x1b, 0x1b, 0, 0},
	{0xB7, 0x24, 0x24, 0, 0},
	{0xB8, 0x3, 0x3, 0, 0},
	{0xB9, 0, 0, 0, 0},
	{0xBA, 0xaa, 0xaa, 0, 0},
	{0xBB, 0, 0, 0, 0},
	{0xBC, 0x4, 0x4, 0, 0},
	{0xBD, 0, 0, 0, 0},
	{0xBE, 0x8, 0x8, 0, 0},
	{0xBF, 0x11, 0x11, 0, 0},
	{0xC0, 0, 0, 0, 0},
	{0xC1, 0, 0, 0, 0},
	{0xC2, 0x62, 0x62, 0, 0},
	{0xC3, 0x1e, 0x1e, 0, 0},
	{0xC4, 0x33, 0x33, 0, 0},
	{0xC5, 0x37, 0x37, 0, 0},
	{0xC6, 0, 0, 0, 0},
	{0xC7, 0x70, 0x70, 0, 0},
	{0xC8, 0x1e, 0x1e, 0, 0},
	{0xC9, 0x6, 0x6, 0, 0},
	{0xCA, 0x4, 0x4, 0, 0},
	{0xCB, 0x2f, 0x2f, 0, 0},
	{0xCC, 0xf, 0xf, 0, 0},
	{0xCD, 0, 0, 0, 0},
	{0xCE, 0xff, 0xff, 0, 0},
	{0xCF, 0x8, 0x8, 0, 0},
	{0xD0, 0x3f, 0x3f, 0, 0},
	{0xD1, 0x3f, 0x3f, 0, 0},
	{0xD2, 0x3f, 0x3f, 0, 0},
	{0xD3, 0, 0, 0, 0},
	{0xD4, 0, 0, 0, 0},
	{0xD5, 0, 0, 0, 0},
	{0xD6, 0xcc, 0xcc, 0, 0},
	{0xD7, 0, 0, 0, 0},
	{0xD8, 0x8, 0x8, 0, 0},
	{0xD9, 0x8, 0x8, 0, 0},
	{0xDA, 0x8, 0x8, 0, 0},
	{0xDB, 0x11, 0x11, 0, 0},
	{0xDC, 0, 0, 0, 0},
	{0xDD, 0x87, 0x87, 0, 0},
	{0xDE, 0x88, 0x88, 0, 0},
	{0xDF, 0x8, 0x8, 0, 0},
	{0xE0, 0x8, 0x8, 0, 0},
	{0xE1, 0x8, 0x8, 0, 0},
	{0xE2, 0, 0, 0, 0},
	{0xE3, 0, 0, 0, 0},
	{0xE4, 0, 0, 0, 0},
	{0xE5, 0xf5, 0xf5, 0, 0},
	{0xE6, 0x30, 0x30, 0, 0},
	{0xE7, 0x1, 0x1, 0, 0},
	{0xE8, 0, 0, 0, 0},
	{0xE9, 0xff, 0xff, 0, 0},
	{0xEA, 0, 0, 0, 0},
	{0xEB, 0, 0, 0, 0},
	{0xEC, 0x22, 0x22, 0, 0},
	{0xED, 0, 0, 0, 0},
	{0xEE, 0, 0, 0, 0},
	{0xEF, 0, 0, 0, 0},
	{0xF0, 0x3, 0x3, 0, 0},
	{0xF1, 0x1, 0x1, 0, 0},
	{0xF2, 0, 0, 0, 0},
	{0xF3, 0, 0, 0, 0},
	{0xF4, 0, 0, 0, 0},
	{0xF5, 0, 0, 0, 0},
	{0xF6, 0, 0, 0, 0},
	{0xF7, 0x6, 0x6, 0, 0},
	{0xF8, 0, 0, 0, 0},
	{0xF9, 0, 0, 0, 0},
	{0xFA, 0x40, 0x40, 0, 0},
	{0xFB, 0, 0, 0, 0},
	{0xFC, 0x1, 0x1, 0, 0},
	{0xFD, 0x80, 0x80, 0, 0},
	{0xFE, 0x2, 0x2, 0, 0},
	{0xFF, 0x10, 0x10, 0, 0},
	{0x100, 0x2, 0x2, 0, 0},
	{0x101, 0x1e, 0x1e, 0, 0},
	{0x102, 0x1e, 0x1e, 0, 0},
	{0x103, 0, 0, 0, 0},
	{0x104, 0x1f, 0x1f, 0, 0},
	{0x105, 0, 0x8, 0, 1},
	{0x106, 0x2a, 0x2a, 0, 0},
	{0x107, 0xf, 0xf, 0, 0},
	{0x108, 0, 0, 0, 0},
	{0x109, 0, 0, 0, 0},
	{0x10A, 0, 0, 0, 0},
	{0x10B, 0, 0, 0, 0},
	{0x10C, 0, 0, 0, 0},
	{0x10D, 0, 0, 0, 0},
	{0x10E, 0, 0, 0, 0},
	{0x10F, 0, 0, 0, 0},
	{0x110, 0, 0, 0, 0},
	{0x111, 0, 0, 0, 0},
	{0x112, 0, 0, 0, 0},
	{0x113, 0, 0, 0, 0},
	{0x114, 0, 0, 0, 0},
	{0x115, 0, 0, 0, 0},
	{0x116, 0, 0, 0, 0},
	{0x117, 0, 0, 0, 0},
	{0x118, 0, 0, 0, 0},
	{0x119, 0, 0, 0, 0},
	{0x11A, 0, 0, 0, 0},
	{0x11B, 0, 0, 0, 0},
	{0x11C, 0x1, 0x1, 0, 0},
	{0x11D, 0, 0, 0, 0},
	{0x11E, 0, 0, 0, 0},
	{0x11F, 0, 0, 0, 0},
	{0x120, 0, 0, 0, 0},
	{0x121, 0, 0, 0, 0},
	{0x122, 0x80, 0x80, 0, 0},
	{0x123, 0, 0, 0, 0},
	{0x124, 0xf8, 0xf8, 0, 0},
	{0x125, 0, 0, 0, 0},
	{0x126, 0, 0, 0, 0},
	{0x127, 0, 0, 0, 0},
	{0x128, 0, 0, 0, 0},
	{0x129, 0, 0, 0, 0},
	{0x12A, 0, 0, 0, 0},
	{0x12B, 0, 0, 0, 0},
	{0x12C, 0, 0, 0, 0},
	{0x12D, 0, 0, 0, 0},
	{0x12E, 0, 0, 0, 0},
	{0x12F, 0, 0, 0, 0},
	{0x130, 0, 0, 0, 0},
	{0xFFFF, 0, 0, 0, 0}
};

const uint32_t lcnphy_23bitgaincode_table[] = {
	0x200100,
	0x200200,
	0x200004,
	0x200014,
	0x200024,
	0x200034,
	0x200134,
	0x200234,
	0x200334,
	0x200434,
	0x200037,
	0x200137,
	0x200237,
	0x200337,
	0x200437,
	0x000035,
	0x000135,
	0x000235,
	0x000037,
	0x000137,
	0x000237,
	0x000337,
	0x00013f,
	0x00023f,
	0x00033f,
	0x00034f,
	0x00044f,
	0x00144f,
	0x00244f,
	0x00254f,
	0x00354f,
	0x00454f,
	0x00464f,
	0x01464f,
	0x02464f,
	0x03464f,
	0x04464f,
};

const int8_t lcnphy_gain_table[] = {
	-16,
	-13,
	10,
	7,
	4,
	0,
	3,
	6,
	9,
	12,
	15,
	18,
	21,
	24,
	27,
	30,
	33,
	36,
	39,
	42,
	45,
	48,
	50,
	53,
	56,
	59,
	62,
	65,
	68,
	71,
	74,
	77,
	80,
	83,
	86,
	89,
	92,
};

const int8_t lcnphy_gain_index_offset_for_rssi[] = {
	7,
	7,
	7,
	7,
	7,
	7,
	7,
	8,
	7,
	7,
	6,
	7,
	7,
	4,
	4,
	4,
	4,
	4,
	4,
	4,
	4,
	3,
	3,
	3,
	3,
	3,
	3,
	4,
	2,
	2,
	2,
	2,
	2,
	2,
	-1,
	-2,
	-2,
	-2
};

const uint16_t LCNPHY_txdigfiltcoeffs_cck[LCNPHY_NUM_TX_DIG_FILTERS_CCK]
	[LCNPHY_NUM_DIG_FILT_COEFFS + 1] = {
	{0, 1, 415, 1874, 64, 128, 64, 792, 1656, 64, 128, 64, 778, 1582, 64,
	 128, 64,},
	{1, 1, 402, 1847, 259, 59, 259, 671, 1794, 68, 54, 68, 608, 1863, 93,
	 167, 93,},
	{2, 1, 415, 1874, 64, 128, 64, 792, 1656, 192, 384, 192, 778, 1582, 64,
	 128, 64,},
	{3, 1, 302, 1841, 129, 258, 129, 658, 1720, 205, 410, 205, 754, 1760,
	 170, 340, 170,},
	{20, 1, 360, 1884, 242, 1734, 242, 752, 1720, 205, 1845, 205, 767, 1760,
	 256, 185, 256,},
	{21, 1, 360, 1884, 149, 1874, 149, 752, 1720, 205, 1883, 205, 767, 1760,
	 256, 273, 256,},
	{22, 1, 360, 1884, 98, 1948, 98, 752, 1720, 205, 1924, 205, 767, 1760,
	 256, 352, 256,},
	{23, 1, 350, 1884, 116, 1966, 116, 752, 1720, 205, 2008, 205, 767, 1760,
	 128, 233, 128,},
	{24, 1, 325, 1884, 32, 40, 32, 756, 1720, 256, 471, 256, 766, 1760, 256,
	 1881, 256,},
	{25, 1, 299, 1884, 51, 64, 51, 736, 1720, 256, 471, 256, 765, 1760, 256,
	 1881, 256,},
	{26, 1, 277, 1943, 39, 117, 88, 637, 1838, 64, 192, 144, 614, 1864, 128,
	 384, 288,},
	{27, 1, 245, 1943, 49, 147, 110, 626, 1838, 256, 768, 576, 613, 1864,
	 128, 384, 288,},
	{30, 1, 302, 1841, 61, 122, 61, 658, 1720, 205, 410, 205, 754, 1760,
	 170, 340, 170,},
};

const uint16_t LCNPHY_txdigfiltcoeffs_ofdm[LCNPHY_NUM_TX_DIG_FILTERS_OFDM]
	[LCNPHY_NUM_DIG_FILT_COEFFS + 1] = {
	{0, 0, 0xa2, 0x0, 0x100, 0x100, 0x0, 0x0, 0x0, 0x100, 0x0, 0x0,
	 0x278, 0xfea0, 0x80, 0x100, 0x80,},
	{1, 0, 374, 0xFF79, 16, 32, 16, 799, 0xFE74, 50, 32, 50,
	 750, 0xFE2B, 212, 0xFFCE, 212,},
	{2, 0, 375, 0xFF16, 37, 76, 37, 799, 0xFE74, 32, 20, 32, 748,
	 0xFEF2, 128, 0xFFE2, 128}
};

const iqcal_gain_params_lcnphy tbl_iqcal_gainparams_lcnphy_2G[] = {
	{0, 0, 0, 0, 0, 0, 0, 0, 0},
};

const iqcal_gain_params_lcnphy *tbl_iqcal_gainparams_lcnphy[1] = {
	tbl_iqcal_gainparams_lcnphy_2G,
};

const uint16_t iqcal_gainparams_numgains_lcnphy[1] = {
	ARRAY_SIZE(tbl_iqcal_gainparams_lcnphy_2G),
};

const
uint16_t lcnphy_iqcal_loft_gainladder[] = {
	((2 << 8) | 0),
	((3 << 8) | 0),
	((4 << 8) | 0),
	((6 << 8) | 0),
	((8 << 8) | 0),
	((11 << 8) | 0),
	((16 << 8) | 0),
	((16 << 8) | 1),
	((16 << 8) | 2),
	((16 << 8) | 3),
	((16 << 8) | 4),
	((16 << 8) | 5),
	((16 << 8) | 6),
	((16 << 8) | 7),
	((23 << 8) | 7),
	((32 << 8) | 7),
	((45 << 8) | 7),
	((64 << 8) | 7),
	((91 << 8) | 7),
	((128 << 8) | 7)
};

const
uint16_t lcnphy_iqcal_ir_gainladder[] = {
	((1 << 8) | 0),
	((2 << 8) | 0),
	((4 << 8) | 0),
	((6 << 8) | 0),
	((8 << 8) | 0),
	((11 << 8) | 0),
	((16 << 8) | 0),
	((23 << 8) | 0),
	((32 << 8) | 0),
	((45 << 8) | 0),
	((64 << 8) | 0),
	((64 << 8) | 1),
	((64 << 8) | 2),
	((64 << 8) | 3),
	((64 << 8) | 4),
	((64 << 8) | 5),
	((64 << 8) | 6),
	((64 << 8) | 7),
	((91 << 8) | 7),
	((128 << 8) | 7)
};

const
struct lcnphy_spb_tone lcnphy_spb_tone_3750[] = {
	{88, 0},
	{73, 49},
	{34, 81},
	{-17, 86},
	{-62, 62},
	{-86, 17},
	{-81, -34},
	{-49, -73},
	{0, -88},
	{49, -73},
	{81, -34},
	{86, 17},
	{62, 62},
	{17, 86},
	{-34, 81},
	{-73, 49},
	{-88, 0},
	{-73, -49},
	{-34, -81},
	{17, -86},
	{62, -62},
	{86, -17},
	{81, 34},
	{49, 73},
	{0, 88},
	{-49, 73},
	{-81, 34},
	{-86, -17},
	{-62, -62},
	{-17, -86},
	{34, -81},
	{73, -49},
};

const
uint16_t iqlo_loopback_rf_regs[20] = {
	RADIO_2064_REG036,
	RADIO_2064_REG11A,
	RADIO_2064_REG03A,
	RADIO_2064_REG025,
	RADIO_2064_REG028,
	RADIO_2064_REG005,
	RADIO_2064_REG112,
	RADIO_2064_REG0FF,
	RADIO_2064_REG11F,
	RADIO_2064_REG00B,
	RADIO_2064_REG113,
	RADIO_2064_REG007,
	RADIO_2064_REG0FC,
	RADIO_2064_REG0FD,
	RADIO_2064_REG012,
	RADIO_2064_REG057,
	RADIO_2064_REG059,
	RADIO_2064_REG05C,
	RADIO_2064_REG078,
	RADIO_2064_REG092,
};

const
uint16_t tempsense_phy_regs[14] = {
	0x503,
	0x4a4,
	0x4d0,
	0x4d9,
	0x4da,
	0x4a6,
	0x938,
	0x939,
	0x4d8,
	0x4d0,
	0x4d7,
	0x4a5,
	0x40d,
	0x4a2,
};

const
uint16_t rxiq_cal_rf_reg[11] = {
	RADIO_2064_REG098,
	RADIO_2064_REG116,
	RADIO_2064_REG12C,
	RADIO_2064_REG06A,
	RADIO_2064_REG00B,
	RADIO_2064_REG01B,
	RADIO_2064_REG113,
	RADIO_2064_REG01D,
	RADIO_2064_REG114,
	RADIO_2064_REG02E,
	RADIO_2064_REG12A,
};


#endif /* _BCM4313_PHYTBL_LCN_H_ */
