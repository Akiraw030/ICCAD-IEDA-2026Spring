# Strategy8 Score Optimization Campaign（2026-08-28）

## 目標與實驗規則

終極目標是提高題目 weighted score。所有實驗遵守：

1. 以 2026-08-28 的本機 baseline run 作為起始 baseline；raw report 未納入公開 repository。
2. 快速篩選優先跑 `testcase0 testcase2 testcase1_v2`；通過後再跑
   `testcase4 testcase0_v2`，最終候選才跑完整 suite 與 `testcase530`。
3. 每個實驗保留 timing report 資料夾、精確參數／演算法差異、結果與採用判斷。
4. 同時計算目前 Pipeline 的 `0.40/0.30/0.30` score，以及依 Beta 排名推估的
   timing-heavy `0.64/0.18/0.18` score，避免對單一未公開係數過度擬合。
5. 未通過實驗仍保留記錄，不直接覆蓋或隱藏結果。

可用以下方式執行子集合，未設定變數時 `run_reports.sh` 仍跑全部 8 個 case：

```bash
CADD0045_TESTCASES="testcase0 testcase2 testcase1_v2" \
./run_reports.sh /path/to/testcase_root report/optimization_campaign_20260828/run_TIMESTAMP_TAG
```

## Baseline：run_20260828_022408

| Case | SS TNS / WNS / NVP | FF TNS / WNS / NVP | Area | Score 0.40/0.30/0.30 | 推估 Score 0.64/0.18/0.18 |
|---|---|---|---:|---:|---:|
| testcase0 | -0.0050 / -0.0028 / 5 | 0 / 0 / 0 | 115.779 | 0.967452 | 1.376264 |
| testcase1 | -0.0759 / -0.0037 / 41 | 0 / 0 / 0 | 174.646 | 0.970136 | 1.376083 |
| testcase2 | -0.4638 / -0.0064 / 179 | 0 / 0 / 0 | 394.158 | 1.569034 | 1.731064 |
| testcase3 | -1.1750 / -0.0055 / 451 | 0 / 0 / 0 | 1228.096 | 1.561626 | 1.730931 |
| testcase4 | -9.9047 / -0.0083 / 2658 | 0 / 0 / 0 | 4251.989 | 1.571639 | 1.735653 |
| testcase0_v2 | -10.6638 / -0.0114 / 2110 | 0 / 0 / 0 | 2786.223 | 1.568152 | 1.728503 |
| testcase1_v2 | -1.3023 / -0.0075 / 365 | 0 / 0 / 0 | 667.761 | 1.569300 | 1.732060 |
| testcase530 | -35.9712 / -0.0105 / 16304 | -0.0098 / -0.0007 / 14 | 115015.557 | 1.442887 | 1.657152 |

### 與 Beta Top 組的主要差距

- Top 組在多數公開與隱藏 case 能把 SS/FF violation 完全關閉；baseline 仍留下
  5 到 16,318 個 violations。
- testcase530 的 post-Phase0 Existing shared 搜尋花約 40 秒但接受 0 個 insertion；
  交替前必須加入無效搜尋停用機制。
- Strategy8 Perturb 在多個 case 會把 FinalAlt 已很小的 timing violation重新放大；
  需要測試 checkpoint／停用／closure-aware acceptance。

## 實驗索引

| ID | 想法 | 狀態 | Historical run label | 判斷 |
|---|---|---|---|---|
| E01 | 目標權重改為 0.64/0.18/0.18 | 已淘汰 | `E01_weight64_screen` | timing 改善但 area 成本過高，連推估權重分數也下降 |
| E02 | Existing shared 連續無 insertion 後停用 | 部分採用 | `E02_shared_streak_screen` | 保留 streak guard；「初次 0 就停用」因 testcase4 反例撤銷 |
| E03 | Strategy8 以 0.64/0.18/0.18 選擇最終 checkpoint | 已採用 | `run_20260828_E03_target_checkpoint_screen`, `run_20260828_E03_target_checkpoint_medium` | 保留 0.40/0.30/0.30 探索，分離最終狀態選擇 |
| E04 | 撤銷 Existing-shared 初次空結果立即停用 | 已採用 | `run_20260828_E04_delayed_shared_retry_testcase4` | testcase4 推估 score 恢復並超越 baseline |
| E05 | Strategy8 target checkpoint 無改善早停 | 已採用 | `run_20260828_E05_target_streak_screen` | 品質完全不變，中型案例總 runtime 減少 31–38% |
| E06 | FinalAlt 補入最接近 required delay 的 overshoot types | 部分採用 | `run_20260828_E06_delay_overshoot_screen` | testcase0/2 closure 顯著改善；需 E07 checkpoint 防止 reclaim 覆蓋佳解 |
| E07 | FinalAlt 以 0.64/0.18/0.18 保存 repair/reclaim 中最佳 checkpoint | 已採用 | `run_20260828_E07_finalalt_target_checkpoint_screen` | checkpoint 正確；但 E06 過早改變路徑，需 E08 修正候選順序 |
| E08 | Overshoot 只在舊候選不足時補齊 | 已採用 | `run_20260828_E08_overshoot_fallback_screen`, `..._medium` | closure 大幅改善，中型案例總 runtime 再減約 38–44% |
| E09 | Strategy8 outer acceptance 改用 target score | 已採用 | `run_20260828_E09_target_outer_acceptance_screen`, `..._remaining_public`, `..._large530` | 一般案例持平或略升；530 target score +0.012795 |
| E10 | 大型 case 略過無效 Strategy8，時間交回 FinalAlt | 已採用 | `run_20260828_E10_large_finalalt_budget` | 530 violations 11,736 -> 3,241，target score 1.669947 -> 1.675925 |
| E11 | 大型 FinalAlt reclaim top-K 50,000 -> 10,000 | 已淘汰 | `run_20260828_E11_large_reclaim_top10k` | top-K 被 delete 佔滿，target score 降至約 1.663 |
| E12 | 大型 reclaim 依 move kind 分層保留 5k，總上限 15k | 已淘汰 | `run_20260828_E12_large_stratified_reclaim` | violations 略少但 Area 過高，target score 1.672402 < E10 |
| E13 | 大型 finalization reserve 60 s -> 35 s | 已淘汰 | `run_20260828_E13_large_reserve35` | 多跑一輪但最佳 checkpoint 與 E10 相同；runtime 529.0 -> 554.4 s |
| E14 | 大型 case 略過零產出的 ExistingBufferSharedReclaim | 已採用 | `run_20260828_E14_skip_large_shared` | 530 target score 1.675925 -> 1.701764、Area -15.72%，runtime 535.8 s |
| E15 | Deadline-aware post-Phase0 score/heuristic portfolio | 部分採用 | `run_20260828_E15_portfolio_smoke`, `..._public_screen`, `..._testcase4` | 只在 baseline SS NVP <=1000 啟用；tc0/tc1 target +0.002292/+0.000363，其餘選回原 branch |
| E16 | 中大型 public case 加入 0.52/0.24/0.24 midweight branch | 已淘汰 | `run_20260828_E16_midweight_screen` | tc2/tc1_v2 target 分別 -0.009471/-0.004790；timing gain 無法抵銷 Area 增加 |
| E17 | 0.35/0.25/0.40 area-oriented branch | 已淘汰 | `run_20260828_E17_areaweight_screen` | tc2/tc1_v2 target 分別 -0.003926/-0.003054；Area gain 不足或 timing giveback 過大 |
| E18 | Score branch 的 adaptive Repair Pulse delay scale x0.75 | 已淘汰 | `run_20260828_E18_delayscale075_screen` | tc2/tc1_v2 target -0.003201/-0.004796；小 buffer 導致更多結構與 Area |
| E19 | Score branch 的 adaptive Repair Pulse delay scale x1.25 | 已淘汰 | `run_20260828_E19_delayscale125_screen` | tc2/tc1_v2 target -0.006213/-0.002825；相反方向同樣失敗 |
| E20 | 修正並啟用 normalized indexed Phase0 pressure | 修復保留／加速淘汰 | `run_20260828_E20_pressure_verify_buffer_only`, `..._pressure_medium`, `..._pressure_large530` | candidate pressure 完全等價、QoR 相同，但 530 pressure 9.797 -> 11.640 s，預設 off |
| E21 | 略過 FinalAlt repair→reclaim 間冗餘 FastTiming full sync | 已採用 | `run_20260828_E21_skip_finalalt_resync_medium` | FastTiming commit 後 cache 已正確，省去一個 full sync |
| E22 | 略過 Repair Pulse→Reclaim 間冗餘 FastTiming full sync | 已採用 | `run_20260828_E22_skip_rr_resync_medium` | testcase0_v2 QoR 完全相同，52.0695 -> 46.7095 s（-10.3%） |
| E23/E24 | Strategy8 預設開／關 | 關閉 | `...E23_area60_s8_on_medium`, `...E24_area60_s8_off_medium` | testcase0_v2 最終 tree 完全相同；關閉後 70.6737 -> 42.8797 s（-39.3%） |
| E25/E26 | 完整 Pipeline score / heuristic 兩分支 | 部分採用 | `...E25_area60_medium_heuristic_fork20k`, `...E26_phase0_gate_portfolio_*` | testcase4 heuristic target 1.709411 > score 1.598905；不同 branch 可找到不同 minima |
| E27 | 先完成 full_score fallback，再按實際剩餘時間啟動後續 route | 已採用 | `run_20260828_E27_time_scheduler_smoke` | 不再依固定 path/NVP gate 決定是否嘗試分支；保留小/中/大 case output reserve |
| E28 | Phase0 後 48 個相鄰 rank 的 seeded resize | 淘汰 | `run_20260828_E28_seeded_resize_*` | testcase3 1.686312 < 1.732958；testcase0_v2 1.645505 < 1.661459 |
| E29 | Phase0 後 pressure-directed resize | 淘汰 | `run_20260828_E29_pressure_resize_testcase3` | testcase3 1.732200 < 1.732958 |
| E31 | 面積優先 cosine objective schedule | 淘汰，保留選項 | `run_20260828_E31_score_schedule_{off,cosine}_testcase3` | 0.10/0.10/0.80 hold 4 cycles，12-cycle cosine 至 0.60/0.20/0.20；最佳 target 1.732958 -> 1.723740（-0.53%） |
| E32 | 5%/10% unrestricted initial-buffer perturbation（Phase0 前／後） | testcase3 採用候選 | `run_20260828_E32_aggressive_initial_states_testcase3` | pre-Phase0 10% target 1.735434，較 fallback +0.002476；待 testcase0_v2 驗證 |
| E33 | scheduler 接在 Phase0 checkpoint 後的 heuristic route | 設計修正 | `run_20260828_E33_cosine_portfolio_testcase3` | testcase3 的 tree/QoR 與普通 heuristic 完全相同；scheduler 未能影響 heuristic 後段的 move acceptance |
| E34 | scheduler 從 original tree 重跑完整 Phase0 的 heuristic route | 部分採用 | `run_20260828_E34_cosine_full_pipeline_testcase3` | testcase3 correctly reproduces E31’s distinct result (target 1.723740)，selector safely chooses prephase 10% target 1.735434 |
| E35 | 提交版 deadline-filling multi-start（5%/10%/20% + rotating seeds） | 已採用 | 短版 testcase0 smoke（restart cap=2） | 9 routes 完整結束，2.66 s、20132 KB、1,730 行合法輸出；正式版還原為 deadline-only |
| E36 | O3 / native / LTO compiler A/B | 不採用 native/LTO | testcase2，固定 9 routes，各版 3 runs | 交錯測試 O3 17.68 s、LTO 17.83 s；native 無正收益；所有輸出 SHA-256 相同 |

## E01：全 Pipeline 改用 0.64/0.18/0.18

### 改動

只把 `OptimizerConfig::alpha/beta/gamma` 從 `0.40/0.30/0.30` 改為
`0.64/0.18/0.18`；所有 score-based stage 都使用新權重決策。測試後已還原。

### Screen 結果

| Case | Baseline 推估 0.64 score | E01 score | Baseline SS TNS / Area | E01 SS TNS / Area | 判斷 |
|---|---:|---:|---|---|---|
| testcase0 | 1.376264 | 1.367343 | -0.0050 / 115.779 | -0.0161 / 131.982 | timing、area、score 均退步 |
| testcase2 | 1.731064 | 1.725470 | -0.4638 / 394.158 | -0.1298 / 473.360 | timing 改善，但 area +20.1%，score 下降 |

E01 顯示不能讓 Phase0、shared repair 與 reclaim 全部直接改用 timing-heavy 權重；
新權重應只考慮用在後段 closure repair，而不是整條 Pipeline。

## E02：Existing-shared 無效掃描停用機制

### 改動

- 若 Phase0 後第一次 Existing-buffer shared cycle 的 insertion 為 0，
  不再於後續 Repair/Reclaim cycle 前重複掃描。
- 若曾成功插入，後續連續 2 個 Existing-shared cycle 都為 0 insertion，
  則只停用 Existing-shared 交替；普通 Repair/Reclaim 繼續執行。

### Screen 結果

| Case | Baseline score | E02 score | Baseline runtime | E02 runtime | 品質變化 |
|---|---:|---:|---:|---:|---|
| testcase0 | 0.967452 | 0.967452 | 1.082 s | 0.977 s | timing/area 完全相同 |
| testcase2 | 1.569034 | 1.569034 | 5.168 s | 4.948 s | timing/area 完全相同 |
| testcase1_v2 | 1.569300 | 1.569606 | 12.009 s | 12.617 s | SS TNS `-1.3023 -> -1.2679`，Area `667.761 -> 666.379` |

E02 沒有改變 shared repair 本身的候選、排序或接受規則，只排除邊際效益已消失後的重複搜尋。

### 中型案例修正

E03 擴大篩選時，`testcase4` 顯示初次 shared insertion 為 0 並不代表
後續仍無候選：普通 Repair/Reclaim 改變 tree 後，baseline 的交替 shared cycle
又插入了約 1,099 個 buffer，並顯著改善後續 WNS。因此：

- `existing_shared_disable_alternation_if_initial_no_insertion = false`；
- 仍保留連續 2 個 zero-insertion cycle 的 streak guard，作為較保守的邊際效益停止條件。

## E03：Strategy8 目標係數 checkpoint

Strategy8 內部的 repair/reclaim 與 perturb cycle 仍使用 Pipeline 的
`0.40/0.30/0.30` 接受準則，但進入 Stage 前的 FinalAlt tree 與每個已接受狀態
改用 `0.64/0.18/0.18` 比較，Stage 結束時復原最高 target-score tree。

| Case | 原推估 score | E03 推估 score | SS TNS / WNS / NVP | Area | 結果 |
|---|---:|---:|---|---:|---|
| testcase0 | 1.376264 | 1.376264 | -0.0050 / -0.0028 / 5 | 115.779 | 持平 |
| testcase2 | 1.731064 | 1.735920 | -0.0405 / -0.0034 / 29 | 417.289 | +0.004856 |
| testcase1_v2 | 1.732298（E02） | 1.736026 | -0.5443 / -0.0047 / 241 | 689.890 | +0.003728 |
| testcase0_v2 | 1.728503 | 1.737211 | -1.9949 / -0.0044 / 959 | 2967.355 | +0.008708 |

`testcase4` 在 E03 run 降為 1.715499，定位為 E02 的 aggressive initial-empty guard，
因此交由 E04 撤銷該 guard 後重測，不當作 target checkpoint 本身的結論。

## E04：允許 initial-empty 後再試一輪

`testcase4` 重測結果：

- FinalAlt：SS TNS/WNS/NVP `-0.5104 / -0.0064 / 338`，Area `4607.692`；
- Strategy8 target checkpoint：`1.734266 -> 1.737159`；
- 最終：SS `-6.1574 / -0.0064 / 2064`，Area `4345.354`；
- baseline 推估 score `1.735653`，E04 `1.737159`，淨改善 `+0.001506`。

因此預設不再因第一輪為空就永久停用，但仍會在連續兩輪為空後停止。

## E05：Strategy8 target-checkpoint stagnation stop

若連續 6 個 perturb/recover cycle 都沒有改善最後真正會保留的
`0.64/0.18/0.18` checkpoint，Strategy8 立即停止，不再因內部 Pipeline score
仍有動作就強制做滿 20 cycles。

| Case | E03/E04 cycles | E05 cycles | 舊總 runtime | E05 總 runtime | 最終品質 |
|---|---:|---:|---:|---:|---|
| testcase0 | 20 | 6 | 約 1.01 s | 0.53 s | 完全相同 |
| testcase2 | 20 | 6 | 約 5.08 s | 2.39 s | 完全相同 |
| testcase1_v2 | 20 | 8 | 約 12.20 s | 6.92 s | 完全相同 |
| testcase0_v2 | 20 | 7 | 136.01 s | 83.88 s | 完全相同 |
| testcase4 | 20 | 7 | 275.26 s | 188.70 s | 完全相同 |

`testcase0_v2` 總 runtime 減少約 38%，`testcase4` 減少約 31%；兩者
Strategy8 runtime 分別由約 `75.35 -> 24.63 s` 與 `131.77 -> 44.85 s`。

## E06：Near-closure delay overshoot candidates

舊規則只產生 delay 不超過 required delay 的 type；若所有 buffer 都超過，
卻 fallback 至面積最小、同時也是 delay 最大的 `REALBUF_X2`。E06 改為將所有
types 依 `|buffer delay - required delay|` 補齊候選，最終是否接受仍由原
FinalAlt heuristic 決定。

| Case | E05 target score | E06 target score | SS TNS / WNS / NVP | 觀察 |
|---|---:|---:|---|---|
| testcase0 | 1.376264 | 1.378406 | -0.0008 / -0.0008 / 3 | 改善 |
| testcase2 | 1.735920 | 1.741388 | -0.0004 / -0.0002 / 6 | 顯著接近 closure |
| testcase1_v2 | 1.736026 | 1.719831 | -3.0336 / -0.0141 / 509 | reclaim 覆蓋過程中更佳 timing state |

E06 證明 type 補齊有效，但也證明 FinalAlt 只在整個 iteration 後保留狀態不夠；
E07 因此同時在 repair 後與 reclaim 後比較 target checkpoint。

## E07：FinalAlt target checkpoint

E07 在 FinalAlt 進入點、每輪 repair 後、每輪 reclaim 後以
`0.64/0.18/0.18` 比較 tree，Stage 結束時復原最佳狀態。`testcase0`
推估 score 再提高至 `1.379004`，`testcase2` 保持 `1.741388`。

`testcase1_v2` 雖成功 rollback 到 E06 新路徑的最佳點，但該路徑從初期就因
overshoot types 參與排序而偏離 E05，最佳 target score 仍只有約 `1.716354`。
因此 E08 將 overshoot 改成只補齊舊候選未滿的空位。

## E08：Fallback-only overshoot

E08 先完整保留舊候選順序，只在 `final_alt_repair_types_per_target = 3`
尚有空位時，依 delay 由小到大補入超過 required delay 的 types。

| Case | E05 SS NVP | E08 SS NVP | E05 target score | E08 target score | E05 runtime | E08 runtime |
|---|---:|---:|---:|---:|---:|---:|
| testcase0 | 5 | 3 | 1.376264 | 1.379373 | 0.53 s | 0.48 s |
| testcase2 | 29 | 5 | 1.735920 | 1.741085 | 2.39 s | 2.28 s |
| testcase1_v2 | 241 | 10 | 1.736026 | 1.738565 | 6.92 s | 6.42 s |
| testcase0_v2 | 959 | 938（FinalAlt 終點 31） | 1.737211 | 1.737124 | 83.88 s | 51.82 s |
| testcase4 | 2064 | 2042（FinalAlt 終點 47） | 1.737159 | 1.737131 | 188.70 s | 105.36 s |

中型案例最終 target score 差異小於 `0.0001`，但 FinalAlt closure 與 runtime 大幅改善；
這組取捨保留為新預設。

## E09：Strategy8 target-score outer acceptance

Strategy8 recovery kernel 內部仍使用 Pipeline `0.40/0.30/0.30` score 找回合法狀態；
只有整個 perturb/recover candidate 是否取代 current state 改用 `0.64/0.18/0.18`。

- testcase2、testcase0_v2、testcase4 與 E08 完全相同；
- testcase1_v2 target score `1.738565 -> 1.738723`；
- testcase1 與 testcase3 相對原 baseline 分別提高到 `1.381132`、`1.734758`。

### testcase530

| Metric | Baseline | E09 | 變化 |
|---|---:|---:|---:|
| Target score | 1.657152 | 1.669947 | +0.012795 |
| SS TNS / WNS / NVP | -35.9712 / -0.0105 / 16,304 | -19.5027 / -0.0114 / 11,718 | TNS/NVP 改善，WNS 略退 |
| FF TNS / WNS / NVP | -0.0098 / -0.0007 / 14 | -0.0130 / -0.0009 / 18 | 略退 |
| Area | 115,015.557 | 104,310.122 | -9.31% |
| Optimizer runtime | 約 615 s（舊 run 觀察） | 532.62 s | 合法完成且低於 570 s |

E09 的 FinalAlt 於累積 373 s 為 Strategy8 預留時間而停；Strategy8 只做 1 cycle，
花 143.32 s，accepted cycles 為 0，最終 tree 與 FinalAlt 相同。因此 E10 針對
path count 達 250,000 的案例停用 Strategy8，但仍保留 60 s final validation reserve。

## E10：Large-case Stage budget reallocation

E10 在 path count 達 250,000 時停用 Strategy8，FinalAlt 改為一直搜尋到
`time_limit - 60 s validation reserve`。

| Metric | E09 | E10 | 變化 |
|---|---:|---:|---:|
| Target score | 1.669947 | 1.675925 | +0.005978 |
| SS TNS / WNS / NVP | -19.5027 / -0.0114 / 11,718 | -1.6321 / -0.0049 / 3,237 | 大幅改善 |
| FF TNS / WNS / NVP | -0.0130 / -0.0009 / 18 | -0.0004 / -0.0001 / 4 | 大幅改善 |
| Area | 104,310.122 | 107,253.601 | +2.82% |
| Runtime | 532.62 s | 529.02 s | 持平且安全完成 |

相對原 baseline，E10 total violations 減少約 80.1%、Area 減少約 6.75%、
target score 增加 `0.018773`。

## E11：Large-case reclaim top-10k（淘汰）

E11 將大型 case 每輪 reclaim 從 50,000 減到 10,000，但排名前 10,000 幾乎
全為 delete candidates：每輪只接受 3–74 個 delete，resize 為 0。雖然 FinalAlt
iterations 由 15 增到 18，Area 持續增加，target checkpoint 的 best iteration 停在 0。

| Metric | E10 | E11 |
|---|---:|---:|
| Target score | 1.675925 | 約 1.663 |
| SS TNS / WNS / NVP | -1.6321 / -0.0049 / 3,237 | -31.4514 / -0.0045 / 15,316 |
| Area | 107,253.601 | 115,917.211 |
| FinalAlt iterations | 15 | 18 |

結論是 global top-K 不能在沒有 move-kind diversity 時直接縮小。E12 改為先對
Delete、original resize、new-buffer resize 各保留 5,000，再套用 15,000 總上限。

## E12：Large-case stratified reclaim（淘汰）

E12 成功讓 resize candidates 重新進入 top-K，但 target score 只有約
`1.672402`，低於 E10 `1.675925`。SS NVP 從 3,237 略降到 3,113，但
Area 從 `107,253.601` 升到 `110,504.462`，不符合目標係數下的最佳 tradeoff。
因此大型預設完整回復 E10：global top-K 50,000、per-kind cap 關閉。

## E13：縮短大型 finalization reserve（淘汰）

E13 將大型 case 的 final validation/output 預留從 60 秒縮短至 35 秒，讓
FinalAlt 額外完成 iteration 15。該 iteration 沒有刷新 target checkpoint；最佳
狀態仍是 iteration 12，最終 SS/FF/Area 與 E10 完全相同：SS TNS/WNS/NVP
`-1.6321/-0.0049/3237`、FF `-0.0004/-0.0001/4`、Area `107253.601`、
target score 約 `1.675925`。總 runtime 則由 E10 的 `529.024 s` 增至
`554.401 s`。因此這段額外時間已落在相同搜尋軌跡的邊際效益飽和區，預設恢復
60 秒；未來若要使用更多時間，應先改變候選或 acceptance 軌跡。

## E14：大型 case 略過 ExistingBufferSharedReclaim（採用）

E10 的 testcase530 在初始 shared cycle 與第一次 Repair/Reclaim 後的 alternation
各試 16 個 shared candidates，兩輪都沒有接受任何 insertion/reclaim move，合計
約花 60 秒。E14 在 path count >= 250,000 時略過初始 shared Stage 及後續
alternation，小中型 case 不變。

| Metric | E10 | E14 | 變化 |
|---|---:|---:|---:|
| Target score (0.64/0.18/0.18) | 1.675925 | 1.701764 | +0.025839 (+1.54%) |
| SS TNS / WNS / NVP | -1.6321 / -0.0049 / 3237 | -2.0124 / -0.0016 / 3678 | TNS/NVP 略退、WNS 改善 |
| FF TNS / WNS / NVP | -0.0004 / -0.0001 / 4 | 0 / 0 / 0 | 完全 closure |
| Area | 107253.601 | 90391.536 | -16862.065 (-15.72%) |
| Runtime | 529.024 s | 535.793 s | +6.769 s |

移除零產出的 trial/rollback 不只省時間，也避免其 tree/index 重建改變後續 tie
order。E14 的 Repair/Reclaim 最終 Area 明顯較低，FinalAlt 再把 target score
從約 1.673 推到 1.702。雖然總 violation 多 437，但在目前推估權重下 Area 與
FF closure 的收益明顯較大，因此採用。testcase0 smoke 維持 3 violations、
Area 118.336，確認 large gate 不影響小 case。

## E15：Deadline-aware post-Phase0 portfolio（部分採用）

E15 從同一個 Phase0 checkpoint 依序跑目前預設的 score-based Repair/Reclaim 與
heuristic Repair/Reclaim，兩條都接完整 FinalAlt/Strategy8，最後以
0.64/0.18/0.18 target score 選 winner。第二條 branch 只能使用外層 optimizer
剩餘的 global deadline；舊版 alternating-delete 分支不執行，大型 250k-path
case 也直接略過。

| Case | Score branch target/runtime | Heuristic target/runtime | Winner | Portfolio runtime |
|---|---:|---:|---|---:|
| testcase0 | 1.379373 / 0.439 s | **1.381665 / 0.437 s** | heuristic | 0.878 s |
| testcase1 | 1.381132 / 0.698 s | **1.381495 / 0.614 s** | heuristic | 1.316 s |
| testcase2 | **1.741085 / 2.116 s** | 1.739998 / 2.683 s | score | 4.807 s |
| testcase3 | **1.734758 / 13.242 s** | 1.710739 / 39.017 s | score | 52.296 s |
| testcase1_v2 | **1.738723 / 8.654 s** | 1.737850 / 7.660 s | score | 16.332 s |
| testcase0_v2 | **1.737124 / 49.815 s** | 1.691531 / 176.096 s | score | 226.064 s |
| testcase4 | **1.737131 / 102.262 s** | 1.730771 / 187.668 s | score | 290.138 s |

Target selector 能保證這組測資品質不退，但 baseline SS NVP >= 2,157 後第二條
branch 全部落敗，且成本快速上升。因此正式預設只在 baseline SS violation
count <= 1,000 時啟用 portfolio；這保留 testcase0/1 的實際增益，又避免在
中大型 case 用數十到數百秒做無效保險。門檻是可調參數，不改變兩條 branch
內部演算法。

## E16：0.52/0.24/0.24 midweight branch（淘汰）

E16 保持 Phase0 與 target selector 不變，只在第二條 score-based branch 將內部
搜尋權重由 0.40/0.30/0.30 改成 0.52/0.24/0.24。快速 screen 已呈現一致負向：

| Case | Default target / SS / Area | Midweight target / SS / Area | Target delta |
|---|---:|---:|---:|
| testcase2 | 1.741085 / -0.0004 / 427.506 | 1.731614 / -0.0010 / 469.840 | -0.009471 |
| testcase1_v2 | 1.738723 / -0.0788 / 721.258 | 1.733933 / -0.0287 / 775.182 | -0.004790 |

Midweight 在 testcase1_v2 修掉更多 timing，但增加 53.924 Area；testcase2 則
timing/Area 都較差。這與 E01 的結論一致：提高 repair 搜尋內的 SS 權重會使路徑
付出過多 buffer Area，即使最終 target 的 SS 權重是 0.64 仍不划算。因此停止
後續昂貴 case，功能保留但預設關閉。

## E17：0.35/0.25/0.40 area-oriented branch（淘汰）

方向相反的 E17 也未改善 target。testcase2 的 alternative branch target
`1.737159 < 1.741085`，Area 甚至由 `427.506` 增至 `431.531`；testcase1_v2
雖將 Area `721.258 -> 701.425`，SS TNS 卻由 `-0.0788` 惡化至 `-0.4406`，
target 降至 `1.735669`（default `1.738723`）。因此 0.40/0.30/0.30 暫時應視為
有效的搜尋 heuristic，而不是需要直接追隨推估真實係數；不再繼續局部掃權重。

## E18–E19：Repair Pulse delay-scale branches（淘汰）

兩個分支保持 0.40/0.30/0.30 acceptance，只把 adaptive pulse delay scale 分別
乘 0.75 與 1.25，嘗試從 Phase0 後改變第一輪 repair 軌跡。

| Case | Default target | 0.75x target | 1.25x target |
|---|---:|---:|---:|
| testcase2 | 1.741085 | 1.737884 | 1.734872 |
| testcase1_v2 | 1.738723 | 1.733927 | 1.735898 |

0.75x 在兩案都增加最終 Area；1.25x 也沒有以較少 buffer 換到收益，反而同時
留下較差 timing/Area minimum。倍率兩側皆負向後停止參數掃描，兩項功能預設
關閉。

## E21 / E22：FastTimingEngine 冗餘同步移除（採用）

FastTimingEngine 的 transactional commit 已更新 arrival/timing cache，因此在沒有
外部直接改 tree 的情況下，Repair Pulse→Reclaim 與 FinalAlt repair→reclaim 不必再
`sync_from_tree()` 一次；只有 cache 失效的 perturb/bootstrap 等路徑仍保留同步。

| Case | E21 runtime | E22 runtime | 最終 timing / Area |
|---|---:|---:|---|
| testcase0_v2 | 52.0695 s | 46.7095 s | 完全相同 |

E22 減少 `315 -> 311` 次 FastTiming sync，總 runtime 降 `10.3%`，因此保留。

## E23 / E24：關閉沒有貢獻的 Strategy8（採用）

在目前 area-heavy pipeline (`0.20/0.20/0.60`) 的 testcase0_v2，Strategy8 花
28.538 秒跑 6 cycles、沒有接受任何 recovery，最後 checkpoint 與 Stage8 進入點
完全相同。

| Mode | Runtime | SS TNS / WNS / NVP | FF | Area | Pipeline score |
|---|---:|---|---|---:|---:|
| Strategy8 on | 70.6737 s | -4.8495 / -0.0676 / 1244 | clean | 2924.824 | 1.122761 |
| Strategy8 off | 42.8797 s | 完全相同 | clean | 2924.824 | 1.122761 |

所以 production 預設 `enable_perturb_recover = false`；實驗 code 仍完整保留。

## E25–E27：完整 Pipeline 分支與 runtime scheduler（部分採用）

現行 portfolio 固定先跑 `full_score` 作為可輸出的 fallback。只有它完成後仍有
實際安全餘裕時，才依序啟動 heuristic 或初始狀態分支。不是把 570 秒平均切分，
而是以每個 route 的實際停止時間決定下一條是否啟動；small/medium/large 分別留下
10/30/60 秒 final validation reserve。

testcase4 顯示 branch 的價值很明確：score route 的 target score 為 `1.598905`，
heuristic route 為 `1.709411`（+`0.110506`），雖然 heuristic 的 Area 較高
（5091.900 vs 4563.544），卻把 SS NVP 從 1677 降到 55。因此後續搜尋多個
minimum 比只微調單一路徑更有潛力。

## E28 / E29：保守 initial-state resize（淘汰）

E28 從 Phase0 checkpoint 隨機選 48 個 original buffers，只在平均 delay rank
±2 的合法 type 中 deterministic-random resize；E29 則以 endpoint pressure 排序，
將前 48 個 node 往預期有利的 delay 方向推最多兩個 rank。兩者都不改 topology、
不插入或刪除 buffer，且每次都先做 legality check。

它們沒有勝過原 checkpoint：testcase3 target 分別為 `1.686312`、`1.732200`，
baseline score branch 為 `1.732958`；testcase0_v2 seeded 為 `1.645505`，heuristic
branch 為 `1.661459`。實作保留為 optional baseline，但 defaults 關閉。

## E31：面積優先 cosine objective scheduler（淘汰，保留可選）

這次 scheduler 確實作用於 score-based acceptance，而不只是 report：

```text
Phase0 / ExistingShared / RR cycle 0–3 : 0.10 / 0.10 / 0.80
RR cycle 4–15                          : half-cosine 逐步轉為 0.60 / 0.20 / 0.20
FinalAlt                               : 0.60 / 0.20 / 0.20
```

每個 Repair/Reclaim cycle 固定一組權重，避免 pulse 與 reclaim 的同一次
transaction 在中途改變 objective。Timing report 同時列出排程設定與每個 cycle
實際使用的 objective weights；`enable_score_weight_scheduler` 是獨立可開關的
config。

testcase3 A/B：

| Mode | Winning branch | Target score 0.64/0.18/0.18 | SS TNS / WNS / NVP | Area | Outer runtime |
|---|---|---:|---|---:|---:|
| Scheduler off | full_score | 1.732958 | -0.0094 / -0.0031 / 14 | 1279.285 | 31.039 s |
| Scheduler on | full_heuristic | 1.723740 | -0.0118 / -0.0022 / 16 | 1441.200 | 49.427 s |

testcase3 的最佳 target score 降 `0.009218`（約 `-0.53%`），因此 scheduler
不取代全域預設。主要原因是
score route 常在第 9 cycle 左右因 split-depth exhausted 停止，尚未完成 transition；
它在 area-heavy hold 中累積的 repair 選擇又不足以被 FinalAlt 的 end weights 補回。
但 testcase0_v2 的 standalone heuristic route 則從 E25 的 `1.661459` 提升到
`1.678323`（+`0.016864`），SS NVP `74 -> 572` 而 Area `3694.776 -> 3339.620`。
這是一種 target score 上正向、但不同 case 差異很大的路徑；E33 因此把 scheduler
改成額外 portfolio branch，而非全域切換。若日後重試 scheduler 本身，仍應讓它依
「timing closure 進度」而非只依 cycle 數前進。

## E32：5% / 10% unrestricted initial-buffer perturbation（測試中）

為了真的走到不同 initial basin，E32 不再只在 delay rank ±1/±2 內 resize：

1. 收集所有 original buffer（不觸碰 optimizer 新插入的 `NEW_BUF`）。
2. 以固定 seed 打亂 node 順序，選取全部 original buffers 的 5% 或 10%。
3. 每個被選的 node 從整個 `buf.lib` 的 **所有**其他 legal types 均勻選一種；
   仍檢查該 type 是否支援現有 fanout。
4. 變更後完整 legality 驗證。若失敗，整個 initial state rollback，絕不把非法 tree
   交給 Pipeline。
5. 分別在 Phase0 checkpoint 後開始 remaining Pipeline（post-Phase0），以及在
   original tree 擾動後重新跑完整 Phase0 + remaining Pipeline（pre-Phase0）。

所有 diversification branch 都排在 `full_score`、`full_heuristic`，以及 E33 的
`cosine_heuristic` fallback branches 後；最多 7 routes，且只有扣除 case-size
finalization reserve 後仍至少剩下 60 秒才開始 perturb branch。這個門檻是為了避免
剛建立另一個完整 tree/state 後才撞到 deadline。

### testcase3

| Route | Changed buffers | Target score | SS TNS / WNS / NVP | Area | Delta vs full_score |
|---|---:|---:|---|---:|---:|
| full_score fallback | 0 | 1.732958 | -0.0094 / -0.0031 / 14 | 1279.285 | — |
| post-Phase0 5% | 228 | 1.733656 | -0.0089 / -0.0026 / 18 | 1279.284 | +0.000698 |
| post-Phase0 10% | 456 | 1.732452 | -0.0079 / -0.0028 / 16 | 1293.624 | -0.000506 |
| pre-Phase0 5% | 228 | 1.720909 | -0.0225 / -0.0118 / 19 | 1277.840 | -0.012049 |
| pre-Phase0 10% | 456 | **1.735434** | -0.0042 / -0.0013 / 16 | 1279.923 | **+0.002476** |

完整 sequential portfolio runtime 為 `80.3732 s`，最終選到
`prephase_aggressive_10pct`。這是一個小但明確的改善，且不是只靠 Area：它同時
改善 SS TNS/WNS。中型 testcase0_v2 的驗證 report 會補在
`run_20260828_E32_aggressive_initial_states_medium`，再決定是否把這組 routes
保留為 production default。

## E33：scheduler 接在 shared Phase0 checkpoint（設計修正）

全域 `enable_score_weight_scheduler` 維持 `false`，使 normal `full_score` 與
`full_heuristic` 的既有軌跡完全不變。第一個 E33 implementation 在這兩個 fallback
route 都完成後，從 shared Phase0 checkpoint 執行：

```text
cosine_heuristic (first implementation)
  = shared Phase0 checkpoint
  + heuristic Repair/Reclaim acceptance
  + 0.10/0.10/0.80 hold 4 cycles
  + 12-cycle half-cosine → 0.60/0.20/0.20
```

testcase3 report 顯示它與普通 `full_heuristic` 的 SS/FF/Area/target score **全部相同**
（`1.726390`）。原因是 Phase0 之後的 heuristic Repair/Reclaim 不依 alpha/beta/gamma
接受 move，FinalAlt 又使用獨立的 target checkpoint；只改後段 scheduler 不會改 tree。
這個結果保留在 `run_20260828_E33_cosine_portfolio_testcase3`，但不採用。

E34 改為從 original tree 開始、在 scheduler 已設為 `0.10/0.10/0.80` 時重跑
完整 Phase0；這才重現 E31 的有效作用點。它仍排在 normal fallback routes 後，
最多 7 routes，並由 target selector 選取，因此不會讓 testcase3 的 scheduler
負向結果覆蓋正常解。

`run_20260828_E34_cosine_full_pipeline_testcase3` 驗證它已正確產生獨立 state：
`cosine_heuristic` target 為 `1.723740`（SS `-0.0118/-0.0022/16`，Area `1441.200`），
而 normal `full_score` 為 `1.732958`，E32 的 `prephase_aggressive_10pct` 為
`1.735434`。selector 最終選到後者，完整 sequential runtime `129.2354 s`，所有
七個 routes legality 都是 OK。

250k+ path case 預設不保留 pre-Phase0 original-tree clone
(`portfolio_allow_prephase_routes_on_large_case = false`)；這是 memory guard，不是
QoR gate。既有 Phase0 checkpoint 之後的 normal/post-Phase0 routes 仍可依實際時間
啟動，但需要另外一個完整原 tree 的 E34/E32 pre-Phase0 route 會略過，避免 500k FF
hidden case 因多份 tree 同時存活而被 cgroup 提前 kill。

## E35：Deadline-filling multi-start 提交版

先無條件完成 `full_score` 保底 route，再輪替 5% / 10% / 20%
pre/post-Phase0 initial-state perturbation、score/heuristic acceptance 與 cosine
scheduler。每次 restart 使用不同 deterministic seed，route 數不設上限，
只由 global deadline 與 small/medium/large 10/30/60 秒 reserve 停止。記憶體
只保留一棵當前 target-score 最佳 tree。

以暫時 `portfolio_max_restart_routes=2` 執行 testcase0 smoke：9 routes 在
2.66 秒內完整結束，峰值 RSS 20,132 KB，成功輸出 1,730 行 tree。
驗證後已恢復 `0`（deadline-only）並重編正式版。

## E36：Compiler optimization A/B

為避免 deadline-filling 使每次 benchmark 固定跑滿十分鐘，實驗暫時固定
testcase2 為 9 routes，每種 binary 重複三次。所有版本產生的 clock tree
SHA-256 皆為
`1762f6e2a43345f6eb7f7753a6800a5ea641364ff2ebcacf1c2c1acaefb1375c`，
因此 QoR 完全相同。

| Compiler flags | Wall time（3-run average） | Binary size | 結論 |
|---|---:|---:|---|
| `-O3 -DNDEBUG` | 16.34 s（首輪） | 1,070,728 B | baseline |
| `-O3 -DNDEBUG -march=native -mtune=native` | 18.77 s | 1,091,208 B | 慢 14.9%，不採用 |
| `-O3 -DNDEBUG -flto=auto` | 18.00 s（順序測試） | 946,472 B | 無速度收益 |
| `-O3 -DNDEBUG -march=native -mtune=native -flto=auto` | 17.64 s | 966,952 B | 慢 7.9%，不採用 |

另外將 O3 與 LTO binary 交錯執行三組以降低溫度／CPU frequency
偏差：O3 平均 17.68 秒，LTO 平均 17.83 秒，LTO 仍慢約 0.9%。
因此提交版維持 portable `-O3 -DNDEBUG`，不加 native、LTO 或 fast-math。
