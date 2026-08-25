// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
#[tauri::command]
fn greet(name: &str) -> String {
    format!("Hello, {}! You've been greeted from Rust!", name)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let mut app = tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_http::init())
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_dialog::init())
        // USB 시리얼. 블루투스가 없는 컴퓨터에서 보드를 깨우는 데 쓴다.
        // 보드는 BLE 와 시리얼이 같은 명령을 쓴다 (PROTOCOL.md §9).
        .plugin(tauri_plugin_serialplugin::init())
        .invoke_handler(tauri::generate_handler![greet]);

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
