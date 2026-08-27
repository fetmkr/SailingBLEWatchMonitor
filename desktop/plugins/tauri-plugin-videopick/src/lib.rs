use tauri::{
  plugin::{Builder, TauriPlugin},
  Manager, Runtime,
};

pub use models::*;

#[cfg(desktop)]
mod desktop;
#[cfg(mobile)]
mod mobile;

mod commands;
mod error;
mod models;

pub use error::{Error, Result};

#[cfg(desktop)]
use desktop::Videopick;
#[cfg(mobile)]
use mobile::Videopick;

/// Extensions to [`tauri::App`], [`tauri::AppHandle`] and [`tauri::Window`] to access the videopick APIs.
pub trait VideopickExt<R: Runtime> {
  fn videopick(&self) -> &Videopick<R>;
}

impl<R: Runtime, T: Manager<R>> crate::VideopickExt<R> for T {
  fn videopick(&self) -> &Videopick<R> {
    self.state::<Videopick<R>>().inner()
  }
}

/// Initializes the plugin.
pub fn init<R: Runtime>() -> TauriPlugin<R> {
  Builder::new("videopick")
    .invoke_handler(tauri::generate_handler![commands::pick])
    .setup(|app, api| {
      #[cfg(mobile)]
      let videopick = mobile::init(app, api)?;
      #[cfg(desktop)]
      let videopick = desktop::init(app, api)?;
      app.manage(videopick);
      Ok(())
    })
    .build()
}
