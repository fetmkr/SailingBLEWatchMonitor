// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
#[tauri::command]
fn greet(name: &str) -> String {
    format!("Hello, {}! You've been greeted from Rust!", name)
}

/// 이 기기에서 무엇이 되는가.
///
/// 화면 쪽에서 기기 이름을 보고 짐작하면 언젠가 어긋난다. 어떤 부품이
/// 실제로 들어갔는지는 컴파일할 때 정해지고, 그걸 아는 건 여기뿐이다.
/// 그래서 답을 여기서 만든다. 자세한 이야기는 src/platform.ts 에 적어 뒀다.
#[derive(serde::Serialize)]
struct Caps {
    os: &'static str,
    usb: bool,
    ble: bool,
    wifi: bool,
    #[serde(rename = "localFiles")]
    local_files: bool,
}

#[tauri::command]
fn caps() -> Caps {
    Caps {
        os: std::env::consts::OS,

        // USB 시리얼. 아이패드에서는 부품 자체가 안 들어간다.
        // 아래 plugin 등록과 Cargo.toml 의 조건이 이 값과 같아야 한다.
        usb: cfg!(not(target_os = "ios")),

        ble: true,
        wifi: true,

        // 아무 경로나 직접 읽는 것. 맥·윈도만 된다.
        // 아이패드와 안드로이드는 사용자가 고른 파일만 읽을 수 있다.
        local_files: cfg!(not(any(target_os = "ios", target_os = "android"))),
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    #[allow(unused_mut)]
    let mut app = tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_http::init())
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_dialog::init())
        // 영상 고르기. 아이패드에서만 실제로 일한다.
        .plugin(tauri_plugin_videopick::init())
        .invoke_handler(tauri::generate_handler![greet, caps]);

    // USB 시리얼. 블루투스가 없는 컴퓨터에서 보드를 깨우는 데 쓴다.
    // 보드는 BLE 와 시리얼이 같은 명령을 쓴다 (PROTOCOL.md §9).
    //
    // 아이패드에서는 이 부품을 아예 안 넣는다. 부품이 iOS 에서 serialport 를
    // 빼도록 되어 있어서 (tauri-plugin-serialplugin Cargo.toml 85번 줄)
    // 넣어 봐야 아무것도 못 한다.
    #[cfg(not(target_os = "ios"))]
    {
        app = app.plugin(tauri_plugin_serialplugin::init());
    }

    // BLE. 보드를 찾고 "WiFi 켜" 를 시키는 데 쓴다 (PROTOCOL.md §9).
    //
    // 못 올라와도 앱은 뜬다. 블루투스가 꺼져 있거나 권한이 없을 수 있는데,
    // 그렇다고 파일 보는 것까지 막을 이유가 없다. 주소를 손으로 쳐도 된다.
    match tauri_plugin_blec::try_init() {
        Ok(plugin) => app = app.plugin(plugin),
        Err(e) => eprintln!("[BLE] 못 올렸습니다: {e:?}  — 주소를 손으로 치면 됩니다"),
    }

    app.run(tauri::generate_context!())
        .expect("error while running tauri application");
}
