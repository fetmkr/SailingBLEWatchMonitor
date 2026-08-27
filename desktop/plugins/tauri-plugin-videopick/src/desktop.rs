use serde::de::DeserializeOwned;
use tauri::{plugin::PluginApi, AppHandle, Runtime};

use crate::models::*;

pub fn init<R: Runtime, C: DeserializeOwned>(
  app: &AppHandle<R>,
  _api: PluginApi<R, C>,
) -> crate::Result<Videopick<R>> {
  Ok(Videopick(app.clone()))
}

pub struct Videopick<R: Runtime>(AppHandle<R>);

impl<R: Runtime> Videopick<R> {
  /// 맥·윈도에는 사진 보관함이 없다. 화면이 직접 고르는 길을 쓴다.
  pub fn pick(&self) -> crate::Result<Picked> {
    Ok(Picked::default())
  }
}
