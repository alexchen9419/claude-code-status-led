# Claude Code 狀態燈（ESP32-S3）

用 ESP32-S3 板載的 WS2812 RGB 燈，即時顯示 Claude Code 的工作狀態：工作中、等待批准、完成、錯誤等。重要事件會**鎖存並持續循環播放**，就算當下沒看到燈，回來時也知道發生過什麼。

原理是 Claude Code 的 **Hooks**：每當觸發特定事件，就執行一行 `curl` 打 ESP32 的 HTTP API，ESP32 再依指令切換燈號。

```bash
git clone https://github.com/alexchen9419/claude-code-status-led.git
```

---

## 目錄

- [燈號說明](#燈號說明)
- [硬體需求](#硬體需求)
- [WiFi 版安裝](#wifi-版安裝)
- [COM port 版安裝](#com-port-版安裝)
- [設定 Claude Code](#設定-claude-code)
- [清除鎖存事件](#清除鎖存事件)
- [HTTP API](#http-api)
- [自訂](#自訂)
- [疑難排解](#疑難排解)
- [專案結構](#專案結構)
- [授權](#授權)

---

## 燈號說明

### 看到這個燈，代表什麼？

| 燈號 | 意思 | 你需要做什麼 |
|---|---|---|
| ⚪ 極暗白，恆亮 | **待機**：沒有開啟中的 Claude Code session，或 session 已結束 | 不用做事 |
| 🔵 藍色，慢慢呼吸 | **工作中**：Claude 正在思考、讀檔、改程式或執行指令 | 不用管它，讓它跑 |
| 🟣 紫色，呼吸（越快 = 越多） | **Subagent 工作中**：Claude 把工作分派給子代理平行處理，呼吸越快代表同時跑的子代理越多 | 不用管它，讓它跑 |
| 🟠 琥珀色，快速閃爍 | **等你回應**：Claude 要執行的動作需要你批准，或 MCP 工具在問你問題 | ⚠️ **回到 Claude Code 按允許 / 拒絕或回答問題**，不處理它會一直停在這裡 |
| 🩵 青色，快速脈動 | **整理記憶**：對話太長，Claude 正在壓縮上下文 | 稍等幾秒，完成後會自動變回藍色 |
| 🟢 綠色，慢慢呼吸 | **完成**：Claude 已經回覆完，在等你下一個指令 | 回去看結果、輸入下一個 prompt |
| 🔴 紅色，慢閃 | **出錯中斷**：API 錯誤（網路斷線、額度用完、服務異常），這一輪沒跑完 | 回去看錯誤訊息，必要時重送 prompt |
| 🟣 暗紫色，慢閃 | **燈本身斷線**（僅 COM port 版）：`led_bridge.py` 沒在執行 | 重新啟動 `led_bridge.py` |

### 穿插在上面燈號之間的「提醒閃爍」

這些是**你不在時發生過的事**。它們會一直循環播放，直到你按 BOOT 鍵或送出下一個 prompt。**閃幾下 = 發生幾次**（最多顯示 5 下）。

| 燈號 | 意思 | 你需要做什麼 |
|---|---|---|
| 🟧 橘紅，閃 n 下 | 有 **n 個指令 / 工具執行失敗**（例如編譯錯誤、檔案找不到） | 回去看 Claude 有沒有自己修好；它通常會自動重試 |
| 🟥 紅，閃 n 下 | 有 **n 個動作被權限規則自動擋下** | 確認是否需要調整權限設定 |
| 🟩 綠，閃 n 下 | Claude **完成了 n 個待辦任務** | 回去看進度 |

播放方式：

```
[橘紅閃 2 下] → [綠閃 1 下] → [目前狀態 2.5 秒] → 重複…
```

上面這個例子代表：你離開期間有 2 次工具失敗、完成了 1 個任務，而目前的狀態要看那 2.5 秒顯示什麼燈。

### 只閃一下的燈（不用理會）

| 燈號 | 意思 |
|---|---|
| ⚪ 白色快閃 3 下 | 開啟了新的 Claude Code session，燈已連上 |
| ⚪ 白光輕閃 1 下（工作中） | Claude 剛修改或寫入了一個檔案 |
| ⚪ 白光輕閃 1 下（按鍵後） | 你按了 BOOT 鍵，提醒閃爍已清除 |

### 最常見的情況

- **一直藍色** → 它還在做，去忙別的。
- **琥珀色在閃** → 它卡住在等你批准，**這是最需要注意的燈**。
- **綠色 + 夾雜橘紅閃爍** → 做完了，但中間有遇到錯誤，回去確認結果。
- **綠色 + 夾雜綠色閃爍** → 做完了，而且有完成的任務，回去驗收。

<details>
<summary>技術對照：燈號與 Claude Code hook 事件</summary>

| 燈號 | Hook 事件 |
|---|---|
| 待機 | `SessionEnd` |
| 工作中 | `UserPromptSubmit`、`PreToolUse`、`PostToolUse`、`PostCompact`、`ElicitationResult` |
| Subagent | `SubagentStart` / `SubagentStop`（韌體內計數） |
| 等你回應 | `PermissionRequest`、`Notification`(permission_prompt)、`Elicitation` |
| 整理記憶 | `PreCompact` |
| 完成 | `Stop`、`Notification`(idle_prompt) |
| 出錯中斷 | `StopFailure` |
| 橘紅提醒 | `PostToolUseFailure` |
| 紅色提醒 | `PermissionDenied` |
| 綠色提醒 | `TaskCompleted` |
| 白色快閃 3 下 | `SessionStart` |
| 白光輕閃 1 下 | `PostToolUse`（`Edit` / `Write` / `MultiEdit`） |

</details>

---

## 硬體需求

- ESP32-S3 開發板（DevKitC-1 或相容板），需有板載 WS2812 RGB 燈
- USB 傳輸線

### RGB 燈腳位

| 板子 | `LED_PIN` |
|---|---|
| 官方 DevKitC-1 v1.0 | 48 |
| 官方 DevKitC-1 v1.1 | 38 |
| 副廠相容板 | 大多是 48，看絲印確認 |

> ⚠️ 部分副廠板在 RGB 燈旁有標示 **RGB** 的焊接跳線，出廠時是斷開的，要焊通燈才會亮。

---

## WiFi 版安裝

**韌體：** `firmware/claude_status_led_wifi/`
**Hooks：** `hooks/settings.wifi.json`

### 1. 安裝函式庫

Arduino IDE → Library Manager → 搜尋並安裝 **Adafruit NeoPixel**（by Adafruit）。

> 注意不要裝成 DMA neopixel、NeoMatrix 或 NeoPXL8。

### 2. 設定 WiFi 帳密

把 `secrets.example.h` 複製一份並改名為 `secrets.h`：

```powershell
cd firmware\claude_status_led_wifi
copy secrets.example.h secrets.h
```

編輯 `secrets.h`：

```cpp
const char* WIFI_SSID = "你的 WiFi 名稱";
const char* WIFI_PASS = "你的 WiFi 密碼";
```

> 🔒 `secrets.h` 已列在 `.gitignore`，不會被 commit。

### 3. 確認腳位

在 `claude_status_led_wifi.ino` 確認 `LED_PIN`（見[硬體需求](#rgb-燈腳位)）：

```cpp
#define LED_PIN 48
```

### 4. 燒錄

- Board：**ESP32S3 Dev Module**
- **Tools → USB CDC On Boot → Enabled**（使用原生 USB 口時）

### 5. 確認 IP

打開 Serial Monitor（115200），按 RST，會看到：

```
IP: 192.168.x.x
```

### 6. 測試

PowerShell 要用 `curl.exe`，不是 `curl`：

```powershell
curl.exe http://claude-led.local/approval
```

燈開始琥珀色快閃就成功了。如果顯示 `Could not resolve host`，改用 IP：

```powershell
curl.exe http://192.168.x.x/approval
```

> 💡 建議在路由器設定 DHCP 保留，固定 ESP32 的 IP。

---

## COM port 版安裝

**韌體：** `firmware/claude_status_led_serial/`
**Bridge：** `bridge/led_bridge.py`
**Hooks：** `hooks/settings.serial.json`

### 架構

```
Claude Code hook ──curl──▶ led_bridge.py (127.0.0.1:8765) ──序列埠──▶ ESP32
```

Bridge 會常駐並持有 COM port，避免：

- 多個 hook 同時搶同一個 COM port
- 每次開 port 拉動 DTR/RTS，導致 ESP32 重置

### 安裝

燒錄 `firmware/claude_status_led_serial/` 後：

```powershell
cd bridge
pip install -r requirements.txt
python led_bridge.py              # 自動偵測 ESP32
python led_bridge.py --port COM3  # 或手動指定
python led_bridge.py -v           # 顯示詳細 log
```

### 開機自動執行（Windows）

工作排程器 → 建立工作：

- **觸發程序：** 登入時
- **程式：** `pythonw.exe`
- **引數：** `C:\路徑\led_bridge.py`

> ⚠️ Bridge 執行時會佔住 COM port。重新燒錄韌體前要先關閉 bridge。

---

## 設定 Claude Code

### 1. 編輯設定檔

| 範圍 | 路徑 |
|---|---|
| 所有專案（建議） | `C:\Users\<使用者>\.claude\settings.json` |
| 單一專案（會 commit） | `<專案>\.claude\settings.json` |
| 單一專案（不 commit） | `<專案>\.claude\settings.local.json` |

依你使用的版本選擇 Hooks 設定：

| 版本 | 檔案 |
|---|---|
| WiFi | `hooks/settings.wifi.json` |
| COM port | `hooks/settings.serial.json` |

- **檔案不存在**：直接貼上整份內容
- **已有其他設定**：只把 `"hooks": { ... }` 區塊加進去，跟原本的設定並列
- **`.local` 解析不到**：把 `claude-led.local` 全部取代成 ESP32 的 IP

### 2. 重新啟動

關閉後重新執行 `claude`，輸入：

```
/hooks
```

確認事件都有列出來。開新 session 時燈會白色閃 3 下。

### 讓 Claude Code 自己合併

```
把 C:\路徑\hooks\settings.wifi.json 裡的 hooks 合併進我的 ~/.claude/settings.json，保留原本的其他設定
```

---

## 清除鎖存事件

| 方式 | 效果 |
|---|---|
| 按板子上的 **BOOT 鍵** | 清除鎖存事件；Done / Error 狀態切回待機 |
| 在 Claude Code **送出新 prompt** | 自動清除鎖存事件 |
| `curl http://claude-led.local/ack` | 手動清除 |

---

## HTTP API

WiFi 版主機為 `http://claude-led.local`，COM port 版為 `http://127.0.0.1:8765`，路徑相同。

### 基礎狀態

| 路徑 | 說明 |
|---|---|
| `/idle` | 待機（同時清除 subagent 計數） |
| `/working` | 工作中 |
| `/approval` | 等待批准 |
| `/done` | 完成 |
| `/error` | 錯誤 |
| `/compact` | 壓縮上下文 |
| `/subagent/start` | subagent 計數 +1 |
| `/subagent/stop` | subagent 計數 −1 |

### 鎖存與閃爍

| 路徑 | 說明 |
|---|---|
| `/latch?id=fail` | 鎖存事件，id 可用 `fail`、`denied`、`task` |
| `/ack` | 清除所有鎖存事件 |
| `/flash?c=FF0000&ms=150&n=2` | 一次性閃爍：顏色、亮的毫秒數、次數 |

### 除錯

| 路徑 | 說明 |
|---|---|
| `/status` | 回傳目前狀態、subagent 數量、各鎖存事件次數（JSON） |
| `/ping` | 心跳（僅 COM port 版） |

`/status` 回傳範例：

```json
{"state":1,"subagents":0,"latches":{"fail":2,"denied":0,"task":1}}
```

`state` 對應：`0` 待機、`1` 工作中、`2` 等待批准、`3` 完成、`4` 錯誤、`5` 壓縮。

---

## 自訂

### 時間參數

```cpp
const uint16_t BASE_MS  = 2500;  // 每輪顯示基礎狀態的時間
const uint16_t BLINK_MS = 150;   // 鎖存事件閃爍速度
const uint16_t GAP_MS   = 500;   // 不同事件之間的間隔
const uint8_t  MAX_SHOW = 5;     // 次數最多顯示幾下
```

### 亮度

```cpp
led.setBrightness(60);  // 0–255
```

### 新增鎖存事件

1. 在韌體的 `latches[]` 加一行（兩個版本的韌體都要改）：

   ```cpp
   Latch latches[] = {
     { "fail",   0xFF4000, 0 },
     { "denied", 0xFF0000, 0 },
     { "task",   0x00FF00, 0 },
     { "mine",   0x0080FF, 0 },   // 新增
   };
   ```

2. 在 `hooks/settings.*.json` 對應的 hook 呼叫 `/latch?id=mine`。

### 外接燈條

修改 `LED_PIN` 為燈條的資料腳位，並把 `LED_COUNT` 改成燈珠數量，所有燈會顯示相同顏色。

---

## 疑難排解

| 症狀 | 可能原因 / 解法 |
|---|---|
| 編譯錯誤 `Adafruit_NeoPixel.h: No such file` | 沒安裝 Adafruit NeoPixel 函式庫 |
| 編譯錯誤 `secrets.h: No such file` | 沒有把 `secrets.example.h` 複製成 `secrets.h` |
| 燈完全不亮 | `LED_PIN` 錯誤（試 48 / 38）；副廠板 RGB 跳線未焊通 |
| 一直紫色、沒變待機 | WiFi 連不上，檢查 SSID / 密碼（只支援 2.4 GHz） |
| Serial Monitor 沒輸出 | `USB CDC On Boot` 沒設成 Enabled |
| `Could not resolve host: claude-led.local` | Windows mDNS 不穩，改用 IP |
| PowerShell 的 `curl` 出現奇怪錯誤 | 改用 `curl.exe` |
| `/hooks` 看不到事件 | JSON 格式錯誤（多餘逗號）；未重開 session |
| 按 Esc 中斷後燈停在藍色 | 正常，`Stop` 不會在中斷時觸發；下次送 prompt 會恢復 |
| 工具執行時一直是黃燈 | 正常，批准後要等工具執行完 `PostToolUse` 才會切回藍色 |
| 每個工具呼叫都變慢 | 調低 `curl -m` 逾時，或移除 `PreToolUse` hook |
| COM port 版暗紫慢閃 | `led_bridge.py` 沒在執行 |
| COM port 版無法燒錄 | Bridge 佔用 COM port，先關閉 |

---

## 專案結構

```
claude-code-status-led/
├── README.md
├── LICENSE
├── .gitignore
├── firmware/
│   ├── claude_status_led_wifi/
│   │   ├── claude_status_led_wifi.ino   # WiFi 版韌體
│   │   └── secrets.example.h            # WiFi 帳密範本
│   └── claude_status_led_serial/
│       └── claude_status_led_serial.ino # COM port 版韌體
├── bridge/
│   ├── led_bridge.py                    # COM port ↔ HTTP 轉接
│   └── requirements.txt
└── hooks/
    ├── settings.wifi.json               # WiFi 版 Claude Code hooks
    └── settings.serial.json             # COM port 版 Claude Code hooks
```

---

## 授權

[MIT License](LICENSE)
