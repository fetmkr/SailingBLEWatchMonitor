import AVFoundation
import PhotosUI
import SwiftRs
import Tauri
import UIKit
import UniformTypeIdentifiers
import WebKit

/// 영상 하나를 고르게 하고 그 파일 자리를 돌려준다.
///
/// Tauri 의 dialog 부품도 같은 일을 하는데 두 가지가 다르다.
///
///  1. 그쪽은 preferredAssetRepresentationMode 를 안 건다. 기본값이
///     "필요하면 변환" 이라, 아이폰이 찍은 HEVC 영상을 호환 형식으로 다시
///     인코딩한다. 11초짜리에 3초가 걸렸다. 여기서는 .current 로 걸어
///     원본을 그대로 받는다.
///     [확인: tauri-plugin-dialog-2.7.2/ios/Sources/FilePickerController.swift 197줄]
///
///  2. 그쪽은 받은 파일을 또 한 번 복사한다. 여기서는 옮기기(move)로 끝낸다.
///     같은 디스크 안이면 옮기기는 자리표만 바꾼다.
///
/// 찍은 시각도 여기서 읽어 같이 보낸다. 화면 쪽에서 읽으려면 파일 앞뒤 2MB 를
/// IPC 로 끌어와야 하는데 그게 아주 느리다 (4MB 에 10초쯤).
class VideoPickPlugin: Plugin {
  /// 고르는 동안 델리게이트를 살려 둔다. 놓으면 화면이 그대로 사라진다.
  private var runner: AnyObject?

  @objc public func pick(_ invoke: Invoke) throws {
    // ★ PHPicker 는 iOS 14 부터다.
    //   부품을 따로 빌드할 때 swift 가 iOS 13 을 target 으로 넘기므로
    //   여기서 갈라 줘야 한다. Package.swift 를 고쳐도 그 값이 이긴다.
    if #available(iOS 14, *) {
      let r = PickRunner(plugin: self, invoke: invoke)
      runner = r
      r.start()
    } else {
      invoke.reject("영상 고르기는 iOS 14 부터 됩니다")
    }
  }

  func done() { runner = nil }
  func say(_ percent: Int, _ phase: String) {
    trigger("progress", data: ["percent": percent, "phase": phase] as JSObject)
  }
}

@available(iOS 14, *)
final class PickRunner: NSObject, PHPickerViewControllerDelegate {
  private unowned let plugin: VideoPickPlugin
  private let invoke: Invoke
  private var watcher: NSKeyValueObservation?

  init(plugin: VideoPickPlugin, invoke: Invoke) {
    self.plugin = plugin
    self.invoke = invoke
  }

  func start() {
    DispatchQueue.main.async {
      var cfg = PHPickerConfiguration(photoLibrary: PHPhotoLibrary.shared())
      cfg.filter = .videos
      cfg.selectionLimit = 1
      cfg.preferredAssetRepresentationMode = .current   // ★ 원본 그대로

      let picker = PHPickerViewController(configuration: cfg)
      picker.delegate = self
      picker.modalPresentationStyle = .fullScreen
      self.plugin.manager.viewController?.present(picker, animated: true, completion: nil)
    }
  }

  func picker(_ picker: PHPickerViewController, didFinishPicking results: [PHPickerResult]) {
    picker.dismiss(animated: true, completion: nil)

    guard let provider = results.first?.itemProvider,
          provider.hasItemConformingToTypeIdentifier(UTType.movie.identifier)
    else {
      invoke.resolve(["path": ""])          // 취소했거나 영상이 아니다
      plugin.done()
      return
    }

    // 아이클라우드에만 있는 영상은 먼저 내려받아야 한다. 몇십 초가 걸릴 수
    // 있는데 아무 말도 없으면 앱이 멎은 줄 안다.
    plugin.say(0, "start")

    let progress = provider.loadFileRepresentation(
      forTypeIdentifier: UTType.movie.identifier
    ) { url, error in
      self.watcher = nil
      defer { self.plugin.done() }

      if let error = error {
        self.invoke.reject(error.localizedDescription)
        return
      }
      guard let url = url else {
        self.invoke.reject("영상을 못 가져왔습니다")
        return
      }

      // 넘어온 파일은 이 자리를 벗어나면 사라진다. 우리 자리로 옮긴다.
      let fm = FileManager.default
      let dst = fm.temporaryDirectory
        .appendingPathComponent(UUID().uuidString + "-" + url.lastPathComponent)
      do {
        try? fm.removeItem(at: dst)
        try fm.moveItem(at: url, to: dst)

        let size = (try? fm.attributesOfItem(atPath: dst.path)[.size] as? Int) ?? 0
        var shotAt: Double = 0
        if let item = AVURLAsset(url: dst).creationDate, let d = item.dateValue {
          shotAt = d.timeIntervalSince1970 * 1000     // 밀리초
        }

        self.invoke.resolve([
          "path": dst.path,
          "name": url.lastPathComponent,
          "size": size ?? 0,
          "shotAt": shotAt,
        ])
      } catch {
        self.invoke.reject("옮기다 실패: \(error.localizedDescription)")
      }
    }

    watcher = progress.observe(\.fractionCompleted) { p, _ in
      self.plugin.say(Int(p.fractionCompleted * 100), "loading")
    }
  }
}

@_cdecl("init_plugin_videopick")
func initPlugin() -> Plugin {
  return VideoPickPlugin()
}
