//
//  rs.cpp
//  itlwm
//
//  Created by zxystd on 2021/8/27.
//  Copyright © 2021 钟先耀. All rights reserved.
//

#include "ItlIwm.hpp"

#define IWL_RATE_MAX_WINDOW        62    /* # tx in history window */

/* Calculations of success ratio are done in fixed point where 12800 is 100%.
 * Use this macro when dealing with thresholds consts set as a percentage
 */
#define RS_PERCENT(x) (128 * x)

static u8 rs_ht_to_legacy[] = {
    [IWL_RATE_MCS_0_INDEX] = IWL_RATE_6M_INDEX,
    [IWL_RATE_MCS_1_INDEX] = IWL_RATE_9M_INDEX,
    [IWL_RATE_MCS_2_INDEX] = IWL_RATE_12M_INDEX,
    [IWL_RATE_MCS_3_INDEX] = IWL_RATE_18M_INDEX,
    [IWL_RATE_MCS_4_INDEX] = IWL_RATE_24M_INDEX,
    [IWL_RATE_MCS_5_INDEX] = IWL_RATE_36M_INDEX,
    [IWL_RATE_MCS_6_INDEX] = IWL_RATE_48M_INDEX,
    [IWL_RATE_MCS_7_INDEX] = IWL_RATE_54M_INDEX,
    [IWL_RATE_MCS_8_INDEX] = IWL_RATE_54M_INDEX,
    [IWL_RATE_MCS_9_INDEX] = IWL_RATE_54M_INDEX,
};

static const u8 ant_toggle_lookup[] = {
    [ANT_NONE] = ANT_NONE,
    [ANT_A] = ANT_B,
    [ANT_B] = ANT_A,
    [ANT_AB] = ANT_AB,
};

#define IWL_DECLARE_RATE_INFO(r, s, rp, rn)                  \
    [IWL_RATE_##r##M_INDEX] = { IWL_RATE_##r##M_PLCP,          \
                    IWL_RATE_HT_SISO_MCS_##s##_PLCP,  \
                    IWL_RATE_HT_MIMO2_MCS_##s##_PLCP, \
                    IWL_RATE_VHT_SISO_MCS_##s##_PLCP, \
                    IWL_RATE_VHT_MIMO2_MCS_##s##_PLCP,\
                    IWL_RATE_##rp##M_INDEX,          \
                    IWL_RATE_##rn##M_INDEX }

#define IWL_DECLARE_MCS_RATE(s)                          \
    [IWL_RATE_MCS_##s##_INDEX] = { IWL_RATE_INVM_PLCP,          \
                       IWL_RATE_HT_SISO_MCS_##s##_PLCP,      \
                       IWL_RATE_HT_MIMO2_MCS_##s##_PLCP,  \
                       IWL_RATE_VHT_SISO_MCS_##s##_PLCP,  \
                       IWL_RATE_VHT_MIMO2_MCS_##s##_PLCP, \
                       IWL_RATE_INVM_INDEX,              \
                       IWL_RATE_INVM_INDEX }

/*
 * Parameter order:
 *   rate, ht rate, prev rate, next rate
 *
 * If there isn't a valid next or previous rate then INV is used which
 * maps to IWL_RATE_INVALID
 *
 */
struct iwl_rs_rate_info iwl_rates[IWL_RATE_COUNT] = {
    IWL_DECLARE_RATE_INFO(1, INV, INV, 2),   /*  1mbps */
    IWL_DECLARE_RATE_INFO(2, INV, 1, 5),     /*  2mbps */
    IWL_DECLARE_RATE_INFO(5, INV, 2, 11),    /*5.5mbps */
    IWL_DECLARE_RATE_INFO(11, INV, 9, 12),   /* 11mbps */
    IWL_DECLARE_RATE_INFO(6, 0, 5, 11),      /*  6mbps ; MCS 0 */
    IWL_DECLARE_RATE_INFO(9, INV, 6, 11),    /*  9mbps */
    IWL_DECLARE_RATE_INFO(12, 1, 11, 18),    /* 12mbps ; MCS 1 */
    IWL_DECLARE_RATE_INFO(18, 2, 12, 24),    /* 18mbps ; MCS 2 */
    IWL_DECLARE_RATE_INFO(24, 3, 18, 36),    /* 24mbps ; MCS 3 */
    IWL_DECLARE_RATE_INFO(36, 4, 24, 48),    /* 36mbps ; MCS 4 */
    IWL_DECLARE_RATE_INFO(48, 5, 36, 54),    /* 48mbps ; MCS 5 */
    IWL_DECLARE_RATE_INFO(54, 6, 48, INV),   /* 54mbps ; MCS 6 */
    IWL_DECLARE_MCS_RATE(7),                 /* MCS 7 */
    IWL_DECLARE_MCS_RATE(8),                 /* MCS 8 */
    IWL_DECLARE_MCS_RATE(9),                 /* MCS 9 */
};

enum rs_action {
    RS_ACTION_STAY = 0,
    RS_ACTION_DOWNSCALE = -1,
    RS_ACTION_UPSCALE = 1,
};

enum rs_column_mode {
    RS_INVALID = 0,
    RS_LEGACY,
    RS_SISO,
    RS_MIMO2,
};

#define MAX_NEXT_COLUMNS 7
#define MAX_COLUMN_CHECKS 3

struct rs_tx_column;

typedef bool (*allow_column_func_t) (struct iwm_softc *sc,
                     struct ieee80211_node *ni,
                     struct rs_rate *rate,
                     const struct rs_tx_column *next_col);

struct rs_tx_column {
    enum rs_column_mode mode;
    u8 ant;
    bool sgi;
    enum rs_column next_columns[MAX_NEXT_COLUMNS];
    allow_column_func_t checks[MAX_COLUMN_CHECKS];
};

static bool rs_ant_allow(struct iwm_softc *sc, struct ieee80211_node *ni,
             struct rs_rate *rate,
             const struct rs_tx_column *next_col)
{
    return ItlIwm::iwm_coex_is_ant_avail(sc, next_col->ant);
}

static bool iwl_mvm_bt_coex_is_mimo_allowed(struct iwm_softc *sc, struct ieee80211_node *ni)
{
    return ItlIwm::iwm_coex_is_mimo_allowed(sc, ni);
}

static bool rs_mimo_allow(struct iwm_softc *sc, struct ieee80211_node *ni,
              struct rs_rate *rate,
              const struct rs_tx_column *next_col)
{
    ItlIwm *that = container_of(sc, ItlIwm, com);

    if (!ieee80211_node_supports_ht(ni))
        return false;

//    if (sta->smps_mode == IEEE80211_SMPS_STATIC)
//        return false;

    if (that->iwm_num_of_ant(that->iwm_fw_valid_tx_ant(sc)) < 2)
        return false;

    if (!iwl_mvm_bt_coex_is_mimo_allowed(sc, ni))
        return false;

    if (sc->sc_nvm.sku_cap_mimo_disable)
        return false;

    return true;
}

static bool rs_siso_allow(struct iwm_softc *sc, struct ieee80211_node *ni,
              struct rs_rate *rate,
              const struct rs_tx_column *next_col)
{
    if (!ieee80211_node_supports_ht(ni))
        return false;

    return true;
}

static bool rs_sgi_allow(struct iwm_softc *sc, struct ieee80211_node *ni,
             struct rs_rate *rate,
             const struct rs_tx_column *next_col)
{
    if (is_ht20(rate) && (ieee80211_node_supports_ht_sgi20(ni)))
        return true;
    if (is_ht40(rate) && (ieee80211_node_supports_ht_sgi40(ni)))
        return true;
    if (is_ht80(rate) && (ieee80211_node_supports_vht_sgi80(ni)))
        return true;
    if (is_ht160(rate) && (ieee80211_node_supports_vht_sgi160(ni)))
        return true;

    return false;
}

static const struct rs_tx_column rs_tx_columns[] = {
    [RS_COLUMN_LEGACY_ANT_A] = {
        .mode = RS_LEGACY,
        .ant = ANT_A,
        .next_columns = {
            RS_COLUMN_LEGACY_ANT_B,
            RS_COLUMN_SISO_ANT_A,
            RS_COLUMN_MIMO2,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_ant_allow,
        },
    },
    [RS_COLUMN_LEGACY_ANT_B] = {
        .mode = RS_LEGACY,
        .ant = ANT_B,
        .next_columns = {
            RS_COLUMN_LEGACY_ANT_A,
            RS_COLUMN_SISO_ANT_B,
            RS_COLUMN_MIMO2,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_ant_allow,
        },
    },
    [RS_COLUMN_SISO_ANT_A] = {
        .mode = RS_SISO,
        .ant = ANT_A,
        .next_columns = {
            RS_COLUMN_SISO_ANT_B,
            RS_COLUMN_MIMO2,
            RS_COLUMN_SISO_ANT_A_SGI,
            RS_COLUMN_LEGACY_ANT_A,
            RS_COLUMN_LEGACY_ANT_B,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_siso_allow,
            rs_ant_allow,
        },
    },
    [RS_COLUMN_SISO_ANT_B] = {
        .mode = RS_SISO,
        .ant = ANT_B,
        .next_columns = {
            RS_COLUMN_SISO_ANT_A,
            RS_COLUMN_MIMO2,
            RS_COLUMN_SISO_ANT_B_SGI,
            RS_COLUMN_LEGACY_ANT_A,
            RS_COLUMN_LEGACY_ANT_B,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_siso_allow,
            rs_ant_allow,
        },
    },
    [RS_COLUMN_SISO_ANT_A_SGI] = {
        .mode = RS_SISO,
        .ant = ANT_A,
        .sgi = true,
        .next_columns = {
            RS_COLUMN_SISO_ANT_B_SGI,
            RS_COLUMN_MIMO2_SGI,
            RS_COLUMN_SISO_ANT_A,
            RS_COLUMN_LEGACY_ANT_A,
            RS_COLUMN_LEGACY_ANT_B,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_siso_allow,
            rs_ant_allow,
            rs_sgi_allow,
        },
    },
    [RS_COLUMN_SISO_ANT_B_SGI] = {
        .mode = RS_SISO,
        .ant = ANT_B,
        .sgi = true,
        .next_columns = {
            RS_COLUMN_SISO_ANT_A_SGI,
            RS_COLUMN_MIMO2_SGI,
            RS_COLUMN_SISO_ANT_B,
            RS_COLUMN_LEGACY_ANT_A,
            RS_COLUMN_LEGACY_ANT_B,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_siso_allow,
            rs_ant_allow,
            rs_sgi_allow,
        },
    },
    [RS_COLUMN_MIMO2] = {
        .mode = RS_MIMO2,
        .ant = ANT_AB,
        .next_columns = {
            RS_COLUMN_SISO_ANT_A,
            RS_COLUMN_MIMO2_SGI,
            RS_COLUMN_LEGACY_ANT_A,
            RS_COLUMN_LEGACY_ANT_B,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_mimo_allow,
        },
    },
    [RS_COLUMN_MIMO2_SGI] = {
        .mode = RS_MIMO2,
        .ant = ANT_AB,
        .sgi = true,
        .next_columns = {
            RS_COLUMN_SISO_ANT_A_SGI,
            RS_COLUMN_MIMO2,
            RS_COLUMN_LEGACY_ANT_A,
            RS_COLUMN_LEGACY_ANT_B,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
            RS_COLUMN_INVALID,
        },
        .checks = {
            rs_mimo_allow,
            rs_sgi_allow,
        },
    },
};

static inline u8 rs_extract_rate(u32 rate_n_flags)
{
    /* also works for HT because bits 7:6 are zero there */
    return (u8)(rate_n_flags & RATE_LEGACY_RATE_MSK);
}

static int iwl_hwrate_to_plcp_idx(u32 rate_n_flags)
{
    int idx = 0;

    if (rate_n_flags & RATE_MCS_HT_MSK) {
        idx = rate_n_flags & RATE_HT_MCS_RATE_CODE_MSK;
        idx += IWL_RATE_MCS_0_INDEX;

        /* skip 9M not supported in HT*/
        if (idx >= IWL_RATE_9M_INDEX)
            idx += 1;
        if ((idx >= IWL_FIRST_HT_RATE) && (idx <= IWL_LAST_HT_RATE))
            return idx;
    } else if (rate_n_flags & RATE_MCS_VHT_MSK ||
           rate_n_flags & RATE_MCS_HE_MSK) {
        idx = rate_n_flags & RATE_VHT_MCS_RATE_CODE_MSK;
        idx += IWL_RATE_MCS_0_INDEX;

        /* skip 9M not supported in VHT*/
        if (idx >= IWL_RATE_9M_INDEX)
            idx++;
        if ((idx >= IWL_FIRST_VHT_RATE) && (idx <= IWL_LAST_VHT_RATE))
            return idx;
        if ((rate_n_flags & RATE_MCS_HE_MSK) &&
            (idx <= IWL_LAST_HE_RATE))
            return idx;
    } else {
        /* legacy rate format, search for match in table */

        u8 legacy_rate = rs_extract_rate(rate_n_flags);
        for (idx = 0; idx < ARRAY_SIZE(iwl_rates); idx++)
            if (iwl_rates[idx].plcp == legacy_rate)
                return idx;
    }

    return IWL_RATE_INVALID;
}

static void rs_rate_scale_perform(struct iwm_softc *sc,
                  struct ieee80211_node *ni,
                  struct iwl_lq_sta *lq_sta,
                  int tid, bool ndp);
static void rs_fill_lq_cmd(struct iwm_softc *sc,
               struct ieee80211_node *ni,
               struct iwl_lq_sta *lq_sta,
               const struct rs_rate *initial_rate);
static void rs_stay_in_table(struct iwl_lq_sta *lq_sta, bool force_search);

/*
 * The following tables contain the expected throughput metrics for all rates
 *
 *    1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54, 60 MBits
 *
 * where invalid entries are zeros.
 *
 * CCK rates are only valid in legacy table and will only be used in G
 * (2.4 GHz) band.
 */
static const u16 expected_tpt_legacy[IWL_RATE_COUNT] = {
    7, 13, 35, 58, 40, 57, 72, 98, 121, 154, 177, 186, 0, 0, 0
};

/* Expected TpT tables. 4 indexes:
 * 0 - NGI, 1 - SGI, 2 - AGG+NGI, 3 - AGG+SGI
 */
static const u16 expected_tpt_siso_20MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0, 42, 0,  76, 102, 124, 159, 183, 193, 202, 216, 0},
    {0, 0, 0, 0, 46, 0,  82, 110, 132, 168, 192, 202, 210, 225, 0},
    {0, 0, 0, 0, 49, 0,  97, 145, 192, 285, 375, 420, 464, 551, 0},
    {0, 0, 0, 0, 54, 0, 108, 160, 213, 315, 415, 465, 513, 608, 0},
};

static const u16 expected_tpt_siso_40MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0,  77, 0, 127, 160, 184, 220, 242, 250,  257,  269,  275},
    {0, 0, 0, 0,  83, 0, 135, 169, 193, 229, 250, 257,  264,  275,  280},
    {0, 0, 0, 0, 101, 0, 199, 295, 389, 570, 744, 828,  911, 1070, 1173},
    {0, 0, 0, 0, 112, 0, 220, 326, 429, 629, 819, 912, 1000, 1173, 1284},
};

static const u16 expected_tpt_siso_80MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0, 130, 0, 191, 223, 244,  273,  288,  294,  298,  305,  308},
    {0, 0, 0, 0, 138, 0, 200, 231, 251,  279,  293,  298,  302,  308,  312},
    {0, 0, 0, 0, 217, 0, 429, 634, 834, 1220, 1585, 1760, 1931, 2258, 2466},
    {0, 0, 0, 0, 241, 0, 475, 701, 921, 1343, 1741, 1931, 2117, 2468, 2691},
};

static const u16 expected_tpt_siso_160MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0, 191, 0, 244, 288,  298,  308,  313,  318,  323,  328,  330},
    {0, 0, 0, 0, 200, 0, 251, 293,  302,  312,  317,  322,  327,  332,  334},
    {0, 0, 0, 0, 439, 0, 875, 1307, 1736, 2584, 3419, 3831, 4240, 5049, 5581},
    {0, 0, 0, 0, 488, 0, 972, 1451, 1925, 2864, 3785, 4240, 4691, 5581, 6165},
};

static const u16 expected_tpt_mimo2_20MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0,  74, 0, 123, 155, 179, 213, 235, 243, 250,  261, 0},
    {0, 0, 0, 0,  81, 0, 131, 164, 187, 221, 242, 250, 256,  267, 0},
    {0, 0, 0, 0,  98, 0, 193, 286, 375, 550, 718, 799, 878, 1032, 0},
    {0, 0, 0, 0, 109, 0, 214, 316, 414, 607, 790, 879, 965, 1132, 0},
};

static const u16 expected_tpt_mimo2_40MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0, 123, 0, 182, 214, 235,  264,  279,  285,  289,  296,  300},
    {0, 0, 0, 0, 131, 0, 191, 222, 242,  270,  284,  289,  293,  300,  303},
    {0, 0, 0, 0, 200, 0, 390, 571, 741, 1067, 1365, 1505, 1640, 1894, 2053},
    {0, 0, 0, 0, 221, 0, 430, 630, 816, 1169, 1490, 1641, 1784, 2053, 2221},
};

static const u16 expected_tpt_mimo2_80MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0, 182, 0, 240,  264,  278,  299,  308,  311,  313,  317,  319},
    {0, 0, 0, 0, 190, 0, 247,  269,  282,  302,  310,  313,  315,  319,  320},
    {0, 0, 0, 0, 428, 0, 833, 1215, 1577, 2254, 2863, 3147, 3418, 3913, 4219},
    {0, 0, 0, 0, 474, 0, 920, 1338, 1732, 2464, 3116, 3418, 3705, 4225, 4545},
};

static const u16 expected_tpt_mimo2_160MHz[4][IWL_RATE_COUNT] = {
    {0, 0, 0, 0, 240, 0, 278,  308,  313,  319,  322,  324,  328,  330,   334},
    {0, 0, 0, 0, 247, 0, 282,  310,  315,  320,  323,  325,  329,  332,   338},
    {0, 0, 0, 0, 875, 0, 1735, 2582, 3414, 5043, 6619, 7389, 8147, 9629,  10592},
    {0, 0, 0, 0, 971, 0, 1925, 2861, 3779, 5574, 7304, 8147, 8976, 10592, 11640},
};

/* mbps, mcs */
static const struct iwl_rate_mcs_info iwl_rate_mcs[IWL_RATE_COUNT] = {
    {  "1", "BPSK DSSS"},
    {  "2", "QPSK DSSS"},
    {"5.5", "BPSK CCK"},
    { "11", "QPSK CCK"},
    {  "6", "BPSK 1/2"},
    {  "9", "BPSK 1/2"},
    { "12", "QPSK 1/2"},
    { "18", "QPSK 3/4"},
    { "24", "16QAM 1/2"},
    { "36", "16QAM 3/4"},
    { "48", "64QAM 2/3"},
    { "54", "64QAM 3/4"},
    { "60", "64QAM 5/6"},
};

#define MCS_INDEX_PER_STREAM    (8)

static const char *rs_pretty_ant(u8 ant)
{
    static const char * const ant_name[] = {
        [ANT_NONE] = "None",
        [ANT_A]    = "A",
        [ANT_B]    = "B",
        [ANT_AB]   = "AB",
        [ANT_C]    = "C",
        [ANT_AC]   = "AC",
        [ANT_BC]   = "BC",
        [ANT_ABC]  = "ABC",
    };

    if (ant > ANT_ABC)
        return "UNKNOWN";

    return ant_name[ant];
}

static const char *rs_pretty_lq_type(enum iwl_table_type type)
{
    static const char * const lq_types[] = {
        [LQ_NONE] = "NONE",
        [LQ_LEGACY_A] = "LEGACY_A",
        [LQ_LEGACY_G] = "LEGACY_G",
        [LQ_HT_SISO] = "HT SISO",
        [LQ_HT_MIMO2] = "HT MIMO",
        [LQ_VHT_SISO] = "VHT SISO",
        [LQ_VHT_MIMO2] = "VHT MIMO",
        [LQ_HE_SISO] = "HE SISO",
        [LQ_HE_MIMO2] = "HE MIMO",
    };

    if (type < LQ_NONE || type >= LQ_MAX)
        return "UNKNOWN";

    return lq_types[type];
}

static char *rs_pretty_rate(const struct rs_rate *rate)
{
    static char buf[40];
    static const char * const legacy_rates[] = {
        [IWL_RATE_1M_INDEX] = "1M",
        [IWL_RATE_2M_INDEX] = "2M",
        [IWL_RATE_5M_INDEX] = "5.5M",
        [IWL_RATE_11M_INDEX] = "11M",
        [IWL_RATE_6M_INDEX] = "6M",
        [IWL_RATE_9M_INDEX] = "9M",
        [IWL_RATE_12M_INDEX] = "12M",
        [IWL_RATE_18M_INDEX] = "18M",
        [IWL_RATE_24M_INDEX] = "24M",
        [IWL_RATE_36M_INDEX] = "36M",
        [IWL_RATE_48M_INDEX] = "48M",
        [IWL_RATE_54M_INDEX] = "54M",
    };
    static const char *const ht_vht_rates[] = {
        [IWL_RATE_MCS_0_INDEX] = "MCS0",
        [IWL_RATE_MCS_1_INDEX] = "MCS1",
        [IWL_RATE_MCS_2_INDEX] = "MCS2",
        [IWL_RATE_MCS_3_INDEX] = "MCS3",
        [IWL_RATE_MCS_4_INDEX] = "MCS4",
        [IWL_RATE_MCS_5_INDEX] = "MCS5",
        [IWL_RATE_MCS_6_INDEX] = "MCS6",
        [IWL_RATE_MCS_7_INDEX] = "MCS7",
        [IWL_RATE_MCS_8_INDEX] = "MCS8",
        [IWL_RATE_MCS_9_INDEX] = "MCS9",
    };
    const char *rate_str;

    if (is_type_legacy(rate->type) && (rate->index <= IWL_RATE_54M_INDEX))
        rate_str = legacy_rates[rate->index];
    else if ((is_type_ht(rate->type) || is_type_vht(rate->type)) &&
         (rate->index >= IWL_RATE_MCS_0_INDEX) &&
         (rate->index <= IWL_RATE_MCS_9_INDEX))
        rate_str = ht_vht_rates[rate->index];
    else
        rate_str = NULL;

    snprintf(buf, sizeof(buf), "(%s|%s|%s)", rs_pretty_lq_type(rate->type),
        rs_pretty_ant(rate->ant), rate_str ?: "BAD_RATE");
    return buf;
}

static inline void rs_dump_rate(struct iwm_softc *sc, const struct rs_rate *rate,
                const char *prefix)
{
    IWL_DEBUG_RATE(sc,
               "%s: %s BW: %d SGI: %d LDPC: %d STBC: %d\n",
               prefix, rs_pretty_rate(rate), rate->bw,
               rate->sgi, rate->ldpc, rate->stbc);
}

static void rs_rate_scale_clear_window(struct iwl_rate_scale_data *window)
{
    window->data = 0;
    window->success_counter = 0;
    window->success_ratio = IWL_INVALID_VALUE;
    window->counter = 0;
    window->average_tpt = IWL_INVALID_VALUE;
}

static void rs_rate_scale_clear_tbl_windows(struct iwm_softc *sc,
                        struct iwl_scale_tbl_info *tbl)
{
    int i;

    IWL_DEBUG_RATE(sc, "Clearing up window stats\n");
    for (i = 0; i < IWL_RATE_COUNT; i++)
        rs_rate_scale_clear_window(&tbl->win[i]);

    for (i = 0; i < ARRAY_SIZE(tbl->tpc_win); i++)
        rs_rate_scale_clear_window(&tbl->tpc_win[i]);
}

static inline u8 rs_is_valid_ant(u8 valid_antenna, u8 ant_type)
{
    return (ant_type & valid_antenna) == ant_type;
}

extern int ieee80211_can_use_ampdu(struct ieee80211com *,
                                      struct ieee80211_node *);

static int rs_tl_turn_on_agg_for_tid(struct iwm_softc *sc,
                     struct iwl_lq_sta *lq_data, u8 tid,
                     struct ieee80211_node *ni)
{
    IWL_DEBUG_RATE(sc, "Starting Tx agg: STA: %s tid: %d\n",
             ether_sprintf(ni->ni_macaddr), tid);

    /* start BA session until the peer sends del BA */
    if (!ieee80211_can_use_ampdu(&sc->sc_ic, ni))
        return -1;
    ieee80211_node_trigger_addba_req(ni, tid);
    return 0;
}

static void rs_tl_turn_on_agg(struct iwm_softc *sc,
                  u8 tid, struct iwl_lq_sta *lq_sta,
                  struct ieee80211_node *ni)
{
    struct iwm_tx_ba *tid_data;
    struct ieee80211_tx_ba *tx_ba;
    struct ieee80211com *ic = &sc->sc_ic;
    
    if ((ic->ic_caps & IEEE80211_C_TX_AMPDU_SETUP_IN_RS) == 0)
        return;

    /*
     * In AP mode, tid can be equal to IWL_MAX_TID_COUNT
     * when the frame is not QoS
     */
    if (WARN_ON_ONCE(tid > IWL_MAX_TID_COUNT)) {
        IWL_ERR(mvm, "tid exceeds max TID count: %d/%d\n",
            tid, IWL_MAX_TID_COUNT);
        return;
    } else if (tid == IWL_MAX_TID_COUNT) {
        return;
    }

    tx_ba = &ni->ni_tx_ba[tid];
    tid_data = &sc->sc_tx_ba[tid];
    if (sc->sc_ic.ic_state >= IEEE80211_S_RUN &&
        tx_ba->ba_state == IEEE80211_BA_INIT &&
        (lq_sta->tx_agg_tid_en & BIT(tid)) &&
        tid_data->tx_count_last >= IWL_MVM_RS_AGG_START_THRESHOLD) {
        IWL_DEBUG_RATE("RS: try to aggregate tid %d\n", tid);
        rs_tl_turn_on_agg_for_tid(sc, lq_sta, tid, ni);
    }
}

static inline int get_num_of_ant_from_rate(u32 rate_n_flags)
{
    return !!(rate_n_flags & RATE_MCS_ANT_A_MSK) +
           !!(rate_n_flags & RATE_MCS_ANT_B_MSK) +
           !!(rate_n_flags & RATE_MCS_ANT_C_MSK);
}

/*
 * Static function to get the expected throughput from an iwl_scale_tbl_info
 * that wraps a NULL pointer check
 */
static s32 get_expected_tpt(struct iwl_scale_tbl_info *tbl, int rs_index)
{
    if (tbl->expected_tpt)
        return tbl->expected_tpt[rs_index];
    return 0;
}

/*
 * rs_collect_tx_data - Update the success/failure sliding window
 *
 * We keep a sliding window of the last 62 packets transmitted
 * at this rate.  window->data contains the bitmask of successful
 * packets.
 */
static int _rs_collect_tx_data(struct iwm_softc *sc,
                   struct iwl_scale_tbl_info *tbl,
                   int scale_index, int attempts, int successes,
                   struct iwl_rate_scale_data *window)
{
    static const u64 mask = (((u64)1) << (IWL_RATE_MAX_WINDOW - 1));
    s32 fail_count, tpt;

    /* Get expected throughput */
    tpt = get_expected_tpt(tbl, scale_index);

    /*
     * Keep track of only the latest 62 tx frame attempts in this rate's
     * history window; anything older isn't really relevant any more.
     * If we have filled up the sliding window, drop the oldest attempt;
     * if the oldest attempt (highest bit in bitmap) shows "success",
     * subtract "1" from the success counter (this is the main reason
     * we keep these bitmaps!).
     */
    while (attempts > 0) {
        if (window->counter >= IWL_RATE_MAX_WINDOW) {
            /* remove earliest */
            window->counter = IWL_RATE_MAX_WINDOW - 1;

            if (window->data & mask) {
                window->data &= ~mask;
                window->success_counter--;
            }
        }

        /* Increment frames-attempted counter */
        window->counter++;

        /* Shift bitmap by one frame to throw away oldest history */
        window->data <<= 1;

        /* Mark the most recent #successes attempts as successful */
        if (successes > 0) {
            window->success_counter++;
            window->data |= 0x1;
            successes--;
        }

        attempts--;
    }

    /* Calculate current success ratio, avoid divide-by-0! */
    if (window->counter > 0)
        window->success_ratio = 128 * (100 * window->success_counter)
                    / window->counter;
    else
        window->success_ratio = IWL_INVALID_VALUE;

    fail_count = window->counter - window->success_counter;

    /* Calculate average throughput, if we have enough history. */
    if ((fail_count >= IWL_MVM_RS_RATE_MIN_FAILURE_TH) ||
        (window->success_counter >= IWL_MVM_RS_RATE_MIN_SUCCESS_TH))
        window->average_tpt = (window->success_ratio * tpt + 64) / 128;
    else
        window->average_tpt = IWL_INVALID_VALUE;

    return 0;
}

static int rs_collect_tpc_data(struct iwm_softc *sc,
                   struct iwl_lq_sta *lq_sta,
                   struct iwl_scale_tbl_info *tbl,
                   int scale_index, int attempts, int successes,
                   u8 reduced_txp)
{
    struct iwl_rate_scale_data *window = NULL;

    if (WARN_ON_ONCE(reduced_txp > TPC_MAX_REDUCTION))
        return -EINVAL;

    window = &tbl->tpc_win[reduced_txp];
    return  _rs_collect_tx_data(sc, tbl, scale_index, attempts, successes,
                    window);
}

static void rs_update_tid_tpt_stats(struct iwm_softc *sc,
                    u8 tid, int successes)
{
    if (tid >= IWL_MAX_TID_COUNT)
        return;

    struct iwm_tx_ba *tid_data = &sc->sc_tx_ba[tid];

    /*
     * Measure if there're enough successful transmits per second.
     * These statistics are used only to decide if we can start a
     * BA session, so it should be updated only when A-MPDU is
     * off.
     */
    if (tid_data->wn != NULL)
        return;

    if (time_is_before_jiffies(tid_data->tpt_meas_start + hz) ||
        (tid_data->tx_count >= IWL_MVM_RS_AGG_START_THRESHOLD)) {
        tid_data->tx_count_last = tid_data->tx_count;
        tid_data->tx_count = 0;
        tid_data->tpt_meas_start = ticks;
    } else {
        tid_data->tx_count += successes;
    }
}

static int rs_collect_tlc_data(struct iwm_softc *sc,
                   u8 tid,
                   struct iwl_scale_tbl_info *tbl,
                   int scale_index, int attempts, int successes)
{
    struct iwl_rate_scale_data *window = NULL;

    if (scale_index < 0 || scale_index >= IWL_RATE_COUNT)
        return -EINVAL;

    if (tbl->column != RS_COLUMN_INVALID) {
        struct lq_sta_pers *pers = &sc->lq_sta.rs_drv.pers;

        pers->tx_stats[tbl->column][scale_index].total += attempts;
        pers->tx_stats[tbl->column][scale_index].success += successes;
    }

    rs_update_tid_tpt_stats(sc, tid, successes);

    /* Select window for current tx bit rate */
    window = &(tbl->win[scale_index]);
    return _rs_collect_tx_data(sc, tbl, scale_index, attempts, successes,
                   window);
}

/* Convert rs_rate object into ucode rate bitmask */
static u32 ucode_rate_from_rs_rate(struct iwm_softc *sc,
                  struct rs_rate *rate)
{
    u32 ucode_rate = 0;
    int index = rate->index;

    ucode_rate |= ((rate->ant << RATE_MCS_ANT_POS) &
             RATE_MCS_ANT_ABC_MSK);

    if (is_legacy(rate)) {
        ucode_rate |= iwl_rates[index].plcp;
        if (index >= IWL_FIRST_CCK_RATE && index <= IWL_LAST_CCK_RATE)
            ucode_rate |= RATE_MCS_CCK_MSK;
        return ucode_rate;
    }

    /* set RTS protection for all non legacy rates
     * This helps with congested environments reducing the conflict cost to
     * RTS retries only, instead of the entire BA packet.
     */
    ucode_rate |= RATE_MCS_RTS_REQUIRED_MSK;

    if (is_ht(rate)) {
        if (index < IWL_FIRST_HT_RATE || index > IWL_LAST_HT_RATE) {
            IWL_ERR(mvm, "Invalid HT rate index %d\n", index);
            index = IWL_LAST_HT_RATE;
        }
        ucode_rate |= RATE_MCS_HT_MSK;

        if (is_ht_siso(rate))
            ucode_rate |= iwl_rates[index].plcp_ht_siso;
        else if (is_ht_mimo2(rate))
            ucode_rate |= iwl_rates[index].plcp_ht_mimo2;
        else
            WARN_ON_ONCE(1);
    } else if (is_vht(rate)) {
        if (index < IWL_FIRST_VHT_RATE || index > IWL_LAST_VHT_RATE) {
            IWL_ERR(mvm, "Invalid VHT rate index %d\n", index);
            index = IWL_LAST_VHT_RATE;
        }
        ucode_rate |= RATE_MCS_VHT_MSK;
        if (is_vht_siso(rate))
            ucode_rate |= iwl_rates[index].plcp_vht_siso;
        else if (is_vht_mimo2(rate))
            ucode_rate |= iwl_rates[index].plcp_vht_mimo2;
        else
            WARN_ON_ONCE(1);

    } else {
        IWL_ERR(mvm, "Invalid rate->type %d\n", rate->type);
    }

    if (is_siso(rate) && rate->stbc) {
        /* To enable STBC we need to set both a flag and ANT_AB */
        ucode_rate |= RATE_MCS_ANT_AB_MSK;
        ucode_rate |= RATE_MCS_STBC_MSK;
    }

    ucode_rate |= rate->bw;
    if (rate->sgi)
        ucode_rate |= RATE_MCS_SGI_MSK;
    if (rate->ldpc)
        ucode_rate |= RATE_MCS_LDPC_MSK;

    return ucode_rate;
}

/* Convert a ucode rate into an rs_rate object */
static int rs_rate_from_ucode_rate(const u32 ucode_rate,
                   enum nl80211_band band,
                   struct rs_rate *rate)
{
    u32 ant_msk = ucode_rate & RATE_MCS_ANT_ABC_MSK;
    u8 num_of_ant = get_num_of_ant_from_rate(ucode_rate);
    u8 nss;

    memset(rate, 0, sizeof(*rate));
    rate->index = iwl_hwrate_to_plcp_idx(ucode_rate);

    if (rate->index == IWL_RATE_INVALID)
        return -EINVAL;

    rate->ant = (ant_msk >> RATE_MCS_ANT_POS);

    /* Legacy */
    if (!(ucode_rate & RATE_MCS_HT_MSK) &&
        !(ucode_rate & RATE_MCS_VHT_MSK) &&
        !(ucode_rate & RATE_MCS_HE_MSK)) {
        if (num_of_ant == 1) {
            if (band == NL80211_BAND_5GHZ)
                rate->type = LQ_LEGACY_A;
            else
                rate->type = LQ_LEGACY_G;
        }

        return 0;
    }

    /* HT, VHT or HE */
    if (ucode_rate & RATE_MCS_SGI_MSK)
        rate->sgi = true;
    if (ucode_rate & RATE_MCS_LDPC_MSK)
        rate->ldpc = true;
    if (ucode_rate & RATE_MCS_STBC_MSK)
        rate->stbc = true;
    if (ucode_rate & RATE_MCS_BF_MSK)
        rate->bfer = true;

    rate->bw = ucode_rate & RATE_MCS_CHAN_WIDTH_MSK;

    if (ucode_rate & RATE_MCS_HT_MSK) {
        nss = ((ucode_rate & RATE_HT_MCS_NSS_MSK) >>
               RATE_HT_MCS_NSS_POS) + 1;

        if (nss == 1) {
            rate->type = LQ_HT_SISO;
            WARN_ON(!rate->stbc && !rate->bfer && num_of_ant != 1,
                  "stbc %d bfer %d",
                  rate->stbc, rate->bfer);
        } else if (nss == 2) {
            rate->type = LQ_HT_MIMO2;
            WARN_ON_ONCE(num_of_ant != 2);
        } else {
            WARN_ON_ONCE(1);
        }
    } else if (ucode_rate & RATE_MCS_VHT_MSK) {
        nss = ((ucode_rate & RATE_VHT_MCS_NSS_MSK) >>
               RATE_VHT_MCS_NSS_POS) + 1;

        if (nss == 1) {
            rate->type = LQ_VHT_SISO;
            WARN_ON(!rate->stbc && !rate->bfer && num_of_ant != 1,
                  "stbc %d bfer %d",
                  rate->stbc, rate->bfer);
        } else if (nss == 2) {
            rate->type = LQ_VHT_MIMO2;
            WARN_ON_ONCE(num_of_ant != 2);
        } else {
            WARN_ON_ONCE(1);
        }
    } else if (ucode_rate & RATE_MCS_HE_MSK) {
        nss = ((ucode_rate & RATE_VHT_MCS_NSS_MSK) >>
              RATE_VHT_MCS_NSS_POS) + 1;

        if (nss == 1) {
            rate->type = LQ_HE_SISO;
            WARN_ON(!rate->stbc && !rate->bfer && num_of_ant != 1,
                  "stbc %d bfer %d", rate->stbc, rate->bfer);
        } else if (nss == 2) {
            rate->type = LQ_HE_MIMO2;
            WARN_ON_ONCE(num_of_ant != 2);
        } else {
            WARN_ON_ONCE(1);
        }
    }

    WARN_ON_ONCE(rate->bw == RATE_MCS_CHAN_WIDTH_80 &&
             !is_he(rate) && !is_vht(rate));

    return 0;
}

/* switch to another antenna/antennas and return 1 */
/* if no other valid antenna found, return 0 */
static int rs_toggle_antenna(u32 valid_ant, struct rs_rate *rate)
{
    u8 new_ant_type;

    if (!rate->ant || WARN_ON_ONCE(rate->ant & ANT_C))
        return 0;

    if (!rs_is_valid_ant(valid_ant, rate->ant))
        return 0;

    new_ant_type = ant_toggle_lookup[rate->ant];

    while ((new_ant_type != rate->ant) &&
           !rs_is_valid_ant(valid_ant, new_ant_type))
        new_ant_type = ant_toggle_lookup[new_ant_type];

    if (new_ant_type == rate->ant)
        return 0;

    rate->ant = new_ant_type;

    return 1;
}

static u16 rs_get_supported_rates(struct iwl_lq_sta *lq_sta,
                  struct rs_rate *rate)
{
    if (is_legacy(rate))
        return lq_sta->active_legacy_rate;
    else if (is_siso(rate))
        return lq_sta->active_siso_rate;
    else if (is_mimo2(rate))
        return lq_sta->active_mimo2_rate;

    WARN_ON_ONCE(1);
    return 0;
}

static u16 rs_get_adjacent_rate(struct iwm_softc *sc, u8 index, u16 rate_mask,
                int rate_type)
{
    u8 high = IWL_RATE_INVALID;
    u8 low = IWL_RATE_INVALID;

    /* 802.11A or ht walks to the next literal adjacent rate in
     * the rate table */
    if (is_type_a_band(rate_type) || !is_type_legacy(rate_type)) {
        int i;
        u32 mask;

        /* Find the previous rate that is in the rate mask */
        i = index - 1;
        if (i >= 0)
            mask = BIT(i);
        for (; i >= 0; i--, mask >>= 1) {
            if (rate_mask & mask) {
                low = i;
                break;
            }
        }

        /* Find the next rate that is in the rate mask */
        i = index + 1;
        for (mask = (1 << i); i < IWL_RATE_COUNT; i++, mask <<= 1) {
            if (rate_mask & mask) {
                high = i;
                break;
            }
        }

        return (high << 8) | low;
    }

    low = index;
    while (low != IWL_RATE_INVALID) {
        low = iwl_rates[low].prev_rs;
        if (low == IWL_RATE_INVALID)
            break;
        if (rate_mask & (1 << low))
            break;
    }

    high = index;
    while (high != IWL_RATE_INVALID) {
        high = iwl_rates[high].next_rs;
        if (high == IWL_RATE_INVALID)
            break;
        if (rate_mask & (1 << high))
            break;
    }

    return (high << 8) | low;
}

static inline bool rs_rate_supported(struct iwl_lq_sta *lq_sta,
                     struct rs_rate *rate)
{
    return BIT(rate->index) & rs_get_supported_rates(lq_sta, rate);
}

/* Get the next supported lower rate in the current column.
 * Return true if bottom rate in the current column was reached
 */
static bool rs_get_lower_rate_in_column(struct iwl_lq_sta *lq_sta,
                    struct rs_rate *rate)
{
    u8 low;
    u16 high_low;
    u16 rate_mask;
    struct iwm_softc *sc = lq_sta->pers.drv;

    rate_mask = rs_get_supported_rates(lq_sta, rate);
    high_low = rs_get_adjacent_rate(sc, rate->index, rate_mask,
                    rate->type);
    low = high_low & 0xff;

    /* Bottom rate of column reached */
    if (low == IWL_RATE_INVALID)
        return true;

    rate->index = low;
    return false;
}

/* Get the next rate to use following a column downgrade */
static void rs_get_lower_rate_down_column(struct iwl_lq_sta *lq_sta,
                      struct rs_rate *rate)
{
    struct iwm_softc *sc = lq_sta->pers.drv;
    ItlIwm *that = container_of(sc, ItlIwm, com);

    if (is_legacy(rate)) {
        /* No column to downgrade from Legacy */
        return;
    } else if (is_siso(rate)) {
        /* Downgrade to Legacy if we were in SISO */
        if (lq_sta->band == NL80211_BAND_5GHZ)
            rate->type = LQ_LEGACY_A;
        else
            rate->type = LQ_LEGACY_G;

        rate->bw = RATE_MCS_CHAN_WIDTH_20;

        if (WARN_ON_ONCE(rate->index < IWL_RATE_MCS_0_INDEX))
            rate->index = rs_ht_to_legacy[IWL_RATE_MCS_0_INDEX];
        else if (WARN_ON_ONCE(rate->index > IWL_RATE_MCS_9_INDEX))
            rate->index = rs_ht_to_legacy[IWL_RATE_MCS_9_INDEX];
        else
            rate->index = rs_ht_to_legacy[rate->index];

        rate->ldpc = false;
    } else {
        /* Downgrade to SISO with same MCS if in MIMO  */
        rate->type = is_vht_mimo2(rate) ?
            LQ_VHT_SISO : LQ_HT_SISO;
    }

    if (that->iwm_num_of_ant(rate->ant) > 1)
        rate->ant = first_antenna(that->iwm_fw_valid_tx_ant(sc));

    /* Relevant in both switching to SISO or Legacy */
    rate->sgi = false;

    if (!rs_rate_supported(lq_sta, rate))
        rs_get_lower_rate_in_column(lq_sta, rate);
}

/* Check if both rates share the same column */
static inline bool rs_rate_column_match(struct rs_rate *a,
                    struct rs_rate *b)
{
    bool ant_match;

    if (a->stbc || a->bfer)
        ant_match = (b->ant == ANT_A || b->ant == ANT_B);
    else
        ant_match = (a->ant == b->ant);

    return (a->type == b->type) && (a->bw == b->bw) && (a->sgi == b->sgi)
        && ant_match;
}

static inline enum rs_column rs_get_column_from_rate(struct rs_rate *rate)
{
    if (is_legacy(rate)) {
        if (rate->ant == ANT_A)
            return RS_COLUMN_LEGACY_ANT_A;

        if (rate->ant == ANT_B)
            return RS_COLUMN_LEGACY_ANT_B;

        goto err;
    }

    if (is_siso(rate)) {
        if (rate->ant == ANT_A || rate->stbc || rate->bfer)
            return rate->sgi ? RS_COLUMN_SISO_ANT_A_SGI :
                RS_COLUMN_SISO_ANT_A;

        if (rate->ant == ANT_B)
            return rate->sgi ? RS_COLUMN_SISO_ANT_B_SGI :
                RS_COLUMN_SISO_ANT_B;

        goto err;
    }

    if (is_mimo(rate))
        return rate->sgi ? RS_COLUMN_MIMO2_SGI : RS_COLUMN_MIMO2;

err:
    return RS_COLUMN_INVALID;
}

/*
 * mac80211 sends us Tx status
 */
void rs_drv_mac80211_tx_status(struct iwm_softc *sc,
                                      struct ieee80211_node *sta,
                      struct ieee80211_tx_info *info, int tid, uint16_t fc, int ssn)
{
    if (!ieee80211_is_data(fc) ||
        info->flags & IEEE80211_TX_CTL_NO_ACK)
        return;

    iwl_mvm_rs_tx_status(sc, sta, tid, info,
                 false);
}

/*
 * Begin a period of staying with a selected modulation mode.
 * Set "stay_in_tbl" flag to prevent any mode switches.
 * Set frame tx success limits according to legacy vs. high-throughput,
 * and reset overall (spanning all rates) tx success history statistics.
 * These control how long we stay using same modulation mode before
 * searching for a new mode.
 */
static void rs_set_stay_in_table(struct iwm_softc *mvm, u8 is_legacy,
                 struct iwl_lq_sta *lq_sta)
{
    IWL_DEBUG_RATE(mvm, "Moving to RS_STATE_STAY_IN_COLUMN\n");
    lq_sta->rs_state = RS_STATE_STAY_IN_COLUMN;
    if (is_legacy) {
        lq_sta->table_count_limit = IWL_MVM_RS_LEGACY_TABLE_COUNT;
        lq_sta->max_failure_limit = IWL_MVM_RS_LEGACY_FAILURE_LIMIT;
        lq_sta->max_success_limit = IWL_MVM_RS_LEGACY_SUCCESS_LIMIT;
    } else {
        lq_sta->table_count_limit = IWL_MVM_RS_NON_LEGACY_TABLE_COUNT;
        lq_sta->max_failure_limit = IWL_MVM_RS_NON_LEGACY_FAILURE_LIMIT;
        lq_sta->max_success_limit = IWL_MVM_RS_NON_LEGACY_SUCCESS_LIMIT;
    }
    lq_sta->table_count = 0;
    lq_sta->total_failed = 0;
    lq_sta->total_success = 0;
    lq_sta->flush_timer = ticks;
    lq_sta->visited_columns = 0;
}

static inline int rs_get_max_rate_from_mask(unsigned long rate_mask)
{
    if (rate_mask)
        return find_last_bit(&rate_mask, BITS_PER_LONG);
    return IWL_RATE_INVALID;
}

static int rs_get_max_allowed_rate(struct iwl_lq_sta *lq_sta,
                   const struct rs_tx_column *column)
{
    switch (column->mode) {
    case RS_LEGACY:
        return lq_sta->max_legacy_rate_idx;
    case RS_SISO:
        return lq_sta->max_siso_rate_idx;
    case RS_MIMO2:
        return lq_sta->max_mimo2_rate_idx;
    default:
        WARN_ON_ONCE(1);
    }

    return lq_sta->max_legacy_rate_idx;
}

static const u16 *rs_get_expected_tpt_table(struct iwl_lq_sta *lq_sta,
                        const struct rs_tx_column *column,
                        u32 bw)
{
    /* Used to choose among HT tables */
    const u16 (*ht_tbl_pointer)[IWL_RATE_COUNT];

    if (WARN_ON_ONCE(column->mode != RS_LEGACY &&
             column->mode != RS_SISO &&
             column->mode != RS_MIMO2))
        return expected_tpt_legacy;

    /* Legacy rates have only one table */
    if (column->mode == RS_LEGACY)
        return expected_tpt_legacy;

    ht_tbl_pointer = expected_tpt_mimo2_20MHz;
    /* Choose among many HT tables depending on number of streams
     * (SISO/MIMO2), channel width (20/40/80), SGI, and aggregation
     * status */
    if (column->mode == RS_SISO) {
        switch (bw) {
        case RATE_MCS_CHAN_WIDTH_20:
            ht_tbl_pointer = expected_tpt_siso_20MHz;
            break;
        case RATE_MCS_CHAN_WIDTH_40:
            ht_tbl_pointer = expected_tpt_siso_40MHz;
            break;
        case RATE_MCS_CHAN_WIDTH_80:
            ht_tbl_pointer = expected_tpt_siso_80MHz;
            break;
        case RATE_MCS_CHAN_WIDTH_160:
            ht_tbl_pointer = expected_tpt_siso_160MHz;
            break;
        default:
            WARN_ON_ONCE(1);
        }
    } else if (column->mode == RS_MIMO2) {
        switch (bw) {
        case RATE_MCS_CHAN_WIDTH_20:
            ht_tbl_pointer = expected_tpt_mimo2_20MHz;
            break;
        case RATE_MCS_CHAN_WIDTH_40:
            ht_tbl_pointer = expected_tpt_mimo2_40MHz;
            break;
        case RATE_MCS_CHAN_WIDTH_80:
            ht_tbl_pointer = expected_tpt_mimo2_80MHz;
            break;
        case RATE_MCS_CHAN_WIDTH_160:
            ht_tbl_pointer = expected_tpt_mimo2_160MHz;
            break;
        default:
            WARN_ON_ONCE(1);
        }
    } else {
        WARN_ON_ONCE(1);
    }

    if (!column->sgi && !lq_sta->is_agg)        /* Normal */
        return ht_tbl_pointer[0];
    else if (column->sgi && !lq_sta->is_agg)        /* SGI */
        return ht_tbl_pointer[1];
    else if (!column->sgi && lq_sta->is_agg)        /* AGG */
        return ht_tbl_pointer[2];
    else                        /* AGG+SGI */
        return ht_tbl_pointer[3];
}

static void rs_set_expected_tpt_table(struct iwl_lq_sta *lq_sta,
                      struct iwl_scale_tbl_info *tbl)
{
    struct rs_rate *rate = &tbl->rate;
    const struct rs_tx_column *column = &rs_tx_columns[tbl->column];

    tbl->expected_tpt = rs_get_expected_tpt_table(lq_sta, column, rate->bw);
}

/* rs uses two tables, one is active and the second is for searching better
 * configuration. This function, according to the index of the currently
 * active table returns the search table, which is located at the
 * index complementary to 1 according to the active table (active = 1,
 * search = 0 or active = 0, search = 1).
 * Since lq_info is an arary of size 2, make sure index cannot be out of bounds.
 */
static inline u8 rs_search_tbl(u8 active_tbl)
{
    return (active_tbl ^ 1) & 1;
}

static s32 rs_get_best_rate(struct iwm_softc *sc,
                struct iwl_lq_sta *lq_sta,
                struct iwl_scale_tbl_info *tbl,    /* "search" */
                unsigned long rate_mask, s8 index)
{
    struct iwl_scale_tbl_info *active_tbl =
        &(lq_sta->lq_info[lq_sta->active_tbl]);
    s32 success_ratio = active_tbl->win[index].success_ratio;
    u16 expected_current_tpt = active_tbl->expected_tpt[index];
    const u16 *tpt_tbl = tbl->expected_tpt;
    u16 high_low;
    u32 target_tpt;
    int rate_idx;

    if (success_ratio >= RS_PERCENT(IWL_MVM_RS_SR_NO_DECREASE)) {
        target_tpt = 100 * expected_current_tpt;
        IWL_DEBUG_RATE(mvm,
                   "SR %d high. Find rate exceeding EXPECTED_CURRENT %d\n",
                   success_ratio, target_tpt);
    } else {
        target_tpt = lq_sta->last_tpt;
        IWL_DEBUG_RATE(mvm,
                   "SR %d not that good. Find rate exceeding ACTUAL_TPT %d\n",
                   success_ratio, target_tpt);
    }

    rate_idx = find_first_bit(&rate_mask, BITS_PER_LONG);

    while (rate_idx != IWL_RATE_INVALID) {
        if (target_tpt < (100 * tpt_tbl[rate_idx]))
            break;

        high_low = rs_get_adjacent_rate(sc, rate_idx, rate_mask,
                        tbl->rate.type);

        rate_idx = (high_low >> 8) & 0xff;
    }

    IWL_DEBUG_RATE(mvm, "Best rate found %d target_tp %d expected_new %d\n",
               rate_idx, target_tpt,
               rate_idx != IWL_RATE_INVALID ?
               100 * tpt_tbl[rate_idx] : IWL_INVALID_VALUE);

    return rate_idx;
}

static u32 rs_bw_from_sta_bw(struct ieee80211_node *ni)
{
    struct ieee80211_vht_cap vht_cap = {
        .vht_cap_info = cpu_to_le32(ni->ni_vhtcaps),
        .supp_mcs = ni->ni_vht_mcsinfo,
    };
    
    if (ni->ni_chw == IEEE80211_CHAN_WIDTH_40 &&
        IEEE80211_IS_CHAN_2GHZ(ni->ni_chan))
        return RATE_MCS_CHAN_WIDTH_20;

    switch (ni->ni_chw) {
    case IEEE80211_CHAN_WIDTH_160:
        /*
         * Don't use 160 MHz if VHT extended NSS support
         * says we cannot use 2 streams, we don't want to
         * deal with this.
         * We only check MCS 0 - they will support that if
         * we got here at all and we don't care which MCS,
         * we want to determine a more global state.
         */
        if (ieee80211_get_vht_max_nss(&vht_cap,
                          IEEE80211_VHT_CHANWIDTH_160MHZ,
                          0, true,
                          ni->ni_rx_nss) < ni->ni_rx_nss)
            return RATE_MCS_CHAN_WIDTH_80;
        return RATE_MCS_CHAN_WIDTH_160;
    case IEEE80211_CHAN_WIDTH_80:
        return RATE_MCS_CHAN_WIDTH_80;
    case IEEE80211_CHAN_WIDTH_40:
        return RATE_MCS_CHAN_WIDTH_40;
    case IEEE80211_CHAN_WIDTH_20:
    default:
        return RATE_MCS_CHAN_WIDTH_20;
    }
}

/*
 * Check whether we should continue using same modulation mode, or
 * begin search for a new mode, based on:
 * 1) # tx successes or failures while using this mode
 * 2) # times calling this function
 * 3) elapsed time in this mode (not used, for now)
 */
static void rs_stay_in_table(struct iwl_lq_sta *lq_sta, bool force_search)
{
    struct iwl_scale_tbl_info *tbl;
    int active_tbl;
    int flush_interval_passed = 0;
    struct iwm_softc *sc;

    sc = lq_sta->pers.drv;
    active_tbl = lq_sta->active_tbl;

    tbl = &(lq_sta->lq_info[active_tbl]);

    /* If we've been disallowing search, see if we should now allow it */
    if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN) {
        /* Elapsed time using current modulation mode */
        if (lq_sta->flush_timer)
            flush_interval_passed =
                time_after(ticks,
                       (unsigned long)(lq_sta->flush_timer +
                               (IWL_MVM_RS_STAY_IN_COLUMN_TIMEOUT * hz)));

        /*
         * Check if we should allow search for new modulation mode.
         * If many frames have failed or succeeded, or we've used
         * this same modulation for a long time, allow search, and
         * reset history stats that keep track of whether we should
         * allow a new search.  Also (below) reset all bitmaps and
         * stats in active history.
         */
        if (force_search ||
            (lq_sta->total_failed > lq_sta->max_failure_limit) ||
            (lq_sta->total_success > lq_sta->max_success_limit) ||
            ((!lq_sta->search_better_tbl) &&
             (lq_sta->flush_timer) && (flush_interval_passed))) {
            IWL_DEBUG_RATE(mvm,
                       "LQ: stay is expired %d %d %d\n",
                     lq_sta->total_failed,
                     lq_sta->total_success,
                     flush_interval_passed);

            /* Allow search for new mode */
            lq_sta->rs_state = RS_STATE_SEARCH_CYCLE_STARTED;
            IWL_DEBUG_RATE(mvm,
                       "Moving to RS_STATE_SEARCH_CYCLE_STARTED\n");
            lq_sta->total_failed = 0;
            lq_sta->total_success = 0;
            lq_sta->flush_timer = 0;
            /* mark the current column as visited */
            lq_sta->visited_columns = BIT(tbl->column);
        /*
         * Else if we've used this modulation mode enough repetitions
         * (regardless of elapsed time or success/failure), reset
         * history bitmaps and rate-specific stats for all rates in
         * active table.
         */
        } else {
            lq_sta->table_count++;
            if (lq_sta->table_count >=
                lq_sta->table_count_limit) {
                lq_sta->table_count = 0;

                IWL_DEBUG_RATE(sc,
                           "LQ: stay in table clear win\n");
                rs_rate_scale_clear_tbl_windows(sc, tbl);
            }
        }

        /* If transitioning to allow "search", reset all history
         * bitmaps and stats in active table (this will become the new
         * "search" table). */
        if (lq_sta->rs_state == RS_STATE_SEARCH_CYCLE_STARTED) {
            rs_rate_scale_clear_tbl_windows(sc, tbl);
        }
    }
}

static void rs_set_amsdu_len(struct iwm_softc *sc, struct ieee80211_node *ni,
                 struct iwl_scale_tbl_info *tbl,
                 enum rs_action scale_action)
{
#if 0
    struct iwl_mvm_sta *mvmsta = iwl_mvm_sta_from_mac80211(sta);
    int i;

    sta->max_amsdu_len = rs_fw_get_max_amsdu_len(sta);

    /*
     * In case TLC offload is not active amsdu_enabled is either 0xFFFF
     * or 0, since there is no per-TID alg.
     */
    if ((!is_vht(&tbl->rate) && !is_ht(&tbl->rate)) ||
        tbl->rate.index < IWL_RATE_MCS_5_INDEX ||
        scale_action == RS_ACTION_DOWNSCALE)
        mvmsta->amsdu_enabled = 0;
    else
        mvmsta->amsdu_enabled = 0xFFFF;

    if (mvmsta->vif->bss_conf.he_support &&
        !iwlwifi_mod_params.disable_11ax)
        mvmsta->max_amsdu_len = sta->max_amsdu_len;
    else
        mvmsta->max_amsdu_len = min_t(int, sta->max_amsdu_len, 8500);

    sta->max_rc_amsdu_len = mvmsta->max_amsdu_len;

    for (i = 0; i < IWL_MAX_TID_COUNT; i++) {
        if (mvmsta->amsdu_enabled)
            sta->max_tid_amsdu_len[i] =
                iwl_mvm_max_amsdu_size(mvm, sta, i);
        else
            /*
             * Not so elegant, but this will effectively
             * prevent AMSDU on this TID
             */
            sta->max_tid_amsdu_len[i] = 1;
    }
#endif
}

/**
 * iwl_mvm_send_lq_cmd() - Send link quality command
 * @mvm: Driver data.
 * @lq: Link quality command to send.
 *
 * The link quality command is sent as the last step of station creation.
 * This is the special case in which init is set and we call a callback in
 * this case to clear the state indicating that station creation is in
 * progress.
 */
int iwl_mvm_send_lq_cmd(struct iwm_softc *sc, struct iwm_lq_cmd *lq)
{
    ItlIwm *that = container_of(sc, ItlIwm, com);

    struct iwm_host_cmd cmd = {
        .id = IWM_LQ_CMD,
        .len = { sizeof(struct iwm_lq_cmd), },
        .flags = IWM_CMD_ASYNC,
        .data = { lq, },
    };

    return that->iwm_send_cmd(sc, &cmd);;
}

/*
 * setup rate table in uCode
 */
static void rs_update_rate_tbl(struct iwm_softc *sc,
                   struct ieee80211_node *ni,
                   struct iwl_lq_sta *lq_sta,
                   struct iwl_scale_tbl_info *tbl)
{
    rs_fill_lq_cmd(sc, ni, lq_sta, &tbl->rate);
    iwl_mvm_send_lq_cmd(sc, &lq_sta->lq);
}

static bool rs_tweak_rate_tbl(struct iwm_softc *sc,
                  struct ieee80211_node *ni,
                  struct iwl_lq_sta *lq_sta,
                  struct iwl_scale_tbl_info *tbl,
                  enum rs_action scale_action)
{
    if (rs_bw_from_sta_bw(ni) != RATE_MCS_CHAN_WIDTH_80)
        return false;

    if (!is_vht_siso(&tbl->rate))
        return false;

    if ((tbl->rate.bw == RATE_MCS_CHAN_WIDTH_80) &&
        (tbl->rate.index == IWL_RATE_MCS_0_INDEX) &&
        (scale_action == RS_ACTION_DOWNSCALE)) {
        tbl->rate.bw = RATE_MCS_CHAN_WIDTH_20;
        tbl->rate.index = IWL_RATE_MCS_4_INDEX;
        IWL_DEBUG_RATE(sc, "Switch 80Mhz SISO MCS0 -> 20Mhz MCS4\n");
        goto tweaked;
    }

    /* Go back to 80Mhz MCS1 only if we've established that 20Mhz MCS5 is
     * sustainable, i.e. we're past the test window. We can't go back
     * if MCS5 is just tested as this will happen always after switching
     * to 20Mhz MCS4 because the rate stats are cleared.
     */
    if ((tbl->rate.bw == RATE_MCS_CHAN_WIDTH_20) &&
        (((tbl->rate.index == IWL_RATE_MCS_5_INDEX) &&
         (scale_action == RS_ACTION_STAY)) ||
         ((tbl->rate.index > IWL_RATE_MCS_5_INDEX) &&
          (scale_action == RS_ACTION_UPSCALE)))) {
        tbl->rate.bw = RATE_MCS_CHAN_WIDTH_80;
        tbl->rate.index = IWL_RATE_MCS_1_INDEX;
        IWL_DEBUG_RATE(sc, "Switch 20Mhz SISO MCS5 -> 80Mhz MCS1\n");
        goto tweaked;
    }

    return false;

tweaked:
    rs_set_expected_tpt_table(lq_sta, tbl);
    rs_rate_scale_clear_tbl_windows(sc, tbl);
    return true;
}

static enum rs_column rs_get_next_column(struct iwm_softc *sc,
                     struct iwl_lq_sta *lq_sta,
                     struct ieee80211_node *ni,
                     struct iwl_scale_tbl_info *tbl)
{
    int i, j, max_rate;
    enum rs_column next_col_id;
    const struct rs_tx_column *curr_col = &rs_tx_columns[tbl->column];
    const struct rs_tx_column *next_col;
    allow_column_func_t allow_func;
    ItlIwm *that = container_of(sc, ItlIwm, com);
    u8 valid_ants = that->iwm_fw_valid_tx_ant(sc);
    const u16 *expected_tpt_tbl;
    u16 tpt, max_expected_tpt;

    for (i = 0; i < MAX_NEXT_COLUMNS; i++) {
        next_col_id = curr_col->next_columns[i];

        if (next_col_id == RS_COLUMN_INVALID)
            continue;

        if (lq_sta->visited_columns & BIT(next_col_id)) {
            IWL_DEBUG_RATE(sc, "Skip already visited column %d\n",
                       next_col_id);
            continue;
        }

        next_col = &rs_tx_columns[next_col_id];

        if (!rs_is_valid_ant(valid_ants, next_col->ant)) {
            IWL_DEBUG_RATE(sc,
                       "Skip column %d as ANT config isn't supported by chip. valid_ants 0x%x column ant 0x%x\n",
                       next_col_id, valid_ants, next_col->ant);
            continue;
        }

        for (j = 0; j < MAX_COLUMN_CHECKS; j++) {
            allow_func = next_col->checks[j];
            if (allow_func && !allow_func(sc, ni, &tbl->rate,
                              next_col))
                break;
        }

        if (j != MAX_COLUMN_CHECKS) {
            IWL_DEBUG_RATE(sc,
                       "Skip column %d: not allowed (check %d failed)\n",
                       next_col_id, j);

            continue;
        }

        tpt = lq_sta->last_tpt / 100;
        expected_tpt_tbl = rs_get_expected_tpt_table(lq_sta, next_col,
                             rs_bw_from_sta_bw(ni));
        if (WARN_ON_ONCE(!expected_tpt_tbl))
            continue;

        max_rate = rs_get_max_allowed_rate(lq_sta, next_col);
        if (max_rate == IWL_RATE_INVALID) {
            IWL_DEBUG_RATE(sc,
                       "Skip column %d: no rate is allowed in this column\n",
                       next_col_id);
            continue;
        }

        max_expected_tpt = expected_tpt_tbl[max_rate];
        if (tpt >= max_expected_tpt) {
            IWL_DEBUG_RATE(sc,
                       "Skip column %d: can't beat current TPT. Max expected %d current %d\n",
                       next_col_id, max_expected_tpt, tpt);
            continue;
        }

        IWL_DEBUG_RATE(sc,
                   "Found potential column %d. Max expected %d current %d\n",
                   next_col_id, max_expected_tpt, tpt);
        break;
    }

    if (i == MAX_NEXT_COLUMNS)
        return RS_COLUMN_INVALID;

    return next_col_id;
}

static int rs_switch_to_column(struct iwm_softc *sc,
                   struct iwl_lq_sta *lq_sta,
                   struct ieee80211_node *ni,
                   enum rs_column col_id)
{
    struct iwl_scale_tbl_info *tbl = &lq_sta->lq_info[lq_sta->active_tbl];
    struct iwl_scale_tbl_info *search_tbl =
        &lq_sta->lq_info[rs_search_tbl(lq_sta->active_tbl)];
    struct rs_rate *rate = &search_tbl->rate;
    const struct rs_tx_column *column = &rs_tx_columns[col_id];
    const struct rs_tx_column *curr_column = &rs_tx_columns[tbl->column];
    unsigned long rate_mask = 0;
    u32 rate_idx = 0;

    memcpy(search_tbl, tbl, offsetof(struct iwl_scale_tbl_info, win));

    rate->sgi = column->sgi;
    rate->ant = column->ant;

    if (column->mode == RS_LEGACY) {
        if (lq_sta->band == NL80211_BAND_5GHZ)
            rate->type = LQ_LEGACY_A;
        else
            rate->type = LQ_LEGACY_G;

        rate->bw = RATE_MCS_CHAN_WIDTH_20;
        rate->ldpc = false;
        rate_mask = lq_sta->active_legacy_rate;
    } else if (column->mode == RS_SISO) {
        rate->type = lq_sta->is_vht ? LQ_VHT_SISO : LQ_HT_SISO;
        rate_mask = lq_sta->active_siso_rate;
    } else if (column->mode == RS_MIMO2) {
        rate->type = lq_sta->is_vht ? LQ_VHT_MIMO2 : LQ_HT_MIMO2;
        rate_mask = lq_sta->active_mimo2_rate;
    } else {
        WARN_ON(1, "Bad column mode");
    }

    if (column->mode != RS_LEGACY) {
        rate->bw = rs_bw_from_sta_bw(ni);
        rate->ldpc = lq_sta->ldpc;
    }

    search_tbl->column = col_id;
    rs_set_expected_tpt_table(lq_sta, search_tbl);

    lq_sta->visited_columns |= BIT(col_id);

    /* Get the best matching rate if we're changing modes. e.g.
     * SISO->MIMO, LEGACY->SISO, MIMO->SISO
     */
    if (curr_column->mode != column->mode) {
        rate_idx = rs_get_best_rate(sc, lq_sta, search_tbl,
                        rate_mask, rate->index);

        if ((rate_idx == IWL_RATE_INVALID) ||
            !(BIT(rate_idx) & rate_mask)) {
            IWL_DEBUG_RATE(sc,
                       "can not switch with index %d"
                       " rate mask %lx\n",
                       rate_idx, rate_mask);

            goto err;
        }

        rate->index = rate_idx;
    }

    IWL_DEBUG_RATE(sc, "Switched to column %d: Index %d\n",
               col_id, rate->index);

    return 0;

err:
    rate->type = LQ_NONE;
    return -1;
}

static enum rs_action rs_get_rate_action(struct iwm_softc *sc,
                     struct iwl_scale_tbl_info *tbl,
                     s32 sr, int low, int high,
                     int current_tpt,
                     int low_tpt, int high_tpt)
{
    enum rs_action action = RS_ACTION_STAY;

    if ((sr <= RS_PERCENT(IWL_MVM_RS_SR_FORCE_DECREASE)) ||
        (current_tpt == 0)) {
        IWL_DEBUG_RATE(mvm,
                   "Decrease rate because of low SR\n");
        return RS_ACTION_DOWNSCALE;
    }

    if ((low_tpt == IWL_INVALID_VALUE) &&
        (high_tpt == IWL_INVALID_VALUE) &&
        (high != IWL_RATE_INVALID)) {
        IWL_DEBUG_RATE(mvm,
                   "No data about high/low rates. Increase rate\n");
        return RS_ACTION_UPSCALE;
    }

    if ((high_tpt == IWL_INVALID_VALUE) &&
        (high != IWL_RATE_INVALID) &&
        (low_tpt != IWL_INVALID_VALUE) &&
        (low_tpt < current_tpt)) {
        IWL_DEBUG_RATE(mvm,
                   "No data about high rate and low rate is worse. Increase rate\n");
        return RS_ACTION_UPSCALE;
    }

    if ((high_tpt != IWL_INVALID_VALUE) &&
        (high_tpt > current_tpt)) {
        IWL_DEBUG_RATE(mvm,
                   "Higher rate is better. Increate rate\n");
        return RS_ACTION_UPSCALE;
    }

    if ((low_tpt != IWL_INVALID_VALUE) &&
        (high_tpt != IWL_INVALID_VALUE) &&
        (low_tpt < current_tpt) &&
        (high_tpt < current_tpt)) {
        IWL_DEBUG_RATE(mvm,
                   "Both high and low are worse. Maintain rate\n");
        return RS_ACTION_STAY;
    }

    if ((low_tpt != IWL_INVALID_VALUE) &&
        (low_tpt > current_tpt)) {
        IWL_DEBUG_RATE(mvm,
                   "Lower rate is better\n");
        action = RS_ACTION_DOWNSCALE;
        goto out;
    }

    if ((low_tpt == IWL_INVALID_VALUE) &&
        (low != IWL_RATE_INVALID)) {
        IWL_DEBUG_RATE(mvm,
                   "No data about lower rate\n");
        action = RS_ACTION_DOWNSCALE;
        goto out;
    }

    IWL_DEBUG_RATE(mvm, "Maintain rate\n");

out:
    if ((action == RS_ACTION_DOWNSCALE) && (low != IWL_RATE_INVALID)) {
        if (sr >= RS_PERCENT(IWL_MVM_RS_SR_NO_DECREASE)) {
            IWL_DEBUG_RATE(mvm,
                       "SR is above NO DECREASE. Avoid downscale\n");
            action = RS_ACTION_STAY;
        } else if (current_tpt > (100 * tbl->expected_tpt[low])) {
            IWL_DEBUG_RATE(mvm,
                       "Current TPT is higher than max expected in low rate. Avoid downscale\n");
            action = RS_ACTION_STAY;
        } else {
            IWL_DEBUG_RATE(mvm, "Decrease rate\n");
        }
    }

    return action;
}

static bool rs_stbc_allow(struct iwm_softc *sc, struct ieee80211_node *ni,
              struct iwl_lq_sta *lq_sta)
{
    /* Our chip supports Tx STBC and the peer is an HT/VHT STA which
     * supports STBC of at least 1*SS
     */
    if (!lq_sta->stbc_capable)
        return false;

    if (!iwl_mvm_bt_coex_is_mimo_allowed(sc, ni))
        return false;

    return true;
}

static void rs_get_adjacent_txp(struct iwm_softc *mvm, int index,
                int *weaker, int *stronger)
{
    *weaker = index + IWL_MVM_RS_TPC_TX_POWER_STEP;
    if (*weaker > TPC_MAX_REDUCTION)
        *weaker = TPC_INVALID;

    *stronger = index - IWL_MVM_RS_TPC_TX_POWER_STEP;
    if (*stronger < 0)
        *stronger = TPC_INVALID;
}

static bool iwl_mvm_bt_coex_is_tpc_allowed(struct iwm_softc *mvm,
                    enum nl80211_band band)
{
    return ItlIwm::iwm_coex_is_tpc_allowed(mvm, band == NL80211_BAND_5GHZ);
}

static bool rs_tpc_allowed(struct iwm_softc *mvm,
               struct rs_rate *rate, enum nl80211_band band)
{
    int index = rate->index;
    bool cam = /*(iwlmvm_mod_params.power_scheme == IWL_POWER_SCHEME_CAM)*/ false;
    bool sta_ps_disabled = /*(vif->type == NL80211_IFTYPE_STATION &&
                !vif->bss_conf.ps)*/ true;

    IWL_DEBUG_RATE(mvm, "cam: %d sta_ps_disabled %d\n",
               cam, sta_ps_disabled);
    /*
     * allow tpc only if power management is enabled, or bt coex
     * activity grade allows it and we are on 2.4Ghz.
     */
    if ((cam || sta_ps_disabled) &&
        !iwl_mvm_bt_coex_is_tpc_allowed(mvm, band))
        return false;

    IWL_DEBUG_RATE(mvm, "check rate, table type: %d\n", rate->type);
    if (is_legacy(rate))
        return index == IWL_RATE_54M_INDEX;
    if (is_ht(rate))
        return index == IWL_RATE_MCS_7_INDEX;
    if (is_vht(rate))
        return index == IWL_RATE_MCS_9_INDEX;

    WARN_ON_ONCE(1);
    return false;
}

enum tpc_action {
    TPC_ACTION_STAY,
    TPC_ACTION_DECREASE,
    TPC_ACTION_INCREASE,
    TPC_ACTION_NO_RESTIRCTION,
};

static enum tpc_action rs_get_tpc_action(struct iwm_softc *mvm,
                     s32 sr, int weak, int strong,
                     int current_tpt,
                     int weak_tpt, int strong_tpt)
{
    /* stay until we have valid tpt */
    if (current_tpt == IWL_INVALID_VALUE) {
        IWL_DEBUG_RATE(mvm, "no current tpt. stay.\n");
        return TPC_ACTION_STAY;
    }

    /* Too many failures, increase txp */
    if (sr <= RS_PERCENT(IWL_MVM_RS_TPC_SR_FORCE_INCREASE) ||
        current_tpt == 0) {
        IWL_DEBUG_RATE(mvm, "increase txp because of weak SR\n");
        return TPC_ACTION_NO_RESTIRCTION;
    }

    /* try decreasing first if applicable */
    if (sr >= RS_PERCENT(IWL_MVM_RS_TPC_SR_NO_INCREASE) &&
        weak != TPC_INVALID) {
        if (weak_tpt == IWL_INVALID_VALUE &&
            (strong_tpt == IWL_INVALID_VALUE ||
             current_tpt >= strong_tpt)) {
            IWL_DEBUG_RATE(mvm,
                       "no weak txp measurement. decrease txp\n");
            return TPC_ACTION_DECREASE;
        }

        if (weak_tpt > current_tpt) {
            IWL_DEBUG_RATE(mvm,
                       "lower txp has better tpt. decrease txp\n");
            return TPC_ACTION_DECREASE;
        }
    }

    /* next, increase if needed */
    if (sr < RS_PERCENT(IWL_MVM_RS_TPC_SR_NO_INCREASE) &&
        strong != TPC_INVALID) {
        if (weak_tpt == IWL_INVALID_VALUE &&
            strong_tpt != IWL_INVALID_VALUE &&
            current_tpt < strong_tpt) {
            IWL_DEBUG_RATE(mvm,
                       "higher txp has better tpt. increase txp\n");
            return TPC_ACTION_INCREASE;
        }

        if (weak_tpt < current_tpt &&
            (strong_tpt == IWL_INVALID_VALUE ||
             strong_tpt > current_tpt)) {
            IWL_DEBUG_RATE(mvm,
                       "lower txp has worse tpt. increase txp\n");
            return TPC_ACTION_INCREASE;
        }
    }

    IWL_DEBUG_RATE(mvm, "no need to increase or decrease txp - stay\n");
    return TPC_ACTION_STAY;
}

static bool rs_tpc_perform(struct iwm_softc *mvm,
               struct ieee80211_node *ni,
               struct iwl_lq_sta *lq_sta,
               struct iwl_scale_tbl_info *tbl)
{
    enum nl80211_band band;
    struct iwl_rate_scale_data *window;
    struct rs_rate *rate = &tbl->rate;
    enum tpc_action action;
    s32 sr;
    u8 cur = lq_sta->lq.reduced_tpc;
    int current_tpt;
    int weak, strong;
    int weak_tpt = IWL_INVALID_VALUE, strong_tpt = IWL_INVALID_VALUE;

    if (WARN_ON(!ni->ni_chan))
        band = NUM_NL80211_BANDS;
    else
        band = IEEE80211_IS_CHAN_2GHZ(ni->ni_chan) ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;

    if (!rs_tpc_allowed(mvm, rate, band)) {
        IWL_DEBUG_RATE(mvm,
                   "tpc is not allowed. remove txp restrictions\n");
        lq_sta->lq.reduced_tpc = TPC_NO_REDUCTION;
        return cur != TPC_NO_REDUCTION;
    }

    rs_get_adjacent_txp(mvm, cur, &weak, &strong);

    /* Collect measured throughputs for current and adjacent rates */
    window = tbl->tpc_win;
    sr = window[cur].success_ratio;
    current_tpt = window[cur].average_tpt;
    if (weak != TPC_INVALID)
        weak_tpt = window[weak].average_tpt;
    if (strong != TPC_INVALID)
        strong_tpt = window[strong].average_tpt;

    IWL_DEBUG_RATE(mvm,
               "(TPC: %d): cur_tpt %d SR %d weak %d strong %d weak_tpt %d strong_tpt %d\n",
               cur, current_tpt, sr, weak, strong,
               weak_tpt, strong_tpt);

    action = rs_get_tpc_action(mvm, sr, weak, strong,
                   current_tpt, weak_tpt, strong_tpt);

    /* override actions if we are on the edge */
    if (weak == TPC_INVALID && action == TPC_ACTION_DECREASE) {
        IWL_DEBUG_RATE(mvm, "already in lowest txp, stay\n");
        action = TPC_ACTION_STAY;
    } else if (strong == TPC_INVALID &&
           (action == TPC_ACTION_INCREASE ||
            action == TPC_ACTION_NO_RESTIRCTION)) {
        IWL_DEBUG_RATE(mvm, "already in highest txp, stay\n");
        action = TPC_ACTION_STAY;
    }

    switch (action) {
    case TPC_ACTION_DECREASE:
        lq_sta->lq.reduced_tpc = weak;
        return true;
    case TPC_ACTION_INCREASE:
        lq_sta->lq.reduced_tpc = strong;
        return true;
    case TPC_ACTION_NO_RESTIRCTION:
        lq_sta->lq.reduced_tpc = TPC_NO_REDUCTION;
        return true;
    case TPC_ACTION_STAY:
        /* do nothing */
        break;
    }
    return false;
}

/*
 * Do rate scaling and search for new modulation mode.
 */
static void rs_rate_scale_perform(struct iwm_softc *mvm,
                  struct ieee80211_node *ni,
                  struct iwl_lq_sta *lq_sta,
                  int tid, bool ndp)
{
    int low = IWL_RATE_INVALID;
    int high = IWL_RATE_INVALID;
    int index;
    struct iwl_rate_scale_data *window = NULL;
    int current_tpt = IWL_INVALID_VALUE;
    int low_tpt = IWL_INVALID_VALUE;
    int high_tpt = IWL_INVALID_VALUE;
    u32 fail_count;
    enum rs_action scale_action = RS_ACTION_STAY;
    u16 rate_mask;
    u8 update_lq = 0;
    struct iwl_scale_tbl_info *tbl, *tbl1;
    u8 active_tbl = 0;
    u8 done_search = 0;
    u16 high_low;
    s32 sr;
    u8 prev_agg = lq_sta->is_agg;
    struct rs_rate *rate;
    int i;
    
    lq_sta->is_agg = false;
    for (i = 0; i <= IWM_MAX_TID_COUNT; i++) {
        if (mvm->sc_tx_ba[i].wn != NULL) {
            lq_sta->is_agg = true;
            break;
        }
    }

    /*
     * Select rate-scale / modulation-mode table to work with in
     * the rest of this function:  "search" if searching for better
     * modulation mode, or "active" if doing rate scaling within a mode.
     */
    if (!lq_sta->search_better_tbl)
        active_tbl = lq_sta->active_tbl;
    else
        active_tbl = rs_search_tbl(lq_sta->active_tbl);

    tbl = &(lq_sta->lq_info[active_tbl]);
    rate = &tbl->rate;

    if (prev_agg != lq_sta->is_agg) {
        XYLog("Aggregation changed: prev %d current %d. Update expected TPT table\n",
                   prev_agg, lq_sta->is_agg);
        rs_set_expected_tpt_table(lq_sta, tbl);
        rs_rate_scale_clear_tbl_windows(mvm, tbl);
    }

    /* current tx rate */
    index = rate->index;

    /* rates available for this association, and for modulation mode */
    rate_mask = rs_get_supported_rates(lq_sta, rate);

    if (!(BIT(index) & rate_mask)) {
        IWL_ERR(mvm, "Current Rate is not valid\n");
        if (lq_sta->search_better_tbl) {
            /* revert to active table if search table is not valid*/
            rate->type = LQ_NONE;
            lq_sta->search_better_tbl = 0;
            tbl = &(lq_sta->lq_info[lq_sta->active_tbl]);
            rs_update_rate_tbl(mvm, ni, lq_sta, tbl);
        }
        return;
    }

    /* Get expected throughput table and history window for current rate */
    if (!tbl->expected_tpt) {
        IWL_ERR(mvm, "tbl->expected_tpt is NULL\n");
        return;
    }

    /* TODO: handle rate_idx_mask and rate_idx_mcs_mask */
    window = &(tbl->win[index]);

    /*
     * If there is not enough history to calculate actual average
     * throughput, keep analyzing results of more tx frames, without
     * changing rate or mode (bypass most of the rest of this function).
     * Set up new rate table in uCode only if old rate is not supported
     * in current association (use new rate found above).
     */
    fail_count = window->counter - window->success_counter;
    if ((fail_count < IWL_MVM_RS_RATE_MIN_FAILURE_TH) &&
        (window->success_counter < IWL_MVM_RS_RATE_MIN_SUCCESS_TH)) {
        IWL_DEBUG_RATE(mvm,
                   "%s: Test Window: succ %d total %d\n",
                   rs_pretty_rate(rate),
                   window->success_counter, window->counter);

        /* Can't calculate this yet; not enough history */
        window->average_tpt = IWL_INVALID_VALUE;

        /* Should we stay with this modulation mode,
         * or search for a new one? */
        rs_stay_in_table(lq_sta, false);

        return;
    }

    /* If we are searching for better modulation mode, check success. */
    if (lq_sta->search_better_tbl) {
        /* If good success, continue using the "search" mode;
         * no need to send new link quality command, since we're
         * continuing to use the setup that we've been trying. */
        if (window->average_tpt > lq_sta->last_tpt) {
            IWL_DEBUG_RATE(mvm,
                       "SWITCHING TO NEW TABLE SR: %d "
                       "cur-tpt %d old-tpt %d\n",
                       window->success_ratio,
                       window->average_tpt,
                       lq_sta->last_tpt);

            /* Swap tables; "search" becomes "active" */
            lq_sta->active_tbl = active_tbl;
            current_tpt = window->average_tpt;
        /* Else poor success; go back to mode in "active" table */
        } else {
            IWL_DEBUG_RATE(mvm,
                       "GOING BACK TO THE OLD TABLE: SR %d "
                       "cur-tpt %d old-tpt %d\n",
                       window->success_ratio,
                       window->average_tpt,
                       lq_sta->last_tpt);

            /* Nullify "search" table */
            rate->type = LQ_NONE;

            /* Revert to "active" table */
            active_tbl = lq_sta->active_tbl;
            tbl = &(lq_sta->lq_info[active_tbl]);

            /* Revert to "active" rate and throughput info */
            index = tbl->rate.index;
            current_tpt = lq_sta->last_tpt;

            /* Need to set up a new rate table in uCode */
            update_lq = 1;
        }

        /* Either way, we've made a decision; modulation mode
         * search is done, allow rate adjustment next time. */
        lq_sta->search_better_tbl = 0;
        done_search = 1;    /* Don't switch modes below! */
        goto lq_update;
    }

    /* (Else) not in search of better modulation mode, try for better
     * starting rate, while staying in this mode. */
    high_low = rs_get_adjacent_rate(mvm, index, rate_mask, rate->type);
    low = high_low & 0xff;
    high = (high_low >> 8) & 0xff;

    /* TODO: handle rate_idx_mask and rate_idx_mcs_mask */

    sr = window->success_ratio;

    /* Collect measured throughputs for current and adjacent rates */
    current_tpt = window->average_tpt;
    if (low != IWL_RATE_INVALID)
        low_tpt = tbl->win[low].average_tpt;
    if (high != IWL_RATE_INVALID)
        high_tpt = tbl->win[high].average_tpt;

    IWL_DEBUG_RATE(mvm,
               "%s: cur_tpt %d SR %d low %d high %d low_tpt %d high_tpt %d\n",
               rs_pretty_rate(rate), current_tpt, sr,
               low, high, low_tpt, high_tpt);

    scale_action = rs_get_rate_action(mvm, tbl, sr, low, high,
                      current_tpt, low_tpt, high_tpt);

    /* Force a search in case BT doesn't like us being in MIMO */
    if (is_mimo(rate) &&
        !iwl_mvm_bt_coex_is_mimo_allowed(mvm, ni)) {
        IWL_DEBUG_RATE(mvm,
                   "BT Coex forbids MIMO. Search for new config\n");
        rs_stay_in_table(lq_sta, true);
        goto lq_update;
    }

    switch (scale_action) {
    case RS_ACTION_DOWNSCALE:
        /* Decrease starting rate, update uCode's rate table */
        if (low != IWL_RATE_INVALID) {
            update_lq = 1;
            index = low;
        } else {
            IWL_DEBUG_RATE(mvm,
                       "At the bottom rate. Can't decrease\n");
        }

        break;
    case RS_ACTION_UPSCALE:
        /* Increase starting rate, update uCode's rate table */
        if (high != IWL_RATE_INVALID) {
            update_lq = 1;
            index = high;
        } else {
            IWL_DEBUG_RATE(mvm,
                       "At the top rate. Can't increase\n");
        }

        break;
    case RS_ACTION_STAY:
        /* No change */
        if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN)
            update_lq = rs_tpc_perform(mvm, ni, lq_sta, tbl);
        break;
    default:
        break;
    }

lq_update:
    /* Replace uCode's rate table for the destination station. */
    if (update_lq) {
        tbl->rate.index = index;
        if (IWL_MVM_RS_80_20_FAR_RANGE_TWEAK)
            rs_tweak_rate_tbl(mvm, ni, lq_sta, tbl, scale_action);
        rs_set_amsdu_len(mvm, ni, tbl, scale_action);
        rs_update_rate_tbl(mvm, ni, lq_sta, tbl);
    }

    rs_stay_in_table(lq_sta, false);

    /*
     * Search for new modulation mode if we're:
     * 1)  Not changing rates right now
     * 2)  Not just finishing up a search
     * 3)  Allowing a new search
     */
    if (!update_lq && !done_search &&
        lq_sta->rs_state == RS_STATE_SEARCH_CYCLE_STARTED
        && window->counter) {
        enum rs_column next_column;

        /* Save current throughput to compare with "search" throughput*/
        lq_sta->last_tpt = current_tpt;

        IWL_DEBUG_RATE(mvm,
                   "Start Search: update_lq %d done_search %d rs_state %d win->counter %d\n",
                   update_lq, done_search, lq_sta->rs_state,
                   window->counter);

        next_column = rs_get_next_column(mvm, lq_sta, ni, tbl);
        if (next_column != RS_COLUMN_INVALID) {
            int ret = rs_switch_to_column(mvm, lq_sta, ni,
                              next_column);
            if (!ret)
                lq_sta->search_better_tbl = 1;
        } else {
            IWL_DEBUG_RATE(mvm,
                       "No more columns to explore in search cycle. Go to RS_STATE_SEARCH_CYCLE_ENDED\n");
            lq_sta->rs_state = RS_STATE_SEARCH_CYCLE_ENDED;
        }

        /* If new "search" mode was selected, set up in uCode table */
        if (lq_sta->search_better_tbl) {
            /* Access the "search" table, clear its history. */
            tbl = &lq_sta->lq_info[rs_search_tbl(lq_sta->active_tbl)];
            rs_rate_scale_clear_tbl_windows(mvm, tbl);

            /* Use new "search" start rate */
            index = tbl->rate.index;

            rs_dump_rate(mvm, &tbl->rate,
                     "Switch to SEARCH TABLE:");
            rs_update_rate_tbl(mvm, ni, lq_sta, tbl);
        } else {
            done_search = 1;
        }
    }

    if (!ndp)
        rs_tl_turn_on_agg(mvm, tid, lq_sta, ni);

    if (done_search && lq_sta->rs_state == RS_STATE_SEARCH_CYCLE_ENDED) {
        tbl1 = &(lq_sta->lq_info[lq_sta->active_tbl]);
        rs_set_stay_in_table(mvm, is_legacy(&tbl1->rate), lq_sta);
    }
}

struct rs_init_rate_info {
    s8 rssi;
    u8 rate_idx;
};

static const struct rs_init_rate_info rs_optimal_rates_24ghz_legacy[] = {
    { -60, IWL_RATE_54M_INDEX },
    { -64, IWL_RATE_48M_INDEX },
    { -68, IWL_RATE_36M_INDEX },
    { -80, IWL_RATE_24M_INDEX },
    { -84, IWL_RATE_18M_INDEX },
    { -85, IWL_RATE_12M_INDEX },
    { -86, IWL_RATE_11M_INDEX },
    { -88, IWL_RATE_5M_INDEX  },
    { -90, IWL_RATE_2M_INDEX  },
    { S8_MIN, IWL_RATE_1M_INDEX },
};

static const struct rs_init_rate_info rs_optimal_rates_5ghz_legacy[] = {
    { -60, IWL_RATE_54M_INDEX },
    { -64, IWL_RATE_48M_INDEX },
    { -72, IWL_RATE_36M_INDEX },
    { -80, IWL_RATE_24M_INDEX },
    { -84, IWL_RATE_18M_INDEX },
    { -85, IWL_RATE_12M_INDEX },
    { -87, IWL_RATE_9M_INDEX  },
    { S8_MIN, IWL_RATE_6M_INDEX },
};

static const struct rs_init_rate_info rs_optimal_rates_ht[] = {
    { -60, IWL_RATE_MCS_7_INDEX },
    { -64, IWL_RATE_MCS_6_INDEX },
    { -68, IWL_RATE_MCS_5_INDEX },
    { -72, IWL_RATE_MCS_4_INDEX },
    { -80, IWL_RATE_MCS_3_INDEX },
    { -84, IWL_RATE_MCS_2_INDEX },
    { -85, IWL_RATE_MCS_1_INDEX },
    { S8_MIN, IWL_RATE_MCS_0_INDEX},
};

/* MCS index 9 is not valid for 20MHz VHT channel width,
 * but is ok for 40, 80 and 160MHz channels.
 */
static const struct rs_init_rate_info rs_optimal_rates_vht_20mhz[] = {
    { -60, IWL_RATE_MCS_8_INDEX },
    { -64, IWL_RATE_MCS_7_INDEX },
    { -68, IWL_RATE_MCS_6_INDEX },
    { -72, IWL_RATE_MCS_5_INDEX },
    { -80, IWL_RATE_MCS_4_INDEX },
    { -84, IWL_RATE_MCS_3_INDEX },
    { -85, IWL_RATE_MCS_2_INDEX },
    { -87, IWL_RATE_MCS_1_INDEX },
    { S8_MIN, IWL_RATE_MCS_0_INDEX},
};

static const struct rs_init_rate_info rs_optimal_rates_vht[] = {
    { -60, IWL_RATE_MCS_9_INDEX },
    { -64, IWL_RATE_MCS_8_INDEX },
    { -68, IWL_RATE_MCS_7_INDEX },
    { -72, IWL_RATE_MCS_6_INDEX },
    { -80, IWL_RATE_MCS_5_INDEX },
    { -84, IWL_RATE_MCS_4_INDEX },
    { -85, IWL_RATE_MCS_3_INDEX },
    { -87, IWL_RATE_MCS_2_INDEX },
    { -88, IWL_RATE_MCS_1_INDEX },
    { S8_MIN, IWL_RATE_MCS_0_INDEX },
};

#define IWL_RS_LOW_RSSI_THRESHOLD (-76) /* dBm */

/* Init the optimal rate based on STA caps
 * This combined with rssi is used to report the last tx rate
 * to userspace when we haven't transmitted enough frames.
 */
static void rs_init_optimal_rate(struct iwm_softc *mvm,
                 struct ieee80211_node *sta,
                 struct iwl_lq_sta *lq_sta)
{
    struct rs_rate *rate = &lq_sta->optimal_rate;

    if (lq_sta->max_mimo2_rate_idx != IWL_RATE_INVALID)
        rate->type = lq_sta->is_vht ? LQ_VHT_MIMO2 : LQ_HT_MIMO2;
    else if (lq_sta->max_siso_rate_idx != IWL_RATE_INVALID)
        rate->type = lq_sta->is_vht ? LQ_VHT_SISO : LQ_HT_SISO;
    else if (lq_sta->band == NL80211_BAND_5GHZ)
        rate->type = LQ_LEGACY_A;
    else
        rate->type = LQ_LEGACY_G;

    rate->bw = rs_bw_from_sta_bw(sta);
    rate->sgi = rs_sgi_allow(mvm, sta, rate, NULL);

    /* ANT/LDPC/STBC aren't relevant for the rate reported to userspace */

    if (is_mimo(rate)) {
        lq_sta->optimal_rate_mask = lq_sta->active_mimo2_rate;
    } else if (is_siso(rate)) {
        lq_sta->optimal_rate_mask = lq_sta->active_siso_rate;
    } else {
        lq_sta->optimal_rate_mask = lq_sta->active_legacy_rate;

        if (lq_sta->band == NL80211_BAND_5GHZ) {
            lq_sta->optimal_rates = rs_optimal_rates_5ghz_legacy;
            lq_sta->optimal_nentries =
                ARRAY_SIZE(rs_optimal_rates_5ghz_legacy);
        } else {
            lq_sta->optimal_rates = rs_optimal_rates_24ghz_legacy;
            lq_sta->optimal_nentries =
                ARRAY_SIZE(rs_optimal_rates_24ghz_legacy);
        }
    }

    if (is_vht(rate)) {
        if (rate->bw == RATE_MCS_CHAN_WIDTH_20) {
            lq_sta->optimal_rates = rs_optimal_rates_vht_20mhz;
            lq_sta->optimal_nentries =
                ARRAY_SIZE(rs_optimal_rates_vht_20mhz);
        } else {
            lq_sta->optimal_rates = rs_optimal_rates_vht;
            lq_sta->optimal_nentries =
                ARRAY_SIZE(rs_optimal_rates_vht);
        }
    } else if (is_ht(rate)) {
        lq_sta->optimal_rates = rs_optimal_rates_ht;
        lq_sta->optimal_nentries = ARRAY_SIZE(rs_optimal_rates_ht);
    }
}

/* Compute the optimal rate index based on RSSI */
static struct rs_rate *rs_get_optimal_rate(struct iwm_softc *mvm,
                       struct iwl_lq_sta *lq_sta)
{
    struct rs_rate *rate = &lq_sta->optimal_rate;
    int i;

    rate->index = find_first_bit(&lq_sta->optimal_rate_mask,
                     BITS_PER_LONG);

    for (i = 0; i < lq_sta->optimal_nentries; i++) {
        int rate_idx = lq_sta->optimal_rates[i].rate_idx;

        if ((lq_sta->pers.last_rssi >= lq_sta->optimal_rates[i].rssi) &&
            (BIT(rate_idx) & lq_sta->optimal_rate_mask)) {
            rate->index = rate_idx;
            break;
        }
    }

    return rate;
}

/* Choose an initial legacy rate and antenna to use based on the RSSI
 * of last Rx
 */
static void rs_get_initial_rate(struct iwm_softc *mvm,
                struct ieee80211_node *sta,
                struct iwl_lq_sta *lq_sta,
                enum nl80211_band band,
                struct rs_rate *rate)
{
    int i, nentries;
    unsigned long active_rate;
    s8 best_rssi = S8_MIN;
    u8 best_ant = ANT_NONE;
    ItlIwm *that = container_of(mvm, ItlIwm, com);
    u8 valid_tx_ant = that->iwm_fw_valid_tx_ant(mvm);
    const struct rs_init_rate_info *initial_rates;

    for (i = 0; i < ARRAY_SIZE(lq_sta->pers.chain_signal); i++) {
        if (!(lq_sta->pers.chains & BIT(i)))
            continue;

        if (lq_sta->pers.chain_signal[i] > best_rssi) {
            best_rssi = lq_sta->pers.chain_signal[i];
            best_ant = BIT(i);
        }
    }

    IWL_DEBUG_RATE(mvm, "Best ANT: %s Best RSSI: %d\n",
               rs_pretty_ant(best_ant), best_rssi);

    if (best_ant != ANT_A && best_ant != ANT_B)
        rate->ant = first_antenna(valid_tx_ant);
    else
        rate->ant = best_ant;

    rate->sgi = false;
    rate->ldpc = false;
    rate->bw = RATE_MCS_CHAN_WIDTH_20;

    rate->index = find_first_bit(&lq_sta->active_legacy_rate,
                     BITS_PER_LONG);

    if (band == NL80211_BAND_5GHZ) {
        rate->type = LQ_LEGACY_A;
        initial_rates = rs_optimal_rates_5ghz_legacy;
        nentries = ARRAY_SIZE(rs_optimal_rates_5ghz_legacy);
    } else {
        rate->type = LQ_LEGACY_G;
        initial_rates = rs_optimal_rates_24ghz_legacy;
        nentries = ARRAY_SIZE(rs_optimal_rates_24ghz_legacy);
    }

    if (!IWL_MVM_RS_RSSI_BASED_INIT_RATE)
        goto out;

    /* Start from a higher rate if the corresponding debug capability
     * is enabled. The rate is chosen according to AP capabilities.
     * In case of VHT/HT when the rssi is low fallback to the case of
     * legacy rates.
     */
    if (ieee80211_node_supports_vht(sta) &&
        best_rssi > IWL_RS_LOW_RSSI_THRESHOLD) {
        /*
         * In AP mode, when a new station associates, rs is initialized
         * immediately upon association completion, before the phy
         * context is updated with the association parameters, so the
         * sta bandwidth might be wider than the phy context allows.
         * To avoid this issue, always initialize rs with 20mhz
         * bandwidth rate, and after authorization, when the phy context
         * is already up-to-date, re-init rs with the correct bw.
         */
        u32 bw = mvm->sc_ic.ic_state < IEEE80211_S_RUN ?
                RATE_MCS_CHAN_WIDTH_20 : rs_bw_from_sta_bw(sta);

        switch (bw) {
        case RATE_MCS_CHAN_WIDTH_40:
        case RATE_MCS_CHAN_WIDTH_80:
        case RATE_MCS_CHAN_WIDTH_160:
            initial_rates = rs_optimal_rates_vht;
            nentries = ARRAY_SIZE(rs_optimal_rates_vht);
            break;
        case RATE_MCS_CHAN_WIDTH_20:
            initial_rates = rs_optimal_rates_vht_20mhz;
            nentries = ARRAY_SIZE(rs_optimal_rates_vht_20mhz);
            break;
        default:
            IWL_ERR(mvm, "Invalid BW %d\n", sta->ni_chw);
            goto out;
        }

        active_rate = lq_sta->active_siso_rate;
        rate->type = LQ_VHT_SISO;
        rate->bw = bw;
    } else if (ieee80211_node_supports_ht(sta) &&
           best_rssi > IWL_RS_LOW_RSSI_THRESHOLD) {
        initial_rates = rs_optimal_rates_ht;
        nentries = ARRAY_SIZE(rs_optimal_rates_ht);
        active_rate = lq_sta->active_siso_rate;
        rate->type = LQ_HT_SISO;
    } else {
        active_rate = lq_sta->active_legacy_rate;
    }

    for (i = 0; i < nentries; i++) {
        int rate_idx = initial_rates[i].rate_idx;

        if ((best_rssi >= initial_rates[i].rssi) &&
            (BIT(rate_idx) & active_rate)) {
            rate->index = rate_idx;
            break;
        }
    }

out:
    rs_dump_rate(mvm, rate, "INITIAL");
}

/* Save info about RSSI of last Rx */
void rs_update_last_rssi(struct iwm_softc *mvm,
             struct ieee80211_rx_status *rx_status)
{
    struct iwl_lq_sta *lq_sta = &mvm->lq_sta.rs_drv;
    int i;

    lq_sta->pers.chains = rx_status->chains;
    lq_sta->pers.chain_signal[0] = rx_status->chain_signal[0];
    lq_sta->pers.chain_signal[1] = rx_status->chain_signal[1];
    lq_sta->pers.chain_signal[2] = rx_status->chain_signal[2];
    lq_sta->pers.last_rssi = S8_MIN;

    for (i = 0; i < ARRAY_SIZE(lq_sta->pers.chain_signal); i++) {
        if (!(lq_sta->pers.chains & BIT(i)))
            continue;

        if (lq_sta->pers.chain_signal[i] > lq_sta->pers.last_rssi)
            lq_sta->pers.last_rssi = lq_sta->pers.chain_signal[i];
    }
}

/*
 * rs_initialize_lq - Initialize a station's hardware rate table
 *
 * The uCode's station table contains a table of fallback rates
 * for automatic fallback during transmission.
 *
 * NOTE: This sets up a default set of values.  These will be replaced later
 *       if the driver's iwl-agn-rs rate scaling algorithm is used, instead of
 *       rc80211_simple.
 *
 * NOTE: Run REPLY_ADD_STA command to set up station table entry, before
 *       calling this function (which runs REPLY_TX_LINK_QUALITY_CMD,
 *       which requires station table entry to exist).
 */
static void rs_initialize_lq(struct iwm_softc *mvm,
                 struct ieee80211_node *sta,
                 struct iwl_lq_sta *lq_sta,
                 enum nl80211_band band)
{
    struct iwl_scale_tbl_info *tbl;
    struct rs_rate *rate;
    u8 active_tbl = 0;

    if (!sta || !lq_sta)
        return;

    if (!lq_sta->search_better_tbl)
        active_tbl = lq_sta->active_tbl;
    else
        active_tbl = rs_search_tbl(lq_sta->active_tbl);

    tbl = &(lq_sta->lq_info[active_tbl]);
    rate = &tbl->rate;

    rs_get_initial_rate(mvm, sta, lq_sta, band, rate);
    rs_init_optimal_rate(mvm, sta, lq_sta);

    WARN_ON(rate->ant != ANT_A && rate->ant != ANT_B,
          "ant: 0x%x, chains 0x%x, fw tx ant: 0x%x, nvm tx ant: 0x%x\n",
          rate->ant, lq_sta->pers.chains, mvm->fw->valid_tx_ant,
          mvm->nvm_data ? mvm->nvm_data->valid_tx_ant : ANT_INVALID);

    tbl->column = rs_get_column_from_rate(rate);

    rs_set_expected_tpt_table(lq_sta, tbl);
    rs_fill_lq_cmd(mvm, sta, lq_sta, rate);
    /* TODO restore station should remember the lq cmd */
    iwl_mvm_send_lq_cmd(mvm, &lq_sta->lq);
}

static void rs_drv_get_rate(struct iwm_softc *mvm, struct ieee80211_node *sta,
                struct ieee80211_tx_rate *r)
{
    struct iwl_lq_sta *lq_sta;
    struct rs_rate *optimal_rate;
    u32 last_ucode_rate;

    if (!sta)
        return;

    lq_sta = &mvm->lq_sta.rs_drv;
    
    IOSimpleLockLock(mvm->lq_sta.rs_drv.pers.lock);
    iwl_mvm_hwrate_to_tx_rate(lq_sta->last_rate_n_flags,
                  IEEE80211_IS_CHAN_2GHZ(sta->ni_chan) ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ, r);

    /* Report the optimal rate based on rssi and STA caps if we haven't
     * converged yet (too little traffic) or exploring other modulations
     */
    if (lq_sta->rs_state != RS_STATE_STAY_IN_COLUMN) {
        optimal_rate = rs_get_optimal_rate(mvm, lq_sta);
        last_ucode_rate = ucode_rate_from_rs_rate(mvm,
                              optimal_rate);
        iwl_mvm_hwrate_to_tx_rate(last_ucode_rate, IEEE80211_IS_CHAN_2GHZ(sta->ni_chan) ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ,
                      r);
    }
    IOSimpleLockUnlock(mvm->lq_sta.rs_drv.pers.lock);
}

void *rs_drv_alloc_sta(iwm_softc *sc, struct ieee80211_node *ni)
{
    struct iwl_lq_sta *lq_sta = &sc->lq_sta.rs_drv;

    IWL_DEBUG_RATE(mvm, "create station rate scale window\n");

    lq_sta->pers.drv = sc;
    lq_sta->pers.chains = 0;
    memset(lq_sta->pers.chain_signal, 0, sizeof(lq_sta->pers.chain_signal));
    lq_sta->pers.last_rssi = S8_MIN;

    return lq_sta;
}

void rs_drv_free_sta(iwm_softc *sc, struct ieee80211_node *ni)
{}

static int rs_vht_highest_rx_mcs_index(struct ieee80211_node *sta,
                       int nss)
{
    u16 rx_mcs = le16_to_cpu(sta->ni_vht_mcsinfo.rx_mcs_map) &
        (0x3 << (2 * (nss - 1)));
    rx_mcs >>= (2 * (nss - 1));

    if (rx_mcs == IEEE80211_VHT_MCS_SUPPORT_0_7)
        return IWL_RATE_MCS_7_INDEX;
    else if (rx_mcs == IEEE80211_VHT_MCS_SUPPORT_0_8)
        return IWL_RATE_MCS_8_INDEX;
    else if (rx_mcs == IEEE80211_VHT_MCS_SUPPORT_0_9)
        return IWL_RATE_MCS_9_INDEX;

    WARN_ON_ONCE(rx_mcs != IEEE80211_VHT_MCS_NOT_SUPPORTED);
    return -1;
}

static void rs_vht_set_enabled_rates(struct ieee80211_node *sta,
                     struct iwl_lq_sta *lq_sta)
{
    int i;
    int highest_mcs = rs_vht_highest_rx_mcs_index(sta, 1);

    if (highest_mcs >= IWL_RATE_MCS_0_INDEX) {
        for (i = IWL_RATE_MCS_0_INDEX; i <= highest_mcs; i++) {
            if (i == IWL_RATE_9M_INDEX)
                continue;

            /* VHT MCS9 isn't valid for 20Mhz for NSS=1,2 */
            if (i == IWL_RATE_MCS_9_INDEX &&
                sta->ni_chw == IEEE80211_CHAN_WIDTH_20)
                continue;

            lq_sta->active_siso_rate |= BIT(i);
        }
    }

    if (sta->ni_rx_nss < 2)
        return;

    highest_mcs = rs_vht_highest_rx_mcs_index(sta, 2);
    if (highest_mcs >= IWL_RATE_MCS_0_INDEX) {
        for (i = IWL_RATE_MCS_0_INDEX; i <= highest_mcs; i++) {
            if (i == IWL_RATE_9M_INDEX)
                continue;

            /* VHT MCS9 isn't valid for 20Mhz for NSS=1,2 */
            if (i == IWL_RATE_MCS_9_INDEX &&
                sta->ni_chw == IEEE80211_CHAN_WIDTH_20)
                continue;

            lq_sta->active_mimo2_rate |= BIT(i);
        }
    }
}

static void rs_ht_init(struct iwm_softc *mvm,
               struct ieee80211_node *sta,
               struct iwl_lq_sta *lq_sta)
{
    ItlIwm *that = container_of(mvm, ItlIwm, com);
    /* active_siso_rate mask includes 9 MBits (bit 5),
     * and CCK (bits 0-3), supp_rates[] does not;
     * shift to convert format, force 9 MBits off.
     */
    lq_sta->active_siso_rate = sta->ni_rxmcs[0] << 1;
    lq_sta->active_siso_rate |= sta->ni_rxmcs[0] & 0x1;
    lq_sta->active_siso_rate &= ~((u16)0x2);
    lq_sta->active_siso_rate <<= IWL_FIRST_OFDM_RATE;

    lq_sta->active_mimo2_rate = sta->ni_rxmcs[1] << 1;
    lq_sta->active_mimo2_rate |= sta->ni_rxmcs[1] & 0x1;
    lq_sta->active_mimo2_rate &= ~((u16)0x2);
    lq_sta->active_mimo2_rate <<= IWL_FIRST_OFDM_RATE;

    if ((sta->ni_htcaps & IEEE80211_HTCAP_LDPC) &&
        mvm->support_ldpc)
        lq_sta->ldpc = true;

    if ((that->iwm_num_of_ant(that->iwm_fw_valid_tx_ant(mvm)) > 1) &&
        (sta->ni_htcaps & IEEE80211_HTCAP_RXSTBC_MASK))
        lq_sta->stbc_capable = true;

    lq_sta->is_vht = false;
}

static void rs_vht_init(struct iwm_softc *mvm,
            struct ieee80211_node *sta,
            struct iwl_lq_sta *lq_sta)
{
    ItlIwm *that = container_of(mvm, ItlIwm, com);
    rs_vht_set_enabled_rates(sta, lq_sta);

    if ((sta->ni_vhtcaps & IEEE80211_VHTCAP_RXLDPC) &&
        mvm->support_ldpc)
        lq_sta->ldpc = true;

    if ((that->iwm_num_of_ant(that->iwm_fw_valid_tx_ant(mvm)) > 1) &&
        (sta->ni_vhtcaps & IEEE80211_VHTCAP_RXSTBC_MASK))
        lq_sta->stbc_capable = true;

    if (isset(mvm->sc_enabled_capa, IWM_UCODE_TLV_CAPA_BEAMFORMER) &&
        (that->iwm_num_of_ant(that->iwm_fw_valid_tx_ant(mvm)) > 1) &&
        (sta->ni_vhtcaps & IEEE80211_VHTCAP_SU_BEAMFORMEE_CAPABLE))
        lq_sta->bfer_capable = true;

    lq_sta->is_vht = true;
}

static u8 iwl_mvm_bt_coex_get_single_ant_msk(struct iwm_softc *mvm, u8 enabled_ants)
{
    if (isset(&mvm->sc_enabled_capa, IWM_UCODE_TLV_CAPA_COEX_SCHEMA_2) &&
        (mvm->non_shared_ant & enabled_ants))
        return mvm->non_shared_ant;

    return first_antenna(enabled_ants);
}

/**
 * struct ieee80211_rate - bitrate definition
 *
 * This structure describes a bitrate that an 802.11 PHY can
 * operate with. The two values @hw_value and @hw_value_short
 * are only for driver use when pointers to this structure are
 * passed around.
 *
 * @flags: rate-specific flags
 * @bitrate: bitrate in units of 100 Kbps
 * @hw_value: driver/hardware value for this rate
 * @hw_value_short: driver/hardware value for this rate when
 *    short preamble is used
 */
struct ieee80211_rate {
    u32 flags;
    u16 bitrate;
    u16 hw_value, hw_value_short;
};

/**
 * enum ieee80211_rate_flags - rate flags
 *
 * Hardware/specification flags for rates. These are structured
 * in a way that allows using the same bitrate structure for
 * different bands/PHY modes.
 *
 * @IEEE80211_RATE_SHORT_PREAMBLE: Hardware can send with short
 *    preamble on this bitrate; only relevant in 2.4GHz band and
 *    with CCK rates.
 * @IEEE80211_RATE_MANDATORY_A: This bitrate is a mandatory rate
 *    when used with 802.11a (on the 5 GHz band); filled by the
 *    core code when registering the wiphy.
 * @IEEE80211_RATE_MANDATORY_B: This bitrate is a mandatory rate
 *    when used with 802.11b (on the 2.4 GHz band); filled by the
 *    core code when registering the wiphy.
 * @IEEE80211_RATE_MANDATORY_G: This bitrate is a mandatory rate
 *    when used with 802.11g (on the 2.4 GHz band); filled by the
 *    core code when registering the wiphy.
 * @IEEE80211_RATE_ERP_G: This is an ERP rate in 802.11g mode.
 * @IEEE80211_RATE_SUPPORTS_5MHZ: Rate can be used in 5 MHz mode
 * @IEEE80211_RATE_SUPPORTS_10MHZ: Rate can be used in 10 MHz mode
 */
enum ieee80211_rate_flags {
    IEEE80211_RATE_SHORT_PREAMBLE    = 1<<0,
    IEEE80211_RATE_MANDATORY_A    = 1<<1,
    IEEE80211_RATE_MANDATORY_B    = 1<<2,
    IEEE80211_RATE_MANDATORY_G    = 1<<3,
    IEEE80211_RATE_ERP_G        = 1<<4,
    IEEE80211_RATE_SUPPORTS_5MHZ    = 1<<5,
    IEEE80211_RATE_SUPPORTS_10MHZ    = 1<<6,
};

/* rate data (static) */
static struct ieee80211_rate iwl_cfg80211_rates[] = {
    { .bitrate = 1 * 10, .hw_value = 0, .hw_value_short = 0, },
    { .bitrate = 2 * 10, .hw_value = 1, .hw_value_short = 1,
      .flags = IEEE80211_RATE_SHORT_PREAMBLE, },
    { .bitrate = (u16)(5.5 * 10), .hw_value = 2, .hw_value_short = 2,
      .flags = IEEE80211_RATE_SHORT_PREAMBLE, },
    { .bitrate = 11 * 10, .hw_value = 3, .hw_value_short = 3,
      .flags = IEEE80211_RATE_SHORT_PREAMBLE, },
    { .bitrate = 6 * 10, .hw_value = 4, .hw_value_short = 4, },
    { .bitrate = 9 * 10, .hw_value = 5, .hw_value_short = 5, },
    { .bitrate = 12 * 10, .hw_value = 6, .hw_value_short = 6, },
    { .bitrate = 18 * 10, .hw_value = 7, .hw_value_short = 7, },
    { .bitrate = 24 * 10, .hw_value = 8, .hw_value_short = 8, },
    { .bitrate = 36 * 10, .hw_value = 9, .hw_value_short = 9, },
    { .bitrate = 48 * 10, .hw_value = 10, .hw_value_short = 10, },
    { .bitrate = 54 * 10, .hw_value = 11, .hw_value_short = 11, },
};
#define RATES_24_OFFS    0
#define N_RATES_24    ARRAY_SIZE(iwl_cfg80211_rates)
#define RATES_52_OFFS    4
#define N_RATES_52    (N_RATES_24 - RATES_52_OFFS)

/*
 * Called after adding a new station to initialize rate scaling
 */
static void rs_drv_rate_init(struct iwm_softc *mvm, struct ieee80211_node *sta,
                 enum nl80211_band band)
{
    int i, j;
    struct iwl_lq_sta *lq_sta = &mvm->lq_sta.rs_drv;
    ItlIwm *that = container_of(mvm, ItlIwm, com);
    uint8_t rate;

//    lockdep_assert_held(&mvmsta->lq_sta.rs_drv.pers.lock);

    /* clear all non-persistent lq data */
    memset(lq_sta, 0, offsetof(typeof(*lq_sta), pers));

    lq_sta->lq.sta_id = IWM_STATION_ID;
#if 0
    mvm->amsdu_enabled = 0;
    mvm->max_amsdu_len = sta->max_amsdu_len;
#endif

    for (j = 0; j < LQ_SIZE; j++)
        rs_rate_scale_clear_tbl_windows(mvm, &lq_sta->lq_info[j]);

    lq_sta->flush_timer = 0;
    lq_sta->last_tx = ticks;

    IWL_DEBUG_RATE(mvm,
               "LQ: *** rate scale station global init for station %d ***\n",
               IWM_STATION_ID);
    /* TODO: what is a good starting rate for STA? About middle? Maybe not
     * the lowest or the highest rate.. Could consider using RSSI from
     * previous packets? Need to have IEEE 802.1X auth succeed immediately
     * after assoc.. */

    lq_sta->missed_rate_counter = IWL_MVM_RS_MISSED_RATE_MAX;
    lq_sta->band = band;
    /*
     * active legacy rates as per supported rates bitmap
     */
    lq_sta->active_legacy_rate = 0;
    for (i = 0; i < sta->ni_rates.rs_nrates; i++) {
        rate = sta->ni_rates.rs_rates[i] & IEEE80211_RATE_VAL;
        /* Map 802.11 rate to HW rate index. */
        for (j = 0; j <= (band == NL80211_BAND_2GHZ ? N_RATES_24 : N_RATES_52); j++) {
            if (iwl_cfg80211_rates[j + (band == NL80211_BAND_2GHZ ? RATES_24_OFFS : RATES_52_OFFS)].bitrate / 5 == rate)
                break;
        }
        lq_sta->active_legacy_rate |= BIT(iwl_cfg80211_rates[j + (band == NL80211_BAND_2GHZ ? RATES_24_OFFS : RATES_52_OFFS)].hw_value);
    }

    /* TODO: should probably account for rx_highest for both HT/VHT */
    if ((sta->ni_flags & IEEE80211_NODE_VHT) == 0)
        rs_ht_init(mvm, sta, lq_sta);
    else
        rs_vht_init(mvm, sta, lq_sta);

    lq_sta->max_legacy_rate_idx =
        rs_get_max_rate_from_mask(lq_sta->active_legacy_rate);
    lq_sta->max_siso_rate_idx =
        rs_get_max_rate_from_mask(lq_sta->active_siso_rate);
    lq_sta->max_mimo2_rate_idx =
        rs_get_max_rate_from_mask(lq_sta->active_mimo2_rate);

    XYLog("LEGACY=%lX SISO=%lX MIMO2=%lX VHT=%d LDPC=%d STBC=%d BFER=%d\n",
               lq_sta->active_legacy_rate,
               lq_sta->active_siso_rate,
               lq_sta->active_mimo2_rate,
               lq_sta->is_vht, lq_sta->ldpc, lq_sta->stbc_capable,
               lq_sta->bfer_capable);
    XYLog("MAX RATE: LEGACY=%d SISO=%d MIMO2=%d\n",
               lq_sta->max_legacy_rate_idx,
               lq_sta->max_siso_rate_idx,
               lq_sta->max_mimo2_rate_idx);

    /* These values will be overridden later */
    lq_sta->lq.single_stream_ant_msk =
        iwl_mvm_bt_coex_get_single_ant_msk(mvm, that->iwm_fw_valid_tx_ant(mvm));
    lq_sta->lq.dual_stream_ant_msk = ANT_AB;

    /* as default allow aggregation for all tids */
    lq_sta->tx_agg_tid_en = IWL_AGG_ALL_TID;
    lq_sta->is_agg = 0;
    rs_initialize_lq(mvm, sta, lq_sta, band);
}

void rs_drv_rate_update(struct iwm_softc *mvm,
                   struct ieee80211_node *sta,
                   enum nl80211_band band, u32 changed)
{
    /* Stop any ongoing aggregations as rs starts off assuming no agg */
    ieee80211_stop_ampdu_tx(&mvm->sc_ic, sta, 0);

    iwl_mvm_rs_rate_init(mvm, sta, band, true);
}

static void __iwl_mvm_rs_tx_status(struct iwm_softc *mvm,
                   struct ieee80211_node *sta,
                   int tid, struct ieee80211_tx_info *info,
                   bool ndp)
{
    int legacy_success;
    int retries;
    int i;
    struct iwm_lq_cmd *table;
    u32 lq_hwrate;
    uint32_t last_rate;
    struct rs_rate lq_rate, tx_resp_rate;
    struct iwl_scale_tbl_info *curr_tbl, *other_tbl, *tmp_tbl;
    u32 tlc_info = (u32)(uintptr_t)info->status.status_driver_data[0];
    u8 reduced_txp = tlc_info & RS_DRV_DATA_TXP_MSK;
    u8 lq_color = RS_DRV_DATA_LQ_COLOR_GET(tlc_info);
    u32 tx_resp_hwrate = (u32)(uintptr_t)info->status.status_driver_data[1];
    struct iwl_lq_sta *lq_sta = &mvm->lq_sta.rs_drv;
    char pretty_rate[100];

    if (!lq_sta->pers.drv) {
        IWL_DEBUG_RATE(mvm, "Rate scaling not initialized yet.\n");
        return;
    }

    /* This packet was aggregated but doesn't carry status info */
    if ((info->flags & IEEE80211_TX_CTL_AMPDU) &&
        !(info->flags & IEEE80211_TX_STAT_AMPDU))
        return;

    if (rs_rate_from_ucode_rate(tx_resp_hwrate, (enum nl80211_band)info->band,
                    &tx_resp_rate)) {
        WARN_ON_ONCE(1);
        return;
    }

    if (time_after(ticks,
               (unsigned long)(lq_sta->last_tx +
                       (IWL_MVM_RS_IDLE_TIMEOUT * hz)))) {
        IWL_DEBUG_RATE(mvm, "Tx idle for too long. reinit rs\n");
        /* reach here only in case of driver RS, call directly
         * the unlocked version
         */
        rs_drv_rate_init(mvm, sta, (nl80211_band)info->band);
        return;
    }
    lq_sta->last_tx = ticks;

    /* Ignore this Tx frame response if its initial rate doesn't match
     * that of latest Link Quality command.  There may be stragglers
     * from a previous Link Quality command, but we're no longer interested
     * in those; they're either from the "active" mode while we're trying
     * to check "search" mode, or a prior "search" mode after we've moved
     * to a new "search" mode (which might become the new "active" mode).
     */
    table = &lq_sta->lq;
    lq_hwrate = le32_to_cpu(table->rs_table[0]);
    if (rs_rate_from_ucode_rate(lq_hwrate, (enum nl80211_band)info->band, &lq_rate)) {
        WARN_ON_ONCE(1);
        return;
    }

    /* Here we actually compare this rate to the latest LQ command */
    if (lq_color != LQ_FLAG_COLOR_GET(table->flags)) {
        IWL_DEBUG_RATE(mvm,
                   "tx resp color 0x%x does not match 0x%x\n",
                   lq_color, LQ_FLAG_COLOR_GET(table->flags));

        /* Since rates mis-match, the last LQ command may have failed.
         * After IWL_MISSED_RATE_MAX mis-matches, resync the uCode with
         * ... driver.
         */
        lq_sta->missed_rate_counter++;
        if (lq_sta->missed_rate_counter > IWL_MVM_RS_MISSED_RATE_MAX) {
            lq_sta->missed_rate_counter = 0;
            IWL_DEBUG_RATE(mvm,
                       "Too many rates mismatch. Send sync LQ. rs_state %d\n",
                       lq_sta->rs_state);
            iwl_mvm_send_lq_cmd(mvm, &lq_sta->lq);
        }
        /* Regardless, ignore this status info for outdated rate */
        return;
    }

    /* Rate did match, so reset the missed_rate_counter */
    lq_sta->missed_rate_counter = 0;

    if (!lq_sta->search_better_tbl) {
        curr_tbl = &lq_sta->lq_info[lq_sta->active_tbl];
        other_tbl = &lq_sta->lq_info[rs_search_tbl(lq_sta->active_tbl)];
    } else {
        curr_tbl = &lq_sta->lq_info[rs_search_tbl(lq_sta->active_tbl)];
        other_tbl = &lq_sta->lq_info[lq_sta->active_tbl];
    }

    if (WARN_ON_ONCE(!rs_rate_column_match(&lq_rate, &curr_tbl->rate))) {
        IWL_DEBUG_RATE(mvm,
                   "Neither active nor search matches tx rate\n");
        tmp_tbl = &lq_sta->lq_info[lq_sta->active_tbl];
        rs_dump_rate(mvm, &tmp_tbl->rate, "ACTIVE");
        tmp_tbl = &lq_sta->lq_info[rs_search_tbl(lq_sta->active_tbl)];
        rs_dump_rate(mvm, &tmp_tbl->rate, "SEARCH");
        rs_dump_rate(mvm, &lq_rate, "ACTUAL");

        /* no matching table found, let's by-pass the data collection
         * and continue to perform rate scale to find the rate table
         */
        rs_stay_in_table(lq_sta, true);
        goto done;
    }

    /* Updating the frame history depends on whether packets were
     * aggregated.
     *
     * For aggregation, all packets were transmitted at the same rate, the
     * first index into rate scale table.
     */
    if (info->flags & IEEE80211_TX_STAT_AMPDU) {
        rs_collect_tpc_data(mvm, lq_sta, curr_tbl, tx_resp_rate.index,
                    info->status.ampdu_len,
                    info->status.ampdu_ack_len,
                    reduced_txp);

        /* ampdu_ack_len = 0 marks no BA was received. For TLC, treat
         * it as a single frame loss as we don't want the success ratio
         * to dip too quickly because a BA wasn't received.
         * For TPC, there's no need for this optimisation since we want
         * to recover very quickly from a bad power reduction and,
         * therefore we'd like the success ratio to get an immediate hit
         * when failing to get a BA, so we'd switch back to a lower or
         * zero power reduction. When FW transmits agg with a rate
         * different from the initial rate, it will not use reduced txp
         * and will send BA notification twice (one empty with reduced
         * txp equal to the value from LQ and one with reduced txp 0).
         * We need to update counters for each txp level accordingly.
         */
        if (info->status.ampdu_ack_len == 0)
            info->status.ampdu_len = 1;

        rs_collect_tlc_data(mvm, tid, curr_tbl,
                    tx_resp_rate.index,
                    info->status.ampdu_len,
                    info->status.ampdu_ack_len);

        /* Update success/fail counts if not searching for new mode */
        if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN) {
            lq_sta->total_success += info->status.ampdu_ack_len;
            lq_sta->total_failed += (info->status.ampdu_len -
                    info->status.ampdu_ack_len);
        }
    } else {
        /* For legacy, update frame history with for each Tx retry. */
        retries = info->status.rates[0].count - 1;
        /* HW doesn't send more than 15 retries */
        retries = min(retries, 15);

        /* The last transmission may have been successful */
        legacy_success = !!(info->flags & IEEE80211_TX_STAT_ACK);
        /* Collect data for each rate used during failed TX attempts */
        for (i = 0; i <= retries; ++i) {
            lq_hwrate = le32_to_cpu(table->rs_table[i]);
            if (rs_rate_from_ucode_rate(lq_hwrate, (enum nl80211_band)info->band,
                            &lq_rate)) {
                WARN_ON_ONCE(1);
                return;
            }

            /* Only collect stats if retried rate is in the same RS
             * table as active/search.
             */
            if (rs_rate_column_match(&lq_rate, &curr_tbl->rate))
                tmp_tbl = curr_tbl;
            else if (rs_rate_column_match(&lq_rate,
                              &other_tbl->rate))
                tmp_tbl = other_tbl;
            else
                continue;

            rs_collect_tpc_data(mvm, lq_sta, tmp_tbl,
                        tx_resp_rate.index, 1,
                        i < retries ? 0 : legacy_success,
                        reduced_txp);
            rs_collect_tlc_data(mvm, tid, tmp_tbl,
                        tx_resp_rate.index, 1,
                        i < retries ? 0 : legacy_success);
        }

        /* Update success/fail counts if not searching for new mode */
        if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN) {
            lq_sta->total_success += legacy_success;
            lq_sta->total_failed += retries + (1 - legacy_success);
        }
    }
    /* The last TX rate is cached in lq_sta; it's set in if/else above */
    last_rate = lq_sta->last_rate_n_flags;
    lq_sta->last_rate_n_flags = lq_hwrate;
    if (last_rate != lq_hwrate) {
        rs_pretty_print_rate(pretty_rate, sizeof(pretty_rate), lq_sta->last_rate_n_flags);
        XYLog("%s new rate: %s\n", __FUNCTION__, pretty_rate);
    }
    if ((lq_sta->last_rate_n_flags & RATE_MCS_VHT_MSK) || (lq_sta->last_rate_n_flags & RATE_MCS_HE_MSK)) {
        sta->ni_txmcs = (lq_sta->last_rate_n_flags & RATE_VHT_MCS_RATE_CODE_MSK);
    } else if (lq_sta->last_rate_n_flags & RATE_MCS_HT_MSK) {
        sta->ni_txmcs = (lq_sta->last_rate_n_flags &
                        (RATE_HT_MCS_RATE_CODE_MSK |
                         RATE_HT_MCS_NSS_MSK));
    } else {
        int index = iwl_mvm_legacy_rate_to_mac80211_idx(lq_sta->last_rate_n_flags, (enum nl80211_band)info->band);
        if (index < 0 || index >= ieee80211_std_rateset_11g.rs_nrates)
            goto done;
        
        sta->ni_txrate = ieee80211_std_rateset_11g.rs_rates[index] / 2;
    }
    IWL_DEBUG_RATE(mvm, "reduced txpower: %d\n", reduced_txp);
done:
    /* See if there's a better rate or modulation mode to try. */
    if (/*sta->supp_rates[info->band]*/ true)
        rs_rate_scale_perform(mvm, sta, lq_sta, tid, ndp);
}

void iwl_mvm_rs_tx_status(struct iwm_softc *mvm, struct ieee80211_node *sta,
              int tid, struct ieee80211_tx_info *info, bool ndp)
{
    /* If it's locked we are in middle of init flow
     * just wait for next tx status to update the lq_sta data
     */
    if (!IOSimpleLockTryLock(mvm->lq_sta.rs_drv.pers.lock))
        return;

    __iwl_mvm_rs_tx_status(mvm, sta, tid, info, ndp);
    IOSimpleLockUnlock(mvm->lq_sta.rs_drv.pers.lock);
}

static void rs_fill_rates_for_column(struct iwm_softc *mvm,
                     struct iwl_lq_sta *lq_sta,
                     struct rs_rate *rate,
                     __le32 *rs_table, int *rs_table_index,
                     int num_rates, int num_retries,
                     u8 valid_tx_ant, bool toggle_ant)
{
    int i, j;
    __le32 ucode_rate;
    bool bottom_reached = false;
    int prev_rate_idx = rate->index;
    int end = LINK_QUAL_MAX_RETRY_NUM;
    int index = *rs_table_index;

    for (i = 0; i < num_rates && index < end; i++) {
        for (j = 0; j < num_retries && index < end; j++, index++) {
            ucode_rate = cpu_to_le32(ucode_rate_from_rs_rate(mvm,
                                     rate));
            rs_table[index] = ucode_rate;
            if (toggle_ant)
                rs_toggle_antenna(valid_tx_ant, rate);
        }

        prev_rate_idx = rate->index;
        bottom_reached = rs_get_lower_rate_in_column(lq_sta, rate);
        if (bottom_reached && !is_legacy(rate))
            break;
    }

    if (!bottom_reached && !is_legacy(rate))
        rate->index = prev_rate_idx;

    *rs_table_index = index;
}

/* Building the rate table is non trivial. When we're in MIMO2/VHT/80Mhz/SGI
 * column the rate table should look like this:
 *
 * rate[0] 0x400F019 VHT | ANT: AB BW: 80Mhz MCS: 9 NSS: 2 SGI
 * rate[1] 0x400F019 VHT | ANT: AB BW: 80Mhz MCS: 9 NSS: 2 SGI
 * rate[2] 0x400F018 VHT | ANT: AB BW: 80Mhz MCS: 8 NSS: 2 SGI
 * rate[3] 0x400F018 VHT | ANT: AB BW: 80Mhz MCS: 8 NSS: 2 SGI
 * rate[4] 0x400F017 VHT | ANT: AB BW: 80Mhz MCS: 7 NSS: 2 SGI
 * rate[5] 0x400F017 VHT | ANT: AB BW: 80Mhz MCS: 7 NSS: 2 SGI
 * rate[6] 0x4005007 VHT | ANT: A BW: 80Mhz MCS: 7 NSS: 1 NGI
 * rate[7] 0x4009006 VHT | ANT: B BW: 80Mhz MCS: 6 NSS: 1 NGI
 * rate[8] 0x4005005 VHT | ANT: A BW: 80Mhz MCS: 5 NSS: 1 NGI
 * rate[9] 0x800B Legacy | ANT: B Rate: 36 Mbps
 * rate[10] 0x4009 Legacy | ANT: A Rate: 24 Mbps
 * rate[11] 0x8007 Legacy | ANT: B Rate: 18 Mbps
 * rate[12] 0x4005 Legacy | ANT: A Rate: 12 Mbps
 * rate[13] 0x800F Legacy | ANT: B Rate: 9 Mbps
 * rate[14] 0x400D Legacy | ANT: A Rate: 6 Mbps
 * rate[15] 0x800D Legacy | ANT: B Rate: 6 Mbps
 */
static void rs_build_rates_table(struct iwm_softc *mvm,
                 struct ieee80211_node *sta,
                 struct iwl_lq_sta *lq_sta,
                 const struct rs_rate *initial_rate)
{
    struct rs_rate rate;
    int num_rates, num_retries, index = 0;
    u8 valid_tx_ant = 0;
    struct iwm_lq_cmd *lq_cmd = &lq_sta->lq;
    bool toggle_ant = false;
    u32 color;
    ItlIwm *that = container_of(mvm, ItlIwm, com);

    memcpy(&rate, initial_rate, sizeof(rate));

    valid_tx_ant = that->iwm_fw_valid_tx_ant(mvm);

    /* TODO: remove old API when min FW API hits 14 */
    if (!isset(&mvm->sc_ucode_api, IWM_UCODE_TLV_API_LQ_SS_PARAMS) &&
        rs_stbc_allow(mvm, sta, lq_sta))
        rate.stbc = true;

    if (is_siso(&rate)) {
        num_rates = IWL_MVM_RS_INITIAL_SISO_NUM_RATES;
        num_retries = IWL_MVM_RS_HT_VHT_RETRIES_PER_RATE;
    } else if (is_mimo(&rate)) {
        num_rates = IWL_MVM_RS_INITIAL_MIMO_NUM_RATES;
        num_retries = IWL_MVM_RS_HT_VHT_RETRIES_PER_RATE;
    } else {
        num_rates = IWL_MVM_RS_INITIAL_LEGACY_NUM_RATES;
        num_retries = IWL_MVM_RS_INITIAL_LEGACY_RETRIES;
        toggle_ant = true;
    }

    rs_fill_rates_for_column(mvm, lq_sta, &rate, (__le32 *)lq_cmd->rs_table, &index,
                 num_rates, num_retries, valid_tx_ant,
                 toggle_ant);

    rs_get_lower_rate_down_column(lq_sta, &rate);

    if (is_siso(&rate)) {
        num_rates = IWL_MVM_RS_SECONDARY_SISO_NUM_RATES;
        num_retries = IWL_MVM_RS_SECONDARY_SISO_RETRIES;
        lq_cmd->mimo_delim = index;
    } else if (is_legacy(&rate)) {
        num_rates = IWL_MVM_RS_SECONDARY_LEGACY_NUM_RATES;
        num_retries = IWL_MVM_RS_SECONDARY_LEGACY_RETRIES;
    } else {
        WARN_ON_ONCE(1);
    }

    toggle_ant = true;

    rs_fill_rates_for_column(mvm, lq_sta, &rate, (__le32 *)lq_cmd->rs_table, &index,
                 num_rates, num_retries, valid_tx_ant,
                 toggle_ant);

    rs_get_lower_rate_down_column(lq_sta, &rate);

    num_rates = IWL_MVM_RS_SECONDARY_LEGACY_NUM_RATES;
    num_retries = IWL_MVM_RS_SECONDARY_LEGACY_RETRIES;

    rs_fill_rates_for_column(mvm, lq_sta, &rate, (__le32 *)lq_cmd->rs_table, &index,
                 num_rates, num_retries, valid_tx_ant,
                 toggle_ant);

    /* update the color of the LQ command (as a counter at bits 1-3) */
    color = LQ_FLAGS_COLOR_INC(LQ_FLAG_COLOR_GET(lq_cmd->flags));
    lq_cmd->flags = LQ_FLAG_COLOR_SET(lq_cmd->flags, color);
}

#if 0
struct rs_bfer_active_iter_data {
    struct ieee80211_node *exclude_sta;
    struct ieee80211_node *bfer_mvmsta;
};

static void rs_bfer_active_iter(void *_data,
                struct ieee80211_node *sta)
{
    struct rs_bfer_active_iter_data *data = (struct rs_bfer_active_iter_data *)_data;
    struct iwm_lq_cmd *lq_cmd = &sta->ni_ic->lq_sta.rs_drv.lq;
    u32 ss_params = le32_to_cpu(lq_cmd->ss_params);

    if (sta == data->exclude_sta)
        return;

    /* The current sta has BFER allowed */
    if (ss_params & LQ_SS_BFER_ALLOWED) {
        WARN_ON_ONCE(data->bfer_mvmsta != NULL);

        data->bfer_mvmsta = mvmsta;
    }
}

static int rs_bfer_priority(struct ieee80211_node *sta)
{
    return 1;
}

/* Returns >0 if sta1 has a higher BFER priority compared to sta2 */
static int rs_bfer_priority_cmp(struct ieee80211_node *sta1,
                struct ieee80211_node *sta2)
{
    int prio1 = rs_bfer_priority(sta1);
    int prio2 = rs_bfer_priority(sta2);

    if (prio1 > prio2)
        return 1;
    if (prio1 < prio2)
        return -1;
    return 0;
}

static inline void ieee80211_iterate_stations_atomic(struct iwm_softc *hw,
            void (*iterator)(void *data,
                     struct ieee80211_node *sta),
            void *data)
{
    iterator(data, hw->sc_ic.ic_bss);
}

#endif

static void rs_set_lq_ss_params(struct iwm_softc *mvm,
                struct ieee80211_node *sta,
                struct iwl_lq_sta *lq_sta,
                const struct rs_rate *initial_rate)
{
    struct iwm_lq_cmd *lq_cmd = &lq_sta->lq;
#if 0
    struct rs_bfer_active_iter_data data = {
        .exclude_sta = sta,
        .bfer_mvmsta = NULL,
    };
#endif
    u32 ss_params = LQ_SS_PARAMS_VALID;

    if (!iwl_mvm_bt_coex_is_mimo_allowed(mvm, sta))
        goto out;

    if (lq_sta->stbc_capable)
        ss_params |= LQ_SS_STBC_1SS_ALLOWED;

#if 0
    if (!lq_sta->bfer_capable)
        goto out;

    ieee80211_iterate_stations_atomic(mvm,
                      rs_bfer_active_iter,
                      &data);
    bfer_mvmsta = data.bfer_mvmsta;

    /* This code is safe as it doesn't run concurrently for different
     * stations. This is guaranteed by the fact that calls to
     * ieee80211_tx_status wouldn't run concurrently for a single HW.
     */
    if (!bfer_mvmsta) {
        IWL_DEBUG_RATE(mvm, "No sta with BFER allowed found. Allow\n");

        ss_params |= LQ_SS_BFER_ALLOWED;
        goto out;
    }

    IWL_DEBUG_RATE(mvm, "Found existing sta %d with BFER activated\n",
               bfer_mvmsta->sta_id);

    /* Disallow BFER on another STA if active and we're a higher priority */
    if (rs_bfer_priority_cmp(mvmsta, bfer_mvmsta) > 0) {
        struct iwm_lq_cmd *bfersta_lq_cmd =
            &bfer_mvmsta->lq_sta.rs_drv.lq;
        u32 bfersta_ss_params = le32_to_cpu(bfersta_lq_cmd->ss_params);

        bfersta_ss_params &= ~LQ_SS_BFER_ALLOWED;
        bfersta_lq_cmd->ss_params = cpu_to_le32(bfersta_ss_params);
        iwl_mvm_send_lq_cmd(mvm, bfersta_lq_cmd);

        ss_params |= LQ_SS_BFER_ALLOWED;
        IWL_DEBUG_RATE(mvm,
                   "Lower priority BFER sta found (%d). Switch BFER\n",
                   bfer_mvmsta->sta_id);
    }
#else
    if (lq_sta->bfer_capable)
        ss_params |= LQ_SS_BFER_ALLOWED;
#endif
out:
    lq_cmd->ss_params = cpu_to_le32(ss_params);
}

static void rs_fill_lq_cmd(struct iwm_softc *mvm,
               struct ieee80211_node *sta,
               struct iwl_lq_sta *lq_sta,
               const struct rs_rate *initial_rate)
{
    struct iwm_lq_cmd *lq_cmd = &lq_sta->lq;
    ItlIwm *that = container_of(mvm, ItlIwm, com);

    lq_cmd->agg_disable_start_th = IWL_MVM_RS_AGG_DISABLE_START;
    lq_cmd->agg_time_limit =
        cpu_to_le16(IWL_MVM_RS_AGG_TIME_LIMIT);

    if (WARN_ON_ONCE(!sta || !initial_rate))
        return;

    rs_build_rates_table(mvm, sta, lq_sta, initial_rate);

    if (isset(&mvm->sc_ucode_api, IWM_UCODE_TLV_API_LQ_SS_PARAMS))
        rs_set_lq_ss_params(mvm, sta, lq_sta, initial_rate);

    if (!isset(&mvm->sc_enabled_capa, IWM_UCODE_TLV_CAPA_COEX_SCHEMA_2) &&
        that->iwm_num_of_ant(initial_rate->ant) == 1)
        lq_cmd->single_stream_ant_msk = initial_rate->ant;

    lq_cmd->agg_frame_cnt_limit = LINK_QUAL_AGG_FRAME_LIMIT_DEF;

#if 0
    if (mvmsta->vif->p2p)
        lq_cmd->flags |= LQ_FLAG_USE_RTS_MSK;
#endif

    lq_cmd->agg_time_limit =
            cpu_to_le16(that->iwm_coex_agg_time_limit(mvm, sta));
}

int rs_pretty_print_rate(char *buf, int bufsz, const u32 rate)
{

    char *type, *bw;
    u8 mcs = 0, nss = 0;
    u8 ant = (rate & RATE_MCS_ANT_ABC_MSK) >> RATE_MCS_ANT_POS;

    if (!(rate & RATE_MCS_HT_MSK) &&
        !(rate & RATE_MCS_VHT_MSK) &&
        !(rate & RATE_MCS_HE_MSK)) {
        int index = iwl_hwrate_to_plcp_idx(rate);

        return snprintf(buf, bufsz, "Legacy | ANT: %s Rate: %s Mbps",
                 rs_pretty_ant(ant),
                 index == IWL_RATE_INVALID ? "BAD" :
                 iwl_rate_mcs[index].mbps);
    }

    if (rate & RATE_MCS_VHT_MSK) {
        type = "VHT";
        mcs = rate & RATE_VHT_MCS_RATE_CODE_MSK;
        nss = ((rate & RATE_VHT_MCS_NSS_MSK)
               >> RATE_VHT_MCS_NSS_POS) + 1;
    } else if (rate & RATE_MCS_HT_MSK) {
        type = "HT";
        mcs = rate & RATE_HT_MCS_INDEX_MSK;
        nss = ((rate & RATE_HT_MCS_NSS_MSK)
               >> RATE_HT_MCS_NSS_POS) + 1;
    } else if (rate & RATE_MCS_HE_MSK) {
        type = "HE";
        mcs = rate & RATE_VHT_MCS_RATE_CODE_MSK;
        nss = ((rate & RATE_VHT_MCS_NSS_MSK)
               >> RATE_VHT_MCS_NSS_POS) + 1;
    } else {
        type = "Unknown"; /* shouldn't happen */
    }

    switch (rate & RATE_MCS_CHAN_WIDTH_MSK) {
    case RATE_MCS_CHAN_WIDTH_20:
        bw = "20Mhz";
        break;
    case RATE_MCS_CHAN_WIDTH_40:
        bw = "40Mhz";
        break;
    case RATE_MCS_CHAN_WIDTH_80:
        bw = "80Mhz";
        break;
    case RATE_MCS_CHAN_WIDTH_160:
        bw = "160Mhz";
        break;
    default:
        bw = "BAD BW";
    }

    return snprintf(buf, bufsz,
             "0x%x: %s | ANT: %s BW: %s MCS: %d NSS: %d %s%s%s%s%s",
             rate, type, rs_pretty_ant(ant), bw, mcs, nss,
             (rate & RATE_MCS_SGI_MSK) ? "SGI " : "NGI ",
             (rate & RATE_MCS_STBC_MSK) ? "STBC " : "",
             (rate & RATE_MCS_LDPC_MSK) ? "LDPC " : "",
             (rate & RATE_HE_DUAL_CARRIER_MODE_MSK) ? "DCM " : "",
             (rate & RATE_MCS_BF_MSK) ? "BF " : "");
}

void iwl_mvm_rs_rate_init(struct iwm_softc *mvm, struct ieee80211_node *sta,
              enum nl80211_band band, bool update)
{
    IOSimpleLockLock(mvm->lq_sta.rs_drv.pers.lock);
    rs_drv_rate_init(mvm, sta, band);
    IOSimpleLockUnlock(mvm->lq_sta.rs_drv.pers.lock);
}

static int rs_drv_tx_protection(struct iwm_softc *mvm,
                bool enable)
{
    struct iwm_lq_cmd *lq = &mvm->lq_sta.rs_drv.lq;

    if (enable) {
        if (mvm->tx_protection == 0)
            lq->flags |= LQ_FLAG_USE_RTS_MSK;
        mvm->tx_protection++;
    } else {
        mvm->tx_protection--;
        if (mvm->tx_protection == 0)
            lq->flags &= ~LQ_FLAG_USE_RTS_MSK;
    }

    return iwl_mvm_send_lq_cmd(mvm, lq);
}

/**
 * iwl_mvm_tx_protection - ask FW to enable RTS/CTS protection
 * @mvm: The mvm component
 * @mvmsta: The station
 * @enable: Enable Tx protection?
 */
int iwl_mvm_tx_protection(struct iwm_softc *mvm,
              bool enable)
{
    return rs_drv_tx_protection(mvm, enable);
}

void
iwm_rs_alloc(struct iwm_softc *sc)
{
    sc->lq_sta.rs_drv.pers.lock = IOSimpleLockAlloc();
}

void iwm_rs_free(struct iwm_softc *sc)
{
    if (sc->lq_sta.rs_drv.pers.lock) {
        IOSimpleLockFree(sc->lq_sta.rs_drv.pers.lock);
        sc->lq_sta.rs_drv.pers.lock = NULL;
    }
}
