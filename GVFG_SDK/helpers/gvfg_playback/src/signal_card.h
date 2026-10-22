#pragma once
#include <windows.h>
#include <commctrl.h>

// UI-thread-owned overlay. It never changes capture buffers or video timing.
class SignalCard {
public:
    ~SignalCard();
    bool attach(HWND parent);
    bool set_connected(bool connected);
    bool on_window_thread() const;
private:
    static LRESULT CALLBACK parent_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void detach();
    HWND parent_ = nullptr, view_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    bool connected_ = true;
};
