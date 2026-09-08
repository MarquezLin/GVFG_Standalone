# GVFG SDK 錯誤處理架構

本文說明目前 SDK 如何產生、傳遞與保存錯誤。重點不是只看回傳碼，而是同時保留「錯誤分類」與「實際失敗位置」。

## 1. 三層錯誤資訊

```text
公開 gvfg_status_t
    用來讓 application 判斷錯誤類別

ChannelErrorState 詳細字串
    用來指出哪個操作被拒絕，或哪個 Win32/driver 呼叫真正失敗

Win32 GetLastError()
    driver/OS 呼叫失敗時的原始錯誤碼
```

例如 driver IOCTL 失敗可能得到：

```text
回傳值：GVFG_EIO
詳細字串：copy get frame failed: The device is not ready. (21)
```

## 2. 公開 status

公開型別是 `gvfg_status_t`，常見分類如下：

| Status | 意義 | 常見情況 |
|---|---|---|
| `GVFG_OK` | 成功 | 操作完成 |
| `GVFG_EINVAL` | 參數錯誤 | null pointer、錯誤 channel、frame token 不符 |
| `GVFG_ENODEV` | 找不到裝置 | device index 不存在 |
| `GVFG_ESTATE` | 呼叫時機或狀態錯誤 | 尚未 open、尚未 start、上一張 frame 未 release |
| `GVFG_EIO` | backend/driver/OS I/O 失敗 | event、register、IOCTL 失敗 |
| `GVFG_ENOTSUP` | 不支援 | pixel format 或功能不支援 |
| `GVFG_ETIMEOUT` | 等待超時 | deadline 前沒有 frame/event |

`gvfg_strerror(status)` 只回傳固定分類文字，例如 `"I/O error"`。它不包含實際 Win32 error 或失敗函式。

## 3. 每條 channel 的 `ChannelErrorState`

`gvfg_handle_t` 直接持有兩份錯誤狀態：

```cpp
std::array<ChannelErrorState, 2> channelErrors;
```

對應關係：

```text
channelErrors[0] = CH0 最近一次詳細 fault/rejection
channelErrors[1] = CH1 最近一次詳細 fault/rejection
```

每個 `gvfg_channel_session_t` 建立時，取得對應錯誤狀態的 reference：

```cpp
explicit gvfg_channel_session_t(ChannelErrorState &errorState)
    : errorState(errorState)
{
}
```

backend 也取得同一份 reference：

```cpp
backend = std::make_unique<PcieS2mmCaptureSession>(errorState);
```

因此同一條 channel 的三層共用同一份詳細訊息：

```text
公開 C API wrapper
       ↓
gvfg_channel_session_t
       ↓
PcieS2mmCaptureSession
       ↓
同一個 ChannelErrorState
```

即使 `gvfg_open_channel()` 中途失敗、session 沒有存進 `channels[]`，詳細錯誤仍保留在 `gvfg_handle_t::channelErrors[]`。

## 4. 為什麼 `ChannelErrorState` 內部有 mutex

```cpp
class ChannelErrorState final
{
public:
    void set(std::string message);
    void clear();
    std::string message() const;

private:
    mutable std::mutex mutex_;
    std::string message_;
};
```

backend event thread、capture/read thread 與 application thread 都可能產生或讀取錯誤。`mutex_` 防止 `std::string` 同時讀寫造成 data race。

`message()` 回傳一份字串 copy，而不是把內部 `c_str()` 直接交出去；離開 lock 後仍安全。

`gvfg_poll_channel_event()` 的 timeout／non-blocking 無資料不會寫入
`ChannelErrorState`。目前 `gvfg_read_channel_frame()` 的 backend timeout 會經過
`PcieS2mmCaptureSession::reject()`，因此會保存 timeout 詳細訊息。新的
`gvfg_start_channel()` request 會先清除舊內容；若 start 失敗，失敗路徑會立即寫入新的
詳細資訊。

這裡必須區分「目前實作」與「可能希望的契約」：公開 header 曾將所有
`GVFG_ETIMEOUT` 描述成不保存，但目前 backend 並非如此。若要改成 timeout 全部不覆寫，
應修改 backend 行為，而不是只改文件。

## 5. `reject()` 與 `fail()` 的差異

### `reject()`：SDK 主動拒絕不合法操作

典型例子：

```cpp
if (frameHeld)
    return reject(
        GVFG_ESTATE,
        "previous frame has not been released");
```

這不是 driver 呼叫失敗，而是 SDK 在呼叫 driver 前就判定狀態不合法。

常見來源：

- channel 尚未 open/start。
- 已有 read 正在進行。
- 上一張 frame 尚未 release。
- release token 與目前 held frame 不符。
- 設定功能的呼叫時機不合法。

### `fail()`：Win32／driver 操作已經失敗

backend 的 `fail()` 會組合：

```text
操作名稱 + FormatMessage(GetLastError()) + 數字 error code
```

例如：

```cpp
return fail(PCIES2MM_EIO, "wait DMA event", err);
```

同時會：

1. 寫入 `ChannelErrorState`。
2. 透過 error diagnostic log 輸出。
3. 回傳 backend status。

## 6. Backend status 如何轉成公開 status

backend 使用 `pcies2mm_status_t`，facade 透過 `map_status()` 轉成 `gvfg_status_t`：

```text
PCIES2MM_OK       -> GVFG_OK
PCIES2MM_EINVAL   -> GVFG_EINVAL
PCIES2MM_ENODEV   -> GVFG_ENODEV
PCIES2MM_ESTATE   -> GVFG_ESTATE
PCIES2MM_ENOTSUP  -> GVFG_ENOTSUP
PCIES2MM_ETIMEOUT -> GVFG_ETIMEOUT
PCIES2MM_EIO      -> GVFG_EIO
```

轉換只改錯誤分類，不應覆蓋 backend 已寫入的詳細訊息。

## 7. Application 正確取得錯誤的方式

```cpp
const gvfg_status_t status =
    gvfg_read_channel_frame(handle, channel, &frame, 1000);

if (status != GVFG_OK && status != GVFG_ETIMEOUT)
{
    char detail[512] = {};
    gvfg_get_channel_last_error_detail(
        handle,
        channel,
        detail,
        sizeof(detail));

    printf("%s: %s\n", gvfg_strerror(status), detail);
}
```

應在失敗後立即讀取，因為同一 channel 後續發生的新錯誤可能覆蓋舊訊息。
`GVFG_ETIMEOUT` 本身已完整表達「期限內沒有結果」，不應讀取詳細錯誤並把先前內容配到這次 timeout。

## 8. Frame 錯誤時的 ownership 規則

| 情況 | 是否呼叫 release | 原因 |
|---|---:|---|
| `read` 成功 | 是，恰好一次 | SDK/backend 已進入 held state |
| `GVFG_ETIMEOUT` | 否 | 沒有 frame 交付給 caller |
| `GVFG_EIO` 且沒有成功 frame | 否 | caller 沒有取得 ownership token |
| 第二次 read 被 `frameHeld` 拒絕 | 否 | 應先 release 原本成功取得的 frame |
| release token 不符 | 不可清除 held state | 正確 token 仍需重試 release |
| zero-copy release IOCTL 一般失敗 | 不可清除 held state | driver frame 可能仍被占用，必須保留 token 供 retry |
| 已確認 unplug 後 release 回 `ERROR_BAD_COMMAND (22)` | 清除相符 held state | driver 已撤銷該 ownership；保留 token 會阻塞 unplug/replug lifecycle |

## 9. Transient retry 規則

DMA event 到達後，Acquire/GetFrame 仍可能短暫回報 frame not ready。目前只將以下 Win32 error 視為 transient：

- `ERROR_NOT_READY`
- `ERROR_BUSY`
- `ERROR_RETRY`
- `ERROR_NO_MORE_ITEMS`

處理規則：

```text
transient error
    -> 保留原始 timeout deadline
    -> 等下一次 event
    -> deadline 到達後回 GVFG_ETIMEOUT

其他 error
    -> 保存 GetLastError()
    -> fail(...)
    -> 立即回 GVFG_EIO
```

不得使用無界 retry，也不得加入 `Sleep()` 讓總等待時間超出 caller 指定的 timeout。

## 10. Stop 與錯誤

`stop()` 的核心順序是：

```text
facade running = false
    -> 嘗試 release facade held frame
    -> backend stop_stream()
    -> 停 DMA／喚醒 wait
    -> 等 read 結束
    -> release backend pending zero-copy frame
    -> 停 event monitoring/thread
```

一般 zero-copy release 失敗時，backend 不應假裝已經釋放；錯誤必須回傳並保留
足以重試的狀態。若已確認 signal disconnected，且 release 回
`ERROR_BAD_COMMAND (22)`，則表示 driver 已在拔線流程撤銷 ownership；backend 會
清除相符的本地 held state 並完成 release，避免 event-monitor thread 永久等待。
這個例外不可套用到 connected 狀態或其他錯誤碼。

正常 `stop()` 為了結束 blocking read/event poll 而喚醒 waiter 時，只回傳 `GVFG_ESTATE`，不寫入
永久錯誤；application 應以自己的 stop flag 判斷這是不是預期的結束流程。

## 11. 查錯順序

遇到錯誤時依序記錄：

1. 公開 API 名稱與 `gvfg_status_t`。
2. `gvfg_get_channel_last_error_detail()`。
3. channel index、copy/zero-copy mode、解析度與格式。
4. 是否有成功 read 但尚未 release 的 frame。
5. 是否正在 stop、拔插或 format change。
6. driver 版本與原始 Win32 error code。

不要只根據 `GVFG_EIO` 推斷 driver 壞掉；它只是分類，實際失敗邊界必須看詳細字串與當時狀態。
