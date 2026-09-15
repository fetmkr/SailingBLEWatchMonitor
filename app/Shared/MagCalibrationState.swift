import Foundation

/// 진행 알림은 매초 오므로, 저장 결과와 따로 보관한다.
struct MagCalibrationState {
    private(set) var progress: (done: Int, total: Int)?
    private(set) var message = ""

    var isFull: Bool {
        guard let p = progress else { return false }
        return p.done >= p.total
    }

    mutating func receive(_ text: String) {
        guard text.hasPrefix("magcal ") else { return }
        var body = text.dropFirst(7)
        if body.hasPrefix("on ") { body = body.dropFirst(3) }
        if let token = body.split(separator: " ").first {
            let parts = token.split(separator: "/")
            if parts.count == 2, let done = Int(parts[0]), let total = Int(parts[1]),
               total > 0, done >= 0, done <= total {
                progress = (done, total)
                return // 거절·저장 결과는 다음 진행 알림으로 지우지 않는다.
            }
        }

        message = text
        if text.hasPrefix("magcal 시작") || text.hasPrefix("magcal 처음부터") ||
           text.hasPrefix("magcal 저장") || text.hasPrefix("magcal 적용(") ||
           text.hasPrefix("magcal 지웠") || text.hasPrefix("magcal off") ||
           text.hasPrefix("magcal 모으는 중이 아닙니다") {
            progress = nil
        }
    }

    mutating func showConnectionError(_ text: String) {
        message = text
    }
}
