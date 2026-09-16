// 보드와 말을 맞추는 값들.
//
// ★ 이 파일은 사본이 넷째다. 고칠 때 넷을 같이 고친다.
//     firmware-rak/include/protocol.h
//     firmware/include/protocol.h
//     app/Shared/Protocol.swift
//     desktop/src/protocol.ts        ← 여기
//
// 규격 원문: ../../PROTOCOL.md

export const SERVICE_UUID   = "b0a70001-0000-4000-8000-000000000001";
export const TELEMETRY_UUID = "b0a70002-0000-4000-8000-000000000001";
/** 설정 통로. 글자 한 줄을 쓰면 한 줄로 답한다 (PROTOCOL.md §9). */
export const CONTROL_UUID   = "b0a70003-0000-4000-8000-000000000001";
/** 수신 보드가 LoRa로 들은 배를 앱에 전달하는 30바이트 알림 (§10.13). */
export const FLEET_UUID     = "b0a70004-0000-4000-8000-000000000001";

/** 광고 이름 앞에 붙는 것. 이걸로 우리 보드를 골라낸다. */
export const NAME_PREFIX = "SAIL-";
