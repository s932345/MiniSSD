# MiniSSD 驗證結果

完成驗證日期：2026-10-10～2026-10-11（Asia/Taipei）。固定 seed 保持 20261010，方便重現。

驗證範圍包含 NAND、FTL、自動 Greedy GC、Trim、操作前故障注入、CLI 資料驗證與模擬 Benchmark。

## 建置與測試

- Windows MSYS2 UCRT64 g++ 16.2.0，C++20，Release `-O2 -DNDEBUG`。
- `build.ps1` 使用嚴格警告、`-Werror`、`_GLIBCXX_ASSERTIONS`，全部成功。
- NAND 13/13、停用 GC 的 FTL 11/11、GC／故障 12/12，合計 36 組通過。
- 三組混合工作負載各 30,000 次，合計 90,000 次；另測每 Block 1／2 頁裝置的連續更新。
- 本機可攜 CMake 3.31.12、Ninja 1.13.2 與 g++ 成功建置。CMake ZIP 已核對官方 SHA-256。
- CTest：NAND、FTL、GC、CLI 驗證、CLI Benchmark、Unicode 報告，6/6 項通過。
- `build-cmake.ps1` 從父資料夾執行成功；CLI 的錯誤參數、最小 count、seed 0 與中文報告檔名已驗證。

## 故障驗證

已通過：Host Program 失敗、GC Read 失敗、部分搬移 Program 失敗、victim Erase 失敗、
暫存清理也失敗後重試，以及 GC 成功後 Host Program 失敗。
每種情境核對各 LBA 最新資料與映射有效性。
注入故障發生在操作改變 bytes 前，沒有模擬部分硬體 Program 或掉電。

## 獨立 CLI 驗證結果

20,000 次操作、800,000 次 LBA 核對、2,422 次成功 GC，結果 pass。
成功寫入 10,567 次、成功讀取 3,265 次、Trim 1,911 次、預期拒絕 2,256 次、手動 GC 請求 2,001 次。
上述請求計數加總 20,000。NAND Program 19,070 次，搬移有效頁 8,503 次，GC 故障 0。
原始 JSON 保存在 [validation.json](results/validation.json)。

## 模擬 Benchmark 的一次本機量測

初始填入 192 個 LBA 不計入下面的操作與時間。每個工作負載量測 5,000 次 4 KiB 更新，最後完整讀回驗證。

| 工作負載 | Host 寫入 | NAND Program | GC 次數 | WAF | p95（µs） | 迴圈吞吐量（MiB/s） |
|---|---:|---:|---:|---:|---:|---:|
| 循序更新 | 5,000 | 5,000 | 310 | 1.000 | 3.4 | 265.863 |
| 隨機更新 | 5,000 | 12,560 | 782 | 2.512 | 11.3 | 241.264 |

這是 RAM 模擬程式的一次量測，不代表實體 SSD，也不是效能改善宣稱。
吞吐量涵蓋 payload 生成及 oracle 更新，延遲涵蓋 FTL 呼叫、結果檢查與計時成本。
原始 JSON 保存在 [benchmark.json](results/benchmark.json)，WAF 公式與分位數順序已另行核對。

## GitHub Actions 遠端驗證

2026-10-11 已發布 [MiniSSD](https://github.com/s932345/MiniSSD)。
原始碼提交 `5d19a7f7b403bf5a3417ed8334b04f10f8ed0c4d` 的 [CI 執行](https://github.com/s932345/MiniSSD/actions/runs/38125315237) 成功：

| 工作 | 結果 | 驗證內容 |
|---|---|---|
| Windows Release | success | CMake 建置、6 項 CTest、20,000 次 CLI 操作、兩組 Benchmark |
| Linux Release | success | CMake 建置、6 項 CTest、20,000 次 CLI 操作、兩組 Benchmark |
| Linux ASan／UBSan | success | Debug 建置及 6 項 CTest，含資料驗證與 Benchmark |

Windows／Linux 的 JSON 報告與 CTest 紀錄已保存為該次執行的 artifacts，保留 14 天。
`docs/results/` 的數據仍是先前本機量測，與遠端執行產生的報告分別保存。

## 未涵蓋的範圍

模型沒有持久化、掉電恢復、並行 I/O 或真實硬體介面。
