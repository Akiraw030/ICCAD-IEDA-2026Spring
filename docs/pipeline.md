# Strategy8 目前 Pipeline

這份文件先記錄目前實際使用的 Strategy8 Pipeline，後段另保留具明確標示的
實驗結果；歷史數據不代表目前預設參數。

當文件與程式不一致時，以以下順序為準：

```text
src/optimizer.h、src/optimizer.cpp
    > docs/pipeline.md
    > docs/strategy8.md
    > docs/strategy7.md
```

所有主要策略參數集中在 `src/optimizer.h` 的 `OptimizerConfig`。

目前建置也固定使用唯一的 `build/` 目錄。`run_reports.sh` 每次會明確以
`Release`（`-O3 -DNDEBUG`）和 `ENABLE_OUTPUT=ON` 重新 configure 同一個目錄，
因此每個 batch report 都會產生 timing report，且不會建立額外 build 目錄。

## 程式同步狀態

最後依 `src/optimizer.h`、`src/optimizer.cpp`、`CMakeLists.txt` 與
`run_reports.sh` 核對：2026-08-28（Strategy8 提交前版本）。

目前 Repair/Reclaim 的關鍵狀態為：

```text
Global objective              = alpha/beta/gamma = 0.20 / 0.20 / 0.60
Global optimizer limit        = 570 seconds
Phase0                        = On；5 passes、最多 6 types/node、180 s safety cap
Phase0 acceptance             = Score-based
Repair/Reclaim                = Score-based；absolute end 360 s、最多 200 cycles
Per-cycle Reclaim budget      = 120 seconds
Repair split/failure recovery = depth 2 / On；最多 3 failures、scale ×0.70、最低 0.50
ExistingBufferShared/Reclaim  = On；與 Repair/Reclaim 交替；large case 略過
FinalAlt                      = On；score acceptance Off；target checkpoint On
Strategy8 Perturb/Recover     = Off（E23/E24 沒有 accepted recovery，僅耗時）
Post-Phase0 portfolio         = On；先 full_score，再依實際剩餘時間啟動 route
Portfolio candidate routes    = full_heuristic、cosine_heuristic、post/pre-Phase0 5%/10% perturb
Deadline-filling restarts     = On；post/pre-Phase0 5%/10%/20%，每次使用新 seed
Portfolio selector            = legality first，再以 0.64 / 0.18 / 0.18 target score
Portfolio max routes/reserve  = 不限次數，由 deadline 控制；small/medium/large 保留 10 / 30 / 60 s
Large-case memory guard       = 250k+ paths 不保留 pre-Phase0 tree clone
Global score scheduler        = Off；只在 cosine_heuristic route 中開啟
```

### 本次一致性驗證

2026-07-30 以目前 Brutal Delete-Only Path v8 預設重新執行 testcase0 與
testcase4：

- 一般 build 與 `-Wall -Wextra -Wpedantic` build 通過。
- ASan/UBSan 執行通過；執行環境不支援 LeakSanitizer，因此 leak scan 未執行。
- 兩個輸出皆通過 bundled checker：0 floating、0 multi-drive、0 illegal
  buffer/FF/fanout/path。
- Checker 使用語意相同、只修正 `cell (REALBUF_X16)` 空格的
  `testcase0_v2/testcase4_v2` library；原始 library 的該行無空格，checker
  自己無法解析。
- testcase0 最終 checker SS TNS/WNS/NVP：
  `-0.0223 / -0.0038 / 12`；Area：`119.4871`。
- testcase4 最終 checker SS TNS/WNS/NVP：
  `-0.0464 / -0.0023 / 43`；Area：`5607.6052`。

目前驗證報表：

- `report/strategy8/brutal_delete_only_v8/testcase0/timing_report_testcase0.txt`
- `report/strategy8/brutal_delete_only_v8/testcase4/timing_report_testcase4.txt`

---

# Pipeline 快速總覽

```text
讀入資料／建立 Clock Tree → Baseline Timing + Area
        ↓
Phase0：pressure-guided original-buffer resize
        ↓
儲存 shared Phase0 checkpoint（large case 僅保留這一份）
        ↓
必跑保底：full_score remaining Pipeline
        ↓
尚有安全餘裕？ ── No → 選 full_score → Final validation / output
        │
       Yes
        ↓
依序嘗試 full_heuristic、cosine_heuristic（從 original tree 重跑 Phase0）、
post-Phase0 5%/10% perturb、pre-Phase0 5%/10% perturb
        ↓
仍有安全餘裕？ ── Yes → 輪替 post/pre-Phase0 5%/10%/20% perturb、
新 seed、score/heuristic acceptance 與 cosine schedule，直到 safe deadline
        ↓
legality first + 0.64/0.18/0.18 target-score selector
        ↓
完整 Timing／Area／Legality 驗證 → 輸出 Clock Tree（OUTPUT=ON 時另輸出 report）
```

---

# Pipeline 詳細流程

## 0. Baseline、Score 與 Checkpoint 初始化

### 這個階段會做什麼

1. 對輸入 tree 做完整 SS/FF timing analysis。
2. 計算初始 area。
3. 將這組 timing 與 area 保存為不可變 normalization baseline。
4. 若 `enable_best_checkpoint` 開啟，以輸入 tree 初始化 best-state checkpoint；目前預設關閉。
5. 建立 FastTimingEngine 與其他 cache。

後面所有 score 都使用同一個 baseline，不能在 Pipeline 中途更換。

---

## 1. 統一的 Weighted Score

### 預設係數

```text
alpha = 0.20  → SS timing
beta  = 0.20  → FF timing
gamma = 0.60  → Area
```

### Score 公式

```text
SS component
  = normalized SS TNS improvement
  + normalized SS WNS improvement

FF component
  = normalized FF TNS improvement
  + normalized FF WNS improvement

Area component
  = normalized Area improvement

Score
  = alpha × SS component
  + beta  × FF component
  + gamma × Area component
```

Score 越大越好。

### Score 模式取代哪些判斷

Score acceptance 開啟時，它只取代「這次改動有沒有使結果惡化」的 timing-only 判斷。

以下仍然是硬條件：

- Tree mutation 必須成功。
- Buffer type 必須存在。
- Fanout 必須合法。
- 不得製造 illegal tree。
- 不得超過時間或工作量限制。
- Reclaim 預設仍只產生 area-saving candidates。

### 各階段的獨立開關

```cpp
enable_phase0_score_based_acceptance = true;
enable_repair_reclaim_score_based_acceptance = true;
enable_final_alt_score_based_acceptance = false;
enable_best_checkpoint = false;
```

目前 Phase 0 與正式 Repair/Reclaim 都開啟 score acceptance；Strategy8
Recovery 的獨立 score acceptance 也開啟。FinalAlt 與全域 checkpoint 仍預設
關閉。Alpha、beta、gamma 會直接影響這些 score-based acceptance。

### 開啟後的 Score 接受條件

| Stage / Operation | 對應開關開啟時的接受條件 |
|---|---|
| Phase 0 resize | Score 必須嚴格提高 |
| Phase 0 batch | 整批 Score 必須嚴格提高 |
| Repair Pulse | 整批 Score 不得變差 |
| Repair/Reclaim resize/delete | 相對目前狀態 Score 不得變差 |
| FinalAlt repair insertion | Score 必須嚴格提高 |
| FinalAlt reclaim resize/delete | 相對目前狀態 Score 不得變差 |

浮點比較使用：

```text
score_acceptance_epsilon = 1e-9
```

目前 Phase 0 與正式 Repair/Reclaim 都使用 weighted-score acceptance；
FinalAlt 保留原本 heuristic acceptance。Strategy8 Recovery 另有自己的 score
switch，目前同樣開啟。

---

## 2. Phase 0（預設 Score-guided）

### 目的

仍然只 resize 原始 buffer，不插入或刪除 buffer。

### 目前模式：Estimated-value 排序 + Individual FastTiming Trial

目前 Phase 0 預設不使用 batch，而是依 estimated value 排序後逐一嘗試 node。
Estimated value **只決定嘗試順序**，不代表該 resize 一定會採用；真正是否採用，
由目前預設的 weighted-score acceptance 決定。

### 每個 Pass 的流程

1. 計算 timing pressure。
2. 對每個 node 掃描合法且方向正確的 type，取得最佳可行 estimated value。
3. 依 estimated value 排序 buffer nodes。
4. 每個 node 最多實測排名前 6 個 type。
5. 對每個 type 執行 FastTimingEngine trial，記錄試跑結果後立即 rollback。
6. 以 weighted score 篩選可接受候選；只有關閉 score mode 時才使用 Pareto
   fallback。
7. 從可接受候選中選出最佳 type，再重新執行一次 trial。
8. 第二次 trial 成功後才 commit 到 Clock Tree，並更新目前 timing、area 與 score。

目前預設的 node/type 排序使用：

```text
estimated value
  = |pressure| × |average SS/FF delay delta|
  - phase0_area_tiebreak_penalty × max(0, Area delta)
```

其中 `average SS/FF delay delta = (SS delta + FF delta) / 2`，
`phase0_area_tiebreak_penalty = 0.01`。Area 增加會受罰，Area 減少不會額外加分。
Best achievable type 必須支援目前 fanout，且 SS/FF delay 變化方向符合 pressure。
這個排序公式不使用 alpha/beta/gamma；係數仍會用於 trial 後的 Score-only
acceptance。

Score-aligned ranking 實驗目前已關閉：

```cpp
enable_phase0_score_aligned_ranking = false;
```

若重新開啟，才會改用 alpha/beta/gamma 加權且經 baseline normalization 的
estimated score gain。相關 A/B 結果保留在本文件後段。

Estimated value 相同時，node 依 `|pressure|` 與 node name 排序；同一 node 內的
type 則依序偏好：

1. Area delta 較小。
2. Delay 改動幅度較小。
3. 與目前 type rank 距離較近。
4. Type name 較小。

### Individual Mode：Trial、Rollback 與採用規定

對排名前 6 個 type，Phase 0 會依序執行以下動作：

1. 先檢查 type 是否存在、fanout 是否受支援，以及 resize 的 local legality。
2. 使用 `FastTimingEngine::trial_resize()` 計算試跑後的 SS/FF timing 與 area。
3. 保存 trial timing cost、weighted score 與 area。
4. **不論這個候選是否符合接受條件，都先呼叫 `rollback()`。**
5. 所有候選比較完後，才選出其中最佳且可接受的 type。

因此候選比較期間，正式 Clock Tree 不會保留任何暫時 resize；前一個 type 的
試跑結果也不會污染下一個 type。

#### Score 關閉時的 Fallback：WNS/TNS/Area Pareto Guard

此條件目前保留為可切換的對照模式；只有在：

```cpp
enable_phase0_score_based_acceptance = false;
```

所以候選的 SS/FF WNS、SS/FF TNS 與 Area 必須全部不變差：

```text
trial SS WNS >= current SS WNS - phase0_wns_tolerance
trial SS TNS >= current SS TNS - phase0_tns_tolerance
trial FF WNS >= current FF WNS - phase0_wns_tolerance
trial FF TNS >= current FF TNS - phase0_tns_tolerance
trial Area   <= current Area   + area_comparison_epsilon
```

此外，五項指標中至少一項必須超過 numerical tolerance 地嚴格改善。若任何一項
變差，或五項都沒有實質改善，該 trial 都不保留並 rollback。

目前 numerical tolerance：

```text
phase0_wns_tolerance = 1e-9
phase0_tns_tolerance = 1e-9
area_comparison_epsilon = 1e-9
```

Normalized timing cost 不再決定候選是否合格；它只用來在多個已通過 Pareto
guard 的 type 之間挑選最佳候選。這代表 Area 改善不能補償任何 WNS/TNS
惡化，反過來 timing 改善也不能補償 Area 增加。

#### 目前預設採用條件：Weighted Score

目前已開啟：

```text
enable_phase0_score_based_acceptance = true
```

單一 resize 必須：

```text
trial score > current score + score_acceptance_epsilon
```

目前 `score_acceptance_epsilon = 1e-9`。這個模式會以
`alpha=0.20, beta=0.20, gamma=0.60` 的總分取代上述 WNS guard 與 timing-cost
改善條件；因此個別 timing 指標是否允許小幅退步，會由總分結果共同決定。

#### 從可接受候選中選出最佳 Type

- Score mode 開啟：選 weighted score 最高者。
- Score mode 關閉：先要求全部 WNS/TNS/Area 不變差，再選 normalized timing
  cost 最低者。
- 主指標平手：依序選 area 較小、delay 改動幅度較小、type rank 距離較近者。

選出最佳 type 後，FastTimingEngine 會對它再執行一次 trial；只有第二次 trial
與 `commit()` 都成功，才會真正修改 Clock Tree。若第二次 trial 或 commit
失敗，該 node 不作修改，current timing、area 與 score 也不更新。

每個不符合接受條件的 **type trial** 都會增加一次
`consecutive_rejects`；一個 node 成功 commit 後才歸零。Phase 0 的 rollback
是候選 transaction 層級的還原，不是把整個 Phase 0 回復到進入階段前的狀態：
先前已 commit 的 resize 會保留。

### 可選的 Batch Mode：整批採用與 Rollback

`enable_phase0_batch_auto` 目前預設為 `false`，因為 testcase4 實測顯示 individual FastTimingEngine 已經足夠快，而且 timing 結果較好。

若手動開啟 automatic batch，啟用門檻為：

- SS violations ≥ 15,000，或
- Pressure candidates ≥ 10,000。

目前可選 batch 實作：

- Batch size = 12。
- 整批 resize 使用一次 FastTimingEngine multi-resize transaction。
- 使用 TreeIndex 的 preorder/subtree interval，以 `O(1)` 判斷兩個 node 是否有祖先關係。
- Batch 失敗可以遞迴 split，但最多 split 2 層。
- Split 後左右兩半都會嘗試，不再因左半成功而跳過右半。
- Fast batch 可以用 `enable_phase0_fast_batch_timing` 獨立控制；關閉時使用 incremental fallback。

Batch 中每個 node 只取 type 排名第一名，並且同一批 node 之間不可互為祖先。
整批 trial 使用與 individual mode 相同的階段接受規則：

- Score mode 關閉：整批 SS/FF WNS、SS/FF TNS 與 Area 全部不得變差，且至少
  一項必須有實質改善。
- Score mode 開啟：整批 weighted score 必須嚴格提高。

整批符合條件且 commit 成功時，所有 resize 一起採用。若 trial 失敗、接受條件
不成立或 commit 失敗，FastTimingEngine 會 rollback 整批 transaction，不會只
留下其中一部分。

Timing acceptance 失敗且 split 功能開啟時，會先 rollback 原批次，再把它切成
左右兩半，各自重新 trial 並獨立決定採用或 rollback；到達 split depth 2 後不再
繼續切分。若 fast batch 關閉而使用 incremental fallback，拒絕時也會還原
buffer types、arrival cache、affected path groups 與 timing cache。

### Phase 0 結束條件

| Stop reason | 目前數值 | 意義 |
|---|---:|---|
| `candidate_budget` | 關閉（0） | 不限制跨 Pass 累積掃描的 node 數 |
| `time_budget` | 180 秒 | Phase 0 local time 到期 |
| `global_time_budget` | 570 秒 | 整體時間到期 |
| `consecutive_rejects` | 1,000 | 個別模式連續被拒絕的 type trial |
| `consecutive_failed_batches` | 300 | Batch 模式連續失敗 |
| `candidates_exhausted` | 一個 pass | 整個 pass 沒有接受修改 |
| `max_passes` | 5 | 完成最大 pass 數 |

目前 `phase0_node_fraction = 1.0` 且 candidate budget 關閉，因此每個 Pass
原則上會掃描全部非零 pressure node；仍可能因連續 1,000 次 type trial
被拒絕、180 秒 local time 或 570 秒 global time 而提前停止。

### Phase 0 結束後

- 執行完整 snapshot。
- 計算該狀態 score。
- Checkpoint 開啟時，如果高於 checkpoint，就用目前 tree 更新 checkpoint。
- 進入 adaptive parameter selection。

---

## 3. Adaptive Parameter Selection

這一階段本身沒有變更。

### 分類依據

- Baseline SS violations。
- Phase 0 pressure candidates。
- Phase 0 是否使用 batch。

### Delay Scale

| Case | Pulse delay scale |
|---|---:|
| Small | 0.5 |
| Medium | 1.0 |
| Large | 1.5 |

這是一次性判斷，沒有獨立停止條件。

---

## 4. Repair/Reclaim Cycles（可選 Score-guided）

### 每個 Cycle

```text
完整 Timing 分析
      ↓
Repair Pulse（預設 score-based）
      ↓
Reclaim（預設 area/pressure heuristic 排序 + score acceptance）
      ↓
依設定更新 Best Checkpoint
      ↓
檢查 No-Progress 與 Stage Stop Conditions
```

---

### 4.1 Repair Pulse

#### Target 選擇方式

目前 Target 選擇方式：

- Setup violation → capture FF。
- Hold violation → launch FF。
- 依最差 slack 排序。
- 同一批避免重複使用相同 launch/capture FF。
- 依 violation 與 adaptive scale 選 buffer。

#### 目前預設：Score-based

目前正式設定為：

```cpp
enable_repair_reclaim_score_based_acceptance = true;
repair_pulse_require_both_tns_not_worse = false;
```

因此 Repair Pulse 整批 weighted score 不得變差；Reclaim resize/delete 也必須
在 Area 降低的同時讓 weighted score 不變差。

若日後關閉 score acceptance，才會使用以下 TNS-only Guarded OR fallback：

```text
若 before FF TNS 已在 ±1e-9 內：
    after FF TNS 不變差
    且 after SS TNS 至少嚴格改善 1e-9
否則：
    after SS TNS 不變差
    或 after FF TNS 不變差
```

Guarded OR 避免 FF 已關閉為 0 時，單靠「FF 不變差」讓任意 SS 退步都自動
通過，但目前不作為正式接受條件。

若將 `repair_pulse_require_both_tns_not_worse` 改為 `true`，則切回較保守的
AND：after SS TNS 與 after FF TNS 都必須不變差。

#### Score 開啟時的接受條件

若改為開啟 Repair/Reclaim 共用 score switch，整批插入完成後計算：

```text
before pulse score
after pulse score
```

共用模式沿用較寬鬆的：

```text
after pulse score >= before pulse score - 1e-9
```

才接受整批插入。

否則：

- 刪除這個 pulse 新插入的所有 buffer。
- 回復 FastTimingEngine。
- Stop reason 設為 `batch_worsened_score`。

#### 失敗 Batch 的深度式拆分

目前：

```cpp
repair_pulse_split_on_fail = true;
repair_pulse_max_split_depth = 2;
```

若 batch 不符合目前的 TNS 或 score 接受條件：

1. Rollback 該 batch 插入的所有 buffer。
2. 將 target list 左右對半切分。
3. 左右子批依序各自 trial，並相對當下 tree 獨立接受或 rollback。
4. 子批仍失敗時繼續對半切，直到 depth 2。
5. 到達 depth 2 的失敗分支才正式放棄。

只要任一子批成功，成功的 buffer 會保留，Repair/Reclaim Stage 會繼續；其他失敗
分支不會拖累已成功的子批。所有分支均失敗時會得到
`split_depth_exhausted_timing`（或 score mode 的
`split_depth_exhausted_score`）。目前 failure recovery 預設開啟，因此不會因
第一次 exhaustion 立即結束 Stage，而會進入下一節的 recovery。若拆分途中到達
時間限制，整個 Pulse 已接受的子批也會一起 rollback，維持原本的時間中止語意。

#### 目前預設開啟的 Repair Pulse Failure Recovery

目前開關與參數：

```cpp
enable_repair_pulse_failure_recovery = true;
repair_pulse_max_consecutive_failures = 3;
repair_pulse_recovery_scale_multiplier = 0.70;
repair_pulse_recovery_min_delay_scale = 0.50;
repair_pulse_recovery_blacklist_targets = 8;
```

Depth split 全部失敗時不會立即結束 Stage：

1. 失敗 Pulse 已先完整 rollback。
2. 將當批最差的前 8 個 targets 加入暫時 blacklist。
3. 下一次 Pulse 的 delay scale 乘上 0.70，但不低於 0.50。
4. 重新執行完整 timing analysis 並建立新的 target batch。
5. Recovery Pulse 成功後清空 blacklist，delay scale 恢復 adaptive baseline。
6. 連續 3 次可恢復失敗後才結束；`no_batch_targets`、`no_inserted_buffers`
   與時間限制仍直接停止。

此功能不更動 Guarded OR、目前的 depth-2 split 或 Reclaim heuristic。

#### Post-Phase0 Existing-Buffer Shared/Reclaim Cycle

原本內嵌於 Repair/Reclaim Pulse 的 pressure-guided sibling grouping 已預設關閉，
改為 Phase0 後獨立執行一次。此 cycle 不做 endpoint repair，只做：

```text
Pressure-guided shared insertion
  original Buffer parent -> original Buffer 或 FF children
        ↓
Pressure-guided Reclaim
```

目前設定：

```cpp
enable_shared_path_repair = false;
enable_phase0_existing_buffer_shared_reclaim_cycle = true;
enable_existing_buffer_shared_reclaim_alternation = true;
existing_shared_children_must_be_buffers = false;
existing_shared_disable_alternation_if_initial_no_insertion = true;
existing_shared_max_consecutive_no_insertion_cycles = 2;
```

Phase0 後先執行一個 Existing-Buffer Shared/Reclaim，接著執行一個普通
Repair/Reclaim cycle；之後持續交替，直到原本 Repair/Reclaim 的 360 秒絕對邊界、
global deadline、no-progress 或其他 stop condition。每一個 Existing-Buffer
Shared/Reclaim 自己共用 `cycle_reclaim_time_budget_seconds`（目前 120 秒）：shared
candidate generation 與 Reclaim 合計不能超過此值。只有至少一個 shared insertion
成功後才會進入 Reclaim；若 shared insertion 全數拒絕，該子 cycle 不改變 tree。
timing report 會保留 initial stage snapshot，並在獨立 section 列出每輪交替的 history。

若第 0 輪 Existing shared 沒有 accepted insertion，後續交替會立即停用，因為普通
Repair/Reclaim 插入的新 buffer 不會增加 original-to-original 的合法 edge。若第 0
輪曾成功，後續則允許繼續交替，但連續 2 個 Existing cycle 無 insertion 後停用，
避免大型 case 重複支付全樹 pressure/candidate generation 成本。普通
Repair/Reclaim 不受影響，會繼續使用剩餘 stage 時間。

`repair_shared_max_consecutive_no_insertion_pulses` 保留為未來重新開啟內嵌
shared-path Repair 時使用；目前不會影響這個一次性的 post-Phase0 cycle。

若要回到最嚴格的「只允許原始 Buffer-to-Buffer」實驗，將
`existing_shared_children_must_be_buffers` 改為 `true`。目前預設允許原始 FF
children，仍不改變任兩個既有元件之間的 ancestor/descendant 關係。

#### Repair Pulse 結束原因

| Stop reason | 意義 |
|---|---|
| `timing_closed` | SS、FF TNS 都是 0 |
| `no_batch_targets` | 找不到 repair targets |
| `no_inserted_buffers` | 無法合法插入 buffer |
| `batch_worsened_score` | 拆批關閉時，整批 weighted score 變差 |
| `batch_worsened_timing` | 拆批關閉時，整批 SS 或 FF TNS 變差 |
| `split_depth_exhausted_score` | Score 模式拆到最大深度仍全部失敗；Recovery 開啟時先重試 |
| `split_depth_exhausted_timing` | Timing 模式拆到最大深度仍全部失敗；Recovery 開啟時先重試 |
| `time_limit` | Global time 到期 |
| `cycle_time_window` | Repair/Reclaim 絕對時間邊界到期 |

拆批開啟後，第一次 `batch_worsened_score` 或 `batch_worsened_timing` 不再直接
結束 Stage，而是先進入 bounded split retry。

---

### 4.2 Reclaim（可選 Score-guided）

#### 候選操作

- Resize 原始 buffer。
- Resize `NEW_BUF`。
- Delete `NEW_BUF`。

#### 候選排序

目前不論 Score 是否開啟，都使用原本的 heuristic：

```text
area saving × reclaim_rank_area_weight
+ helpful pressure effect × reclaim_rank_timing_help_weight
- harmful pressure effect × reclaim_rank_timing_harm_weight
+ NEW_BUF delete bonus
```

預設：

| Ranking parameter | 數值 |
|---|---:|
| Area weight | 100.0 |
| Timing help weight | 1.0 |
| Timing harm weight | 5.0 |
| Delete bonus | 25.0 |

候選排序不直接使用 alpha/beta/gamma；開啟 score acceptance 時，只有
候選接受條件使用 weighted score。

#### Score 開啟時的接受條件

- Candidate 必須降低 Area。
- Trial score 不得低於執行該 move 前的 current score。

也就是：

```text
trial score >= previous score - 1e-9
```

當 score acceptance 開啟時，舊的 20% TNS giveback、WNS guard 與
violation-count guard 不再作為主要接受依據；它們只在關閉 score acceptance
時使用。

#### 單次 Reclaim Scan 停止條件

| 條件 | 目前數值 |
|---|---:|
| Candidates 全部試完 | 無固定數量 |
| Absolute Repair/Reclaim boundary | 360 秒 |
| Per-cycle reclaim safety budget | 120 秒 |
| Global time | 570 秒 |
| 連續 rejected candidates | 3,000 |
| 最大 trials per cycle | 0，代表不限制 |

這些通常只結束該次 reclaim scan。

---

### 4.3 整個 Repair/Reclaim Stage 結束條件

| Stop reason | 目前數值 | 意義 |
|---|---:|---|
| `timing_closed` | SS/FF TNS = 0 | Timing 已完全關閉 |
| `global_time_budget` | 570 秒 | Global time 到期 |
| `cycle_time_window` | 360 秒 | 從 optimizer 開始計算的絕對時間 |
| `max_cycles` | 200 | 完成最大 cycles |
| `no_progress_streak` | 100 cycles | 連續 cycles 幾乎沒有 timing/area 進展 |
| `batch_worsened_score` | — | 拆批關閉時，Repair Pulse score 變差 |
| `batch_worsened_timing` | — | 拆批關閉時，Repair Pulse 使 SS 或 FF TNS 變差 |
| `split_depth_exhausted_score` | Depth 2 | 所有 score-mode 子批均失敗；最多連續 recovery 3 次 |
| `split_depth_exhausted_timing` | Depth 2 | 所有 timing-mode 子批均失敗；最多連續 recovery 3 次 |
| `no_batch_targets` | — | 找不到修復目標 |
| `no_inserted_buffers` | — | 無法插入 buffer |
| `not_run` | — | 沒有實際執行 cycle |

Checkpoint 開啟時，每個成功完成的 cycle 都會比較並更新 best checkpoint。

重要：`360 秒`是從 optimizer 開始計算的絕對時間點，不是進入
Repair/Reclaim 後重新計時。單次 Reclaim 另受 120 秒相對上限限制。目前
Phase 0 預設不用 batch，在最新 FinalAlt 實驗中約為 15～16 秒，因此
Repair/Reclaim 通常實際可用約 344 秒，仍需扣除其他前置成本。

---

## 5. Final Alternating Greedy（可選 Score-guided）

### 每個 Iteration

```text
檢查是否仍需要 Repair
      ↓ Yes
最多進行 1,000 次 Repair Insertion 嘗試
      ↓
Reclaim，最多保留 50,000 個 Candidates
      ↓
依設定更新 Best Checkpoint
      ↓
檢查是否仍有 Meaningful Progress
```

### Repair 判定

目前 threshold 仍然全部為 0：

- SS TNS < 0。
- SS WNS < 0。
- SS violations > 0。
- FF TNS < 0。
- FF violations > 0。

### Score 開啟時的 Repair Insertion 接受條件

目前改為：

```text
trial score > previous score + 1e-9
```

插入還是必須合法且 FastTimingEngine transaction 必須成功。

目前使用有上限的 cooldown blacklist：

- `final_alt_blacklist_cooldown_iters > 0`：經過指定 outer iterations 後重試。
- `final_alt_max_failures_per_target`：累積到指定失敗次數後永久 blacklist，避免無限循環。
- cooldown 空輪不執行 reclaim scan；等 target 到期後直接進下一輪。

目前預設 cooldown 為 `3`；同一 target 累積失敗 `3` 次後才永久 blacklist。

Repair 預設每個 target 試 `3` 個 buffer types。FastTimingEngine 會試跑多個
低-delay type，heuristic 模式選 timing cost 最低者，score 模式選 weighted
score 最高者，再正式 commit。

### FinalAlt Reclaim

候選：

- Delete `NEW_BUF`。
- Downsize 原始 buffer。
- Downsize `NEW_BUF`。

目前：

- Ranked candidates 啟用。
- Global top-K = 50,000。
- Repair attempts per iteration = 1,000。
- 每個 node 預設產生最多 `3` 個 smaller types，以擴大 downsize 搜尋。
- Score 開啟時，reclaim move 必須使 score 不變差。
- Score 關閉時，沿用原本 5% timing-cost giveback 與相關 guard。

當 score acceptance 啟用時，舊的 5% timing-cost giveback 不再是主要接受條件，只保留作 fallback。

### FinalAlt 結束條件

| Stop reason | 目前數值 | 意義 |
|---|---:|---|
| `timing_good` | Threshold = 0 | 已不需 repair；最後再跑一次 reclaim |
| `no_repair_insertion` | — | 仍有 violation，但沒有可接受 insertion |
| `no_meaningful_progress` | 1e-9 | Timing 與 area 都沒有進展 |
| `time_limit` | 無獨立 local limit；Global 570 秒 | 時間到期 |
| `max_iters` | 10,000 | 達到最大 iterations |
| `max_iters_zero` | 0 | 設定不允許 iteration |

目前 `final_alt_time_budget_seconds = 0.0`，代表沒有獨立 local budget。
Strategy8 開啟時，FinalAlt 最晚會在 global deadline 前預留 150 秒給
Perturb-and-Recover、10 秒給 final validation；若 Strategy8 關閉，才會使用到
global 570 秒以前的剩餘時間。

若因 Strategy8 reserve 結束，stop reason 是 `strategy8_time_reserve`，不算
global early stop。如果真的撞到 global budget，OptimizationSummary 會標成
`EARLY_STOPPED`，但程式仍會執行 final validation 並輸出合法結果；若 checkpoint
有開啟，也會先處理 checkpoint。這不等於 legality failure。

Checkpoint 開啟時，每個 iteration 結束後都會比較並更新 best checkpoint。

---

## 6. Strategy8 Perturb-and-Recover（實作保留，提交預設關閉）

### 這個 Stage 做什麼

FinalAlt 結束後可保存 current 與 best tree。以下描述的是可選的 Brutal
Maximum-Area Path 模式，每個 outer cycle 執行：

```text
從 current checkpoint 開始
        ↓
找出尚未嘗試、累積 Buffer Area 最大的 Leaf-to-Root path
（只考慮至少含一個可刪 Buffer 的 path）
        ↓
由 Leaf 往 Root 直接破壞整條 path
  可合法移除的 Buffer → Delete
  原始或其他不可刪 Buffer → 完全不動
        ↓
不做 Quick FinalAlt Recovery
        ↓
把破壞後的 raw state 直接交給完整 Reclaim-first Recovery
  第一輪 Bootstrap Reclaim（Area 降低且 Score 不變差）
  → Repair Pulse
  → Normal Pressure-Guided Reclaim（Area 降低且 Score 不變差）
  → 重複 Repair/Reclaim，最多 200 cycles
        ↓
每個 Recovery cycle 後保留目前最高 Score endpoint
        ↓
Score-only Acceptance
   Yes → 更新 current 與 best
   No  → rollback current checkpoint
```

Path Area 仍是路徑上所有 Buffer 依目前 type 計算的 Area 總和；FF、input 與
其他非 Buffer node 只用來連接 path，不計入 Area。不過候選 path 必須至少包含
一個通過既有 `reclaim_delete_ok` 的 Buffer，避免選到完全無法產生擾亂的厚
path。若多個 leaf 得到相同 Area，以 leaf 名稱決定固定順序。

破壞順序固定為 leaf 到 root。路徑上每個 Buffer 都先經過
`reclaim_delete_ok`：可以安全移除才 Delete；原始 Buffer、fanout/topology
不允許刪除的 Buffer，以及任何其他不可刪 Buffer，連 type 都不改。v8 因此不再
做隨機 resize，brutal mode 下 random seed 不影響 path mutation。

已拒絕的 path 以「路徑上的 Buffer 名稱序列」記錄，避免不同 leaf 共用同一段
Buffer path 時反覆做相同刪除；接受新狀態後 tree 已改變，這份嘗試紀錄會清空並
重新搜尋。

暴力模式完全跳過 v6 的四候選 Quick Recovery。Strategy8 進入 Recovery 時會
暫時開啟自己獨立的 score-based Repair/Reclaim，並不改變前方正式
Repair/Reclaim 的 heuristic 設定：

- Bootstrap Reclaim：Area 必須降低，而且每個 move 的 Score 不得變差。
- Repair Pulse：整批 Score 不得變差；仍保留 depth split、降低 delay scale
  與 temporary target blacklist。
- Normal Pressure-Guided Reclaim：Area 必須降低，而且每個 move 的 Score
  不得變差。

離開 Strategy8 後會恢復前方正式 Repair/Reclaim 的 acceptance 狀態。

Brutal Recovery 將 Bootstrap Reclaim、Repair Pulse targets 與 Normal Reclaim
的 candidate 數量上限設為 `0`（代表 unlimited），但仍受每次 Reclaim 的 120
秒 safety cap、Strategy8 的 150 秒 local budget、全域 deadline 與 10 秒 final
validation reserve 限制。Recovery 最多 200 cycles；每個 cycle 完成後會用完整
Timing/Area 重算 Score，保留最高分 recovery checkpoint，避免後面的 cycle
覆蓋先前較好的修復結果。

設定 `perturb_recover_use_brutal_area_path=false` 可恢復 v6：
Timing/Area/Balanced/Random 四候選、Quick FinalAlt screening，以及 bounded
Bootstrap Reclaim → Repair Pulse → Reclaim。

### 接受條件

目前 score-only acceptance 只要求：

1. 完整 legality 通過。
2. Weighted score 嚴格提高。

SS/FF TNS、WNS、NVP 或 Area 任一項都可以退步，只要
`candidate score > current score + 1e-9`。Timing-not-worse 與 NVP-growth
guard 都保留為獨立開關，但目前預設關閉。

Score 沒有嚴格提高就 rollback，最後也一定復原 stage 內的 best checkpoint。
目前不會接受總 Score 較差的 state，也沒有 temperature schedule。

### 結束條件

| Stop reason | 目前數值 | 意義 |
|---|---:|---|
| `max_cycles` | 20 | 完成最大 outer cycles／不同 path |
| `time_budget` | 150 秒 | local budget 到期，或已碰到 final-validation reserve |
| `no_legal_perturbation` | 連續 3 cycles | 沒有尚未嘗試且含可刪 Buffer 的 path |

Strategy8 額外保留 10 秒給 final validation。各輪 requested moves、perturb
種類、leaf、path node 數、path Area、recovery work、score/area 與 rollback
原因都記錄在 timing report。

完整設計、A/B 結果與目前限制見 `docs/strategy8.md`。

---

## 7. Best-State Checkpoint

這是獨立功能，`enable_best_checkpoint` 目前預設為 `false`。開啟任何 score acceptance 不會自動開啟 checkpoint，反之亦然。

### Checkpoint 保存內容

目前只保存一組最佳狀態：

- 完整 `ClockTree` clone。
- Weighted score。
- Timing analysis result。
- Area。
- 產生最佳狀態的 stage 名稱。

### 更新時機

- 初始化輸入 tree。
- Phase 0 結束後。
- 每個成功完成的 Repair/Reclaim cycle 後。
- 每個 FinalAlt iteration 後。
- Pipeline 結束、final validation 前。

### 更新條件

只有：

```text
current score > best score + 1e-9
```

才取代原 checkpoint。

### 最後復原

Final validation 前會重新完整計算目前 tree 的 timing 與 area。

如果：

```text
checkpoint score > current score + 1e-9
```

就將 tree 恢復成 checkpoint。

目前沒有：

- 多層 checkpoint。
- 手動指定 checkpoint 起點。
- Restart history。
- 自動擾亂。
- Simulated annealing acceptance。

這個簡單 checkpoint 主要是為未來故意接受 bad steps 的擾亂搜尋做準備。

---

## 8. Final Validation 與輸出

Checkpoint 比較／復原完成後：

1. 用完整 `DelayModel` 重算 SS/FF timing。
2. 重算實際 area。
3. 執行完整 legality validation。
4. 計算最終 weighted score。
5. 必要時同步並驗證 FastTimingEngine。
6. 輸出 Clock Tree 與 timing report。

報表額外記錄：

- Alpha、beta、gamma。
- Phase 0、Repair/Reclaim、FinalAlt 各自是否開啟 score-based acceptance。
- Checkpoint 更新次數。
- Best checkpoint stage。
- Best score。
- Restore 前 score。
- 最後是否真的執行 restore。

---

## 9. 目前重要參數

| Parameter | 目前設定 |
|---|---:|
| Global optimizer time limit | 570 秒 |
| `alpha / beta / gamma` | 0.20 / 0.20 / 0.60 |
| Phase 0 score acceptance | On |
| Phase 0 score weights | 0.20 / 0.20 / 0.60 |
| Phase 0 score-aligned ranking | Off |
| Phase 0 score-off fallback | SS/FF WNS、TNS、Area Pareto guard |
| Phase 0 WNS/TNS/Area tolerance | 1e-9 / 1e-9 / 1e-9 |
| Repair/Reclaim score acceptance | On |
| FinalAlt score acceptance | Off（可獨立開啟） |
| Best checkpoint | Off（可選一組） |
| Phase 0 time budget | 180 秒 |
| Phase 0 candidate budget | Unlimited（0） |
| Phase 0 node fraction | 1.0 |
| Phase 0 types per node | 6 |
| Phase 0 passes | 5 |
| Phase 0 automatic batch | Off |
| Phase 0 optional batch size | 12 |
| Phase 0 fast multi-resize batch | On（僅在 batch 啟用時使用） |
| Phase 0 maximum split depth | 2 |
| Phase 0 consecutive rejects | 1,000 |
| Phase 0 failed batches | 300 |
| Repair/Reclaim absolute end | 360 秒 |
| Per-cycle Reclaim time budget | 120 秒 |
| Repair/Reclaim max cycles | 200 |
| Repair Pulse split on failure | On |
| Repair Pulse maximum split depth | 2 |
| Repair Pulse failure recovery | On |
| Recovery maximum failures | 3 |
| Recovery scale multiplier/minimum | 0.70 / 0.50 |
| Recovery blacklisted targets/failure | 8 |
| Adaptive Repair parameters | On |
| Small / Medium / Large pulse scale | 0.5 / 1.0 / 1.5 |
| Repair Pulse acceptance | Score 不變差 |
| Repair Pulse AND/Guarded-OR switch | Guarded OR（`false`） |
| Guarded OR closed-TNS epsilon | `1e-9` |
| Guarded OR required SS improvement | `1e-9` |
| Reclaim giveback ratio | 0.20（僅 Repair/Reclaim score acceptance 關閉時的 fallback） |
| Reclaim WNS worsen allowance | 0.001（僅 Repair/Reclaim score acceptance 關閉時的 fallback） |
| Reclaim violation growth allowance | 5%（僅 Repair/Reclaim score acceptance 關閉時的 fallback） |
| Reclaim consecutive rejects | 3,000 |
| No-progress streak | 100 |
| FinalAlt local time | Disabled（0，使用 Global 570 秒） |
| FinalAlt repair attempts/iteration | 500 |
| FinalAlt max iterations | 10,000 |
| FinalAlt reclaim top-K | 50,000 |
| FinalAlt blacklist cooldown | 3 iterations |
| FinalAlt failures/target | 3 |
| FinalAlt repair types/target | 3 |
| FinalAlt reclaim types/node | 3 |
| Post-Phase0 portfolio | On；full_score 保底必跑 |
| Deadline-filling restart | On；5% / 10% / 20%，不同 deterministic seed |
| Portfolio route cap | Unlimited（0）；僅受 safe deadline 限制 |
| Portfolio restart minimum remaining | 5 秒（已扣除 finalization reserve） |
| Portfolio finalization reserve | small / medium / large = 10 / 30 / 60 秒 |
| Portfolio retained full-tree checkpoints | 1（只保留當前最佳） |
| Strategy8 Perturb/Recover | Off（實作保留為可選） |
| Strategy8 local time / validation reserve | 150 / 10 秒 |
| Strategy8 max outer cycles | 20 |
| Strategy8 brutal Area path | On |
| Strategy8 candidates/cycle | 1 條含可刪 Buffer 的最大累積 Area path |
| Strategy8 path mutation | 可刪 Buffer 全刪；其餘完全不動 |
| Strategy8 Quick Recovery | Off |
| Strategy8 Full Recovery | Reclaim-first Repair/Reclaim，最多 200 cycles |
| Strategy8 Recovery score acceptance | On（獨立於正式 Repair/Reclaim） |
| Strategy8 Bootstrap Reclaim | Unlimited trials、Area 降低且 Score 不變差 |
| Strategy8 Repair Pulse / Normal Reclaim | Unlimited targets / trials |
| Strategy8 Reclaim local safety cap | 每次 120 秒 |
| Strategy8 Recovery checkpoint | 每 cycle 保留最高 weighted score |
| Strategy8 random seed | 7（v8 mutation 不使用；供 v6 beam 使用） |
| Strategy8 final timing guard | Off |
| Strategy8 NVP-growth guard | Off |
| Strategy8 optional NVP growth ratio | 5%（只有 guard 開啟時使用） |

---

## 10. Phase 0 歷史實驗紀錄

以下 Individual/Batch 數據是較早期三個 pass、score switches 全關時的 A/B，
用來解釋為何目前仍預設 individual FastTimingEngine；它不是目前五個 pass、
Phase 0 score acceptance 開啟時的完整 Pipeline 輸出。

| 指標 | Individual FastTimingEngine（目前預設） | Fast multi-resize batch（可選） |
|---|---:|---:|
| Phase 0 runtime | 11.03 秒 | 8.26 秒 |
| Phase 0 stop | `max_passes` | `max_passes` |
| Phase 0 attempts | 93,438 | 74,873 |
| Phase 0 timing cost | 0.642165 | 0.856759 |
| Total runtime | 80.09 秒 | 76.77 秒 |
| Final SS TNS | -0.0252 | -1.9787 |
| Final SS WNS | -0.0119 | -0.1087 |
| Final SS violations | 28 | 116 |
| Final area | 7035.544 | 5883.535 |
| Legality | OK | OK |

結論：

- 當時 Individual 模式約 11 秒完成三個 pass。
- Individual 模式雖比 fast batch 多約 2.77 秒，但 timing 結果明顯較好，因此設為預設。
- Fast batch 保留為可選功能；它使用新的 Timing Engine、`O(1)` ancestor interval 與 bounded split。
- Fast batch correctness 另外在 testcase0 以每次 commit 對完整 DelayModel 驗證，TNS/WNS/area 均一致。

### Estimated-value Node Ranking A/B

兩組測試都使用 individual FastTimingEngine，只有 node 排序鍵不同。

| testcase0 指標 | `|pressure|` 排序 | Estimated-value 排序（目前） |
|---|---:|---:|
| Phase 0 runtime | 0.1148 秒 | 0.1155 秒 |
| Phase 0 timing cost | 0.512817 | 0.521356 |
| Final SS TNS | -0.0143 | -0.0052 |
| Final SS WNS | -0.0031 | -0.0024 |
| Final SS violations | 9 | 5 |
| Final area | 144.318 | 131.856 |

| testcase4 指標 | `|pressure|` 排序 | Estimated-value 排序（目前） |
|---|---:|---:|
| Phase 0 runtime | 11.0332 秒 | 10.5689 秒 |
| Phase 0 timing cost | 0.642165 | 0.669386 |
| Final SS TNS | -0.0252 | -0.0253 |
| Final SS WNS | -0.0119 | -0.0084 |
| Final SS violations | 28 | 15 |
| Final area | 7035.544 | 7192.539 |
| Total runtime | 80.0883 秒 | 79.5336 秒 |

### Phase 0 Pareto Acceptance A/B

兩組都使用 estimated-value node ranking 與 individual FastTimingEngine。舊條件要求
SS/FF WNS 不變差且 normalized timing cost 改善；該 Pareto 實驗條件要求 SS/FF WNS、
SS/FF TNS 與 Area 全部不變差，並且至少一項嚴格改善。

| testcase0 指標 | 舊 timing-cost acceptance | WNS/TNS/Area Pareto |
|---|---:|---:|
| Phase 0 runtime | 0.1110 秒 | 0.1331 秒 |
| Phase 0 accepted resizes | 333 | 179 |
| Phase 0 SS TNS | -4.7848 | -10.6810 |
| Phase 0 SS WNS | -0.1844 | -0.2617 |
| Phase 0 area | 333.097 | 177.436 |
| Final SS TNS | -0.0052 | -0.0148 |
| Final SS WNS | -0.0024 | -0.0048 |
| Final SS violations | 5 | 8 |
| Final area | 131.856 | 116.115 |
| Total runtime | 0.5681 秒 | 2.7373 秒 |

| testcase4 指標 | 舊 timing-cost acceptance | WNS/TNS/Area Pareto |
|---|---:|---:|
| Phase 0 runtime | 10.2980 秒 | 9.8572 秒 |
| Phase 0 accepted resizes | 13,256 | 7,471 |
| Phase 0 SS TNS | -295.4692 | -757.3903 |
| Phase 0 SS WNS | -0.3768 | -0.5598 |
| Phase 0 FF TNS | -0.3914 | -3.7843 |
| Phase 0 area | 13209.423 | 6833.392 |
| Final SS TNS | -0.0253 | -7.7792 |
| Final SS WNS | -0.0084 | -0.1536 |
| Final SS violations | 15 | 545 |
| Final area | 7192.539 | 5565.201 |
| Total runtime | 79.5336 秒 | 64.5019 秒（FinalAlt time limit） |

觀察：Pareto guard 成功避免 Phase 0 增加 area，但也拒絕了大量以暫時 area
增長換取 timing 改善的 resize。兩個 case 的最終 area 都較低，但 timing 都退步；
testcase4 尤其明顯，且在 FinalAlt time limit 到期時仍有 545 條 SS violations。

### Phase 0 Score-only Acceptance A/B

目前 Phase 0 使用 `alpha=0.20, beta=0.20, gamma=0.60` 的 weighted score 作為唯一
品質接受條件。候選只要 score 嚴格提高就能採用；Pareto guard、個別 WNS/TNS
guard 與 normalized timing-cost improvement 均不參與 acceptance。

| testcase0 指標 | 舊 timing-cost | Pareto guard | Score-only（目前） |
|---|---:|---:|---:|
| Phase 0 runtime | 0.1110 秒 | 0.1331 秒 | 0.1211 秒 |
| Phase 0 accepted resizes | 333 | 179 | 289 |
| Phase 0 SS TNS | -4.7848 | -10.6810 | -6.9139 |
| Phase 0 SS WNS | -0.1844 | -0.2617 | -0.2007 |
| Phase 0 area | 333.097 | 177.436 | 204.065 |
| Final SS TNS | -0.0052 | -0.0148 | -0.0159 |
| Final SS WNS | -0.0024 | -0.0048 | -0.0049 |
| Final SS violations | 5 | 8 | 6 |
| Final area | 131.856 | 116.115 | 129.912 |
| Total runtime | 0.5681 秒 | 2.7373 秒 | 0.5275 秒 |

| testcase4 指標 | 舊 timing-cost | Pareto guard | Score-only（目前） |
|---|---:|---:|---:|
| Phase 0 runtime | 10.2980 秒 | 9.8572 秒 | 11.0902 秒 |
| Phase 0 accepted resizes | 13,256 | 7,471 | 11,794 |
| Phase 0 SS TNS | -295.4692 | -757.3903 | -470.7511 |
| Phase 0 SS WNS | -0.3768 | -0.5598 | -0.4007 |
| Phase 0 FF TNS | -0.3914 | -3.7843 | -0.4517 |
| Phase 0 area | 13209.423 | 6833.392 | 6951.655 |
| Final SS TNS | -0.0253 | -7.7792 | -0.1482 |
| Final SS WNS | -0.0084 | -0.1536 | -0.0220 |
| Final SS violations | 15 | 545 | 72 |
| Final area | 7192.539 | 5565.201 | 5627.254 |
| Total runtime | 79.5336 秒 | 64.5019 秒 | 83.1056 秒 |

Score-only 對 testcase4 是比 Pareto guard 好得多的折衷：最終 TNS 絕對值減少
98.1%、WNS 絕對值減少 85.7%，Area 只增加 1.1%。但相較舊 timing-cost
acceptance，最終 timing 仍較差，而 Area 降低 21.8%。這表示目前 30% Area
權重仍會讓 Phase 0 偏向保留較小 buffer；若後續優先改善 timing，可再降低
`gamma` 或提高 `alpha`。

### Score-aligned Estimated-value Ranking A/B

兩組皆使用相同的 Score-only acceptance 與 `0.40/0.30/0.30` 權重，唯一差異是
候選嘗試順序。此實驗完成後已將
`enable_phase0_score_aligned_ranking=false`，恢復原 estimated-value 排序。

| testcase0 指標 | 原 estimated-value | Score-aligned ranking |
|---|---:|---:|
| Phase 0 runtime | 0.1211 秒 | 0.1593 秒 |
| Phase 0 accepted resizes | 289 | 294 |
| Phase 0 SS TNS | -6.9139 | -6.7927 |
| Phase 0 SS WNS | -0.2007 | -0.1974 |
| Phase 0 area | 204.065 | 208.960 |
| Final SS TNS | -0.0159 | -0.0110 |
| Final SS WNS | -0.0049 | -0.0035 |
| Final SS violations | 6 | 9 |
| Final area | 129.912 | 126.087 |
| Final weighted score | 0.948523 | 0.954902 |
| Total runtime | 0.5275 秒 | 0.6355 秒 |

| testcase4 指標 | 原 estimated-value | Score-aligned ranking |
|---|---:|---:|
| Phase 0 runtime | 11.0902 秒 | 10.5984 秒 |
| Phase 0 accepted resizes | 11,794 | 10,834 |
| Phase 0 SS TNS | -470.7511 | -465.1955 |
| Phase 0 SS WNS | -0.4007 | -0.3938 |
| Phase 0 FF TNS | -0.4517 | -0.3875 |
| Phase 0 FF WNS | -0.0464 | -0.0497 |
| Phase 0 area | 6951.655 | 7239.089 |
| Final SS TNS | -0.1482 | -1.5825 |
| Final SS WNS | -0.0220 | -0.1247 |
| Final SS violations | 72 | 110 |
| Final area | 5627.254 | 5757.861 |
| Final weighted score | 1.524978 | 1.451827 |
| Total runtime | 83.1056 秒 | 61.3656 秒 |

小 case 的 final score 提高 0.67%，但大 case 降低 4.80%。新排序雖讓
testcase4 的 Phase 0 timing 略好，卻留下較大 area 並導向較差的後續搜尋路徑。
此外，ranking 只以單一合併 pressure 和 local delay delta 近似全域 TNS/WNS
變化，並不等於真正 FastTiming trial 算出的 score。因此目前結果不支持直接用
這個近似公式取代原排序；保留獨立開關以便決定是否回復。

### Repair Pulse Depth-split A/B

基準使用 `report/strategy7/2_phase0_modify`，新版本除 Repair Pulse 拆批外使用相同
參數；兩次 testcase0/testcase4 的 Phase 0 輸出指標也完全相同。

| testcase0 指標 | 拆批前 | Depth split = 4 |
|---|---:|---:|
| Repair rejected batches | — | 0 |
| Repair batch splits | — | 0 |
| Repair/Reclaim cycles | 12 | 12 |
| Final SS TNS | -0.0109 | -0.0109 |
| Final SS WNS | -0.0047 | -0.0047 |
| Final SS violations | 10 | 10 |
| Final area | 125.741 | 125.741 |
| Final weighted score | 0.953516 | 0.953516 |
| Total runtime | 0.6142 秒 | 0.6315 秒 |

| testcase4 指標 | 拆批前 | Depth split = 4 |
|---|---:|---:|
| Repair stop reason | `batch_worsened_timing` | `split_depth_exhausted_timing` |
| Repair/Reclaim cycles | 16 | 22 |
| Repair pulse insertions | 11,463 | 11,670 |
| Repair batch attempts | — | 94 |
| Rejected repair batches | — | 59 |
| Repair batch splits | — | 36 |
| Split depth-limit hits | — | 23 |
| Repair SS TNS | -4.2857 | -3.8312 |
| Repair SS WNS | -0.1357 | -0.1020 |
| Final SS TNS | -1.7600 | -1.7494 |
| Final SS WNS | -0.1020 | -0.1020 |
| Final SS violations | 98 | 97 |
| Final area | 5736.149 | 5731.692 |
| Final weighted score | 1.467705 | 1.467834 |
| Repair/Reclaim runtime | 30.632 秒 | 37.112 秒 |
| Total runtime | 77.8908 秒 | 86.8111 秒 |

深度拆批成功延後 Stage 停止，Repair 後 TNS/WNS 也更好；但 FinalAlt 之後的最終
改善很小：weighted score 約提高 0.009%、TNS 絕對值改善約 0.60%、area 減少
約 0.08%，總時間則增加約 11.5%。testcase0 沒有 rejected batch，因此結果完全
不變，只留下少量量測波動。

### Repair Pulse Strict Timing Guard A/B

為隔離接受條件的影響，兩組在該次歷史實驗都使用 depth 3 split；唯一差異是舊版只要求
SS/FF TNS 不變差，新版另外保護 SS/FF WNS 並要求至少一項 timing 實質改善。

| testcase0 指標 | 舊 TNS-only | Strict timing experiment |
|---|---:|---:|
| Repair pulse insertions | 295 | 292 |
| Repair batch attempts | 12 | 18 |
| Rejected repair batches | 0 | 4 |
| Repair SS TNS | -0.1248 | -0.1458 |
| Repair SS WNS | -0.0065 | -0.0069 |
| Final SS TNS | -0.0109 | -0.0078 |
| Final SS WNS | -0.0047 | -0.0031 |
| Final SS violations | 10 | 10 |
| Final area | 125.741 | 126.317 |
| Final weighted score | 0.953516 | 0.955286 |
| Total runtime | 0.6392 秒 | 0.6445 秒 |

| testcase4 指標 | 舊 TNS-only | Strict timing experiment |
|---|---:|---:|
| Repair/Reclaim cycles | 28 | 21 |
| Repair pulse insertions | 12,174 | 10,969 |
| Repair batch attempts | 58 | 131 |
| Rejected repair batches | 22 | 92 |
| Repair batch splits | 15 | 55 |
| Repair SS TNS | -1.0634 | -11.1185 |
| Repair SS WNS | -0.0712 | -0.1020 |
| Final SS TNS | -0.2857 | -1.7152 |
| Final SS WNS | -0.0357 | -0.1020 |
| Final SS violations | 72 | 98 |
| Final area | 5721.929 | 5751.893 |
| Final weighted score | 1.513030 | 1.467265 |
| Total runtime | 97.0410 秒 | 82.9782 秒 |

新條件在規則層面已解決兩個問題：任何 Repair batch 都不能惡化 WNS，也不能在
timing 完全不變時只增加 area。testcase0 的 final score 提高約 0.19%；但
testcase4 的 final score 降低約 3.02%，因為嚴格 WNS guard 使更多 batch 進入
拆分並較早耗盡 depth 3，Repair 接受的 timing-improving buffer 顯著減少。

### Repair Pulse Strict Score A/B

兩組都使用 depth 3 split。新版移除 WNS hard guard，改成 Repair batch weighted
score 必須嚴格提高；Reclaim 仍使用 legacy giveback/guard。

| testcase0 指標 | Strict timing | Strict score experiment |
|---|---:|---:|
| Repair/Reclaim cycles | 12 | 5 |
| Repair pulse insertions | 292 | 226 |
| Repair SS TNS | -0.1458 | -0.5476 |
| Repair SS WNS | -0.0069 | -0.0153 |
| Final SS TNS | -0.0078 | -0.0350 |
| Final SS WNS | -0.0031 | -0.0098 |
| Final SS violations | 10 | 14 |
| Final area | 126.317 | 130.555 |
| Final weighted score | 0.955286 | 0.940290 |
| Total runtime | 0.6445 秒 | 0.4589 秒 |

| testcase4 指標 | Strict timing | Strict score experiment |
|---|---:|---:|
| Repair/Reclaim cycles | 21 | 8 |
| Repair pulse insertions | 10,969 | 10,094 |
| Repair SS TNS | -11.1185 | -10.2196 |
| Repair SS WNS | -0.1020 | -0.1389 |
| Final SS TNS | -1.7152 | -1.9477 |
| Final SS WNS | -0.1020 | -0.1020 |
| Final SS violations | 98 | 143 |
| Final area | 5751.893 | 5880.431 |
| Final weighted score | 1.467265 | 1.463565 |
| Total runtime | 82.9782 秒 | 59.4035 秒 |

Strict score 移除了 WNS hard guard，也保證零 timing 改善且增加 area 的 Pulse
無法通過；但 testcase0 score 降低 1.57%，testcase4 降低 0.25%。主要原因是
Repair 的必要中間動作本來就會增加 area，而 `gamma=0.30` 在 Reclaim 執行前
便懲罰這筆面積，導致 Repair 分別只完成 5 與 8 cycles。若要沿用 score 方向，
較合理的是在完整 Repair+Reclaim cycle 結束後才要求 score 改善，或給 Repair
暫時 area debt。

### Repair Pulse TNS AND/OR A/B

兩組都使用 depth 3 split 與原始 TNS-only 判斷，唯一差異是兩個 corner
使用 AND 或原始 OR。這是歷史實驗；目前預設已改為 Guarded OR。

| testcase0 指標 | AND baseline | Original OR experiment |
|---|---:|---:|
| Repair/Reclaim cycles | 12 | 12 |
| Repair pulse insertions | 295 | 295 |
| Final SS TNS | -0.0109 | -0.0109 |
| Final SS WNS | -0.0047 | -0.0047 |
| Final SS violations | 10 | 10 |
| Final area | 125.741 | 125.741 |
| Final weighted score | 0.953516 | 0.953516 |
| Total runtime | 0.6392 秒 | 0.6165 秒 |

| testcase4 指標 | AND baseline | Original OR experiment |
|---|---:|---:|
| Repair stop reason | `split_depth_exhausted_timing` | `cycle_time_window` |
| Repair/Reclaim cycles | 28 | 106 |
| Repair pulse insertions | 12,174 | 15,649 |
| Rejected repair batches | 22 | 0 |
| Repair SS TNS | -1.0634 | -0.7226 |
| Repair SS WNS | -0.0712 | -0.0229 |
| Final SS TNS | -0.2857 | -0.1615 |
| Final SS WNS | -0.0357 | -0.0216 |
| Final SS violations | 72 | 74 |
| Final area | 5721.929 | 5673.175 |
| Final weighted score | 1.513030 | 1.523938 |
| Total runtime | 97.0410 秒 | 245.7929 秒 |

OR 在 testcase4 將 weighted score 提高約 0.72%，TNS/WNS 與 area 也改善，但
runtime 增加約 153%。關鍵原因是 FF TNS 關閉為 0 後，「FF 不變差」幾乎永遠
成立，即使 SS TNS 變差也能接受，所以 106 個 Repair cycles 完全沒有 rejected
batch，直到 225 秒 stage window 才停止。這使 OR 更接近「解除 Repair
acceptance」而不是更合理的 corner tradeoff，因此不設為預設。

### Repair Pulse Guarded OR A/B

Guarded OR 保留一般 OR 的非 closed-corner 分支，但當 pulse 前 FF TNS 已為
0（容差 `1e-9`）時，要求 FF 維持不變差且 SS TNS 嚴格改善至少 `1e-9`。
此規則已設為目前正式預設。

| testcase0 指標 | AND baseline | 原始 OR | Guarded OR（目前） |
|---|---:|---:|---:|
| Repair/Reclaim cycles | 12 | 12 | 12 |
| Repair pulse insertions | 295 | 295 | 295 |
| Rejected repair batches | 0 | 0 | 0 |
| Final SS TNS | -0.0109 | -0.0109 | -0.0109 |
| Final area | 125.741 | 125.741 | 125.741 |
| Final weighted score | 0.953516 | 0.953516 | 0.953516 |
| Total runtime | 0.6392 秒 | 0.6165 秒 | 0.6131 秒 |

| testcase4 指標 | AND baseline | 原始 OR | Guarded OR（目前） |
|---|---:|---:|---:|
| Repair stop reason | `split_depth_exhausted_timing` | `cycle_time_window` | `split_depth_exhausted_timing` |
| Repair/Reclaim cycles | 28 | 106 | 28 |
| Repair pulse insertions | 12,174 | 15,649 | 12,174 |
| Rejected repair batches | 22 | 0 | 22 |
| Repair SS TNS | -1.0634 | -0.7226 | -1.0634 |
| Final SS TNS | -0.2857 | -0.1615 | -0.2857 |
| Final SS WNS | -0.0357 | -0.0216 | -0.0357 |
| Final area | 5721.929 | 5673.175 | 5721.929 |
| Final weighted score | 1.513030 | 1.523938 | 1.513030 |
| Total runtime | 97.0410 秒 | 245.7929 秒 | 95.7828 秒 |

Guarded OR 成功修正原始 OR 的漏洞：testcase4 恢復 22 個 rejected batches，
在 split depth 用盡時停止，而不是無拒絕地跑滿 stage window。不過這兩個
case 的 FF 在主要 Repair 搜尋期間已關閉，因此 Guarded OR 的實際搜尋結果與
AND 完全相同；約 1.3% 的 testcase4 runtime 差距屬正常執行波動，不能視為
演算法加速。只有在 FF 尚未關閉、且一個 pulse 能改善其中一個 corner 時，
Guarded OR 才可能與 AND 產生不同路徑。

### Reclaim Phase0-style Score Policy A/B

此實驗的前後兩組都開啟
`enable_repair_reclaim_score_based_acceptance=true`，因此 Repair Pulse 的
score 接受規則完全相同。唯一實作差異是 Reclaim：

- Before：legacy area/pressure heuristic 排序、score 不變差即可接受。
- After：Phase0 estimated-value 排序、score 必須嚴格改善，否則 rollback。

| testcase0 指標 | Before | After |
|---|---:|---:|
| Final SS TNS | -0.0342 | -0.0282 |
| Final SS WNS | -0.0128 | -0.0128 |
| Final SS violations | 11 | 12 |
| Final area | 126.905 | 126.618 |
| Final weighted score | 0.939922 | 0.940331 |
| Total runtime | 0.5083 秒 | 0.5209 秒 |

testcase0 的 score 提高約 0.04%，Area 減少約 0.23%，SS TNS 絕對值改善約
17.54%；runtime 增加約 2.48%，但絕對差只有約 0.013 秒。

| testcase4 指標 | Before | After |
|---|---:|---:|
| Repair SS TNS | -68.6498 | -77.3909 |
| Repair SS WNS | -0.1598 | -0.1320 |
| Repair area | 3875.778 | 3862.597 |
| Final SS TNS | -34.1647 | -39.3713 |
| Final SS WNS | -0.1327 | -0.1217 |
| Final SS violations | 4,566 | 4,942 |
| Final area | 4310.180 | 4239.365 |
| Final weighted score | 1.481923 | 1.490434 |
| Total runtime | 57.6795 秒 | 56.2875 秒 |

testcase4 的 score 提高約 0.57%、Area 減少約 1.64%、runtime 減少約 2.41%，
WNS 絕對值改善約 8.29%；代價是 TNS 絕對值惡化約 15.24%，violations 增加
約 8.23%。這表示新政策更忠實地最佳化目前加權總分，但不保證每個 timing
指標都改善。

此 Phase0-style 實驗已撤回。現在 Score-on 分支也恢復原本的 heuristic 排序與
score-not-worse 接受方式；目前正式 Pipeline 仍維持 Repair/Reclaim score
acceptance 關閉。以上數據與報表只保留為歷史實驗紀錄。

### Repair Pulse Failure Recovery A/B

兩組都使用 Guarded OR、depth-3 split 與原本 Reclaim heuristic；唯一差異是
是否在所有 split branches 失敗後執行降 scale／temporary blacklist recovery。

| testcase0 指標 | 原版 | Failure recovery |
|---|---:|---:|
| Recovery retries | 0 | 0 |
| Final SS TNS | -0.0109 | -0.0109 |
| Final SS WNS | -0.0047 | -0.0047 |
| Final SS violations | 10 | 10 |
| Final area | 125.741 | 125.741 |
| Final weighted score | 0.953516 | 0.953516 |
| Total runtime | 0.6131 秒 | 0.6638 秒 |

testcase0 沒有發生 recoverable pulse failure，因此輸出完全相同；約 0.05 秒
差距屬短執行的正常波動。

| testcase4 指標 | 原版 | Failure recovery |
|---|---:|---:|
| Repair stop reason | `split_depth_exhausted_timing` | `no_inserted_buffers` |
| Repair/Reclaim records | 28 | 55 |
| Recovery retries / successes | 0 / 0 | 11 / 8 |
| Repair pulse insertions | 12,174 | 12,464 |
| Repair SS TNS | -1.0634 | -0.5173 |
| Repair SS WNS | -0.0712 | -0.0235 |
| Final SS TNS | -0.2857 | -0.1724 |
| Final SS WNS | -0.0357 | -0.0235 |
| Final SS violations | 72 | 65 |
| Final area | 5721.929 | 5702.649 |
| Final weighted score | 1.513030 | 1.521817 |
| Total runtime | 95.7828 秒 | 139.1467 秒 |

testcase4 的 score 提高約 0.58%、Area 減少約 0.34%、TNS 絕對值改善約
39.66%、WNS 絕對值改善約 34.17%，violations 減少約 9.72%。代價是 runtime
增加約 45.27%，但仍低於原本希望的 testcase4 約四分鐘基準。

Recovery 成功跨過原本 cycle 27 的第一次 depth exhaustion；總共安排 11 次
retry，其中 8 次成功。最後在 scale `0.735`、blacklist 16 個 targets 時因
`no_inserted_buffers` 結束。這組 A/B 是當時使用 depth 3、225 秒 stage
boundary 的歷史結果；後續目前版本已將 Recovery 設為預設開啟，並把 split
depth 改為 2、absolute stage boundary 改為 360 秒、單次 Reclaim budget 改為
120 秒，因此目前參數下的完整 testcase4 結果需另行重跑才可直接比較。

### FinalAlt Search-Space 與 Score Objective 實驗

本實驗以目前 Pipeline 為 baseline，前三個 search-space 改動一起開啟：

```text
Blacklist cooldown iterations = 3
Permanent blacklist threshold = 3 failures/target
Repair types per target        = 3
Reclaim types per node         = 3
```

其中 cooldown 空輪會直接進下一個 iteration，不再浪費時間掃一次完整
reclaim candidates。根據本實驗結果，三項 search-space 擴展目前已升為正式
預設；Score objective 仍只是可選模式且預設關閉。

testcase0 的 expanded heuristic 從 baseline 的 SS TNS `-0.0109`、WNS
`-0.0047`、10 violations、Area `125.741`、Score `0.953516`、runtime
`0.6184s`，改善為 SS TNS `-0.0062`、WNS `-0.0031`、9 violations、Area
`121.365`、Score `0.960800`、runtime `0.6369s`。Score 提高約 0.76%，
runtime 增加約 3.0%。

testcase4 三組都從完全相同的 Repair/Reclaim 終點開始：

```text
SS TNS = -0.4814, SS WNS = -0.0216, SS violations = 261
FF TNS = 0, FF WNS = 0, FF violations = 0
Area = 5674.076
```

| testcase4 最終指標 | 原 FinalAlt heuristic | Expanded heuristic | Expanded + Score objective |
|---|---:|---:|---:|
| Status / legality | Completed / OK | Completed / OK | Completed / OK |
| SS TNS | -0.1314 | -0.0468 | -29.7840 |
| SS WNS | -0.0216 | -0.0027 | -0.0128 |
| SS violations | 68 | 58 | 4,765 |
| FF TNS / WNS / violations | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| Area | 5608.860 | 5608.400 | 4756.082 |
| SS score component | 1.963566 | 1.995432 | 1.965368 |
| FF score component | 2.000000 | 2.000000 | 2.000000 |
| Area score component | 0.467825 | 0.467868 | 0.548737 |
| Total weighted score | 1.525774 | 1.538533 | 1.550768 |
| Total runtime | 181.5129s | 289.5563s | 185.3599s |
| FinalAlt runtime | 43.391s | 156.898s | 55.251s |
| FinalAlt iterations | 5 | 13 | 13 |
| Stop reason | `no_repair_insertion` | `no_repair_insertion` | `no_repair_insertion` |
| Repair insertions | 235 | 261 | 7 |
| Accepted reclaim moves | 542 | 553 | 3,908 |
| NEW_BUF deletes / resizes | 42 / 500 | 50 / 503 | 913 / 2,995 |

Expanded heuristic 相對原版：

- Total score 提高約 0.84%。
- SS TNS 絕對值降低約 64.38%，WNS 絕對值降低 87.50%。
- SS violations 減少 14.71%。
- Area 只降低約 0.008%。
- Total runtime 增加約 59.52%。

因此三個擴展能找到明顯較好的 timing minimum，主要代價是多 type trials
與較晚停止所增加的時間。目前已設為預設；若之後需要壓縮 runtime，可再分別
關閉各項做消融測試。

Score objective 相對 expanded heuristic 又把 total score 提高約 0.80%，並將
Area 降低約 15.20%；然而 SS TNS 絕對值變成約 636 倍，violations 變成約
82 倍。它並非計算錯誤，而是 `gamma=0.30` 下的 area gain 足以補償大量分散、
但單條幅度較小的 timing violations。若排名或實際需求重視 timing closure，
FinalAlt 不應直接使用目前的純 score acceptance；至少要再加 TNS 或 violation
guard。正式預設因此維持 score acceptance 關閉，但選項與實作完整保留。

目前工作目錄沒有保留上述歷史 A/B 的 raw reports，因此此文件只保留已整理的
數值與結論，不再列出不存在的檔案路徑。

---

# 目前最重要的已知問題

## 1. Near-zero Slack 可能使 Violation Count 多算少量路徑

以目前 Brutal Delete-Only v8 預設跑 testcase0，Optimizer 與 bundled checker
的 SS TNS/WNS 都是 `-0.0223 / -0.0038`，legality 與 timing paths 也全部
通過；但 Optimizer 記錄 15 條 violations，checker 記錄 12 條。先前逐 commit
FastTiming
verification 顯示，
差異來自 slack 極接近 0 時的浮點正負漂移。

這不影響 weighted score、TNS、WNS、Area 或 legality；Strategy8 的 NVP guard
目前也已關閉。解讀少量 NVP 差異時仍應以完整 checker 為最終依據；若未來重新
開啟 NVP guard，應先統一一個 zero-slack epsilon。

## 2. Score-aligned Ranking 仍只是近似值

先前實驗曾讓排序使用 alpha/beta/gamma，但 node pressure 仍是 SS/FF 合併值，
local delay delta 也不能精確預測全域 TNS/WNS 變化，因此目前預設已關閉。

結果可能是：

- Estimated score gain 與 FastTiming trial 的真實 score gain 排序不一致。
- Phase 0 局部結果略好，但後續 Repair/Reclaim 搜尋路徑反而較差。

若要繼續此方向，下一步應拆開 SS pressure 與 FF pressure，或利用歷史 trial
資料校正 estimate；否則原 estimated-value 排序在 testcase4 較穩定。

## 3. 原始 Repair Pulse OR 會在單一 Corner 關閉後失去約束

OR 實驗在 FF TNS 變成 0 後幾乎不再拒絕 batch，testcase4 因此一路跑到
225 秒 stage window。雖然 score 提高 0.72%，runtime 卻增加 153%。

目前預設使用 Guarded OR，已封住 FF 關閉後的無條件放行；本次
testcase0/4 與 AND 產生相同解。若要進一步允許可控制的 corner tradeoff，
下一步應使用有限 TNS giveback budget，而不是原始無條件 OR。

## 4. FinalAlt Cooldown 的 Runtime 代價

正式預設為 cooldown `3`、每個 target 最多失敗 `3` 次，以及 Repair/Reclaim
各試最多 `3` 個 type。testcase4 timing 改善明顯，但 runtime 增加約 59.52%；
若要縮小成本，下一步應分開做三項消融。

## 5. 歷史 FinalAlt Local-Budget 數據不可直接套用

先前 Score-only testcase4 基準中，Phase 0 約 11.09 秒，整體約 83.11 秒便因
FinalAlt 的 20 秒 local budget 停止。這組數據是在本次 Phase 0 參數調整前取得；
目前 local budget 已停用，FinalAlt 使用 Global 570 秒的剩餘時間，因此該歷史
runtime 與停止原因不能當成目前 Pipeline 的預期行為。

---

# Strategy8 實測結論

目前 v8 保留「最大累積 Area 的 Leaf-to-Root path」選擇與完整
Reclaim-first Repair/Reclaim recovery，但 perturb 只刪除通過 legality 的
Buffer；原始與其他不可刪 Buffer 不再隨機 resize。下表比較舊 brutal v7 與目前
delete-only v8：

| Case | Brutal Path v7 score/area/runtime | Delete-Only v8 score/area/runtime | 結果 |
|---|---|---|---|
| testcase0 | 0.968735 / 105.489 / 9.6467 s | 0.961604 / 119.487 / 2.7576 s | Timing 大幅恢復、runtime -71.4%，但失去激進 Area/Score tradeoff |
| testcase4 | 1.538533 / 5608.400 / 438.0085 s | 1.538826 / 5607.605 / 440.5007 s | 接受 5 輪；Score、TNS、WNS、Area 全部優於 v7 |

testcase0 共嘗試 20 條 path、每條只刪一個 Buffer，接受 3 輪。相較 v7，SS
TNS/WNS 從 `-0.2641/-0.0070` 恢復到 `-0.0223/-0.0038`，但 Area 回升
`13.9979`，因此 Score 也下降。相較 Strategy8 stage 起點，v8 仍帶來
`+0.000804` Score 與 `-1.878` Area。

testcase4 在 150 秒內完成 15 條 path、接受 5 輪；Score 從 `1.538533` 提高到
`1.538826`，SS TNS/WNS 從 `-0.0468/-0.0027` 改善為
`-0.0464/-0.0023`，Area 從 `5608.400` 降到 `5607.605`。它也略高於 v6 的
Score `1.538765`，且 Area 少 `2.1652`。

因此「不可刪就不動」有效解決 v7 在大型案例過度破壞、完全無法接受的問題，
目前 v8 維持預設。設 `perturb_recover_use_brutal_area_path=false` 仍可恢復 v6。
完整 v1～v8 實驗見 `docs/strategy8.md`。

---

# 可選：Phase0 後完整 Pipeline Portfolio

程式保留一個預設關閉的離線實驗：

```cpp
enable_post_phase0_full_pipeline_fork_experiment = false;
portfolio_experiment_disable_time_limits = false;
enable_repair_reclaim_alternating_experiment = false;
repair_reclaim_alternating_delete_recent_limit = 2;
```

開啟第一個選項時，Phase0 完成後會複製同一個 checkpoint，分別跑完整
heuristic、score-based、alternating-delete 三套剩餘 Pipeline，最後依 legality
與 weighted score 選輸出。`alternating-delete` 在 heuristic 與 score cycle
間交錯；score cycle 開始前，最多刪除兩個由上一個 heuristic cycle 新插入且
當下仍符合 reclaim-delete legality 的高 Area Buffer。

`portfolio_experiment_disable_time_limits=true` 只供自然停止實驗使用：
global 與 Repair/Reclaim boundary 會提高到 86,400 秒安全上限，其他 local
wall-clock cap 停用；max-cycle、split-depth、no-progress 等條件仍有效。
正式預設已還原為 `false`，原本限制仍是：

```text
Global optimizer              = 570 秒
Repair/Reclaim absolute end   = optimizer 啟動後第 360 秒
Per-cycle Reclaim cap         = 120 秒
Strategy8 local / reserve     = 150 / 10 秒
```

Timing report 會在 Stage Contribution Summary 後列出完整 branch comparison，
並為每個分支逐 cycle 記錄 pre-delete、Pulse、Reclaim、四個 score checkpoint、
SS/FF timing、Area 與停止原因。testcase0/4 的無時間限制實驗與結論見
`docs/strategy8.md` 的「Phase0 後完整 Pipeline Portfolio 實驗」；大型案例三
分支依序執行共 2581.862 秒，因此這項功能不列入正式 Pipeline。
