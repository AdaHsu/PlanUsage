<p align="center">
  <img src="images/Banner.png" width="480" alt="Plan Usage">
</p>

# Plan Usage

[English](README.md) | 正體中文

透過 LilyGo T-Display S3 顯示各家 AI 訂閱的投報率。支援任意數量的帳號，且完全自己連網抓資料，不需要電腦端的配套程式、不需要橋接裝置、不需要哪台機器一直開著跑 CLI。

一顆按鍵切換帳號；螢幕上正在顯示的那一個，才是唯一會發網路請求的那一個。

<table>
<tr>
<td><img src="images/3388.jpg" width="280" alt="裝置顯示 Claude 個人版 Pro 方案"></td>
<td><img src="images/3389.jpg" width="280" alt="裝置顯示 Claude Teams 組織"></td>
<td><img src="images/3387.jpg" width="280" alt="裝置顯示 Codex Plus 方案"></td>
</tr>
<tr>
<td align="center">Claude · 個人版（Pro）</td>
<td align="center">Claude · Teams</td>
<td align="center">Codex · Plus</td>
</tr>
</table>

## 這個裝置做了什麼

- **自己連網。** 最多存 8 組 WiFi，依清單順序依序嘗試連線（是優先序，不是訊號強度）。斷線時在背景重連，不會重開機。
- **帳號組合完全自由，最多 12 筆。** 2 個 Claude 個人帳號、1 個 Claude 企業版 org、再加 3 個 Codex 帳號，這種組合完全合理。程式裡沒有任何「每家該有幾筆」的假設，也不會把不同模型商配對在一起。
- **只有畫面上顯示的那一筆會抓資料。** 沒被看到的帳號不跑任何計時器，也不會發出任何請求。
- **每家模型商畫自己的畫面。** 一個 provider 模組自己管抓取、排版、設定表單；外殼只負責給它標題列跟一塊矩形區域。

## 按鍵

| 按法 | 動作 |
|---|---|
| KEY 按一下 | 切換下一個帳號 |
| KEY 按兩下 | Stats／Info 切換 |
| KEY 連按三下以上 | 依按的次數跳過對應數量的帳號 |
| KEY 長按 10 秒 | 進入重開機倒數（倒數中按住 BOOT 2 秒可觸發原廠重置） |
| BOOT 按一下 | 旋轉畫面 90 度 |
| BOOT 長按 2 秒 | 關閉螢幕 |
| BOOT + KEY 同時按 5 秒 | 進入深度睡眠 |
| 睡眠中按任一鍵 | 喚醒，這次按壓不會觸發其他動作 |

KEY 每次按下後會先等 280 毫秒，確認是否還有後續按壓，這就是讓一顆按鍵能同時做兩件事的關鍵。

## 什麼時候會抓資料

按下按鍵到真的發出請求之間，隔著兩道閘門：

1. **停留滿 5 秒。** 每切換一次就重新計時，所以連按過清單裡一半的帳號，完全不會發出任何請求。
2. **快取要超過 3 分鐘才會重抓。** 切回剛看過的帳號，直接沿用已經有的資料。

沒有切換動作時，畫面上顯示的帳號每 5 分鐘自動更新一次（可在管理頁調整為 1 到 60 分鐘）。

## 設定流程

1. 第一次開機會開一個叫 `Plan Usage` 的開放式熱點。用手機連上，captive portal 會掃描並列出附近的網路可以點選（也能手動輸入隱藏網路的名稱），**只會問 WiFi**，不問其他任何東西，因為之後要填的憑證都是上千字元的長字串，不適合在手機上打。
2. 連上網路後，螢幕會顯示 `http://planusage.local`。用電腦瀏覽器打開這個網址（預設帳密是 `admin`／`admin`，可以在 Settings 裡修改）。
3. 新增帳號。**每個登入只需要貼一次憑證**，貼完按 Discover 就會列出這把憑證能存取的所有 organization／workspace，勾選要加入的，每個勾選項目都會變成獨立的一筆，共用同一份憑證。

已存的憑證一律顯示遮蔽過的樣子（例如 `sk-ant-...a3f9`），不會完整顯示。憑證欄位留空表示沿用原本存的那份。

### Claude

去 claude.ai 用瀏覽器開發者工具複製 `sessionKey` 這個 cookie。一筆帳號就是一組 `(sessionKey, orgId)`；同一個登入底下的個人版跟 Teams 版是兩筆、共用同一把 sessionKey，另一個登入就是另一把獨立的 sessionKey。

### Codex／ChatGPT

從 `~/.codex/auth.json` 複製 `tokens.access_token`。帳號 id 跟到期時間會直接從這個 token 本身解析出來。這個 token 大約 10 天到期，快到期時畫面會顯示警示。

裝置有自動更新 token 的功能，但**預設關閉**：refresh token 每次更新都會輪替，如果裝置幫你換了新的，電腦上的 Codex CLI 可能會因此被登出。

## 編譯

```bash
pio run                  # 編譯
pio run -t upload        # 燒錄
pio device monitor -b 115200
```

## 檔案結構

```
src/
  core/      provider 介面、帳號儲存、輪播邏輯與抓取閘門
  providers/ 每個模型商一個目錄：抓取 + 排版 + 設定表單
  ui/        面板、sprite、繪圖元件，以及外殼自己負責的畫面
  net/       WiFi 清單、HTTPS、NTP
  web/       captive portal 與管理頁
  hal/       板子腳位與電池
```

新增一家模型商，只需要在 `src/providers` 底下開一個新目錄，再到 `src/core/registry.cpp` 加一行，不用動任何核心檔案。

## 附註

- 兩家的用量端點都是非公開 API，隨時可能無預警改動，所以每個欄位都做了容錯處理：缺欄位只會讓畫面顯示不完整，不會讓抓取整個掛掉。
- TLS 憑證沒有驗證（`setInsecure()`），跟上游專案一樣的取捨。裝置上存的憑證才是真正的機敏資訊，這台裝置也該當作實體上可信任的環境來用。

## 致謝

架構、板子處理方式跟 pace 門檻值都是沿用 Albert Horta 的
[ClaudeStatsPortable](https://github.com/alberthorta/ClaudeStatsPortable)（MIT 授權）。這個專案是圍繞「可插拔模型商層」與「多帳號切換」從頭重寫的版本。
