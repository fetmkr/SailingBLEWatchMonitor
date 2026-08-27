use serde::de::DeserializeOwned;
use tauri::{
  plugin::{PluginApi, PluginHandle},
  AppHandle, Runtime,
};

use crate::models::*;

#[cfg(target_os = "ios")]
tauri::ios_plugin_binding!(init_plugin_videopick);

pub fn init<R: Runtime, C: DeserializeOwned>(
  _app: &AppHandle<R>,
  api: PluginApi<R, C>,
) -> crate::Result<Videopick<R>> {
  #[cfg(target_os = "ios")]
  let handle = api.register_ios_plugin(init_plugin_videopick)?;
  Ok(Videopick(handle))
}

pub struct Videopick<R: Runtime>(PluginHandle<R>);

impl<R: Runtime> Videopick<R> {
  pub fn pick(&self) -> crate::Result<Picked> {
    self
      .0
      .run_mobile_plugin("pick", PickRequest::default())
      .map_err(Into::into)
  }
}
