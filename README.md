# MiniSSD

C++20 in-memory SSD storage simulator and validation tool.

MiniSSD models NAND constraints and implements a page-mapped Flash Translation Layer (FTL), out-of-place updates, greedy garbage collection, Trim, and fault injection. Reproducible workloads verify logical data and report write amplification. Benchmarks measure this RAM simulator, not physical SSD performance.

## 專案目的

探索 SSD 資料管理如何在寫入限制、反覆更新、空間回收與操作失敗的情境下維持資料正確。專案提供可執行的 CLI、獨立參考資料核對與自動測試，展示 C++、狀態管理、錯誤處理及儲存驗證。

## 功能

| 模組 | 行為 |
|---|---|
| Virtual NAND | Page 讀取與 Program、Block Erase、每頁每個抹除週期一次 Program |
| Page-mapped FTL | LBA 映射、異地更新、Free／Valid／Invalid 狀態、反向有效 LBA |
| Greedy GC | 選擇最多 Invalid 頁的 Block；搬移有效資料、抹除並交換預留 Block |
| Trim | 移除邏輯映射，後續由 GC 回收空間 |
| 故障注入 | Read／Program／Erase 操作前一次失敗，驗證回復及暫存清理重試 |
| 驗證與指標 | 獨立資料 oracle、固定 seed、JSON 報告、WAF、延遲 p50／p95／p99 |

## 建置與執行

### Windows g++

需要支援 C++20 的 g++。腳本會先查找 PATH，預設亦支援 MSYS2 UCRT64 的安裝位置。

```powershell
# Release 建置、36 組測試、Demo、驗證及 Benchmark
.\build.ps1

# 分別執行
.\build.ps1 -Action test
.\build.ps1 -Action demo
.\build.ps1 -Action report

# Debug 測試或指定編譯器
.\build.ps1 -BuildType Debug -Action test
.\build.ps1 -Compiler 'C:\msys64\ucrt64\bin\g++.exe'
```

腳本不下載工具或修改系統設定，使用相對編譯路徑以支援中文專案路徑。
Release 使用 `-O2 -DNDEBUG`；測試以明確條件檢查，不因 NDEBUG 停用。
建置開啟嚴格警告、`-Werror` 與 `_GLIBCXX_ASSERTIONS`。

### CMake

需要 CMake 3.20+ 與 C++20 編譯器；Windows 可選用 Visual Studio 或 g++／Ninja。

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --config Release
ctest --test-dir build-cmake -C Release --output-on-failure
```

Windows g++ 使用 Ninja 時，可在 CMake、Ninja 和 g++ 可用的環境執行 `build-cmake.ps1`。

Linux GCC／Clang 的選用 ASan／UBSan 建置：

```sh
cmake -S . -B build-sanitized -DCMAKE_BUILD_TYPE=Debug -DMINISSD_SANITIZERS=ON
cmake --build build-sanitized
ctest --test-dir build-sanitized --output-on-failure
```

### CLI

以下為 Windows 腳本產生的執行檔路徑；CMake 請改用建置目錄中的 `minissd` 或 `minissd.exe`。

```powershell
.\build\minissd.exe demo
.\build\minissd.exe nand-demo
.\build\minissd.exe ftl-demo
.\build\minissd.exe gc-demo

.\build\minissd.exe validate --seed 20261010 --operations 20000 --report reports/validation.json
.\build\minissd.exe benchmark --seed 20261010 --writes 5000 --report reports/benchmark.json
.\build\minissd.exe --help
```

完整 Demo 展示自動 GC、1,000 次更新後的資料核對、Trim 及故障時的資料保留。
`ftl-demo` 停用 GC，展示空間耗盡的行為。
操作數範圍為 1～1,000,000，seed 為 uint32。退出碼：成功 0、執行失敗 1、參數錯誤 2。

## 驗證結果

目前已實測 Windows MSYS2 UCRT64 g++ 16.2.0，以及 CMake 3.31.12／Ninja 1.13.2：

- 36 組測試通過：NAND 13 組、停用 GC 的 FTL 11 組、GC／故障 12 組。
- 測試包含固定 seed 的 90,000 次混合操作，另有小容量裝置連續更新。
- CLI 完成 20,000 次操作、800,000 次 LBA 核對及 2,422 次成功 GC。
- CTest 6/6 通過，包含 CLI 驗證、Benchmark 及 Unicode 報告路徑。
- JSON 操作計數、WAF 公式與延遲分位數順序已核對。

詳細紀錄見 [驗證結果](docs/verification.md)；保留的原始結果見 [validation.json](docs/results/validation.json) 與 [benchmark.json](docs/results/benchmark.json)。

GitHub Actions 已配置 Windows／Linux CMake 和 Linux ASan／UBSan；遠端執行結果尚待確認，目前不宣稱 Linux、MSVC 或 sanitizer 已通過。

## 模型與量測範圍

預設 16 個 Block × 16 個 Page × 4 KiB，共 1 MiB；提供 192 個 LBA，共 768 KiB。
一個 model LBA 等於完整 NAND Page。自動 GC 保留一整個 Block，另保留更新餘裕。
未寫入或 Trim 後讀取回傳 `UnwrittenLba`。

`stats().free_pages` 包含預留頁，`available_free_pages()` 才是 Host 可配置頁數。
Program 成功後才發布新映射。寫入失敗保留原邏輯資料；先前若已成功 GC，實體位置可能改變。
故障注入發生在操作修改資料之前，GC 暫存清理失敗時會在後續回收前重試。

Benchmark 先填入全部 LBA，再量測循序或隨機更新。WAF 與 GC 計數扣除初始填入，初始化及最後驗證不計時。
迴圈吞吐量包含資料生成及 oracle 更新；單次延遲包含 FTL 呼叫、結果檢查及計時成本。
數據受 CPU、編譯方式及負載影響，不能用來預測實體 SSD 效能。

本模型同步、單執行緒且只存在 RAM。未實作持久化、掉電恢復、並行 I/O、ECC、壞塊、磨耗平均、真實 NAND 時序、NVMe 或硬體控制。
「每頁每個 Erase 週期一次 Program」是本模型的簡化合約。

## 專案結構

```text
include/minissd/   NAND、FTL 與 workloads 公開介面
src/              模型、驗證、Benchmark 與 CLI
tests/            NAND、FTL、GC 與故障測試
docs/             架構、合約與驗證結果
.github/workflows/ GitHub Actions
```

設計細節見 [架構與 API 合約](docs/architecture.md)。

Development used AI assistance for implementation, debugging, and test development.
