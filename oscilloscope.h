/*
 * oscilloscope.h
 * Win32 오실로스코프 커스텀 컨트롤 (v2 - 재작성)
 * 서울특별시 디지털계량기 프로토콜 V1.2 UART 파형 표시
 *
 * 설계 원칙:
 *  - 항상 "전체 파형이 컨트롤 폭에 맞춰 자동 스케일" 되어 표시됨
 *    (줌/스크롤 없음 -> 화면에 아무것도 안 보이는 버그 원천 차단)
 *  - 기본 상태(Reset 직후)는 항상 Low 레벨 한 줄로 표시됨
 *    (프로그램 실행 즉시 "TX/RX 모두 Low" 요구사항 반영)
 *
 * 사용법:
 *   OscRegister(hInstance);                 // WinMain에서 1회
 *   HWND h = OscCreate(hParent, x,y,w,h,id);
 *   OscSetColor(h, RGB(255,215,0));
 *   OscReset(h);                            // Low 한 줄로 초기화
 *   OscAddLevel(h, 0, 50.0);                // Low 상태를 50ms 유지
 *   OscAddLevel(h, 1, 35.0);                // High 상태를 35ms 유지
 *   OscAddBits(h, data, len);               // UART 바이트 파형(1200bps, 8N1)
 */
#pragma once
#include <windows.h>
#include <stdint.h>

#define OSC_CLASS_NAME  L"OscilloscopeViewV2"

void OscRegister(HINSTANCE hInst);
HWND OscCreate(HWND parent, int x, int y, int w, int h, int id);
void OscSetColor(HWND hOsc, COLORREF color);

/* 파형을 전체 Low 한 줄로 초기화 (프로그램 시작/연결 직후 반드시 호출) */
void OscReset(HWND hOsc);

/* level: 0=Low, 1=High 상태를 durationMs 동안 유지하는 구간 추가 */
void OscAddLevel(HWND hOsc, int level, double durationMs);

/*
 * UART 바이트 파형 추가 (1200bps, Non parity, 8 data bit, 1 stop bit)
 * - 각 바이트: start bit(Low,1bit) + data 8bit(LSB first) + stop bit(High,1bit)
 * - 바이트 사이는 관례상 idle High 유지하지 않고 바로 이어서 전송(연속 프레임 가정)
 * - 호출 직전 레벨이 무엇이든 상관없이 시작 시 Low 로 강제 전환(스타트비트)
 */
void OscAddBits(HWND hOsc, const uint8_t *data, int len);
