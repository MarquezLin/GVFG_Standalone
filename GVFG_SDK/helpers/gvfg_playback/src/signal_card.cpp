#include "signal_card.h"

SignalCard::~SignalCard() {
    detach();
    if (bitmap_) DeleteObject(bitmap_);
}
bool SignalCard::on_window_thread() const {
    return !parent_ || GetWindowThreadProcessId(parent_, nullptr) == GetCurrentThreadId();
}
void SignalCard::detach() {
    if (parent_) RemoveWindowSubclass(parent_, parent_proc, reinterpret_cast<UINT_PTR>(this));
    if (view_) DestroyWindow(view_);
    parent_ = view_ = nullptr;
}
bool SignalCard::attach(HWND parent) {
    if (!on_window_thread() || (parent &&
        GetWindowThreadProcessId(parent, nullptr) != GetCurrentThreadId())) return false;
    if (parent_ == parent) return set_connected(connected_);
    detach();
    parent_ = parent;
    if (parent_ && !SetWindowSubclass(parent_, parent_proc,
            reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) {
        parent_ = nullptr;
        return false;
    }
    return set_connected(connected_);
}
bool SignalCard::set_connected(bool connected) {
    if (!on_window_thread()) return false;
    if (connected_ == connected && (connected || view_)) return true;
    connected_ = connected;
    if (!parent_) return true;
    if (connected) {
        if (view_) ShowWindow(view_, SW_HIDE);
        return true;
    }
    if (!view_) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&parent_proc), &module)) return false;
        if (!bitmap_) bitmap_ = static_cast<HBITMAP>(LoadImageW(module,
            MAKEINTRESOURCEW(101), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        if (!bitmap_) return false;
        view_ = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"GVFG NO SIGNAL",
            WS_CHILD | SS_OWNERDRAW, 0, 0, 0, 0, parent_, nullptr, module, nullptr);
        if (!view_) return false;
    }
    RECT area{}; GetClientRect(parent_, &area);
    SetWindowPos(view_, HWND_TOP, 0, 0, area.right, area.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}
LRESULT CALLBACK SignalCard::parent_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR data) {
    auto *self = reinterpret_cast<SignalCard *>(data);
    if (msg == WM_DRAWITEM) {
        const auto *draw = reinterpret_cast<DRAWITEMSTRUCT *>(lp);
        if (draw && draw->hwndItem == self->view_) {
            BITMAP info{}; GetObjectW(self->bitmap_, sizeof(info), &info);
            HDC source = CreateCompatibleDC(draw->hDC);
            if (!source) return TRUE;
            auto old = SelectObject(source, self->bitmap_);
            SetStretchBltMode(draw->hDC, HALFTONE);
            StretchBlt(draw->hDC, draw->rcItem.left, draw->rcItem.top,
                draw->rcItem.right - draw->rcItem.left, draw->rcItem.bottom - draw->rcItem.top,
                source, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
            SelectObject(source, old); DeleteDC(source);
            return TRUE;
        }
    } else if (msg == WM_SIZE && self->view_) {
        RECT area{}; GetClientRect(hwnd, &area);
        MoveWindow(self->view_, 0, 0, area.right, area.bottom, TRUE);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, parent_proc, id);
        self->parent_ = self->view_ = nullptr;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
