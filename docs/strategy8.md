# Strategy 8：Deterministic Perturb-and-Recover

## 目前採用設定（2026-08-28，提交前版本）

這一節是目前可執行版本的設定快照。歷史 A/B 數據保留在後文，但不應覆蓋本節；
若本文與程式不同，以 `src/optimizer.h` 和 `src/optimizer.cpp` 為準。

| 項目 | 目前採用值 |
|---|---|
| Global time limit | 570 秒 |
| Weighted objective | `alpha / beta / gamma = 0.20 / 0.20 / 0.60` |
| Phase0 | On；individual FastTimingEngine trial；5 passes；每 node 最多 6 types；180 秒 safety budget；不使用 batch；score acceptance On |
| Phase0 ranking | pressure × 可達 average SS/FF delay delta − area-growth penalty；score-aligned ranking Off |
| Repair/Reclaim | On；absolute boundary 360 秒；最多 200 cycles；每輪 Reclaim safety cap 120 秒；score acceptance On |
| Repair Pulse | score 不得變差；rejected batch depth-2 split；failure recovery On（最多 3 次、scale ×0.70、最低 0.50、暫時略過 8 targets） |
| Reclaim | 保留原本 area/pressure heuristic 排序；只產生 area-saving candidates；score 不得變差 |
| FinalAlt | On；最多 10,000 iterations；500 repair attempts/iteration；top 50,000 reclaim candidates；score acceptance Off |
| Strategy8 | **Off**；E23/E24 沒有接受任何 recovery，正式提交不花時間在此 stage；實作與參數保留可選 |
| Strategy8 Recovery | score-based；reclaim-first；最多 200 cycles；bootstrap/pulse/reclaim candidate count unlimited（仍受時間限制） |
| Strategy8 guards | timing guard Off；NVP-growth guard Off；候選必須 legality OK 且 weighted score 嚴格提高 |
| Post-Phase0 portfolio | On；先完整跑 full_score fallback，再依實際餘裕跑 heuristic、cosine、以及 initial-state routes |
| Deadline filling | On；固定 routes 後輪替 post/pre-Phase0 5%/10%/20% 擾動，使用不同 deterministic seed，並混合 score/heuristic/cosine 路線 |
| Portfolio selector | legality first，然後 `0.64 / 0.18 / 0.18` target score；route 數不設上限，reserve 10/30/60 秒 |
| Large-case portfolio guard | 250k+ paths 不保留 pre-Phase0 原 tree clone，避免額外完整 tree copy 導致記憶體風險 |
| Global best checkpoint | Off；FinalAlt target checkpoint 仍會保存 stage 內 target-score 最佳狀態 |

目前正式 Repair/Reclaim 與 Strategy8 Recovery 都是 score-based，但二者仍由
不同開關控制：`enable_repair_reclaim_score_based_acceptance` 與
`enable_perturb_recover_score_based_repair_reclaim`。FinalAlt 保留 heuristic
acceptance，沒有隨這次設定一併切換。

### 目前建置與報表流程

正式 build 固定使用專案根目錄唯一的 `build/` 目錄。新的 single-config build
若未指定 build type，CMake 預設為 Release；`run_reports.sh` 每次更會明確重新設定：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_OUTPUT=ON
cmake --build build
```

因此目前 `build/cadd0045` 使用 `-O3 -DNDEBUG` 並定義 `ENABLE_OUTPUT`。
`run_reports.sh` 會將七個 testcase 的 timing report 收集至指定的 `report/`
子目錄；它不建立 `build-o3`、`build-release` 或其他 build 資料夾。

2026-07-31 的受控 O3 比較確認：Output ON 與 OFF 的七份輸出 Clock Tree
SHA-256 全部相同；ON 總 wall-clock 為 438.53 秒、OFF 為 430.18 秒，差 1.94%。
所以 Output ON 保留為目前預設，方便取得 timing report，而不會改變 QoR。
完整紀錄見
`report/strategy8/4_beta_test_release_output/comparison_output_on_vs_off.md`。

## 目標

Strategy8 在 Strategy7 的 Final Alternating Greedy 後方加入一個額外的
`PerturbAndRecover` stage。它不是重新執行整套 optimizer，也不是目前就導入
simulated annealing；目前的目標是建立安全、可重現、可量測的擾亂與復原
骨架：

```text
Strategy7 最終狀態
        ↓
保存 Current / Best checkpoint
        ↓
刪除最大累積 Area path 上可合法移除的 Buffer
        ↓
把 raw perturbed state 直接交給完整 Reclaim-first Repair/Reclaim recovery
        ↓
完整 Timing / Area / Legality 評估
        ↓
只接受合法且 Score 嚴格提高的狀態
        ↓
失敗則 rollback Current checkpoint
        ↓
結束時一定復原 Best checkpoint
```

這使未來加入「暫時接受壞解」或 temperature schedule 時，不需要再重做 tree
checkpoint、recovery kernel 與最終 best-state restore。

## 與 Strategy7 相比新增的內容

### 1. 新的 Pipeline stage

目前完整預設流程為：

```text
Phase0
  -> Repair/Reclaim cycles
  -> Final Alternating Greedy
  -> Strategy8 Perturb-and-Recover
  -> restore Strategy8 best checkpoint
  -> full timing/area/legality validation
```

Strategy8 本身只在 FinalAlt 後方啟動，並重用 Repair/Reclaim 的 Pulse 與
pressure-guided Reclaim implementation。其間，正式 Pipeline 已演進為 Phase0 與
Repair/Reclaim 使用 weighted-score acceptance；這是目前採用設定的一部分，不是
Strategy8 stage 對前方流程做的臨時覆寫。

### 2. v8 目前預設：Brutal Delete-Only Path perturbation

每個 outer cycle 先檢查所有 leaf，對每條 leaf-to-root path 累加目前 Buffer
type 的 Area，從「至少含一個可刪 Buffer」的 path 中選擇總和最大者。FF、
input 與其他非 Buffer node 會保留在 path 中以維持順序，但不計 Area。Area
相同時以 leaf 名稱排序，確保選擇穩定。

接著由 leaf 往 root 處理整條 path：

- 通過既有 reclaim-delete legality 的 Buffer：直接刪除。
- 原始 Buffer、fanout/topology 不允許刪除的 Buffer，以及其他不可刪 Buffer：
  type 與 topology 都完全不動。

這些動作不逐步檢查 Timing 或 Score，也不做 Resize 或 Insert。它會一次刪除
path 上所有可刪 Buffer，再交給 recovery 修復。被拒絕的 Buffer-path signature
會記錄，避免不同 leaf 其實共用同一段 Buffer path 時重複嘗試；接受新 current
後 tree 已改變，signature 集合會清空。因為 v8 path mutation 沒有隨機操作，
random seed 在 brutal mode 下不影響結果。

v6 的 Timing/Area/Balanced/Random 四候選與隨機 resize/insert/delete 實作仍
保留；設定 `perturb_recover_use_brutal_area_path=false` 即可恢復，但不再是
預設。

### 3. 完整 Reclaim-first recovery kernel

Brutal v8 不執行每個候選的 Quick FinalAlt 小修復。破壞後的 raw state 直接進入：

```text
Bootstrap Reclaim（第一個 recovery cycle）
  candidate 數量 unlimited
  Area 降低且 weighted score 不變差
        ↓
Repair Pulse
  target 數量 unlimited
  weighted score 不變差 + depth split
  failure 時降低 scale 並 temporary-blacklist targets
        ↓
Normal Pressure-Guided Reclaim
  candidate 數量 unlimited
  Area 降低且 weighted score 不變差
        ↓
重複 Repair Pulse → Reclaim，最多 200 recovery cycles
        ↓
復原 recovery cycles 中 Weighted Score 最高的 endpoint
```

`unlimited` 是 candidate/target count 設為 `0`，不是無限時間。Bootstrap 與
Normal Reclaim 各自仍有 120 秒 local safety cap，整個 Strategy8 仍受 150 秒
local budget、全域 deadline 與 10 秒 validation reserve 約束。

這套 recovery acceptance 由
`enable_perturb_recover_score_based_repair_reclaim=true` 獨立控制。目前前方
正式 Repair/Reclaim 的 score acceptance 也已開啟，因此兩套 Repair/Reclaim
都是 score-based；兩個開關仍可各自獨立調整。

每個完整 recovery cycle 後都以 full Timing/Area 重算 Score 並更新 recovery
checkpoint；因此後續 cycle 即使退步，也不會覆蓋早先較好的 recovery endpoint。
FastTimingEngine 仍負責 recovery 內的 insert/resize/delete trial；直接 perturb
完成、checkpoint restore 或 rollback 後則做一次完整同步。

### 4. Current 與 Best checkpoint

Strategy8 保存兩份 tree：

- `current_tree`：上一個已接受狀態；某輪失敗時立即回到這裡。
- `best_tree`：目前最高分狀態；stage 結束時無條件復原。

目前採 improvement-only acceptance，所以 current 與 best 通常相同。仍分開
保存，是為了未來允許 bad step 時可以讓 current 暫時變差，同時保護 best。

Rejected cycle 的 recovery move log、FinalAlt counters 與 tree mutation 都會
rollback，不會混入正式輸出或 Strategy7 的 FinalAlt 統計。

## Acceptance 與 rollback 規則

Recovery 後先用完整 analyzer 重算 Timing、Area 與 Legality，不直接以 incremental
cache 作最後裁決。目前 score-only v3 一輪只需同時滿足：

1. Tree legality 通過。
2. 使用固定 baseline 與 `alpha/beta/gamma = 0.40/0.30/0.30` 計算的 weighted
   score 嚴格提高，差異需大於 `score_acceptance_epsilon`。

SS/FF TNS、WNS、NVP 或 Area 任一項都可以退步，只要總 Score 嚴格提高。
Timing-not-worse 與 NVP-growth guard 仍保留成獨立開關，但目前預設都關閉。

任一必要條件失敗就 rollback 到 `current_tree`。常見 stop reason：

- `accepted_improvement`
- `score_not_better`
- `timing_guard`
- `violation_guard`
- `illegal_candidate`
- `no_legal_perturbation`

目前沒有接受 bad state，也沒有 temperature、cooling schedule 或 probabilistic
acceptance。因此這一版是 deterministic perturb-and-recover，不是完整 simulated
annealing。

## 時間與停止條件

主要預設：

```cpp
enable_perturb_recover = false; // 實作保留，目前提交版不執行
enable_perturb_recover_score_based_repair_reclaim = true;
perturb_recover_time_budget_seconds = 150.0;
perturb_recover_validation_reserve_seconds = 10.0;
perturb_recover_max_cycles = 20;
perturb_recover_use_brutal_area_path = true;
perturb_recover_brutal_recovery_max_cycles = 200;
perturb_recover_random_seed = 7;
```

以下是選擇性重新開啟 Strategy8 時才會使用的時間規則：FinalAlt 會在
全域 570 秒限制前，替 Strategy8 的 150 秒 local budget 和最後驗證的
10 秒預留時間。Strategy8 遇到以下任一條件即停止：

- 完成 20 outer cycles／不同 path：`max_cycles`
- local 150 秒到期：`time_budget`
- 已接近全域 deadline，只剩 validation reserve：`time_budget`
- 連續三輪找不到尚未嘗試且可更動的 path：`no_legal_perturbation`

結束後先復原 `best_tree`，再進入原本的完整 final timing/area/legality
validation。

## 新增參數

所有參數集中在 `src/optimizer.h`：

| 參數 | 預設 | 意義 |
|---|---:|---|
| `enable_perturb_recover` | `true` | 是否執行 Strategy8 |
| `perturb_recover_time_budget_seconds` | `150.0` | stage local wall-clock budget |
| `perturb_recover_validation_reserve_seconds` | `10.0` | 最終驗證／輸出的全域保留時間 |
| `perturb_recover_max_cycles` | `20` | Brutal outer cycles／最多嘗試的不同 path 數 |
| `perturb_recover_use_brutal_area_path` | `true` | 刪除最大 Area path 上可刪 Buffer；關閉即恢復 v6 beam |
| `perturb_recover_brutal_recovery_max_cycles` | `200` | 每條 brutal path 的完整 recovery cycle 上限 |
| `perturb_recover_random_seed` | `7` | v6 beam 固定亂數 seed；v8 mutation 不使用 |
| `perturb_recover_violation_growth_ratio` | `0.05` | 可接受的 NVP 成長上限 |
| `perturb_recover_require_timing_not_worse` | `false` | 是否要求兩 corner TNS/WNS 不退步 |
| `perturb_recover_require_violation_guard` | `false` | 是否限制兩 corner 的 NVP 成長 |

以下 v6 參數仍保留，只有
`perturb_recover_use_brutal_area_path=false` 時才控制主要搜尋：

| v6 相容參數 | 預設 | 意義 |
|---|---:|---|
| `perturb_recover_candidates_per_cycle` | `4` | 每個 beam round 的候選數 |
| `perturb_recover_guided_target_ratio` | `0.70` | 從 guided pool 選 target 的機率 |
| `perturb_recover_min_moves` / `max_moves` | `4 / 12` | v6 擾亂強度範圍 |
| `perturb_recover_quick_recovery_iters` | `1` | 每個候選的 Quick Recovery |
| `perturb_recover_quick_repair_attempts` | `50` | Quick Repair attempts |
| `perturb_recover_quick_reclaim_candidates` | `1000` | Quick Reclaim trial 上限 |
| `perturb_recover_recovery_iters` | `3` | 被選候選的 Cycle Recovery 上限 |
| `perturb_recover_repair_attempts` | `250` | v6 Repair Pulse target 上限 |
| `perturb_recover_reclaim_candidates` | `10000` | v6 Normal Reclaim trial 上限 |

## Deterministic v1 A/B 實驗

兩組皆使用同一個 build、相同 Strategy7 參數與
`alpha/beta/gamma = 0.40/0.30/0.30`。Before 關閉 Strategy8；After 開啟上述
deterministic v1。執行為 sequential debug/report build，因此 runtime 只代表本機
這次量測。

### testcase0

| 指標 | Strategy7 before | Strategy8 after | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0062 | -0.0062 | 相同 |
| SS WNS | -0.0031 | -0.0031 | 相同 |
| Internal SS NVP | 9 | 9 | 相同 |
| FF TNS/WNS/NVP | 0 / 0 / 0 | 0 / 0 / 0 | 相同 |
| Area | 121.365 | 121.031 | -0.334（-0.275%） |
| Weighted score | 0.960800 | 0.961170 | +0.000370（+0.0385%） |
| Total runtime | 0.6526 s | 2.2374 s | +1.5848 s（3.43x） |

Strategy8 local runtime 為 1.624 秒；20 cycles 中接受 2、拒絕 18。接受的兩輪
合計在正式 state 中造成 `+7 buffer / -5 buffer / 7 resize`。Timing 完全不退步，
改善全部來自 area。

### testcase4

| 指標 | Strategy7 before | Strategy8 after | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0468 | -0.0468 | 相同 |
| SS WNS | -0.0027 | -0.0027 | 相同 |
| Internal SS NVP | 58 | 58 | 相同 |
| FF TNS/WNS/NVP | 0 / 0 / 0 | 0 / 0 / 0 | 相同 |
| Area | 5608.400 | 5608.400 | 相同 |
| Weighted score | 1.538533 | 1.538533 | 相同 |
| Total runtime | 334.3298 s | 410.1842 s | +75.8544 s（+22.7%） |

Strategy8 local runtime 為 92.372 秒，共完成 5 cycles，全部 rollback。拒絕原因為
3 次 `timing_guard` 與 2 次 `score_not_better`。其中一輪 score 只提高約
`0.000004` 且 area 減少 `0.138`，但 timing 退步，因此沒有為了小幅 score 增益
放寬 guard。

大案例證明 checkpoint 與 rollback 正常，但也顯示目前 random perturb +
完整 FinalAlt candidate rebuild 的成本太高，第一版尚未在 testcase4 產生 QoR
收益。

原始報表：

- `report/strategy8/baseline_strategy7/testcase0/`
- `report/strategy8/baseline_strategy7/testcase4/`
- `report/strategy8/deterministic_v1/testcase0/`
- `report/strategy8/deterministic_v1/testcase4/`

## Aggressive v2 Guarded 實驗

為了擴大 search space，v2 同時增加 perturb 寬度、recovery 深度與 stage
budget；acceptance、checkpoint 與 rollback 規則完全不變。

| 設定 | Deterministic v1 | Aggressive v2 |
|---|---:|---:|
| Local budget | 90 秒 | 150 秒 |
| Maximum cycles | 20 | 40 |
| Perturb moves | 1～4 | 4～16 |
| Intensity step streak | 3 | 2 |
| Move attempt multiplier | 20 | 30 |
| Recovery iterations | 2 | 4 |
| Repair attempts/recovery | 100 | 250 |
| Reclaim candidates/recovery | 5,000 | 10,000 |

### testcase0：Aggressive v2 有效

| 指標 | v1 | Aggressive v2 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0062 | -0.0045 | 絕對值 -27.42% |
| SS WNS | -0.0031 | -0.0031 | 相同 |
| Internal SS NVP | 9 | 7 | -22.22% |
| Area | 121.031 | 118.497 | -2.534（-2.094%） |
| Weighted score | 0.961170 | 0.964004 | +0.002834（+0.2948%） |
| Total runtime | 2.2374 s | 5.8882 s | +163.17% |

Aggressive v2 跑滿 40 cycles，接受 9、拒絕 31。Perturb operation 數從 v1
的 38 增加到 211；recovery 內接受的 insertion/reclaim moves 從 235 增加到
1,420。最後正式 state 累積 `+102 buffer / -86 buffer / 87 resize`。

### testcase4：工作量增加，但仍無可接受解

| 指標 | v1 | Aggressive v2 | 變化 |
|---|---:|---:|---:|
| SS TNS/WNS/NVP | -0.0468 / -0.0027 / 58 | 相同 | 相同 |
| Area | 5608.400 | 5608.400 | 相同 |
| Weighted score | 1.538533 | 1.538533 | 相同 |
| Strategy8 runtime | 92.372 s | 151.743 s | +64.27% |
| Total runtime | 410.1842 s | 432.1149 s | +5.35% |
| Completed cycles | 5 | 3 | -40% |
| Applied perturb moves | 7 | 13 | +85.71% |
| Recovery insertion/reclaim moves | 32 | 44 | +37.50% |

三個 aggressive candidates 全部被 `timing_guard` rollback。後兩輪的 score
分別從 `1.538533` 提高到 `1.538535` 與 `1.538544`，Area 也減少 `0.069`
與 `0.392`，但 SS TNS 或 WNS 退步，因此沒有接受。最終輸出與 v1 的 SHA-256
完全相同。

這說明 aggressive v2 的確在每輪做了更多擾亂與 recovery work，但大案例一次
recovery 太昂貴，150 秒只完成三個 checkpoint evaluations。它擴大了單輪跳躍
半徑，卻沒有增加完整候選狀態的數量。

Aggressive v2 報表：

- `report/strategy8/aggressive_v2/testcase0/`
- `report/strategy8/aggressive_v2/testcase4/`

## Score-only v3 實驗

v3 保留 aggressive v2 的所有 search-space 參數，只關閉：

```cpp
perturb_recover_require_timing_not_worse = false;
perturb_recover_require_violation_guard = false;
```

因此 acceptance 只剩：

```text
Full legality passes
AND
candidate score > current score + score_acceptance_epsilon
```

### testcase0

| 指標 | Aggressive guarded v2 | Score-only v3 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0045 | -0.0110 | 絕對值 +144.44% |
| SS WNS | -0.0031 | -0.0031 | 相同 |
| Internal SS NVP | 7 | 11 | +57.14% |
| Checker SS NVP | 4 | 8 | +100% |
| Area | 118.497 | 118.302 | -0.195（-0.165%） |
| Weighted score | 0.964004 | 0.964122 | +0.000118（+0.0122%） |
| Total runtime | 5.8882 s | 6.1706 s | +4.80% |
| Accepted cycles | 9 | 11 | +2 |

Score-only 找到更低 Area 與更高 Score，但用非常小的 `0.0122%` Score 增益交換
明顯的 TNS/NVP 退步。WNS 與 FF timing 保持相同。

### testcase4

| 指標 | Aggressive guarded v2 | Score-only v3 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0468 | -0.0479 | 絕對值 +2.35% |
| SS WNS | -0.0027 | -0.0027 | 相同 |
| Internal SS NVP | 58 | 59 | +1.72% |
| Checker SS NVP | 40 | 41 | +2.50% |
| Area | 5608.400 | 5608.331 | -0.069（-0.00123%） |
| Weighted score | 1.538533 | 1.538535 | 約 +0.000002（+0.00013%） |
| Strategy8 runtime | 151.743 s | 151.859 s | 幾乎相同 |
| Total runtime | 432.1149 s | 472.3351 s | +9.31%（前段 runtime 波動） |
| Accepted cycles | 0 | 1 | +1 |

被接受的是 guarded v2 第二輪曾拒絕的 candidate。它以 Area `-0.069` 換取
SS TNS `-0.0011` 與一條 internal/checker NVP 增加，總 Score 仍嚴格提高。

Score-only v3 報表：

- `report/strategy8/score_only_v3/testcase0/`
- `report/strategy8/score_only_v3/testcase4/`

## Guided-Beam v4 實驗

v4 保留 score-only acceptance，改變 candidate generation 與 recovery：

```text
每個 round：
  產生 4 個 candidates
  70% guided / 30% random targets
  每個 candidate 做 1 輪 Quick Recovery
  完整評估後選最高 Score candidate
  最佳 candidate 做最多 3 輪 Deep Recovery
  Quick 與 Deep checkpoint 保留較高 Score
```

### testcase0

| 指標 | Score-only v3 | Guided-Beam v4 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0110 | -0.0005 | 絕對值 -95.45% |
| SS WNS | -0.0031 | -0.0002 | 絕對值 -93.55% |
| Internal SS NVP | 11 | 4 | -63.64% |
| Checker SS NVP | 8 | 3 | -62.50% |
| Area | 118.302 | 121.781 | +2.941% |
| Weighted score | 0.964122 | 0.964703 | +0.000581（+0.0603%） |
| Total runtime | 6.1706 s | 8.4277 s | +36.58% |
| Search work | 40 single candidates | 20 rounds / 80 quick candidates | 2x candidates |
| Accepted rounds | 11 | 8 | — |

v4 用較大 Area 換到接近 timing closure 的結果，總 Score 仍高於 v3。第一次實作
曾只保留 Deep 終點，得到 Score `0.963694`；加入「Deep 不得覆蓋更好的 Quick
checkpoint」後提高至 `0.964703`。

### testcase4

| 指標 | Score-only v3 | Guided-Beam v4 | 變化 |
|---|---:|---:|---:|
| SS TNS/WNS/NVP | -0.0479 / -0.0027 / 59 | -0.0468 / -0.0027 / 58 | 恢復 Strategy7 終點 |
| Area | 5608.331 | 5608.400 | +0.069 |
| Weighted score | 1.538535 | 1.538533 | 約 -0.000002 |
| Strategy8 runtime | 151.859 s | 152.282 s | 幾乎相同 |
| Total runtime | 472.3351 s | 501.1184 s | +6.09%（前段波動） |
| Fully evaluated states | 3 | 8 quick candidates | +166.7% |
| Accepted rounds | 1 | 0 | -1 |

分級 recovery 在相同 local budget 內將完整候選評估數從 3 提高到 8，達到增加
嘗試數的目的；但兩個 round 的最佳 Score 分別只有 `1.538516` 與 `1.538433`，
沒有超越 current `1.538533`，所以全部 rollback。testcase4 最終輸出與
Strategy7 baseline 的 SHA-256 完全相同。

Guided-Beam v4 報表：

- `report/strategy8/guided_beam_v4/testcase0/`
- `report/strategy8/guided_beam_v4/testcase4/`

## Profiled Dual-Pool v5 實驗

v5 保留 v4 的 beam、Quick/Deep Recovery 與 score-only acceptance，只改四個 beam
slot 的候選生成策略：

```text
Candidate 0 = Timing profile
Candidate 1 = Area profile
Candidate 2 = Balanced profile
Candidate 3 = Random profile
```

`perturb_recover_enable_candidate_profiles` 是獨立開關；設為 `false` 時會回到 v4
四個候選共用 timing-guided pool 與等機率 operation 的行為。

### testcase0

| 指標 | Guided-Beam v4 | Profiled Dual-Pool v5 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0005 | -0.0038 | 絕對值 +660% |
| SS WNS | -0.0002 | -0.0013 | 絕對值 +550% |
| Internal SS NVP | 4 | 9 | +5 |
| Checker SS NVP | 3 | 6 | +3 |
| Area | 121.781 | 120.109 | -1.672（-1.373%） |
| Weighted score | 0.964703 | 0.964883 | +0.000180（+0.0187%） |
| Total runtime | 8.4277 s | 6.4448 s | -23.53% |
| Quick candidates | 80 | 80 | 相同 |
| Accepted rounds | 8 | 4 | -4 |

v5 找到較高 Score 與較低 Area，代價是 timing 不再像 v4 那樣接近完全 closure。
這不是 acceptance 漏洞：目前明確採 score-only，`alpha/beta/gamma =
0.40/0.30/0.30` 下 Area gain 足以補償這個 timing giveback。

### testcase4

| 指標 | Guided-Beam v4 | Profiled Dual-Pool v5 | 變化 |
|---|---:|---:|---:|
| SS TNS/WNS/Internal NVP | -0.0468 / -0.0027 / 58 | 相同 | 相同 |
| Checker SS NVP | 40 | 40 | 相同 |
| Area | 5608.400 | 5608.400 | 相同 |
| Weighted score | 1.538533 | 1.538533 | 相同 |
| Strategy8 runtime | 152.282 s | 153.828 s | +1.02% |
| Total runtime | 501.1184 s | 479.2936 s | -4.36%（前段波動） |
| Quick candidates | 8 | 9 | +12.5% |
| Accepted rounds | 0 | 0 | 相同 |

v5 完成兩個完整 rounds 與第三輪一個 Timing candidate。兩輪最佳 profile 分別是
Random 與 Area，Score 為 `1.538530`、`1.538415`；第三輪 incomplete Timing
candidate 為 `1.536503`。最接近者只比 current `1.538533` 低約 `0.000003`，
仍不符合嚴格改善，所以全部 rollback。最終輸出 SHA-256 與 v4 完全相同。

v5 報表：

- `report/strategy8/profiled_dual_pool_v5/testcase0/`
- `report/strategy8/profiled_dual_pool_v5/testcase4/`

## Reclaim-First Cycle Recovery v6 實驗（歷史預設）

v6 保留 v5 的候選 profiles、Quick Recovery 與 score-only acceptance，只替換
被選中候選的 Deep Recovery：

```text
Quick checkpoint
      ↓
Bootstrap Reclaim
  1,000 trials
  SS/FF TNS giveback = 0
  保護本輪 perturb inserts 不被立即刪除
      ↓
Repair Pulse
  最多 250 targets
  Guarded OR + depth split
  failure 時降低 scale 並暫時 blacklist targets
      ↓
Normal Pressure-Guided Reclaim
  最多 10,000 trials
  Repair gain × 20% giveback
      ↓
最多 3 cycles
      ↓
Quick / Cycle checkpoint 擇高分者
```

Pulse 與 Reclaim 的共用實作加入 bounded callback，因此 Strategy8 不受原 Stage
的 absolute 360 秒邊界影響；原 Repair/Reclaim Stage 仍傳入原本的時間條件，
前段 testcase0/4 結果與 v5 完全一致。

### testcase0

| 指標 | Profiled Dual-Pool v5 | Reclaim-First v6 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0038 | -0.0032 | 絕對值 -15.79% |
| SS WNS | -0.0013 | -0.0008 | 絕對值 -38.46% |
| Internal SS NVP | 9 | 9 | 相同 |
| Checker SS NVP | 6 | 6 | 相同 |
| Area | 120.109 | 120.236 | +0.127（+0.106%） |
| Weighted score | 0.964883 | 0.965490 | +0.000607（+0.0629%） |
| Total runtime | 6.4448 s | 5.7161 s | -11.31% |
| Quick candidates | 80 | 80 | 相同 |
| Accepted rounds | 4 | 8 | +4 |

Bootstrap Reclaim 共試 8,140 個 candidates、接受 52 個。Repair Pulse 做了 39
次 batch attempts，但沒有可接受 insertion，因此沒有進入 Normal Reclaim。
testcase0 的 v6 改善完全來自「零 TNS giveback 的高價值 reclaim」與 Quick
checkpoint 選擇，而不是 Repair Pulse。

### testcase4

| 指標 | Profiled Dual-Pool v5 | Reclaim-First v6 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0468 | -0.0421 | 絕對值 -10.04% |
| SS WNS | -0.0027 | -0.0023 | 絕對值 -14.81% |
| Internal SS NVP | 58 | 59 | +1 |
| Checker SS NVP | 40 | 40 | 相同 |
| Area | 5608.400 | 5609.770 | +1.370（+0.0244%） |
| Weighted score | 1.538533 | 1.538765 | +0.000231（約 +0.0150%） |
| Strategy8 runtime | 153.828 s | 153.306 s | -0.34% |
| Total runtime | 479.2936 s | 466.9761 s | -2.57%（含前段波動） |
| Quick candidates | 9 | 30 | +233.3% |
| Accepted rounds | 0 | 1 | +1 |

第一輪選到 Random profile。Cycle Recovery 接受 2 個 Pulse insertions 與 7 個
Normal Reclaim moves；最終 Score 從 `1.538533` 提高到 `1.538765`，所以正式
接受。Stage 全部工作共包含：

- Bootstrap Reclaim：7,000 tried / 2 accepted。
- Repair Pulse：14 batch attempts / 12 inserted buffers。
- Normal Reclaim：50,738 tried / 40 accepted。

後續 rounds 都沒有超過新的 current checkpoint，因此全部 rollback。v6 的最終
輸出與 v5 SHA-256 不同，代表確實找到新的結構。

v6 報表：

- `report/strategy8/reclaim_first_cycle_v6/testcase0/`
- `report/strategy8/reclaim_first_cycle_v6/testcase4/`

## Brutal Maximum-Area Path v7 實驗（歷史版本）

v7 依新的擾亂方向，完整取代 v6 的四候選 perturb 與 Quick Recovery：

```text
一輪只選 1 條最大累積 Buffer Area 的 Leaf-to-Root path
  → 刪除 path 上可安全移除的新 Buffer
  → 將 path 上原始 Buffer 隨機換成另一個合法 type
  → raw state 直接進完整 Reclaim-first Repair/Reclaim
  → recovery cycle 中持續保存最高 Score endpoint
  → 最終只以 legality + strict weighted-score gain 接受
```

Recovery 的 Bootstrap Reclaim、Repair Pulse target 與 Normal Reclaim candidate
數量皆不設上限，最多 200 recovery cycles，但仍由 150 秒 Strategy8 budget、
每次 Reclaim 120 秒 cap 與全域 validation reserve 限制。以下直接和被取代的 v6
比較。

### testcase0

| 指標 | Reclaim-First v6 | Brutal Path v7 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0032 | -0.2641 | 絕對值 +8153% |
| SS WNS | -0.0008 | -0.0070 | 絕對值 +775% |
| Internal SS NVP | 9 | 74 | +65 |
| Checker SS NVP | 6 | 73 | +67 |
| Area | 120.2362 | 105.4892 | -14.7470（-12.27%） |
| Weighted score | 0.965490 | 0.968735 | +0.003245（約 +0.336%） |
| Total runtime | 5.7161 s | 9.6467 s | +68.76% |
| Strategy8 runtime | — | 9.028 s | — |
| Distinct brutal paths | — | 20 | — |
| Accepted outer cycles | 8 | 2 | -6 |

v7 的 Stage 起點 Score 是 `0.960800`，最後 `0.968735`。20 條 path 中接受第 2
與第 12 輪。完整工作量：

- Bootstrap Reclaim：7,911 tried / 1,616 accepted。
- Repair Pulse：490 batch attempts / 6,557 inserted buffers。
- Normal Reclaim：150,658 tried / 7,752 accepted。

這證明暴力擾亂與完整 recovery 確實產生顯著結構改變：Area 比 v6 少 12.27%；
但由於 acceptance 明確是 score-only，演算法允許以明顯較差的 TNS/WNS 換 Area。

### testcase4

| 指標 | Reclaim-First v6 | Brutal Path v7 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0421 | -0.0468 | 絕對值 +11.16% |
| SS WNS | -0.0023 | -0.0027 | 絕對值 +17.39% |
| Internal SS NVP | 59 | 58 | -1 |
| Checker SS NVP | 40 | 40 | 相同 |
| Area | 5609.7704 | 5608.3998 | -1.3706 |
| Weighted score | 1.538765 | 1.538533 | -0.000232（約 -0.0151%） |
| Strategy8 runtime | 153.306 s | 152.898 s | -0.27% |
| Total runtime | 466.9761 s | 438.0085 s | -6.20%（含前段波動） |
| Fully recovered paths | 30 Quick candidates | 2 brutal paths | 搜尋單位不同 |
| Accepted outer cycles | 1 | 0 | -1 |

兩條 path 的詳細結果：

| Path leaf | Path nodes | Path Area | 直接破壞 | Recovery cycles | Recovery 最高 Score | Recovery endpoint dArea | 結果 |
|---|---:|---:|---|---:|---:|---:|---|
| `FF_29858` | 12 | 10.862 | 9 original resize、1 new delete | 44 | 1.515458 | -415.226 | rollback |
| `FF_26024` | 11 | 10.735 | 9 original resize | 21 | 1.488315 | -794.452 | rollback |

兩輪總工作量：

- Bootstrap Reclaim：40,433 tried / 9,589 accepted。
- Repair Pulse：161 batch attempts / 20,942 inserted buffers。
- Normal Reclaim：448,754 tried / 17,325 accepted。

雖然每輪 recovery 中都保存最佳 cycle endpoint，兩條 path 仍無法回到 current
Score `1.538533`，所以都正確 rollback。最終 tree 與 FinalAlt endpoint 的
SHA-256 相同，表示被拒絕的暴力修改沒有洩漏；也表示 v7 沒有保留 v6 曾找到的
`1.538765` 改善。

v7 報表：

- `report/strategy8/brutal_area_path_v7/testcase0/`
- `report/strategy8/brutal_area_path_v7/testcase4/`

## Brutal Delete-Only Path v8 實驗（目前預設）

v8 只改 brutal path mutation，Recovery、checkpoint 與 score-only acceptance
完全沿用 v7：

```text
選擇至少含一個可刪 Buffer 的最大累積 Area path
  → path 上通過 reclaim-delete legality 的 Buffer 全部刪除
  → 原始與其他不可刪 Buffer 完全不動
  → raw state 直接進完整 Reclaim-first Repair/Reclaim
```

這避免 v7 將 path 上 9 個原始 Buffer 全部隨機 resize，將擾亂集中在移除後段
Pipeline 已插入、但可能形成局部 minimum 的 Buffer。

### testcase0

| 指標 | Brutal Path v7 | Delete-Only v8 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.2641 | -0.0223 | 絕對值 -91.56% |
| SS WNS | -0.0070 | -0.0038 | 絕對值 -45.71% |
| Internal SS NVP | 74 | 15 | -59 |
| Checker SS NVP | 73 | 12 | -61 |
| Area | 105.4892 | 119.4871 | +13.9979（+13.27%） |
| Weighted score | 0.968735 | 0.961604 | -0.007131（-0.736%） |
| Total runtime | 9.6467 s | 2.7576 s | -71.41% |
| Accepted outer cycles | 2 | 3 | +1 |

v8 共嘗試 20 條 path，每條實際刪除一個 Buffer，接受 3 輪。Stage Score 從
`0.960800` 提高到 `0.961604`，Area 從 `121.365` 降到 `119.487`。因此 v8
仍有正收益，但不再採用 v7「犧牲大量 timing 換 Area」的極端解。

### testcase4

| 指標 | Brutal Path v7 | Delete-Only v8 | 變化 |
|---|---:|---:|---:|
| SS TNS | -0.0468 | -0.0464 | 絕對值 -0.85% |
| SS WNS | -0.0027 | -0.0023 | 絕對值 -14.81% |
| Internal SS NVP | 58 | 61 | +3 |
| Checker SS NVP | 40 | 43 | +3 |
| Area | 5608.3998 | 5607.6052 | -0.7946（-0.0142%） |
| Weighted score | 1.538533 | 1.538826 | +0.000293（約 +0.0190%） |
| Strategy8 runtime | 152.898 s | 153.317 s | +0.27% |
| Total runtime | 438.0085 s | 440.5007 s | +0.57% |
| Completed / accepted paths | 2 / 0 | 15 / 5 | +13 / +5 |

v8 的 15 個 perturb 全部只刪一個 Buffer，5 輪通過 strict Score acceptance。
最終 SS TNS/WNS 與 Area 都比 v7 好，只有 NVP 從 checker 40 增至 43。相較
Reclaim-First v6，v8 Score `1.538826` 也略高於 `1.538765`，Area 少
`2.1652`，WNS 同為 `-0.0023`；代價是 TNS 從 `-0.0421` 退到 `-0.0464`。

v8 工作量：

- testcase0：40 recovery cycles、21 Pulse insertions、80 retained reclaim
  moves，Strategy8 runtime `2.128` 秒。
- testcase4：36 recovery cycles、37 Pulse insertions、42 retained reclaim
  moves，Strategy8 runtime `153.317` 秒。

v8 報表：

- `report/strategy8/brutal_delete_only_v8/testcase0/`
- `report/strategy8/brutal_delete_only_v8/testcase4/`

## 驗證

兩個 After 輸出都通過 bundled checker：

- 0 floating
- 0 multi-drive
- 0 illegal input/buffer/FF/fanout
- 0 mismatched node
- 0 illegal timing path

Delete-Only v8 testcase0 的 checker 結果為 SS TNS/WNS
`-0.0223/-0.0038`、NVP `12`、Area `119.4871`；internal report 的 NVP 是
`15`。testcase4 checker 為 SS TNS/WNS `-0.0464/-0.0023`、NVP `43`、Area
`5607.6052`；internal report NVP 是 `61`。兩者 legality 全部通過。

原始 testcase0/4 的 `buf.lib` 將最後一個 cell 寫成
`cell(REALBUF_X16)`，bundled checker 無法解析；驗證使用語意完全相同、只補上
空格的 `testcase0_v2/buf.lib` 與 `testcase4_v2/buf.lib`。Optimizer 本身可解析
兩種格式，未修改原始 testcase。

一般 `-Wall -Wextra -Wpedantic` build 無警告，ASan/UBSan testcase0 也通過。
NVP 差異仍是 Strategy7 已記錄的 near-zero slack 分類問題；TNS/WNS、Area 與
legality 一致。

## Phase0 後完整 Pipeline Portfolio 實驗

為比較「從 Phase0 checkpoint 完整複製剩餘 Pipeline」與「在同一個
Repair/Reclaim 中交錯兩種 policy」，Strategy8 新增一個預設關閉的實驗模式。
三個分支都從完全相同的 Phase0 tree 開始，並各自完整執行
Repair/Reclaim、FinalAlt、Strategy8 與 final validation：

```text
Phase0 checkpoint
  ├─ full_heuristic
  │    heuristic Repair/Reclaim → FinalAlt → Strategy8
  ├─ full_score
  │    score-based Repair/Reclaim → FinalAlt → Strategy8
  └─ alternating_delete
       偶數 cycle：heuristic
       奇數 cycle：先刪除最多 2 個上一輪新插入且仍可合法刪除的高 Area Buffer，
                   再使用 score acceptance
       → FinalAlt → Strategy8
  ↓
合法分支中選 weighted score 最高者
```

實驗時將 global 與 Repair/Reclaim boundary 提高為 86,400 秒安全上限，並停用
per-cycle Reclaim、FinalAlt 與 Strategy8 local cap；對本次實驗等同不受正常
wall-clock 限制。`max_cycles`、split-depth、no-progress 與 candidate
exhaustion 等演算法停止條件仍保留。所有分支都沿用最原始輸入的 baseline
計算 score，避免用 Phase0 狀態重新正規化造成分數不可比較。

### testcase0

| 分支 | Runtime (s) | Score | SS TNS/WNS/NVP | Area | RR stop |
|---|---:|---:|---|---:|---|
| `full_heuristic` | 4.634 | 0.961604 | -0.0223 / -0.0038 / 15 | 119.487 | `no_inserted_buffers` |
| `full_score` | 5.093 | 0.960839 | -0.5971 / -0.0140 / 101 | 98.744 | `split_depth_exhausted_score` |
| `alternating_delete` | 5.694 | **0.964620** | -0.0544 / -0.0039 / 25 | 116.195 | `no_inserted_buffers` |

完整 fork 的 heuristic/score 二選一由 heuristic 勝出；加入使用者提出的
交錯分支後，`alternating_delete` 以較少 Area 得到最高 weighted score，但
timing 比 heuristic 分支差。三分支 sequential 實驗耗時 15.446 秒。

### testcase4

| 分支 | Runtime (s) | Score | SS TNS/WNS/NVP | Area | RR stop |
|---|---:|---:|---|---:|---|
| `full_heuristic` | 799.022 | 1.538829 | **-0.0479 / -0.0023 / 63** | 5607.478 | `no_inserted_buffers` |
| `full_score` | 662.698 | 1.570968 | -12.7670 / -0.0100 / 2853 | 4217.672 | `split_depth_exhausted_score` |
| `alternating_delete` | 1117.899 | **1.570972** | -15.2960 / -0.0105 / 3198 | **4190.106** | `max_cycles` |

三分支 sequential runtime 為 2581.862 秒。依目前
`alpha/beta/gamma = 0.40/0.30/0.30`，完整 fork 由 `full_score` 勝出，
三者總選擇則是 `alternating_delete`；但它只比 `full_score` 多
`0.000004` score，代價是更差的 TNS/WNS/NVP 與多 455.201 秒。

相較目前有正常時間限制的 Delete-Only v8 testcase4
（score `1.538826`、SS `-0.0464/-0.0023/61`、Area `5607.605`、
total runtime `440.501` 秒），無限制 heuristic 只增加約 `0.000003`
score，卻多花約 358.5 秒。移除時間限制對 heuristic 幾乎沒有實質收益。
score 與 alternating 的高分則主要來自約 25% 的額外 Area 降幅，不是更好的
timing closure。

### 結論

- 完整 fork 適合作為離線消融工具，不適合直接放進 570 秒正式 Pipeline。
- 若真正目標就是目前假設的 weighted score，`full_score` 已幾乎達到
  alternating 的結果，後者額外複雜度與 runtime 不划算。
- 若 timing closure 比假設權重更重要，`full_heuristic` 明顯較安全；不應只靠
  weighted score 自動選出 timing 已大幅退步的分支。
- 正式預設因此仍維持單一路徑 Delete-Only v8；portfolio 與 alternating
  功能保留為可選實驗，兩者預設關閉。

完整逐輪資料保存在：

- `report/strategy8/post_phase0_portfolio_unlimited/testcase0/timing_report_testcase0.txt`
- `report/strategy8/post_phase0_portfolio_unlimited/testcase4/timing_report_testcase4.txt`

報表在最上方的 Stage Contribution Summary 後新增 Portfolio 區塊；每個分支逐
Repair/Reclaim cycle 記錄 policy、pre-delete 數量／Area、Pulse insertion、
Reclaim tried/accepted、cycle start／perturb／pulse／end score、兩 corner
timing、Area 與 stop reason。

## 目前結論與下一步

Strategy8 的基礎設施已完成：

- 可重現 perturbation
- reusable bounded recovery
- current/best checkpoints
- rejected-state rollback
- 時間與 validation reserve
- stage/cycle 級 timing report

Brutal Delete-Only Path v8 現在是預設 search policy，Score-only v3 acceptance
保持不變。它保留 v7「不做 Quick 小擾亂、直接跑完整 Repair/Reclaim」的架構，
但不可刪 Buffer 完全不動。

這項限制解決了 v7 的主要問題：

- testcase0 不再出現極端 timing tradeoff；TNS 絕對值比 v7 減少 91.56%，
  runtime 也減少 71.41%。
- testcase4 從 2 條全部 rollback，提升到 15 條中接受 5 條；Score、TNS、WNS
  與 Area 都優於 v7。
- testcase4 Score `1.538826` 略高於 v6 `1.538765`，代表目前 v8 是三者中
  weighted objective 最好的大型案例結果；但 v6 的 TNS 仍較好。

下一步若要擴大 delete-only perturb，可以考慮一次選兩條互不重疊的高 Area
path，或刪除後先跑一輪零 giveback Reclaim 再進 Pulse；不建議立刻重新開啟
原始 Buffer 隨機 resize，因為 v7 已顯示該動作會讓大型案例 recovery 距離過大。
若未來加入 bad-step acceptance，仍應保留目前兩層 best checkpoint 作最後輸出
保護。
