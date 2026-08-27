use tauri::{command, AppHandle, Runtime};

use crate::models::*;
use crate::Result;
use crate::VideopickExt;

#[command]
pub(crate) async fn pick<R: Runtime>(app: AppHandle<R>) -> Result<Picked> {
  app.videopick().pick()
}
