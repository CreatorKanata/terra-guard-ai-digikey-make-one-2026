/*
 * TerraGuard AI — FRDM-MCXN947
 * bg_subtract: 背景差分（背景モデル + 前景抽出 + 候補判定）の実装
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "bg_subtract.h"
#include "sensor_bus.h"
#include "fsl_debug_console.h"

/*******************************************************************************
 * パラメータ（#define で集約。実機で調整しビルド・書き込みで反映）
 ******************************************************************************/
/* 初期背景の蓄積フレーム数（約7.x fps で 40枚 ≒ 5〜6秒）。 */
#define BG_THERMAL_INIT_FRAMES 40
#define BG_DIST_INIT_FRAMES    40 /* 距離の初期化は中央値。リングに保持する枚数。 */

/* 背景の EMA 更新係数（候補なしのときだけ更新）。
   サーマルは日射・気温変化があるので少し速め、距離は固定物中心なので遅め。 */
#define BG_THERMAL_ALPHA 0.01f
#define BG_DIST_ALPHA    0.005f

/* 前景とみなす差分の下限（これ未満は前景マップで0扱い）。
   ⚠️ 1.0℃ まで下げると完璧な市松模様が出る（実機確認 2026-06-21）。
   原因: MLX90640 はサブページ0/1で系統的なオフセット差が約1〜1.6℃あり、
   閾値が低いと片サブページ(市松の片半分)だけが一様に閾値を超えて前景化し、
   もう片方は超えず0 → チェッカーボードになる。生サーマルは市松でないのに
   前景だけ市松になるのが特徴。2.0℃ ならサブページ差を超えるので市松は出ない。
   羽毛表面 30〜35℃（深部体温ではない。docs/sensor-processing.md）のカラスは
   近接・大きく写れば背景と2℃以上の差が出る前提。 */
#define BG_THERMAL_FG_MIN_C  2.0f /* サーマル前景: +2.0℃以上を前景画素とみなす */

/* 距離前景のヒステリシス（境界でのちらつき防止）。
   ゾーンは ON 閾値で前景になり、OFF 閾値を下回るまで前景のまま。
   σ揺れで ON 閾値を跨いでも、間(OFF〜ON)では状態を維持するのでチラつかない。

   ⚠️ 旧 ON=300mm では「地面に立つ陸上カラス」が検出できなかった（実機検証
   2026-06-22, dataset/validation/）。陸上カラスは背景=地面との距離差が体高ぶん
   15〜20cm 程度しか出ず、最大でも 80mm 程度。一方、地面ノイズ床は σ p90 41mm /
   変動幅 max 126mm。単一ゾーンの絶対値では両者が重なり分離不能。
   → ON を 30mm まで下げて弱い反応も拾い、誤検出は下の【複合判定】
   （連結塊サイズ＋前景総和）で抑える。tools/ml/test_foreground_detect.py で
   保存データを用い あり/なし の完全分離を確認済み。 */
#define BG_DIST_FG_ON_MM   30  /* 点灯閾値 = 30mm（地面ノイズσ p90 41mm の少し下〜同等） */
#define BG_DIST_FG_OFF_MM  15  /* 消灯閾値 = 15mm（< ON。ヒステリシス幅は狭めでよい） */

/* 候補判定の面積閾値。 */
#define BG_THERMAL_AREA_MIN 4 /* サーマル前景画素が4以上 */

/* --- 距離前景の【複合判定】パラメータ（実機検証 2026-06-22 / 保存データで確定）---
   単一ゾーンの絶対値ではなく「まとまり（連結塊）」と「総和」で陸上カラスを
   地面の散発ノイズから分離する。カラスは隣接ゾーンが連結して反応し前景総和が
   大きい。地面ノイズは単発・散発で連結も総和も伸びない。
     - 連結塊: ≥ CLUSTER_MIN ゾーンが 4近傍で連結し、その塊内 fg 合計 ≥ CLUSTER_SUM_MM
     - または: 前景 上位3ゾーン和 ≥ TOP3_SUM_MM
   いずれかを満たせば「距離側で陸上カラス候補あり」とする。 */
#define BG_DIST_CLUSTER_MIN     2   /* 連結塊の最小ゾーン数[px] */
#define BG_DIST_CLUSTER_SUM_MM  100 /* 連結塊内の前景合計[mm] */
#define BG_DIST_TOP3_SUM_MM     120 /* 前景 上位3ゾーン和[mm] */

#define BG_DIST_INVALID (-1) /* 距離フレームの無効ゾーン値（tof側と一致） */

/*******************************************************************************
 * 状態
 ******************************************************************************/
/* --- サーマル --- */
static float s_thermalBg[BG_THERMAL_PIXELS];   /* 背景モデル[℃] */
static float s_thermalFg[BG_THERMAL_PIXELS];   /* 前景マップ[℃]（正のみ、未満は0） */
static float s_thermalCur[BG_THERMAL_PIXELS];  /* 直近の生フレーム[℃]（EMA更新用に保持） */
static bool  s_thermalHasCur;                  /* 直近生フレームを保持済みか */
static int   s_thermalInitCnt;                 /* 初期蓄積で投入済みのフレーム数 */
static bool  s_thermalReady;                   /* 背景確立済みか */
static float s_thermalFgMax;                   /* 直近前景の最大差分 */
static int   s_thermalFgArea;                  /* 直近前景の有効画素数 */

/* --- 距離 --- */
static float   s_distBg[BG_DIST_ZONES];        /* 背景モデル[mm] */
static int16_t s_distFg[BG_DIST_ZONES];        /* 前景マップ[mm]（正のみ、未満/無効は0） */
static bool    s_distFgOn[BG_DIST_ZONES];      /* ゾーンごとの前景ラッチ（ヒステリシス用） */
static int16_t s_distCur[BG_DIST_ZONES];       /* 直近の生フレーム[mm]（EMA更新用、無効は-1） */
static bool    s_distHasCur;                   /* 直近生フレームを保持済みか */
static bool    s_distReady;                    /* 背景確立済みか */
static int     s_distFgMax;                    /* 直近前景の最大量 */
static int     s_distFgArea;                   /* 直近前景の有効ゾーン数 */
static int     s_distClusterSize;              /* 直近前景の最大連結塊サイズ[px] */
static int     s_distClusterSum;               /* その塊内の前景合計[mm] */
static int     s_distTop3Sum;                  /* 前景 上位3ゾーン和[mm] */

/* 距離の初期化中央値用リングバッファ。ゾーンごとに最大 BG_DIST_INIT_FRAMES 枚保持。
   無効(-1)は積まない。ゾーンごとの蓄積枚数を s_distInitCnt[zone] で管理。 */
static int16_t s_distInitBuf[BG_DIST_ZONES][BG_DIST_INIT_FRAMES];
static uint8_t s_distInitCnt[BG_DIST_ZONES];
static int     s_distInitFrames;               /* 投入済み距離フレーム数（初期化進捗） */

static bool s_candidatePresent;                /* 直近の鳥候補判定 */

/*******************************************************************************
 * Code
 ******************************************************************************/
void bg_reset(void)
{
    for (int i = 0; i < BG_THERMAL_PIXELS; i++)
    {
        s_thermalBg[i] = 0.0f;
        s_thermalFg[i] = 0.0f;
    }
    s_thermalInitCnt = 0;
    s_thermalReady   = false;
    s_thermalHasCur  = false;
    s_thermalFgMax   = 0.0f;
    s_thermalFgArea  = 0;

    for (int z = 0; z < BG_DIST_ZONES; z++)
    {
        s_distBg[z]      = 0.0f;
        s_distFg[z]      = 0;
        s_distFgOn[z]    = false;
        s_distInitCnt[z] = 0;
    }
    s_distReady       = false;
    s_distHasCur      = false;
    s_distFgMax       = 0;
    s_distFgArea      = 0;
    s_distClusterSize = 0;
    s_distClusterSum  = 0;
    s_distTop3Sum     = 0;
    s_distInitFrames  = 0;

    s_candidatePresent = false;
}

/* int16 配列を昇順に並べたときの中央値を返す（コピーして挿入ソート。N<=40 で十分軽い）。 */
static int16_t median_i16(const int16_t *src, int n)
{
    int16_t tmp[BG_DIST_INIT_FRAMES];
    for (int i = 0; i < n; i++) tmp[i] = src[i];
    for (int i = 1; i < n; i++)
    {
        int16_t key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    return tmp[n / 2];
}

/* ---------------------------------------------------------------- サーマル */
void bg_thermal_update(const float *to768)
{
    /* EMA更新で使うため直近の生フレームを保持。 */
    for (int i = 0; i < BG_THERMAL_PIXELS; i++) s_thermalCur[i] = to768[i];
    s_thermalHasCur = true;

    /* --- 初期背景の蓄積（最初の N フレームを平均） --- */
    if (!s_thermalReady)
    {
        for (int i = 0; i < BG_THERMAL_PIXELS; i++)
        {
            s_thermalBg[i] += to768[i];
        }
        s_thermalInitCnt++;
        if (s_thermalInitCnt >= BG_THERMAL_INIT_FRAMES)
        {
            for (int i = 0; i < BG_THERMAL_PIXELS; i++)
            {
                s_thermalBg[i] /= (float)s_thermalInitCnt;
            }
            s_thermalReady = true;
        }
        /* 初期化中は前景なし。 */
        s_thermalFgMax  = 0.0f;
        s_thermalFgArea = 0;
        return;
    }

    /* --- 前景抽出: 背景より暖かい正の差分のみ --- */
    float maxFg = 0.0f;
    int   area  = 0;
    for (int i = 0; i < BG_THERMAL_PIXELS; i++)
    {
        float fg = to768[i] - s_thermalBg[i];
        if (fg < 0.0f) fg = 0.0f;
        s_thermalFg[i] = fg;
        if (fg > maxFg) maxFg = fg;
        if (fg >= BG_THERMAL_FG_MIN_C) area++;
    }
    s_thermalFgMax  = maxFg;
    s_thermalFgArea = area;
    /* 背景の EMA 更新は両センサの候補状態を見て bg_apply_update_policy() で行う。 */
}

const float *bg_thermal_fg(void) { return s_thermalFg; }
bool  bg_thermal_ready(void)     { return s_thermalReady; }
float bg_thermal_fg_max(void)    { return s_thermalFgMax; }
int   bg_thermal_fg_area(void)   { return s_thermalFgArea; }

/* ------------------------------------------------------------------ 距離 */
void bg_dist_update(const int16_t *dist64)
{
    /* EMA更新で使うため直近の生フレームを保持。 */
    for (int z = 0; z < BG_DIST_ZONES; z++) s_distCur[z] = dist64[z];
    s_distHasCur = true;

    /* --- 初期背景の蓄積（ゾーンごとに中央値）。無効(-1)は積まない --- */
    if (!s_distReady)
    {
        for (int z = 0; z < BG_DIST_ZONES; z++)
        {
            int16_t d = dist64[z];
            if (d != BG_DIST_INVALID && s_distInitCnt[z] < BG_DIST_INIT_FRAMES)
            {
                s_distInitBuf[z][s_distInitCnt[z]++] = d;
            }
        }
        s_distInitFrames++;
        if (s_distInitFrames >= BG_DIST_INIT_FRAMES)
        {
            for (int z = 0; z < BG_DIST_ZONES; z++)
            {
                /* 有効サンプルが1枚も無いゾーンは背景未確立 → 大きい値で初期化し前景を出さない。 */
                s_distBg[z] = (s_distInitCnt[z] > 0)
                                  ? (float)median_i16(s_distInitBuf[z], s_distInitCnt[z])
                                  : 0.0f;
            }
            s_distReady = true;
        }
        s_distFgMax  = 0;
        s_distFgArea = 0;
        return;
    }

    /* --- 前景抽出: 背景より手前に出た正の量のみ。無効ゾーンは前景0 --- */
    int maxFg = 0;
    int area  = 0;
    for (int z = 0; z < BG_DIST_ZONES; z++)
    {
        int16_t d = dist64[z];
        int raw = 0; /* 背景より手前に出た生の量[mm] */
        if (d != BG_DIST_INVALID && s_distBg[z] > 0.0f)
        {
            raw = (int)(s_distBg[z] - (float)d);
            if (raw < 0) raw = 0;
        }
        else
        {
            /* 無効ゾーンは前景ラッチを解除（古い点灯を引きずらない）。 */
            s_distFgOn[z] = false;
        }

        /* ヒステリシス: ON閾値で点灯、OFF閾値を下回るまで点灯維持。
           境界(OFF〜ON)では状態維持なので σ揺れでチラつかない。 */
        if (s_distFgOn[z])
        {
            if (raw < BG_DIST_FG_OFF_MM) s_distFgOn[z] = false;
        }
        else
        {
            if (raw >= BG_DIST_FG_ON_MM) s_distFgOn[z] = true;
        }

        int fg = s_distFgOn[z] ? raw : 0;
        s_distFg[z] = (int16_t)fg;
        if (fg > maxFg) maxFg = fg;
        if (s_distFgOn[z]) area++;
    }
    s_distFgMax  = maxFg;
    s_distFgArea = area;

    /* --- 複合特徴量: 最大連結塊（4近傍）と前景 上位3和を算出 ---
       単一ゾーン値ではなく「まとまり」を見ることで、地面の散発ノイズと
       連結して反応する陸上カラスを分離する。前景マップ s_distFg を 8×8
       グリッドとみなし、点灯ゾーン(fg>0)の連結成分を反復DFSで走査する。 */
    bool visited[BG_DIST_ZONES] = {false};
    int  stack[BG_DIST_ZONES];
    int  bestSize = 0, bestSum = 0;
    for (int start = 0; start < BG_DIST_ZONES; start++)
    {
        if (visited[start] || s_distFg[start] <= 0) continue;
        int sp = 0;
        stack[sp++] = start;
        visited[start] = true;
        int compSize = 0, compSum = 0;
        while (sp > 0)
        {
            int z = stack[--sp];
            compSize++;
            compSum += s_distFg[z];
            int zr = z / 8, zc = z % 8;
            static const int dr[4] = {1, -1, 0, 0};
            static const int dc[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++)
            {
                int nr = zr + dr[k], nc = zc + dc[k];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) continue;
                int nz = nr * 8 + nc;
                if (!visited[nz] && s_distFg[nz] > 0)
                {
                    visited[nz] = true;
                    stack[sp++] = nz;
                }
            }
        }
        if (compSize > bestSize) { bestSize = compSize; bestSum = compSum; }
    }
    s_distClusterSize = bestSize;
    s_distClusterSum  = bestSum;

    /* 上位3ゾーン和（部分選択ソート相当。64要素なので軽い）。 */
    int t1 = 0, t2 = 0, t3 = 0;
    for (int z = 0; z < BG_DIST_ZONES; z++)
    {
        int v = s_distFg[z];
        if (v > t1)      { t3 = t2; t2 = t1; t1 = v; }
        else if (v > t2) { t3 = t2; t2 = v; }
        else if (v > t3) { t3 = v; }
    }
    s_distTop3Sum = t1 + t2 + t3;
}

const int16_t *bg_dist_fg(void) { return s_distFg; }
bool bg_dist_ready(void)        { return s_distReady; }
int  bg_dist_fg_max(void)       { return s_distFgMax; }
int  bg_dist_fg_area(void)      { return s_distFgArea; }

/* -------------------------------------------------- 候補判定・背景更新 */
bool bg_candidate_present(void) { return s_candidatePresent; }

void bg_apply_update_policy(void)
{
    /* 候補判定: サーマル前景 AND 距離前景。両系統で前景が出たときだけ候補ありとする。
       熱だけだと日なたの壁等で止まりすぎ、距離だけだと人や物の通過で止まりすぎるため、
       両方の AND で「鳥候補」を絞る。 */
    bool hasThermalFg = s_thermalReady &&
                        (s_thermalFgMax >= BG_THERMAL_FG_MIN_C) &&
                        (s_thermalFgArea >= BG_THERMAL_AREA_MIN);

    /* 距離側は【複合判定】。陸上カラスは単一ゾーンの絶対値では地面ノイズに
       埋もれるため、「連結塊（≥CLUSTER_MIN px かつ 塊和≥CLUSTER_SUM_MM）」
       または「前景 上位3和≥TOP3_SUM_MM」で『まとまった反応』を検出する。
       地面ノイズは単発・散発なので連結も総和も伸びず弾かれる。
       （実機検証 2026-06-22 / tools/ml/test_foreground_detect.py で あり/なし 分離確認） */
    bool distCluster = (s_distClusterSize >= BG_DIST_CLUSTER_MIN) &&
                       (s_distClusterSum  >= BG_DIST_CLUSTER_SUM_MM);
    bool distTop3    = (s_distTop3Sum >= BG_DIST_TOP3_SUM_MM);
    bool hasDistFg   = s_distReady && (distCluster || distTop3);

    /* 鳥候補は依然「サーマル AND 距離」で絞る（誤検出を抑える）。 */
    s_candidatePresent = hasThermalFg && hasDistFg;

    /* 背景の凍結は【センサごとに独立】で判定する。
       理由: 旧実装は thermal AND dist の候補成立時のみ凍結していたため、
       片方（サーマル）の前景がもう一方の条件不成立で候補にならないと、
       前景が出ているセンサの背景まで EMA 更新され続け、途中で現れた対象が
       数秒で背景に吸収されて前景が消える（=「途中登場のカラスがサーマル
       foreground に出ない」バグの主因）。
       → 各センサは「自分の前景が立っている間は自分の背景を凍結」する。 */

    /* サーマル: 前景が立っていなければ EMA 更新、立っていれば凍結。 */
    if (s_thermalReady && s_thermalHasCur && !hasThermalFg)
    {
        for (int i = 0; i < BG_THERMAL_PIXELS; i++)
        {
            s_thermalBg[i] = (1.0f - BG_THERMAL_ALPHA) * s_thermalBg[i]
                             + BG_THERMAL_ALPHA * s_thermalCur[i];
        }
    }

    /* 距離: 前景が立っていなければ EMA 更新、立っていれば凍結。 */
    if (s_distReady && s_distHasCur && !hasDistFg)
    {
        for (int z = 0; z < BG_DIST_ZONES; z++)
        {
            int16_t d = s_distCur[z];
            /* 無効ゾーン、または背景未確立ゾーンは更新しない。 */
            if (d != BG_DIST_INVALID && s_distBg[z] > 0.0f)
            {
                s_distBg[z] = (1.0f - BG_DIST_ALPHA) * s_distBg[z]
                              + BG_DIST_ALPHA * (float)d;
            }
        }
    }
}

/* ------------------------------------------------------------ シリアル出力 */
void bg_dist_print_frame(void)
{
    /* 距離前景マップ（背景より手前に出た量[mm]、0=前景なし）。 */
    PRINTF("DFG");
    for (int z = 0; z < BG_DIST_ZONES; z++)
    {
        PRINTF(",%d", (int)s_distFg[z]);
    }
    PRINTF("\r\n");

    /* 候補判定の要約。t_max は ℃×100 整数（負値化け回避のため非負前提）。
       後方互換: 既存5フィールド(cand,t_max,t_area,d_max,d_area)の後ろに、
       距離複合判定の特徴量(d_cluster_size, d_cluster_sum, d_top3_sum)を追加する。
       旧ビューアは先頭5つだけ読めばよい。 */
    int tMaxCenti = (int)(s_thermalFgMax * 100.0f);
    if (tMaxCenti < 0) tMaxCenti = 0;
    PRINTF("DET,%d,%d,%d,%d,%d,%d,%d,%d\r\n",
           s_candidatePresent ? 1 : 0,
           tMaxCenti, s_thermalFgArea,
           s_distFgMax, s_distFgArea,
           s_distClusterSize, s_distClusterSum, s_distTop3Sum);
}

bool bg_thermal_send_fg_bin(void)
{
    if (!s_thermalReady)
    {
        return false;
    }
    /* magic(2) + 576画素(1152) = 1154バイト。リトルエンディアンで詰める（24×24）。 */
    static uint8_t buf[2 + BG_THERMAL_PIXELS * 2];
    size_t pos = 0;

    buf[pos++] = 0xAAU;
    buf[pos++] = 0x56U; /* 生サーマル(0xAA55)と区別する前景フレーム用 magic */
    for (int i = 0; i < BG_THERMAL_PIXELS; i++)
    {
        int c = (int)(s_thermalFg[i] * 100.0f); /* 前景は非負。℃×100 */
        if (c < 0) c = 0;
        if (c > 32767) c = 32767;
        buf[pos++] = (uint8_t)(c & 0xFF);
        buf[pos++] = (uint8_t)((c >> 8) & 0xFF);
    }
    sensor_write_raw(buf, pos);
    return true;
}
