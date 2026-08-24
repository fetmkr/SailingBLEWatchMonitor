fn main() {
    // 맨 실행파일에도 Info.plist 를 박아 넣는다.
    //
    // 왜 필요한가 — 맥은 블루투스를 쓰기 전에 "왜 쓰는지" 를 요구한다
    // (NSBluetoothAlwaysUsageDescription). 그 글이 없으면 앱이 블루투스를
    // 만지는 순간 그냥 죽는다.
    //
    // 배포판(.app)은 Tauri 가 옆의 Info.plist 를 찾아 합쳐 준다.
    // [확인: tauri-utils/src/config.rs:662]
    //
    // 그런데 `tauri dev` 는 번들이 아니라 맨 실행파일을 그대로 돌린다.
    // 그때는 합칠 자리가 없다. 그래서 링커에게 실행파일 안의 __TEXT 칸에
    // 이 파일을 통째로 넣으라고 시킨다. 맥이 거기서도 읽어 간다.
    #[cfg(target_os = "macos")]
    {
        let plist = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("Info.plist");
        if plist.exists() {
            println!("cargo:rerun-if-changed=Info.plist");
            println!(
                "cargo:rustc-link-arg=-Wl,-sectcreate,__TEXT,__info_plist,{}",
                plist.display()
            );
        }
    }

    tauri_build::build()
}
