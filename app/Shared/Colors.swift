//
//  Colors.swift
//  iOS / watchOS 공용 색
//

import SwiftUI

extension Color {
    /// 경고 색.
    ///
    /// 값이 지어낸 것이거나(시뮬레이터) 센서가 죽었을 때 쓴다.
    /// 시스템 주황(`Color.orange`) 대신 순수 빨강 — RGB(255, 0, 0).
    ///
    /// ※ 연결 상태를 나타내는 색(연결=초록 / 광고=주황)은 경고가 아니라
    ///   신호등이므로 이 색을 쓰지 않는다. 광고로 값을 받는 건 나쁜 상태가 아니다.
    static let sailWarn = Color(red: 1, green: 0, blue: 0)
}
