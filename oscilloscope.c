/*
 * oscilloscope.c
 * Win32 오실로스코프 커스텀 컨트롤 (v2 - 재작성, 자동 맞춤 렌더링)
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "oscilloscope.h"

#define BIT_MS        (1000.0 / 1200.0)   /* 1200bps -> 약 0.8333ms / bit */
#define MAX_POINTS    4096
#define MIN_TOTAL_MS  10.0                /* 전체 구간 최소 길이(0 나눗셈 방지) */

typedef struct {
    double   t;      /* 이 지점부터 시작하는 시각(ms) */
    int      level;  /* 0=Low, 1=High */
} OscPoint;

typedef struct {
    OscPoint points[MAX_POINTS];
    int      count;
    double   curT;     /* 현재까지 누적된 시간(ms) = 마지막 점 이후 진행된 총 길이 */
    int      curLevel;  /* 현재(마지막) 레벨 */
    COLORREF color;
} OscState;

static void osc_push(OscState *s, int level, double atT) {
    if (s->count >= MAX_POINTS) return;
    s->points[s->count].t = atT;
    s->points[s->count].level = level;
    s->count++;
}

static void OscResetState(OscState *s) {
    s->count = 0;
    s->curT = 0.0;
    s->curLevel = 0;
    osc_push(s, 0, 0.0);      /* t=0 에서 Low 시작 */
    osc_push(s, 0, MIN_TOTAL_MS); /* 최소 구간 확보 */
}

static void OscAppendLevelState(OscState *s, int level, double durationMs) {
    if (durationMs < 0) durationMs = 0;
    /* 직전 레벨을 현재 시각까지 연장(계단 유지) 후 새 레벨로 전환 */
    osc_push(s, s->curLevel, s->curT);
    s->curT += durationMs;
    s->curLevel = level;
    osc_push(s, level, s->curT);
}

static void OscAppendBitsState(OscState *s, const uint8_t *data, int len) {
    int i, b;
    for (i = 0; i < len; i++) {
        uint8_t byte = data[i];
        /* start bit: Low, 1비트 길이 */
        OscAppendLevelState(s, 0, BIT_MS);
        /* data bits: LSB first */
        for (b = 0; b < 8; b++) {
            int bit = (byte >> b) & 0x01;
            OscAppendLevelState(s, bit, BIT_MS);
        }
        /* stop bit: High, 1비트 길이 */
        OscAppendLevelState(s, 1, BIT_MS);
    }
}

/* ════════════════════════════════════════════════════
   렌더링 (항상 전체 파형을 컨트롤 폭에 자동 맞춤)
   ════════════════════════════════════════════════════ */
static void OscPaint(HWND hwnd, OscState *s, HDC hdc, RECT *rc) {
    int W = rc->right - rc->left;
    int H = rc->bottom - rc->top;
    if (W <= 2 || H <= 2) return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    /* 배경 */
    HBRUSH bg = CreateSolidBrush(RGB(6, 13, 20));
    RECT full = {0, 0, W, H};
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    /* 그리드 (가로 5분할) */
    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(30, 45, 55));
    HPEN oldPen = (HPEN)SelectObject(mem, gridPen);
    int i;
    for (i = 1; i < 5; i++) {
        int x = W * i / 5;
        MoveToEx(mem, x, 0, NULL);
        LineTo(mem, x, H);
    }
    SelectObject(mem, oldPen);
    DeleteObject(gridPen);

    /* 전체 길이 계산 */
    double totalMs = (s->count > 0) ? s->points[s->count - 1].t : MIN_TOTAL_MS;
    if (totalMs < MIN_TOTAL_MS) totalMs = MIN_TOTAL_MS;

    int marginTop = 14, marginBottom = 14;
    int hiY = marginTop;
    int loY = H - marginBottom;
    if (loY <= hiY) { loY = H - 2; hiY = 2; }

    /* Low/High 기준선 */
    HPEN refPen = CreatePen(PS_DOT, 1, RGB(50, 65, 75));
    SelectObject(mem, refPen);
    MoveToEx(mem, 0, hiY, NULL); LineTo(mem, W, hiY);
    MoveToEx(mem, 0, loY, NULL); LineTo(mem, W, loY);
    SelectObject(mem, oldPen);
    DeleteObject(refPen);

    /* 파형(계단형) */
    HPEN sigPen = CreatePen(PS_SOLID, 2, s->color);
    SelectObject(mem, sigPen);
    for (i = 0; i < s->count; i++) {
        double t = s->points[i].t;
        int lvl = s->points[i].level;
        int x = (int)(t / totalMs * W);
        int y = lvl ? hiY : loY;
        if (i == 0) {
            MoveToEx(mem, x, y, NULL);
        } else {
            /* 이전 지점에서 수평 이동 후, 값이 바뀌면 수직 이동 */
            LineTo(mem, x, y);
        }
    }
    /* 마지막 지점에서 우측 끝까지 수평 연장 */
    if (s->count > 0) {
        int y = s->points[s->count - 1].level ? hiY : loY;
        LineTo(mem, W, y);
    }
    SelectObject(mem, oldPen);
    DeleteObject(sigPen);

    /* Low/High 레이블 */
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(120, 140, 150));
    HFONT font = CreateFont(11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(mem, font);
    TextOut(mem, 2, hiY - 12, L"High", 4);
    TextOut(mem, 2, loY - 12, L"Low", 3);
    SelectObject(mem, oldFont);
    DeleteObject(font);

    BitBlt(hdc, rc->left, rc->top, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK OscWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OscState *s = (OscState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE:
        return DefWindowProc(hwnd, msg, wp, lp);

    case WM_CREATE: {
        s = (OscState *)calloc(1, sizeof(OscState));
        s->color = RGB(255, 215, 0);
        OscResetState(s);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        return 0;
    }

    case WM_DESTROY:
        if (s) free(s);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        if (s) OscPaint(hwnd, s, hdc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ════════════════════════════════════════════════════
   공개 API
   ════════════════════════════════════════════════════ */
void OscRegister(HINSTANCE hi) {
    WNDCLASSEX wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OscWndProc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = OSC_CLASS_NAME;
    RegisterClassEx(&wc);
}

HWND OscCreate(HWND parent, int x, int y, int w, int h, int id) {
    return CreateWindowEx(WS_EX_CLIENTEDGE, OSC_CLASS_NAME, NULL,
        WS_CHILD | WS_VISIBLE,
        x, y, w, h, parent,
        (HMENU)(UINT_PTR)id,
        (HINSTANCE)GetWindowLongPtr(parent, GWLP_HINSTANCE),
        NULL);
}

void OscSetColor(HWND hOsc, COLORREF color) {
    OscState *s = (OscState *)GetWindowLongPtr(hOsc, GWLP_USERDATA);
    if (s) { s->color = color; InvalidateRect(hOsc, NULL, FALSE); }
}

void OscReset(HWND hOsc) {
    OscState *s = (OscState *)GetWindowLongPtr(hOsc, GWLP_USERDATA);
    if (s) { OscResetState(s); InvalidateRect(hOsc, NULL, FALSE); }
}

void OscAddLevel(HWND hOsc, int level, double durationMs) {
    OscState *s = (OscState *)GetWindowLongPtr(hOsc, GWLP_USERDATA);
    if (s) { OscAppendLevelState(s, level, durationMs); InvalidateRect(hOsc, NULL, FALSE); }
}

void OscAddBits(HWND hOsc, const uint8_t *data, int len) {
    OscState *s = (OscState *)GetWindowLongPtr(hOsc, GWLP_USERDATA);
    if (s) { OscAppendBitsState(s, data, len); InvalidateRect(hOsc, NULL, FALSE); }
}
