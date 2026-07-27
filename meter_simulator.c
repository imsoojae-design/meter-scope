/*
 * meter_simulator.c
 * 수도미터 USB 시뮬레이터 (계량기 / Slave 역할)
 * 서울특별시 디지털계량기 프로토콜 V1.2 + NB-IoT 서버 데이터포맷 V1.5 기물정보 규격
 *
 * 역할: 검침단말기(Master)의 REQ_UD2(검침요청)를 수신하여
 *       REP_UD(검침값 회신, Long Frame)로 응답하는 "계량기"를 시뮬레이션
 *
 * ★ 필수 요구사항 ★
 *   - 프로그램 실행 직후 / 포트 연결 직후 TX, RX 오실로스코프 모두
 *     "Low level" 한 줄로 즉시 표시되어야 한다.
 *   - 실제 TX 라인도 연결 즉시 SetCommBreak() 로 강제 Low(전기적 절연) 상태로
 *     만든다. 통신(REQ 수신 -> REP 송신) 중에만 잠깐 High 로 전환된다.
 *
 * 빌드:
 *   x86_64-w64-mingw32-gcc -o MeterSimulator.exe meter_simulator.c oscilloscope.c \
 *     -lcomctl32 -luser32 -lgdi32 -mwindows -O2 -municode -Wall
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "oscilloscope.h"

#pragma comment(lib, "comctl32.lib")

/* ════════════════════════════════════════════════════
   상수 / 컨트롤 ID
   ════════════════════════════════════════════════════ */
#define ID_CB_PORT      101
#define ID_BTN_REFRESH  102
#define ID_BTN_CONNECT  103
#define ID_LBL_STATUS   104

#define ID_ED_METERNO   201   /* 기물번호 8자리 */
#define ID_CB_DIAM      202   /* 구경 */
#define ID_ED_READING   203   /* 검침값 */
#define ID_CB_DECIMALS  204   /* 소수점 자리 */
#define ID_ED_ADDR      205   /* 슬레이브 주소 */
#define ID_CK_OVERQ     206   /* 과부하(Q3초과) */
#define ID_CK_BACKFLOW  207   /* 역류 */
#define ID_CK_LEAK      208   /* 누수 */
#define ID_CK_LOWBATT   209   /* 저전압 */

#define ID_OSC_TX       301   /* 계량기 -> 단말기 (REP 송신) */
#define ID_OSC_RX       302   /* 단말기 -> 계량기 (REQ 수신) */

#define ID_ED_LOG       401
#define ID_BTN_CLEARLOG 402

#define WM_APP_RX_DRAW  (WM_APP + 1)  /* wParam=len, lParam=uint8_t* (malloc) */
#define WM_APP_TX_DRAW  (WM_APP + 2)  /* wParam=len, lParam=uint8_t* (malloc) */
#define WM_APP_LOG      (WM_APP + 3)  /* lParam=wchar_t* (malloc)             */
#define WM_APP_CONN_UI  (WM_APP + 4)  /* wParam=1 connected / 0 disconnected  */

#define BIT_MS  (1000.0 / 1200.0)     /* 1200bps -> 1비트 약 0.8333ms */

/* ════════════════════════════════════════════════════
   전역 상태
   ════════════════════════════════════════════════════ */
static HINSTANCE g_hInst;
static HWND g_hWnd;
static HWND g_hPort, g_hConn, g_hStatus;
static HWND g_hMeterNo, g_hDiam, g_hReading, g_hDecimals, g_hAddr;
static HWND g_hOverQ, g_hBackflow, g_hLeak, g_hLowBatt;
static HWND g_hOscTx, g_hOscRx, g_hLog;

static HANDLE g_hComm = INVALID_HANDLE_VALUE;
static HANDLE g_hThread = NULL;
static volatile BOOL g_running = FALSE;

/* ════════════════════════════════════════════════════
   유틸: 컨트롤 생성 매크로
   ════════════════════════════════════════════════════ */
static HWND mk_label(HWND parent, const wchar_t *t, int x, int y, int w, int h) {
    return CreateWindowEx(0, L"STATIC", t, WS_CHILD | WS_VISIBLE,
        x, y, w, h, parent, NULL, g_hInst, NULL);
}
static HWND mk_edit(HWND parent, const wchar_t *t, int x, int y, int w, int hh, int id, DWORD extra) {
    return CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", t,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
        x, y, w, hh, parent, (HMENU)(UINT_PTR)id, g_hInst, NULL);
}
static HWND mk_btn(HWND parent, const wchar_t *t, int x, int y, int w, int h, int id) {
    return CreateWindowEx(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, g_hInst, NULL);
}
static HWND mk_check(HWND parent, const wchar_t *t, int x, int y, int w, int h, int id) {
    return CreateWindowEx(0, L"BUTTON", t,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, g_hInst, NULL);
}
static HWND mk_combo(HWND parent, int x, int y, int w, int h, int id) {
    return CreateWindowEx(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, g_hInst, NULL);
}

static void log_line(const wchar_t *fmt, ...) {
    wchar_t *buf = (wchar_t *)malloc(sizeof(wchar_t) * 512);
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 512, fmt, ap);
    va_end(ap);
    PostMessage(g_hWnd, WM_APP_LOG, 0, (LPARAM)buf);
}

/* ════════════════════════════════════════════════════
   BCD / 체크섬 유틸
   ════════════════════════════════════════════════════ */

/* 8자리 십진 문자열(digits, 널종료 8글자) -> BCD 4바이트(big-endian 자연순) */
static void digits_to_bcd4(const char *digits, uint8_t out[4]) {
    int i;
    for (i = 0; i < 4; i++) {
        int hi = digits[i * 2] - '0';
        int lo = digits[i * 2 + 1] - '0';
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

/* 4바이트 BCD(big-endian 자연순) -> 전송용 little-endian(역순) 복사 */
static void bcd4_to_wire(const uint8_t in[4], uint8_t out[4]) {
    out[0] = in[3]; out[1] = in[2]; out[2] = in[1]; out[3] = in[0];
}

static int diam_to_code(int mm) {
    switch (mm) {
        case 15: return 1; case 20: return 2; case 25: return 3; case 32: return 4;
        case 40: return 5; case 50: return 6; case 80: return 7; case 100: return 8;
        case 150: return 9; case 200: return 0xA; case 250: return 0xB; case 300: return 0xC;
        default: return 1;
    }
}

/* ════════════════════════════════════════════════════
   REP (Long Frame) 생성
   ════════════════════════════════════════════════════ */
static int build_rep_frame(uint8_t *frame, int maxlen,
                           uint8_t addr, const char meterNoDigits[9],
                           int diamMm, int decimals, double reading,
                           uint8_t statusByte) {
    uint8_t ident_be[4], ident_wire[4];
    uint8_t data_be[4], data_wire[4];
    char digitbuf[9];
    long long scaled;
    uint8_t userdata[16];
    int ulen = 0;
    int i;
    uint8_t cs;

    digits_to_bcd4(meterNoDigits, ident_be);
    bcd4_to_wire(ident_be, ident_wire);

    scaled = (long long)llround(reading * pow(10.0, decimals));
    if (scaled < 0) scaled = 0;
    if (scaled > 99999999LL) scaled = 99999999LL;
    snprintf(digitbuf, sizeof(digitbuf), "%08lld", scaled);
    digits_to_bcd4(digitbuf, data_be);
    bcd4_to_wire(data_be, data_wire);

    userdata[ulen++] = 0x0F;                                  /* MDH */
    userdata[ulen++] = ident_wire[0];
    userdata[ulen++] = ident_wire[1];
    userdata[ulen++] = ident_wire[2];
    userdata[ulen++] = ident_wire[3];
    userdata[ulen++] = statusByte;                            /* Status */
    userdata[ulen++] = (uint8_t)((diam_to_code(diamMm) << 4) | 0x0C); /* DIF */
    userdata[ulen++] = (uint8_t)((0x1 << 4) | (decimals & 0x0F));     /* VIF: 단위=1(m3) */
    userdata[ulen++] = data_wire[0];
    userdata[ulen++] = data_wire[1];
    userdata[ulen++] = data_wire[2];
    userdata[ulen++] = data_wire[3];
    /* UDF 없음(0 byte) */

    if (maxlen < 6 + ulen + 1) return 0;

    i = 0;
    frame[i++] = 0x68;
    frame[i++] = (uint8_t)(3 + ulen);   /* L field */
    frame[i++] = (uint8_t)(3 + ulen);   /* L field (반복) */
    frame[i++] = 0x68;
    frame[i++] = 0x08;                  /* C field: REP_UD */
    frame[i++] = addr;                  /* A field */
    frame[i++] = 0x78;                  /* CI field */
    memcpy(&frame[i], userdata, ulen);
    i += ulen;

    cs = 0;
    cs = (uint8_t)(cs + 0x08 + addr + 0x78);
    for (int k = 0; k < ulen; k++) cs = (uint8_t)(cs + userdata[k]);
    frame[i++] = cs;
    frame[i++] = 0x16;

    return i;
}

/* ════════════════════════════════════════════════════
   시리얼 포트 열기 / 설정
   ════════════════════════════════════════════════════ */
static BOOL open_port(const wchar_t *portName) {
    wchar_t path[32];
    swprintf(path, 32, L"\\\\.\\%s", portName);

    g_hComm = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    if (g_hComm == INVALID_HANDLE_VALUE) return FALSE;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(g_hComm, &dcb);
    dcb.BaudRate = 1200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    SetCommState(g_hComm, &dcb);

    COMMTIMEOUTS to = {0};
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 50;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 200;
    to.WriteTotalTimeoutMultiplier = 5;
    SetCommTimeouts(g_hComm, &to);

    /*
     * ★ 연결 직후 TX 강제 Low (전기적 절연) ★
     * 프로토콜 V1.2: "전자식 수도계량기는 검침단말기와 통신을 하지 않는
     * 상태에서는 검침단말기와 전기적으로 절연상태이어야 한다."
     */
    SetCommBreak(g_hComm);
    return TRUE;
}

static void close_port(void) {
    if (g_hComm != INVALID_HANDLE_VALUE) {
        ClearCommBreak(g_hComm);   /* Break 해제 후 닫기 */
        CloseHandle(g_hComm);
        g_hComm = INVALID_HANDLE_VALUE;
    }
}

/* ════════════════════════════════════════════════════
   워커 스레드: REQ 수신 대기 -> REP 송신
   ════════════════════════════════════════════════════ */
static DWORD WINAPI serial_thread(LPVOID arg) {
    uint8_t buf[8];
    int blen = 0;
    DWORD nread;

    while (g_running) {
        uint8_t b;
        if (!ReadFile(g_hComm, &b, 1, &nread, NULL)) break;
        if (nread == 0) continue; /* 타임아웃, 계속 대기 */

        if (blen == 0) {
            if (b != 0x10) continue;  /* Short Frame Start(10h) 대기 */
        }
        buf[blen++] = b;

        if (blen == 5) {
            /* 10 | C | A | CS | 16 */
            uint8_t c = buf[1], a = buf[2], cs = buf[3], stop = buf[4];
            uint8_t calc_cs = (uint8_t)(c + a);
            int addr_wanted = GetDlgItemInt(g_hWnd, ID_ED_ADDR, NULL, FALSE);

            if (buf[0] == 0x10 && stop == 0x16 && cs == calc_cs && c == 0x5B &&
                (a == (uint8_t)addr_wanted)) {

                /* RX(수신) 오실로스코프 표시용 사본 */
                uint8_t *rxCopy = (uint8_t *)malloc(blen);
                memcpy(rxCopy, buf, blen);
                PostMessage(g_hWnd, WM_APP_RX_DRAW, (WPARAM)blen, (LPARAM)rxCopy);

                log_line(L"RX 검침요청 수신 (Addr=%d) - REP 준비", a);

                /* ── 계량기 정보 읽기 (GUI 스레드 값이지만 읽기 전용 조회는 안전) ── */
                wchar_t wbuf[64];
                char meterNoDigits[9] = "00000000";
                GetDlgItemText(g_hWnd, ID_ED_METERNO, wbuf, 64);
                {
                    char tmp[64]; WideCharToMultiByte(CP_ACP,0,wbuf,-1,tmp,64,NULL,NULL);
                    /* 숫자만 추출, 8자리로 맞춤 */
                    int di = 0; char digits[16] = {0};
                    for (int k=0; tmp[k] && di<8; k++) if (tmp[k]>='0'&&tmp[k]<='9') digits[di++]=tmp[k];
                    while (di<8) digits[di++]='0';
                    memcpy(meterNoDigits, digits, 8);
                    meterNoDigits[8]=0;
                }

                int diamIdx = (int)SendDlgItemMessage(g_hWnd, ID_CB_DIAM, CB_GETCURSEL, 0, 0);
                static const int diamTable[] = {15,20,25,32,40,50,80,100,150,200,250,300};
                int diamMm = (diamIdx>=0 && diamIdx<12) ? diamTable[diamIdx] : 15;

                int decimals = (int)SendDlgItemMessage(g_hWnd, ID_CB_DECIMALS, CB_GETCURSEL, 0, 0);
                if (decimals < 0) decimals = 3;

                GetDlgItemText(g_hWnd, ID_ED_READING, wbuf, 64);
                double reading = _wtof(wbuf);

                uint8_t status = 0;
                if (IsDlgButtonChecked(g_hWnd, ID_CK_OVERQ))    status |= 0x80;
                if (IsDlgButtonChecked(g_hWnd, ID_CK_BACKFLOW)) status |= 0x40;
                if (IsDlgButtonChecked(g_hWnd, ID_CK_LEAK))     status |= 0x20;
                if (IsDlgButtonChecked(g_hWnd, ID_CK_LOWBATT))  status |= 0x04;

                uint8_t rep[40];
                int replen = build_rep_frame(rep, sizeof(rep), a, meterNoDigits,
                                              diamMm, decimals, reading, status);

                if (replen > 0) {
                    /*
                     * ★ 프로토콜 V1.2 계량기 TX 시퀀스 ★
                     * "계량기는 검침단말기의 검침 요청 메시지를 받은 후
                     *  Low level로 0~100ms 대기 후 High level로 전환한다.
                     *  (20~50ms 대기 후 start bit 시작)"
                     */
                    Sleep(50);                 /* 0~100ms: Low 유지(이미 SetCommBreak 상태) */
                    ClearCommBreak(g_hComm);   /* TX -> High 전환 */
                    Sleep(35);                 /* 20~50ms: High 유지 후 start bit */

                    DWORD written;
                    WriteFile(g_hComm, rep, replen, &written, NULL);

                    /* 전송 실제 소요시간만큼 대기 (10 bit/byte: start+8data+stop) */
                    double txMs = replen * 10.0 * BIT_MS;
                    Sleep((DWORD)txMs + 10);

                    Sleep(50);                 /* 전송 완료 후 0~100ms 대기 */
                    SetCommBreak(g_hComm);      /* TX -> Low 복귀(절연) */

                    /* TX(송신) 오실로스코프 표시용 사본 */
                    uint8_t *txCopy = (uint8_t *)malloc(replen);
                    memcpy(txCopy, rep, replen);
                    PostMessage(g_hWnd, WM_APP_TX_DRAW, (WPARAM)replen, (LPARAM)txCopy);

                    log_line(L"TX 검침값 회신 완료 (%d bytes) 검침값=%.3f", replen, reading);
                } else {
                    log_line(L"REP 프레임 생성 실패");
                }
            } else {
                log_line(L"RX 프레임 오류 또는 주소불일치 (수신무시)");
            }
            blen = 0;
        }
        if (blen >= 5) blen = 0; /* 안전장치 */
    }
    return 0;
}

/* ════════════════════════════════════════════════════
   연결 / 해제
   ════════════════════════════════════════════════════ */
static void do_connect(void) {
    wchar_t port[32];
    int sel = (int)SendMessage(g_hPort, CB_GETCURSEL, 0, 0);
    if (sel < 0) { MessageBox(g_hWnd, L"COM 포트를 선택하세요.", L"알림", MB_OK); return; }
    SendMessage(g_hPort, CB_GETLBTEXT, sel, (LPARAM)port);

    if (!open_port(port)) {
        MessageBox(g_hWnd, L"포트를 열 수 없습니다.", L"오류", MB_ICONERROR);
        return;
    }

    /* ★ 연결 즉시 TX/RX 오실로스코프를 Low 한 줄로 초기화 ★ */
    OscReset(g_hOscTx);
    OscReset(g_hOscRx);

    g_running = TRUE;
    g_hThread = CreateThread(NULL, 0, serial_thread, NULL, 0, NULL);

    SetWindowText(g_hConn, L"연결 해제");
    SetWindowText(g_hStatus, L"● 연결됨 (계량기 대기 중)");
    log_line(L"포트 연결됨: %s (1200bps, TX=Low 절연 상태)", port);
}

static void do_disconnect(void) {
    g_running = FALSE;
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 1000);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    close_port();

    OscReset(g_hOscTx);
    OscReset(g_hOscRx);

    SetWindowText(g_hConn, L"연결");
    SetWindowText(g_hStatus, L"● 연결 안됨");
    log_line(L"포트 연결 해제됨");
}

static void refresh_ports(void) {
    SendMessage(g_hPort, CB_RESETCONTENT, 0, 0);
    for (int i = 1; i <= 40; i++) {
        wchar_t name[16], path[32];
        swprintf(name, 16, L"COM%d", i);
        swprintf(path, 32, L"\\\\.\\COM%d", i);
        HANDLE h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            SendMessage(g_hPort, CB_ADDSTRING, 0, (LPARAM)name);
        } else if (GetLastError() == ERROR_ACCESS_DENIED) {
            SendMessage(g_hPort, CB_ADDSTRING, 0, (LPARAM)name); /* 사용 중이지만 존재함 */
        }
    }
    if (SendMessage(g_hPort, CB_GETCOUNT, 0, 0) > 0)
        SendMessage(g_hPort, CB_SETCURSEL, 0, 0);
}

/* ════════════════════════════════════════════════════
   윈도우 프로시저
   ════════════════════════════════════════════════════ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        /* ── 상단: 연결 ── */
        mk_label(hwnd, L"💧 수도미터 USB 시뮬레이터 (계량기)", 10, 10, 320, 20);
        g_hStatus = mk_label(hwnd, L"● 연결 안됨", 10, 34, 220, 18);

        mk_label(hwnd, L"포트", 10, 60, 30, 16);
        g_hPort = mk_combo(hwnd, 44, 58, 100, 200, ID_CB_PORT);
        mk_btn(hwnd, L"새로고침", 150, 57, 70, 22, ID_BTN_REFRESH);
        g_hConn = mk_btn(hwnd, L"연결", 226, 57, 80, 22, ID_BTN_CONNECT);

        /* ── 계량기 정보 ── */
        mk_label(hwnd, L"기물번호(8자리)", 10, 92, 100, 16);
        g_hMeterNo = mk_edit(hwnd, L"09123456", 114, 90, 100, 20, ID_ED_METERNO, 0);

        mk_label(hwnd, L"구경", 224, 92, 30, 16);
        g_hDiam = mk_combo(hwnd, 254, 90, 60, 200, ID_CB_DIAM);
        {
            const wchar_t *d[] = {L"15",L"20",L"25",L"32",L"40",L"50",L"80",L"100",L"150",L"200",L"250",L"300"};
            for (int i = 0; i < 12; i++) SendMessage(g_hDiam, CB_ADDSTRING, 0, (LPARAM)d[i]);
            SendMessage(g_hDiam, CB_SETCURSEL, 0, 0);
        }

        mk_label(hwnd, L"검침값(㎥)", 10, 118, 70, 16);
        g_hReading = mk_edit(hwnd, L"12345.678", 84, 116, 100, 20, ID_ED_READING, 0);

        mk_label(hwnd, L"소수점", 194, 118, 40, 16);
        g_hDecimals = mk_combo(hwnd, 234, 116, 50, 200, ID_CB_DECIMALS);
        for (int i = 0; i <= 4; i++) {
            wchar_t b[4]; swprintf(b, 4, L"%d", i);
            SendMessage(g_hDecimals, CB_ADDSTRING, 0, (LPARAM)b);
        }
        SendMessage(g_hDecimals, CB_SETCURSEL, 3, 0);

        mk_label(hwnd, L"슬레이브 주소", 294, 118, 80, 16);
        g_hAddr = mk_edit(hwnd, L"1", 372, 116, 40, 20, ID_ED_ADDR, ES_NUMBER);

        /* ── 상태 경보 테스트 체크박스 ── */
        mk_label(hwnd, L"상태(테스트용)", 10, 144, 90, 16);
        g_hOverQ    = mk_check(hwnd, L"과부하(Q3초과)", 104, 142, 110, 18, ID_CK_OVERQ);
        g_hBackflow = mk_check(hwnd, L"역류",           220, 142, 50,  18, ID_CK_BACKFLOW);
        g_hLeak     = mk_check(hwnd, L"누수",           276, 142, 50,  18, ID_CK_LEAK);
        g_hLowBatt  = mk_check(hwnd, L"저전압",         332, 142, 60,  18, ID_CK_LOWBATT);

        /* ── 오실로스코프 ── */
        mk_label(hwnd, L"TX (계량기 -> 단말기, REP 회신)", 10, 172, 250, 16);
        g_hOscTx = OscCreate(hwnd, 10, 190, 470, 70, ID_OSC_TX);
        OscSetColor(g_hOscTx, RGB(255, 215, 0));

        mk_label(hwnd, L"RX (단말기 -> 계량기, REQ 요청 수신)", 10, 266, 250, 16);
        g_hOscRx = OscCreate(hwnd, 10, 284, 470, 70, ID_OSC_RX);
        OscSetColor(g_hOscRx, RGB(0, 207, 255));

        /* ★ 초기 상태: 프로그램 실행 직후 즉시 Low 한 줄 표시 ★ */
        OscReset(g_hOscTx);
        OscReset(g_hOscRx);

        /* ── 로그 ── */
        mk_label(hwnd, L"통신 로그", 10, 362, 100, 16);
        g_hLog = mk_edit(hwnd, L"", 10, 380, 470, 160, ID_ED_LOG,
            WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY);
        mk_btn(hwnd, L"로그 지우기", 10, 546, 100, 22, ID_BTN_CLEARLOG);

        refresh_ports();
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_BTN_REFRESH) {
            refresh_ports();
        } else if (id == ID_BTN_CONNECT) {
            if (g_hComm == INVALID_HANDLE_VALUE) do_connect();
            else do_disconnect();
        } else if (id == ID_BTN_CLEARLOG) {
            SetWindowText(g_hLog, L"");
        }
        return 0;
    }

    case WM_APP_RX_DRAW: {
        uint8_t *d = (uint8_t *)lp;
        int len = (int)wp;
        OscReset(g_hOscTx);
        OscReset(g_hOscRx);
        OscAddLevel(g_hOscRx, 1, 35.0);
        OscAddBits(g_hOscRx, d, len);
        OscAddLevel(g_hOscRx, 1, 50.0);
        OscAddLevel(g_hOscRx, 0, 0.0);
        free(d);
        return 0;
    }

    case WM_APP_TX_DRAW: {
        uint8_t *d = (uint8_t *)lp;
        int len = (int)wp;
        OscAddLevel(g_hOscTx, 0, 50.0);
        OscAddLevel(g_hOscTx, 1, 35.0);
        OscAddBits(g_hOscTx, d, len);
        OscAddLevel(g_hOscTx, 1, 50.0);
        OscAddLevel(g_hOscTx, 0, 0.0);
        free(d);
        return 0;
    }

    case WM_APP_LOG: {
        wchar_t *s = (wchar_t *)lp;
        int len = GetWindowTextLength(g_hLog);
        SendMessage(g_hLog, EM_SETSEL, len, len);
        SendMessage(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)s);
        SendMessage(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
        free(s);
        return 0;
    }

    case WM_CLOSE:
        if (g_hComm != INVALID_HANDLE_VALUE) do_disconnect();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ════════════════════════════════════════════════════
   WinMain
   ════════════════════════════════════════════════════ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show) {
    (void)hPrev; (void)cmd;
    g_hInst = hInst;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    OscRegister(hInst);

    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MeterSimulatorWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassEx(&wc);

    g_hWnd = CreateWindowEx(0, L"MeterSimulatorWnd",
        L"수도미터 USB 시뮬레이터 (계량기) - V1.2/V1.5",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 640,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hWnd, show);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
